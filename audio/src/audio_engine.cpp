// audio_engine.cpp -- audio::Engine: device (miniaudio), 3D mixer, voices.
//
// Overview:
//   * Mixer: up to kMaxCars engine voices (EngineSynth + tyre skid + nitro per car) and
//     kMaxShots pooled one-shot samples, each positioned in 3D (constant-power pan,
//     inverse-distance attenuation, Doppler pitch, distance low-pass).
//   * Data handoff: the game thread calls the public Engine API, which takes a mutex only
//     long enough to update plain data (car states, listener, event queue). The audio
//     thread copies that data once per callback and works on the copy. The only
//     lock-free item is the std::atomic CPU-load meter.
//   * Real-time design: all procedural sounds are precomputed in init(); the callback
//     uses fixed-size pools and stack buffers (apart from the small std::map/vector
//     copies of the snapshot) and flushes denormals to zero.
//   * Output stage: smoothed master gain, a fast-attack peak limiter and a soft clip.
//   * Without a device (Config::noDevice or init failure) render() can still be called
//     directly for offline rendering.
//
// Threading: the game thread only writes into `Shared` under a mutex; the
// audio thread copies it at the start of each callback and then synthesises
// without holding the lock. Synthesis runs in 64-frame blocks; spatial gains
// are ramped across each block.
#include "../include/audio.h"

#include "engine_synth.h"
#include "sfx.h"

#include "ma_config.h"
#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

// Denormal flushing is only available where SSE intrinsics exist (x86/x64).
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define DRIFTO_AUDIO_FTZ 1
#endif

