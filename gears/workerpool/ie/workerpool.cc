// Copyright 2006, Google Inc.
//
// Redistribution and use in source and binary forms, with or without 
// modification, are permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice, 
//     this list of conditions and the following disclaimer.
//  2. Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//  3. Neither the name of Google Inc. nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
// EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR 
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF 
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// JavaScript worker threads in Internet Explorer.
//
// Implementation details:
//
// CROSS-THREAD COMMUNICATION: Each thread has a (non-visible) window.  For
// every Gears message sent to that thread, a Win32 message is posted to the
// thread's window.
// This is necessary so we can wake the "root" JS thread on a page. The browser
// owns that thread, but we need a way to tell it to run our JS
// callback (onmessage) -- and only when the JS engine exits to the top level.
// Window messages give just the right behavior, because senders identify
// a window by its handle, but multiple windows can be associated with a
// thread, and messages sent to a window get queued at the thread level.
// BTW, I'm told IE6 XmlHttpRequest also uses window messages.
// We didn't need to use the same scheme for the non-root (created) threads,
// but it makes sense to be consistent.

#include <assert.h> // TODO(cprince): use DCHECK() when have google3 logging
#include <queue>
#include "gears/third_party/scoped_ptr/scoped_ptr.h"

#include "gears/workerpool/ie/workerpool.h"

#include "gears/base/common/atomic_ops.h"
#include "gears/base/common/js_runner.h"
#include "gears/base/common/js_runner_utils.h"
#include "gears/base/common/scoped_win32_handles.h"
#include "gears/base/common/string_utils.h"
#include "gears/base/common/url_utils.h"
#include "gears/base/ie/activex_utils.h"
#include "gears/base/ie/atl_headers.h"
#include "gears/base/ie/factory.h"
#include "gears/localserver/common/http_constants.h"
#include "gears/localserver/common/http_request.h"
#include "gears/workerpool/common/workerpool_utils.h"


//
// Message container.
//

struct Message {
  std::string16 text;
  int sender;
  SecurityOrigin origin;

  Message(const std::string16 &t, int s, const SecurityOrigin &o)
      : text(t), sender(s), origin(o) { }
  Message() : sender(-1) { }
};


//
// JavaScriptWorkerInfo -- contains the state of each JavaScript worker.
//

struct JavaScriptWorkerInfo {
  // Our code assumes some items begin cleared. Zero all members w/o ctors.
  JavaScriptWorkerInfo()
      : threads_manager(NULL), js_runner(NULL), message_hwnd(0),
        thread_init_signalled(false), thread_init_ok(false),
        script_signalled(false), script_ok(false), http_request(NULL),
        is_factory_suspended(false), thread_handle(INVALID_HANDLE_VALUE) {}

  //
  // These fields are used for all workers in pool (parent + children).
  //
  PoolThreadsManager *threads_manager;
  JsRunnerInterface *js_runner;
  scoped_ptr<JsRootedCallback> onmessage_handler;
  scoped_ptr<JsRootedCallback> onerror_handler;

  HWND message_hwnd;
  std::queue<Message> message_queue;

  // 
  // These fields are used only for created workers (children).
  //
  Mutex thread_init_mutex;  // Protects: thread_init_signalled
  bool thread_init_signalled;
  bool thread_init_ok;  // Owner: child before signal, parent after signal

  Mutex script_mutex;  // Protects: script_signalled
  bool script_signalled;  
  bool script_ok;  // Owner: parent before signal, immutable after
  std::string16 script_text;  // Owner: parent before signal, immutable after
  SecurityOrigin script_origin;  // Owner: parent before signal, immutable after

  ScopedHttpRequestPtr http_request;  // For createWorkerFromUrl()
  scoped_ptr<HttpRequest::ReadyStateListener> http_request_listener;
  // TODO(cprince): Find a clean way in IE to store the native interface and
  // keep a scoped AddRef, without requiring two separate pointers.
  GearsFactory *factory_ptr;
  CComPtr<IUnknown> factory_ref;
  bool is_factory_suspended;
  SAFE_HANDLE thread_handle;
};


//
// GearsWorkerPool -- handles the browser glue.
//

GearsWorkerPool::GearsWorkerPool()
    : threads_manager_(NULL),
      owns_threads_manager_(false) {
}

GearsWorkerPool::~GearsWorkerPool() {
  if (owns_threads_manager_) {
    assert(threads_manager_);
    threads_manager_->ShutDown();
  }

  if (threads_manager_) {
    threads_manager_->UninitWorkerThread();
    threads_manager_->ReleaseWorkerRef();
  }
}

