// StableVector, Arena/FrameArena, FrameTimer and Logger: the pieces the frame
// loop leans on. The arena checks assert the "zero allocations per frame in
// steady state" rule directly.

#include "cfw/core/Arena.h"
#include "cfw/core/Clock.h"
#include "cfw/core/CommandLine.h"
#include "cfw/core/Logger.h"
#include "cfw/core/StableVector.h"

#include <memory_resource>
#include <thread>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

void stableVectorNeverMovesElements() {
    StableVector<std::string, 4> v;
    std::vector<const std::string *> addresses;
    for (int i = 0; i < 100; ++i) {
        addresses.push_back(&v.emplace_back("item " + std::to_string(i)));
    }
    int moved = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        moved += &v[i] == addresses[i] ? 0 : 1;
    }
    checkEqual(moved, 0, "no element moved across 25 chunk allocations");
    checkEqual(v[57u], std::string("item 57"), "indexing across chunks");

    std::size_t count = 0;
    for (const std::string &s : v) {
        count += s.empty() ? 0u : 1u;
    }
    checkEqual(count, std::size_t(100), "iteration visits every element");

    StableVector<std::string, 4> taken = std::move(v);
    checkEqual(taken.size(), std::size_t(100), "move keeps the elements");
    check(v.empty(), "moved-from is empty"); // NOLINT(bugprone-use-after-move)
    check(&taken[3u] == addresses[3], "and their addresses");
}

struct Point {
    float x, y;
};

// Counts every upstream allocation, to prove the arena stops asking for memory.
class CountingResource final : public std::pmr::memory_resource {
public:
    int allocations = 0;
    int live = 0;

private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        ++live;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void *p, std::size_t bytes, std::size_t alignment) override {
        --live;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &o) const noexcept override { return this == &o; }
};

void arenaReusesMemoryAcrossFrames() {
    CountingResource upstream;
    {
        Arena arena(1024, &upstream);
        for (int frame = 0; frame < 100; ++frame) {
            arena.reset();
            for (int i = 0; i < 200; ++i) {
                Point *p = arena.make<Point>(Point{static_cast<float>(i), 0});
                (void)p;
            }
            std::pmr::vector<int> scratch(&arena);
            scratch.resize(300);
        }
        const int afterWarmup = upstream.allocations;
        for (int frame = 0; frame < 100; ++frame) {
            arena.reset();
            for (int i = 0; i < 200; ++i) {
                (void)arena.make<Point>(Point{1, 2});
            }
            std::pmr::vector<int> scratch(&arena);
            scratch.resize(300);
        }
        checkEqual(upstream.allocations, afterWarmup, "steady-state frames allocate nothing upstream");
        check(arena.highWaterMark() >= 200 * sizeof(Point), "high-water mark tracks usage");
    }
    checkEqual(upstream.live, 0, "destroying the arena returns every chunk");
}

void arenaHonoursAlignmentAndLargeRequests() {
    Arena arena(256);
    (void)arena.make<char>('x');
    struct alignas(64) Wide {
        char bytes[64];
    };
    Wide *wide = arena.make<Wide>();
    check(reinterpret_cast<std::uintptr_t>(wide) % 64 == 0, "64-byte alignment honoured");
    const Span<int> big = arena.makeArray<int>(10000); // far bigger than a chunk
    checkEqual(big.size(), std::size_t(10000), "oversized request gets its own chunk");
    checkEqual(big[9999], 0, "arrays are value-initialised");
}

void frameArenaKeepsLastFrameAlive() {
    FrameArena frames(1024);
    int *fromFrame1 = frames.current().make<int>(111);
    frames.nextFrame();
    (void)frames.current().make<int>(222);
    checkEqual(*fromFrame1, 111, "the previous frame's data survives one frame");
    check(frames.previous().bytesUsed() > 0, "previous arena still holds it");
    frames.nextFrame();
    checkEqual(frames.current().bytesUsed(), std::size_t(0), "two frames later its arena is reset");
}

void frameTimerClampsStalls() {
    FrameTimer timer;
    const TimePoint start{};
    checkEqual(timer.tick(start).delta, 0.0, "first tick has no delta");
    const FrameTime normal = timer.tick(start + std::chrono::milliseconds(16));
    checkNear(normal.delta, 0.016, 1e-9, "normal frame delta");
    const FrameTime stalled = timer.tick(start + std::chrono::seconds(10));
    checkEqual(stalled.delta, FrameTimer::kMaxDelta, "a 10-second stall is clamped");
    checkEqual(stalled.index, std::uint64_t(2), "frame index counts ticks after the first");
    const FrameTime backwards = timer.tick(start);
    checkEqual(backwards.delta, 0.0, "time going backwards gives zero, never negative");
}

