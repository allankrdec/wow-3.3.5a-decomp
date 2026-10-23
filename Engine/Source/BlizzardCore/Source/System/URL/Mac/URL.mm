// macOS counterpart of BlizzardCore/Source/System/URL/Win64/URL.cpp: asynchronous HTTP(S) downloads with
// NSURLSession instead of WinINet.
//
// DownloadURL returns as soon as the request is sent (false when the URL cannot be parsed or the request cannot be
// created, like the Win64 version) and the callback then sees, on a thread of the request's delegate queue, the
// same sequence as ReadResponseData/InternetCallback: ConnectedOK, GetHTTPStatusCode, and for a 2xx status
// GetLastModified, GetContentLength, WantData and ReceivedData (at most 16 KB at a time, polling Stop after each
// chunk); finally Completed(status) exactly once, after which neither the request nor the callback is touched.
// The request is POST with GetPostData, else GET, else HEAD when WantData is false; it bypasses caches and sends
// If-Modified-Since, Range and the cookie of the callback. The connect and receive timeouts are GetTimeout().
//
// Differences from WinINet: the cookie is sent as a Cookie header of this request instead of being stored in the
// WinINet cookie jar for the URL; an open-ended range (m_end == -1) is sent as "bytes=start-" (Win64 formats
// "bytes=start--1"); a transfer that fails after the response started reports the error (DOWNLOAD_FAILED or
// DOWNLOAD_TIMEOUT; Win64 stops reading and reports DOWNLOAD_SUCCESS); Content-Length is passed as 64 bits (Win64 truncates to 32).

#import <Foundation/Foundation.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/BlizzardCore/Source/System/URL/URL.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"

// The delegate of one request's NSURLSession (m_connect of the HttpRequest).
@interface BlizzardURLDelegate : NSObject <NSURLSessionDataDelegate> {
    @public
    Blizzard::URL::HttpRequest* m_httpRequest;
}
@end

static Blizzard::URL::DownloadCallback::Status StatusFromHttpCode(NSInteger statusCode) {
    switch (statusCode) {
        case 304:
            return Blizzard::URL::DownloadCallback::DOWNLOAD_NOT_MODIFIED;
        case 400:
            return Blizzard::URL::DownloadCallback::DOWNLOAD_BAD_REQUEST;
        case 403:
            return Blizzard::URL::DownloadCallback::DOWNLOAD_FORBIDDEN;
        case 404:
            return Blizzard::URL::DownloadCallback::DOWNLOAD_NOT_FOUND;
        default:
            return Blizzard::URL::DownloadCallback::DOWNLOAD_FAILED;
    }
}

// The ERROR_INTERNET_* mapping of ReadResponseData.
static Blizzard::URL::DownloadCallback::Status StatusFromError(NSError* error) {
    if (![[error domain] isEqualToString:NSURLErrorDomain]) {
        return Blizzard::URL::DownloadCallback::DOWNLOAD_FAILED;
    }

    switch ([error code]) {
        case NSURLErrorTimedOut:
            return Blizzard::URL::DownloadCallback::DOWNLOAD_TIMEOUT;
        case NSURLErrorCannotFindHost:
        case NSURLErrorDNSLookupFailed:
            return Blizzard::URL::DownloadCallback::DOWNLOAD_NAME_NOT_RESOLVED;
        case NSURLErrorCannotConnectToHost:
        case NSURLErrorNotConnectedToInternet:
            return Blizzard::URL::DownloadCallback::DOWNLOAD_CANNOT_CONNECT;
        default:
            return Blizzard::URL::DownloadCallback::DOWNLOAD_FAILED;
    }
}

@implementation BlizzardURLDelegate

