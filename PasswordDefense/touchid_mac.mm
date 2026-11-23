// touchid_mac.mm — see touchid.h.

#include "touchid.h"

#import <Cocoa/Cocoa.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <Security/Security.h>

#include <algorithm>
#include <string>

namespace {

NSString* const kSecretService = @"com.StackedSoftware.PasswordDefenseOffline.touchid";
NSString* const kMarkerService = @"com.StackedSoftware.PasswordDefenseOffline.touchid.enabled";

NSMutableDictionary* BaseQuery(NSString* service, const std::string& account)
{
    NSMutableDictionary* q = [NSMutableDictionary dictionary];
    q[(__bridge id)kSecClass]                    = (__bridge id)kSecClassGenericPassword;
    q[(__bridge id)kSecAttrService]              = service;
    q[(__bridge id)kSecUseDataProtectionKeychain] = @YES;
    if (!account.empty())
        q[(__bridge id)kSecAttrAccount] = [NSString stringWithUTF8String:account.c_str()];
    return q;
}

std::string DescribeStatus(OSStatus st)
{
    if (st == errSecMissingEntitlement)
        return "Touch ID unlock needs the signed app (keychain entitlement missing).";
    NSString* msg = CFBridgingRelease(SecCopyErrorMessageString(st, nullptr));
    std::string out = msg ? std::string(msg.UTF8String) : std::string();
    return out.empty() ? ("Keychain error " + std::to_string((int)st)) : out;
}

touchid::Status StatusFromLAError(NSError* err)
{
    switch (err.code) {
    case LAErrorUserCancel:
    case LAErrorSystemCancel:
    case LAErrorAppCancel:
    case LAErrorUserFallback:
        return touchid::Status::Cancelled;
    case LAErrorBiometryNotAvailable:
    case LAErrorBiometryNotEnrolled:
    case LAErrorBiometryLockout:
        return touchid::Status::Unavailable;
    default:
        return touchid::Status::Error;
    }
}

dispatch_queue_t Queue()
{
    static dispatch_queue_t q = dispatch_queue_create("com.passworddefense.offline.touchid", DISPATCH_QUEUE_SERIAL);
    return q;
}

}  // namespace

