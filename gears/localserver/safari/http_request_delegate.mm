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

#include "gears/base/common/http_utils.h"
#include "gears/base/safari/cf_string_utils.h"
#ifdef OFFICIAL_BUILD
// Blobs are not yet supported within official builds.
#else  // OFFICIAL_BUILD
#include "gears/blob/blob_input_stream_sf.h"
#include "gears/blob/blob_interface.h"
#endif  // OFFICIAL_BUILD
#include "gears/localserver/common/http_request.h"
#import "gears/localserver/safari/http_request_delegate.h"

@implementation HttpRequestDelegate

#pragma mark Public Instance methods

- (id)initWithOwner:(SFHttpRequest *)owner {
  self = [super init];
  if (self) {
    owner_ = owner;
    dataEncoding_ = kCFStringEncodingUTF8;
  }
  return self;
}

- (void)dealloc {
  [request_ release];
  [connection_ release];
  [receivedData_ release];
  [headerDictionary_ release];
  [mimeType_ release];
  
  [super dealloc];
}

- (bool)open:(const std::string16 &)full_url
      method:(const std::string16 &)method {
  // It is illegal to reuse this object for multiple connections.
  assert(!request_ && !connection_);

  NSString *url_str = [NSString stringWithString16:full_url.c_str()];
  if (!url_str) {
    return false;
  }
  
  request_ = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url_str]];
  if (!request_) {
    return false;
  }
  [request_ retain];

  // Set the request method.
  [request_ setHTTPMethod:[NSString stringWithString16:method.c_str()]];
  return true;
}

- (bool)send:(NSInputStream *)post_data_stream
   userAgent:(const std::string16 &)user_agent
     headers:(const SFHttpRequest::HttpHeaderVector &)headers
     bypassBrowserCache:(bool)bypass_browser_cache {
  assert(request_);
  assert(connection_ == nil);  // It's illegal to call send more than once.
  
  if (bypass_browser_cache) {
    [request_ setCachePolicy:NSURLRequestReloadIgnoringCacheData];
  } else {
    [request_ setCachePolicy:NSURLRequestUseProtocolCachePolicy];
  }
  
  // Set the user agent header.
  NSString *user_agent_header = [NSString 
                                     stringWithCString:HTTPHeaders::USER_AGENT
                                              encoding:NSUTF8StringEncoding];
  [request_ setValue:[NSString stringWithString16:user_agent.c_str()] 
            forHTTPHeaderField:user_agent_header];
              
  // Add remaining headers.
  for (SFHttpRequest::HttpHeaderVectorConstIterator it = headers.begin();
       it != headers.end();
       ++it) {
    NSString *header_name = [NSString stringWithString16:it->first.c_str()];
    NSString *header_value = [NSString stringWithString16:it->second.c_str()];
    [request_ setValue:header_value forHTTPHeaderField:header_name];
  }
  
  if (post_data_stream) {
    [request_ setHTTPBodyStream: post_data_stream];
  }
  
  [connection_ release];  // Defensive coding: stop potential memory leak in the
                          // unlikely case that we allow multiple calls of send:
                          // per instance.
  connection_ = [[NSURLConnection alloc] initWithRequest:request_ 
                                                delegate:self];
  [request_ release];
  request_ = nil;
                         
  if (connection_) {
    receivedData_ = [[NSMutableData data] retain];
    return true;
  }
  return false;
}

- (void)abort {
  [connection_ cancel];
}

#pragma mark NSURLConnection delegate methods

// This method is called when the connection has received the mimetype and
// content length for the request.
// In some cases (e.g. loading content who's mimetype is 
// multipart/x-mixed-replace) this method may be called multiple times.
- (void)connection:(NSURLConnection *)connection 
  didReceiveResponse:(NSURLResponse *)response {  
  [receivedData_ setLength:0];
  
  // Save string encoding, for use in GetResponseAsString.
  const NSString *encoding_str = [response textEncodingName];
  if (encoding_str) {
    dataEncoding_ = CFStringConvertIANACharSetNameToEncoding(
                         reinterpret_cast<CFStringRef>(encoding_str));
  }
  
  // Squirrel away http headers, response code and mimeType.
  NSHTTPURLResponse *http_response = static_cast<NSHTTPURLResponse *>(response);
  NSDictionary *all_headers = [[http_response allHeaderFields] retain];
  [headerDictionary_ autorelease];
  headerDictionary_ = all_headers;
  statusCode_ = (NSInteger)[(NSHTTPURLResponse *)response statusCode];
  [mimeType_ autorelease];
  mimeType_ = [[response MIMEType] retain];
}

