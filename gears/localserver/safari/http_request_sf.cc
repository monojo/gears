// Copyright 2007, Google Inc.
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

#include <CoreFoundation/CoreFoundation.h>

#include "gears/base/safari/browser_utils.h"
#include "gears/base/common/string_utils.h"
#include "gears/base/safari/scoped_cf.h"
#include "gears/base/safari/string_utils.h"
#include "gears/localserver/common/http_constants.h"
#include "gears/localserver/safari/http_request_sf.h"
#include "gears/third_party/scoped_ptr/scoped_ptr.h"

//------------------------------------------------------------------------------
// Create
//------------------------------------------------------------------------------
HttpRequest *HttpRequest::Create() {
  HttpRequest *request = new SFHttpRequest;
  
  request->AddReference();
  
  return request;
}

//------------------------------------------------------------------------------
// AddReference
//------------------------------------------------------------------------------
int SFHttpRequest::AddReference() {
  return ++ref_count_;
}

//------------------------------------------------------------------------------
// ReleaseReference
//------------------------------------------------------------------------------
int SFHttpRequest::ReleaseReference() {
  --ref_count_;
  
  if (ref_count_ < 1)
    delete this;
  
  return ref_count_;
}

//------------------------------------------------------------------------------
// GetReadyState
//------------------------------------------------------------------------------
bool SFHttpRequest::GetReadyState(long *state) {
  *state = ready_state_;
  return true;
}

//------------------------------------------------------------------------------
// GetResponseBody
//------------------------------------------------------------------------------
std::vector<unsigned char> *SFHttpRequest::GetResponseBody() {
  scoped_ptr< std::vector<unsigned char> > body(new std::vector<unsigned char>);
  if (!GetResponseBody(body.get())) {
    return NULL;
  } else {
    return body.release();
  }
}

//------------------------------------------------------------------------------
// GetResponseBody
//------------------------------------------------------------------------------
bool SFHttpRequest::GetResponseBody(std::vector<unsigned char> *body) {
  assert(body);
  unsigned long body_len = CFDataGetLength(body_.get());

  body->reserve(body_len);
  body->resize(body_len);
  
  if (body_len) {
    const unsigned char *ptr = CFDataGetBytePtr(body_.get());
    memcpy(&(*body)[0], ptr, body_len);
  }

  return true;
}

//------------------------------------------------------------------------------
// GetStatus
//------------------------------------------------------------------------------
bool SFHttpRequest::GetStatus(long *status) {
  if (!response_.get())
    return false;

  *status = CFHTTPMessageGetResponseStatusCode(response_.get());
  return true;
}

//------------------------------------------------------------------------------
// GetStatusText
//------------------------------------------------------------------------------
bool SFHttpRequest::GetStatusText(std::string16 *status_text) {
  // If we've got a response, copy everything to the right of the HTTP
  // response code in the response status
  if (response_.get()) {
    scoped_CFString
      status(CFHTTPMessageCopyResponseStatusLine(response_.get()));
    char buffer[1024];
    CFStringGetCString(status.get(), buffer, sizeof(buffer),
                       kCFStringEncodingUTF8);
    int response_code = CFHTTPMessageGetResponseStatusCode(response_.get());
    char response_buffer[16];
    snprintf(response_buffer, sizeof(response_buffer), "%d", response_code);
    char *response_text = strstr(buffer, response_buffer);
    if (response_text) {
      response_text += strlen(response_buffer);
      response_text += strspn(response_text, " \t");
      return UTF8ToString16(response_text, strlen(response_text), status_text);
    }
  }

  return false;
}

//------------------------------------------------------------------------------
// GetStatusLine
//------------------------------------------------------------------------------
bool SFHttpRequest::GetStatusLine(std::string16 *status_line) {
  if (response_.get()) {
    scoped_CFString 
      response_status(CFHTTPMessageCopyResponseStatusLine(response_.get()));
    return CFStringRefToString16(response_status.get(), status_line);
  }

  return false;
}

//------------------------------------------------------------------------------
// Open
//------------------------------------------------------------------------------
bool SFHttpRequest::Open(const char16 *method, const char16* url, bool async) {
  Reset();
  
  scoped_CFString method_str(CFStringCreateWithString16(method));  
  scoped_CFURL url_ref(CFURLCreateWithString16(url));
  
  request_.reset(CFHTTPMessageCreateRequest(kCFAllocatorDefault, 
                                            method_str.get(),
                                            url_ref.get(), kCFHTTPVersion1_1));
  ready_state_ = 0; // Uninitialized
  return (request_.get() != NULL);
}

