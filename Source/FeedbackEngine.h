// FeedbackEngine.h
// ---------------------------------------------------------------------------
// Feedback-Erkennung und -Unterdrückung für einen Mono-Kanal.
// Reines C++17, keine JUCE-Abhängigkeit (deshalb auch ohne Plugin-Host testbar).
//
// Unterschiede zur Python-Version (feedback_guard_2ch.py):
//   * FFT 4096 mit echter Frequenzauflösung (statt 256 Samples + Zero-Padding)
//   * Parabolische Interpolation für die Frequenz
//   * Peak-Tracking über mehrere Frames (Anstiegsrate, Frequenzstabilität)
//   * Harmonik-Test: Sprache/Gesang hat Obertonreihen, Feedback ist ein Einzelton
//   * Notches als schmale Peaking-Filter mit weichem Ein-/Ausblenden
//   * Kein Datei-I/O, kein print(), keine Allokation im Audio-Thread
//   * Null Latenz: Analyse läuft seitlich, das Signal geht nur durch die Filter
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#ifdef FG_DEBUG
#include <cstdio>
#endif

namespace fg
{
constexpr int kMaxNotches  = 12;
constexpr int kMaxTracks   = 8;
constexpr int kHistory     = 16;
constexpr int kDisplayBins = 160;
constexpr double kPi       = 3.14159265358979323846;

struct Settings
{
    bool  enabled      = true;
    bool  monitorOnly  = false;   // erkennen und anzeigen, aber nicht filtern
    float prominenceDb = 14.0f;   // Mindest-Überhöhung des Peaks über die Umgebung
    int   speed        = 1;       // 0 vorsichtig, 1 normal, 2 schnell
    int   maxNotches   = 8;
    float maxDepthDb   = 18.0f;   // maximale Absenkung pro Notch (positiver Wert)
    float holdSeconds  = 60.0f;   // danach Notch ausblenden; >= 600 = dauerhaft
    float minFreq      = 150.0f;
    float maxFreq      = 10000.0f;
    bool  duckEnabled  = true;
    float duckDb       = 6.0f;    // Notbremse bei sehr schnellem Aufschaukeln
    float notchQ       = 30.0f;
};

enum class Rule : uint8_t { Runaway = 0, Growing = 1, Sustained = 2 };

struct Event
{
    float   freq         = 0;
    float   depthDb      = 0;     // Zielabsenkung (negativ)
    float   prominenceDb = 0;
    float   slopeDbPerS  = 0;
    uint8_t rule         = 0;
    bool    deepened     = false; // bestehende Notch vertieft statt neu gesetzt
    bool    monitorOnly  = false;
};

// ---------------------------------------------------------------------------
class FFT
{
public:
    void init (int n)
    {
        N = n;
        int l = 0;
        while ((1 << l) < n) ++l;
        cosT.resize ((size_t) n / 2);
        sinT.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
        {
            cosT[(size_t) i] = (float) std::cos (2.0 * kPi * i / n);
            sinT[(size_t) i] = (float) -std::sin (2.0 * kPi * i / n);
        }
        rev.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < l; ++b)
                if (i & (1 << b)) r |= 1 << (l - 1 - b);
            rev[(size_t) i] = r;
        }
    }

    void forward (float* re, float* im) const
    {
        for (int i = 0; i < N; ++i)
        {
            const int j = rev[(size_t) i];
            if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        for (int size = 2; size <= N; size <<= 1)
        {
            const int half = size >> 1, step = N / size;
            for (int start = 0; start < N; start += size)
                for (int k = 0; k < half; ++k)
                {
                    const float wr = cosT[(size_t) (k * step)];
                    const float wi = sinT[(size_t) (k * step)];
                    const int a = start + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;  im[b] = im[a] - ti;
                    re[a] += tr;         im[a] += ti;
                }
        }
    }

private:
    int N = 0;
    std::vector<float> cosT, sinT;
    std::vector<int> rev;
};

