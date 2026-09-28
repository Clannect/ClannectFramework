// Files on a real disk, inside a TemporaryDirectory: atomic save, reads with
// limits, directories, mapping, standard paths, settings and the watcher.

#include "cfw/io/Environment.h"

#include <cstdlib>
#include "cfw/io/FileSystem.h"
#include "cfw/io/FileWatcher.h"
#include "cfw/io/MappedFile.h"
#include "cfw/io/Settings.h"
#include "cfw/io/StandardPaths.h"
#include "cfw/io/TemporaryDirectory.h"

#include <thread>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

TemporaryDirectory makeTemp() {
    Result<TemporaryDirectory> temp = TemporaryDirectory::create("cfw-test-");
    check(temp.ok(), "temporary directory created");
    return std::move(temp).value();
}

void atomicSaveReplacesAndLeavesNoDebris() {
    TemporaryDirectory temp = makeTemp();
    const Path file = temp.path() / "scene.cescene";
    check(writeFileAtomic(file, "first").ok(), "create by atomic write");
    check(writeFileAtomic(file, "second version").ok(), "replace by atomic write");
    checkEqual(readTextFile(file).valueOr(String()), String("second version"), "new contents visible");

    const auto entries = listDirectory(temp.path()).valueOr({});
    checkEqual(entries.size(), std::size_t(1), "no temporary files left behind");

    const Result<void> noParent = writeFileAtomic(temp.path() / "missing-dir" / "x.txt", "data");
    check(!noParent.ok() && noParent.error().code() == ErrorCode::NotFound, "missing parent is NotFound");
}

void unicodePathsWork() {
    TemporaryDirectory temp = makeTemp();
    const Path dir = temp.path() / "Sp\xC3\xA9l \xE6\x97\xA5\xE6\x9C\xAC";
    check(createDirectories(dir).ok(), "create a non-ASCII directory");
    const Path file = dir / "d\xC3\xA9j\xC3\xA0.txt";
    check(writeFileAtomic(file, "ok").ok(), "write a non-ASCII file name");
    checkEqual(readTextFile(file).valueOr(String()), String("ok"), "read it back");
    checkEqual(file.fileName(), String("d\xC3\xA9j\xC3\xA0.txt"), "file name round-trips as UTF-8");
    check(file.toString().find('\\') == String::npos, "toString uses '/' on every platform");
}

void readsEnforceLimitsAndEncoding() {
    TemporaryDirectory temp = makeTemp();
    const Path big = temp.path() / "big.bin";
    check(writeFileAtomic(big, String(1000, 'x')).ok(), "write 1000 bytes");
    const auto limited = readFile(big, 100);
    check(!limited.ok() && limited.error().code() == ErrorCode::LimitExceeded, "size limit enforced before reading");

    const Path bom = temp.path() / "bom.txt";
    check(writeFileAtomic(bom, "\xEF\xBB\xBFhello").ok(), "write with BOM");
    checkEqual(readTextFile(bom).valueOr(String()), String("hello"), "BOM dropped");

    const Path binary = temp.path() / "bad.txt";
    check(writeFileAtomic(binary, "ok\xFF").ok(), "write invalid UTF-8");
    check(!readTextFile(binary).ok(), "text read rejects invalid UTF-8");

    const auto missing = readFile(temp.path() / "nope");
    check(!missing.ok() && missing.error().code() == ErrorCode::NotFound, "missing file is NotFound");
}

void directoriesAndMetadata() {
    TemporaryDirectory temp = makeTemp();
    check(createDirectories(temp.path() / "a" / "b" / "c").ok(), "nested create");
    check(createDirectories(temp.path() / "a").ok(), "existing directory is fine");
    check(writeFileAtomic(temp.path() / "a" / "z.txt", "12345").ok(), "file in a");
    check(writeFileAtomic(temp.path() / "a" / "m.txt", "1").ok(), "another file");

    const auto entries = listDirectory(temp.path() / "a").valueOr({});
    checkEqual(entries.size(), std::size_t(3), "two files and a directory");
    check(entries[0].path.fileName() == "b" && entries[0].isDirectory, "sorted: b first");
    checkEqual(entries[2].size, std::uint64_t(5), "size reported");
    checkEqual(fileSize(temp.path() / "a" / "z.txt").valueOr(0), std::uint64_t(5), "fileSize");

    check(copyFile(temp.path() / "a" / "z.txt", temp.path() / "copy.txt").ok(), "copy");
    check(renameFile(temp.path() / "copy.txt", temp.path() / "moved.txt").ok(), "rename");
    check(isFile(temp.path() / "moved.txt") && !exists(temp.path() / "copy.txt"), "rename moved it");
    check(removeAll(temp.path() / "a").ok() && !exists(temp.path() / "a"), "removeAll");
    check(removeFile(temp.path() / "never-existed").ok(), "removing nothing succeeds");
}

void temporaryDirectoryCleansUp() {
    Path where;
    {
        TemporaryDirectory temp = makeTemp();
        where = temp.path();
        check(writeFileAtomic(where / "file.txt", "x").ok(), "write inside");
        check(isDirectory(where), "exists while alive");
    }
    check(!exists(where), "removed with its contents on destruction");
}

void mapsFiles() {
    TemporaryDirectory temp = makeTemp();
    const Path file = temp.path() / "asset.bin";
    check(writeFileAtomic(file, "mapped bytes").ok(), "write");
    Result<MappedFile> mapped = MappedFile::open(file);
    check(mapped.ok(), "map");
    const auto bytes = mapped.value().bytes();
    checkEqual(StringView(reinterpret_cast<const char *>(bytes.data()), bytes.size()), StringView("mapped bytes"), "contents");

    const Path empty = temp.path() / "empty.bin";
    check(writeFileAtomic(empty, "").ok(), "write empty");
    Result<MappedFile> none = MappedFile::open(empty);
    check(none.ok() && none.value().size() == 0, "an empty file maps to an empty span");
}