void GearsWorkerPool::SetThreadsManager(PoolThreadsManager *manager) {
  assert(!threads_manager_);
  threads_manager_ = manager;
  threads_manager_->AddWorkerRef();

  // Leave owns_threads_manager_ set to false.
  assert(!owns_threads_manager_);
}

// NOTE: get_onmessage() is not yet implemented (low priority)

STDMETHODIMP GearsWorkerPool::createWorker(const BSTR *full_script_bstr,
                                           int *retval) {
  Initialize();

  const char16 *full_script = *full_script_bstr;

  int worker_id_temp;  // protects against modifying output param on failure
  bool succeeded = threads_manager_->CreateThread(full_script,
                                                  true,  // is_param_script
                                                  &worker_id_temp);
  if (!succeeded) {
    RETURN_EXCEPTION(STRING16(L"Internal error."));
  }

  *retval = worker_id_temp;
  RETURN_NORMAL();
}

STDMETHODIMP GearsWorkerPool::createWorkerFromUrl(const BSTR *url_bstr,
                                                  int *retval) {
  Initialize();

  // Make sure URLs are only fetched from the main thread.
  // TODO(michaeln): Remove this limitation of Firefox HttpRequest someday.
  if (EnvIsWorker()) {
    RETURN_EXCEPTION(STRING16(L"createWorkerFromUrl() cannot be called from a"
                              L" worker."));
  }
  
  const char16 *url = *url_bstr;

  int worker_id_temp;  // protects against modifying output param on failure
  bool succeeded = threads_manager_->CreateThread(url,
                                                  false,  // is_param_script
                                                  &worker_id_temp);
  if (!succeeded) {
    RETURN_EXCEPTION(STRING16(L"Internal error."));
  }

  *retval = worker_id_temp;
  RETURN_NORMAL();
}

STDMETHODIMP GearsWorkerPool::allowCrossOrigin() {
  Initialize();

  if (owns_threads_manager_) {
    RETURN_EXCEPTION(STRING16(L"Method is only used by child workers."));
  }

  threads_manager_->AllowCrossOrigin();
  RETURN_NORMAL();
}

STDMETHODIMP GearsWorkerPool::sendMessage(const BSTR *message_bstr,
                                          int dest_worker_id) {
  Initialize();

  const char16 *text = *message_bstr;

  bool succeeded = threads_manager_->PutPoolMessage(text,
                                                    dest_worker_id,
                                                    EnvPageSecurityOrigin());
  if (!succeeded) {
    std::string16 worker_id_string;
    IntegerToString(dest_worker_id, &worker_id_string);

    std::string16 error(STRING16(L"Worker "));
    error += worker_id_string;
    error += STRING16(L" does not exist.");

    RETURN_EXCEPTION(error.c_str());
  }
  RETURN_NORMAL();
}

STDMETHODIMP GearsWorkerPool::put_onmessage(const VARIANT *in_value) {
  scoped_ptr<JsRootedCallback> onmessage_handler;

  if (!ActiveXUtils::VariantIsNullOrUndefined(in_value)) {
    if (in_value->vt == VT_DISPATCH) {
      onmessage_handler.reset(new JsRootedCallback(NULL, *in_value));
    } else {
      RETURN_EXCEPTION(STRING16(L"The onmessage callback must be a function."));
    }
  }

  Initialize();

  if (!threads_manager_->SetCurrentThreadMessageHandler(
                             onmessage_handler.release())) {
    RETURN_EXCEPTION(STRING16(L"Error setting onmessage handler."));
  }

  RETURN_NORMAL();
}

STDMETHODIMP GearsWorkerPool::put_onerror(const VARIANT *in_value) {
  scoped_ptr<JsRootedCallback> onerror_handler;

  if (!ActiveXUtils::VariantIsNullOrUndefined(in_value)) {
    if (in_value->vt == VT_DISPATCH) {
      onerror_handler.reset(new JsRootedCallback(NULL, *in_value));
    } else {
      RETURN_EXCEPTION(STRING16(L"The onerror callback must be a function."));
    }
  }

  Initialize();

  if (!threads_manager_->SetCurrentThreadErrorHandler(
                             onerror_handler.release())) {
    // Currently, the only reason this can fail is because of this one
    // particular error.
    // TODO(aa): We need a system throughout Gears for being able to handle
    // exceptions from deep inside the stack better.
    RETURN_EXCEPTION(STRING16(L"The onerror property cannot be set from "
                              L"inside a worker"));
  }

  RETURN_NORMAL();
}

