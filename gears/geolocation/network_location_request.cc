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

#ifdef OFFICIAL_BUILD
// The Geolocation API has not been finalized for official builds.
#else

#include "gears/geolocation/network_location_request.h"

#include "gears/blob/buffer_blob.h"
#include "gears/localserver/common/http_constants.h"
#include "third_party/jsoncpp/reader.h"
#include "third_party/jsoncpp/value.h"
#include "third_party/jsoncpp/writer.h"

static const char *kGearsNetworkLocationProtocolVersion = "1.0";

// Local functions
static const char16* RadioTypeToString(RadioType type);
// Adds a string if it's valid to the JSON object.
static void AddString(const std::string &property_name,
                      const std::string16 &value,
                      Json::Value *object);
// Adds an integer if it's valid to the JSON object.
static void AddInteger(const std::string &property_name,
                       const int &value,
                       Json::Value *object);
// Adds an angle as a double if it's valid to the JSON object. Returns true if
// added.
static bool AddAngle(const std::string &property_name,
                     const double &value,
                     Json::Value *object);
// Parses the server response body. Returns true if parsing was successful.
static bool ParseServerResponse(const std::string &response_body,
                                int64 timestamp,
                                Position *position);

// static
NetworkLocationRequest* NetworkLocationRequest::Create(
    const std::string16 &url,
    const std::string16 &host_name,
    ListenerInterface *listener) {
  return new NetworkLocationRequest(url, host_name, listener);
}

NetworkLocationRequest::NetworkLocationRequest(const std::string16 &url,
                                               const std::string16 &host_name,
                                               ListenerInterface *listener)
    : AsyncTask(NULL),
      listener_(listener),
      url_(url),
      host_name_(host_name) {
  if (!Init()) {
    assert(false);
  }
}

bool NetworkLocationRequest::MakeRequest(const RadioData &radio_data,
                                         const WifiData &wifi_data,
                                         bool request_address,
                                         std::string16 address_language,
                                         double latitude,
                                         double longitude,
                                         int64 timestamp) {
  if (!FormRequestBody(host_name_, radio_data, wifi_data, request_address,
                       address_language, latitude, longitude, &post_body_)) {
    return false;
  }
  timestamp_ = timestamp;
  // This will fail if there's a request currently in progress.
  return Start();
}

// AsyncTask implementation.

void NetworkLocationRequest::Run() {
  WebCacheDB::PayloadInfo payload;
  bool result = HttpPost(url_.c_str(),
                         false,            // Not capturing, so follow redirects
                         NULL,             // reason_header_value
                         NULL,             // mod_since_date
                         NULL,             // required_cookie
                         post_body_.get(),
                         &payload,
                         NULL,             // was_redirected
                         NULL,             // full_redirect_url
                         NULL);            // error_message

  MutexLock lock(&is_processing_response_mutex_);
  // is_aborted_ may be true even if HttpPost succeeded.
  if (is_aborted_) {
    LOG(("NetworkLocationRequest::Run() : HttpPost request was cancelled.\n"));
    return;
  }

  if (listener_) {
    Position position;
    // If HttpPost succeeded, payload.data is guaranteed to be non-NULL,
    // even if the vector it points to is empty.
    std::string response_body;
    if (result && !payload.data->empty()) {
      response_body.assign(
          reinterpret_cast<const char*>(&payload.data.get()[0]),
          payload.data->size());
    }
    GetLocationFromResponse(result, payload.status_code, response_body,
                            timestamp_, url_, &position);
    LOG(("NetworkLocationRequest::Run() : Calling listener with position.\n"));
    listener_->LocationResponseAvailable(position);
  }
}

