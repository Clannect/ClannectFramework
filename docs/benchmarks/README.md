# Benchmarks

CFW's rule for performance: *parity is measured, not asserted.* This page records what is measured today, how, and against
what.

## Running

```sh
cmake --preset release && cmake --build --preset release
build/release/bench/cfw-bench                                   # one run, table
build/release/bench/cfw-bench --repeat 5 \
    --baseline bench/baselines/cfw-mingw-i5-12450H.json \
    --qt-reference bench/baselines/qt-6.8.3-mingw-i5-12450H.json
```

| Exit code | Meaning |
|---|---|
| 0 | Every budget was met. |
| 2 | An allocation budget was exceeded. This is checked on every CTest run (`BenchAllocationBudgets`), in every build type. |
| 3 | A benchmark is more than 5% slower than `--baseline`. |

## Method

- **Timing:** each benchmark is timed over N samples after warm-up; the median per operation is reported.
  `--repeat N` runs the whole suite N times and keeps each benchmark's best median, because machine noise
  (turbo, scheduling across the hybrid cores) only ever makes a run slower.
- **Noise:** on the reference laptop, two single runs of identical code differed by up to 25%. With
  `--repeat 5` they agree within ±3.6%, so the 5% regression gate is meaningful only with `--repeat 5`, and
  only against a baseline recorded on the same machine.
- **Allocations:** counted by replacing the global `operator new`. The benchmark links the C++ runtime
  statically, so allocations inside the standard library are counted too. Allocation counts do not depend
  on the machine, so they gate every build.
- **The regression gate trips:** injecting a doubled property lookup reported +118% and exit 3. Injecting
  one hidden allocation in `ClassInfo::indexOf` failed `BenchAllocationBudgets`.

**Reference machine:** Intel Core i5-12450H (8 cores, 12 threads, hybrid), 32 GB RAM, Windows 11, MinGW-w64
GCC 13.1, `-O3`.

**Qt reference:** a small program outside this repository, built against Qt 6.8.3 with the same compiler,
timed `QJsonDocument::fromJson` and `toJson(Indented)` on the identical file. Only the resulting numbers are
committed (`bench/baselines/qt-*.json`); see [decision 0007](../decisions/0007-qt-behaviour-pinned-by-oracle-values.md).
It also confirmed that **Qt re-writes CFW's 5 MB output byte for byte.**

## Results (2026-09-27, `--repeat 5`)

| Benchmark | CFW | Qt 6.8.3 | Spec budget | Status |
|---|---|---|---|---|
| Scene load, 5 MB JSON (parse) | 13.2 ms | 21.4 ms | ≤ Qt | **0.62× Qt** |
| Scene save, 5 MB JSON (write, Indented) | 7.2 ms | 21.8 ms | *(none)* | 0.33× Qt |
| Property get by `Name` | 5.4 ns, 0 allocations | *(no equivalent: `QVariantMap` lookup)* | O(1), 0 allocations | met |
| Property set (Vec3, type-checked) | 14.3 ns, 0 allocations | | 0 allocations | met |
| `ClassInfo::indexOf(text)` (hash + probe) | 10.6 ns, 0 allocations | | O(1) | met |
| `Signal::emit` with 3 slots | 12.3 ns, 0 allocations | | | |
| Arena frame, 2000 quads | 23.4 µs, 0 allocations | | 0 allocations per frame | met |
| SHA-256, 1 MB | 3.2 ms (~310 MB/s) | | | |
| UTF-8 validation, 1 MB mixed scripts | 0.66 ms (~1.5 GB/s) | | | |
| `Quat::fromEulerDegrees` | 116 ns | | | slow; see below |
| `Transform2D::map` (perspective) | 3.1 ns | | | |

### Networking (`cfw-bench-net`)

200 WebSocket clients and one server on a single event loop and thread, over loopback. Each client sends 500
binary messages of 48 bytes, and the server echoes them. Qt's figures are for the same workload on
`QWebSocketServer`/`QWebSocket`, measured by an out-of-tree program.

| | CFW | Qt 6.8.3 | Spec |
|---|---|---|---|
| Messages/s (server receives and echoes) | **205,000** | 193,000 – 207,000 | "tens of thousands" |
| Open 200 connections | 0.53 s | 1.78 s | |

The first version ran at 32,000 messages/s because it made one `send()` system call per message. Write
coalescing (one flush per socket per loop pass) fixed it; see
[decision 0010](../decisions/0010-cfw-net-design.md).

### What changed to get here

The first run parsed the scene at 0.92× Qt: inside the budget, but not by a margin that survives noise.
Profiling pointed at object construction. Every JSON object was stable-sorted, which copied the members
into a new vector, using a comparator that decoded UTF-8 one character at a time. Two changes fixed it:

- **Skip the sort for ordered input.** Files written by CFW or Qt are already in order, so the parser takes
  their member vector as it is.
- **One byte test for the UTF-16 key order.** Byte order and UTF-16 order can only disagree where a 4-byte
  UTF-8 lead meets an EE/EF lead, so no decoding is needed.

Parse went from 19.8 to 13.2 ms, and from 128k to 81k allocations. Both changes are covered by mutation
tests.

### Known slow spots (not budget items yet)

- **`Quat::fromEulerDegrees`, 116 ns.** Six float trig calls through MinGW's libm. It is fine for
  per-object updates, but worth a `sincos` or a vectorised path before the physics sync calls it per body
  per frame.
- **JSON parse: 81k allocations for 5 MB.** Most are strings longer than the 15-byte small-string buffer,
  and container growth. A pooled or arena-backed document model would cut this further, if scene load ever
  becomes a problem.

## Not yet measurable

Most budget rows need modules that do not exist yet:

- **Rendering rows** (rounded rects, text runs, image blits, editor frame): M3/M4.
- **Explorer scrolling:** M5.
- **Cold start, RSS and binary size:** these compare whole applications, so they wait for the ports.
- **Runtime RSS below 40 MB with no GUI module linked:** measurable once the Runtime builds on CFW (M1 exit).