#ifdef DEBUG
STDMETHODIMP GearsWorkerPool::forceGC() {
  // TODO(aa): Investigate why this is crashing the unit tests.  In the
  // meantime, not a big deal for IE.  forceGC() was added for finding garbage
  // collection bugs on Firefox, where we control the JS engine more manually.
  // Though it can also be useful for debugging JsRootedToken on all browsers.
  //
  //threads_manager_->ForceGCCurrentThread();
  RETURN_NORMAL();
}
#endif

void GearsWorkerPool::HandleEventUnload(void *user_param) {
  GearsWorkerPool *wp = static_cast<GearsWorkerPool*>(user_param);
  if (wp->threads_manager_) {
    wp->threads_manager_->ShutDown();
  }
}

void GearsWorkerPool::Initialize() {
  if (!threads_manager_) {
    assert(EnvPageSecurityOrigin().full_url() == EnvPageLocationUrl());
    SetThreadsManager(new PoolThreadsManager(EnvPageSecurityOrigin(),
                                             GetJsRunner()));
    owns_threads_manager_ = true;
  }

  // Monitor 'onunload' to shutdown threads when the page goes away.
  //
  // A thread that keeps running after the page changes can cause odd problems,
  // if it continues to send messages. (This can happen if it busy-loops.)  On
  // Firefox, such a thread triggered the Print dialog after the page changed!
  //
  // TODO(cprince): Expose HTML event notifications to threads other than
  // the main thread.  If a worker thread creates a _new_ workerpool + thread,
  // the latter thread will not get destroyed. (TBD: do we need to make sure
  // thread hierarchies get cleaned up in a particular order?)
  if (!EnvIsWorker() && unload_monitor_ == NULL) {
    unload_monitor_.reset(new HtmlEventMonitor(kEventUnload,
                                               HandleEventUnload, this));
    CComPtr<IHTMLWindow3> event_source;
    IUnknown *site = this->EnvPageIUnknownSite();
    assert(site);
    HRESULT hr = ActiveXUtils::GetHtmlWindow3(site, &event_source);
    assert(SUCCEEDED(hr));
    unload_monitor_->Start(event_source);
  }
}


//
// PageTheadsManager -- handles threading and JS engine setup.
//

PoolThreadsManager::PoolThreadsManager(
                        const SecurityOrigin &page_security_origin,
                        JsRunnerInterface *root_js_runner)
    : num_workers_(0),
      is_shutting_down_(false),
      page_security_origin_(page_security_origin) {

  // Add a JavaScriptWorkerInfo entry for the owning worker.
  JavaScriptWorkerInfo *wi = new JavaScriptWorkerInfo;
  wi->threads_manager = this;
  wi->js_runner = root_js_runner;
  InitWorkerThread(wi);
  worker_info_.push_back(wi);
}


PoolThreadsManager::~PoolThreadsManager() {
  for (size_t i = 0; i < worker_info_.size(); ++i) {
    delete worker_info_[i];
  }
}


int PoolThreadsManager::GetCurrentPoolWorkerId() {
  // no MutexLock here because this function is private, and callers are
  // responsible for acquiring the exclusive lock
  DWORD os_thread_id = ::GetCurrentThreadId();

  // lookup OS-defined id in list of known mappings
  // (linear scan is fine because number of threads per pool will be small)
  int count = static_cast<int>(worker_id_to_os_thread_id_.size());

  for (int i = 0; i < count; ++i) {
    if (worker_id_to_os_thread_id_[i] == os_thread_id) {
      assert(i < static_cast<int>(worker_info_.size()));
      assert(worker_info_[i]);
      return i;
    }
  }

  assert(false);
  return kInvalidWorkerId;
}


void PoolThreadsManager::AllowCrossOrigin() {
  MutexLock lock(&mutex_);

  int current_worker_id = GetCurrentPoolWorkerId();
  JavaScriptWorkerInfo *wi = worker_info_[current_worker_id];

  // is_factory_suspended ensures ...UpdatePermissions() happens at most once,
  // and only for cross-origin workers.
  if (wi->is_factory_suspended) {
    wi->is_factory_suspended = false;
    wi->factory_ptr->ResumeObjectCreationAndUpdatePermissions();
  }
}


