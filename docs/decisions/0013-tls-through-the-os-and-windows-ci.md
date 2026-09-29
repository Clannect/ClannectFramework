# 0013 — TLS through the operating system; Windows builds tested under Wine

**Status:** accepted, 2026-09-27. Direction from the project owner (Linux: "load the system OpenSSL"). It
closes the last CFW-side M2 items together with `cfw::Locale`.

## TLS: the OS's implementation, never our own crypto, never a shipped library

`TlsStream` is TLS 1.2+ over any `ByteStream`, with two backends:

- **Windows: Schannel** (`TlsSchannel.cpp`). The certificate chain and name are verified with the system
  policy on the `Executor`, because building a chain may fetch intermediates or revocation data.
- **Linux and other POSIX systems: the system's OpenSSL**, loaded at runtime (`TlsOpenSsl.cpp`).
  - `libssl.so.3` is tried first, then `libssl.so.1.1`. It is loaded with `dlopen`, and the ~60 functions
    used are declared in CFW with their documented C signatures, so building needs no OpenSSL headers or
    packages.
  - Verification uses the distribution's trust store (`SSL_CTX_set_default_verify_paths`, which honours
    `SSL_CERT_FILE`/`SSL_CERT_DIR`) plus `SSL_set1_host` or the IP-address check.
  - Throwaway server certificates (EC P-256) need OpenSSL 3.
  - With no usable libssl, `TlsStream::available()` is false and every handshake fails with a clear
    `Unsupported` error.

This keeps decision 0012 intact: CFW ships no crypto. As with Schannel, the TLS stack is the one the
operating system provides and patches. Writing our own TLS and crypto was rejected; constant-time
arithmetic and side channels are not where Clannect should spend its risk.

**Process-wide state (CFW's rules ask for the reason).** The loaded OpenSSL function table is a
function-local static: built once on first use (thread-safe initialisation) and immutable afterwards. The
library it describes is itself process-wide, so this is the one global in cfw-net. It is never unloaded,
because OpenSSL registers exit handlers and `dlclose` before them crashes at exit.

**Using it.** `HttpClient` now does `https://`: redirects still refuse https→http, and `HttpOptions::tls`
carries pinning. `WebSocket::connect` does `wss://`, with `ConnectOptions::tls`. Both default to the
system trust store, and both allow pinning for Clannect's own servers. `WebSocketServer` stays plain
`ws://`, as the Runtime is today; a TLS listener is `TlsStream::startServer` over accepted connections,
when it is needed.

## Found while doing it

- **Use-after-free (both backends).** A handshake that failed while handling incoming bytes reported the
  failure to its owner, which destroyed the stream; the data handler then read the freed state. ASan
  and TSan found it in the OpenSSL backend on the first run. Schannel had the same shape and is fixed the
  same way (the liveness guard the rest of cfw-net uses).
- **`TlsSchannel.cpp` had never compiled with MinGW.** `SCH_CREDENTIALS` (needed for TLS 1.3) is only
  declared with `SCHANNEL_USE_BLACKLISTS` defined and `<subauth.h>` included, as in the Windows SDK. The
  file was not in the build before this change.

## Windows in CI: a MinGW-w64 cross build, run under Wine

`cmake/toolchains/mingw-w64-cross.cmake` builds CFW for Windows on Linux with MinGW-w64 GCC 13 (the
reference compiler family), statically linked. The `windows-cross` CI job:

- builds everything with `-Werror`;
- runs the whole suite as Windows binaries under Wine: sockets, the Win32 file layer, known folders,
  Schannel;
- runs the TLS interop check in `testing/tls-interop`. Through Schannel, a Windows client connects to a
  native Linux server that uses the OpenSSL backend. It must accept the pinned certificate, echo 300 KB
  intact, and refuse the certificate when it is not pinned (untrusted).

Wine limits, stated rather than hidden:

- Wine needs a UTF-8 locale to map non-ASCII file names; the test environment sets it.
- Wine's `CertCreateSelfSignCertificate` does not accept CNG keys, so the Schannel *server* cannot run
  under Wine. `TlsTest` reports this and skips its end-to-end cases there (`CFW_UNDER_WINE`); the interop
  job covers the client.
- Wine is not Windows. The native Windows run on the dev machine (and a hosted Windows runner, when there
  is one) remains the authority; this job catches breakage on every push instead of weeks later.

## What would change this

- **macOS:** it needs a backend (Network.framework or Security.framework) before Clannect ships there.
- **Hosted Windows CI:** a hosted Windows runner with the reference MinGW would replace the Wine job, not
  the tests.
