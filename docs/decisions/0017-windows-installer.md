# 0017 — The Windows installer is a CFW program

**Status:** accepted, 2026-09-30.

Windows users get installers, as Qt's users do, not only zips. There are two: an **online installer** that
downloads the version you choose and can tell you about new ones, and an **offline installer** that carries one
version. Both are the same program, `tools/installer`, written with CFW.

## How it works

| Piece | How |
|---|---|
| One program, three roles | With a release zip appended to the executable it is the offline installer; without one, the online installer; installed as `<root>/maintenance/Clannect Framework Maintenance.exe`, the maintenance tool. `findPackage()` looks for a zip at the end of its own file: `ZipArchive` finds an archive from its end record, whatever precedes it. The release workflow makes the offline installer with `cat installer.exe package.zip`. |
| Where versions go | `%LOCALAPPDATA%\Programs\ClannectFramework\<version>`, side by side, per user. The manifest says `asInvoker`: Windows would otherwise ask for administrator rights for any program named "installer". |
| What Windows is told | An Installed apps entry per version (`HKCU\...\Uninstall\ClannectFramework-<version>`), whose Uninstall runs the maintenance tool with `--uninstall <folder>`; CMake's user package registry (`HKCU\Software\Kitware\CMake\Packages\ClannectFramework`), so `find_package` needs no prefix path; and, for online installs that want it, `--check-for-updates` in the Run key. |
| Downloads | GitHub's releases API over `HttpClient` (TLS through Schannel). The package is streamed to memory and checked against the size and SHA-256 GitHub publishes (`digest`) before it is opened. Only `https://` package URLs are accepted. |
| Update notifications | At logon, at most once every 20 hours: the newest release with a Windows package, newer than every installed version, not skipped, and a pre-release only if pre-releases are wanted. Nothing is installed without asking. |
| Safety | Only folders holding `installed.json` are ever deleted, so an install folder pointed at Documents loses nothing but CFW. A version is extracted to `<version>.partial` and renamed into place: an interrupted install leaves no half version. Zip entries that would leave the folder are refused (`zipEntryRelativePath`). |
| New in CFW | `cfw::ZipArchive` and `extractZip` (cfw-io), tested against Python's `zipfile` and Info-ZIP and fuzzed; `cfw::chooseFolder` (cfw-app). |

The installer's logic (`Installer.cpp`, `Download.cpp`) builds and is tested on every platform. The registry
calls (`System.cpp`) do nothing outside Windows; under Wine in CI they write to a sandbox key
(`CFW_INSTALLER_REGISTRY_SANDBOX`), which the tests read back. `InstallerEndToEnd` makes an offline installer,
installs with it and uninstalls with the maintenance tool it left.

## Rejected

- **NSIS, Inno Setup or WiX.** They are third-party code in the product's own installer, and CFW has the
  window, text, network, zip and file pieces an installer needs. Building it on CFW also tests CFW.
- **MSI.** Heavier than a per-user developer package needs, and it cannot download a version chosen at install
  time.
- **A resident tray program for update notifications.** A check at logon is enough for a developer tool, and
  costs nothing while it does not run.
- **Code signing.** It costs a yearly certificate. The installers are unsigned, and the release notes explain
  SmartScreen's warning.

## Known limits

- HTTP proxies are not supported yet (`HttpClient` connects directly). Behind a mandatory proxy, use the offline
  installer.
- Only MinGW-w64 GCC 13 packages exist, so the installers install those.
