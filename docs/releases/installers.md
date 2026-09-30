## Windows installers: online or offline?

Both install Clannect Framework for MinGW-w64 GCC 13 (headers, static libraries, the CMake package and the widget
gallery) into `%LOCALAPPDATA%\Programs\ClannectFramework\<version>`, for your user only, with no administrator
rights. Both add Clannect Framework to Windows' **Installed apps** (uninstall it from there), and can register it with
CMake so `find_package(ClannectFramework)` works without setting a path.

| | Online installer | Offline installer |
|---|---|---|
| File | `ClannectFramework-windows-x64-online-installer.exe` | `ClannectFramework-<version>-windows-x64-offline-installer.exe` |
| Size | Small: it downloads the package | Larger: the package is inside it |
| Internet | Needed while installing | Not needed |
| Versions | Any published version: you choose in the installer | Only the version it was made for |
| New releases | Can tell you when a new version is released (checked once a day at sign-in; pre-releases optional) | Does not check for updates |

**Pick the online installer** to stay up to date. **Pick the offline installer** for a machine without internet, or to
install exactly this version.

The installers are not code-signed, so SmartScreen may warn the first time you run one: choose **More info → Run
anyway**.
