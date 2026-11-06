// audio_demo -- offline renders of every Drifto sound to build/audio_*.wav plus
// a level / spectrum report (no audio device needed).
//
//   ./audio_demo [--out build] [--assets assets]
//   ./audio_demo --analyze file.wav|ogg ...   (stats of arbitrary files)
#include "audio/include/audio.h"

#include "audio/src/ma_config.h"
#include "miniaudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using dm::Vec3;

static bool writeWav(const std::string &path, const std::vector<float> &lr, uint32_t fs)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t n = (uint32_t)lr.size();
    uint32_t dataBytes = n * 2, riff = 36 + dataBytes, fmtLen = 16, byteRate = fs * 4;
    uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtLen, 4, 1, f);
    std::fwrite(&fmt, 2, 1, f);
    std::fwrite(&ch, 2, 1, f);
    std::fwrite(&fs, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&dataBytes, 4, 1, f);
    for (float v : lr)
    {
        int s = (int)std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f);
        int16_t s16 = (int16_t)s;
        std::fwrite(&s16, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

struct Stats
{
    float peak = 0, rms = 0;
    int clipped = 0;
};

static Stats stats(const std::vector<float> &x)
{
    Stats s;
    double e = 0;
    for (float v : x)
    {
        s.peak = std::max(s.peak, std::fabs(v));
        e += v * v;
        if (std::fabs(v) >= 0.999f) ++s.clipped;
    }
    s.rms = (float)std::sqrt(e / std::max<size_t>(1, x.size()));
    return s;
}

static float db(float v) { return 20.0f * std::log10(std::max(v, 1e-9f)); }

static void fft(std::vector<std::complex<double>> &a)
{
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        double ang = -2 * 3.14159265358979323846 / len;
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k)
            {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// magnitude spectrum (power) of mono x starting at `start`, N = 32768
static std::vector<double> spectrum(const std::vector<float> &mono, size_t start, size_t N)
{
    std::vector<std::complex<double>> a(N);
    for (size_t i = 0; i < N; ++i)
    {
        double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / (N - 1));
        a[i] = mono[start + i] * w;
    }
    fft(a);
    std::vector<double> p(N / 2);
    for (size_t i = 0; i < N / 2; ++i) p[i] = std::norm(a[i]);
    return p;
}

class Renderer
{
public:
    audio::Engine eng;
    std::vector<float> out;
    uint32_t fs;
    double t = 0;
    explicit Renderer(const std::string &assets)
    {
        audio::Config c;
        c.noDevice = true;
        eng.init(assets, c);
        fs = eng.sampleRate();
        eng.setListener({0, 1.5f, 0}, {0, 0, -1}, {0, 1, 0});
    }
    // run `seconds`, calling step(t) every 1/60 s (game rate)
    void run(double seconds, const std::function<void(double)> &step)
    {
        const uint32_t tick = fs / 60;
        std::vector<float> buf(tick * 2);
        double end = t + seconds;
        while (t < end - 1e-9)
        {
            step(t);
            eng.render(buf.data(), tick);
            out.insert(out.end(), buf.begin(), buf.end());
            t += (double)tick / fs;
        }
    }
};

// rpm/throttle script: idle, then pull through gears with upshift drops,
// a limiter bounce, lift-off, back to idle.
struct SweepState
{
    float rpm, throttle;
    bool shift, limiter;
};

static SweepState sweep(double t, float idle, float redline, float dropTo)
{
    SweepState s{idle, 0.0f, false, false};
    if (t < 2.0) return s; // idle
    double u = t - 2.0;
    const double gearT[3] = {1.9, 2.6, 3.2};
    double g0 = 0;
    for (int g = 0; g < 3; ++g)
    {
        double len = gearT[g];
        if (u < g0 + len)
        {
            double p = (u - g0) / len;
            float from = g == 0 ? idle + 600 : dropTo;
            s.rpm = from + (float)std::pow(p, 0.85) * (redline - from);
            s.throttle = 1.0f;
            if (g == 2 && p > 0.82) // hold it on the limiter in 3rd
            {
                s.rpm = redline - 150 + 150 * (float)std::sin(u * 70.0);
                s.limiter = true;
            }
            return s;
        }
        g0 += len;
        if (u < g0 + 0.12) // shift: clutch in, revs drop
        {
            s.shift = u < g0 + 0.02;
            s.rpm = redline - (float)((u - g0) / 0.12) * (redline - dropTo);
            s.throttle = 0.0f;
            return s;
        }
        g0 += 0.12;
    }
    // lift off: coast down with the throttle shut (overrun pops)
    double c = u - g0;
    s.rpm = std::max(idle, redline - 1800.0f * (float)c);
    s.throttle = 0.0f;
    return s;
}

static void report(const char *name, const std::vector<float> &lr)
{
    Stats s = stats(lr);
    std::printf("  %-34s %6.2f s  peak %6.1f dBFS  rms %6.1f dBFS  clipped %d\n", name, lr.size() / 2 / 48000.0,
                db(s.peak), db(s.rms), s.clipped);
}

static void analyzeEngine(const std::string &assets, const std::string &outDir, audio::CarModel model,
                          const char *name, float rpm)
{
    Renderer r(assets);
    audio::CarAudioState c;
    c.model = model;
    c.isLocal = true;
    c.position = {0, 0.5f, -6};
    c.rpm = rpm;
    c.throttle = 1.0f;
    r.run(3.0, [&](double) { r.eng.updateCar(1, c); });
    char path[256];
    std::snprintf(path, sizeof path, "%s/audio_%s_%d.wav", outDir.c_str(), name, (int)rpm);
    writeWav(path, r.out, r.fs);
    std::vector<float> mono(r.out.size() / 2);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (r.out[2 * i] + r.out[2 * i + 1]);
    const size_t N = 65536;
    auto p = spectrum(mono, r.fs, N);
    double binHz = (double)r.fs / N;
    double fire = 2.0 * rpm / 60.0;
    // tonal energy at quarter-firing-order lines (cycle rate for a 4-stroke)
    auto lineE = [&](double f) {
        int b = (int)std::lround(f / binHz);
        double e = 0;
        for (int k = b - 3; k <= b + 3; ++k)
            if (k > 0 && k < (int)p.size()) e += p[k];
        return e;
    };
    double eFire = 0, eHalf = 0, eQuarter = 0, total = 0, centroidNum = 0;
    for (size_t i = 1; i < p.size(); ++i)
    {
        total += p[i];
        centroidNum += p[i] * i * binHz;
    }
    for (int k = 1; k <= 64; ++k)
    {
        double e = lineE(k * fire / 4.0);
        if (k % 4 == 0) eFire += e;
        else if (k % 2 == 0) eHalf += e;
        else eQuarter += e;
    }
    size_t pk = 1;
    for (size_t i = 2; i < (size_t)(4000 / binHz); ++i)
        if (p[i] > p[pk]) pk = i;
    std::printf("  %s @ %.0f rpm: firing f = %.1f Hz, strongest peak %.1f Hz (%.2f x f), centroid %.0f Hz\n", name, rpm,
                fire, pk * binHz, pk * binHz / fire, centroidNum / total);
    std::printf("    tonal energy: firing harmonics %.0f%%, half-order lines (rpm/60) %.0f%%, quarter-order "
                "lines (rpm/120) %.0f%%\n",
                100 * eFire / (eFire + eHalf + eQuarter), 100 * eHalf / (eFire + eHalf + eQuarter),
                100 * eQuarter / (eFire + eHalf + eQuarter));
    std::printf("    firing harmonics 1..8 (dB re strongest):");
    double h[9], hm = 1e-30;
    for (int k = 1; k <= 8; ++k)
    {
        h[k] = lineE(k * fire);
        hm = std::max(hm, h[k]);
    }
    for (int k = 1; k <= 8; ++k) std::printf(" %.0f", 10 * std::log10(h[k] / hm));
    std::printf("\n");
}

static int analyzeFiles(int argc, char **argv)
{
    for (int i = 2; i < argc; ++i)
    {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 48000);
        ma_decoder dec;
        if (ma_decoder_init_file(argv[i], &cfg, &dec) != MA_SUCCESS)
        {
            std::printf("%s: cannot decode\n", argv[i]);
            continue;
        }
        std::vector<float> x(48000 * 20);
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&dec, x.data(), x.size(), &got);
        ma_decoder_uninit(&dec);
        x.resize((size_t)got);
        Stats s = stats(x);
        // centroid of the first 0.5 s, attack-to--20dB time
        size_t N = 16384;
        x.resize(std::max(x.size(), N), 0.0f);
        auto p = spectrum(x, 0, N);
        double num = 0, den = 0, lo = 0;
        for (size_t k = 1; k < p.size(); ++k)
        {
            num += p[k] * k * 48000.0 / N;
            den += p[k];
            if (k * 48000.0 / N < 200) lo += p[k];
        }
        size_t last = 0;
        for (size_t k = 0; k < (size_t)got; ++k)
            if (std::fabs(x[k]) > s.peak * 0.1f) last = k;
        std::printf("%-40s %5.2f s  peak %5.1f dB  rms %5.1f dB  centroid %5.0f Hz  <200Hz %3.0f%%  decay-to--20dB "
                    "%.2f s\n",
                    argv[i], got / 48000.0, db(s.peak), db(s.rms), num / den, 100 * lo / den, last / 48000.0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--analyze") == 0) return analyzeFiles(argc, argv);
    std::string outDir = "build", assets = "assets";
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--out") == 0) outDir = argv[++i];
        else if (std::strcmp(argv[i], "--assets") == 0) assets = argv[++i];
    }
    std::printf("audio_demo: offline renders -> %s/audio_*.wav\n", outDir.c_str());
    auto save = [&](const char *name, Renderer &r) {
        std::string p = outDir + "/audio_" + name + ".wav";
        writeWav(p, r.out, r.fs);
        report(p.c_str(), r.out);
    };

    // 1/2. rev sweeps: idle -> pulls through 3 gears with upshift drops ->
    //      limiter -> lift-off coast with overrun pops -> idle
    for (int m = 0; m < 2; ++m)
    {
        Renderer r(assets);
        bool rx = m == 1;
        audio::CarAudioState c;
        c.model = rx ? audio::CarModel::RX7 : audio::CarModel::AE86;
        c.isLocal = true;
        c.position = {0, 0.5f, -6};
        float idle = rx ? 850 : 900, red = rx ? 9000 : 7600, drop = rx ? 6200 : 5100;
        r.run(16.0, [&](double t) {
            SweepState s = sweep(t, idle, red, drop);
            c.rpm = s.rpm;
            c.throttle = s.throttle;
            c.shiftEvent = s.shift;
            c.limiter = s.limiter;
            r.eng.updateCar(1, c);
        });
        save(rx ? "rx7_sweep" : "ae86_sweep", r);
    }
    // 3. skid ramp (squeal only): 0 -> 1 -> 0 over 6 s
    {
        Renderer r(assets);
        r.eng.setEngineVolume(0);
        audio::CarAudioState c;
        c.isLocal = true;
        c.position = {0, 0.5f, -6};
        r.run(6.5, [&](double t) {
            c.skid01 = (float)std::max(0.0, t < 3.0 ? t / 3.0 : 1.0 - (t - 3.0) / 3.0);
            r.eng.updateCar(1, c);
        });
        save("skid_ramp", r);
    }
    // 4. bump vs crash: bumps at 500 / 3000 / 8000, crashes at 6000 / 20000
    {
        Renderer r(assets);
        int next = 0;
        const double when[5] = {0.3, 1.3, 2.3, 3.6, 5.6};
        r.run(8.0, [&](double t) {
            while (next < 5 && t >= when[next])
            {
                Vec3 p{1.5f, 0.6f, -5};
                if (next == 0) r.eng.playBump(p, 500);
                if (next == 1) r.eng.playBump(p, 3000);
                if (next == 2) r.eng.playBump(p, 8000);
                if (next == 3) r.eng.playCrash({-2, 0.6f, -6}, 6000);
                if (next == 4) r.eng.playCrash({-2, 0.6f, -6}, 20000);
                ++next;
            }
        });
        save("bump_vs_crash", r);
    }
    // 5. nitro: AE86 at 6500 rpm, ignite at 1 s, burn for 3 s
    {
        Renderer r(assets);
        r.eng.setEngineVolume(0.35f);
        audio::CarAudioState c;
        c.isLocal = true;
        c.position = {0, 0.5f, -6};
        c.rpm = 6500;
        c.throttle = 1;
        bool fired = false;
        r.run(5.5, [&](double t) {
            c.nitro01 = (t > 1.0 && t < 4.0) ? 1.0f : 0.0f;
            c.rpm = 6500 + (float)std::max(0.0, std::min(t, 4.0) - 1.0) * 300;
            if (t > 1.0 && !fired)
            {
                r.eng.playNitroIgnite({0, 0.4f, -4});
                fired = true;
            }
            r.eng.updateCar(1, c);
        });
        save("nitro", r);
    }
    // 6. pass-by: RX7 flat out at 45 m/s passing the listener 4 m to the
    //    right (doppler, panning, distance)
    {
        Renderer r(assets);
        audio::CarAudioState c;
        c.model = audio::CarModel::RX7;
        c.rpm = 8200;
        c.throttle = 1;
        c.velocity = {0, 0, 45};
        r.run(6.0, [&](double t) {
            c.position = {4, 0.5f, -130 + 45 * (float)t};
            r.eng.updateCar(7, c);
        });
        save("rx7_passby", r);
    }
    // 7. four cars at once (CPU cost)
    {
        Renderer r(assets);
        audio::CarAudioState cs[4];
        for (int i = 0; i < 4; ++i)
        {
            cs[i].model = i & 1 ? audio::CarModel::RX7 : audio::CarModel::AE86;
            cs[i].isLocal = i == 0;
            cs[i].throttle = 1;
            cs[i].skid01 = 0.5f;
            cs[i].nitro01 = i == 2 ? 1.0f : 0.0f;
        }
        auto t0 = std::chrono::steady_clock::now();
        r.run(10.0, [&](double t) {
            for (int i = 0; i < 4; ++i)
            {
                cs[i].rpm = 4000 + 3000 * (float)std::sin(t * 0.7 + i);
                cs[i].position = {-6.0f + 4.0f * i, 0.5f, -8.0f - 5.0f * i};
                r.eng.updateCar(i, cs[i]);
            }
            if (std::fmod(t, 2.0) < 1.0 / 60) r.eng.playBump({0, 0.5f, -8}, 2500);
        });
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        save("four_cars", r);
        std::printf("  CPU: 4 cars + squeal + nitro + bumps: %.1f ms per second of audio (%.2f%% of one core)\n",
                    secs * 100.0, secs * 10.0);
    }
    std::printf("spectra (steady full throttle, local car):\n");
    analyzeEngine(assets, outDir, audio::CarModel::AE86, "ae86", 7000);
    analyzeEngine(assets, outDir, audio::CarModel::RX7, "rx7", 7000);
    analyzeEngine(assets, outDir, audio::CarModel::AE86, "ae86", 900);
    analyzeEngine(assets, outDir, audio::CarModel::RX7, "rx7", 900);
    return 0;
}
