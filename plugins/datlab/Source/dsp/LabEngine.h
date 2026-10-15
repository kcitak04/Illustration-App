// Runs one Sample Lab process with the SHAPE section that DATLAB adds around every process:
// region, input gain, reverse in, then dry/wet, reverse out, filters, width, fades, loop crossfade, trim, normalize, output gain.
#pragma once
#include "LabCore.h"

namespace lab
{
struct Shape
{
    double start = 0, end = 1;      // region of the source (fractions)
    double inGain = 0;              // dB
    bool revIn = false;
    double mix = 1;                 // dry/wet
    bool revOut = false;
    double lowCut = 20, highCut = 20000;   // Hz (20 / 20000 = off)
    double width = 1;               // stereo width 0..2
    double fadeIn = 0, fadeOut = 5;  // ms
    double loopXfade = 0;           // ms: crossfades the tail into the head for a seamless loop
    bool trim = false;              // removes silence at both ends
    bool normalize = true;
    double outGain = 0;             // dB
    double maxLen = 180;            // seconds
};

struct Job
{
    Chans src; double sr = 44100;
    std::string procId; Params params;
    Shape shape;
    Chans aux;                      // impulse / modulator / mask sample (resampled to sr)
    double bpm = 120;
};

struct Result { Chans ch; double sr = 44100; std::string error; };

// progress(0..1) may throw lab::Cancelled to abort
Result run (const Job& job, const std::function<void (double)>& progress);

// Pure shape helpers (also used by the tests)
void applyPre (Chans& ch, double sr, const Shape& s);
void applyPost (Chans& wet, const Chans& dry, double sr, const Shape& s);
}
