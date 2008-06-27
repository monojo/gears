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

#ifndef GEARS_NOTIFIER_SYSTEM_H__
#define GEARS_NOTIFIER_SYSTEM_H__

#ifdef OFFICIAL_BUILD
  // The notification API has not been finalized for official builds.
#else

#include <string>

namespace glint {
class Rectangle;
}

class System {
 public:
  static std::string GetResourcePath();

  // Returns bounds suitable for showing UI (does not include system menu,
  // taskbar or sidebars that occupy the space on a side).
  static void GetMainScreenBounds(glint::Rectangle *bounds);

  // If the system supports "large fonts" setting by scaling fonts behind the
  // scene (as Windows does when it scales 10pt font to look like 18pt), this
  // will report the 'font multiplication factor" for our UI to follow.
  static double GetSystemFontScaleFactor();

 private:
  System() {}  // Static class.
};

#endif  // OFFICIAL_BUILD
#endif  // GEARS_NOTIFIER_SYSTEM_H__