// static
bool NetworkLocationRequest::FormRequestBody(const std::string16 &host_name,
    const RadioData &radio_data,
    const WifiData &wifi_data,
    bool request_address,
    std::string16 address_language,
    double latitude,
    double longitude,
    scoped_refptr<BlobInterface> *blob) {
  assert(blob);
  Json::Value body_object;
  assert(body_object.isObject());
  // Version and host are required.
  if (host_name.empty()) {
    return false;
  }
  body_object["version"] = Json::Value(kGearsNetworkLocationProtocolVersion);
  AddString("host", host_name, &body_object);

  AddInteger("home_mobile_country_code", radio_data.home_mobile_country_code,
             &body_object);
  AddInteger("home_mobile_network_code", radio_data.home_mobile_network_code,
             &body_object);
  AddString("radio_type", RadioTypeToString(radio_data.radio_type),
            &body_object);
  AddString("carrier", radio_data.carrier, &body_object);
  body_object["request_address"] = request_address;
  AddString("address_language", address_language, &body_object);

  Json::Value location;
  if (AddAngle("latitude", latitude, &location) &&
      AddAngle("longitude", longitude, &location)) {
    body_object["location"] = location;
  }

  Json::Value cell_towers;
  assert(cell_towers.isArray());
  int num_cell_towers = static_cast<int>(radio_data.cell_data.size());
  for (int i = 0; i < num_cell_towers; ++i) {
    Json::Value cell_tower;
    assert(cell_tower.isObject());
    AddInteger("cell_id", radio_data.cell_data[i].cell_id, &cell_tower);
    AddInteger("location_area_code", radio_data.cell_data[i].location_area_code,
               &cell_tower);
    AddInteger("mobile_country_code",
               radio_data.cell_data[i].mobile_country_code, &cell_tower);
    AddInteger("mobile_network_code",
               radio_data.cell_data[i].mobile_network_code, &cell_tower);
    AddInteger("age", radio_data.cell_data[i].age, &cell_tower);
    AddInteger("signal_strength", radio_data.cell_data[i].radio_signal_strength,
               &cell_tower);
    AddInteger("timing_advance", radio_data.cell_data[i].timing_advance,
               &cell_tower);
    cell_towers[i] = cell_tower;
  }
  if (num_cell_towers > 0) {
    body_object["cell_towers"] = cell_towers;
  }

  Json::Value wifi_towers;
  assert(wifi_towers.isArray());
  int num_wifi_towers = static_cast<int>(wifi_data.access_point_data.size());
  for (int i = 0; i < num_wifi_towers; ++i) {
    Json::Value wifi_tower;
    assert(wifi_tower.isObject());
    AddString("mac_address", wifi_data.access_point_data[i].mac_address,
              &wifi_tower);
    AddInteger("signal_strength",
               wifi_data.access_point_data[i].radio_signal_strength,
               &wifi_tower);
    AddInteger("age", wifi_data.access_point_data[i].age, &wifi_tower);
    AddInteger("channel", wifi_data.access_point_data[i].channel, &wifi_tower);
    AddInteger("signal_to_noise",
               wifi_data.access_point_data[i].signal_to_noise, &wifi_tower);
    AddString("ssid", wifi_data.access_point_data[i].ssid, &wifi_tower);
    wifi_towers[i] = wifi_tower;
  }
  if (num_wifi_towers > 0) {
    body_object["wifi_towers"] = wifi_towers;
  }

  Json::FastWriter writer;
  std::string body_string = writer.write(body_object);
  blob->reset(new BufferBlob(body_string.c_str(), body_string.size()));
  return true;
}

// static
void NetworkLocationRequest::GetLocationFromResponse(
    bool http_post_result,
    int status_code,
    const std::string &response_body,
    int64 timestamp,
    std::string16 server_url,
    Position *position) {
  assert(position);
  // HttpPost can fail for a number of reasons. Most likely this is because
  // we're offline, or there was no response.
  if (!http_post_result) {
    LOG(("NetworkLocationRequest::GetLocationFromResponse() : HttpPost request "
         "failed.\n"));
    position->error = STRING16(L"No response from network provider at ");
    position->error += server_url.c_str();
    position->error += STRING16(L".");
  } else if (status_code == HttpConstants::HTTP_OK) {
    // We use the timestamp from the device data that was used to generate
    // this position fix.
    if (ParseServerResponse(response_body, timestamp, position)) {
      // The response was successfully parsed, but it may not be a valid
      // position fix.
      if (!position->IsGoodFix()) {
        position->error = STRING16(L"Network provider at ");
        position->error += server_url.c_str();
        position->error += STRING16(L" did not provide a good position fix.");
      }
    } else {
      // We failed to parse the repsonse.
      LOG(("NetworkLocationRequest::GetLocationFromResponse() : Response "
           "malformed.\n"));
      position->error = STRING16(L"Response from network provider at ");
      position->error += server_url.c_str();
      position->error += STRING16(L" was malformed.");
    }
  } else {
    // The response was bad.
    LOG(("NetworkLocationRequest::GetLocationFromResponse() : HttpPost "
         "response was bad.\n"));
    position->error = STRING16(L"Network provider at ");
    position->error += server_url.c_str();
    position->error += STRING16(L" returned error code ");
    position->error += IntegerToString16(status_code);
    position->error += STRING16(L".");
  }
}

