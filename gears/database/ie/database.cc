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

#include <shlobj.h> // for SID_SWebBrowserApp
#include "gears/third_party/sqlite_google/preprocessed/sqlite3.h"

#include "gears/base/common/paths.h"
#include "gears/base/common/security_model.h"
#include "gears/base/common/sqlite_wrapper.h"
#include "gears/base/common/string_utils.h"
#include "gears/base/common/timer.h"
#include "gears/base/ie/activex_utils.h"
#include "gears/base/ie/atl_headers.h"
#include "gears/database/common/database_utils.h"
#include "gears/database/ie/database.h"
#include "gears/database/ie/result_set.h"

#ifdef DEBUG
Timer GearsDatabase::g_timer_;
#endif // DEBUG

GearsDatabase::GearsDatabase() : db_(NULL) {}

GearsDatabase::~GearsDatabase() {
  if (db_ != NULL) {
    ATLTRACE(_T("~GearsDatabase - client did not call Close() \n"));
    sqlite3_close(db_);
    db_ = NULL;
  }
}

HRESULT GearsDatabase::open(const VARIANT *database_name) {
  if (db_) {
    RETURN_EXCEPTION(STRING16(L"A database is already open."));
  }

  // Get the database_name arg (if caller passed it in).
  CComBSTR database_name_bstr(L"");
  if (ActiveXUtils::OptionalVariantIsPresent(database_name)) {
    if (database_name->vt != VT_BSTR) {
      RETURN_EXCEPTION(STRING16(L"Database name must be a string."));
    }
    database_name_bstr = database_name->bstrVal;
  }

  ATLTRACE(_T("GearsDatabase::open(%s)\n"), database_name_bstr);

  // For now, callers cannot open DBs in other security origins.
  // To support that, parse an 'origin' argument here and call
  // IsOriginAccessAllowed (yet to be written).

  // Open the database.
  if (!OpenSqliteDatabase(database_name_bstr, EnvPageSecurityOrigin(),
                          &db_)) {
    RETURN_EXCEPTION(STRING16(L"Couldn't open SQLite database."));
  }
  const int kSQLiteBusyTimeout = 5000;
  sqlite3_busy_timeout(db_, kSQLiteBusyTimeout);

  RETURN_NORMAL();
}

// private method, so don't use RETURN_ macros
HRESULT GearsDatabase::BindArgsToStatement(const VARIANT *arg_array,
                                           sqlite3_stmt *stmt) {
  _ASSERTE(stmt != NULL);

  int num_args_expected = sqlite3_bind_parameter_count(stmt);
  LONG num_args = 0;
  CComVariant args_safearray;
  HRESULT hr;

  if (ActiveXUtils::OptionalVariantIsPresent(arg_array)) {
    HRESULT hr = ActiveXUtils::ConvertJsArrayToSafeArray(
        arg_array, &args_safearray, &num_args);
    if (FAILED(hr)) {
      RETURN_EXCEPTION(STRING16(L"Invalid SQL parameters array."));
    }
  }

  // check that the correct number of SQL arguments were passed
  if (num_args_expected != num_args) {
    RETURN_EXCEPTION(STRING16(L"Wrong number of SQL parameters"));
  }

  // Bind each arg to its sql param
  for (LONG i = 0; i < num_args_expected; i++) {
    CComVariant arg;
    hr = SafeArrayGetElement(args_safearray.parray, &i, &arg);
    if (FAILED(hr)) {
      return hr;
    }
    hr = BindArg(arg, i, stmt);
    if (FAILED(hr)) {
      return hr;
    }
  }

  return S_OK;
}

HRESULT GearsDatabase::BindArg(const CComVariant &arg, int index,
                               sqlite3_stmt *stmt) {
  _ASSERTE(stmt != NULL);

  int err = SQLITE_OK;

  // TODO(): perhaps add cases for numeric types rather than using
  // string conversion so sqlite is aware of the actual types being
  // bound to parameters.
  switch (arg.vt) {
    case VT_EMPTY:
      // TODO(miket): We'd like to come up with a more principled approach
      // to undefined and missing parameters. For now, this is consistent
      // with the Firefox implementation.
      err = sqlite3_bind_text16(stmt, index + 1,
                                L"undefined", -1,
                                SQLITE_TRANSIENT);
      ATLTRACE(L"  Parameter: [VT_EMPTY]\n");
      break;

    case VT_NULL:
      err = sqlite3_bind_null(stmt, index + 1);
      ATLTRACE(L"  Parameter: [VT_NULL]\n");
      break;

    case VT_BSTR:
      // A null bstr value means empty string
      err = sqlite3_bind_text16(stmt, index + 1,
                                arg.bstrVal ? arg.bstrVal : L"", -1,
                                SQLITE_TRANSIENT);
      ATLTRACE(L"  Parameter: [VT_BSTR] %s\n", arg.bstrVal ? arg.bstrVal : L"");
      break;

    default:
      // Convert to a string representation if we need to
      CComVariant arg_copy(arg);
      if (FAILED(arg_copy.ChangeType(VT_BSTR))) {
        ATLTRACE(_T("CComVariant::ChangeType failed\n"));
        return E_INVALIDARG;
      }
      // A null bstr value means empty string
      err = sqlite3_bind_text16(stmt, index + 1,
                                arg_copy.bstrVal ? arg_copy.bstrVal : L"", -1,
                                SQLITE_TRANSIENT);
      ATLTRACE(L"  Parameter: [other] %s\n", arg_copy.bstrVal ? arg_copy.bstrVal
                                             : L"");
      break;
  }

  return (err == SQLITE_OK) ? S_OK :  E_FAIL;
}