void standardPathsFollowTheQtLayout() {
    const AppIdentity app{"Clannect", "Clannect Engine"};
    const Result<Path> data = appDataDirectory(app);
    check(data.ok(), "app data directory resolves");
    check(data.value().toString().ends_with("Clannect/Clannect Engine"), "<base>/<org>/<app>");
    check(cacheDirectory(app).value().toString().ends_with("cache") ||
              cacheDirectory(app).value().toString().ends_with("Clannect Engine"),
          "cache directory resolves");
    check(documentsDirectory().ok() && homeDirectory().ok(), "documents and home resolve");
}

void settingsPersistAtomically() {
    TemporaryDirectory temp = makeTemp();
    const Path file = temp.path() / "nested" / "settings.json";
    {
        Settings settings = Settings::open(file).value();
        checkEqual(settings.getInt("editor/zoom", 3), std::int64_t(3), "missing key gives the fallback");
        settings.set("editor/zoom", 5);
        settings.set("editor/theme", "dark");
        settings.set("editor/snap", true);
        check(settings.isDirty(), "changes mark it dirty");
        check(settings.save().ok(), "save creates the directory and writes");
        check(!settings.isDirty(), "clean after save");
    }
    Settings reloaded = Settings::open(file).value();
    checkEqual(reloaded.getInt("editor/zoom", 0), std::int64_t(5), "int persisted");
    checkEqual(reloaded.getString("editor/theme", ""), String("dark"), "string persisted");
    check(reloaded.getBool("editor/snap", false), "bool persisted");
    checkEqual(reloaded.getInt("editor/theme", 7), std::int64_t(7), "wrong type gives the fallback");
    reloaded.set("editor/zoom", 5);
    check(!reloaded.isDirty(), "setting an unchanged value is not a change");

    check(writeFileAtomic(file, "[1, 2]").ok(), "valid JSON, wrong shape");
    const auto wrongShape = Settings::open(file);
    check(!wrongShape.ok() && wrongShape.error().code() == ErrorCode::Corrupt, "a non-object file is Corrupt");
    check(writeFileAtomic(file, "{ broken").ok(), "invalid JSON");
    const auto broken = Settings::open(file);
    check(!broken.ok() && broken.error().code() == ErrorCode::ParseError, "invalid JSON is a ParseError, not ignored");
}

void watcherSeesChanges() {
    TemporaryDirectory temp = makeTemp();
    const Path a = temp.path() / "a.txt";
    check(writeFileAtomic(a, "1").ok(), "initial file");
    FileWatcher watcher;
    watcher.watch(temp.path());
    check(watcher.poll().empty(), "nothing changed yet");

    const Path b = temp.path() / "b.txt";
    check(writeFileAtomic(b, "new").ok(), "create b");
    check(writeFileAtomic(a, "changed size").ok(), "modify a");
    const auto changes = watcher.poll();
    checkEqual(changes.size(), std::size_t(2), "two changes");
    check(changes.size() == 2 && changes[0].path == a && changes[0].kind == FileChange::Kind::Modified, "a modified");
    check(changes.size() == 2 && changes[1].path == b && changes[1].kind == FileChange::Kind::Created, "b created");

    check(removeFile(b).ok(), "delete b");
    const auto removed = watcher.poll();
    check(removed.size() == 1 && removed[0].kind == FileChange::Kind::Removed, "b removed");
}

} // namespace

void readsEnvironmentVariables() {
#ifdef _WIN32
    _putenv_s("CFW_ENV_TEST", "caf\xc3\xa9");
#else
    setenv("CFW_ENV_TEST", "caf\xc3\xa9", 1);
#endif
    const auto value = environmentVariable("CFW_ENV_TEST");
    check(value.has_value(), "set variable found");
    check(!environmentVariable("CFW_ENV_TEST_UNSET_1234").has_value(), "unset variable is nothing");
}

void setsEnvironmentVariables() {
    check(setEnvironmentVariable("CFW_TEST_SET", "caf\xc3\xa9 = ok"), "a variable is set");
    check(environmentVariable("CFW_TEST_SET") == String("caf\xc3\xa9 = ok"), "and reads back, UTF-8 intact");
    const char *crt = std::getenv("CFW_TEST_SET");
    check(crt != nullptr, "the C runtime sees it too");
    check(setEnvironmentVariable("CFW_TEST_SET", "second"), "set again");
    check(environmentVariable("CFW_TEST_SET") == String("second"), "replaces the value");
    check(!setEnvironmentVariable("", "x"), "an empty name is refused");
    check(!setEnvironmentVariable("A=B", "x"), "a name with '=' is refused");
}

void findsTheExecutable() {
    auto self = executablePath();
    check(bool(self), "the executable's path is known");
    if (self) {
        check(self.value().isAbsolute(), "absolute");
        check(self.value().stem().find("FileSystemTest") != String::npos, "and it is this test");
        check(isFile(self.value()), "and it exists");
    }
}

int main() {
    readsEnvironmentVariables();
    setsEnvironmentVariables();
    findsTheExecutable();
    atomicSaveReplacesAndLeavesNoDebris();
    unicodePathsWork();
    readsEnforceLimitsAndEncoding();
    directoriesAndMetadata();
    temporaryDirectoryCleansUp();
    mapsFiles();
    standardPathsFollowTheQtLayout();
    settingsPersistAtomically();
    watcherSeesChanges();
    return cfw::test::finish("FileSystemTest");
}
