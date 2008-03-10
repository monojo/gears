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
// Shared utility for creating desktop shortcut files.

#ifdef WIN32

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tchar.h>

#include "gears/desktop/shortcut_utils_win32.h"

#include "gears/base/common/common.h"
#include "gears/base/common/int_types.h"
#include "gears/base/common/png_utils.h"
#include "gears/base/common/string16.h"
#include "gears/base/common/string_utils.h"
#include "gears/third_party/scoped_ptr/scoped_ptr.h"


static bool CreateShellLink(const char16 *link_path,
                            const char16 *icon_path,
                            const char16 *object_path,
                            const char16 *arguments) {
#ifdef WINCE
  // On WinCE we can't create simple shortcuts on the home screen, so instead we
  // add them to the Start Menu. Also, IShellLink does not exist on WinCE.
  // Instead, we use SHCreateShortcut and SHGetShortcutTarget, but these do not
  // allow us to set a custom icon.
  // TODO(steveblock): Use plugin to add shortcuts to the home screen with
  // custom icons.
  std::string16 target = object_path;
  target += L" -";
  target += arguments;
  return SHCreateShortcut(const_cast<char16*>(link_path),
                          const_cast<char16*>(target.c_str())) == TRUE;
#else
  HRESULT result;
  IShellLink* shell_link;

  result = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                            IID_IShellLink, (LPVOID*)&shell_link);
  if (SUCCEEDED(result)) {
    shell_link->SetPath(object_path);
    shell_link->SetArguments(arguments);
    shell_link->SetIconLocation(icon_path, 0);

    IPersistFile* persist_file;
    result = shell_link->QueryInterface(IID_IPersistFile,
        reinterpret_cast<void**>(&persist_file));

    if (SUCCEEDED(result)) {
      result = persist_file->Save(link_path, TRUE);
      persist_file->Release();
    }
    shell_link->Release();
  }
  return SUCCEEDED(result);
#endif
}

static bool ReadShellLink(const char16 *link_path,
                          std::string16 *icon_path,
                          std::string16 *object_path,
                          std::string16 *arguments) {
#ifdef WINCE
  char16 target[CHAR_MAX];
  return SHGetShortcutTarget(link_path, target, CHAR_MAX) == TRUE;
#else
  HRESULT result;
  IShellLink* shell_link;

  result = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                            IID_IShellLink, (LPVOID*)&shell_link);
  if (SUCCEEDED(result)) {
    IPersistFile* persist_file;
    result = shell_link->QueryInterface(
                             IID_IPersistFile,
                             reinterpret_cast<void**>(&persist_file));

    if (SUCCEEDED(result)) {
      result = persist_file->Load(link_path, TRUE);

      char16 icon_buffer[MAX_PATH];
      if (SUCCEEDED(result) && icon_path) {
        int icon_id;
        result = shell_link->GetIconLocation(icon_buffer, MAX_PATH, &icon_id);
      }
      char16 object_buffer[MAX_PATH];
      if (SUCCEEDED(result) && object_path) {
        result = shell_link->GetPath(object_buffer, MAX_PATH, NULL, 0);
      }
      char16 arg_buffer[MAX_PATH];
      if (SUCCEEDED(result) && arguments) {
        result = shell_link->GetArguments(arg_buffer, MAX_PATH);
      }

      if (SUCCEEDED(result)) {
        // Only set the return values when the function succeeded.
        if (icon_path)
          *icon_path = icon_buffer;
        if (object_path)
          *object_path = object_buffer;
        if (arguments)
          *arguments = arg_buffer;
      }
      persist_file->Release();
    }
    shell_link->Release();
  }
  return SUCCEEDED(result);
#endif
}

static bool GetShortcutLocationPath(std::string16 *shortcut_location_path) {
  assert(shortcut_location_path);
  bool succeeded = false;
  char16 path_buf[MAX_PATH];

  // We use the old version of this function because the new version apparently
  // won't tell you the Desktop folder path.
  BOOL result = SHGetSpecialFolderPath(NULL, path_buf,
#ifdef WINCE
                                       CSIDL_STARTMENU,
#else
                                       CSIDL_DESKTOPDIRECTORY,
#endif
                                       TRUE);

  if (result) {
    *shortcut_location_path = path_buf;
    succeeded = true;
  }
  return succeeded;
}

bool CreateShortcutFileWin32(const std::string16 &name,
                             const std::string16 &browser_path,
                             const std::string16 &url,
                             const std::string16 &icons_path,
                             std::string16 *error) {
  // Note: We assume that shortcut.app_name has been validated as a valid
  // filename and that the shortuct.app_url has been converted to absolute URL
  // by the caller.
  std::string16 link_path;
  if (!GetShortcutLocationPath(&link_path)) {
    *error = GET_INTERNAL_ERROR_MESSAGE();
    return false;
  }

  link_path += STRING16(L"\\");
  link_path += name;
  link_path += STRING16(L".lnk");

#ifdef WINCE
  // On WinCE we can't use the icon path to determine whether this shortcut was
  // created by Gears. We stay safe and fail if the shortcut already exists.
  if (ReadShellLink(link_path.c_str(), NULL, NULL, NULL)) {
    return false;
  }
#else
  std::string16 old_icon;
  if (ReadShellLink(link_path.c_str(), &old_icon, NULL, NULL)) {
    int old_icon_length = GetLongPathNameW(old_icon.c_str(), NULL, 0);
    scoped_array<char16> old_icon_buf(new char16[old_icon_length]);
    GetLongPathNameW(old_icon.c_str(), old_icon_buf.get(), old_icon_length);
    old_icon.assign(old_icon_buf.get());

    // [naming] -- we hardcode the name of the path that Gears shortcut icons
    // are stored in here because we want to be able to overwrite shortcuts no
    // matter which browser wrote them. This is particularly important during
    // testing, where a developer would frequently create the same shortcut with
    // multiple browsers.
    if (old_icon.find(STRING16(L"Google Gears for ")) == old_icon.npos) {
      *error = STRING16(L"Cannot overwrite shortcut not created by ");
      *error += PRODUCT_FRIENDLY_NAME;
      *error += STRING16(L".");
      return false;
    }
  }
#endif

  if (!CreateShellLink(link_path.c_str(), icons_path.c_str(),
                       browser_path.c_str(), url.c_str())) {
      *error = GET_INTERNAL_ERROR_MESSAGE();
      return false;
  }

  return true;
}
#endif  // #if defined(WIN32) && !defined(WINCE)