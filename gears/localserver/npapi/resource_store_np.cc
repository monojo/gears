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

#include "gears/localserver/npapi/resource_store_np.h"

#include "gears/localserver/npapi/file_submitter_np.h"


//------------------------------------------------------------------------------
// GetName
//------------------------------------------------------------------------------
void GearsResourceStore::GetName() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// GetRequiredCookie
//------------------------------------------------------------------------------
void GearsResourceStore::GetRequiredCookie() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// GetEnabled
//------------------------------------------------------------------------------
void GearsResourceStore::GetEnabled() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// SetEnabled
//------------------------------------------------------------------------------
void GearsResourceStore::SetEnabled() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// Capture
//------------------------------------------------------------------------------
void GearsResourceStore::Capture() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// AbortCapture
//------------------------------------------------------------------------------
void GearsResourceStore::AbortCapture() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// IsCaptured
//------------------------------------------------------------------------------
void GearsResourceStore::IsCaptured() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// Remove
//------------------------------------------------------------------------------
void GearsResourceStore::Remove() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// Rename
//------------------------------------------------------------------------------
void GearsResourceStore::Rename() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// Copy
//------------------------------------------------------------------------------
void GearsResourceStore::Copy() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}


//------------------------------------------------------------------------------
// CaptureFile
//------------------------------------------------------------------------------
void GearsResourceStore::CaptureFile() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// GetCapturedFileName
//------------------------------------------------------------------------------
void GearsResourceStore::GetCapturedFileName() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// GetHeader
//------------------------------------------------------------------------------
void GearsResourceStore::GetHeader() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// GetAllHeaders
//------------------------------------------------------------------------------
void GearsResourceStore::GetAllHeaders() {
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// CreateFileSubmitter
//------------------------------------------------------------------------------
void GearsResourceStore::CreateFileSubmitter() {
  if (EnvIsWorker()) {
    RETURN_EXCEPTION(
        STRING16(L"createFileSubmitter cannot be called in a worker."));
  }

  ScopedModuleWrapper submitter_wrapper(CreateGearsFileSubmitter(this));
  if (!submitter_wrapper.get())
    return;  // Create function sets an error message.

  JsToken retval = submitter_wrapper.get()->GetWrapperToken();
  GetJsRunner()->SetReturnValue(JSPARAM_OBJECT_TOKEN, &retval);
  RETURN_EXCEPTION(STRING16(L"Not Implemented"));
}

//------------------------------------------------------------------------------
// HandleEvent
//------------------------------------------------------------------------------
void GearsResourceStore::HandleEvent(JsEventType event_type) {
  assert(event_type == JSEVENT_UNLOAD);
}