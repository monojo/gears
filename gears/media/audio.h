// Copyright 2008, Google Inc.
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
// Audio player provides playback abilities, similar to HTML5 AudioElement

#ifndef GEARS_MEDIA_AUDIO_H__
#define GEARS_MEDIA_AUDIO_H__

#include "gears/base/common/base_class.h"
#include "gears/base/common/common.h"
#include "gears/media/media.h"

class GearsAudio
    : public GearsMedia,
      public ModuleImplBaseClass {
 public:
  GearsAudio();

  // 1. TODO(aprasath): API from GearsMedia that are overridden should be
  // added here later. Overriding of couple of API shown below.
  void GetError(JsCallContext *context);
  void GetBuffered(JsCallContext *context);

  // 2. TODO(aprasath): Any new API introduced in this subclass
  // is to be added here.

 protected:
  ~GearsAudio();

 private:
  DISALLOW_EVIL_CONSTRUCTORS(GearsAudio);
};
#endif  // GEARS_MEDIA_AUDIO_H__
