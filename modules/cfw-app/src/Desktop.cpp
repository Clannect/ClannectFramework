#include "cfw/app/Desktop.h"

#include "cfw/core/Strings.h"
#include "cfw/io/Process.h"
#include "cfw/platform/Window.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <filesystem>
#include <string>

#include "cfw/core/Utf8.h"
#endif

namespace cfw {

#ifdef _WIN32

namespace {

std::wstring wide(StringView text) {
    const Result<std::u16string> converted = utf8ToUtf16(text);
    if (!converted) {
        return {};
    }
    return std::wstring(converted.value().begin(), converted.value().end());
}

Result<void> shellOpen(const std::wstring &target) {
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        return Error(ErrorCode::IoError, "the shell could not open it");
    }
    return {};
}

} // namespace

Result<std::vector<Path>> chooseFilesToOpen(const Window *owner, const OpenFileOptions &options) {
    // "Name\0*.a;*.b\0...\0\0"
    std::wstring filter;
    for (const FileFilter &f : options.filters) {
        filter += wide(f.name);
        filter.push_back(L'\0');
        filter += wide(join(f.patterns, ";"));
        filter.push_back(L'\0');
    }
    if (filter.empty()) {
        filter = std::wstring(L"All files", 9) + L'\0' + L"*.*" + L'\0';
    }
    filter.push_back(L'\0');

    std::wstring buffer(65536, L'\0');
    const std::wstring title = wide(options.title);
    std::wstring directory;
    if (!options.directory.empty()) {
        std::filesystem::path native = options.directory.native();
        native.make_preferred();
        directory = native.wstring();
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = owner ? static_cast<HWND>(owner->nativeHandle()) : nullptr;
    dialog.lpstrFilter = filter.c_str();
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = DWORD(buffer.size());
    dialog.lpstrTitle = title.c_str();
    dialog.lpstrInitialDir = directory.empty() ? nullptr : directory.c_str();
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   (options.multiple ? OFN_ALLOWMULTISELECT : 0);
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError() != 0) {
            return Error(ErrorCode::IoError, "the file dialog failed");
        }
        return std::vector<Path>{}; // cancelled
    }

    // One file: its full path. Several: the folder, then each name, then an
    // empty string.
    std::vector<std::wstring> parts;
    for (const wchar_t *p = buffer.c_str(); *p; p += wcslen(p) + 1) {
        parts.emplace_back(p);
    }
    std::vector<Path> chosen;
    if (parts.size() == 1) {
        chosen.push_back(Path(std::filesystem::path(parts[0])));
    } else {
        const std::filesystem::path folder(parts[0]);
        for (std::size_t i = 1; i < parts.size(); ++i) {
            chosen.push_back(Path(folder / parts[i]));
        }
    }
    return chosen;
}

Result<std::optional<Path>> chooseFolder(const Window *owner, const ChooseFolderOptions &options) {
    // The Vista-style dialog in folder mode (COM, apartment-threaded on the
    // window's thread).
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(init);
    struct Uninit {
        bool active;
        ~Uninit() {
            if (active) {
                CoUninitialize();
            }
        }
    } uninit{uninitialize};

    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void **>(&dialog)))) {
        return Error(ErrorCode::Unsupported, "the folder dialog is not available");
    }
    struct Release {
        IUnknown *object;
        ~Release() { object->Release(); }
    } releaseDialog{dialog};

    FILEOPENDIALOGOPTIONS flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    const std::wstring title = wide(options.title);
    dialog->SetTitle(title.c_str());
    if (!options.directory.empty()) {
        std::filesystem::path native = options.directory.native();
        native.make_preferred();
        IShellItem *start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(native.wstring().c_str(), nullptr, IID_IShellItem,
                                                  reinterpret_cast<void **>(&start)))) {
            dialog->SetFolder(start);
            start->Release();
        }
    }
    const HRESULT shown = dialog->Show(owner ? static_cast<HWND>(owner->nativeHandle()) : nullptr);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::optional<Path>{};
    }
    if (FAILED(shown)) {
        return Error(ErrorCode::IoError, "the folder dialog failed");
    }
    IShellItem *item = nullptr;
    if (FAILED(dialog->GetResult(&item))) {
        return Error(ErrorCode::IoError, "the folder dialog returned nothing");
    }
    PWSTR name = nullptr;
    const HRESULT got = item->GetDisplayName(SIGDN_FILESYSPATH, &name);
    item->Release();
    if (FAILED(got) || name == nullptr) {
        return Error(ErrorCode::IoError, "the chosen folder has no file system path");
    }
    Path chosen{std::filesystem::path(std::wstring(name))};
    CoTaskMemFree(name);
    return std::optional<Path>(std::move(chosen));
}

