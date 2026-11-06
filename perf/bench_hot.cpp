// bench_hot.cpp -- perf harness for drifto's innermost per-tick functions.
//
// The lecturer's method: don't profile the whole networked game; find the
// innermost-loop hot functions, call them many times with large inputs from a
// plain main(), and profile that. This harness drives the pure game/ module
// (no GL, no DDS, no window), so `perf record` sees only simulation code.
//
//   ./bench_hot bot   [runs]  bot races: BotDriver::drive + stepCar, 600 ticks each
//   ./bench_hot world [runs]  4-car stepWorld races (bots, contacts, slipstream)
//   ./bench_hot track [runs]  Track generation: 3000 sections per run
//   ./bench_hot all   [runs]
//
// Every mode prints wall time and a checksum of the final simulation state.
// stepCar is deterministic, so an optimisation that changes the checksum has
// changed behaviour -- the checksum is how we prove before == after.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "game/include/car_physics.h"
#include "game/include/race.h"
#include "game/include/track.h"

using namespace game;

namespace {

double nowSec()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// FNV-1a over the raw bytes of a value: any bit change in any run shows up.
struct Checksum
{
    uint64_t h = 1469598103934665603ull;
    template <class T> void add(const T &v)
    {
        unsigned char b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        for (unsigned char c : b) h = (h ^ c) * 1099511628211ull;
    }
};

void addCar(Checksum &c, const CarSimState &s)
{
    c.add(s.position.x); c.add(s.position.y); c.add(s.position.z);
    c.add(s.velocity.x); c.add(s.velocity.y); c.add(s.velocity.z);
    c.add(s.distance); c.add(s.section);
}

constexpr int kTicks = 600; // 10 s of racing at 60 Hz

// One bot racing alone: the per-tick cost is dominated by BotDriver::drive's
// look-ahead (it re-simulates its own car for 25 steering plans every 5 ticks).
uint64_t benchBot(int runs)
{
    Checksum c;
    const CarParams &p = defaultCarParams();
    for (int r = 0; r < runs; ++r)
    {
        Track t(1000u + r);
        CarSimState car = spawnOnGrid(t, 0, p);
        BotDriver bot;
        for (uint32_t tick = 0; tick < kTicks; ++tick)
        {
            net::CarInput in = bot.drive(car, t, p, tick);
            stepCar(car, in, t, p);
            t.update(car.section);
        }
        addCar(c, car);
    }
    return c.h;
}

// Four bots in one world: slipstream, car-car contacts, the full stepWorld.
uint64_t benchWorld(int runs)
{
    Checksum c;
    const CarParams &p = defaultCarParams();
    RaceParams race;
    for (int r = 0; r < runs; ++r)
    {
        Track t(2000u + r);
        CarSimState cars[4];
        BotDriver bots[4];
        std::vector<WorldCar> world(4);
        for (int i = 0; i < 4; ++i)
        {
            cars[i] = spawnOnGrid(t, i, p);
            bots[i].laneBias = -1.5f + i;
            world[i].id = i + 1;
            world[i].state = &cars[i];
        }
        for (uint32_t tick = 0; tick < kTicks; ++tick)
        {
            uint32_t lead = 0;
            for (int i = 0; i < 4; ++i)
            {
                world[i].input = bots[i].drive(cars[i], t, p, tick);
                if (cars[i].section > lead) lead = cars[i].section;
            }
            stepWorld(world, t, p, race);
            t.update(lead);
        }
        for (const CarSimState &s : cars) addCar(c, s);
    }
    return c.h;
}

// Track generation with the self-intersection check (spatial hash).
uint64_t benchTrack(int runs)
{
    Checksum c;
    for (int r = 0; r < runs; ++r)
    {
        Track t(3000u + r);
        t.ensureGenerated(3000);
        const TrackSection &last = t.section(t.sectionCount() - 1);
        dm::Vec3 end = last.pointAt(last.length, 0.0f);
        c.add(end.x); c.add(end.z); c.add(t.sectionCount());
    }
    return c.h;
}

void run(const char *name, uint64_t (*fn)(int), int runs)
{
    double t0 = nowSec();
    uint64_t h = fn(runs);
    double t1 = nowSec();
    std::printf("%-6s runs=%-4d time=%8.3f s  per-run=%8.3f ms  checksum=%016llx\n", name, runs, t1 - t0,
                1000.0 * (t1 - t0) / runs, static_cast<unsigned long long>(h));
}

} // namespace

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "all";
    int runs = argc > 2 ? std::atoi(argv[2]) : 100;
    bool all = std::strcmp(mode, "all") == 0;
    if (all || !std::strcmp(mode, "bot")) run("bot", benchBot, runs);
    if (all || !std::strcmp(mode, "world")) run("world", benchWorld, runs / 4 > 0 ? runs / 4 : 1);
    if (all || !std::strcmp(mode, "track")) run("track", benchTrack, runs / 10 > 0 ? runs / 10 : 1);
    return 0;
}