namespace audio {

namespace {

// Limits: simultaneous engine voices, simultaneous one-shot sounds (impacts), and the
// mixer block length in frames (also the size of the stack scratch buffers).
constexpr int kMaxCars = 4;
constexpr int kMaxShots = 32;
constexpr int kBlock = 64;
constexpr float kSpeedOfSound = 343.0f;

// Listener (camera) pose in world space: position, forward/up unit vectors, velocity (m/s).
struct Listener
{
    dm::Vec3 pos{0, 0, 0}, fwd{0, 0, -1}, up{0, 1, 0}, vel{0, 0, 0};
};

// Per-car state held on the game-thread side. `seq` increments on every update so the
// audio thread can detect fresh data (stale cars fade out); `pendingShift` latches a
// shift event until the audio thread consumes it, so a shift between two callbacks is not lost.
struct SharedCar
{
    CarAudioState st;
    uint64_t seq = 0;
    bool pendingShift = false;
};

// One-shot events queued by the game thread and drained by the audio thread.
enum class EvType
{
    Bump,
    Crash,
    Nitro
};
struct Event
{
    EvType type;
    dm::Vec3 pos;
    float impulse;
};

// Result of 3D positioning: left/right gains, Doppler pitch ratio and a distance low-pass cutoff (Hz).
struct Spatial
{
    float gl = 0, gr = 0, rate = 1, cutoff = 18000;
};

// Computes stereo gains, Doppler rate and air-absorption cutoff for a source at `pos`
// moving at `vel`, relative to listener L. `local` (the player car) is treated as close-miked:
// no distance attenuation, no Doppler, only 30% of the lateral pan.
Spatial spatialize(const Listener &L, const dm::Vec3 &pos, const dm::Vec3 &vel, bool local)
{
    Spatial s;
    dm::Vec3 d = pos - L.pos;
    float dist = dm::length(d);
    dm::Vec3 dir = dist > 1e-3f ? d / dist : L.fwd;
    dm::Vec3 right = dm::normalize(dm::cross(L.fwd, L.up));
    // lateral: -1 (left) .. +1 (right); front: +1 ahead of the listener, -1 behind.
    float lateral = dm::dot(dir, right);
    float front = dm::dot(dir, dm::normalize(L.fwd));
    float pan, att;
    if (local)
    {
        // close-miked: the chase camera is a few metres behind the car
        pan = lateral * 0.3f;
        att = 1.0f;
        s.rate = 1.0f;
    }
    else
    {
        pan = lateral;
        const float ref = 5.0f;
        // Inverse-distance rolloff beyond a 5 m reference distance (gain 1 inside it).
        att = ref / (ref + std::max(0.0f, dist - ref) * 1.0f);
        // Doppler: vr/vs are listener and source velocity components along the line of sight;
        // ratio = (c + vr) / (c + vs), clamped so extreme speeds do not give extreme pitch.
        float vr = dm::dot(L.vel, dir), vs = dm::dot(vel, dir);
        s.rate = dm::clampf((kSpeedOfSound + vr) / (kSpeedOfSound + vs), 0.6f, 1.6f);
    }
    // Constant-power pan: map pan -1..1 to an angle 0..pi/2; cos/sin keep gl^2 + gr^2 constant
    // (sqrt(2) makes the centre gain unity per channel).
    float a = (pan + 1.0f) * 0.25f * dsp::kPi;
    s.gl = std::cos(a) * 1.41421356f * att;
    s.gr = std::sin(a) * 1.41421356f * att;
    // Air absorption approximation: cutoff falls with distance.
    s.cutoff = 18000.0f / (1.0f + (local ? 0.0f : dist) / 60.0f);
    if (front < 0 && !local) s.cutoff *= 1.0f + 0.45f * front; // behind the listener: duller
    return s;
}

// One engine voice: synthesiser plus tyre-squeal and nitro generators for a single car.
// `fade` ramps 0..1 toward `fadeTarget` so voices start and stop without clicks.
// All fields are touched only on the audio thread.
struct CarVoice
{
    int id = -1;
    bool used = false;
    EngineSynth eng;
    sfx::Skid skid;
    sfx::NitroBurn nitro;
    CarAudioState st;
    uint64_t lastSeq = 0;
    double lastChange = 0;
    float fade = 0, fadeTarget = 1;
    Spatial cur;
    dsp::OnePole lpL, lpR;
    bool fresh = true;
};

// A playing one-shot sample (impact etc.): read position in source samples, pitch ratio,
// gain and world position; spatial parameters are smoothed per block.
struct Shot
{
    const dsp::Buffer *buf = nullptr;
    double pos = 0;
    float rate = 1, gain = 1;
    dm::Vec3 p{0, 0, 0};
    bool active = false, fresh = true;
    Spatial cur;
    dsp::OnePole lp, lpR;
};

// Decodes a file to mono float at its native sample rate and peak-normalises it to 0.9.
// Returns false if missing, undecodable or empty. Runs at init time, not in the callback.
bool loadSample(const std::string &path, dsp::Buffer &out)
{
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) return false;
    ma_uint64 frames = 0;
    std::vector<float> data;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &frames) != MA_SUCCESS || frames == 0) frames = 48000 * 10;
    data.resize((size_t)frames);
    ma_uint64 got = 0;
    ma_decoder_read_pcm_frames(&dec, data.data(), frames, &got);
    out.rate = (float)dec.outputSampleRate;
    ma_decoder_uninit(&dec);
    data.resize((size_t)got);
    if (data.empty()) return false;
    float m = 1e-9f;
    for (float v : data) m = std::max(m, std::fabs(v));
    for (float &v : data) v *= 0.9f / m; // normalise
    out.data.swap(data);
    return true;
}

} // namespace

// Private implementation: owns the miniaudio device, the voice/shot pools and all state.
// Data is split into a mutex-guarded section (written by the game thread, read by the audio
// thread) and audio-thread-only state, which needs no locking.
struct Engine::Impl
{
    Config cfg;
    float fs = 48000;
    std::string assetDir;
    ma_context context;
    ma_device device;
    bool contextOk = false;
    bool deviceOk = false;
    bool initialised = false;

    // ---- shared with the game thread (guarded by m) ----
    std::mutex m;
    std::map<int, SharedCar> cars;
    Listener listener;
    std::vector<Event> events;
    float masterVol = 0.8f, engineVol = 1.0f, sfxVol = 1.0f;
    bool muted = false;

    // ---- audio thread only ----
    CarVoice voices[kMaxCars];
    Shot shots[kMaxShots];
    std::vector<Event> evLocal;
    std::map<int, SharedCar> carsLocal;
    Listener lisLocal;
    double clock = 0;
    float limGain = 1.0f;
    dsp::Smooth masterS;
    dsp::Noise rng{0xC0FFEEu};
    // Smoothed mixer CPU load, written by the audio thread and read lock-free by cpuLoad().
    std::atomic<float> load{0.0f};

