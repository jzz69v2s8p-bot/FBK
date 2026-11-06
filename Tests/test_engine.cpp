// Testet die Engine mit synthetischen Signalen (ohne Plugin-Host).
// Bauen:  g++ -O2 -std=c++17 -I../Source test_engine.cpp -o test_engine && ./test_engine
#include "FeedbackEngine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

static constexpr double SR = 48000.0;

struct Speech
{
    std::mt19937 rng { 1234 };
    std::normal_distribution<double> nd { 0.0, 1.0 };
    double phase[40] = {};
    double t = 0;
    bool monotone;
    explicit Speech (bool mono) : monotone (mono) {}

    double next()
    {
        const double dt = 1.0 / SR;
        t += dt;
        const double syl = std::pow (std::max (0.0, std::sin (2 * fg::kPi * 3.3 * t)), 2.0);
        const double f0 = monotone ? 140.0 : 130.0 + 45.0 * std::sin (2 * fg::kPi * 0.37 * t) + 15.0 * std::sin (2 * fg::kPi * 1.9 * t);
        double s = 0;
        for (int h = 1; h <= 36; ++h)
        {
            const double f = f0 * h;
            if (f > 9000) break;
            phase[h] += 2 * fg::kPi * f * dt;
            // grobe Formanten bei 600 / 1700 / 2900 Hz
            const double form = 1.0 + 3.0 * std::exp (-std::pow ((f - 600) / 250, 2)) + 2.0 * std::exp (-std::pow ((f - 1700) / 300, 2))
                                    + 1.5 * std::exp (-std::pow ((f - 2900) / 400, 2));
            s += std::sin (phase[h]) * form / std::pow (h, 1.1);
        }
        const double noise = nd (rng) * 1.0e-4;                   // ~ -80 dBFS Rauschboden
        const double sib   = (std::sin (2 * fg::kPi * 0.8 * t) > 0.95) ? nd (rng) * 0.01 : 0.0; // Zischlaut
        return s * syl * 0.035 + noise + sib;
    }
};

static double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-9)); }

// ---------------------------------------------------------------------------
static int testSpeech (bool monotone)
{
    fg::ChannelEngine e;
    e.prepare (SR);
    fg::Settings s;
    e.setSettings (s);
    Speech sp (monotone);

    int blocks = (int) (60 * SR / 128), events = 0;
    std::vector<float> buf (128);
    for (int b = 0; b < blocks; ++b)
    {
        for (auto& x : buf) x = (float) sp.next();
        e.process (buf.data(), 128);
        fg::Event ev;
        while (e.popEvent (ev))
        {
            ++events;
            std::printf ("   Fehlalarm bei %.1f s: %.0f Hz, +%.1f dB, %.1f dB/s, Regel %d\n",
                         b * 128 / SR, ev.freq, ev.prominenceDb, ev.slopeDbPerS, ev.rule);
        }
    }
    std::printf ("[Sprache %s, 60 s]  Notches gesetzt: %d   %s\n", monotone ? "monoton" : "variabel", events, events == 0 ? "OK" : "PROBLEM");
    return events;
}

// ---------------------------------------------------------------------------
// Geschlossener Regelkreis: Mikro -> Engine -> (Verzögerung + Raum-Resonanz) -> Mikro
// Die Verzögerung beträgt 480 Samples (10 ms); fr ist ein Vielfaches von 100 Hz und fällt damit
// genau auf einen Kammfilter-Zahn, an dem die Schleifenverstärkung "loopGain" beträgt.
struct LoopResult { double firstRealS = -1; double maxToneDb = -200; double finalToneDb = -200; int real = 0, falseAlarms = 0; int notches = 0; double notchHz = 0; };