Result<void> showInFileManager(const Path &folder) {
    std::filesystem::path native = folder.native();
    native.make_preferred();
    return shellOpen(native.wstring());
}

Result<void> openUrl(StringView url) { return shellOpen(wide(url)); }

#elif !defined(__APPLE__) // macOS: mac/DesktopCocoa.mm

namespace {

std::vector<Path> lines(StringView text) {
    std::vector<Path> out;
    for (const StringView line : split(text, '\n', SplitMode::SkipEmpty)) {
        out.emplace_back(String(line));
    }
    return out;
}

Result<void> xdgOpen(StringView target) {
    const std::optional<Path> opener = findExecutable("xdg-open");
    if (!opener) {
        return Error(ErrorCode::Unsupported, "xdg-open is not installed");
    }
    ProcessOptions options;
    options.arguments = {String(target)};
    options.timeout = std::chrono::seconds(20);
    const Result<ProcessResult> ran = runProcess(*opener, options);
    if (!ran) {
        return ran.error();
    }
    if (ran.value().timedOut || ran.value().exitCode != 0) {
        return Error(ErrorCode::IoError, "xdg-open could not open " + String(target));
    }
    return {};
}

} // namespace

Result<std::vector<Path>> chooseFilesToOpen(const Window *, const OpenFileOptions &options) {
    ProcessOptions process;
    process.timeout = std::chrono::hours(24); // the person takes their time
    if (const std::optional<Path> zenity = findExecutable("zenity")) {
        process.arguments = {"--file-selection", "--title=" + options.title};
        if (options.multiple) {
            process.arguments.push_back("--multiple");
            process.arguments.push_back("--separator=\n");
        }
        if (!options.directory.empty()) {
            process.arguments.push_back("--filename=" + options.directory.toString() + "/");
        }
        for (const FileFilter &f : options.filters) {
            process.arguments.push_back("--file-filter=" + f.name + " | " + join(f.patterns, " "));
        }
        const Result<ProcessResult> ran = runProcess(*zenity, process);
        if (!ran) {
            return ran.error();
        }
        // zenity exits with 1 when cancelled.
        return ran.value().exitCode == 0 ? lines(ran.value().standardOutput) : std::vector<Path>{};
    }
    if (const std::optional<Path> kdialog = findExecutable("kdialog")) {
        process.arguments = {"--getopenfilename",
                             options.directory.empty() ? String(".") : options.directory.toString()};
        String filters;
        for (const FileFilter &f : options.filters) {
            if (!filters.empty()) {
                filters += "\n";
            }
            filters += join(f.patterns, " ") + "|" + f.name;
        }
        process.arguments.push_back(filters.empty() ? String("*") : filters);
        if (options.multiple) {
            process.arguments.push_back("--multiple");
            process.arguments.push_back("--separate-output");
        }
        process.arguments.push_back("--title");
        process.arguments.push_back(options.title);
        const Result<ProcessResult> ran = runProcess(*kdialog, process);
        if (!ran) {
            return ran.error();
        }
        return ran.value().exitCode == 0 ? lines(ran.value().standardOutput) : std::vector<Path>{};
    }
    return Error(ErrorCode::Unsupported, "no file picker (zenity or kdialog) is installed");
}

Result<std::optional<Path>> chooseFolder(const Window *, const ChooseFolderOptions &options) {
    ProcessOptions process;
    process.timeout = std::chrono::hours(24);
    std::optional<Path> program;
    if ((program = findExecutable("zenity"))) {
        process.arguments = {"--file-selection", "--directory", "--title=" + options.title};
        if (!options.directory.empty()) {
            process.arguments.push_back("--filename=" + options.directory.toString() + "/");
        }
    } else if ((program = findExecutable("kdialog"))) {
        process.arguments = {"--getexistingdirectory",
                             options.directory.empty() ? String(".") : options.directory.toString(), "--title",
                             options.title};
    } else {
        return Error(ErrorCode::Unsupported, "no folder picker (zenity or kdialog) is installed");
    }
    const Result<ProcessResult> ran = runProcess(*program, process);
    if (!ran) {
        return ran.error();
    }
    const std::vector<Path> chosen = lines(ran.value().standardOutput);
    if (ran.value().exitCode != 0 || chosen.empty()) {
        return std::optional<Path>{}; // cancelled
    }
    return std::optional<Path>(chosen.front());
}

Result<void> showInFileManager(const Path &folder) { return xdgOpen(folder.toString()); }

Result<void> openUrl(StringView url) { return xdgOpen(url); }

#endif

} // namespace cfw
