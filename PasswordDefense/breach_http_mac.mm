// breach_http_mac.mm — minimal HTTPS GET for HIBP's k-anonymity range API.
//
// Why its own file: the offline build stubs out sync_manager::win_http_request
// (which cpp-httplib backed on Windows), and cpp-httplib itself is now
// unreferenced anywhere in this target. The breach check still needs ONE
// HTTPS call per unique password prefix — this is it, implemented natively
// via NSURLSession so we don't need to resurrect the whole HTTP stack
// (or keep OpenSSL linked) for a single endpoint.
//
// Scope on purpose: no POST, no auth, no bearer tokens, no host flexibility.
// HIBP's /range/<5-char-prefix> endpoint is the only legitimate online call
// remaining in the Mac offline build; keeping this helper narrow matches
// that reality and keeps the attack-surface audit trivial ("one function,
// GETs one endpoint, no body").
//
// Thread note: callers MUST NOT invoke this on the main thread. The
// semaphore wait would deadlock the UI; CheckBreachedPasswords already
// runs on a dedicated background thread, so it's safe there.

#import <Foundation/Foundation.h>
#include "sync_manager.h"   // HttpResponse struct
#include <string>

HttpResponse hibp_range_fetch(const std::string& sha1_prefix_hex)
{
    HttpResponse r;
    r.success = false;
    r.status_code = 0;

    if (sha1_prefix_hex.size() != 5) {
        r.error = "HIBP prefix must be 5 hex chars";
        return r;
    }

    NSString* urlStr = [NSString stringWithFormat:
        @"https://api.pwnedpasswords.com/range/%s",
        sha1_prefix_hex.c_str()];
    NSURL* url = [NSURL URLWithString:urlStr];
    if (!url) {
        r.error = "Could not build HIBP URL";
        return r;
    }

    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = @"GET";
    req.timeoutInterval = 8.0;   // bounded so a network hang can't wedge the worker
    [req setValue:@"PasswordDefense (offline, k-anonymity breach check)"
        forHTTPHeaderField:@"User-Agent"];

    __block NSData* respData = nil;
    __block NSURLResponse* respObj = nil;
    __block NSError* respErr = nil;

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask* task =
        [[NSURLSession sharedSession] dataTaskWithRequest:req
            completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                respData = data;
                respObj = response;
                respErr = error;
                dispatch_semaphore_signal(sem);
            }];
    [task resume];

    // Caller is on a background thread (verified: CheckBreachedPasswords runs
    // via std::thread). Wait with a small cushion over the request timeout so
    // the dispatch wait never times out before the request itself does.
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(10 * NSEC_PER_SEC)));

    if (respErr) {
        r.error = [[respErr localizedDescription] UTF8String] ?: "HIBP request failed";
        return r;
    }

    if ([respObj isKindOfClass:[NSHTTPURLResponse class]]) {
        NSHTTPURLResponse* http = (NSHTTPURLResponse*)respObj;
        r.status_code = (int)http.statusCode;
    }

    if (respData && respData.length > 0) {
        r.body.assign((const char*)respData.bytes, respData.length);
    }

    r.success = (r.status_code == 200);
    if (!r.success && r.error.empty()) {
        r.error = "HIBP returned non-200 status";
    }
    return r;
}