void PoolThreadsManager::HandleError(const JsErrorInfo &error_info) {
  MutexLock lock(&mutex_);

  int src_worker_id = GetCurrentPoolWorkerId();

  // TODO(cprince): Add the following lines when ReadyStateChanged doesn't need
  // to be called from the main thread -- i.e. when HttpRequest can fetch URLs
  // from threads other than the main thread.
  //   // We only expect to receive errors from created workers.
  //   assert(src_worker_id != kOwningWorkerId);

  std::string16 text;
  FormatWorkerPoolErrorMessage(error_info, src_worker_id, &text);

  JavaScriptWorkerInfo *dest_wi = worker_info_[kOwningWorkerId];  // parent

  // Copy the message to an internal queue.
  dest_wi->message_queue.push(Message(text, src_worker_id,
                                      dest_wi->script_origin));
  // Notify the receiving worker.
  PostMessage(dest_wi->message_hwnd, WM_WORKERPOOL_ONERROR, 0,
              reinterpret_cast<LPARAM>(dest_wi));
}


bool PoolThreadsManager::PutPoolMessage(const char16 *text,
                                        int dest_worker_id,
                                        const SecurityOrigin &src_origin) {
  MutexLock lock(&mutex_);

  int src_worker_id = GetCurrentPoolWorkerId();

  // check for valid dest_worker_id
  if (dest_worker_id < 0 ||
      dest_worker_id >= static_cast<int>(worker_info_.size())) {
    return false;
  }
  JavaScriptWorkerInfo *dest_wi = worker_info_[dest_worker_id];
  if (NULL == dest_wi || NULL == dest_wi->threads_manager ||
      NULL == dest_wi->message_hwnd) {
    return false;
  }

  // Copy the message to an internal queue.
  dest_wi->message_queue.push(Message(text, src_worker_id, src_origin));
  // Notify the receiving worker.
  PostMessage(dest_wi->message_hwnd, WM_WORKERPOOL_ONMESSAGE, 0,
              reinterpret_cast<LPARAM>(dest_wi));

  return true; // succeeded
}


bool PoolThreadsManager::GetPoolMessage(Message *msg) {
  MutexLock lock(&mutex_);

  int current_worker_id = GetCurrentPoolWorkerId();
  JavaScriptWorkerInfo *wi = worker_info_[current_worker_id];

  assert(!wi->message_queue.empty());

  *msg = wi->message_queue.front();
  wi->message_queue.pop();
  return true;
}


bool PoolThreadsManager::InitWorkerThread(JavaScriptWorkerInfo *wi) {
  MutexLock lock(&mutex_);

  // Sanity-check that we're not calling this twice. Doing so would mean we
  // created multiple hwnds for the same worker, which would be bad.
  assert(!wi->message_hwnd);

  // Register this worker so that it can be looked up by OS thread ID.
  DWORD os_thread_id = ::GetCurrentThreadId();
  worker_id_to_os_thread_id_.push_back(os_thread_id);

  // Also create a message-only window for every worker (including parent).
  // This is how we service JS worker messages synchronously relative to other
  // JS execution.
  //
  // Can create a message-only window by specifying HWND_MESSAGE as the parent.
  // TODO(cprince): an alternative is to set another message-only window as the
  // parent; see if that helps with cleanup.
  static const char16* kWindowClassName = STRING16(L"JsWorkerCommWnd");

  // hinstance should be the Gears DLL's base address, not the process handle
  //   (http://blogs.msdn.com/oldnewthing/archive/2005/04/18/409205.aspx)
  HMODULE hmodule = _AtlBaseModule.GetModuleInstance();

  WNDCLASSEX wc = {0};
  wc.cbSize        = sizeof(wc);
  wc.lpfnWndProc   = ThreadWndProc;
  wc.hInstance     = hmodule;
  wc.lpszClassName = kWindowClassName;
  // use 0 for all other fields
  RegisterClassEx(&wc);

  HWND message_hwnd;
  message_hwnd = CreateWindow(kWindowClassName, // class name
                              kWindowClassName, // window name
                              0 ,               // style
                              0, 0,             // pos
                              0, 0,             // size
                              HWND_MESSAGE,     // parent
                              0,                // ID if a child, else hMenu
                              hmodule,          // module instance
                              NULL);            // fooCREATESTRUCT param
  wi->message_hwnd = message_hwnd;
  return message_hwnd != NULL;
}


void PoolThreadsManager::UninitWorkerThread() {
  MutexLock lock(&mutex_);

  // Destroy message hwnd.
  int worker_id = GetCurrentPoolWorkerId();
  JavaScriptWorkerInfo *wi = worker_info_[worker_id];

  if (wi->message_hwnd) {
    BOOL success = DestroyWindow(wi->message_hwnd);
    assert(success);
  }

  // No need to remove os thread to worker id mapping.
}


