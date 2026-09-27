# 0009 — cfw-io: Qt-compatible JSON, settings migration, and a polling file watcher

**Status:** accepted, 2026-09-27

## JSON is byte-compatible with the Qt build

M1's exit criterion is that scene files round-trip byte-identically. `writeJson` reproduces
`QJsonDocument::toJson` exactly, and every rule was measured against Qt 6.8.3 (see 0007 for the oracle
method):

- **Key order.** Keys are sorted in UTF-16 code-unit order, not byte order. The two differ when a character
  above U+FFFF meets one in U+E000–U+FFFF.
- **Indentation.** Indented output uses 4 spaces. An empty container is written as `[`, newline, indent, `]`.
  The document ends with a trailing newline.
- **Numbers.** Integers, and integral doubles up to 2^53, are written as integers.
  Other doubles use the shortest round-trip digits, in plain or exponent form, whichever is shorter (plain
  on a tie), with at least two exponent digits. NaN and infinity become `null`.
- **Escaping.** Only `"`, `\` and control characters are escaped: control characters as `\u00xx` in
  lowercase hex, except for the named escapes (`\b` `\f` `\n` `\r` `\t`).

The tests round-trip three fixtures byte for byte: two edge-case documents written by Qt, and
`roundtrip_test.cescene`, a real scene saved by the engine.

### The parser is deliberately *more* accepting than Qt in one case, and stricter in one

| Input | Qt | CFW | Why |
|---|---|---|---|
| Scalar document (`42`) | Rejects | Accepts | RFC 8259 allows it. No scene is a scalar, so compatibility is unaffected. |
| Lone surrogate escape (`"\ud800"`) | Keeps it (UTF-16 can) | U+FFFD | UTF-8 cannot represent it. This never occurs in files CFW or Qt wrote. |

Everything else matches. Both accept a UTF-8 BOM and raw control characters. For both, the last of several
duplicate keys wins and a document may nest 1024 levels deep. Invalid UTF-8 and numbers beyond the double
range are errors in both. CFW's parser is iterative, so a document nested 200,000 levels deep is rejected
cleanly instead of overflowing the stack.

### Line endings: scene files must be `-text` in git

The engine writes LF. The engine repository checks files out with `core.autocrlf=true`, which rewrites
`.cescene` files to CRLF on checkout. That breaks byte identity outside of any code: a scene committed and
checked out comes back different. CFW's fixtures carry a `.gitattributes` with `*.cescene -text`.
**The engine repository should add the same line.**

## Settings: a JSON file, and a one-time import from the registry

The Qt build's `QSettings()` stores to the Windows registry, under `HKCU\Software\Clannect\Clannect Engine`.
`cfw::Settings` is a JSON file, which is portable, atomic to save, inspectable and testable. When the editor
moves to CFW (M5), it must import the registry values once on first run, or users lose their window layout.
The import belongs in cfw-platform, because reading the registry is a platform call.

The Figma access token is currently one of those registry values, in plain text. It should move to the OS
credential store (Windows Credential Manager, Keychain, libsecret) in cfw-platform, not into
`settings.json`.

## Standard paths match `QStandardPaths`

`appDataDirectory({"Clannect", "Clannect Engine"})` and the other functions resolve to the same folders Qt
used on each platform, so caches and data survive the port.

## The file watcher polls, for now

Native change notification (ReadDirectoryChangesW, inotify, FSEvents) needs an event loop to deliver
events, and the event loop is cfw-net (M2). `FileWatcher` compares size and modification time on `poll()`,
behind an interface a native implementation can replace without touching callers. Watching is limited to
files and the direct children of directories. Recursive watching arrives with the native version.

## Fuzzing

libFuzzer is not available with MinGW (0005). `FuzzSmokeTest` runs 45,000 deterministic mutated and random
inputs through the JSON reader and ByteReader on every build, including the UBSan build. It asserts that no
input crashes, and that every accepted document survives write and re-parse. Coverage-guided fuzzing with a
committed corpus starts when the Linux CI job exists.
