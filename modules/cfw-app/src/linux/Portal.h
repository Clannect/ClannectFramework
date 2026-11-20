#pragma once

// Calling the freedesktop desktop portal (org.freedesktop.portal.*) with
// CFW's D-Bus client: the pieces ScreenCast and GlobalShortcuts share.
//
// Every portal call that needs the user returns a Request object whose
// Response signal carries the answer; the request's path is predictable from
// our unique name and a token we choose, so we listen before calling.

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <vector>

#include "DBus.h"

namespace cfw::portal {

inline constexpr const char *kPortal = "org.freedesktop.portal.Desktop";
inline constexpr const char *kPath = "/org/freedesktop/portal/desktop";
inline constexpr auto kCallTimeout = std::chrono::seconds(10);

// A token unique in this process ("cfw1234_5").
[[nodiscard]] String newToken();
// {key: variant(value)} entries and a{sv} built from them.
[[nodiscard]] dbus::Value entry(const char *key, dbus::Value value);
[[nodiscard]] dbus::Value options(std::vector<dbus::Value> entries);
// a{sv} as key -> value (the variants' contents).
[[nodiscard]] std::map<String, dbus::Value> dict(const dbus::Value &v);
// Calls a portal method that answers through a Request and waits for the
// answer's results (Cancelled when the user declines, or when `cancel` turns
// true while waiting).
[[nodiscard]] Result<std::map<String, dbus::Value>> request(dbus::Connection &bus, const char *interface,
                                                            const char *method, std::vector<dbus::Value> args,
                                                            const String &token, std::chrono::seconds wait,
                                                            const std::atomic<bool> *cancel = nullptr);
// A portal interface's unsigned property ("version").
[[nodiscard]] std::optional<std::uint32_t> property(dbus::Connection &bus, const char *interface, const char *name);

} // namespace cfw::portal