bool PoolThreadsManager::SetCurrentThreadMessageHandler(
                             JsRootedCallback *handler) {
  MutexLock lock(&mutex_);

  int worker_id = GetCurrentPoolWorkerId();
  JavaScriptWorkerInfo *wi = worker_info_[worker_id];

  // This is where we take ownership of the handler.  If the function returns
  // before this point, we need to delete handler.
  wi->onmessage_handler.reset(handler);
  return true;
}


bool PoolThreadsManager::SetCurrentThreadErrorHandler(
                             JsRootedCallback *handler) {
  MutexLock lock(&mutex_);

  int worker_id = GetCurrentPoolWorkerId();
  if (kOwningWorkerId != worker_id) {
    // TODO(aa): Change this error to an assert when we remove the ability to
    // set 'onerror' from created workers.
    return false;
  }

  JavaScriptWorkerInfo *wi = worker_info_[worker_id];

  // This is where we take ownership of the handler.  If the function returns
  // before this point, we need to delete handler.
  wi->onerror_handler.reset(handler);
  return true;
}


class CreateWorkerUrlFetchListener : public HttpRequest::ReadyStateListener {
 public:
  explicit CreateWorkerUrlFetchListener(JavaScriptWorkerInfo *wi) : wi_(wi) {}

  virtual void ReadyStateChanged(HttpRequest *source) {
    HttpRequest::ReadyState ready_state = HttpRequest::UNINITIALIZED;
    source->GetReadyState(&ready_state);
    if (ready_state == HttpRequest::COMPLETE) {
      // Fetch completed.  First, unregister this listener.
      source->SetOnReadyStateChange(NULL);

      int status_code;
      std::string16 body;
      std::string16 final_url;
      if (source->GetStatus(&status_code) &&
          status_code == HttpConstants::HTTP_OK &&
          source->GetResponseBodyAsText(&body) &&
          source->GetFinalUrl(&final_url)) {
        // These are purposely set before locking mutex, because they are still
        // owned by the parent thread at this point.
        wi_->script_ok = true;
        wi_->script_text += body;
        // Must use security origin of final url, in case there were redirects.
        wi_->script_origin.InitFromUrl(final_url.c_str());
      } else {
        // Throw an error, but don't return!  Continue and set script_signalled
        // so the worker doesn't spin forever in Mutex::Await().
        std::string16 message(STRING16(L"Failed to load script."));
        std::string16 status_line;
        if (source->GetStatusLine(&status_line)) {
          message += STRING16(L" Status: ");
          message += status_line;
        }
        std::string16 requested_url;
        if (source->GetInitialUrl(&requested_url)) {
          message += STRING16(L" URL: ");
          message += requested_url;
        }
        JsErrorInfo error_info = { 0, message };  // line, message
        wi_->threads_manager->HandleError(error_info);
      }

      wi_->script_mutex.Lock();
      assert(!wi_->script_signalled);
      wi_->script_signalled = true;
      wi_->script_mutex.Unlock();
    }
  }
 private:
  JavaScriptWorkerInfo *wi_;
};