// Win64: 0x0042CA00
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveResponse:(NSURLResponse*)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    Blizzard::URL::HttpRequest* request = m_httpRequest;

    if (!request->m_connected) {
        request->m_callback->ConnectedOK();
        request->m_connected = true;
    }

    request->m_state = 2;

    NSInteger statusCode = [response isKindOfClass:[NSHTTPURLResponse class]] ? [static_cast<NSHTTPURLResponse*>(response) statusCode] : 0;
    request->m_callback->GetHTTPStatusCode(static_cast<int32_t>(statusCode));

    if (statusCode < 200 || statusCode >= 300) {
        request->m_status = StatusFromHttpCode(statusCode);
        request->m_done = true;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }

    NSDictionary* headers = [static_cast<NSHTTPURLResponse*>(response) allHeaderFields];
    NSString* lastModified = [headers objectForKey:@"Last-Modified"];

    if (lastModified) {
        request->m_callback->GetLastModified([lastModified UTF8String]);
    }

    NSString* contentLength = [headers objectForKey:@"Content-Length"];

    if (contentLength) {
        request->m_callback->GetContentLength(strtoll([contentLength UTF8String], nullptr, 10));
    }

    if (!request->m_callback->WantData()) {
        request->m_done = true;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }

    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
    Blizzard::URL::HttpRequest* request = m_httpRequest;

    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange byteRange, BOOL* stop) {
        const uint8_t* chunk = static_cast<const uint8_t*>(bytes);
        NSUInteger remaining = byteRange.length;

        // The 16 KB reads of InternetReadFileEx.
        while (remaining && !request->m_done) {
            int32_t size = remaining > 16384 ? 16384 : static_cast<int32_t>(remaining);

            if (!request->m_callback->ReceivedData(chunk, size)) {
                request->m_status = Blizzard::URL::DownloadCallback::DOWNLOAD_ABORTED;
                request->m_done = true;
            }

            if (request->m_callback->Stop()) {
                request->m_status = Blizzard::URL::DownloadCallback::DOWNLOAD_STOPPED;
                request->m_done = true;
            }

            chunk += size;
            remaining -= size;
        }

        if (request->m_done) {
            *stop = YES;
        }
    }];

    if (request->m_done) {
        [dataTask cancel];
    }
}

// The INTERNET_STATUS_HANDLE_CLOSING case of InternetCallback.
// Win64: 0x00451290
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    Blizzard::URL::HttpRequest* request = m_httpRequest;
    m_httpRequest = nullptr;

    if (error && !request->m_done) {
        request->m_status = StatusFromError(error);
    }

    Blizzard::URL::DownloadCallback* callback = request->m_callback;
    Blizzard::URL::DownloadCallback::Status status = request->m_status;

    // The session keeps its delegate until it is invalidated.
    [session finishTasksAndInvalidate];
    delete request;

    callback->Completed(status);
}

@end

// Win64: 0x0042CD80
Blizzard::URL::HttpRequest::~HttpRequest() {
    [static_cast<NSURLSessionDataTask*>(this->m_request) release];
    [static_cast<NSURLSession*>(this->m_session) release];
    [static_cast<BlizzardURLDelegate*>(this->m_connect) release];
}

// Win64: 0x00437840
Blizzard::URL::HttpRequest::HttpRequest() {
    this->m_connectEvent = nullptr;
    this->m_requestEvent = nullptr;
    this->m_completeEvent = nullptr;
    this->m_dataEvent = nullptr;
    this->m_session = nullptr;
    this->m_connect = nullptr;
    this->m_request = nullptr;
    this->m_state = 0;
    this->m_done = false;
    this->m_connected = false;
    this->m_callback = nullptr;
    this->m_error = 0;
    this->m_status = Blizzard::URL::DownloadCallback::DOWNLOAD_SUCCESS;
}