// ---------------------------------------------------------------------------
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    inline double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setPeaking (double f, double gainDb, double q, double sr)
    {
        const double A     = std::pow (10.0, gainDb / 40.0);
        const double w0    = 2.0 * kPi * f / sr;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double c     = std::cos (w0);
        const double a0    = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * c) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * c) / a0;
        a2 = (1.0 - alpha / A) / a0;
    }

    void clear() { z1 = z2 = 0; }
};

// ---------------------------------------------------------------------------
class ChannelEngine
{
public:
    static constexpr int kFFT = 4096;

    ChannelEngine() = default;

    void prepare (double sampleRate)
    {
        sr   = sampleRate;
        N    = (sr > 50000.0) ? 8192 : 4096;
        hop  = N / 8;
        nb   = N / 2 + 1;
        binHz = sr / N;
        frameRate = sr / hop;

        fft.init (N);
        window.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N));

        ring.assign ((size_t) N, 0.0f);
        re.assign ((size_t) N, 0.0f);
        im.assign ((size_t) N, 0.0f);
        db.assign ((size_t) nb, -200.0f);
        prefix.assign ((size_t) nb + 1, 0.0);
        prefixP.assign ((size_t) nb + 1, 0.0);
        floorDb.assign ((size_t) nb, -200.0f);
        dbOffset = 10.0 * std::log10 (16.0 / ((double) N * (double) N));

        // logarithmische Anzeige-Bins 50 Hz .. 16 kHz
        const double fLo = 50.0, fHi = std::min (16000.0, sr * 0.45);
        for (int d = 0; d < kDisplayBins; ++d)
        {
            const double f0 = fLo * std::pow (fHi / fLo, (double) d / kDisplayBins);
            const double f1 = fLo * std::pow (fHi / fLo, (double) (d + 1) / kDisplayBins);
            dispLo[(size_t) d] = std::clamp ((int) std::floor (f0 / binHz), 1, nb - 2);
            dispHi[(size_t) d] = std::clamp ((int) std::ceil  (f1 / binHz), dispLo[(size_t) d], nb - 2);
        }

        duckAttack  = 1.0 - std::exp (-1.0 / (0.005 * sr));
        duckRelease = 1.0 - std::exp (-1.0 / (1.5 * sr));
        rampDownStep = 0.5;                                   // dB pro 32 Samples beim Absenken
        rampUpStep   = 3.6 * 32.0 / sr;                       // dB pro 32 Samples beim Ausblenden (~5 s für 18 dB)
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        for (auto& t : tracks) t = Track{};
        for (auto& s : slots)  s = Slot{};
        writePos = 0; hopCount = 0; rampCount = 0;
        sampleClock = 0;
        duckGain = 1.0; duckHold = 0;
        numCand = numPk = 0;
        for (auto& a : uiNotchFreq)  a.store (0.0f);
        for (auto& a : uiNotchDepth) a.store (0.0f);
        uiDuckDb.store (0.0f);
        for (auto& a : uiSpectrum)   a.store (-120.0f);
    }

    void setSettings (const Settings& s) { cfg = s; }

    // Verarbeitet in-place. Audio-Thread-sicher (keine Allokation, keine Locks).
    void process (float* data, int n)
    {
        if (! cfg.enabled)
        {
            // Durchreichen; Filter langsam abbauen lassen wäre hier unnötig – Bypass soll hart sein.
            return;
        }

        float blockPeak = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float x = data[i];
            blockPeak = std::max (blockPeak, std::fabs (x));

            ring[(size_t) writePos] = x;
            writePos = (writePos + 1) % N;
            ++sampleClock;

            if (++hopCount >= hop) { hopCount = 0; analyseFrame(); }
            if (++rampCount >= 32) { rampCount = 0; updateNotches(); }

            double y = x;
            if (! cfg.monitorOnly)
            {
                for (auto& s : slots)
                    if (s.active) y = s.bq.process (y);

                const double target = (cfg.duckEnabled && duckHold > 0) ? std::pow (10.0, -cfg.duckDb / 20.0) : 1.0;
                duckGain += (target - duckGain) * (target < duckGain ? duckAttack : duckRelease);
                y *= duckGain;
            }
            data[i] = (float) y;
        }

        const float lvl = 20.0f * std::log10 (std::max (blockPeak, 1.0e-6f));
        uiLevelDb.store (lvl, std::memory_order_relaxed);
    }

    // ---- Daten für die Oberfläche (aus beliebigem Thread lesbar) ----------
    std::array<std::atomic<float>, kDisplayBins> uiSpectrum {};   // dBFS (Sinus-äquivalent)
    std::array<std::atomic<float>, kMaxNotches>  uiNotchFreq {};  // 0 = Slot frei
    std::array<std::atomic<float>, kMaxNotches>  uiNotchDepth {}; // dB (negativ)
    std::atomic<float>    uiDuckDb  { 0.0f };
    std::atomic<float>    uiLevelDb { -120.0f };
    std::atomic<uint32_t> detections { 0 };

    // Single-producer (Audio) / single-consumer (UI) Ereignis-Queue
    bool popEvent (Event& e)
    {
        const uint32_t r = evRead.load (std::memory_order_relaxed);
        if (r == evWrite.load (std::memory_order_acquire)) return false;
        e = events[r % kEvents];
        evRead.store (r + 1, std::memory_order_release);
        return true;
    }

    double getSampleRate() const { return sr; }
    int    getActiveNotchCount() const
    {
        int c = 0;
        for (auto& s : slots) if (s.active) ++c;
        return c;
    }

    // Für Tests
    struct NotchInfo { double freq; double currentDb; double targetDb; };
    std::vector<NotchInfo> getNotchInfo() const
    {
        std::vector<NotchInfo> v;
        for (auto& s : slots) if (s.active) v.push_back ({ s.freq, s.currentDb, s.targetDb });
        return v;
    }