bool PoolThreadsManager::CreateThread(const char16 *url_or_full_script,
                                      bool is_param_script, int *worker_id) {
  int new_worker_id = -1;
  JavaScriptWorkerInfo *wi = NULL;
  {
    MutexLock lock(&mutex_);

    // If the creating thread didn't intialize properly it doesn't have a
    // message queue, so there's no point in letting it start a new thread.
    if (!worker_info_[GetCurrentPoolWorkerId()]->message_hwnd) {
      return false;
    }

    // Add a JavaScriptWorkerInfo entry.
    // Is okay not to undo this if code below fails. Behavior will be correct.
    worker_info_.push_back(new JavaScriptWorkerInfo);
    new_worker_id = static_cast<int>(worker_info_.size()) - 1;
    wi = worker_info_.back();
  }

  // The code below should not access shared data structures. We
  // only synchronize the block above, which modifies the shared 
  // worker_info_ vector.

  wi->threads_manager = this;

  wi->script_text = kWorkerInsertedPreamble;
  if (is_param_script) {
    wi->script_ok = true;
    wi->script_text += url_or_full_script;
    wi->script_origin = page_security_origin_;

    wi->script_mutex.Lock();
    wi->script_signalled = true;
    wi->script_mutex.Unlock();
  } else {
    // For URL params we start an async fetch here.  The created thread will
    // setup an incoming message queue, then Mutex::Await for the script to be
    // fetched, before finally pumping messages.

    wi->http_request.reset(HttpRequest::Create());
    if (!wi->http_request.get()) { return false; }
    
    wi->http_request_listener.reset(new CreateWorkerUrlFetchListener(wi));
    if (!wi->http_request_listener.get()) { return false; }

    HttpRequest *request = wi->http_request.get();  // shorthand

    request->SetOnReadyStateChange(wi->http_request_listener.get());
    request->SetCachingBehavior(HttpRequest::USE_ALL_CACHES);
    request->SetRedirectBehavior(HttpRequest::FOLLOW_ALL);

    std::string16 url;
    ResolveAndNormalize(page_security_origin_.full_url().c_str(),
                  url_or_full_script, &url);

    bool is_async = true;
    if (!request->Open(HttpConstants::kHttpGET, url.c_str(), is_async) ||
        !request->Send()) {
      request->SetOnReadyStateChange(NULL);
      request->Abort();
      return false;
    }

    // 'script_signalled' will be set when async fetch completes.
  }

  // Setup notifier to know when thread init has finished.
  // Then create thread and wait for signal.
  wi->thread_init_mutex.Lock();
  wi->thread_init_signalled = false;

  wi->thread_handle.reset((HANDLE)_beginthreadex(
                              NULL, // security descriptor
                              0, // stack size (or 0 for default)
                              JavaScriptThreadEntry, // start address
                              wi, // argument pointer
                              0, // initial state (0 for running)
                              NULL)); // variable to receive thread ID
  if (wi->thread_handle != 0) { // 0 means _beginthreadex() error
    // thread needs to finish message queue init before we continue
    wi->thread_init_mutex.Await(Condition(&wi->thread_init_signalled));
  }

  // cleanup notifier
  wi->thread_init_mutex.Unlock();

  if (wi->thread_handle == 0 || !wi->thread_init_ok) {
    return false; // failed
  }

  *worker_id = new_worker_id;
  return true; // succeeded
}


// Creates the JS engine, then pumps messages for the thread.
unsigned __stdcall PoolThreadsManager::JavaScriptThreadEntry(void *args) {
  assert(args);
  JavaScriptWorkerInfo *wi = static_cast<JavaScriptWorkerInfo*>(args);
  wi->threads_manager->AddWorkerRef();

  // Setup worker thread.
  // Then signal that initialization is done, and indicate success/failure.
  //
  // WARNING: must fire thread_init_signalled even on failure, or caller won't
  // continue.  So fire it from a non-nested location, before any early exits.
  scoped_ptr<JsRunnerInterface> js_runner(NewJsRunner());
  assert(NULL == wi->js_runner);
  wi->js_runner = js_runner.get();

  bool thread_init_succeeded = (NULL != js_runner.get()) &&
                               wi->threads_manager->InitWorkerThread(wi);

  wi->thread_init_ok = thread_init_succeeded;
  wi->thread_init_mutex.Lock();
  wi->thread_init_signalled = true;
  wi->thread_init_mutex.Unlock();

  if (thread_init_succeeded) {
    // Block until 'script_signalled' (i.e. wait for URL fetch, if being used).
    // Thread shutdown will set this flag as well.
    wi->script_mutex.Lock();
    wi->script_mutex.Await(Condition(&wi->script_signalled));
    wi->script_mutex.Unlock();

    if (wi->script_ok) {
      if (SetupJsRunner(js_runner.get(), wi)) {
        // Add JS code to engine.  Any script errors trigger HandleError().
        js_runner->Start(wi->script_text);
      }
    }

    // Pump messages. We do this whether or not the initial script evaluation
    // succeeded (just like in browsers).
    MSG msg;
    BOOL ret; // 0 if WM_QUIT, else non-zero (but -1 if error)
    while (ret = GetMessage(&msg, NULL, 0, 0)) {
      // check flag after waiting, before handling (see ShutDown)
      if (wi->threads_manager->is_shutting_down_) { break; } 
      if (ret != -1) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    }
  }

  // TODO(aa): Consider deleting wi here and setting PTM.worker_info_[i] to
  // NULL. This allows us to free up these thread resources sooner, and it
  // seems a little cleaner too.
  wi->js_runner = NULL;  // scoped_ptr is about to delete the underlying object
  wi->threads_manager->ReleaseWorkerRef();

  return 0; // value is currently unused
}

