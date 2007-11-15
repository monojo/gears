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

#include "gears/base/npapi/plugin.h"

#include "gears/base/npapi/browser_utils.h"

// static
template<class T>
NPObject* PluginBase<T>::Allocate(NPP npp, NPClass *npclass) {
  // Initialize property and method mappings for the derived class.
  static bool did_init = false;
  if (!did_init) {
    did_init = true;
    PluginClass::InitClass();
  }

  return new PluginClass(npp);
}

// static
template<class T>
void PluginBase<T>::Deallocate(NPObject *npobj) {
  delete static_cast<PluginClass *>(npobj);
}

// static
template<class T>
bool PluginBase<T>::HasMethod(NPObject *npobj, NPIdentifier name) {
  const IDList &methods = GetMethodList();
  return methods.find(name) != methods.end();
}

// static
template<class T>
bool PluginBase<T>::Invoke(NPObject *npobj, NPIdentifier name,
                           const NPVariant *args, uint32_t num_args,
                           NPVariant *result) {
  ImplClass *gears = static_cast<PluginClass *>(npobj)->GetImplObject();

  const IDList &methods = GetMethodList();
  IDList::const_iterator method = methods.find(name);
  if (method == methods.end())
    return false;
  ImplCallback callback = method->second;

  BrowserUtils::EnterScope(npobj, static_cast<int>(num_args), args, result);
  (gears->*callback)();
  BrowserUtils::ExitScope();
  return true;
}

// static
template<class T>
bool PluginBase<T>::HasProperty(NPObject * npobj, NPIdentifier name) {
  const IDList &properties = GetPropertyList();
  return properties.find(name) != properties.end();
}

// static
template<class T>
bool PluginBase<T>::GetProperty(NPObject *npobj, NPIdentifier name,
                                NPVariant *result) {
  ImplClass *gears = static_cast<PluginClass *>(npobj)->GetImplObject();

  const IDList &properties = GetPropertyList();
  IDList::const_iterator property = properties.find(name);
  if (property == properties.end())
    return false;
  ImplCallback callback = property->second;

  BrowserUtils::EnterScope(npobj, 0, NULL, result);
  (gears->*callback)();
  BrowserUtils::ExitScope();
  return true;
}

// static
template<class T>
void PluginBase<T>::RegisterProperty(const char *name,
                                     ImplCallback callback) {
  NPIdentifier id = NPN_GetStringIdentifier(name);
  GetPropertyList()[id] = callback;
}

// static
template<class T>
void PluginBase<T>::RegisterMethod(const char *name,
                                   ImplCallback callback) {
  NPIdentifier id = NPN_GetStringIdentifier(name);
  GetMethodList()[id] = callback;
}