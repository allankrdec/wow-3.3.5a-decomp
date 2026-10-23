// macOS counterpart of Os/Win64/OsURLDownload.cpp: the glue's small HTTP GETs (news, patch info) with NSURLSession
// instead of WinINet.
//
// DownloadString returns true once the request is sent. The callback then runs on a thread of the download's
// delegate queue, as in OsUrlDownloadCallback/fnInternetCallback: for a 2xx status, callback(param, data, size,
// false, NoError) for each block of at most 16 KB, then once with size 0 at the end of the data (a false return
// stops the download); finally callback(param, nullptr, 0, true, error) after the download object is freed.
// error is Timeout or Failed when the request fails, NotFound for 404 and Failed for the other statuses.
// The timeout (ms; 0 or less: the 60 s default) applies to connecting and to every receive.
//
// Difference from WinINet: a transfer that fails after the response started ends with Timeout/Failed and no
// size-0 data call (Win64 stops reading and reports NoError).

#import <Foundation/Foundation.h>
#include <cstdint>
#include <cstring>
#include "Engine/Source/Os/OsURLDownload.h"
#include "Engine/Source/Storm/H/storm.h"

// The delegate of one download's NSURLSession (m_connect of the OsUrlDownload).
@interface OsUrlDownloadDelegate : NSObject <NSURLSessionDataDelegate> {
    @public
    OsUrlDownload* m_download;
}
@end

@implementation OsUrlDownloadDelegate

// Win64: 0x0086F960
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveResponse:(NSURLResponse*)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    OsUrlDownload* download = m_download;
    download->m_state = 2;
    download->m_statusCode = [response isKindOfClass:[NSHTTPURLResponse class]] ? static_cast<uint32_t>([static_cast<NSHTTPURLResponse*>(response) statusCode]) : 0;

    if (download->m_statusCode >= 200 && download->m_statusCode < 300) {
        completionHandler(NSURLSessionResponseAllow);
        return;
    }

    download->m_errorCode = download->m_statusCode == 404 ? HTTPError::NotFound : HTTPError::Failed;
    download->m_done = true;
    completionHandler(NSURLSessionResponseCancel);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
    OsUrlDownload* download = m_download;

    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange byteRange, BOOL* stop) {
        const char* chunk = static_cast<const char*>(bytes);
        NSUInteger remaining = byteRange.length;

        // The 16 KB reads of InternetReadFileEx.
        while (remaining && !download->m_done) {
            uint32_t size = remaining > 16384 ? 16384 : static_cast<uint32_t>(remaining);

            if (download->m_callback && !download->m_callback(download->m_param, chunk, size, false, HTTPError::NoError)) {
                download->m_done = true;
            }

            chunk += size;
            remaining -= size;
        }

        if (download->m_done) {
            *stop = YES;
        }
    }];

    if (download->m_done) {
        [dataTask cancel];
    }
}

// The INTERNET_STATUS_HANDLE_CLOSING case of fnInternetCallback.
// Win64: 0x0086FC80
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    OsUrlDownload* download = m_download;
    m_download = nullptr;

    if (!download->m_done) {
        if (error) {
            bool timeout = [[error domain] isEqualToString:NSURLErrorDomain] && [error code] == NSURLErrorTimedOut;
            download->m_errorCode = timeout ? HTTPError::Timeout : HTTPError::Failed;
        } else if (download->m_callback) {
            // The last, empty read.
            char empty[1] = { 0 };
            download->m_callback(download->m_param, empty, 0, false, HTTPError::NoError);
        }
    }

    OsURLDownloadCallback callback = download->m_callback;
    void* param = download->m_param;
    HTTPError::ErrorCode errorCode = download->m_errorCode;

    [static_cast<NSURLSessionDataTask*>(download->m_request) release];
    [static_cast<NSURLSession*>(download->m_internet) release];
    [static_cast<OsUrlDownloadDelegate*>(download->m_connect) release];

    // The session keeps its delegate until it is invalidated.
    [session finishTasksAndInvalidate];
    delete download;

    if (callback) {
        callback(param, nullptr, 0, true, errorCode);
    }
}

@end

// INLINED
OsUrlDownload::OsUrlDownload() {
    this->m_internet = nullptr;
    this->m_connect = nullptr;
    this->m_request = nullptr;
    this->m_connectEvent = nullptr;
    this->m_requestEvent = nullptr;
    this->m_completeEvent = nullptr;
    this->m_dataEvent = nullptr;
    this->m_state = 0;
    this->m_done = false;
    this->m_statusCode = 0;
    this->m_callback = nullptr;
    this->m_error = 0;
    this->m_errorCode = HTTPError::NoError;
}

// Win64: 0x0086FE40
bool OsUrlDownload::DownloadString(const char* url, OsURLDownloadCallback callback, void* param, int32_t timeout) {
    @autoreleasepool {
        // InternetCrackUrlA.
        NSString* urlString = url ? [NSString stringWithUTF8String:url] : nil;
        NSURL* nsurl = urlString ? [NSURL URLWithString:urlString] : nil;
        NSString* scheme = [[nsurl scheme] lowercaseString];

        if (!nsurl || ![nsurl host] || !([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"])) {
            return false;
        }

        OsUrlDownload* download = new (__FILE__, __LINE__) OsUrlDownload();
        download->m_callback = callback;
        download->m_param = param;

        NSTimeInterval timeoutSeconds = timeout > 0 ? timeout / 1000.0 : 60.0;

        NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:nsurl cachePolicy:NSURLRequestReloadIgnoringLocalAndRemoteCacheData timeoutInterval:timeoutSeconds];
        [urlRequest setHTTPMethod:@"GET"];
        [urlRequest setValue:@"Blizzard Web Client" forHTTPHeaderField:@"User-Agent"];

        NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        [configuration setURLCache:nil];
        [configuration setRequestCachePolicy:NSURLRequestReloadIgnoringLocalAndRemoteCacheData];
        [configuration setTimeoutIntervalForRequest:timeoutSeconds];

        NSOperationQueue* queue = [[[NSOperationQueue alloc] init] autorelease];
        [queue setMaxConcurrentOperationCount:1];
        [queue setName:@"OsUrlDownload"];

        OsUrlDownloadDelegate* delegate = [[OsUrlDownloadDelegate alloc] init];
        delegate->m_download = download;
        download->m_connect = delegate;

        NSURLSession* session = [[NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:queue] retain];
        download->m_internet = session;

        NSURLSessionDataTask* task = [[session dataTaskWithRequest:urlRequest] retain];

        if (!task) {
            [session invalidateAndCancel];
            [session release];
            [delegate release];
            delete download;
            return false;
        }

        download->m_request = task;
        download->m_state = 1;
        [task resume];

        return true;
    }
}

// Win64: 0x00870040
bool OsURLDownload(const char* url, OsURLDownloadCallback callback, void* param) {
    return OsUrlDownload::DownloadString(url, callback, param, 5000);
}
