//
//  AppDelegate.m
//  PasswordDefense
//
//  Created by spooks on 11/03/2026.
//

#import "AppDelegate.h"

@interface AppDelegate ()


@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {
    // Insert code here to initialize your application
}


- (void)applicationWillTerminate:(NSNotification *)aNotification {
    // Insert code here to tear down your application
}


- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app {
    return YES;
}

// Single-window app: closing the red ✕ should quit, not just hide the window.
// Without this, the process keeps running headlessly after the last NSWindow
// closes and the user has to Cmd+Q from the menu bar (which then races the
// background HTTP thread teardown).
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}


@end