//------------------------------------------------------------------------------
// SetRequestHeader
//------------------------------------------------------------------------------
bool SFHttpRequest::SetRequestHeader(const char16* name, const char16* value) {
  if (!request_.get())
    return false;

  scoped_CFString name_ref(CFStringCreateWithString16(name));
  scoped_CFString value_ref(CFStringCreateWithString16(value));
  CFHTTPMessageSetHeaderFieldValue(request_.get(), name_ref.get(),
                                   value_ref.get());
  return true;
}

//------------------------------------------------------------------------------
// SetFollowRedirects
//------------------------------------------------------------------------------
bool SFHttpRequest::SetFollowRedirects(bool follow) {
  follow_redirect_ = follow;
  return true;
}

bool SFHttpRequest::WasRedirected() {
  if (!response_.get())
    return false;
  
  bool result = false;
  if (follow_redirect_) {
    scoped_CFURL requested_url(CFHTTPMessageCopyRequestURL(request_.get()));
    scoped_CFURL actual_url(CFHTTPMessageCopyRequestURL(response_.get()));
    std::string16 requested, actual;
    CFURLRefToString16(requested_url.get(), &requested);
    CFURLRefToString16(actual_url.get(), &actual);
    result = (requested == actual) ? false : true;
  }
  
  return result;
}

bool SFHttpRequest::GetRedirectUrl(std::string16 *full_redirect_url) {
  if (!response_.get() || !follow_redirect_)
    return false;
  
  if (!WasRedirected())
    return false;
  
  scoped_CFURL final_url(CFHTTPMessageCopyRequestURL(response_.get()));
  
  return CFURLRefToString16(final_url.get(), full_redirect_url);
}


//------------------------------------------------------------------------------
// Send
//------------------------------------------------------------------------------
bool SFHttpRequest::Send() {
  if (!request_.get())
    return false;

  read_stream_.reset(
    CFReadStreamCreateForHTTPRequest(kCFAllocatorDefault, request_.get()));

  if (!read_stream_.get())
    return false;

  // Set the auto-redirection policy
  CFReadStreamSetProperty(read_stream_.get(),
                          kCFStreamPropertyHTTPShouldAutoredirect,
                          follow_redirect_ ? kCFBooleanTrue : kCFBooleanFalse);
  CFStreamClientContext context = { 0, (void *)this, NULL, NULL, NULL };

  CFOptionFlags events_mask = kCFStreamEventOpenCompleted |
    kCFStreamEventHasBytesAvailable | kCFStreamEventEndEncountered |
    kCFStreamEventErrorOccurred;
  bool result = false;

  // Attach a callback to the stream and schedule
  if (CFReadStreamSetClient(read_stream_.get(), events_mask,
                            StreamReaderFunc, &context)) {
    CFReadStreamScheduleWithRunLoop(read_stream_.get(), CFRunLoopGetCurrent(),
                                    kCFRunLoopCommonModes);
    result = CFReadStreamOpen(read_stream_.get());
  }

  if (!result)
    read_stream_.reset(NULL);

  return result;
}

//------------------------------------------------------------------------------
// GetAllResponseHeaders
//------------------------------------------------------------------------------
bool SFHttpRequest::GetAllResponseHeaders(std::string16 *headers) {
  if (!response_.get())
    return false;
  
  CopyHeadersIfAvailable();

  bool result = false;
  if (response_header_.get()) {
    scoped_CFMutableString str(CFStringCreateMutable(kCFAllocatorDefault, 0));
    CFDictionaryApplyFunction(response_header_.get(), 
                              ConcatenateResponseHeadersToString, str.get());
    result = CFStringRefToString16(str.get(), headers);
  }

  // Needs to be an extra CR/LF
  headers->append(HttpConstants::kCrLf);
  
  return result;
}

//------------------------------------------------------------------------------
// GetResponseHeader
//------------------------------------------------------------------------------
bool SFHttpRequest::GetResponseHeader(const char16* name, 
                                      std::string16 *header) {
  if (!response_.get())
    return false;

  CopyHeadersIfAvailable();

  bool result = false;
  if (response_header_.get()) {
    scoped_CFString name_str(CFStringCreateWithString16(name));
    
    if (CFDictionaryContainsKey(response_header_.get(),
                                (void *)name_str.get())) {
      CFStringRef value_str = reinterpret_cast<CFStringRef>
        (CFDictionaryGetValue(response_header_.get(), (void *)name_str.get()));
      result = CFStringRefToString16(value_str, header);
    }
  }
  
  return result;
}