void NetworkLocationRequest::StopThreadAndDelete() {
  // The FF implementation of AsyncTask::Abort() delivers a message to the UI
  // thread to cancel the request. So if we call this method on the UI thread,
  // we must return to the OS before the call to Abort() will take effect. In
  // particular, we can't call Abort() then block here waiting for HttpPost to
  // return.
  is_processing_response_mutex_.Lock();
  Abort();
  is_processing_response_mutex_.Unlock();
  DeleteWhenDone();
}

// Local functions.

static const char16* RadioTypeToString(RadioType type) {
  switch (type) {
    case RADIO_TYPE_UNKNOWN:
      return STRING16(L"unknown");
    case RADIO_TYPE_GSM:
      return STRING16(L"gsm");
    case RADIO_TYPE_CDMA:
      return STRING16(L"cdma");
    case RADIO_TYPE_WCDMA:
      return STRING16(L"wcdma");
    default:
      assert(false);
  }
  return NULL;
}

static void AddString(const std::string &property_name,
                      const std::string16 &value,
                      Json::Value *object) {
  assert(object);
  assert(object->isObject());
  if (!value.empty()) {
    std::string value_utf8;
    if (String16ToUTF8(value.c_str(), value.size(), &value_utf8)) {
      (*object)[property_name] = Json::Value(value_utf8);
    }
  }
}

static void AddInteger(const std::string &property_name,
                       const int &value,
                       Json::Value *object) {
  assert(object);
  assert(object->isObject());
  if (kint32min != value) {
    (*object)[property_name] = Json::Value(value);
  }
}

static bool AddAngle(const std::string &property_name,
                     const double &value,
                     Json::Value *object) {
  assert(object);
  assert(object->isObject());
  if (value >= -180.0 && value <= 180.0) {
    (*object)[property_name] = Json::Value(value);
    return true;
  }
  return false;
}

// The JsValue::asXXX() methods return zero if a property isn't specified. For
// our purposes, zero is a valid value, so we have to test for existence.

// Gets an integer if it's present.
static bool GetAsInt(const Json::Value &object,
                     const std::string &property_name,
                     int *out) {
  assert(out);
  if (!object[property_name].isInt()) {
    return false;
  }
  *out = object[property_name].asInt();
  return true;
}

// Gets a string if it's present.
static bool GetAsString(const Json::Value &object,
                        const std::string &property_name,
                        std::string16 *out) {
  assert(out);
  if (!object[property_name].isString()) {
    return false;
  }
  std::string out_utf8 = object[property_name].asString();
  return UTF8ToString16(out_utf8.c_str(), out_utf8.size(), out);
}

static bool ParseServerResponse(const std::string &response_body,
                                int64 timestamp,
                                Position *position) {
  assert(position);
  if (response_body.empty()) {
    LOG(("ParseServerResponse() : Response was empty.\n"));
    return false;
  }
  // Parse the response, ignoring comments.
  Json::Reader reader;
  Json::Value response_object;
  if (!reader.parse(response_body, response_object, false)) {
    LOG(("ParseServerResponse() : Failed to parse response : %s.\n",
         reader.getFormatedErrorMessages().c_str()));
    return false;
  }
  assert(response_object.isObject());
  Json::Value location = response_object["location"];

  // If the network provider was unable to provide a position fix, it should
  // return a 200 with location == null.
  if (location.type() == Json::nullValue) {
    LOG(("ParseServerResponse() : Location is null.\n"));
    return true;
  }

  // If response is not null, it must be an object.
  if (!location.isObject()) {
    return false;
  }
  // latitude and longitude fields are required.
  if (!location["latitude"].isDouble() || !location["longitude"].isDouble()) {
    return false;
  }
  position->latitude = location["latitude"].asDouble();
  position->longitude = location["longitude"].asDouble();
  // Other fields are optional.
  GetAsInt(location, "altitude", &position->altitude);
  GetAsInt(location, "horizontal_accuracy", &position->horizontal_accuracy);
  GetAsInt(location, "vertical_accuracy", &position->vertical_accuracy);
  Json::Value address = location["address"];
  if (address.isObject()) {
    GetAsString(address, "street_number", &position->address.street_number);
    GetAsString(address, "street", &position->address.street);
    GetAsString(address, "premises", &position->address.premises);
    GetAsString(address, "city", &position->address.city);
    GetAsString(address, "county", &position->address.county);
    GetAsString(address, "region", &position->address.region);
    GetAsString(address, "country", &position->address.country);
    GetAsString(address, "country_code", &position->address.country_code);
    GetAsString(address, "postal_code", &position->address.postal_code);
  }

  position->timestamp = timestamp;
  return true;
}

#endif  // OFFICIAL_BUILD