bool PoolThreadsManager::SetupJsRunner(JsRunnerInterface *js_runner,
                                       JavaScriptWorkerInfo *wi) {
  if (!js_runner) { return false; }

  // Add global Factory and WorkerPool objects into the namespace.
  //  
  // The factory alone is not enough; GearsFactory.create(GearsWorkerPool)
  // would return a NEW PoolThreadsManager instance, but we want access to
  // the one that was previously created for the current page.
  //
  // js_runner manages the lifetime of these allocated objects.
  // ::CreateInstance does not AddRef (see MSDN), but js_runner handles it.

  CComObject<GearsFactory> *factory;
  HRESULT hr = CComObject<GearsFactory>::CreateInstance(&factory);
  if (FAILED(hr)) { return false; }

  if (!factory->InitBaseManually(true, // is_worker
                                 NULL, // page_site is NULL in workers
                                 wi->script_origin,
                                 js_runner)) {
    return false;
  }

  CComObject<GearsWorkerPool> *workerpool;
  hr = CComObject<GearsWorkerPool>::CreateInstance(&workerpool);
  if (FAILED(hr)) { return false; }

  if (!workerpool->InitBaseManually(true, // is_worker
                                    NULL, // page_site is NULL in workers
                                    wi->script_origin,
                                    js_runner)) {
    return false;
  }


  // This Factory always inherits opt-in permissions.
  factory->is_permission_granted_ = true;
  factory->is_permission_value_from_user_ = true;
  // But for cross-origin workers, object creation is suspended until the
  // callee invokes allowCrossOrigin().
  if (!wi->threads_manager->page_security_origin().IsSameOrigin(
                                                       wi->script_origin)) {
    factory->SuspendObjectCreation();
    wi->is_factory_suspended = true;
  }

  // This WorkerPool needs the same underlying PoolThreadsManager as its parent.
  workerpool->SetThreadsManager(wi->threads_manager);


  // Save an AddRef'd pointer to the factory so we can access it later.
  wi->factory_ptr = factory;
  wi->factory_ref = factory->_GetRawUnknown();


  // Expose created objects as globals in the JS engine.
  if (!js_runner->AddGlobal(kWorkerInsertedFactoryName,
                            factory->_GetRawUnknown(),
                            0)) {
    return false;
  }

  if (!js_runner->AddGlobal(kWorkerInsertedWorkerPoolName,
                            workerpool->_GetRawUnknown(),
                            0)) {
    return false;
  }


  // Register the PoolThreadsManager as the error handler for this JS engine.
  js_runner->SetErrorHandler(wi->threads_manager);

  return true;
}

LRESULT CALLBACK PoolThreadsManager::ThreadWndProc(HWND hwnd, UINT message_type,
                                                   WPARAM wparam,
                                                   LPARAM lparam) {
  switch (message_type) {
    case WM_WORKERPOOL_ONMESSAGE:
    case WM_WORKERPOOL_ONERROR: {
      // Dequeue the message and dispatch it
      JavaScriptWorkerInfo *wi = reinterpret_cast<JavaScriptWorkerInfo*>(lparam);
      assert(wi->message_hwnd == hwnd);

      // See if the workerpool has been invalidated (as on termination).
      if (wi->threads_manager->is_shutting_down_) { return NULL; }

      // Retrieve message information.
      Message msg;
      if (!wi->threads_manager->GetPoolMessage(&msg)) {
        return NULL;
      }

      if (message_type == WM_WORKERPOOL_ONMESSAGE) {
        wi->threads_manager->ProcessMessage(wi, msg);
      } else {
        assert(message_type == WM_WORKERPOOL_ONERROR);
        wi->threads_manager->ProcessError(wi, msg);
      }

      return 0; // anything will do; retval "depends on the message"
    }
  }
  return DefWindowProc(hwnd, message_type, wparam, lparam);
}