    // sounds
    dsp::Buffer bump[3], crash[3], scrape[3], nitroIgn[2];
    dsp::Buffer skidLoop;
    std::vector<dsp::Buffer> recBump, recCrashMetal, recCrashCrunch, recThud;

    // Builds every procedural buffer up front (so the callback never synthesises or allocates
    // them) and then loads any recorded samples; missing files just leave procedural fallbacks.
    void loadAssets()
    {
        for (int i = 0; i < 3; ++i)
        {
            bump[i] = sfx::makeBump(fs, i);
            crash[i] = sfx::makeCrash(fs, i);
            scrape[i] = sfx::makeScrape(fs, i);
        }
        for (int i = 0; i < 2; ++i) nitroIgn[i] = sfx::makeNitroIgnite(fs, i);
        std::string d = assetDir + "/audio/";
        int loaded = 0;
        if (loadSample(d + "skid_loop.wav", skidLoop)) ++loaded;
        auto many = [&](const char *prefix, const char *ext, std::vector<dsp::Buffer> &out) {
            for (int i = 1; i <= 9; ++i)
            {
                dsp::Buffer b;
                char name[128];
                std::snprintf(name, sizeof name, "%s%d.%s", prefix, i, ext);
                if (loadSample(d + name, b))
                {
                    out.push_back(std::move(b));
                    ++loaded;
                }
            }
        };
        many("bump_", "ogg", recBump);
        many("crash_metal_", "ogg", recCrashMetal);
        many("crash_crunch_", "ogg", recCrashCrunch);
        many("thud_", "wav", recThud);
        std::fprintf(stderr, "[audio] %d recorded samples loaded from %s (procedural fallback for the rest)\n", loaded,
                     d.c_str());
    }

    // ---------------------------------------------------------------
    // Starts a one-shot in a free slot; if all slots are busy it steals the one furthest
    // through its playback. The Shot struct is reset in place, so no allocation occurs.
    void playShot(const dsp::Buffer *b, const dm::Vec3 &p, float gain, float rate)
    {
        if (!b || b->empty() || gain <= 0.001f) return;
        Shot *slot = nullptr;
        for (Shot &s : shots)
            if (!s.active)
            {
                slot = &s;
                break;
            }
        if (!slot) // steal the one closest to finishing
        {
            double best = -1;
            for (Shot &s : shots)
            {
                double prog = s.pos / std::max<size_t>(1, s.buf->data.size());
                if (prog > best)
                {
                    best = prog;
                    slot = &s;
                }
            }
        }
        *slot = Shot{};
        slot->buf = b;
        slot->p = p;
        slot->gain = gain;
        slot->rate = rate;
        slot->active = true;
    }

    // Picks a random buffer from a list of recorded variants, or nullptr when none were loaded.
    template <class V> const dsp::Buffer *pick(V &v)
    {
        if (v.empty()) return nullptr;
        return &v[rng.nextU() % v.size()];
    }

    // Converts a queued event into samples. Impulse (kg*m/s) sets gain via sqrt (perceived
    // loudness scales slowly with energy) and, for bumps, lowers pitch for harder hits.
    // Several layers (procedural + recorded) are triggered together; random rate offsets vary repeats.
    void handleEvent(const Event &e)
    {
        float r = rng.white();
        switch (e.type)
        {
        case EvType::Bump:
        {
            float g = dm::clampf(std::sqrt(std::max(0.0f, e.impulse) / 4000.0f), 0.06f, 1.2f);
            float rate = 1.12f - 0.2f * std::min(1.0f, e.impulse / 8000.0f) + 0.05f * r;
            playShot(&bump[rng.nextU() % 3], e.pos, g, rate);
            playShot(pick(recBump), e.pos, g * 0.7f, 1.0f + 0.06f * r);
            playShot(pick(recThud), e.pos, g * 0.5f * std::min(1.0f, e.impulse / 3000.0f), 0.9f + 0.05f * r);
            break;
        }
        case EvType::Crash:
        {
            float g = dm::clampf(std::sqrt(std::max(0.0f, e.impulse) / 9000.0f), 0.3f, 1.3f);
            int v = rng.nextU() % 3;
            playShot(&crash[v], e.pos, g, 1.0f + 0.05f * r);
            playShot(&scrape[v], e.pos, g * 0.55f, 0.95f + 0.1f * r);
            playShot(pick(recCrashMetal), e.pos, g * 0.65f, 0.95f + 0.05f * r);
            playShot(pick(recCrashCrunch), e.pos, g * 0.55f, 1.0f);
            playShot(pick(recThud), e.pos, g * 0.7f, 0.8f);
            break;
        }
        case EvType::Nitro:
            playShot(&nitroIgn[rng.nextU() % 2], e.pos, 0.85f, 1.0f + 0.04f * r);
            break;
        }
    }