private:
    struct Track
    {
        bool  active = false;
        float freq = 0, prom = 0;
        int   count = 0, missed = 0, sinceAct = 1 << 20;
        bool  acted = false;
        std::array<float, kHistory> lvl {}, frq {}, bb {};
        int   head = 0;
    };

    struct Slot
    {
        bool   active = false, releasing = false;
        double freq = 0, currentDb = 0, targetDb = 0;
        int64_t lastRefresh = 0;
        Biquad bq;
    };

    struct Cand { float freq, level, prom; int bin; bool used; };

    // ----------------------------------------------------------------------
    void analyseFrame()
    {
        // Letzte N Samples gefenstert in den FFT-Puffer
        for (int i = 0; i < N; ++i)
        {
            re[(size_t) i] = ring[(size_t) ((writePos + i) % N)] * window[(size_t) i];
            im[(size_t) i] = 0.0f;
        }
        fft.forward (re.data(), im.data());

        prefixP[0] = 0.0;
        for (int k = 0; k < nb; ++k)
        {
            const double p = (double) re[(size_t) k] * re[(size_t) k] + (double) im[(size_t) k] * im[(size_t) k];
            db[(size_t) k] = (float) (10.0 * std::log10 (p + 1.0e-20) + dbOffset);
            prefixP[(size_t) k + 1] = prefixP[(size_t) k] + p;
        }
        bbLo = std::max (2, (int) std::floor (100.0 / binHz));
        bbHi = std::min (nb - 2, (int) std::ceil (8000.0 / binHz));

        // Lokaler Boden: Mittel der dB-Werte in [k-25,k-5] und [k+5,k+25]
        prefix[0] = 0.0;
        for (int k = 0; k < nb; ++k) prefix[(size_t) k + 1] = prefix[(size_t) k] + db[(size_t) k];
        for (int k = 0; k < nb; ++k)
        {
            double sum = 0; int cnt = 0;
            const int l0 = std::max (0, k - 25), l1 = k - 5;          // inkl.
            if (l1 >= l0) { sum += prefix[(size_t) l1 + 1] - prefix[(size_t) l0]; cnt += l1 - l0 + 1; }
            const int r0 = k + 5, r1 = std::min (nb - 1, k + 25);
            if (r1 >= r0) { sum += prefix[(size_t) r1 + 1] - prefix[(size_t) r0]; cnt += r1 - r0 + 1; }
            floorDb[(size_t) k] = cnt > 0 ? (float) (sum / cnt) : db[(size_t) k];
        }

        publishSpectrum();

        // ---- Peaks einsammeln ---------------------------------------------
        numCand = numPk = 0;
        const int kLo = std::max (3, (int) std::floor (60.0 / binHz));
        const int kHi = std::min (nb - 4, (int) std::ceil (std::max ((double) cfg.maxFreq, 8000.0) / binHz));
        const float candThresh = cfg.prominenceDb - 3.0f;
        constexpr float kGate = -75.0f;     // absolute Mindestlautstärke (dBFS, Sinus-äquivalent)

        for (int k = kLo; k <= kHi; ++k)
        {
            const float b = db[(size_t) k];
            if (! (b > db[(size_t) k - 1] && b >= db[(size_t) k + 1])) continue;
            if (b < kGate) continue;
            const float prom = b - floorDb[(size_t) k];
            if (prom < 8.0f && prom < candThresh) continue;

            const float a = db[(size_t) k - 1], c = db[(size_t) k + 1];
            const float denom = a - 2.0f * b + c;
            float delta = denom < -1.0e-6f ? 0.5f * (a - c) / denom : 0.0f;
            delta = std::clamp (delta, -0.5f, 0.5f);
            const float level = b - 0.25f * (a - c) * delta;
            const float freq  = (float) ((k + delta) * binHz);

            if (prom >= 8.0f && numPk < (int) pk.size()) { pk[(size_t) numPk] = freq; pkProm[(size_t) numPk] = prom; ++numPk; }

            if (freq >= cfg.minFreq && freq <= cfg.maxFreq && prom >= candThresh)
            {
                Cand cd { freq, level, prom, k, false };
                if (numCand < (int) cand.size()) cand[(size_t) numCand++] = cd;
                else
                {
                    int w = 0;
                    for (int j = 1; j < numCand; ++j) if (cand[(size_t) j].prom < cand[(size_t) w].prom) w = j;
                    if (cand[(size_t) w].prom < prom) cand[(size_t) w] = cd;
                }
            }
        }

        // ---- Tracks fortschreiben -----------------------------------------
        const float matchRadius = (float) (1.5 * binHz);
        for (auto& t : tracks)
        {
            if (! t.active) continue;
            ++t.sinceAct;
            int best = -1; float bestD = matchRadius;
            for (int j = 0; j < numCand; ++j)
            {
                if (cand[(size_t) j].used) continue;
                const float d = std::fabs (cand[(size_t) j].freq - t.freq);
                if (d <= bestD) { bestD = d; best = j; }
            }
            if (best >= 0)
            {
                auto& c = cand[(size_t) best];
                c.used = true;
                t.freq = c.freq; t.prom = c.prom; t.missed = 0; ++t.count;
                t.lvl[(size_t) t.head] = c.level;
                t.frq[(size_t) t.head] = c.freq;
                t.bb[(size_t) t.head]  = broadbandDb (c.bin);
                t.head = (t.head + 1) % kHistory;
            }
            else if (++t.missed > 1) t.active = false;
        }

        for (int j = 0; j < numCand; ++j)
        {
            auto& c = cand[(size_t) j];
            if (c.used || c.prom < cfg.prominenceDb) continue;
            int slot = -1;
            for (int t = 0; t < kMaxTracks; ++t) if (! tracks[(size_t) t].active) { slot = t; break; }
            if (slot < 0)   // schwächsten Track verdrängen
            {
                slot = 0;
                for (int t = 1; t < kMaxTracks; ++t)
                    if (tracks[(size_t) t].prom < tracks[(size_t) slot].prom) slot = t;
                if (tracks[(size_t) slot].prom >= c.prom) continue;
            }
            Track nt;
            nt.active = true; nt.freq = c.freq; nt.prom = c.prom; nt.count = 1;
            nt.lvl[0] = c.level; nt.frq[0] = c.freq; nt.bb[0] = broadbandDb (c.bin); nt.head = 1;
            tracks[(size_t) slot] = nt;
        }

        // ---- Regeln auswerten ---------------------------------------------
        const double sc[3]       = { 1.5, 1.0, 0.7 };
        const float  growMin[3]  = { 4.0f, 3.0f, 2.0f };
        const float  runMin[3]   = { 15.0f, 12.0f, 9.0f };
        const int sp = std::clamp (cfg.speed, 0, 2);
        const int fastN = (int) std::ceil (5  * sc[sp]);
        const int growN = (int) std::ceil (12 * sc[sp]);
        const int sustN = (int) std::ceil (40 * sc[sp]);
        const int cooldown = (int) (0.3 * frameRate);

        bool severe = false;

        for (auto& t : tracks)
        {
            if (! t.active || t.count < 4) continue;

            const int m = std::min (t.count, 12);
            float slopeFrame = 0, jitterBins = 0, bbFrame = 0;
            lsSlope (t, m, slopeFrame, jitterBins, bbFrame);
            const float slopeAbs = slopeFrame * (float) frameRate;
            const float bbSlope  = bbFrame * (float) frameRate;
            if (jitterBins > 1.5f) continue;

            // Feedback wächst ALLEIN. Wächst das restliche Spektrum mit (Silbenanfang,
            // Atemgeräusch, Lautstärkesprung), ist es Sprache und kein Feedback.
            const bool isolated = bbSlope <= std::max (6.0f, 0.4f * slopeAbs);
            const float slope = isolated ? (slopeAbs - std::max (0.0f, bbSlope)) : -999.0f;

            const bool canAct = ! t.acted || t.sinceAct >= cooldown;
            bool harmChecked = false, harm = false;
            auto isHarm = [&]() { if (! harmChecked) { harm = isHarmonic (t.freq, t.prom); harmChecked = true; } return harm; };

            // Notbremse: sehr schnell und deutlich -> Ducking, auch bevor Notch sitzt
            if (t.count >= 3 && slope >= 20.0f && slope <= 150.0f && t.prom >= 20.0f && ! isHarm())
                severe = true;

            if (! canAct) continue;

            if (t.count >= fastN && slope >= runMin[sp] && slope <= 150.0f && t.prom >= cfg.prominenceDb && ! isHarm())
            { act (t, Rule::Runaway, slope); continue; }

            if (t.count >= growN && slope >= growMin[sp] && jitterBins <= 1.0f && ! isHarm())
            { act (t, Rule::Growing, slope); continue; }

            if (! t.acted && t.count >= sustN && t.prom >= cfg.prominenceDb + 4.0f
                && jitterBins <= 0.6f && slope >= -1.0f && ! isHarm())
            { act (t, Rule::Sustained, slope); continue; }
        }

        if (severe) duckHold = (int) (0.5 * frameRate);
        else if (duckHold > 0) --duckHold;
    }

    void lsSlope (const Track& t, int m, float& slope, float& jitterBins, float& bbSlope) const
    {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, sb = 0, sxb = 0;
        float fmin = 1e9f, fmax = -1e9f;
        for (int i = 0; i < m; ++i)
        {
            const int idx = (t.head - m + i + 2 * kHistory) % kHistory;
            const double y = t.lvl[(size_t) idx];
            const double yb = t.bb[(size_t) idx];
            sx += i; sy += y; sxx += (double) i * i; sxy += i * y; sb += yb; sxb += i * yb;
            fmin = std::min (fmin, t.frq[(size_t) idx]);
            fmax = std::max (fmax, t.frq[(size_t) idx]);
        }
        const double den = m * sxx - sx * sx;
        slope   = den > 0 ? (float) ((m * sxy - sx * sy) / den) : 0.0f;
        bbSlope = den > 0 ? (float) ((m * sxb - sx * sb) / den) : 0.0f;
        jitterBins = (float) ((fmax - fmin) / binHz);
    }

    // Pegel des restlichen Spektrums (100 Hz .. 8 kHz) ohne die Umgebung (+-6 Bins) des Peaks
    float broadbandDb (int bin) const
    {
        const double total = prefixP[(size_t) bbHi + 1] - prefixP[(size_t) bbLo];
        const int e0 = std::max (bin - 6, bbLo), e1 = std::min (bin + 6, bbHi);
        const double excl = e1 >= e0 ? prefixP[(size_t) e1 + 1] - prefixP[(size_t) e0] : 0.0;
        return (float) (10.0 * std::log10 (std::max (total - excl, 0.0) + 1.0e-20) + dbOffset);
    }

    // Gibt es im aktuellen Frame eine Obertonreihe, zu der dieser Peak gehört?
    // Partner zählen nur, wenn sie im Verhältnis zum geprüften Peak nennenswert sind;
    // so erzeugen zufällige Rauschspitzen neben einem starken Feedback-Ton keinen falschen Treffer.
    bool isHarmonic (float f, float prom) const
    {
        return partnerChain (f, prom) || fundamentalSeries (f);
    }

    // Schätzt aus den tiefen Peaks (< 1,5 kHz) einen Grundton f0 und prüft, ob f ein
    // ganzzahliges Vielfaches davon ist. Fängt auch sehr dichte Reihen (tiefe Stimmen) ab.
    bool fundamentalSeries (float f) const
    {
        const float self = (float) (1.5 * binHz);
        float low[32]; int nl = 0;
        for (int j = 0; j < numPk && nl < 32; ++j)
            if (pk[(size_t) j] < 1500.0f && std::fabs (pk[(size_t) j] - f) > self) low[nl++] = pk[(size_t) j];
        if (nl < 4) return false;

        float bestD = 0; int bestM = 0;
        auto tryD = [&] (float d)
        {
            if (d < 55.0f || d > 500.0f) return;
            int m = 0;
            for (int i = 0; i < nl; ++i)
            {
                const int n = (int) std::lround (low[i] / d);
                if (n >= 1 && std::fabs (low[i] - n * d) <= (float) (0.7 * binHz) + 0.3f * n) ++m;
            }
            if (m > bestM || (m == bestM && d > bestD)) { bestM = m; bestD = d; }
        };
        for (int i = 0; i < nl; ++i)
        {
            tryD (low[i]);
            if (i + 1 < nl) tryD (low[i + 1] - low[i]);
        }
        if (bestM < 4 || bestM < (int) std::ceil (0.6f * nl)) return false;

        // Feinabstimmung per Least Squares über die passenden Peaks
        double sn = 0, snf = 0;
        for (int i = 0; i < nl; ++i)
        {
            const int n = (int) std::lround (low[i] / bestD);
            if (n >= 1 && std::fabs (low[i] - n * bestD) <= (float) (0.7 * binHz) + 0.3f * n) { sn += (double) n * n; snf += (double) n * low[i]; }
        }
        if (sn <= 0) return false;
        const double d = snf / sn;
        const int n = (int) std::lround (f / d);
        if (n < 1) return false;
        // Unsicherheit der Vorhersage wächst mit der Ordnung n. Wird sie größer als ein Drittel des
        // Obertonabstands, lässt sich ein Einzelton nicht mehr von einem Oberton trennen -> als Stimme werten.
        const double tol = 0.7 * binHz + 0.6 * n;
        if (tol > 0.3 * d) return true;
        return std::fabs (f - n * d) <= tol;
    }

    bool partnerChain (float f, float prom) const
    {
        const float self = (float) (1.5 * binHz);
        const float partnerMin = std::max (8.0f, prom - 25.0f);

        for (int j = 0; j < numPk; ++j)
        {
            if (pkProm[(size_t) j] < partnerMin) continue;
            const float d = std::fabs (pk[(size_t) j] - f);
            if (d <= self || d < 50.0f || d > 500.0f) continue;

            int matches = 0;
            for (int l = 0; l < numPk; ++l)
            {
                if (pkProm[(size_t) l] < partnerMin) continue;
                const float dist = std::fabs (pk[(size_t) l] - f);
                if (dist <= self) continue;
                const int n = (int) std::lround (dist / d);
                if (n < 1 || n > 14) continue;
                const float tol = (float) (0.6 * binHz) + 0.004f * n * d;
                if (std::fabs (dist - n * d) <= tol) ++matches;
            }
            if (matches >= 3) return true;
        }
        return false;
    }

    void act (Track& t, Rule rule, float slope)
    {
#ifdef FG_DEBUG
        {
            float sf = 0, jb = 0, bf = 0; lsSlope (t, std::min (t.count, 12), sf, jb, bf);
            std::printf ("DEBUG act f=%.1f count=%d slopeRel=%.1f abs=%.1f bb=%.1f jitter=%.2f numPk=%d peaks:", t.freq, t.count, slope, sf * frameRate, bf * frameRate, jb, numPk);
            for (int i = 0; i < numPk; ++i) std::printf (" %.0f", pk[(size_t) i]);
            std::printf ("\n");
        }
#endif
        t.acted = true;
        t.sinceAct = 0;

        const double f = t.freq;
        const double tol = std::max (1.5 * binHz, 0.5 * f / cfg.notchQ);
        const double startDepth[3] = { 12.0, 9.0, 6.0 };
        const double stepDeeper[3] = { 6.0, 4.0, 4.0 };
        const int r = (int) rule;

        Event ev;
        ev.freq = (float) f; ev.prominenceDb = t.prom; ev.slopeDbPerS = slope;
        ev.rule = (uint8_t) rule; ev.monitorOnly = cfg.monitorOnly;

        detections.fetch_add (1, std::memory_order_relaxed);

        if (cfg.monitorOnly)
        {
            ev.depthDb = (float) -std::min (startDepth[r], (double) cfg.maxDepthDb);
            pushEvent (ev);
            return;
        }

        int found = -1;
        for (int i = 0; i < kMaxNotches; ++i)
            if (slots[(size_t) i].active && std::fabs (slots[(size_t) i].freq - f) <= tol) { found = i; break; }

        if (found >= 0)
        {
            auto& s = slots[(size_t) found];
            s.targetDb = -std::min ((double) cfg.maxDepthDb, -s.targetDb + stepDeeper[r]);
            s.lastRefresh = sampleClock;
            s.releasing = false;
            ev.deepened = true;
            ev.depthDb = (float) s.targetDb;
        }
        else
        {
            int free = -1, used = 0;
            for (int i = 0; i < kMaxNotches; ++i)
            {
                if (! slots[(size_t) i].active) { if (free < 0) free = i; }
                else ++used;
            }
            if (used >= std::min (cfg.maxNotches, kMaxNotches) || free < 0)
            {
                // ältesten verdrängen
                int old = -1;
                for (int i = 0; i < kMaxNotches; ++i)
                    if (slots[(size_t) i].active && (old < 0 || slots[(size_t) i].lastRefresh < slots[(size_t) old].lastRefresh)) old = i;
                if (old < 0) return;
                free = old;
            }
            auto& s = slots[(size_t) free];
            s = Slot{};
            s.active = true; s.freq = f; s.currentDb = 0.0;
            s.targetDb = -std::min (startDepth[r], (double) cfg.maxDepthDb);
            s.lastRefresh = sampleClock;
            s.bq.setPeaking (f, 0.0, cfg.notchQ, sr);
            ev.depthDb = (float) s.targetDb;
        }

        if (rule == Rule::Runaway) duckHold = (int) (0.5 * frameRate);
        pushEvent (ev);
    }

    void updateNotches()
    {
        const bool holdForever = cfg.holdSeconds >= 600.0f;
        const int64_t holdSamples = (int64_t) ((double) cfg.holdSeconds * sr);

        for (int i = 0; i < kMaxNotches; ++i)
        {
            auto& s = slots[(size_t) i];
            if (! s.active)
            {
                uiNotchFreq[(size_t) i].store (0.0f, std::memory_order_relaxed);
                continue;
            }

            if (! holdForever && ! s.releasing && sampleClock - s.lastRefresh > holdSamples)
                s.releasing = true;

            const double target = s.releasing ? 0.0 : s.targetDb;
            double next = s.currentDb;
            if (target < next)      next = std::max (target, next - rampDownStep);
            else if (target > next) next = std::min (target, next + rampUpStep);

            if (next != s.currentDb || lastQ != cfg.notchQ)
            {
                s.currentDb = next;
                s.bq.setPeaking (s.freq, s.currentDb, cfg.notchQ, sr);
            }

            if (s.releasing && s.currentDb > -0.05)
            {
                s = Slot{};
                uiNotchFreq[(size_t) i].store (0.0f, std::memory_order_relaxed);
                continue;
            }

            uiNotchFreq[(size_t) i].store ((float) s.freq, std::memory_order_relaxed);
            uiNotchDepth[(size_t) i].store ((float) s.currentDb, std::memory_order_relaxed);
        }
        lastQ = cfg.notchQ;
        uiDuckDb.store ((float) (20.0 * std::log10 (std::max (duckGain, 1.0e-4))), std::memory_order_relaxed);
    }

    void publishSpectrum()
    {
        for (int d = 0; d < kDisplayBins; ++d)
        {
            float m = -200.0f;
            for (int k = dispLo[(size_t) d]; k <= dispHi[(size_t) d]; ++k) m = std::max (m, db[(size_t) k]);
            uiSpectrum[(size_t) d].store (m, std::memory_order_relaxed);
        }
    }

    static constexpr uint32_t kEvents = 64;
    void pushEvent (const Event& e)
    {
        const uint32_t w = evWrite.load (std::memory_order_relaxed);
        if (w - evRead.load (std::memory_order_acquire) >= kEvents) return;   // voll: verwerfen
        events[w % kEvents] = e;
        evWrite.store (w + 1, std::memory_order_release);
    }

    // ---- Zustand ----------------------------------------------------------
    Settings cfg;
    double sr = 48000.0, binHz = 48000.0 / 4096, frameRate = 48000.0 / 512, dbOffset = 0;
    int N = 4096, hop = 512, nb = 2049;
    FFT fft;
    std::vector<float> window, ring, re, im, db, floorDb;
    std::vector<double> prefix, prefixP;
    int writePos = 0, hopCount = 0, rampCount = 0;
    int64_t sampleClock = 0;

    std::array<Track, kMaxTracks> tracks {};
    std::array<Slot,  kMaxNotches> slots {};
    std::array<Cand, 16> cand {};
    std::array<float, 96> pk {}, pkProm {};
    int numCand = 0, numPk = 0, bbLo = 2, bbHi = 100;
    std::array<int, kDisplayBins> dispLo {}, dispHi {};

    double duckGain = 1.0, duckAttack = 0.01, duckRelease = 0.0001;
    int duckHold = 0;
    double rampDownStep = 0.5, rampUpStep = 0.0024;
    float lastQ = 30.0f;

    std::array<Event, kEvents> events {};
    std::atomic<uint32_t> evWrite { 0 }, evRead { 0 };
};

} // namespace fg