namespace touchid {

bool is_available()
{
    LAContext* ctx = [[LAContext alloc] init];
    NSError* err = nil;
    return [ctx canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:&err];
}

bool app_is_active()
{
    return [NSApp isActive];
}

bool is_enabled(const std::string& account)
{
    if (account.empty()) return false;
    NSMutableDictionary* q = BaseQuery(kMarkerService, account);
    q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    return SecItemCopyMatching((__bridge CFDictionaryRef)q, nullptr) == errSecSuccess;
}

bool store_secret(const std::string& account, const std::string& secret, std::string* error)
{
    if (account.empty() || secret.empty()) {
        if (error) *error = "Nothing to store.";
        return false;
    }

    SecItemDelete((__bridge CFDictionaryRef)BaseQuery(kSecretService, account));

    CFErrorRef acErr = nullptr;
    SecAccessControlRef ac = SecAccessControlCreateWithFlags(
        kCFAllocatorDefault,
        kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
        kSecAccessControlBiometryCurrentSet,
        &acErr);
    if (!ac) {
        if (acErr) CFRelease(acErr);
        if (error) *error = "Could not create Touch ID access control.";
        return false;
    }

    NSMutableData* data = [NSMutableData dataWithBytes:secret.data() length:secret.size()];
    NSMutableDictionary* q = BaseQuery(kSecretService, account);
    q[(__bridge id)kSecAttrAccessControl] = (__bridge_transfer id)ac;
    q[(__bridge id)kSecAttrLabel]         = @"Password Defense (Touch ID unlock)";
    q[(__bridge id)kSecValueData]         = data;

    OSStatus st = SecItemAdd((__bridge CFDictionaryRef)q, nullptr);
    [data resetBytesInRange:NSMakeRange(0, data.length)];
    if (st != errSecSuccess) {
        if (error) *error = DescribeStatus(st);
        return false;
    }
    return true;
}

void enable_async(const std::string& account, const std::string& secret,
                  const std::string& reason, Callback done)
{
    // Copies captured by value; zeroed once stored.
    __block std::string acct = account;
    __block std::string sec  = secret;
    NSString* why = [NSString stringWithUTF8String:reason.c_str()];

    dispatch_async(Queue(), ^{
        LAContext* ctx = [[LAContext alloc] init];
        ctx.localizedFallbackTitle = @"";
        NSError* canErr = nil;
        if (![ctx canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:&canErr]) {
            std::fill(sec.begin(), sec.end(), '\0');
            done(Status::Unavailable, "Touch ID isn't available on this Mac.");
            return;
        }

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block BOOL ok = NO;
        __block NSError* evalErr = nil;
        [ctx evaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics
            localizedReason:why
                      reply:^(BOOL success, NSError* e) {
            ok = success;
            evalErr = e;
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        if (!ok) {
            std::fill(sec.begin(), sec.end(), '\0');
            Status s = evalErr ? StatusFromLAError(evalErr) : Status::Error;
            done(s, evalErr ? std::string(evalErr.localizedDescription.UTF8String) : "Touch ID failed.");
            return;
        }

        std::string err;
        bool stored = store_secret(acct, sec, &err);
        std::fill(sec.begin(), sec.end(), '\0');
        if (!stored) {
            done(Status::Error, err);
            return;
        }

        NSMutableDictionary* marker = BaseQuery(kMarkerService, acct);
        SecItemDelete((__bridge CFDictionaryRef)marker);
        marker[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly;
        marker[(__bridge id)kSecAttrLabel]      = @"Password Defense (Touch ID enabled)";
        marker[(__bridge id)kSecValueData]      = [NSData data];
        OSStatus st = SecItemAdd((__bridge CFDictionaryRef)marker, nullptr);
        if (st != errSecSuccess) {
            SecItemDelete((__bridge CFDictionaryRef)BaseQuery(kSecretService, acct));
            done(Status::Error, DescribeStatus(st));
            return;
        }
        done(Status::Ok, std::string());
    });
}

void read_async(const std::string& account, const std::string& reason, Callback done)
{
    __block std::string acct = account;
    NSString* why = [NSString stringWithUTF8String:reason.c_str()];

    dispatch_async(Queue(), ^{
        LAContext* ctx = [[LAContext alloc] init];
        ctx.localizedFallbackTitle = @"";   // no "Enter Password" — our own field is right there
        ctx.localizedReason = why;

        NSMutableDictionary* q = BaseQuery(kSecretService, acct);
        q[(__bridge id)kSecReturnData]              = @YES;
        q[(__bridge id)kSecMatchLimit]              = (__bridge id)kSecMatchLimitOne;
        q[(__bridge id)kSecUseAuthenticationContext] = ctx;

        CFTypeRef out = nullptr;
        OSStatus st = SecItemCopyMatching((__bridge CFDictionaryRef)q, &out);

        if (st == errSecSuccess && out) {
            NSMutableData* data = [(__bridge_transfer NSData*)out mutableCopy];
            std::string secret((const char*)data.bytes, data.length);
            [data resetBytesInRange:NSMakeRange(0, data.length)];
            done(Status::Ok, std::move(secret));
            return;
        }
        if (out) CFRelease(out);

        switch (st) {
        case errSecUserCanceled:
            done(Status::Cancelled, std::string());
            break;
        case errSecItemNotFound:
            // Missing, or invalidated because the enrolled fingerprints changed.
            done(Status::NotFound, std::string());
            break;
        case errSecAuthFailed:
            done(Status::Error, "Touch ID didn't match.");
            break;
        default:
            done(Status::Error, DescribeStatus(st));
            break;
        }
    });
}

void disable(const std::string& account)
{
    if (account.empty()) return;
    SecItemDelete((__bridge CFDictionaryRef)BaseQuery(kSecretService, account));
    SecItemDelete((__bridge CFDictionaryRef)BaseQuery(kMarkerService, account));
}

void disable_all()
{
    // No account in the query → matches every vault's item. The
    // data-protection keychain deletes all matches.
    SecItemDelete((__bridge CFDictionaryRef)BaseQuery(kSecretService, std::string()));
    SecItemDelete((__bridge CFDictionaryRef)BaseQuery(kMarkerService, std::string()));
}

}  // namespace touchid
