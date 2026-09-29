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

int main() {
#if !defined(_WIN32) && !defined(__APPLE__)
    standIns();
#endif
    return cfw::test::finish("DesktopTest");
}