    // Synchronises the voice pool with the snapshot of cars copied from the game thread:
    // applies fresh state, fades out removed or stale (>1 s without update) cars, frees
    // finished voices and allocates voices for new cars. When the pool is full, the local car
    // takes over the voice of the most distant non-local car.
    void assignVoices()
    {
        // drop voices whose car was removed or went stale
        for (CarVoice &v : voices)
        {
            if (!v.used) continue;
            auto it = carsLocal.find(v.id);
            if (it == carsLocal.end()) v.fadeTarget = 0;
            else
            {
                if (it->second.seq != v.lastSeq)
                {
                    v.lastSeq = it->second.seq;
                    v.lastChange = clock;
                    v.st = it->second.st;
                    if (it->second.pendingShift) v.eng.shift();
                    v.fadeTarget = std::max(0.0f, std::min(v.st.gain, 1.0f)); // game-side level / fade
                }
                if (clock - v.lastChange > 1.0) v.fadeTarget = 0;
            }
            if (v.fadeTarget == 0 && v.fade <= 0.0001f) v.used = false;
        }
        // new cars
        for (auto &kv : carsLocal)
        {
            bool have = false;
            for (CarVoice &v : voices)
                if (v.used && v.id == kv.first) have = true;
            if (have) continue;
            CarVoice *slot = nullptr;
            for (CarVoice &v : voices)
                if (!v.used)
                {
                    slot = &v;
                    break;
                }
            if (!slot && kv.second.st.isLocal)
            {
                float far = -1;
                for (CarVoice &v : voices)
                {
                    if (v.st.isLocal) continue;
                    float d = dm::length(v.st.position - lisLocal.pos);
                    if (d > far)
                    {
                        far = d;
                        slot = &v;
                    }
                }
            }
            if (!slot) continue;
            *slot = CarVoice{};
            slot->used = true;
            slot->id = kv.first;
            slot->st = kv.second.st;
            slot->lastSeq = kv.second.seq;
            slot->lastChange = clock;
            slot->fadeTarget = std::max(0.0f, std::min(kv.second.st.gain, 1.0f));
            // Per-car seed so each car has its own noise sequence.
            uint32_t seed = (uint32_t)(kv.first * 7 + 1);
            slot->eng.init(kv.second.st.model, fs, seed);
            slot->skid.init(fs, seed, &skidLoop);
            slot->nitro.init(fs, seed);
        }
    }

    // Adds a mono block into the interleaved stereo buffer with per-sample linear ramps of the
    // left/right gains from the previous block's values (cur) to the new ones (tgt), avoiding
    // zipper noise; also applies the distance low-pass. `fresh` skips the ramp for a new source.
    static void rampAdd(float *outLR, const float *mono, int n, Spatial &cur, const Spatial &tgt, dsp::OnePole &lpL,
                        dsp::OnePole &lpR, float fs, float gainMul, bool fresh)
    {
        if (fresh) cur = tgt;
        lpL.set(fs, tgt.cutoff);
        // Both channels share one cutoff coefficient but keep separate filter states.
        lpR.a = lpL.a;
        float dl = (tgt.gl - cur.gl) / n, dr = (tgt.gr - cur.gr) / n;
        float gl = cur.gl, gr = cur.gr;
        for (int i = 0; i < n; ++i)
        {
            gl += dl;
            gr += dr;
            float s = mono[i] * gainMul;
            outLR[2 * i] += lpL.run(s * gl);
            outLR[2 * i + 1] += lpR.run(s * gr);
        }
        cur.gl = tgt.gl;
        cur.gr = tgt.gr;
        cur.rate = tgt.rate;
        cur.cutoff = tgt.cutoff;
    }

