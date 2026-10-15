// Process registry: names, descriptions and parameter ranges.
// The first 31 entries mirror DATTRACK's LAB_PROCS exactly (same ids, ranges and defaults).
#include "LabCore.h"
#include <cstdio>

namespace lab
{
namespace
{
using O = std::vector<std::string>;
ParamDef P (std::string id, std::string label, double mn, double mx, double def, int flags = 0, std::string unit = {}, Fmt fmt = Fmt::num, O options = {})
{
    ParamDef d; d.id = std::move (id); d.label = std::move (label); d.min = mn; d.max = mx; d.def = def; d.flags = flags; d.unit = std::move (unit); d.fmt = fmt; d.options = std::move (options);
    if (! d.unit.empty() && d.fmt == Fmt::num) d.fmt = Fmt::unit;
    return d;
}
ParamDef Opt (std::string id, std::string label, O options, int def = 0)
{
    const double mx = (double) options.size() - 1;
    return P (std::move (id), std::move (label), 0, mx, def, PInt, {}, Fmt::num, std::move (options));
}
ParamDef Pct (std::string id, std::string label, double def) { return P (std::move (id), std::move (label), 0, 1, def, PPct, {}, Fmt::pct); }
ParamDef Seed() { return P ("seed", "SEED", 1, 99, 1, PInt); }
ParamDef Win (double mn, double mx, double def) { return P ("win", "WINDOW", mn, mx, def, PExp, {}, Fmt::time); }
const O DIVS { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/8D", "1/16D" };
const O SYNC_FREE = [] { O o { "FREE" }; o.insert (o.end(), DIVS.begin(), DIVS.end()); return o; }();

std::vector<ProcDef> build()
{
    using namespace procs_orig;
    std::vector<ProcDef> v;
    auto add = [&] (std::string cat, std::string id, std::string name, std::string tag, std::string desc, std::vector<ParamDef> ps, ProcFn fn, std::string aux = {}, bool isNew = false)
    {
        ProcDef d; d.category = std::move (cat); d.id = std::move (id); d.name = std::move (name); d.tag = std::move (tag); d.desc = std::move (desc);
        d.params = std::move (ps); d.fn = fn; d.aux = std::move (aux); d.isNew = isNew; v.push_back (std::move (d));
    };
    const std::string SPEC = "SPECTRAL", WAVE = "WAVESET", GRAN = "GRANULAR \xc2\xb7 TIME", TEXT = "TEXTURE", FORM = "FORMANT \xc2\xb7 PITCH \xc2\xb7 SPACE";
    const std::string NSPEC = "NEW \xc2\xb7 SPECTRAL", NDRV = "NEW \xc2\xb7 DRIVE \xc2\xb7 DIGITAL", NRHY = "NEW \xc2\xb7 RHYTHM \xc2\xb7 TIME", NSPC = "NEW \xc2\xb7 SPACE \xc2\xb7 FILTER";

    // ---- the original DATTRACK Sample Lab ----
    add (SPEC, "freeze", "SPECTRAL FREEZE", "FRZ", "Grabs one instant of the sound and holds it as an endless drone. SHIMMER makes the frozen partials flutter.",
         { Pct ("pos", "POSITION", 0.3), P ("len", "LENGTH", 1, 60, 10, 0, "s"), Pct ("shimmer", "SHIMMER", 0.3), Pct ("scan", "SCAN", 0), Seed() }, freeze);
    add (SPEC, "blur", "SPECTRAL BLUR", "BLUR", "Smears the spectrum over time: attacks melt, notes bleed into washes. DIFFUSE scrambles the phases.",
         { Pct ("amount", "AMOUNT", 0.7), Pct ("diffuse", "DIFFUSE", 0.3), Win (0.02, 0.5, 0.09), Seed() }, blur);
    add (SPEC, "morph", "MORPH / VOCODE", "MRPH", "Blends the spectra of two sounds. MORPH mixes them by AMOUNT, GLIDE turns the source into the modulator over the length, VOCODE imposes the modulator's spectral envelope and formants on the source (DETAIL = envelope resolution).",
         { Opt ("mode", "MODE", { "MORPH", "GLIDE A\xe2\x86\x92" "B", "VOCODE" }, 1), Pct ("amount", "AMOUNT", 0.5), P ("detail", "DETAIL", 50, 1000, 250, PExp, {}, Fmt::hz), Win (0.02, 0.3, 0.09) }, morph, "MODULATOR");
    add (SPEC, "tune", "SPEC TUNE", "TUNE", "Forces noise, field recordings or speech onto a pitch template: a scale, a chord, quarter tones or a harmonic series. PROGRESSION steps the root through a chord sequence over time.",
         { P ("note", "ROOT", 24, 72, 48, PInt, {}, Fmt::note), Opt ("scale", "TEMPLATE", { "CHROMATIC", "MAJOR", "MINOR", "PENTATONIC", "WHOLE TONE", "MAJ7 CHORD", "MIN9 CHORD", "QUARTER TONES", "HARMONICS" }, 5),
           Opt ("prog", "PROGRESSION", { "STATIC", "I-IV-V-I", "i-VI-III-VII", "RISING", "FALLING" }), P ("width", "FOCUS", 5, 100, 25, 0, "ct"), Pct ("amount", "STRENGTH", 0.9) }, tune);
    add (SPEC, "sinv", "STRETCH / INVERT", "SINV", "STRETCH pulls the partials away from their harmonic positions (exponent around CENTER). INVERT mirrors the whole spectrum around CENTER: lows become highs. Eerie, alien, bell-like.",
         { Opt ("mode", "MODE", { "STRETCH", "INVERT", "BOTH" }), P ("stretch", "STRETCH", 0.5, 2, 1.25, 0, "x"), P ("center", "CENTER", 50, 5000, 500, PExp, {}, Fmt::hz), Win (0.02, 0.3, 0.09) }, sinv);
    add (SPEC, "shift", "SPECTRAL SHIFT", "SHFT", "Phase-vocoder frequency shift (breaks harmonics: bells, metal, alien voices). SPREAD stretches the whole spectrum.",
         { P ("hz", "SHIFT", -2000, 2000, 200, 0, "Hz"), P ("stretch", "SPREAD", 0.5, 2, 1, 0, "x"), Win (0.02, 0.3, 0.09) }, shift);
    add (SPEC, "focus", "FOCUS / RESON", "FOCS", "Keeps only the strongest spectral peaks and damps everything around them: singing resonances and bell tones pulled out of wind, water or noise. RING lets the peaks sustain.",
         { P ("peaks", "PEAKS", 1, 64, 8, PInt), P ("width", "WIDTH", 0, 4, 1, PInt), Pct ("depth", "DEPTH", 0.95), P ("ring", "RING", 0, 8, 1.5, 0, {}, Fmt::time), Win (0.02, 0.3, 0.09) }, focus);
    add (SPEC, "mask", "MASK / SUPPRESS", "MASK", "MASK SAMPLE filters the source against the spectrum of another sample. STRIP PEAKS removes the strongest partials (fundamentals). TRANSIENTS keeps only onsets, SUSTAIN only the steady part.",
         { Opt ("mode", "MODE", { "MASK SAMPLE", "STRIP PEAKS", "TRANSIENTS", "SUSTAIN" }, 1), Pct ("amount", "AMOUNT", 0.8), Win (0.02, 0.3, 0.05) }, mask, "MASK");
    add (SPEC, "gate", "SPECTRAL GATE", "SGAT", "Keeps only the loudest partials (crystalline tones) or only what hides beneath them (ghostly residue).",
         { P ("thr", "THRESH", -60, 0, -20, 0, {}, Fmt::db), Opt ("mode", "KEEP", { "LOUD", "QUIET" }), Win (0.02, 0.3, 0.09) }, gate);
    add (SPEC, "conv", "CONVOLVE", "CONV", "Plays the source \"through\" another sample (its impulse): cross-synthesis of two sounds. Try a vocal through a cymbal.",
         { Pct ("mix", "MIX", 0.7) }, conv, "IMPULSE");
    add (WAVE, "wdist", "WAVESET DISTORT", "WDST", "Works on single wave cycles (between zero crossings), GROUP at a time: REPEAT (stretches), OMIT, INVERT, REVERSE, TRANSPOSE (FACTOR x up, same length) or SHUFFLE, for CHANCE of the cycles. From warm grit to total destruction.",
         { Opt ("mode", "MODE", { "REPEAT", "OMIT", "INVERT", "REVERSE", "TRANSPOSE", "SHUFFLE" }), P ("group", "GROUP", 1, 16, 2, PInt), P ("amt", "FACTOR", 2, 8, 2, PInt), Pct ("chance", "CHANCE", 0.5), Seed() }, wdist);
    add (WAVE, "wavg", "WAVESET AVERAGE", "WAVG", "Reshapes every wave cycle while the pitch stays intact: NORMALIZE gives all cycles the same level, AVERAGE smooths each cycle toward its SPAN neighbours, ACCENT exaggerates their differences.",
         { Opt ("mode", "MODE", { "NORMALIZE", "AVERAGE", "ACCENT" }, 1), Pct ("amount", "AMOUNT", 0.7), P ("span", "SPAN", 1, 8, 3, PInt), P ("group", "GROUP", 1, 8, 1, PInt) }, wavg);
    add (WAVE, "bits", "BIT MANGLER", "BITS", "Destroys the resolution: bit depth, sample-rate decimation and random bit flips for digital errors.",
         { P ("bits", "BITS", 1, 16, 6, PInt), P ("down", "DOWNSMP", 1, 64, 4, PInt, "x"), Pct ("flip", "BIT FLIPS", 0.2), Pct ("mix", "MIX", 1), Seed() }, bits);
    add (GRAN, "brass", "BRASSAGE", "BRAS", "Classic multi-purpose granulator: independent time STRETCH and PITCH, grain size, DENSITY (overlap), SCATTER of the read position and SHUFFLE (grains jump to other places).",
         { P ("stretch", "STRETCH", 0.25, 8, 2, PExp, "x"), P ("pitch", "PITCH", -24, 24, 0, 0, "st"), P ("size", "GRAIN", 10, 300, 60, PExp, "ms"), P ("density", "DENSITY", 1, 8, 4), Pct ("scatter", "SCATTER", 0.2), Pct ("shuffle", "SHUFFLE", 0), Seed() }, brass);
    add (GRAN, "paul", "PAULSTRETCH", "PSTR", "Extreme time stretch with smeared phases: any sound becomes a slow, glassy ambient cloud.",
         { P ("stretch", "STRETCH", 1.5, 50, 8, PExp, "x"), Win (0.02, 1, 0.25), Seed() }, paul);
    add (GRAN, "grain", "GRANULAR CLOUD", "GRN", "Scatters hundreds of tiny grains of the sample over time, pitch and stereo.",
         { P ("lenx", "LENGTH", 0.25, 8, 2, PExp, "x"), P ("density", "DENSITY", 2, 300, 40, PExp, "/s"), P ("size", "GRAIN", 5, 500, 80, PExp, "ms"), P ("pitch", "PITCH \xc2\xb1", 0, 24, 0, 0, "st"),
           Pct ("jitter", "SCATTER", 0.1), Pct ("rev", "REVERSE", 0), Seed() }, grain);
    add (GRAN, "bounce", "REPEATS / BOUNCE", "BNCE", "Grabs one grain at POSITION and repeats it with shrinking (ACCEL < 1: bouncing ball) or growing (> 1: slowing echo) intervals, each hit DECAY quieter. KEEP REST continues with the rest of the sample.",
         { Pct ("pos", "POSITION", 0), P ("size", "GRAIN", 10, 1000, 120, PExp, "ms"), P ("count", "COUNT", 2, 64, 14, PInt), P ("interval", "INTERVAL", 20, 2000, 400, PExp, "ms"),
           P ("accel", "ACCEL", 0.5, 1.5, 0.8, 0, "x"), Pct ("decay", "DECAY", 0.1), Opt ("keep", "KEEP REST", { "NO", "YES" }) }, bounce);
    add (GRAN, "drunk", "DRUNK / SCRAMBLE", "DRNK", "DRUNK WALK reads SEGMENTs by a random walk (up to STEP segments per move, slowly drifting forward). SCRAMBLE plays random segments of random length from anywhere.",
         { Opt ("mode", "MODE", { "DRUNK WALK", "SCRAMBLE" }), P ("seg", "SEGMENT", 20, 1000, 120, PExp, "ms"), P ("step", "STEP", 1, 16, 3, PInt), P ("lenx", "LENGTH", 0.5, 4, 1, 0, "x"), Seed() }, drunk);
    add (GRAN, "iter", "ITERATE", "ITER", "Takes one tiny segment (a click, a transient) and multiplies it DENSITY times per second with pitch, level and stereo scatter: clicks turn into bowed friction, buzzing or harmonic clouds.",
         { Pct ("pos", "POSITION", 0), P ("size", "GRAIN", 5, 500, 40, PExp, "ms"), P ("len", "LENGTH", 1, 30, 6, 0, "s"), P ("density", "DENSITY", 5, 300, 40, PExp, "/s"),
           P ("pitch", "PITCH \xc2\xb1", 0, 12, 1, 0, "st"), Pct ("gain", "LEVEL \xc2\xb1", 0.5), Seed() }, iter);
    add (GRAN, "zig", "ZIGZAG / SCRUB", "ZIGZ", "ZIGZAG runs the read head forward and backward between random turning points about SPAN apart. SCRUB shuttles it on a sine speed curve at SCRUB RATE, like rocking a tape reel.",
         { Opt ("mode", "MODE", { "ZIGZAG", "SCRUB" }), P ("span", "SPAN", 0.02, 2, 0.3, PExp, {}, Fmt::time), P ("rate", "SCRUB RATE", 0.1, 20, 2, PExp, "Hz"), P ("lenx", "LENGTH", 0.5, 4, 1.5, 0, "x"), Seed() }, zig);
    add (GRAN, "stutter", "STUTTER / GLITCH", "STUT", "Chops the sample into steps and re-triggers, reverses or tape-drops some of them. Change SEED for new patterns.",
         { P ("div", "STEPS", 4, 64, 16, PInt), Pct ("chance", "CHANCE", 0.35), P ("reps", "REPEATS", 2, 16, 4, PInt), Pct ("rev", "REVERSE", 0.2), Pct ("drop", "PITCH DROP", 0.2), Seed() }, stutter);
    add (GRAN, "shuffle", "SLICE SHUFFLE", "SHUF", "Cuts the sample into equal slices and plays them back in a random order (breakbeat mangling).",
         { P ("slices", "SLICES", 2, 64, 8, PInt), Pct ("rev", "REVERSE", 0.15), Opt ("keep", "KEEP 1ST", { "NO", "YES" }, 1), Seed() }, shuffle);
    add (GRAN, "tape", "TAPE STOP", "TAPE", "Motor stop (the sound slows down to a halt), spin-up from standstill, or both.",
         { Opt ("mode", "MODE", { "STOP", "SPIN UP", "BOTH" }), Pct ("at", "STOP AT", 0.7), P ("time", "TIME", 0.05, 4, 0.8, PExp, {}, Fmt::time), P ("curve", "CURVE", 0.3, 3, 1) }, tape);
    add (TEXT, "tex", "TEXTURE", "TEXT", "Scatters the whole (short) source over time, pitch and stereo: a random CLOUD, a RHYTHM grid (DENSITY hits per second) or CHORD tones within the PITCH range.",
         { Opt ("mode", "MODE", { "CLOUD", "RHYTHM", "CHORD" }), P ("len", "LENGTH", 1, 60, 10, 0, "s"), P ("density", "DENSITY", 1, 50, 8, PExp, "/s"), P ("pitch", "PITCH \xc2\xb1", 0, 24, 7, 0, "st"), Pct ("width", "WIDTH", 0.8), Seed() }, tex);
    add (TEXT, "seg", "SHRINK / POLY", "SEGM", "SHRINK repeats a segment, each copy shorter by SHRINK: an accelerating cascade. POLYRHYTHM loops it in several VOICES at 3:4:5:6:7 spread across the stereo field.",
         { Opt ("mode", "MODE", { "SHRINK", "POLYRHYTHM" }), Pct ("pos", "POSITION", 0), P ("size", "SEGMENT", 30, 2000, 300, PExp, "ms"), P ("reps", "REPEATS", 2, 32, 12, PInt),
           P ("shrink", "SHRINK", 0.5, 0.98, 0.8, 0, "x"), P ("voices", "VOICES", 2, 5, 3, PInt) }, seg);
    add (TEXT, "reso", "RESONATOR", "RESO", "A bank of tuned strings that ring along with the sound: turns noise and drums into chords.",
         { P ("note", "NOTE", 24, 84, 45, PInt, {}, Fmt::note), Opt ("chord", "CHORD", { "ROOT", "OCTAVE", "FIFTH", "MAJOR", "MINOR", "SUS4", "DOM7", "CLUSTER" }, 3),
           P ("decay", "DECAY", 0.1, 10, 2, PExp, {}, Fmt::time), Pct ("bright", "BRIGHT", 0.5), Pct ("mix", "MIX", 0.6) }, reso);
    add (TEXT, "revrev", "REVERSE REVERB", "RVRB", "Reverse, reverb, reverse again: a ghostly reverb swells INTO every hit.",
         { P ("size", "SIZE", 0.3, 8, 2.5, PExp, {}, Fmt::time), Pct ("damp", "DAMP", 0.5), Pct ("mix", "MIX", 0.6), Seed() }, revrev);
    add (FORM, "fpitch", "FORMANT PITCH", "FPIT", "Transposes up to two octaves while keeping the resonance peaks (formants) in place, so voices and instruments keep their natural character. PRESERVE 0% = plain pitch shift.",
         { P ("semis", "PITCH", -24, 24, 7, PInt, "st"), Pct ("preserve", "PRESERVE", 1), Win (0.02, 0.2, 0.06) }, fpitch);
    add (FORM, "fwarp", "FORMANT WARP", "FWRP", "Moves the formants without touching the pitch: SHIFT them up/down, SQUEEZE them around CENTER, or INVERT them (mirror around CENTER). DETAIL = envelope smoothness.",
         { Opt ("mode", "MODE", { "SHIFT", "SQUEEZE", "INVERT" }), P ("amount", "AMOUNT", 0.5, 2, 1.3, 0, "x"), P ("center", "CENTER", 200, 4000, 1000, PExp, {}, Fmt::hz), P ("detail", "DETAIL", 100, 800, 300, PExp, {}, Fmt::hz) }, fwarp);
    add (FORM, "robot", "ROBOT / WHISPER", "ROBO", "ROBOT flattens every phase and re-pitches the sound to a buzzing monotone. WHISPER randomises them into breath.",
         { Opt ("mode", "MODE", { "ROBOT", "WHISPER" }), P ("pitch", "PITCH", 40, 600, 110, PExp, {}, Fmt::hz), Win (0.01, 0.1, 0.03), Seed() }, robot);
    add (FORM, "space", "SPATIAL / HOVER", "SPAC", "Flies the sound around the listener in stereo: CIRCLE, SPIRAL (outward), LISSAJOUS, HOVER (random drifting) or FLY-BY, with distance, head shadow and DOPPLER pitch shift.",
         { Opt ("mode", "PATH", { "CIRCLE", "SPIRAL", "LISSAJOUS", "HOVER", "FLY-BY" }), P ("rate", "RATE", 0.02, 5, 0.3, PExp, "Hz"), Pct ("depth", "DISTANCE", 0.5), Pct ("doppler", "DOPPLER", 0.5), Seed() }, space);

    // ---- DATLAB additions ----
    using namespace procs_new;
    add (NSPEC, "sdelay", "SPECTRAL DELAY", "SDLY", "Every frequency gets its own echo time: TILT delays the lows or the highs more, RANDOM scatters the bins, FEEDBACK makes the echoes cascade into smeared, laser-like trails.",
         { P ("time", "MAX TIME", 0.05, 4, 0.8, PExp, {}, Fmt::time), P ("tilt", "TILT", -1, 1, 0.6), Pct ("random", "RANDOM", 0.3), P ("feedback", "FEEDBACK", 0, 0.95, 0.5, PPct, {}, Fmt::pct),
           Pct ("mix", "MIX", 0.7), Win (0.02, 0.2, 0.05), Seed() }, sdelay, {}, true);
    add (NSPEC, "scram", "BIN SCRAMBLE", "SCRM", "Cuts the spectrum into BANDS that swap places at random and re-shuffle every HOLD: a sound keeps its rhythm while its timbre turns into a broken radio.",
         { P ("bands", "BAND SIZE", 1, 64, 8, PInt, "bins"), Pct ("amount", "AMOUNT", 0.6), Pct ("range", "RANGE", 0.4), P ("hold", "HOLD", 10, 2000, 150, PExp, "ms"), Win (0.02, 0.2, 0.05), Seed() }, scram, {}, true);
    add (NSPEC, "sarp", "SPECTRAL ARP", "SARP", "A resonant band arpeggiates up and down the spectrum in time with the tempo (UP / DOWN / UP-DOWN / RANDOM / OUTSIDE-IN), carving melodies out of pads, noise or drums.",
         { P ("steps", "STEPS", 2, 16, 8, PInt), Opt ("pattern", "PATTERN", { "UP", "DOWN", "UP-DOWN", "RANDOM", "OUTSIDE-IN" }), Opt ("div", "RATE", DIVS, 4),
           P ("lo", "LOW", 40, 2000, 150, PExp, {}, Fmt::hz), P ("hi", "HIGH", 500, 16000, 5000, PExp, {}, Fmt::hz), P ("width", "WIDTH", 0.05, 2, 0.3, PExp, "oct"),
           Pct ("reso", "RESONANCE", 0.4), Pct ("depth", "DEPTH", 0.9), Pct ("glide", "GLIDE", 0.3), Win (0.02, 0.2, 0.05), Seed() }, sarp, {}, true);
    add (NSPEC, "harm", "HARMONIZER", "HARM", "Layers formant-correct pitch-shifted copies into a chord (or two CUSTOM intervals), DETUNEd and SPREAD across the stereo field. Instant choirs, power chords and octave stacks.",
         { Opt ("chord", "CHORD", { "OCTAVES", "POWER", "MAJOR", "MINOR", "SUS2", "MAJ7", "MIN7", "DIM7", "CLUSTER", "ANGELIC", "CUSTOM" }, 2), P ("a", "CUSTOM A", -24, 24, 5, PInt, "st"),
           P ("b", "CUSTOM B", -24, 24, -7, PInt, "st"), P ("detune", "DETUNE", 0, 50, 8, 0, "ct"), Pct ("spread", "SPREAD", 0.8), Pct ("formant", "FORMANT", 1), Pct ("dry", "DRY", 0.8), Pct ("wet", "WET", 0.8), Win (0.02, 0.2, 0.06) }, harm, {}, true);
    add (NSPEC, "shepard", "SHEPARD RISE", "SHEP", "The endless staircase: octave-spaced layers of the looped source glide UP (or DOWN) forever while fading in and out, so the pitch seems to rise without ever getting higher.",
         { Opt ("dir", "DIRECTION", { "UP", "DOWN" }), P ("cycle", "OCTAVE TIME", 0.5, 30, 6, PExp, {}, Fmt::time), P ("layers", "LAYERS", 2, 6, 4, PInt), P ("len", "LENGTH", 2, 60, 16, 0, "s"),
           P ("speed", "READ SPEED", 0, 2, 1, 0, "x"), P ("center", "CENTER", -12, 12, 0, 0, "st"), Pct ("spread", "SPREAD", 0.6) }, shepard, {}, true);
    add (NDRV, "fold", "DRIVE / FOLD", "FOLD", "Oversampled distortion: SOFT (tube-like), FOLD (wave folding, buzzy and metallic), HARD clip, RECTIFY (octave-up fuzz) or FUZZ (asymmetric). STAGES chains it, TONE tames the top.",
         { Opt ("mode", "MODE", { "SOFT", "FOLD", "HARD CLIP", "RECTIFY", "FUZZ" }, 1), P ("drive", "DRIVE", 0, 48, 18, 0, {}, Fmt::db), P ("bias", "BIAS", -1, 1, 0), P ("stages", "STAGES", 1, 4, 1, PInt),
           P ("tone", "TONE", 500, 20000, 9000, PExp, {}, Fmt::hz), Pct ("mix", "MIX", 1) }, fold, {}, true);
    add (NDRV, "ring", "RING MOD", "RING", "Multiplies the sound with a carrier wave (SINE / TRIANGLE / SQUARE / SAW / NOISE) that can sweep from FREQ to END and wobble with the LFO. AM mode keeps more of the original.",
         { Opt ("mode", "MODE", { "RING", "AM" }), Opt ("wave", "WAVE", { "SINE", "TRIANGLE", "SQUARE", "SAW", "NOISE" }), P ("freq", "FREQ", 1, 5000, 220, PExp, {}, Fmt::hz), P ("end", "END FREQ", 1, 5000, 220, PExp, {}, Fmt::hz),
           P ("lforate", "LFO RATE", 0.05, 20, 2, PExp, "Hz"), P ("lfodepth", "LFO DEPTH", 0, 24, 0, 0, "st"), Pct ("stereo", "STEREO", 0.3), Pct ("mix", "MIX", 1), Seed() }, ring, {}, true);
    add (NDRV, "mosh", "DATAMOSH", "MOSH", "Treats the audio as raw 16-bit data and corrupts it, BLOCK by block: endian swaps, bit rotation, dropped bytes, stuck blocks, sorted samples (audio pixel-sorting) and XOR noise. Harsh, digital, unpredictable.",
         { Opt ("op", "OPERATION", { "RANDOM MIX", "ENDIAN SWAP", "BIT ROTATE", "BYTE DROP", "STUCK BLOCK", "SAMPLE SORT", "XOR NOISE" }), P ("block", "BLOCK", 1, 500, 30, PExp, "ms"),
           Pct ("chance", "CHANCE", 0.3), Pct ("intensity", "INTENSITY", 0.5), Pct ("mix", "MIX", 1), Seed() }, mosh, {}, true);
    add (NRHY, "beatrep", "BEAT REPEAT", "BREP", "Tempo-synced repeater: every INTERVAL, with CHANCE, a GRID-long slice is captured and repeated for GATE of the interval, each repeat PITCH and VOLUME decaying. VARIATION changes the grid on the fly.",
         { Opt ("interval", "INTERVAL", DIVS, 1), Opt ("grid", "GRID", DIVS, 4), Pct ("chance", "CHANCE", 0.5), Pct ("gate", "GATE", 0.5), P ("pdecay", "PITCH DECAY", 0, 12, 1, 0, "st"),
           Pct ("vdecay", "VOL DECAY", 0.1), Pct ("variation", "VARIATION", 0.2), P ("offset", "OFFSET", 0, 15, 0, PInt, "/16"), Pct ("mix", "MIX", 1), Opt ("insert", "MODE", { "MIX", "INSERT" }, 1), Seed() }, beatrep, {}, true);
    add (NRHY, "echo", "SHIMMER ECHO", "ECHO", "Tempo-synced ping-pong delay with a PITCH shifter, DAMPing filter and saturation inside the feedback loop: every repeat climbs (or falls) further.",
         { Opt ("sync", "SYNC", SYNC_FREE, 4), P ("time", "TIME (FREE)", 10, 2000, 375, PExp, "ms"), P ("feedback", "FEEDBACK", 0, 0.98, 0.55, PPct, {}, Fmt::pct), P ("pitch", "PITCH", -12, 12, 12, PInt, "st"),
           Pct ("damp", "DAMP", 0.35), Pct ("pingpong", "PING-PONG", 0.8), Pct ("mix", "MIX", 0.5), P ("tail", "TAIL", 0, 20, 6, 0, "s") }, echo, {}, true);
    add (NRHY, "wear", "TAPE WEAR", "WEAR", "A worn cassette: WOW (slow pitch drift) and FLUTTER (fast wobble), SATURATION, HISS, random DROPOUTS and AGE (lost highs).",
         { P ("wow", "WOW", 0, 20, 4, 0, "ms"), P ("wowrate", "WOW RATE", 0.05, 4, 0.5, PExp, "Hz"), P ("flutter", "FLUTTER", 0, 3, 0.4, 0, "ms"), P ("flutrate", "FLUT RATE", 2, 30, 9, PExp, "Hz"),
           Pct ("sat", "SATURATION", 0.4), P ("hiss", "HISS", -80, -20, -54, 0, {}, Fmt::db), Pct ("dropouts", "DROPOUTS", 0.2), Pct ("age", "AGE", 0.35), Seed() }, wear, {}, true);
    add (NRHY, "scratch", "SCRATCH", "SCRT", "Turntablism on the sample, synced to the tempo: BABY, CHIRP, TRANSFORMER, TEAR, FLARE or a RANDOM DJ. POSITION picks the spot on the record, RANGE how far the hand moves.",
         { Opt ("pattern", "PATTERN", { "BABY", "CHIRP", "TRANSFORMER", "TEAR", "FLARE", "RANDOM DJ" }), Opt ("div", "MOTION", DIVS, 3), Pct ("pos", "POSITION", 0), P ("range", "RANGE", 30, 1500, 250, PExp, "ms"),
           P ("bars", "LENGTH", 1, 8, 2, PInt, "bars"), Seed() }, scratch, {}, true);
    add (NRHY, "ramp", "PITCH RAMP", "RAMP", "Glides the pitch from START to END: risers, dive bombs, turntable brakes. TAPE changes the length with the pitch, KEEP LENGTH uses grains so the timing stays put. CURVE shapes the glide.",
         { Opt ("mode", "MODE", { "TAPE", "KEEP LENGTH" }), P ("start", "START", -36, 36, 0, 0, "st"), P ("end", "END", -36, 36, 12, 0, "st"), P ("curve", "CURVE", 0.2, 5, 1.5, PExp, "x"),
           P ("grain", "GRAIN", 20, 200, 70, PExp, "ms") }, ramp, {}, true);
    add (NRHY, "tgate", "TRANCE GATE", "TGAT", "Rhythmic volume chopping on the tempo grid: five classic step patterns, RANDOM or EUCLIDean (HITS over 16 steps), with SWING and shaped ATTACK / RELEASE.",
         { Opt ("pattern", "PATTERN", { "CLASSIC", "OFFBEAT", "GALLOP", "HALF", "DRIVE", "RANDOM", "EUCLID" }), Opt ("div", "STEP", DIVS, 4), P ("hits", "HITS", 1, 16, 5, PInt),
           P ("attack", "ATTACK", 0.5, 50, 2, PExp, "ms"), P ("release", "RELEASE", 5, 500, 80, PExp, "ms"), Pct ("depth", "DEPTH", 1), Pct ("swing", "SWING", 0), Seed() }, tgate, {}, true);
    add (NSPC, "cloud", "CLOUD REVERB", "CLDV", "A huge, modulated 8-line reverb with PRE-DELAY, DAMPing and an octave-up SHIMMER inside the tail. DECAY up to 30 seconds for endless ambient washes.",
         { P ("size", "SIZE", 0.2, 3, 1.4, PExp, "x"), P ("decay", "DECAY", 0.3, 30, 5, PExp, {}, Fmt::time), Pct ("damp", "DAMP", 0.4), Pct ("mod", "MOD", 0.5), Pct ("shimmer", "SHIMMER", 0.3),
           P ("predelay", "PRE-DELAY", 0, 500, 30, 0, "ms"), Pct ("width", "WIDTH", 1), Pct ("mix", "MIX", 0.5) }, cloud, {}, true);
    add (NSPC, "flange", "FLANGE / PHASE", "FLNG", "Jet-plane FLANGER, THROUGH-ZERO flanger, multi-STAGE PHASER or wide CHORUS, with FEEDBACK and stereo SPREAD of the LFO.",
         { Opt ("mode", "MODE", { "FLANGER", "THRU-ZERO", "PHASER", "CHORUS" }), P ("rate", "RATE", 0.01, 10, 0.25, PExp, "Hz"), Pct ("depth", "DEPTH", 0.8), P ("feedback", "FEEDBACK", -0.95, 0.95, 0.6),
           P ("stages", "STAGES", 2, 24, 8, PInt), Pct ("spread", "SPREAD", 0.5), Pct ("mix", "MIX", 0.7) }, flange, {}, true);
    add (NSPC, "wobble", "WOBBLE FILTER", "WOBL", "Resonant LP / BP / HP / NOTCH filter swept by a tempo-synced LFO (SINE, TRIANGLE, SAWs, SQUARE, SAMPLE & HOLD) and an envelope follower. Dubstep wobbles, auto-wah, gated sweeps.",
         { Opt ("type", "TYPE", { "LOW PASS", "BAND PASS", "HIGH PASS", "NOTCH" }), P ("cutoff", "CUTOFF", 40, 12000, 600, PExp, {}, Fmt::hz), Pct ("reso", "RESONANCE", 0.5),
           Opt ("sync", "LFO SYNC", SYNC_FREE, 4), P ("rate", "RATE (FREE)", 0.05, 30, 2, PExp, "Hz"), Opt ("shape", "SHAPE", { "SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE", "S&H" }),
           P ("depth", "DEPTH", 0, 8, 4, 0, "oct"), P ("env", "ENV FOLLOW", -1, 1, 0), Pct ("drive", "DRIVE", 0.2), Pct ("mix", "MIX", 1), Seed() }, wobble, {}, true);
    add (NSPC, "vowel", "VOWEL / TALKBOX", "VOWL", "Formant filters make the sound speak vowels: STATIC, GLIDE from A to B over the length, WOBBLE between them or STEP on the tempo grid. SHIFT moves all formants, VOICE picks male or female.",
         { Opt ("a", "VOWEL A", { "A", "E", "I", "O", "U" }), Opt ("b", "VOWEL B", { "A", "E", "I", "O", "U" }, 3), Opt ("motion", "MOTION", { "STATIC", "GLIDE A\xe2\x86\x92" "B", "WOBBLE", "STEP" }, 2),
           Opt ("div", "RATE", DIVS, 3), Pct ("reso", "RESONANCE", 0.4), P ("shift", "SHIFT", 0.5, 2, 1, PExp, "x"), Opt ("voice", "VOICE", { "MALE", "FEMALE" }), Pct ("mix", "MIX", 1) }, vowel, {}, true);
    add (NSPC, "squash", "SQUASH (OTT)", "OTT", "Aggressive 3-band upward + downward compression: quiet details are pulled up, peaks pushed down, everything gets loud, bright and in your face. Set the CROSSOVERs and band levels.",
         { Pct ("depth", "DEPTH", 0.8), P ("time", "TIME", 0.2, 5, 1, PExp, "x"), Pct ("up", "UPWARD", 1), Pct ("down", "DOWNWARD", 1), P ("xlo", "LOW X-OVER", 40, 800, 120, PExp, {}, Fmt::hz),
           P ("xhi", "HIGH X-OVER", 1000, 10000, 2500, PExp, {}, Fmt::hz), P ("low", "LOW", -12, 12, 0, 0, {}, Fmt::db), P ("midg", "MID", -12, 12, 0, 0, {}, Fmt::db),
           P ("high", "HIGH", -12, 12, 0, 0, {}, Fmt::db), P ("out", "OUTPUT", -24, 12, -4, 0, {}, Fmt::db) }, squash, {}, true);
    add (NSPC, "crystal", "CRYSTALLIZE", "CRYS", "Granular reverse echoes: grains are pitched (PITCH), often played backwards (REVERSE) and fed back into themselves, so tails sparkle and climb in octaves.",
         { P ("size", "GRAIN", 30, 800, 180, PExp, "ms"), P ("delay", "DELAY", 20, 2000, 250, PExp, "ms"), P ("pitch", "PITCH", -24, 24, 12, PInt, "st"), Pct ("reverse", "REVERSE", 0.7),
           P ("feedback", "FEEDBACK", 0, 0.95, 0.55, PPct, {}, Fmt::pct), Pct ("spread", "SPREAD", 0.7), Pct ("mix", "MIX", 0.6), P ("tail", "TAIL", 0, 20, 5, 0, "s"), Seed() }, crystal, {}, true);
    return v;
}
} // namespace

const std::vector<ProcDef>& procs() { static const auto v = build(); return v; }
const ProcDef* findProc (const std::string& id) { for (auto& p : procs()) if (p.id == id) return &p; return nullptr; }
const std::vector<std::pair<std::string, std::vector<std::string>>>& categories()
{
    static const auto cats = []
    {
        std::vector<std::pair<std::string, std::vector<std::string>>> c;
        for (auto& p : procs())
        {
            if (c.empty() || c.back().first != p.category) c.push_back ({ p.category, {} });
            c.back().second.push_back (p.id);
        }
        return c;
    }();
    return cats;
}
Params defaults (const ProcDef& d) { Params p; for (auto& pd : d.params) p.v[pd.id] = pd.def; return p; }

std::string formatValue (const ParamDef& d, double v)
{
    char b[64];
    if (d.isOptions()) { int i = clamp ((int) jr (v), 0, (int) d.options.size() - 1); return d.options[(size_t) i]; }
    switch (d.fmt)
    {
        case Fmt::pct: std::snprintf (b, sizeof b, "%d%%", (int) jr (v * 100)); return b;
        case Fmt::time: if (v < 1) std::snprintf (b, sizeof b, "%d ms", (int) jr (v * 1000)); else std::snprintf (b, sizeof b, "%.2f s", v); return b;
        case Fmt::hz: if (v >= 1000) std::snprintf (b, sizeof b, "%.2f kHz", v / 1000); else std::snprintf (b, sizeof b, "%d Hz", (int) jr (v)); return b;
        case Fmt::db: std::snprintf (b, sizeof b, "%.1f dB", v); return b;
        case Fmt::note:
        {
            static const char* N[] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
            int n = (int) jr (v); std::snprintf (b, sizeof b, "%s%d", N[((n % 12) + 12) % 12], n / 12); return b;
        }
        default: break;
    }
    if (d.isInt()) std::snprintf (b, sizeof b, "%d", (int) jr (v));
    else if (std::abs (v) >= 100) std::snprintf (b, sizeof b, "%.0f", v);
    else if (std::abs (v) >= 10) std::snprintf (b, sizeof b, "%.1f", v);
    else std::snprintf (b, sizeof b, "%.2f", v);
    std::string s = b;
    if (! d.unit.empty()) s += (d.unit == "x" ? "" : " ") + d.unit;
    return s;
}
} // namespace lab
