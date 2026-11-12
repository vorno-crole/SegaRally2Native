// macOS: the news.txt download of UPDATE AVAILABLE (mac_platform.h), through
// NSURLSession in place of WinHTTP. Same limits as on Windows: HTTPS only,
// status 200, at most 256 KB, a few seconds at most without a connection.
#include "mac_platform.h"

#import <Foundation/Foundation.h>

namespace {
NSString* ns(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += *w < 0x80 ? char(*w) : '?';   // host and path are ASCII
    return [NSString stringWithUTF8String:s.c_str()];
}
}  // namespace

bool mac_https_get(const wchar_t* host, const wchar_t* path, std::string& body) {
    body.clear();
    @autoreleasepool {
        NSURL* url = [NSURL URLWithString:[NSString stringWithFormat:@"https://%@%@", ns(host), ns(path)]];
        if (!url) return false;
        NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest = 4;
        config.timeoutIntervalForResource = 8;
        config.HTTPAdditionalHeaders = @{@"User-Agent" : @"SegaRally2Native"};
        NSURLSession* session = [NSURLSession sessionWithConfiguration:config];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block bool ok = false;
        __block std::string result;
        NSURLSessionDataTask* task = [session dataTaskWithURL:url
                                            completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
            auto* http = [response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)response : nil;
            if (!error && http && http.statusCode == 200 && data && data.length <= 256 * 1024) {
                result.assign(static_cast<const char*>(data.bytes), data.length);
                ok = true;
            }
            dispatch_semaphore_signal(done);
        }];
        [task resume];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t(10) * NSEC_PER_SEC));
        [session invalidateAndCancel];
        if (ok) body = std::move(result);
        return ok;
    }
}
