//
//  main.m
//  PasswordDefense
//
//  Created by spooks on 11/03/2026.
//

#import <Cocoa/Cocoa.h>
#include <sodium.h>
#include <cstdio>

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        // Initialize libsodium BEFORE any crypto call. Without this, every
        // AEAD / Argon2id / randombytes call runs against uninitialized
        // state (UB per libsodium docs). The cloud build had the same bug —
        // only caught when compute_auth_hash started returning empty and
        // login HTTP 400'd. The offline build has no login path, so no
        // deterministic failure, but vault encrypt/decrypt + master-key
        // derivation have been running on undefined state the whole time.
        if (sodium_init() < 0) {
            fprintf(stderr, "FATAL: libsodium failed to initialize\n");
            return 1;
        }
    }
    return NSApplicationMain(argc, argv);
}