    // Renders n (<= kBlock) frames of all voices and shots into out (interleaved stereo,
    // overwritten). engVol/sfxVolL are user volume scalers.
    void mixBlock(float *out, int n, float engVol, float sfxVolL)
    {
        std::fill(out, out + 2 * n, 0.0f);
        float eng[kBlock], sk[kBlock], ni[kBlock];
        for (CarVoice &v : voices)
        {
            if (!v.used) continue;
            Spatial tgt = spatialize(lisLocal, v.st.position, v.st.velocity, v.st.isLocal);
            v.eng.setTargets(v.st.rpm, v.st.throttle, v.st.limiter, tgt.rate, v.st.isLocal);
            v.eng.process(eng, n);
            v.skid.process(sk, n, v.st.skid01, tgt.rate);
            v.nitro.process(ni, n, v.st.nitro01, tgt.rate);
            // Voice fade moves linearly at a rate of one full swing per 0.3 s.
            float fadeStep = 1.0f / (0.3f * fs);
            for (int i = 0; i < n; ++i)
            {
                v.fade += v.fadeTarget > v.fade ? std::min(fadeStep, v.fadeTarget - v.fade)
                                                : -std::min(fadeStep, v.fade - v.fadeTarget);
                float localBoost = v.st.isLocal ? 1.0f : 0.85f;
                eng[i] = (eng[i] * engVol + (sk[i] * 0.55f + ni[i] * 0.5f) * sfxVolL) * v.fade * localBoost;
            }
            rampAdd(out, eng, n, v.cur, tgt, v.lpL, v.lpR, fs, 1.0f, v.fresh);
            v.fresh = false;
        }
        float tmp[kBlock];
        for (Shot &s : shots)
        {
            if (!s.active) continue;
            Spatial tgt = spatialize(lisLocal, s.p, {0, 0, 0}, false);
            const std::vector<float> &d = s.buf->data;
            // Playback increment in source samples per output sample (pitch, Doppler, sample-rate conversion).
            double step = s.rate * tgt.rate * s.buf->rate / fs;
            for (int i = 0; i < n; ++i)
            {
                size_t i0 = (size_t)s.pos;
                if (i0 + 1 >= d.size())
                {
                    tmp[i] = 0;
                    s.active = false;
                    continue;
                }
                float fr = (float)(s.pos - i0);
                // Linear interpolation between adjacent samples for fractional read positions.
                tmp[i] = d[i0] * (1 - fr) + d[i0 + 1] * fr;
                s.pos += step;
            }
            rampAdd(out, tmp, n, s.cur, tgt, s.lp, s.lpR, fs, s.gain * sfxVolL, s.fresh);
            s.fresh = false;
        }
    }

