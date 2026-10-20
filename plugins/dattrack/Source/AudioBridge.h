// Ring buffer between the web view's AudioContext (producer, arbitrary clock) and the host's audio callback (consumer).
// Frames are addressed by their AudioContext frame number, so sync code can say "context frame F plays at host sample S".
// A PI controller resamples slightly (the web view's clock can be off by up to a few percent) to keep the fill level at the target.
#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace dat
{
class AudioBridge
{
public:
    struct Status { double readFrame = 0; double ratio = 1; double rate = 1; bool running = false; double fillSec = 0; int epoch = 0; };

    void setTargetFill (double seconds) { const juce::SpinLock::ScopedLockType l (lock); targetSec = seconds; }

    // producer: frames of interleaved stereo starting at context frame 'frame'
    void push (juce::int64 frame, const float* data, int frames, double sampleRate)
    {
        const juce::SpinLock::ScopedLockType l (lock);
        ctxSr = sampleRate;
        if (writeFrame < 0 || frame < writeFrame - 4096 || frame > writeFrame + CAP / 2)
        {   // first chunk, or the page / AudioContext restarted (the page never sends a frame twice)
            writeFrame = frame; firstFrame = frame; running = false; primed = false; resets++; errSm = 0;
            // the read head moves to the new timeline right away (where it will start), so the position the sync
            // code reports while the buffer fills is on the new context's clock, not on the old one's
            readFrame = (double) frame - targetSec * sampleRate;
        }
        if (frame > writeFrame) { for (juce::int64 f = writeFrame; f < frame; ++f) { auto i = (size_t) (f & (CAP - 1)) * 2; buf[i] = buf[i + 1] = 0; } writeFrame = frame; }
        int skip = (int) juce::jmax<juce::int64> (0, writeFrame - frame);
        for (int k = skip; k < frames; ++k)
        {
            auto i = (size_t) ((frame + k) & (CAP - 1)) * 2;
            buf[i] = data[2 * k]; buf[i + 1] = data[2 * k + 1];
        }
        writeFrame = juce::jmax (writeFrame, frame + frames);
    }

    // consumer: renders n frames, returns the context frame that was at the read head at the start of the block
    // offline: the host waits for the page (hasAhead), so the clocks cannot drift - no drift correction (it would only
    // wind up against the fill the wait keeps above the target, and speed up / pitch up the export by up to 3 %)
    Status pull (float* L, float* R, int n, double hostSr, bool offline = false)
    {
        const juce::SpinLock::ScopedLockType l (lock);
        Status st;
        st.epoch = resets;   // which timeline readFrame is on (a page restart starts a new one)
        const double r0 = ctxSr / hostSr, target = targetSec * ctxSr;
        if (! running && writeFrame >= 0)
        {
            if (! primed && (double) (writeFrame - firstFrame) >= target) { readFrame = (double) writeFrame - target; primed = true; }
            if (primed && (double) writeFrame - readFrame >= target) running = true;
        }
        st.readFrame = readFrame;
        if (! running)
        {
            juce::FloatVectorOperations::clear (L, n); juce::FloatVectorOperations::clear (R, n);
            st.ratio = r0; return st;
        }
        double fill = (double) writeFrame - readFrame;
        if (fill < -target)
        {   // the page stopped sending (reload, paused web view...): go silent and re-buffer once audio flows again.
            // (Re-centring here would replay the last buffer over and over: a machine-gun stutter.)
            running = false; primed = false; firstFrame = writeFrame; stalls++;
            juce::FloatVectorOperations::clear (L, n); juce::FloatVectorOperations::clear (R, n);
            st.ratio = r0; return st;
        }
        // (offline the host may render slower than the page plays: use the whole ring before dropping anything)
        if (fill > (offline ? (double) (CAP - 32768) : std::min (target * 3.0, (double) (CAP - 32768))))
        {   // far too much buffered (a burst after a hiccup): skip ahead to the target (the sync code re-cues).
            // Never let the read head fall a whole ring behind: those frames are overwritten already.
            readFrame = (double) writeFrame - target; fill = target; skips++; st.readFrame = readFrame;
        }
        // PI controller on the smoothed fill level: the integral learns the page clock's drift
        // (a web view's device-less AudioContext can run a percent or so off), P keeps the buffer centred
        const double err = (fill - target) / target;
        if (! offline)
        {
            errSm += (err - errSm) * (1.0 - std::exp (-(double) n / hostSr / 1.0));
            integ = juce::jlimit (-0.03, 0.03, integ + 3e-5 * errSm);
        }
        const double ratio = offline ? r0 : r0 * (1.0 + juce::jlimit (-0.03, 0.03, 0.02 * errSm + integ));
        // The read head never stops: frames that have not arrived yet play as silence, so the mapping
        // context frame <-> host sample stays continuous and the sync survives a dropout.
        const bool shortNow = fill < (double) n * ratio + 2;
        if (shortNow && ! wasShort) underruns++;   // count dropouts, not blocks
        wasShort = shortNow;
        for (int i = 0; i < n; ++i)
        {
            const double fl = std::floor (readFrame); const auto i0 = (juce::int64) fl; const float f = (float) (readFrame - fl);
            if (i0 + 1 >= writeFrame) { L[i] = R[i] = 0; readFrame += ratio; continue; }
            const auto a = (size_t) (i0 & (CAP - 1)) * 2, b = (size_t) ((i0 + 1) & (CAP - 1)) * 2;
            L[i] = buf[a] + (buf[b] - buf[a]) * f;
            R[i] = buf[a + 1] + (buf[b + 1] - buf[a + 1]) * f;
            readFrame += ratio;
        }
        st.ratio = ratio; st.rate = offline ? 1.0 : 1.0 + integ; st.running = true; st.fillSec = fill / ctxSr;
        return st;
    }

    // Offline rendering (DAW export): true once the page has rendered enough to fill the next n host samples
    // while keeping the buffer at its target, so the export consumes audio no faster than the page makes it.
    bool hasAhead (int n, double hostSr)
    {
        const juce::SpinLock::ScopedLockType l (lock);
        if (writeFrame < 0) return false;
        const double target = targetSec * ctxSr;
        if (! running) return (double) (writeFrame - firstFrame) >= target;
        return (double) writeFrame - readFrame >= target + (double) n * ctxSr / hostSr + 2;
    }

    double contextRate() const { return ctxSr; }
    int epoch() { const juce::SpinLock::ScopedLockType l (lock); return resets; }
    juce::int64 framesWritten() const { return writeFrame; }
    int underruns = 0, skips = 0, resets = 0, stalls = 0;

private:
    static constexpr juce::int64 CAP = 1 << 19;   // ~11 s at 48 kHz, ~2.7 s at 192 kHz (latency up to 0.7 s, x3 before a skip)
    std::vector<float> buf = std::vector<float> ((size_t) CAP * 2);
    juce::SpinLock lock;
    juce::int64 writeFrame = -1, firstFrame = 0;
    double readFrame = 0, ctxSr = 48000, targetSec = 0.15, errSm = 0, integ = 0;
    bool running = false, primed = false, wasShort = false;
};
}
