// Ring buffer between the web view's AudioContext (producer, arbitrary clock) and the host's audio callback (consumer).
// Frames are addressed by their AudioContext frame number, so sync code can say "context frame F plays at host sample S".
// A slow proportional controller resamples by a few hundred ppm to keep the fill level at the target despite clock drift.
#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace dat
{
class AudioBridge
{
public:
    struct Status { double readFrame = 0; double ratio = 1; double rate = 1; bool running = false; double fillSec = 0; };

    void setTargetFill (double seconds) { const juce::SpinLock::ScopedLockType l (lock); targetSec = seconds; }

    // producer: frames of interleaved stereo starting at context frame 'frame'
    void push (juce::int64 frame, const float* data, int frames, double sampleRate)
    {
        const juce::SpinLock::ScopedLockType l (lock);
        ctxSr = sampleRate;
        if (writeFrame < 0 || frame < writeFrame - CAP / 4 || frame > writeFrame + CAP / 2)
        {   // first chunk, or the page / AudioContext restarted
            writeFrame = frame; firstFrame = frame; running = false; primed = false; resets++; errSm = 0;
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
    Status pull (float* L, float* R, int n, double hostSr)
    {
        const juce::SpinLock::ScopedLockType l (lock);
        Status st;
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
        if (fill > target * 3.0 || fill < -0.03 * ctxSr)
        {   // far too much buffered, or the page fell behind the read head: re-centre (the sync code re-cues)
            readFrame = (double) writeFrame - target; fill = target; skips++; st.readFrame = readFrame;
        }
        // PI controller on the smoothed fill level: the integral learns the page clock's drift
        // (a web view's device-less AudioContext can run a percent or so off), P keeps the buffer centred
        const double err = (fill - target) / target;
        errSm += (err - errSm) * (1.0 - std::exp (-(double) n / hostSr / 1.0));
        integ = juce::jlimit (-0.03, 0.03, integ + 3e-5 * errSm);
        const double ratio = r0 * (1.0 + juce::jlimit (-0.03, 0.03, 0.02 * errSm + integ));
        // The read head never stops: frames that have not arrived yet play as silence, so the mapping
        // context frame <-> host sample stays continuous and the sync survives a dropout.
        const bool shortNow = fill < (double) n * ratio + 2;
        if (shortNow) underruns++;
        for (int i = 0; i < n; ++i)
        {
            const double fl = std::floor (readFrame); const auto i0 = (juce::int64) fl; const float f = (float) (readFrame - fl);
            if (i0 + 1 >= writeFrame) { L[i] = R[i] = 0; readFrame += ratio; continue; }
            const auto a = (size_t) (i0 & (CAP - 1)) * 2, b = (size_t) ((i0 + 1) & (CAP - 1)) * 2;
            L[i] = buf[a] + (buf[b] - buf[a]) * f;
            R[i] = buf[a + 1] + (buf[b + 1] - buf[a + 1]) * f;
            readFrame += ratio;
        }
        st.ratio = ratio; st.rate = 1.0 + integ; st.running = true; st.fillSec = fill / ctxSr;
        return st;
    }

    double contextRate() const { return ctxSr; }
    juce::int64 framesWritten() const { return writeFrame; }
    int underruns = 0, skips = 0, resets = 0;

private:
    static constexpr juce::int64 CAP = 1 << 18;   // ~5 s at 48 kHz
    std::vector<float> buf = std::vector<float> ((size_t) CAP * 2);
    juce::SpinLock lock;
    juce::int64 writeFrame = -1, firstFrame = 0;
    double readFrame = 0, ctxSr = 48000, targetSec = 0.15, errSm = 0, integ = 0;
    bool running = false, primed = false;
};
}
