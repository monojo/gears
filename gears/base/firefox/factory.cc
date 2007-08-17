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

#include <assert.h>
#include <stdlib.h>
#include <nsXPCOM.h>
#include "gears/third_party/gecko_internal/nsIDOMClassInfo.h"

#include "gears/base/firefox/factory.h"

#include "gears/base/common/common.h"
#include "gears/base/common/factory_utils.h"
#include "gears/base/common/product_version.h"
#include "gears/base/common/string16.h"
#include "gears/base/firefox/dom_utils.h"
#include "gears/database/firefox/database.h"
#include "gears/httprequest/firefox/httprequest_ff.h"
#include "gears/localserver/firefox/localserver_ff.h"
#include "gears/timer/firefox/timer.h"
#include "gears/workerpool/firefox/workerpool.h"


// Boilerplate. == NS_IMPL_ISUPPORTS + ..._MAP_ENTRY_EXTERNAL_DOM_CLASSINFO
NS_IMPL_ADDREF(GearsFactory)
NS_IMPL_RELEASE(GearsFactory)
NS_INTERFACE_MAP_BEGIN(GearsFactory)
  NS_INTERFACE_MAP_ENTRY(GearsBaseClassInterface)
  NS_INTERFACE_MAP_ENTRY(GearsFactoryInterface)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, GearsFactoryInterface)
  NS_INTERFACE_MAP_ENTRY_EXTERNAL_DOM_CLASSINFO(GearsFactory)
NS_INTERFACE_MAP_END

// Object identifiers
const char *kGearsFactoryContractId = "@google.com/gears/factory;1"; // [naming]
const char *kGearsFactoryClassName = "GearsFactory";
const nsCID kGearsFactoryClassId = {0x93b2e433, 0x35ab, 0x46e7, {0xa9, 0x50,
                                    0x41, 0x8f, 0x92, 0x2c, 0xc6, 0xef}};
                                   // {93B2E433-35AB-46e7-A950-418F922CC6EF}


GearsFactory::GearsFactory()
    : is_creation_suspended_(false),
      is_permission_granted_(false),
      is_permission_value_from_user_(false) {
  SetActiveUserFlag();
}


NS_IMETHODIMP GearsFactory::Create(const nsAString &object_nsstring,
                                   const nsAString &version_nsstring,
                                   nsISupports **retval) {
  nsresult nr;

  // Make sure the user gives this site permission to use Gears.

  if (!HasPermissionToUseGears(this)) {
    RETURN_EXCEPTION(STRING16(L"Page does not have permission to use "
                              PRODUCT_FRIENDLY_NAME L"."));
  }

  JsParamFetcher js_params(this);

  // Parse the version string.

  std::string16 version;
  if (!js_params.GetAsString(1, &version)) {
    RETURN_EXCEPTION(STRING16(L"Invalid parameter."));
  }

  int major_version_desired;
  int minor_version_desired;
  if (!ParseMajorMinorVersion(version.c_str(),
                              &major_version_desired,
                              &minor_version_desired)) {
    RETURN_EXCEPTION(STRING16(L"Invalid version string."));
  }

  // Create an instance of the object.
  //
  // Do case-sensitive comparisons, which are always better in APIs. They make
  // code consistent across callers, and they are easier to support over time.

  std::string16 object;
  if (!js_params.GetAsString(0, &object)) {
    RETURN_EXCEPTION(STRING16(L"Invalid parameter."));
  }

  nsCOMPtr<nsISupports> isupports = NULL;

  nr = NS_ERROR_FAILURE;
  if (object == STRING16(L"beta.database")) {
    if (major_version_desired == kGearsDatabaseVersionMajor &&
        minor_version_desired <= kGearsDatabaseVersionMinor) {
      isupports = do_QueryInterface(new GearsDatabase(), &nr);
    }
  } else if (object == STRING16(L"beta.httprequest")) {
    if (major_version_desired == kGearsHttpRequestVersionMajor &&
        minor_version_desired <= kGearsHttpRequestVersionMinor) {
      isupports = do_QueryInterface(new GearsHttpRequest(), &nr);
    }
  } else if (object == STRING16(L"beta.localserver")) {
    if (major_version_desired == kGearsLocalServerVersionMajor &&
        minor_version_desired <= kGearsLocalServerVersionMinor) {
      isupports = do_QueryInterface(new GearsLocalServer(), &nr);
    }
  } else if (object == STRING16(L"beta.timer")) {
    if (major_version_desired == kGearsTimerVersionMajor &&
        minor_version_desired <= kGearsTimerVersionMinor) {
      isupports = do_QueryInterface(new GearsTimer(), &nr);
    }
  } else if (object == STRING16(L"beta.workerpool")) {
    if (major_version_desired == kGearsWorkerPoolVersionMajor &&
        minor_version_desired <= kGearsWorkerPoolVersionMinor) {
      isupports = do_QueryInterface(new GearsWorkerPool(), &nr);
    }
  } else {
    RETURN_EXCEPTION(STRING16(L"Unknown object."));
  }

  // setup the GearsBaseClass (copy settings from this factory)
  if (NS_SUCCEEDED(nr) && isupports) {
    bool base_init_succeeded = false;

    nsCOMPtr<GearsBaseClassInterface> idl_base =
        do_QueryInterface(isupports, &nr);
    if (NS_SUCCEEDED(nr) && idl_base) {
      GearsBaseClass *native_base = NULL;
      idl_base->GetNativeBaseClass(&native_base);
      if (native_base) {
        if (native_base->InitBaseFromSibling(this)) {
          base_init_succeeded = true;
        }
      }
    }
    if (!base_init_succeeded) {
      RETURN_EXCEPTION(STRING16(L"Error initializing base class."));
    }
  }

  if (NS_FAILED(nr) || !isupports) {
    RETURN_EXCEPTION(STRING16(L"Failed to create requested object."));
  }

  *retval = isupports.get();
  (*retval)->AddRef(); // ~nsCOMPtr will Release, so must AddRef here
  assert((*retval)->AddRef() == 3 &&
         (*retval)->Release() == 2);
  RETURN_NORMAL();
}


NS_IMETHODIMP GearsFactory::GetBuildInfo(nsAString &retval) {
  std::string16 build_info;
  AppendBuildInfo(&build_info);
  retval.Assign(build_info.c_str());
  RETURN_NORMAL();
}


// TODO(cprince): See if we can use Suspend/Resume with the opt-in dialog too,
// rather than just the cross-origin worker case.  (Code re-use == good.)
void GearsFactory::SuspendObjectCreation() {
  is_creation_suspended_ = true;
}

void GearsFactory::ResumeObjectCreationAndUpdatePermissions() {
  // TODO(cprince): The transition from suspended to resumed is where we should
  // propagate cross-origin opt-in to the permissions DB.
  is_creation_suspended_ = false;
}