static double bandMaxDb (const std::vector<float>& x, double f0, double f1)
{
    const int N = 4096;
    static fg::FFT fft; static bool init = false;
    if (! init) { fft.init (N); init = true; }
    std::vector<float> re (N), im (N, 0.0f);
    for (int i = 0; i < N; ++i)
        re[(size_t) i] = x[x.size() - N + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2 * fg::kPi * i / N));
    fft.forward (re.data(), im.data());
    double m = -200;
    for (int k = 1; k < N / 2; ++k)
    {
        const double f = k * SR / N;
        if (f < f0 || f > f1) continue;
        const double p = ((double) re[(size_t) k] * re[(size_t) k] + (double) im[(size_t) k] * im[(size_t) k]) * 16.0 / ((double) N * N);
        m = std::max (m, 10.0 * std::log10 (p + 1.0e-20));
    }
    return m;
}

static LoopResult testLoop (double loopGain, double fr, bool withSpeech, bool monoSpeech, const char* name, bool monitor = false, double T = 40.0)
{
    fg::ChannelEngine e;
    e.prepare (SR);
    fg::Settings s;
    s.monitorOnly = monitor;
    e.setSettings (s);

    const int delay = 480;
    const double g0 = 0.3;
    fg::Biquad room;
    room.setPeaking (fr, toDb (loopGain / g0), 15.0, SR);

    std::vector<float> dline ((size_t) delay, 0.0f);
    int dpos = 0;
    Speech sp (monoSpeech);
    std::mt19937 rng (7);
    std::normal_distribution<double> nd (0.0, 1.0);

    LoopResult r;
    const double loopStart = 2.0;
    const int total = (int) (T * SR / 128);
    std::vector<float> buf (128);
    std::vector<float> outHist ((size_t) SR, 0.0f);     // letzte Sekunde Ausgang
    size_t oh = 0;

    for (int b = 0; b < total; ++b)
    {
        const double tNow = b * 128 / SR;
        for (int i = 0; i < 128; ++i)
        {
            double x = (withSpeech ? sp.next() : 0.0) + nd (rng) * 3.0e-4;       // Raumrauschen ~ -70 dBFS
            if (tNow >= loopStart) x += dline[(size_t) dpos];
            buf[(size_t) i] = (float) std::clamp (x, -1.0, 1.0);
            dpos = (dpos + 1) % delay;
        }
        e.process (buf.data(), 128);
        const int start = (dpos - 128 + delay) % delay;
        for (int i = 0; i < 128; ++i)
            dline[(size_t) ((start + i) % delay)] = (float) room.process (buf[(size_t) i] * g0);

        for (float v : buf) { outHist[oh] = v; oh = (oh + 1) % outHist.size(); }

        if (b % 64 == 0 && tNow > loopStart + 0.5)
        {
            std::vector<float> lin (outHist.size());
            for (size_t i = 0; i < lin.size(); ++i) lin[i] = outHist[(oh + i) % outHist.size()];
            const double tone = bandMaxDb (lin, fr - 120.0, fr + 120.0);
            r.maxToneDb = std::max (r.maxToneDb, tone);
            if (tNow > T - 1.5) r.finalToneDb = std::max (r.finalToneDb, tone);
        }

        fg::Event ev;
        while (e.popEvent (ev))
        {
            const bool real = std::fabs (ev.freq - fr) < 100.0;
            if (real) { ++r.real; if (r.firstRealS < 0) r.firstRealS = tNow - loopStart; } else ++r.falseAlarms;
            std::printf ("   t=%5.2f s  %s %s %5.0f Hz  +%.1f dB  Anstieg %.1f dB/s  Regel %d  Tiefe %.0f dB\n",
                         tNow, real ? "ECHT  " : "FALSCH", ev.deepened ? "vertieft" : "neu     ", ev.freq, ev.prominenceDb, ev.slopeDbPerS, ev.rule, ev.depthDb);
        }
    }
    r.notches = e.getActiveNotchCount();
    for (auto& n : e.getNotchInfo()) if (std::fabs (n.freq - fr) < 100.0) r.notchHz = n.freq;
    std::printf ("[%s]\n   erste echte Erkennung nach %.2f s | Ton-Pegel max %.1f dBFS, am Ende %.1f dBFS | echte Events %d, Fehlalarme %d | Notch bei %.1f Hz\n",
                 name, r.firstRealS, r.maxToneDb, r.finalToneDb, r.real, r.falseAlarms, r.notchHz);
    return r;
}

