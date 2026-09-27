# 0011 — Linux CI, Clang, and libFuzzer targets with a committed corpus

**Status:** accepted, 2026-09-27. Implements what 0005 said CI needed from M1.

## What was added

- **`.github/workflows/linux.yml`**, on every push and pull request:
  - GCC debug and release, and Clang debug: build with `-Werror`, run every CTest test.
  - `asan`: ASan + UBSan + LSan over the full suite. A leak, or any UB report, fails the job (§8).
  - `tsan`: TSan over `cfw-net` (§8 asks for it on `cfw-net` and, later, `cfw-app`).
  - `fuzz`: Clang + libFuzzer + ASan/UBSan. It replays the corpus, then fuzzes each target for 60 s and
    uploads any crashing input.
- **`fuzz/`**: one libFuzzer target per untrusted-input parser: `FuzzJson`, `FuzzByteReader`, `FuzzUrl`
  (URLs, percent-decoding, UTF-8/UTF-16), `FuzzWebSocketFrames` (both roles, fed in uneven pieces) and
  `FuzzHttp` (request and response heads, the WebSocket handshake checks, and bodies under every framing).
  Targets check properties as well as crashes: JSON must write and re-parse to an equal value, and a body
  must never deliver more bytes than its limit allows.
- **Corpus replay on every toolchain.** Each target also links into `fuzz/ReplayMain.cpp` and runs over
  `fuzz/corpus/<Target>/` as a CTest test (`<Target>Corpus`, label `fuzz`). MinGW has no libFuzzer, but it
  still runs every input that ever found a bug, in about 0.3 s.
- **Presets:** `tsan` and `fuzz`.

## What it found on its first run

These are not hypothetical. Each has a regression test.

1. **cfw-net stalled on large sends on Linux.** When a flush filled the socket buffer, the connection never
   asked the loop for writability, so the rest of the queue waited for the next `send()`. Windows'
   auto-tuned loopback buffers took the whole 4 MB message in one call, which hid the bug.
   `WebSocketTest`'s 4 MB message timed out on Linux. Fixed in `TcpConnection::flush`.
2. **JSON round-trip inequality.** `-2.5e10` is a Double. Like Qt, the writer prints it as `-25000000000`,
   which re-parses as an Integer, and `operator==` compared the variant type. `JsonValue` equality now
   compares an Integer and a Double as doubles, which is Qt's rule. Two Integers still compare exactly.
3. **CFW did not compile with Clang.** `-Wshadow=local` is GCC-only. Also, `VariantArray` and
   `JsonObject` defined inline members that instantiate `std::vector` of a type that was still incomplete.
   GCC accepts that; Clang rejects it. Those members are now defined after the complete type.

After the fixes, each target ran for 90–180 s (0.45–3 million executions) with no findings. The corpus was
then minimised with `-merge=1` and committed: about 1,900 inputs, 0.5 MB. `.gitattributes` marks it `-text`
so no checkout changes its bytes.

## Running a fuzzer locally

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p scratch-corpus
build/fuzz/fuzz/FuzzJson scratch-corpus fuzz/corpus/FuzzJson -max_total_time=300
# after a crash: reproduce in any build, including MinGW
build/debug/fuzz/FuzzJsonReplay crash-<hash>
# fold new coverage back into the committed corpus
build/fuzz/fuzz/FuzzJson -merge=1 fuzz/corpus/FuzzJson scratch-corpus
```

Any input that crashed goes into `fuzz/corpus/<Target>/` as `regress-<what>` along with the fix.

## Not done yet

- **Continuous fuzzing.** §8 says "fuzzed continuously". 60 s per push is a smoke test, not that. The
  next step is a scheduled job with a longer budget that carries its corpus over between runs (or
  OSS-Fuzz / ClusterFuzzLite).
- **Windows CI.** There is no MinGW job yet, and it cannot run on a GitHub runner without pinning a
  MinGW-w64 GCC 13 (see the README's note on the toolchain). `ubsan-trap` and `SocketWin32.cpp` still only
  run on the dev machine.
- **Benchmarks in CI.** The committed baselines are from one Windows machine, so a hosted runner cannot
  compare against them. The allocation budget does run everywhere (`BenchAllocationBudgets`).
