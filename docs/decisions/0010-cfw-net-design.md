# 0010 — cfw-net: started before M1 closed; a poll reactor; write coalescing; no TLS yet

**Status:** accepted, 2026-09-27

## Starting M2 while M1's last exit criterion is open

The milestone plan says not to start a milestone before the previous one's exit criteria are met. M1's remaining criterion is
that the Runtime builds with no Qt. That is engine-side work (M0/M1 in the engine repository), and it waits on
the engine's uncommitted working tree being settled. Every CFW-side M1 deliverable is done and benchmarked.
cfw-net depends only on cfw-core, so building it now changes nothing in M1 and removes the next blocker
early.

## A `poll()` reactor, not IOCP/epoll (yet)

The original plan listed epoll/kqueue/IOCP. The loop uses `WSAPoll`/`poll`: one portable code path, and simple to
reason about for thread affinity and teardown.

- **Scaling:** its cost is O(watched sockets) per wake-up. At the required scale ("hundreds of connections")
  that is measured, not assumed: 200 connections sustain **205,000 echoed messages/s on one thread, the same
  as Qt 6.8.3's 193,000–207,000** on the identical workload (`cfw-bench-net`).
- **Changing backend:** replacing the backend with IOCP/epoll is local to `EventLoopImpl::iterate` and
  `Socket*.cpp`. Do it when a benchmark at thousands of connections calls for it.
- **Old Windows 10:** before version 2004, `WSAPoll` did not report failed connects. Connect timeouts cover
  that (10 s by default), so a failed connect still ends.

## Write coalescing

The first version made one `send()` system call per message. On the same workload that gave 32,000
messages/s, 6× slower than Qt.

`TcpConnection::send` now only appends to the connection's queue and schedules one flush. The loop flushes
scheduled connections before it blocks, so everything produced during a pass goes out in one system call per
socket, with no added latency, because the flush happens before the loop sleeps. The schedule is a list of
plain pointers: sending does not allocate once the queue has grown to size. WebSocket frames are built into a
reused buffer.

## Shared ownership (the API rules ask for the reason)

- **Connect attempts** (`ConnectState`, the WebSocket client handshake) are shared between the
  `ConnectRequest` that can cancel them, loop callbacks and the resolver thread. A mutex around
  "cancelled / post to loop" guarantees the resolver cannot touch a loop after the request that owns the
  attempt is gone.
- **`alive` tokens** (`shared_ptr<bool>`) let every callback destroy the object that invoked it, which is
  the pattern the engine already relies on with `QPointer`.

## Reentrancy rule

Callbacks are copied before they are invoked, so a callback may replace any callback, itself included. The
WebSocket upgrade relies on this: the handshake finishes inside the TCP data callback and hands the
connection to the WebSocket, which installs new callbacks.

## No TLS yet: `wss://` fails with `Unsupported`

TLS needs either a dependency (mbedTLS, BoringSSL) or the OS stack (Schannel, SecureTransport), and choosing
one is a dependency decision. The Runtime's WebSocket server is plain `ws://` today (the Qt build
uses `NonSecureMode`), so nothing regresses. The HTTP client (asset cache, Figma) is the part that needs
TLS.

## Verified against Qt

A scratchpad program built against Qt 6.8.3 (never committed; see 0007) exercised CFW both ways: a Qt server
with a CFW client, and a Qt client with a CFW server.

- **Messages:** 1000 in order, plus multi-megabyte ones. Qt fragments large messages into 512 KB frames when
  it sends, and CFW reassembled them.
- **Text:** UTF-8 including an emoji.
- **Control frames:** ping and pong in both directions.
- **Close codes:** application codes 4001 and 4003, with their reasons, in both directions.