int main()
{
    int fails = 0;

    std::printf ("=== 1) Sprache darf nichts auslösen ===\n");
    fails += testSpeech (false) > 0;
    fails += testSpeech (true)  > 0;

    std::printf ("\n=== 2) Feedback im geschlossenen Regelkreis (Verstaerkung 1.03 = ca. 25 dB/s, 1.004 = ca. 3 dB/s) ===\n");
    auto a = testLoop (1.03,  2900.0, true,  false, "schnell (~25 dB/s) + Sprache, 2900 Hz");
    auto b = testLoop (1.004, 1000.0, true,  false, "langsam (~3 dB/s) + Sprache, 1000 Hz");
    auto c = testLoop (1.03,  6100.0, false, false, "schnell ohne Sprache, 6100 Hz");
    auto d = testLoop (1.03,  2900.0, true,  false, "Monitor-Modus (nur erkennen), 2900 Hz", true, 4.0);
    auto m = testLoop (1.03,  4200.0, true,  true,  "schnell + monotone Sprache (Worst Case), 4200 Hz");
    auto e2 = testLoop (1.004, 1500.0, false, false, "langsam (~3 dB/s) ohne Sprache, 1500 Hz");
    auto f2 = testLoop (1.01,  1700.0, true,  false, "mittel (~8 dB/s) + Sprache, 1700 Hz");

    auto ok = [&] (bool cond, const char* what) { std::printf ("  %s  %s\n", cond ? "OK     " : "PROBLEM", what); if (! cond) ++fails; };
    std::printf ("\n=== Bewertung (Ton-Pegel < -30 dBFS; Sprachspitzen liegen bei ca. -16 dBFS) ===\n");
    ok (a.real > 0 && a.maxToneDb < -30.0 && a.falseAlarms == 0, "schnell + Sprache: gefangen, keine Fehlalarme");
    ok (b.real > 0 && b.maxToneDb < -30.0 && b.falseAlarms == 0, "langsam + Sprache: gefangen, keine Fehlalarme");
    ok (c.real > 0 && c.maxToneDb < -30.0 && c.falseAlarms == 0, "ohne Sprache: gefangen");
    ok (d.real > 0 && d.notches == 0 && d.firstRealS >= 0, "Monitor-Modus meldet, filtert aber nicht");
    ok (m.real > 0 && m.maxToneDb < -30.0 && m.falseAlarms == 0, "monotone Sprache + Feedback: gefangen, keine Fehlalarme");
    ok (e2.real > 0 && e2.maxToneDb < -30.0 && e2.falseAlarms == 0, "langsames Aufschaukeln ohne Sprache gefangen");
    ok (f2.real > 0 && f2.maxToneDb < -30.0 && f2.falseAlarms == 0, "mittleres Aufschaukeln mit Sprache gefangen");
    ok (std::fabs (a.notchHz - 2900.0) < 15.0 && std::fabs (c.notchHz - 6100.0) < 25.0, "Notch sitzt auf der Feedback-Frequenz");

    // Rechenaufwand
    {
        fg::ChannelEngine e; e.prepare (SR);
        fg::Settings s; e.setSettings (s);
        Speech sp (false);
        std::vector<float> buf (256);
        const int blocks = (int) (30 * SR / 256);
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) { for (auto& x : buf) x = (float) sp.next(); e.process (buf.data(), 256); }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("\nRechenaufwand: 30 s Audio in %.2f s  (%.1f %% einer CPU-Kern-Last pro Kanal, inkl. Signalerzeugung)\n", sec, 100.0 * sec / 30.0);
    }

    std::printf ("\n%s\n", fails == 0 ? "ALLE TESTS BESTANDEN" : "ES GIBT PROBLEME");
    return fails;
}