void PoolThreadsManager::ProcessMessage(JavaScriptWorkerInfo *wi,
                                        const Message &msg) {
  if (wi->onmessage_handler.get()) {
    // Setup the onerror parameter (type: Object).
    scoped_ptr<JsRootedToken> onmessage_param(
                                  wi->js_runner->NewObject(NULL));
    wi->js_runner->SetPropertyString(onmessage_param->token(),
                                     STRING16(L"text"),
                                     msg.text.c_str());
    wi->js_runner->SetPropertyInt(onmessage_param->token(),
                                  STRING16(L"sender"),
                                  msg.sender);
    wi->js_runner->SetPropertyString(onmessage_param->token(),
                                     STRING16(L"origin"),
                                     msg.origin.url().c_str());

    const int argc = 3;
    JsParamToSend argv[argc] = {
      { JSPARAM_STRING16, &msg.text },
      { JSPARAM_INT, &msg.sender },
      { JSPARAM_OBJECT_TOKEN, onmessage_param.get() }
    };
    wi->js_runner->InvokeCallback(wi->onmessage_handler.get(), argc, argv,
                                  NULL);
  } else {
    // It is an error to send a message to a worker that does not have an
    // onmessage handler.
    int worker_id = kInvalidWorkerId;

    // Synchronize only this section because HandleError() below also acquires
    // the lock and locks are not reentrant.
    {
      MutexLock lock(&mutex_);
      worker_id = GetCurrentPoolWorkerId();
    }

    JsErrorInfo error_info = {
      0, // line number -- What we really want is the line number in the
         // sending worker, but that would be hard to get.
      STRING16(L"Could not process message because worker does not have an "
               L"onmessage handler.")
    };

    // We go through the message queue even in the case where this happens on
    // the owning worker, just so that things are consistent for all cases.
    HandleError(error_info);
  }
}


void PoolThreadsManager::ProcessError(JavaScriptWorkerInfo *wi,
                                      const Message &msg) {
#ifdef DEBUG
  {
    // We only expect to be receive errors on the owning worker, all workers
    // forward their errors here (via HandleError).
    MutexLock lock(&mutex_);
    assert(kOwningWorkerId == GetCurrentPoolWorkerId());
  }
#endif

  if (wi->onerror_handler.get()) {
    // Setup the onerror parameter (type: Error).
    scoped_ptr<JsRootedToken> onerror_param(
                                  wi->js_runner->NewObject(STRING16(L"Error")));
    wi->js_runner->SetPropertyString(onerror_param->token(),
                                     STRING16(L"message"),
                                     msg.text.c_str());

    const int argc = 1;
    JsParamToSend argv[argc] = {
      { JSPARAM_OBJECT_TOKEN, onerror_param.get() }
    };
    wi->js_runner->InvokeCallback(wi->onerror_handler.get(), argc, argv, NULL);
  } else {
    // If there's no onerror handler, we bubble the error up to the owning
    // worker's script context. If that worker is also nested, this will cause
    // PoolThreadsManager::HandleError to get called again on that context.
    ThrowGlobalError(wi->js_runner, msg.text);
  }
}


void PoolThreadsManager::ShutDown() {
  MutexLock lock(&mutex_);

  assert(GetCurrentPoolWorkerId() == kOwningWorkerId);

  if (is_shutting_down_) { return; }
  is_shutting_down_ = true;

  for (size_t i = 0; i < worker_info_.size(); ++i) {
    JavaScriptWorkerInfo *wi = worker_info_[i];

    // Cancel any createWorkerFromUrl() network requests that might be pending.
    HttpRequest *request = wi->http_request.get();
    if (request) {
      request->SetOnReadyStateChange(NULL);
      request->Abort();
    }

    // If the worker is a created thread...
    if (wi->thread_handle != INVALID_HANDLE_VALUE) {
      // Ensure the thread isn't waiting on 'script_signalled'.
      wi->script_mutex.Lock();
      wi->script_signalled = true;
      wi->script_mutex.Unlock();

      // Ensure the thread sees 'is_shutting_down_' by sending a dummy message,
      // in case it is blocked waiting for messages.
      PostMessage(wi->message_hwnd, WM_WORKERPOOL_ONMESSAGE, 0, 0);

      // TODO(cprince): Improve handling of a worker spinning in a JS busy loop.
      // Ideas: (1) set it to the lowest thread priority level, or (2) interrupt
      // the JS engine externally (see IActiveScript::InterruptScriptThread
      // on IE, JS_THREADSAFE for Firefox).  We cannot simply terminate the
      // thread; that can leave us in a bad state (e.g. mutexes locked forever).
    }
  }
}


#ifdef DEBUG
void PoolThreadsManager::ForceGCCurrentThread() {
  MutexLock lock(&mutex_);

  int worker_id = GetCurrentPoolWorkerId();

  JavaScriptWorkerInfo *wi = worker_info_[worker_id];
  assert(wi->js_runner);
  wi->js_runner->ForceGC();
}
#endif // DEBUG


void PoolThreadsManager::AddWorkerRef() {
  AtomicIncrement(&num_workers_, 1);
}

void PoolThreadsManager::ReleaseWorkerRef() {
  if (AtomicIncrement(&num_workers_, -1) == 0) {
    delete this;
  }
}