    // Audio-thread entry point (also called directly for offline rendering): snapshots the
    // shared state under the mutex, then mixes in kBlock-sized chunks without holding it,
    // and finally applies the master gain, peak limiter and soft clip. Updates the CPU load meter.
    void render(float *out, uint32_t frames)
    {
#ifdef DRIFTO_AUDIO_FTZ
        // flush denormals to zero: decaying filter tails otherwise get very slow
        // (sets the FTZ and DAZ bits)
        _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
        auto t0 = std::chrono::steady_clock::now();
        float mv, ev, sv;
        bool mute;
        {
            std::lock_guard<std::mutex> lk(m);
            // Copy under the lock so the rest of the callback works on private data.
            carsLocal = cars;
            for (auto &kv : cars) kv.second.pendingShift = false;
            lisLocal = listener;
            // Take ownership of the queued events; the game-thread queue starts empty again.
            evLocal.swap(events);
            events.clear();
            mv = masterVol;
            ev = engineVol;
            sv = sfxVol;
            mute = muted;
        }
        for (const Event &e : evLocal) handleEvent(e);
        evLocal.clear();
        assignVoices();
        uint32_t done = 0;
        while (done < frames)
        {
            int n = (int)std::min<uint32_t>(kBlock, frames - done);
            float *o = out + 2 * done;
            mixBlock(o, n, ev, sv);
            // master + limiter (peak, instant attack, 150 ms release)
            // Master gain is smoothed per block (50 ms time constant) so mute and volume changes
            // fade; g0 and g1 are the gains at the block start and end and are interpolated per sample.
            float target = mute ? 0.0f : mv;
            float peak = 0;
            for (int i = 0; i < 2 * n; ++i) peak = std::max(peak, std::fabs(o[i]));
            float g0 = masterS.v;
            float g1 = masterS.v + (target - masterS.v) * (1.0f - std::exp(-(float)n / (0.05f * fs)));
            masterS.v = g1;
            // Limiter: if the block would exceed ~0.89 full scale, reduce gain immediately (instant
            // attack); otherwise recover gain slowly.
            float want = peak * std::max(g0, g1) > 0.89f ? 0.89f / (peak * std::max(g0, g1)) : 1.0f;
            if (want < limGain) limGain = want;
            else limGain += (want - limGain) * (1.0f - std::exp(-(float)n / (0.15f * fs)));
            for (int i = 0; i < n; ++i)
            {
                float g = (g0 + (g1 - g0) * (i + 1) / n) * limGain;
                for (int c = 0; c < 2; ++c)
                {
                    float x = o[2 * i + c] * g;
                    // Soft clip above 0.95 keeps any residual overshoot below 1.0.
                    if (std::fabs(x) > 0.95f) x = (x > 0 ? 1.0f : -1.0f) * (0.95f + 0.05f * dsp::tanhf_fast((std::fabs(x) - 0.95f) * 20.0f));
                    o[2 * i + c] = x;
                }
            }
            done += n;
            clock += (double)n / fs;
        }
        // CPU load = render time / audio time, low-pass filtered (5% per call).
        float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        float audioSecs = frames / fs;
        float l = load.load();
        load.store(l + (secs / std::max(1e-6f, audioSecs) - l) * 0.05f);
    }

    // miniaudio data callback (runs on the device's audio thread): forwards to render().
    static void callback(ma_device *dev, void *output, const void *, ma_uint32 frames)
    {
        Impl *self = (Impl *)dev->pUserData;
        self->render((float *)output, frames);
    }
};

Engine::Engine() : impl_(new Impl) {}
Engine::~Engine() { shutdown(); }

// Sets up the mixer and procedural buffers, then tries to open and start a stereo float
// playback device. Returns false (and stays silent) when no device is available or when
// Config::noDevice is set; offline rendering via render() still works in that case.
bool Engine::init(const std::string &assetDir, const Config &config)
{
    Impl &I = *impl_;
    if (I.initialised) return I.deviceOk;
    I.cfg = config;
    I.fs = (float)(config.sampleRate ? config.sampleRate : 48000);
    I.assetDir = assetDir.empty() ? "assets" : assetDir;
    I.masterS.v = I.masterVol;
    I.loadAssets();
    I.initialised = true;
    if (config.noDevice) return false;

    // Under WSL the only working output is WSLg's PulseAudio server; skip
    // ALSA there (it has no cards and floods stderr) and fall back to the
    // null device. Everywhere else: miniaudio's default backend order
    // (WASAPI/DirectSound on Windows, PulseAudio/ALSA/JACK on Linux, ...).
    // Backend selection is explained above.
    const char *wsl = std::getenv("WSL_DISTRO_NAME");
    ma_backend wslBackends[] = {ma_backend_pulseaudio, ma_backend_null};
    if (ma_context_init(wsl ? wslBackends : nullptr, wsl ? 2 : 0, nullptr, &I.context) == MA_SUCCESS)
        I.contextOk = true;
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.sampleRate = (ma_uint32)I.fs;
    dc.periodSizeInMilliseconds = config.periodMs ? config.periodMs : 10;
    dc.dataCallback = &Impl::callback;
    dc.pUserData = &I;
    if (ma_device_init(I.contextOk ? &I.context : nullptr, &dc, &I.device) != MA_SUCCESS)
    {
        std::fprintf(stderr, "[audio] no audio device -- running silent\n");
        if (I.contextOk) ma_context_uninit(&I.context);
        I.contextOk = false;
        return false;
    }
    if (ma_device_start(&I.device) != MA_SUCCESS)
    {
        std::fprintf(stderr, "[audio] cannot start the audio device -- running silent\n");
        ma_device_uninit(&I.device);
        if (I.contextOk) ma_context_uninit(&I.context);
        I.contextOk = false;
        return false;
    }
    I.deviceOk = true;
    std::fprintf(stderr, "[audio] %s backend, %u Hz, device '%s'\n",
                 ma_get_backend_name(I.device.pContext->backend), I.device.sampleRate, I.device.playback.name);
    if (I.device.pContext->backend == ma_backend_null)
        std::fprintf(stderr, "[audio] only the null (silent) device is available%s\n",
                     wsl ? " -- WSLg needs PulseAudio's client library: sudo apt install libpulse0" : "");
    return true;
}

