// The desktop services, with stand-ins for the programs they run: the picker
// hands back what was chosen (or nothing when cancelled), and folders and
// links go to xdg-open. On Windows and macOS only that nothing fails to link:
// there the picker is the system's own panel, which waits for a person.

#include <cstdlib>

#include "cfw/app/Desktop.h"
#include "cfw/io/Environment.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/TemporaryDirectory.h"
#include "cfw/test/Check.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <sys/stat.h>
#endif

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

#if !defined(_WIN32) && !defined(__APPLE__)

void writeScript(const Path &path, StringView body) {
    check(bool(writeFileAtomic(path, "#!/bin/sh\n" + String(body))), "a stand-in script");
    ::chmod(path.toString().c_str(), 0755);
}

void standIns() {
    auto dir = TemporaryDirectory::create();
    check(bool(dir), "a temporary directory");
    const Path bin = dir.value().path();
    const Path log = bin / "log.txt";
    const String oldPath = environmentVariable("PATH").value_or("");
    setEnvironmentVariable("PATH", bin.toString());

    // A zenity that records its arguments and "chooses" two files.
    writeScript(bin / "zenity", "printf '%s\\n' \"$@\" > '" + log.toString() +
                                    "'\nprintf '/tmp/a.png\\n/tmp/b c.png\\n'\n");
    OpenFileOptions options;
    options.title = "Import Assets";
    options.multiple = true;
    options.filters = {{"Images", {"*.png", "*.jpg"}}};
    auto chosen = chooseFilesToOpen(nullptr, options);
    check(bool(chosen), "the picker runs");
    check(chosen && chosen.value().size() == 2 && chosen.value()[1].toString() == "/tmp/b c.png",
          "and returns each chosen file");
    const String args = readTextFile(log).valueOr("");
    check(args.find("--title=Import Assets") != String::npos, "with the title");
    check(args.find("--file-filter=Images | *.png *.jpg") != String::npos, "and the filter");

    // Cancelled: exit status 1, nothing chosen.
    writeScript(bin / "zenity", "exit 1\n");
    chosen = chooseFilesToOpen(nullptr, options);
    check(chosen && chosen.value().empty(), "cancelling chooses nothing");

    // The folder picker: zenity in directory mode.
    writeScript(bin / "zenity", "printf '%s\\n' \"$@\" > '" + log.toString() + "'\nprintf '/home/me/Clannect\\n'\n");
    ChooseFolderOptions folderOptions;
    folderOptions.title = "Install to";
    folderOptions.directory = Path("/opt");
    const auto folder = chooseFolder(nullptr, folderOptions);
    check(folder && folder.value() && folder.value()->toString() == "/home/me/Clannect", "a folder is chosen");
    const String folderArgs = readTextFile(log).valueOr("");
    check(folderArgs.find("--directory") != String::npos && folderArgs.find("--title=Install to") != String::npos &&
              folderArgs.find("--filename=/opt/") != String::npos,
          "in directory mode, with the title and start folder");
    writeScript(bin / "zenity", "exit 1\n");
    const auto none = chooseFolder(nullptr, folderOptions);
    check(none && !none.value(), "cancelling the folder picker chooses nothing");
    writeScript(bin / "zenity", "exit 1\n");

    // No picker at all.
    (void)removeFile(bin / "zenity");
    check(!chooseFilesToOpen(nullptr, options), "without a picker it fails");

    writeScript(bin / "xdg-open", "printf '%s' \"$1\" > '" + log.toString() + "'\n");
    check(bool(openUrl("https://clannect.com/")), "a link opens");
    checkEqual(readTextFile(log).valueOr(""), String("https://clannect.com/"), "in xdg-open");
    check(bool(showInFileManager(Path("/tmp"))), "a folder opens");
    checkEqual(readTextFile(log).valueOr(""), String("/tmp"), "there too");

    setEnvironmentVariable("PATH", oldPath);
}

#endif

} // namespace

// Wherever it runs, focusedScreen answers or says it cannot; an answer is a
// real monitor (with KWin or Windows present).
void focused() {
    auto f = focusedScreen();
    if (f) {
        check(f.value().geometry.width > 0 && f.value().geometry.height > 0, "the focused screen has a size");
        std::printf("focused screen: %s %d,%d %dx%d (%s)\n", f.value().name.c_str(), f.value().geometry.x,
                    f.value().geometry.y, f.value().geometry.width, f.value().geometry.height,
                    f.value().application.c_str());
    } else {
        std::printf("focused screen: not known (%s)\n", f.error().message().c_str());
        const ErrorCode c = f.error().code();
        check(c == ErrorCode::Unsupported || c == ErrorCode::NotFound || c == ErrorCode::Timeout,
              "or it says why not");
    }
}

int main() {
#if !defined(_WIN32) && !defined(__APPLE__)
    standIns();
#endif
    focused();
    return cfw::test::finish("DesktopTest");
}