STDMETHODIMP GearsDatabase::execute(const BSTR expression_in, 
                                    const VARIANT *arg_array,
                                    GearsResultSetInterface **rs_retval) {
  const BSTR expression = ActiveXUtils::SafeBSTR(expression_in);

#ifdef DEBUG
  ScopedTimer scoped_timer(&GearsDatabase::g_timer_);
#endif // DEBUG

  ATLTRACE(_T("GearsDatabase::execute(%s)\n"), expression);

  HRESULT hr;
  *rs_retval = NULL;  // set retval in case we exit early

  if (!db_) {
    RETURN_EXCEPTION(STRING16(L"Database handle was NULL."));
  }

  // Prepare a statement for execution.

  scoped_sqlite3_stmt_ptr stmt;
  int sql_status = sqlite3_prepare16_v2(db_, expression, -1, &stmt, NULL);
  if ((sql_status != SQLITE_OK) || (stmt.get() == NULL)) {
    std::string16 msg;
    BuildSqliteErrorString(STRING16(L"SQLite prepare() failed."),
                           sql_status, db_, &msg);
    msg += STRING16(L" EXPRESSION: ");
    msg += expression;
    RETURN_EXCEPTION(msg.c_str());
  }

  // Bind parameters

  hr = BindArgsToStatement(arg_array, stmt.get());
  if (FAILED(hr)) {
    // BindArgsToStatement already called RETURN_EXCEPTION
    return hr;
  }

  // We go through this manual COM stuff because SetStatement() is
  // not part of the public GearsResultSetInterface, so this is the
  // right way to get a pointer to the actual object (not its
  // interface), call the object method, and then grab the interface
  // from it.
  CComObject<GearsResultSet> *rs_internal = NULL;
  hr = CComObject<GearsResultSet>::CreateInstance(&rs_internal);
  if (FAILED(hr)) {
    RETURN_EXCEPTION(STRING16(L"Could not create ResultSet."));
  }

  if (!rs_internal->InitBaseFromSibling(this)) {
    RETURN_EXCEPTION(STRING16(L"Initializing base class failed."));
  }

  // Note the ResultSet takes ownership of the statement
  std::string16 error_message;
  if (!rs_internal->SetStatement(stmt.release(), &error_message)) {
    ATLTRACE(error_message.c_str());
    RETURN_EXCEPTION(error_message.c_str());
  }

  // TODO(cprince): track the open ResultSet objects, so we can
  // auto-close them when the Database is closed.
  hr = rs_internal->QueryInterface(rs_retval);
  if (FAILED(hr)) {
    RETURN_EXCEPTION(STRING16(L"Could not get GearsResultSet interface."));
  }

  assert((*rs_retval)->AddRef() == 2 &&
         (*rs_retval)->Release() == 1); // CComObject* does not Release
  RETURN_NORMAL();
}

STDMETHODIMP GearsDatabase::close() {
  ATLTRACE(_T("GearsDatabase::close()\n"));
  if (db_ != NULL) {
    int sql_status = sqlite3_close(db_);
    db_ = NULL;
    if (sql_status != SQLITE_OK) {
      RETURN_EXCEPTION(STRING16(L"SQLite close() failed."));
    }
  }
  RETURN_NORMAL();
}

STDMETHODIMP GearsDatabase::get_lastInsertRowId(VARIANT *retval) {
  ATLTRACE(_T("GearsDatabase::lastInsertRowId()\n"));
  if (db_ != NULL) {
    VariantClear(retval);
    retval->vt = VT_I8;
    retval->llVal = sqlite3_last_insert_rowid(db_);
    if (FAILED(VariantChangeType(retval, retval, 0, VT_DECIMAL))) {
      RETURN_EXCEPTION(STRING16(L"Converting int64 to VT_DECIMAL failed."));
    }
    RETURN_NORMAL();
  } else {
    RETURN_EXCEPTION(STRING16(L"Database handle was NULL."));
  }
}

#ifdef DEBUG
STDMETHODIMP GearsDatabase::get_executeMsec(int *retval) {
  ATLTRACE(_T("GearsDatabase::executeMSec()\n"));
  *retval = GearsDatabase::g_timer_.GetElapsed();
  RETURN_NORMAL();
}
#endif
