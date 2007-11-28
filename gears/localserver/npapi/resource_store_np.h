// Copyright 2005, Google Inc.
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

#ifndef GEARS_LOCALSERVER_NPAPI_RESOURCE_STORE_NP_H__
#define GEARS_LOCALSERVER_NPAPI_RESOURCE_STORE_NP_H__

#include "gears/base/common/base_class.h"
#include "gears/base/common/common.h"
#include "gears/base/common/js_runner.h"
#include "gears/base/common/string16.h"
#include "gears/third_party/scoped_ptr/scoped_ptr.h"
#include "gears/localserver/common/resource_store.h"

//-----------------------------------------------------------------------------
// GearsResourceStore
//-----------------------------------------------------------------------------
class GearsResourceStore
    : public ModuleImplBaseClass,
      public JsEventHandlerInterface {
 public:
  GearsResourceStore() {}

  // OUT: string name
  void GetName();

  // OUT: string cookie
  void GetRequiredCookie();

  // OUT: bool enabled
  void GetEnabled();
  // IN: bool enabled
  void SetEnabled();

  // IN: string[] url_or_url_array
  // IN: function completion_callback
  // OUT: int capture_id
  void Capture();

  // IN: int capture_id
  void AbortCapture();

  // IN: string url
  // OUT: bool is_captured
  void IsCaptured();

  // IN: string url
  void Remove();

  // IN: string src_url
  // IN: string dst_url
  void Rename();

  // IN: string src_url
  // IN: string dst_url
  void Copy();

  // IN: string url
  // IN: string header_name
  // OUT: string header
  void GetHeader();

  // IN: string url
  // OUT: string headers
  void GetAllHeaders();

  // IN: HtmlElement file_input_element
  // IN: string url
  void CaptureFile();

  // IN: string url
  // OUT: string filename
  void GetCapturedFileName();

  // OUT: GearsFileSubmitter *retval
  void CreateFileSubmitter();

  virtual void HandleEvent(JsEventType event_type);

 private:
  scoped_ptr<JsEventMonitor> unload_monitor_;

  DISALLOW_EVIL_CONSTRUCTORS(GearsResourceStore);
};

ModuleWrapperBaseClass *CreateGearsResourceStore(ModuleImplBaseClass *sibling);

#endif // GEARS_LOCALSERVER_NPAPI_RESOURCE_STORE_NP_H__