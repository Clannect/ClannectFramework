// macOS desktop services: NSOpenPanel for the file picker (a sheet-less
// modal panel), NSWorkspace for Finder and links. Built with ARC.

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "cfw/app/Desktop.h"

namespace cfw {

namespace {

NSString *toNs(StringView text) {
    return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] ?: @"";
}

} // namespace

Result<std::vector<Path>> chooseFilesToOpen(const Window *, const OpenFileOptions &options) {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setMessage:toNs(options.title)];
        [panel setTitle:toNs(options.title)];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:options.multiple ? YES : NO];
        if (!options.directory.empty()) {
            [panel setDirectoryURL:[NSURL fileURLWithPath:toNs(options.directory.toString()) isDirectory:YES]];
        }
        // NSOpenPanel filters by type, not by pattern: "*.png" becomes the
        // png type; any "*" or "*.*" allows everything.
        NSMutableArray<UTType *> *types = [NSMutableArray array];
        bool everything = options.filters.empty();
        for (const FileFilter &filter : options.filters) {
            for (const String &pattern : filter.patterns) {
                if (pattern == "*" || pattern == "*.*") {
                    everything = true;
                } else if (pattern.size() > 2 && pattern.compare(0, 2, "*.") == 0) {
                    if (UTType *type = [UTType typeWithFilenameExtension:toNs(StringView(pattern).substr(2))]) {
                        [types addObject:type];
                    }
                }
            }
        }
        if (!everything && [types count] > 0) {
            [panel setAllowedContentTypes:types];
        }
        std::vector<Path> chosen;
        if ([panel runModal] == NSModalResponseOK) {
            for (NSURL *url in [panel URLs]) {
                if (const char *path = [[url path] UTF8String]) {
                    chosen.emplace_back(String(path));
                }
            }
        }
        return chosen;
    }
}

Result<std::optional<Path>> chooseFolder(const Window *, const ChooseFolderOptions &options) {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setMessage:toNs(options.title)];
        [panel setTitle:toNs(options.title)];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setCanCreateDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        if (!options.directory.empty()) {
            [panel setDirectoryURL:[NSURL fileURLWithPath:toNs(options.directory.toString()) isDirectory:YES]];
        }
        if ([panel runModal] == NSModalResponseOK) {
            if (const char *path = [[[panel URL] path] UTF8String]) {
                return std::optional<Path>(Path(String(path)));
            }
        }
        return std::optional<Path>{};
    }
}

Result<void> showInFileManager(const Path &folder) {
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:toNs(folder.toString()) isDirectory:YES];
        if (![[NSWorkspace sharedWorkspace] openURL:url]) {
            return Error(ErrorCode::IoError, "Finder could not open " + folder.toString());
        }
        return {};
    }
}

Result<void> openUrl(StringView link) {
    @autoreleasepool {
        NSURL *url = [NSURL URLWithString:toNs(link)];
        if (!url || ![[NSWorkspace sharedWorkspace] openURL:url]) {
            return Error(ErrorCode::IoError, "could not open " + String(link));
        }
        return {};
    }
}

} // namespace cfw