void loggerFiltersAndFormats() {
    Logger logger;
    auto &memory = static_cast<MemoryLogSink &>(logger.addSink(std::make_unique<MemoryLogSink>()));
    logger.setLevel(LogLevel::Info);
    logger.setCategoryLevel("net", LogLevel::Warning);

    logger.debug("assets", "too quiet");
    logger.info("assets", "downloaded", {{"bytes", "1024"}, {"path", "My Game/a.png"}});
    logger.info("net", "filtered by category");
    logger.warning("net", "slow peer");

    checkEqual(memory.records().size(), std::size_t(2), "only enabled records reach the sink");
    const String line = formatLogRecord(memory.records()[0]);
    check(line.find("INFO  [assets] downloaded bytes=1024 path=\"My Game/a.png\"") != String::npos,
          "structured fields, quoted when they contain spaces");
    check(line.size() > 24 && line[4] == '-' && line[10] == 'T' && line[23] == 'Z', "ISO-8601 UTC timestamp");
    check(!logger.isEnabled(LogLevel::Info, "net"), "category level applies");
    check(logger.isEnabled(LogLevel::Info, "other"), "default level applies elsewhere");
}

void loggerIsThreadSafe() {
    Logger logger;
    auto &memory = static_cast<MemoryLogSink &>(logger.addSink(std::make_unique<MemoryLogSink>(100000)));
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&logger] {
            for (int i = 0; i < 500; ++i) {
                logger.info("worker", "tick");
            }
        });
    }
    for (std::thread &t : threads) {
        t.join();
    }
    checkEqual(memory.records().size(), std::size_t(2000), "no record lost under concurrent logging");
}

} // namespace

void formatsIsoUtc() {
    checkEqual(formatIsoUtc(0), String("1970-01-01T00:00:00Z"), "epoch");
    checkEqual(formatIsoUtc(951782400), String("2000-02-29T00:00:00Z"), "leap day");
    checkEqual(formatIsoUtc(1790537935), String("2026-09-27T19:38:55Z"), "a recent time");
    checkEqual(formatIsoUtc(-1), String("1969-12-31T23:59:59Z"), "before the epoch");
    checkEqual(formatIsoUtc(4107542399), String("2100-02-28T23:59:59Z"), "2100 is not a leap year");
    check(unixTimeSeconds() > 1700000000, "wall clock is plausible");
}

void parsesCommandLines() {
    CommandLine cl("Runs one Clannect Game Instance.", "1.0");
    cl.addOption("port", "Port.", "port", "7777");
    cl.addOption("scene", "Scene file.", "path");
    cl.addOption("mint-dev-ticket", "Mint a ticket.");
    const std::vector<String> args{"runtime", "--scene", "a b.json", "-port=9000", "--mint-dev-ticket", "extra",
                                   "--", "--port"};
    check(cl.parse(args).ok(), "parses");
    checkEqual(cl.value("scene"), String("a b.json"), "separate value");
    checkEqual(cl.value("port"), String("9000"), "single dash, inline value");
    check(cl.isSet("mint-dev-ticket") && !cl.helpRequested(), "flag");
    check(cl.positional() == std::vector<String>{"extra", "--port"}, "positional, and after --");

    check(cl.parse(std::vector<String>{"runtime"}).ok(), "empty parses");
    checkEqual(cl.value("port"), String("7777"), "default when not given");
    check(!cl.isSet("scene") && cl.value("scene").empty(), "unset option");

    check(!cl.parse(std::vector<String>{"runtime", "--bogus"}).ok(), "unknown option rejected");
    check(!cl.parse(std::vector<String>{"runtime", "--port"}).ok(), "missing value rejected");
    check(!cl.parse(std::vector<String>{"runtime", "--mint-dev-ticket=1"}).ok(), "value on a flag rejected");
    check(cl.parse(std::vector<String>{"runtime", "-h"}).ok() && cl.helpRequested(), "help");
    check(cl.helpText().find("--port <port>") != String::npos, "help lists options");
    check(cl.parse(std::vector<String>{"runtime", "--version"}).ok() && cl.versionRequested(), "version");
}

int main() {
    formatsIsoUtc();
    parsesCommandLines();
    stableVectorNeverMovesElements();
    arenaReusesMemoryAcrossFrames();
    arenaHonoursAlignmentAndLargeRequests();
    frameArenaKeepsLastFrameAlive();
    frameTimerClampsStalls();
    loggerFiltersAndFormats();
    loggerIsThreadSafe();
    return cfw::test::finish("RuntimeSupportTest");
}
