// bot_bench.cpp -- measures race drivers on many seeds (headless, no GL/DDS).
//   make bot_bench && ./bot_bench [seeds=20] [seconds=90]
// Scenarios:
//   solo    one car alone on the track
//   pack    four cars of the same kind racing (stepWorld: contacts, slipstream)
//   duel    elimination races, 2 old bots vs 2 new bots (grid alternates)
//   mix     elimination races between the four presets
//   sweep   prototype RaceParams::wallTouchWrecks at several thresholds,
//           for the bots and a "human-like" driver (autopilot + lag + noise)
// Reported per car-minute: wall wrecks, all deaths (wrecks + takedowns +
// off-road/flip), takedowns suffered, metres of track progress; average
// survival time between deaths.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "game/bench/old_bot.h"
#include "game/include/autopilot.h"
#include "game/include/bot_driver.h"
#include "game/include/race.h"
#include "game/include/rng.h"

using namespace game;

namespace {

enum class Kind { Old, Rookie, Racer, Bully, Ace, Human, HumanGood };
const char *kindName(Kind k)
{
    switch (k)
    {
    case Kind::Old: return "old";
    case Kind::Rookie: return "Rookie";
    case Kind::Racer: return "Racer";
    case Kind::Bully: return "Bully";
    case Kind::Ace: return "Ace";
    case Kind::Human: return "human~";
    case Kind::HumanGood: return "human+";
    }
    return "?";
}

struct Driver
{
    Kind kind;
    bench::OldBot old;
    BotDriver bot;
    // human model
    Pcg32 rng;
    float lagBuf[32] = {};
    uint32_t n = 0;
    int jolt = 0;
    float joltSteer = 0;

    Driver(Kind k, uint32_t seed) : kind(k), rng(seed, 77)
    {
        BotPreset p = BotPreset::Racer;
        if (k == Kind::Rookie) p = BotPreset::Rookie;
        if (k == Kind::Bully) p = BotPreset::Bully;
        if (k == Kind::Ace) p = BotPreset::Ace;
        bot.profile = botPreset(p);
        bot.seed = seed;
    }