// Win64: 0x00451470
bool Blizzard::URL::DownloadURL(const char* url, DownloadCallback* callback) {
    @autoreleasepool {
        // InternetCrackUrlA.
        NSString* urlString = url ? [NSString stringWithUTF8String:url] : nil;
        NSURL* nsurl = urlString ? [NSURL URLWithString:urlString] : nil;
        NSString* scheme = [[nsurl scheme] lowercaseString];

        if (!nsurl || ![nsurl host] || !([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"])) {
            return false;
        }

        HttpRequest* request = new HttpRequest();
        request->m_callback = callback;

        int32_t timeout = callback->GetTimeout();
        NSTimeInterval timeoutSeconds = timeout > 0 ? timeout / 1000.0 : 60.0;

        NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:nsurl cachePolicy:NSURLRequestReloadIgnoringLocalAndRemoteCacheData timeoutInterval:timeoutSeconds];

        const char* verb;

        if (callback->GetPostData()) {
            verb = "POST";
        } else if (callback->WantData()) {
            verb = "GET";
        } else {
            verb = "HEAD";
        }

        [urlRequest setHTTPMethod:[NSString stringWithUTF8String:verb]];
        [urlRequest setValue:@"Blizzard Web Client" forHTTPHeaderField:@"User-Agent"];

        const char* postData = callback->GetPostData();

        if (postData) {
            const char* contentType = callback->GetPostContentType();
            [urlRequest setValue:[NSString stringWithUTF8String:contentType ? contentType : "text/html"] forHTTPHeaderField:@"Content-Type"];
            [urlRequest setHTTPBody:[NSData dataWithBytes:postData length:Blizzard::String::Length(postData)]];
        }

        const char* ifModifiedSince = callback->WantIfModifiedSince();

        if (ifModifiedSince) {
            [urlRequest setValue:[NSString stringWithUTF8String:ifModifiedSince] forHTTPHeaderField:@"If-Modified-Since"];
        }

        DownloadCallback::ByteRange range = callback->GetByteRange();

        if (range.m_start != -1 || range.m_end != -1) {
            // snprintf: Blizzard::String::Format has no %lld.
            char header[128];

            if (range.m_end == -1) {
                snprintf(header, sizeof(header), "bytes=%lld-", static_cast<long long>(range.m_start));
            } else {
                snprintf(header, sizeof(header), "bytes=%lld-%lld", static_cast<long long>(range.m_start == -1 ? 0 : range.m_start), static_cast<long long>(range.m_end));
            }

            [urlRequest setValue:[NSString stringWithUTF8String:header] forHTTPHeaderField:@"Range"];
        }

        DownloadCallback::Cookie cookie = callback->GetCookie();

        if (cookie.m_name && *cookie.m_name && cookie.m_value) {
            NSString* value = [NSString stringWithFormat:@"%s=%s", cookie.m_name, cookie.m_value];
            [urlRequest setValue:value forHTTPHeaderField:@"Cookie"];
        }

        // One session per request: callbacks may block (Win64 runs them on a pool thread), and a blocked serial
        // delegate queue only holds back its own request.
        NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        [configuration setURLCache:nil];
        [configuration setRequestCachePolicy:NSURLRequestReloadIgnoringLocalAndRemoteCacheData];
        [configuration setHTTPShouldSetCookies:NO];
        [configuration setTimeoutIntervalForRequest:timeoutSeconds];

        NSOperationQueue* queue = [[[NSOperationQueue alloc] init] autorelease];
        [queue setMaxConcurrentOperationCount:1];
        [queue setName:@"Blizzard::URL::DownloadURL"];

        BlizzardURLDelegate* delegate = [[BlizzardURLDelegate alloc] init];
        delegate->m_httpRequest = request;
        request->m_connect = delegate;

        NSURLSession* session = [[NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:queue] retain];
        request->m_session = session;

        NSURLSessionDataTask* task = [[session dataTaskWithRequest:urlRequest] retain];

        if (!task) {
            [session invalidateAndCancel];
            delete request;
            return false;
        }

        request->m_request = task;
        request->m_state = 1;
        [task resume];

        return true;
    }
}