// Stops and releases the device and context; safe to call repeatedly.
void Engine::shutdown()
{
    if (!impl_) return;
    Impl &I = *impl_;
    if (I.deviceOk)
    {
        ma_device_uninit(&I.device);
        I.deviceOk = false;
    }
    if (I.contextOk)
    {
        ma_context_uninit(&I.context);
        I.contextOk = false;
    }
    I.initialised = false;
}

bool Engine::deviceActive() const { return impl_->deviceOk; }

// Public API below runs on the game thread: each call takes the mutex briefly and writes
// into the shared snapshot that the audio thread copies at the start of its next callback.
void Engine::setListener(const dm::Vec3 &position, const dm::Vec3 &forward, const dm::Vec3 &up,
                         const dm::Vec3 &velocity)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    Listener &L = impl_->listener;
    L.pos = position;
    L.fwd = dm::lengthSq(forward) > 1e-8f ? dm::normalize(forward) : dm::Vec3{0, 0, -1};
    L.up = dm::lengthSq(up) > 1e-8f ? dm::normalize(up) : dm::Vec3{0, 1, 0};
    L.vel = velocity;
}

void Engine::updateCar(int id, const CarAudioState &state)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    SharedCar &c = impl_->cars[id];
    // Latch the shift flag until the audio thread consumes it.
    bool shift = c.pendingShift || state.shiftEvent;
    c.st = state;
    c.pendingShift = shift;
    ++c.seq;
}

void Engine::removeCar(int id)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->cars.erase(id);
}

void Engine::playBump(const dm::Vec3 &position, float impulse)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->events.push_back({EvType::Bump, position, impulse});
}

void Engine::playCrash(const dm::Vec3 &position, float impulse)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->events.push_back({EvType::Crash, position, impulse});
}

void Engine::playNitroIgnite(const dm::Vec3 &position)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->events.push_back({EvType::Nitro, position, 1.0f});
}

void Engine::setMasterVolume(float v)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->masterVol = dm::clampf(v, 0.0f, 1.0f);
}
void Engine::setEngineVolume(float v)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->engineVol = dm::clampf(v, 0.0f, 2.0f);
}
void Engine::setSfxVolume(float v)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->sfxVol = dm::clampf(v, 0.0f, 2.0f);
}
void Engine::setMuted(bool muted)
{
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->muted = muted;
}

// Offline render. Returns silence when a device thread owns the mixer (calling the mixer
// from two threads would race on audio-thread-only state) or before init().
void Engine::render(float *interleavedStereo, uint32_t frames)
{
    if (!interleavedStereo) return;
    if (impl_->deviceOk || !impl_->initialised) // the device thread owns the mixer
    {
        std::fill(interleavedStereo, interleavedStereo + 2 * (size_t)frames, 0.0f);
        return;
    }
    impl_->render(interleavedStereo, frames);
}

// Accessors: sampleRate returns the configured rate; cpuLoad reads the audio thread's atomic meter.
uint32_t Engine::sampleRate() const { return (uint32_t)impl_->fs; }
float Engine::cpuLoad() const { return impl_->load.load(); }

} // namespace audio