//------------------------------------------------------------------------------
// Abort
//------------------------------------------------------------------------------
bool SFHttpRequest::Abort() {
  if (read_stream_.get()) {
    SetReadyState(4); // Complete
    TerminateStreamReader();
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// SetOnReadyStateChange
//------------------------------------------------------------------------------
bool SFHttpRequest::SetOnReadyStateChange(ReadyStateListener *listener) {
  listener_ = listener;
  return true;
}

//------------------------------------------------------------------------------
// SetReadyState
//------------------------------------------------------------------------------
void SFHttpRequest::SetReadyState(long ready_state) {
  if (ready_state != ready_state_) {
    ready_state_ = ready_state;
    if (listener_)
      listener_->ReadyStateChanged(this);
  }
}

//------------------------------------------------------------------------------
// SFHttpRequest
//------------------------------------------------------------------------------
SFHttpRequest::SFHttpRequest()
  : listener_(NULL),
    ref_count_(0),
    ready_state_(0),
    follow_redirect_(true),
    request_(NULL),
    read_stream_(NULL),
    response_(NULL),
    response_header_(NULL),
    body_(NULL) {
}

//------------------------------------------------------------------------------
// ~SFHttpRequest
//------------------------------------------------------------------------------
SFHttpRequest::~SFHttpRequest() {
  Reset();
}

//------------------------------------------------------------------------------
// static ConcatenateResponseHeadersToString
//------------------------------------------------------------------------------
void SFHttpRequest::ConcatenateResponseHeadersToString(const void *key,
                                                       const void *value,
                                                       void *context) {
  CFMutableStringRef dest = reinterpret_cast<CFMutableStringRef>(context);
  scoped_CFString str(CFStringCreateWithFormat(kCFAllocatorDefault, NULL,
                                               CFSTR("%@: %@\r\n"),
                                               key, value, NULL));
  CFStringAppend(dest, str.get());
}

//------------------------------------------------------------------------------
// static StreamReaderFunc
//------------------------------------------------------------------------------
void SFHttpRequest::StreamReaderFunc(CFReadStreamRef stream,
                                     CFStreamEventType type,
                                     void *clientCallBackInfo) {
  SFHttpRequest *self = reinterpret_cast<SFHttpRequest *>(clientCallBackInfo);

  switch (type) {
    case kCFStreamEventOpenCompleted:
      self->body_.reset(CFDataCreateMutable(kCFAllocatorDefault, 0));
      self->SetReadyState(1); // Loading
      break;

    case kCFStreamEventHasBytesAvailable: {
      // Append the data to the body a page at a time.  This will be called
      // repeatedly if there's more data available.
      CFIndex buffer_size = getpagesize();
      unsigned char *buffer = (unsigned char *)malloc(buffer_size);
      
      if (buffer) {
        CFIndex bytes_read;
        
        do {
          bytes_read = CFReadStreamRead(stream, buffer, buffer_size);
          CFIndex current_length = CFDataGetLength(self->body_.get());
          CFDataAppendBytes(self->body_.get(), buffer, bytes_read);
          CFIndex new_length = CFDataGetLength(self->body_.get());
          
          // Check for failure of appending the data
          if (current_length + bytes_read > new_length) {
            self->body_.reset(NULL);
            self->SetReadyState(4);
            self->TerminateStreamReader();
            return;
          }
        } while (bytes_read > 0);
        
        free(buffer);
      }
      
      break;
    }

    case kCFStreamEventEndEncountered: {
      self->SetReadyState(4); // Complete
      self->TerminateStreamReader();
      break;
    }

    case kCFStreamEventErrorOccurred: {
      self->SetReadyState(4); // Complete
      self->TerminateStreamReader();
      break;
    }
      
    default:
      break;
  }
  
  // Gather the response when available
  if (!self->response_.get()) {
    self->response_.reset(reinterpret_cast<CFHTTPMessageRef>
    (const_cast<void *>(CFReadStreamCopyProperty(stream, 
                                      kCFStreamPropertyHTTPResponseHeader))));
  }
}

//------------------------------------------------------------------------------
// TerminateStreamReader
//------------------------------------------------------------------------------
void SFHttpRequest::TerminateStreamReader() {
  if (!read_stream_.get())
    return;

  CFReadStreamSetClient(read_stream_.get(), kCFStreamEventNone, NULL, NULL);
  CFReadStreamUnscheduleFromRunLoop(read_stream_.get(), CFRunLoopGetCurrent(),
                                    kCFRunLoopCommonModes);
  CFReadStreamClose(read_stream_.get());
  read_stream_.reset(NULL);
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------
void SFHttpRequest::Reset() {
  TerminateStreamReader();
  body_.reset(NULL);
  response_header_.reset(NULL);
  response_.reset(NULL);
  request_.reset(NULL);
}

//------------------------------------------------------------------------------
// CopyHeadersIfAvailable
//------------------------------------------------------------------------------
void SFHttpRequest::CopyHeadersIfAvailable() {
  // Copy the headers from the response into the response_header_
  if (!response_header_.get() && 
      CFHTTPMessageIsHeaderComplete(response_.get())) {
    response_header_.reset(CFHTTPMessageCopyAllHeaderFields(response_.get()));
  }
}

