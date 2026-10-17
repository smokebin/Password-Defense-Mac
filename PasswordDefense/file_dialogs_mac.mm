// file_dialogs_mac.mm — macOS NSOpenPanel/NSSavePanel implementations
// Replaces Windows OPENFILENAMEA / GetOpenFileNameA / GetSaveFileNameA
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <string>
#include "app_internal.h"

static std::string RunOpenPanel(const char* title, NSArray<NSString*>* extensions, bool canCreateDirs = false)
{
    __block std::string result;

    // NSOpenPanel must run on the main thread
    void (^block)(void) = ^{
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = [NSString stringWithUTF8String:title];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.canCreateDirectories = canCreateDirs;

        if (extensions) {
            NSMutableArray<UTType*>* types = [NSMutableArray array];
            for (NSString* ext in extensions) {
                UTType* t = [UTType typeWithFilenameExtension:ext];
                if (t) [types addObject:t];
            }
            if (types.count > 0)
                panel.allowedContentTypes = types;
        }

        if ([panel runModal] == NSModalResponseOK && panel.URL) {
            result = std::string([panel.URL.path UTF8String]);
        }
    };

    if ([NSThread isMainThread])
        block();
    else
        dispatch_sync(dispatch_get_main_queue(), block);

    return result;
}

static std::string RunSavePanel(const char* title, const char* defaultExt, NSArray<NSString*>* extensions)
{
    __block std::string result;

    void (^block)(void) = ^{
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.title = [NSString stringWithUTF8String:title];
        panel.canCreateDirectories = YES;

        if (extensions) {
            NSMutableArray<UTType*>* types = [NSMutableArray array];
            for (NSString* ext in extensions) {
                UTType* t = [UTType typeWithFilenameExtension:ext];
                if (t) [types addObject:t];
            }
            if (types.count > 0)
                panel.allowedContentTypes = types;
        }

        if (defaultExt) {
            panel.nameFieldStringValue = [NSString stringWithFormat:@"Untitled.%s", defaultExt];
        }

        if ([panel runModal] == NSModalResponseOK && panel.URL) {
            result = std::string([panel.URL.path UTF8String]);
        }
    };

    if ([NSThread isMainThread])
        block();
    else
        dispatch_sync(dispatch_get_main_queue(), block);

    return result;
}

// ============================================================
// Public API — matches app_internal.h declarations
// ============================================================

std::string PickOpenFilePath_DB()
{
    return RunOpenPanel("Open Vault", @[@"db"]);
}

std::string PickSaveFilePath_DB()
{
    return RunSavePanel("Create Vault", "db", @[@"db"]);
}

std::string PickSaveFilePath_PWM()
{
    return RunSavePanel("Export Encrypted Vault", "pwm", @[@"pwm"]);
}

std::string PickOpenFilePath_PWM()
{
    return RunOpenPanel("Import Encrypted Vault", @[@"pwm"]);
}

std::string PickSaveFilePath_CSV()
{
    return RunSavePanel("Export CSV", "csv", @[@"csv"]);
}

std::string PickSaveFilePath_KDBX()
{
    return RunSavePanel("Export KeePass", "kdbx", @[@"kdbx"]);
}

std::string PickOpenFilePath_CSV()
{
    return RunOpenPanel("Import CSV", @[@"csv", @"txt"]);
}

std::string PickOpenFilePath_Backup()
{
    return RunOpenPanel("Select Backup File", @[@"lbdb"]);
}