// Called when connection receives data.
- (void)connection:(NSURLConnection *)connection didReceiveData:(NSData *)data {
  [receivedData_ appendData:data];
  
  // Owner should set the ready state to interactive.
  owner_->OnDataAvailable();
}


- (void)connection:(NSURLConnection *)connection
  didFailWithError:(NSError *)error {
  owner_->Abort();
}

- (void)connectionDidFinishLoading:(NSURLConnection *)connection {
  owner_->SetReadyState(HttpRequest::COMPLETE);
}

// Delegate method called on redirects and URL canonicalization.
- (NSURLRequest *)connection:(NSURLConnection *)connection
                 willSendRequest:(NSURLRequest *)request
                 redirectResponse:(NSURLResponse *)redirect_response {
  // if redirect_response is nil then we were called as the result of 
  // transforming a request’s URL to its canonical form which is an event that 
  // doesn't interest us.
  if (!redirect_response) {
    return request;
  }

  NSString *redirect_to_full_url_nsstr = [[request URL] absoluteString];
  std::string16 redirect_to_full_url;
  if (![redirect_to_full_url_nsstr string16:&redirect_to_full_url]) {
    [self abort];
    owner_->SetReadyState(HttpRequest::COMPLETE);
    return nil;
  }
  
  // Abort redirect if it's not legal.
  if (!owner_->AllowRedirect(redirect_to_full_url)) {
    [self abort];
    
    // Save headers and status code from redirect
    NSHTTPURLResponse *http_response = 
                          static_cast<NSHTTPURLResponse *>(redirect_response);
    NSDictionary *all_headers = [[http_response allHeaderFields] retain];
    [headerDictionary_ autorelease];
    headerDictionary_ = all_headers;
    statusCode_ = [http_response statusCode];

    owner_->SetReadyState(HttpRequest::COMPLETE);
    return nil;
  }

  return request;
}

#pragma mark Public Instance methods -- Access Methods

- (void)headers:(SFHttpRequest::HttpHeaderVector *)headers {
  assert(headerDictionary_);
  assert(headers);
  
  NSEnumerator *enumerator = [headerDictionary_ keyEnumerator];
  while (NSString *header_name = [enumerator nextObject]) {
    NSString *header_value = [headerDictionary_ objectForKey:header_name];
    
    std::string16 header_name_str16;
    std::string16 header_value_str16;
    [header_name string16:&header_name_str16];
    [header_value string16:&header_value_str16];
    
    headers->push_back(SFHttpRequest::HttpHeader(
                                        header_name_str16, header_value_str16));
  }
  
}

- (void)headerByName:(const std::string16&)name 
               value:(std::string16 *)value {
  assert(value);
  
  NSString *header_to_find = [NSString stringWithString16:name.c_str()];
  NSString *header_value = nil;
  
  // HTTP Header fields are case insensitive and NSURLConnection has the
  // annoying habit of capitalizing the first letter of every incoming http
  // header name so we need to do a case insensitive search here.
  NSEnumerator *enumerator = [headerDictionary_ keyEnumerator];
  while (NSString *header_name = [enumerator nextObject]) {
    if ([header_name compare:header_to_find options:NSCaseInsensitiveSearch] == 
        NSOrderedSame) {
      header_value = [headerDictionary_ objectForKey:header_name];
      [header_value string16:value];
      return;
    }
  }
  
  // If the requested header doesn't exist, just clear the output value.
  value->clear();
}

- (void)responseBytes:(std::vector<uint8> *)body {
  assert(body);
  assert(receivedData_);
  
  size_t body_len = [receivedData_ length];
  
  body->resize(body_len);
  
  if (body_len) {
    [receivedData_ getBytes:&(*body)[0] length:body_len];
  }
}

- (bool)responseAsString:(std::string16 *)response {
  assert(response);
  assert(receivedData_);

  size_t length = [receivedData_ length];
  if (length > 0) {
    return ConvertToString16UsingEncoding(
               static_cast<const char *>([receivedData_ bytes]), 
               length, 
               dataEncoding_, 
               response);
  } else {
    response->clear();
    return true;
  }
}

- (int)statusCode {
  return statusCode_;
}

- (void)mimeType:(std::string16 *)mime_type_str {
  assert(mime_type_str);
  
  if (mimeType_) {
    [mimeType_ string16:mime_type_str];
  } else {
    *mime_type_str = STRING16(L"");
  }
}

- (void)statusText:(std::string16 *)status_line {
  assert(status_line);
  
  NSString *status_line_nsstr = [NSHTTPURLResponse 
                                    localizedStringForStatusCode:statusCode_];
  [status_line_nsstr string16:status_line];
}
@end