    net::CarInput drive(const CarSimState &s, const Track &t, uint32_t tick, const std::vector<BotOpponent> &others)
    {
        switch (kind)
        {
        case Kind::Old: return old.drive(s, t, defaultCarParams(), tick);
        case Kind::Human:
        case Kind::HumanGood:
        {
            // Keyboard player: autopilot intent, digital steering, reaction
            // lag, and an occasional wrong / late input.
            bool good = kind == Kind::HumanGood;
            AutopilotParams ap;
            ap.digital = !good;
            ap.nitroMinGauge = 0.5f;
            net::CarInput in = autopilotInput(s, t, ap, tick);
            int lag = good ? 6 : 12;
            lagBuf[n % 32] = in.steer;
            in.steer = lagBuf[(n + 32 - lag) % 32];
            ++n;
            if (jolt > 0) { --jolt; in.steer = joltSteer; }
            else if (rng.nextFloat() < (good ? 0.004f : 0.01f))
            {
                jolt = good ? 8 : 15;
                joltSteer = rng.nextFloat() < 0.5f ? -1.0f : 1.0f;
            }
            return in;
        }
        default: return bot.drive(s, t, defaultCarParams(), tick, others);
        }
    }
};

struct Stats
{
    double carSeconds = 0, aliveSeconds = 0, metres = 0;
    int wallWrecks = 0, takedownsSuffered = 0, otherDeaths = 0, takedownsDealt = 0, wallTouches = 0;
    int contacts = 0, hardContacts = 0;
    void add(const Stats &o)
    {
        carSeconds += o.carSeconds; aliveSeconds += o.aliveSeconds; metres += o.metres;
        wallWrecks += o.wallWrecks; takedownsSuffered += o.takedownsSuffered; otherDeaths += o.otherDeaths;
        takedownsDealt += o.takedownsDealt; wallTouches += o.wallTouches; contacts += o.contacts;
        hardContacts += o.hardContacts;
    }
    int deaths() const { return wallWrecks + takedownsSuffered + otherDeaths; }
    void print(const char *label) const
    {
        double min = carSeconds / 60.0;
        std::printf("  %-28s wallWrecks/min %5.2f  deaths/min %5.2f  takedownsSuffered/min %4.2f  dealt/min %4.2f  "
                    "m/min %6.0f  survival %5.1f s  wallTouches/min %5.1f\n",
                    label, wallWrecks / min, deaths() / min, takedownsSuffered / min, takedownsDealt / min, metres / min,
                    aliveSeconds / std::max(1, deaths()), wallTouches / min);
    }
};

// Closing-speed histogram of car contacts (from impactCar ~ (1+e)/2 * closing).
std::vector<int> g_closingHist(40, 0);

struct Result
{
    std::vector<Stats> perCar;
    int winner = -1; // index
    std::vector<int> order;
};

Result runRace(const std::vector<Kind> &kinds, uint32_t seed, float seconds, bool elimination, const RaceParams &race)
{
    Track t(seed);
    const CarParams &P = defaultCarParams();
    size_t n = kinds.size();
    std::vector<CarSimState> cars;
    std::vector<std::unique_ptr<Driver>> drivers;
    for (size_t i = 0; i < n; ++i)
    {
        cars.push_back(spawnOnGrid(t, static_cast<int>(i)));
        drivers.emplace_back(new Driver(kinds[i], seed * 131u + static_cast<uint32_t>(i)));
        drivers.back()->bot.laneBias = (static_cast<float>(i % 2) - 0.5f) * 0.8f;
        drivers.back()->bot.takedownSpeed = race.takedownSpeed;
    }
    Result res;
    res.perCar.resize(n);
    EliminationRace er(race);
    std::vector<int32_t> ids;
    for (size_t i = 0; i < n; ++i) ids.push_back(static_cast<int32_t>(i + 1));
    if (elimination) er.start(ids);
    std::vector<float> lastDist(n);
    for (size_t i = 0; i < n; ++i) lastDist[i] = cars[i].distance;
    const int ticks = static_cast<int>(seconds * 60);
    for (int k = 0; k < ticks; ++k)
    {
        uint32_t maxSec = 0;
        for (size_t i = 0; i < n; ++i) maxSec = std::max(maxSec, cars[i].section);
        t.ensureGenerated(maxSec + 14);
        t.update(maxSec);
        std::vector<BotOpponent> field;
        for (size_t i = 0; i < n; ++i)
            if (!(elimination && er.isEliminated(ids[i]))) field.push_back({ids[i], &cars[i]});
        std::vector<WorldCar> w;
        std::vector<CarSimState> before = cars;
        for (size_t i = 0; i < n; ++i)
        {
            if (elimination && er.isEliminated(ids[i])) continue;
            if (elimination) cars[i].nitroRechargeScale = er.rechargeMultiplierFor(ids[i]);
            w.push_back({ids[i], &cars[i], drivers[i]->drive(cars[i], t, static_cast<uint32_t>(k), field), false});
        }
        if (const char *tr = std::getenv("BENCH_TRACE")) // seed:t0:t1 -> per-tick trace of car 0
        {
            unsigned ts = 0;
            float t0 = 0, t1 = 0;
            if (std::sscanf(tr, "%u:%f:%f", &ts, &t0, &t1) == 3 && ts == seed && k >= t0 * 60 && k <= t1 * 60 && k % 3 == 0)
            {
                const CarSimState &c = cars[0];
                const BotDriver::Debug &d = drivers[0]->bot.debug;
                const TrackSection &sec = t.section(c.section);
                dm::Vec3 h = dm::forwardOf(c.rotation);
                std::printf("    t=%.2f %s R%.0f k%+.3f s%.1f lat %+.2f v %.1f vT %.1f drift %+.0f steerIn %+.2f steer %+.2f safe %d "
                            "cont %.1f path %.1f nose(%+.2f,%+.2f) head(%+.2f,%+.2f) line %+.1f nitro %d\n",
                            k / 60.0, featureName(sec.feature), sec.radius, sec.curvature(), c.sectionS, c.lateral, c.speed,
                            d.targetSpeed, c.driftAngle, w[0].input.steer, c.steer, d.planned, dm::length(d.containment),
                            dm::length(d.path), d.desiredHeading.x, d.desiredHeading.z, h.x, h.z, d.lineOffset, d.nitro);
            }
        }
        stepWorld(w, t, P, race);
        std::vector<EliminationRace::Progress> prog;
        for (size_t i = 0; i < n; ++i)
        {
            if (elimination && er.isEliminated(ids[i])) continue;
            Stats &st = res.perCar[i];
            const CarSimState &b = before[i], &a = cars[i];
            st.carSeconds += kFixedDt;
            if (a.alive) st.aliveSeconds += kFixedDt;
            if (b.alive && !a.alive)
            {
                if (a.takedownEvent == 2) ++st.takedownsSuffered;
                else if (a.impactWall > (race.wallTouchWrecks ? race.wallTouchSpeed : P.crashImpactSpeed))
                {
                    ++st.wallWrecks;
                    if (std::getenv("BENCH_DEBUG"))
                    {
                        const BotDriver::Debug &d = drivers[i]->bot.debug;
                        const TrackSection &sec = t.section(b.section);
                        std::printf("    WRECK %s seed %u t=%.1fs %s R=%.0f speed %.1f (vT %.1f) lat %.1f drift %.0f safety %d steer %.2f contain %.1f\n",
                                    kindName(kinds[i]), seed, k / 60.0, featureName(sec.feature), sec.radius, b.speed,
                                    d.targetSpeed, b.lateral, b.driftAngle, d.planned, b.steer, dm::length(d.containment));
                    }
                }
                else ++st.otherDeaths;
            }
            if (a.takedownEvent == 1)
            {
                ++st.takedownsDealt;
                if (std::getenv("BENCH_DEBUG"))
                {
                    const BotDriver::Debug &d = drivers[i]->bot.debug;
                    std::printf("    TAKEDOWN by %s#%zu seed %u t=%.1fs speed %.1f prey %d overtaking %d impact %.1f %s\n",
                                kindName(kinds[i]), i, seed, k / 60.0, b.speed, d.pursuitTarget, d.overtaking, a.impactCar,
                                featureName(t.section(b.section).feature));
                    for (size_t j = 0; j < n; ++j)
                        if (cars[j].takedownEvent == 2)
                            std::printf("        victim #%zu speed %.1f ghost(before) %.2f lat %.1f vs attacker lat %.1f drift %.0f\n", j,
                                        before[j].speed, before[j].ghostTimer, before[j].lateral, b.lateral, before[j].driftAngle);
                }
            }
            if (a.impactWall > 1.0f && b.impactWall <= 1.0f) ++st.wallTouches;
            if (a.impactCar > 0.15f)
            {
                ++st.contacts;
                float closing = a.impactCar * 2.0f / (1.0f + race.restitution);
                g_closingHist[std::min(39, static_cast<int>(closing))]++;
            }
            // Progress: centreline distance gained (a respawn backs off a little: counted).
            st.metres += a.distance - lastDist[i];
            lastDist[i] = a.distance;
            prog.push_back({ids[i], a.distance});
        }
        if (elimination)
        {
            er.update(prog, kFixedDt);
            if (er.finished()) break;
        }
    }
    if (elimination)
    {
        if (!er.finished()) er.finishByLeader();
        for (int32_t id : er.finishingOrder()) res.order.push_back(id - 1);
        res.winner = er.winnerId() - 1;
    }
    return res;
}

} // namespace

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? std::atoi(argv[1]) : 20;
    float seconds = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 90.0f;
    const char *only = argc > 3 ? argv[3] : nullptr;
    auto want = [only](const char *s) { return !only || std::strstr(only, s); };
    RaceParams race = defaultRaceParams();
    if (std::getenv("LEGACY_CONTACT")) // the pre-change contact model: no takedowns, symmetric hits
    {
        race.takedowns = false;
        race.ramBias = 0.0f;
        race.restitution = 0.3f;
        race.friction = 0.35f;
        race.spinScale = 1.4f;
    }
    std::printf("bot_bench: %d seeds x %.0f s\n", seeds, seconds);
    if (want("feel"))
    {
        // Contact feel: a sideswipe (A slides into B at 6 m/s, both at 18 m/s,
        // no steering afterwards) and a 7 m/s rear-end (no takedown), 1 s on.
        TrackParams tp;
        for (float &w : tp.weight) w = 0;
        tp.weight[static_cast<int>(FeatureKind::Straight)] = 1;
        for (float &w : tp.profileWeight) w = 0;
        tp.profileWeight[0] = 1;
        for (float &w : tp.styleWeight) w = 0;
        tp.styleWeight[0] = 1;
        Track t(14, tp);
        RaceParams legacy = defaultRaceParams();
        legacy.takedowns = false;
        legacy.ramBias = 0;
        legacy.restitution = 0.3f;
        legacy.friction = 0.35f;
        legacy.spinScale = 1.4f;
        std::printf("\nfeel: contact response (1 s after the hit, no steering)\n");
        for (int which = 0; which < 2; ++which)
        {
            RaceParams r = which ? defaultRaceParams() : legacy;
            if (which && std::getenv("FEEL_F")) r.friction = std::atof(std::getenv("FEEL_F"));
            if (which && std::getenv("FEEL_S")) r.spinScale = std::atof(std::getenv("FEEL_S"));
            if (which && std::getenv("FEEL_E")) r.restitution = std::atof(std::getenv("FEEL_E"));
            const float k = kCarScale;
            CarSimState a = spawnCar(t, 1, 10.0f, -0.62f * k, 18.0f), b = spawnCar(t, 1, 10.0f, 0.62f * k, 18.0f);
            a.velocity += t.section(1).rightAt(10.0f) * 6.0f;
            float a0 = a.lateral, b0 = b.lateral, spinA = 0, spinB = 0;
            for (int i = 0; i < 60; ++i)
            {
                std::vector<WorldCar> w{{1, &a, driveInput(i, 0, false), false}, {2, &b, driveInput(i, 0, false), false}};
                stepWorld(w, t, defaultCarParams(), r);
                spinA = std::fmax(spinA, std::fabs(a.spinRate));
                spinB = std::fmax(spinB, std::fabs(b.spinRate));
            }
            std::printf("  %-8s sideswipe: hitter drifted %+.2f m sideways (spin %.2f rad/s), hit car pushed %+.2f m (spin %.2f), "
                        "contact ticks:",
                        which ? "new" : "legacy", a.lateral - a0, spinA, b.lateral - b0, spinB);
            // rear-end
            CarSimState c = spawnCar(t, 1, 20.0f, 0.4f, 13.0f), d = spawnCar(t, 1, 20.0f - 3.2f, 0.0f, 20.0f);
            int touching = 0;
            float d0 = d.speed;
            for (int i = 0; i < 60; ++i)
            {
                std::vector<WorldCar> w{{1, &c, driveInput(i, 0, false), false}, {2, &d, driveInput(i, 0, false), false}};
                stepWorld(w, t, defaultCarParams(), r);
                touching += d.impactCar > 0;
            }
            std::printf(" %d; rear-end 20->13 m/s: rammer speed %.1f -> %.1f, rammer lateral drift %+.2f m, front car spin-out yaw %.0f deg\n",
                        touching, d0, d.speed, d.lateral, std::fabs(c.driftAngle));
            // Passing bump: A (21 m/s) noses into B's rear quarter (16 m/s) at 3 m/s sideways.
            CarSimState e = spawnCar(t, 1, 30.0f - 2.2f, -1.25f, 21.0f), f = spawnCar(t, 1, 30.0f, 0.0f, 16.0f);
            e.velocity += t.section(1).rightAt(30.0f) * 3.0f;
            dm::Vec3 fwd = dm::forwardOf(e.rotation);
            float yawE = 0, yawF = 0, e0 = dm::dot(e.velocity, fwd);
            int contact = 0;
            for (int i = 0; i < 60; ++i)
            {
                std::vector<WorldCar> w{{1, &e, driveInput(i, 0, false), false}, {2, &f, driveInput(i, 0, false), false}};
                stepWorld(w, t, defaultCarParams(), r);
                contact += e.impactCar > 0;
                yawE = std::fmax(yawE, std::fabs(std::acos(dm::clampf(dm::dot(dm::forwardOf(e.rotation), fwd), -1, 1))));
                yawF = std::fmax(yawF, std::fabs(std::acos(dm::clampf(dm::dot(dm::forwardOf(f.rotation), fwd), -1, 1))));
            }
            std::printf("           passing bump 21 vs 16 m/s: passer yaw kick %.0f deg, forward %.1f -> %.1f m/s; bumped car yaw %.0f deg; contact ticks %d\n",
                        dm::degrees(yawE), e0, dm::dot(e.velocity, fwd), dm::degrees(yawF), contact);
        }
    }

    const Kind all[] = {Kind::Old, Kind::Rookie, Kind::Racer, Kind::Bully, Kind::Ace, Kind::Human, Kind::HumanGood};
    if (want("solo"))
    {
        std::printf("\nsolo (one car):\n");
        for (Kind k : all) if (!std::getenv("BENCH_KIND") || std::strcmp(std::getenv("BENCH_KIND"), kindName(k)) == 0)
        {
            Stats s;
            for (int i = 0; i < seeds; ++i) s.add(runRace({k}, 1000 + i * 17, seconds, false, race).perCar[0]);
            s.print(kindName(k));
        }
    }
    if (want("pack"))
    {
        std::printf("\npack (4 cars of one kind, contacts on):\n");
        for (Kind k : all) if (!std::getenv("BENCH_KIND") || std::strcmp(std::getenv("BENCH_KIND"), kindName(k)) == 0)
        {
            Stats s;
            std::fill(g_closingHist.begin(), g_closingHist.end(), 0);
            for (int i = 0; i < seeds; ++i)
                for (const Stats &c : runRace({k, k, k, k}, 2000 + i * 17, seconds, false, race).perCar) s.add(c);
            s.print(kindName(k));
            if (k == Kind::Old || k == Kind::Racer)
            {
                int tot = 0;
                for (int v : g_closingHist) tot += v;
                std::printf("      contact-tick closing speed histogram (m/s: share):");
                for (int b = 0; b < 20; b += 2)
                    std::printf(" %d-%d:%.1f%%", b, b + 2, 100.0 * (g_closingHist[b] + g_closingHist[b + 1]) / std::max(1, tot));
                int over = 0;
                for (int b = 20; b < 40; ++b) over += g_closingHist[b];
                std::printf(" 20+:%.1f%%\n", 100.0 * over / std::max(1, tot));
            }
        }
    }
    if (want("duel"))
    {
        std::printf("\nduel: elimination races, 2 old vs 2 new (Racer preset, then Ace)\n");
        for (Kind nk : {Kind::Racer, Kind::Ace})
        {
            int newWins = 0, races = 0;
            double newPlaceSum = 0, oldPlaceSum = 0;
            Stats so, sn;
            for (int i = 0; i < seeds; ++i)
            {
                std::vector<Kind> kinds = (i % 2) ? std::vector<Kind>{Kind::Old, nk, Kind::Old, nk}
                                                  : std::vector<Kind>{nk, Kind::Old, nk, Kind::Old};
                Result r = runRace(kinds, 3000 + i * 17, 400.0f, true, race);
                ++races;
                newWins += kinds[r.winner] != Kind::Old;
                for (size_t p = 0; p < r.order.size(); ++p)
                    (kinds[r.order[p]] == Kind::Old ? oldPlaceSum : newPlaceSum) += static_cast<double>(p + 1);
                for (size_t c = 0; c < kinds.size(); ++c) (kinds[c] == Kind::Old ? so : sn).add(r.perCar[c]);
            }
            std::printf("  new=%s: new bots win %d/%d (%.0f%%), avg place new %.2f vs old %.2f\n", kindName(nk), newWins,
                        races, 100.0 * newWins / races, newPlaceSum / (2 * races), oldPlaceSum / (2 * races));
            so.print("    old in duel");
            sn.print("    new in duel");
        }
    }
    if (want("pair"))
    {
        std::printf("\npair: second car starts behind a Rookie (can it get past?)\n");
        for (Kind k : {Kind::Old, Kind::Racer, Kind::Bully, Kind::Ace})
        {
            Stats s;
            for (int i = 0; i < seeds; ++i) s.add(runRace({Kind::Rookie, k}, 7000 + i * 17, seconds, false, race).perCar[1]);
            s.print(kindName(k));
        }
    }
    if (want("mix"))
    {
        std::printf("\nmix: elimination races between the presets (grid rotates)\n");
        const Kind presets[4] = {Kind::Rookie, Kind::Racer, Kind::Bully, Kind::Ace};
        int wins[4] = {};
        double place[4] = {};
        Stats st[4];
        for (int i = 0; i < seeds; ++i)
        {
            std::vector<Kind> kinds;
            for (int j = 0; j < 4; ++j) kinds.push_back(presets[(i + j) % 4]);
            Result r = runRace(kinds, 4000 + i * 17, 400.0f, true, race);
            for (int j = 0; j < 4; ++j)
            {
                int pi = (i + j) % 4;
                if (r.winner == j) ++wins[pi];
                st[pi].add(r.perCar[j]);
            }
            for (size_t p = 0; p < r.order.size(); ++p) place[(i + r.order[p]) % 4] += static_cast<double>(p + 1);
        }
        for (int j = 0; j < 4; ++j)
        {
            char label[64];
            std::snprintf(label, sizeof label, "%s wins %d/%d place %.2f", kindName(presets[j]), wins[j], seeds, place[j] / seeds);
            st[j].print(label);
        }
    }
    if (want("sweep"))
    {
        std::printf("\nsweep: RaceParams::wallTouchWrecks threshold (wall impact m/s that wrecks)\n");
        for (float thr : {0.0f, 8.0f, 4.0f, 2.0f, 1.0f})
        {
            RaceParams r = race;
            r.wallTouchWrecks = thr > 0;
            r.wallTouchSpeed = thr;
            std::printf(" threshold %s:\n", thr > 0 ? std::to_string(static_cast<int>(thr)).c_str() : "off (14)");
            for (Kind k : {Kind::Racer, Kind::Ace, Kind::Rookie, Kind::HumanGood, Kind::Human})
            {
                Stats s;
                for (int i = 0; i < seeds; ++i) s.add(runRace({k}, 5000 + i * 17, seconds, false, r).perCar[0]);
                char label[64];
                std::snprintf(label, sizeof label, "solo %s", kindName(k));
                s.print(label);
            }
            Stats p;
            for (int i = 0; i < seeds; ++i)
                for (const Stats &c : runRace({Kind::Racer, Kind::Bully, Kind::Ace, Kind::Racer}, 6000 + i * 17, seconds, false, r).perCar)
                    p.add(c);
            p.print("pack Racer/Bully/Ace/Racer");
        }
    }
    return 0;
}
