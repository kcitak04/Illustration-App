# DATTRACK plugins for Ableton Live

There are two plugins, both VST3 on Windows and VST3 + AU on macOS:

| Plugin | Type | What it does |
|---|---|---|
| **DATLAB** | Audio effect | DATTRACK's **Sample Lab** with all 31 original processes, **20 new ones** and the **343-process CDP collection** (every sound-transforming program of the Composers Desktop Project). Every process gets a SHAPE section for more control. You drag the result straight from the plugin window onto an Ableton track. |
| **DATTRACK** | Instrument | The whole DATTRACK tracker inside Ableton. Its audio plays on the Ableton track, and it syncs as **SLAVE** (follows Ableton) or **MASTER** (drives Ableton through Ableton Link). |

## Install

1. Download the plugins:
   - **Windows:** `DATTRACK-Plugins-Windows`
   - **macOS:** `DATTRACK-Plugins-macOS`

   Get them from the **plugins-latest** release (refreshed by every push to `main`), or from the artifacts of the latest **Build plugins** run under the repository's **Actions** tab.
2. Copy the plugins into the system folder:
   - **Windows:** copy `DATLAB.vst3` and `DATTRACK.vst3` to `C:\Program Files\Common Files\VST3\`.
   - **macOS:**
     - Copy the `.vst3` bundles to `~/Library/Audio/Plug-Ins/VST3/` and the `.component` bundles to `~/Library/Audio/Plug-Ins/Components/`.
     - These builds are not notarized, so remove the download quarantine once in Terminal:
       `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/DAT*.vst3 ~/Library/Audio/Plug-Ins/Components/DAT*.component`
3. In Live, go to **Settings → Plug-Ins**:
   - **Windows:** turn on **Use VST3 Plug-In System Folders**.
   - **macOS:** turn on **Use Audio Units v2** and/or **VST3**.

   Then press **Rescan**. Both plugins appear under **Plug-Ins → DATTRACK**.

Requirements:
- **Windows 10/11:** the Microsoft Edge **WebView2** runtime, which is already part of Windows 10/11.
- **macOS:** 10.15 or newer.

## DATLAB (Sample Lab)

Put DATLAB on any **audio track**. By default the track's audio passes through it (**THRU**).

1. **Choose a source.** Use any of these:
   - Drop an audio file (WAV / AIFF / FLAC / OGG / MP3) anywhere on the window, including clips or files dragged from Ableton's browser.
   - Press **LOAD**.
   - Press **CAPTURE** to record what plays into the track. Press it again to stop; the take becomes the source.
2. **Pick a process** from the list on the left (type in the search box to find one; click a group name to fold it), then set its knobs. Double-click a knob to reset it, or use **RANDOM** to try something new.
3. **Choose the region to process.** Drag the white handles on the SOURCE waveform.
4. Press **PROCESS**. The result renders in the background, appears in green, and is saved as a 24-bit WAV in `Music/DATLAB/`.
5. **PLAY RESULT** previews it through the track.
6. **Drag the green "DRAG TO ABLETON" tile, or the result waveform itself, onto an Ableton track, clip slot or Drum Rack pad.** Ableton imports the WAV like any other file.
7. **CHAIN** makes the result the new source, so you can stack processes. **UNDO** goes back.

Processes that need a second sample (CONVOLVE, MORPH / VOCODE, MASK and the CDP · TWO SOUNDS group) show a row for it. Drop a file on that row, or use **USE RESULT**.

Synced processes follow Ableton's tempo when **BPM: HOST** is on: SPECTRAL ARP, BEAT REPEAT, SHIMMER ECHO, SCRATCH, TRANCE GATE, WOBBLE FILTER and VOWEL.

### The processes

**The original 31, identical to DATTRACK.** The automated parity test renders the same audio sample for sample:

- **SPECTRAL:**
  - Spectral Freeze, Spectral Blur, Morph / Vocode, Spec Tune
  - Stretch / Invert, Spectral Shift, Focus / Reson
  - Mask / Suppress, Spectral Gate, Convolve
- **WAVESET:** Waveset Distort, Waveset Average, Bit Mangler
- **GRANULAR · TIME:**
  - Brassage, Paulstretch, Granular Cloud, Repeats / Bounce
  - Drunk / Scramble, Iterate, Zigzag / Scrub
  - Stutter / Glitch, Slice Shuffle, Tape Stop
- **TEXTURE:** Texture, Shrink / Poly, Resonator, Reverse Reverb
- **FORMANT · PITCH · SPACE:** Formant Pitch, Formant Warp, Robot / Whisper, Spatial / Hover

**The 20 new processes:**

| Group | Process | What it does |
|---|---|---|
| Spectral | **Spectral Delay** | Every frequency bin gets its own echo time (tilt, random, feedback) |
| | **Bin Scramble** | Spectrum bands swap places and re-shuffle every HOLD |
| | **Spectral Arp** | A resonant band arpeggiates through the spectrum in tempo (5 patterns) |
| | **Harmonizer** | Formant-correct pitch-shifted voices in 10 chord shapes or 2 custom intervals, with detune and spread |
| | **Shepard Rise** | Endless rising or falling pitch illusion |
| Drive · Digital | **Drive / Fold** | Soft, wave fold, hard clip, rectify or fuzz, 4× oversampled, up to 4 stages, with tone |
| | **Ring Mod** | Sine / triangle / square / saw / noise carrier, with sweep and LFO, ring or AM |
| | **Datamosh** | Raw 16-bit corruption: endian swap, bit rotate, byte drop, stuck blocks, sample sorting, XOR |
| Rhythm · Time | **Beat Repeat** | Tempo-synced repeats with pitch and volume decay, variation and gate |
| | **Shimmer Echo** | Tempo-synced ping-pong delay with a pitch shifter and saturation in the feedback loop |
| | **Tape Wear** | Wow, flutter, saturation, hiss, dropouts and age |
| | **Scratch** | Baby, chirp, transformer, tear, flare and random DJ patterns, in tempo |
| | **Pitch Ramp** | Risers and dive bombs, either tape-style or keeping the length |
| | **Trance Gate** | Step patterns, random or Euclidean rhythms, with swing and attack/release |
| Space · Filter | **Cloud Reverb** | 8-line modulated reverb with octave-up shimmer, up to 30 s decay |
| | **Flange / Phase** | Flanger, through-zero flanger, multi-stage phaser or chorus |
| | **Wobble Filter** | LP / BP / HP / notch filter with a synced LFO (6 shapes) and an envelope follower |
| | **Vowel / Talkbox** | A-E-I-O-U formant filters, male or female, static / glide / wobble / step |
| | **Squash (OTT)** | 3-band upward + downward compression |
| | **Crystallize** | Reverse, pitched grain echoes that feed back into themselves |

### The CDP collection (343 processes)

Every sound-transforming program of the [Composers Desktop Project](https://github.com/ComposersDesktop/CDP8) (CDP8, LGPL-2.1) is here as a DATLAB process: spectral (phase vocoder), waveset, pitch-synchronous, grain, texture, envelope, filter, two-sound, delay, reverb and spatial processes. Each one follows CDP's algorithm, and its CDP command is shown at the top right of the window (and is searchable: type `distort`, `blur` or `brassage` in the search box).

What changes compared with CDP:

- **No data files.** Where CDP reads a text file (note data, filter banks, time lists, breakpoint curves, pitch files), DATLAB uses controls instead: CHORD / SCALE + ROOT menus, START→END sweeps, LFOs, rhythm patterns on the host tempo, an onset detector, or the built-in pitch tracker.
- **Stereo.** Programs for 8 or 16 speakers are folded to stereo (a virtual speaker ring decoded to left/right).
- **One second sample.** Processes that combine sounds (cross-synthesis, morph, convolution, switching) use the source plus the second sample row.
- **Modes.** CDP's numbered modes are the MODE menu of each process; modes that only differ in units (Hz / MIDI, ratio / semitones) became one control.
- **Left out:** CDP's housekeeping, file-info, data-editing, multi-output and synthesis-only programs, which don't turn a sample into a new sample, and 9 Soundloom-only programs whose source is not in CDP8.

<details><summary><b>CDP · SPECTRAL</b> (5)</summary>

| Process | CDP |
|---|---|
| SPECTRAL GAIN | `spec gain` |
| SPECTRAL LIMIT | `spec gate` |
| HARMONICS ONLY | `spec bare` |
| SPECTRAL CLEAN | `spec clean` |
| SPECTRAL MAGNIFY | `spec magnify` |

</details>

<details><summary><b>CDP · SPECTRAL · BLUR</b> (10)</summary>

| Process | CDP |
|---|---|
| SPECTRAL AVERAGE | `blur avrg` |
| TIME BLUR | `blur blur` |
| SUPPRESS LOUDEST | `blur suppress` |
| SPECTRAL CHORUS | `blur chorus` |
| SPECTRAL DRUNK | `blur drunk` |
| WINDOW SHUFFLE | `blur shuffle` |
| SPECTRAL WEAVE | `blur weave` |
| SPECTRAL NOISE | `blur noise` |
| SPECTRAL SCATTER | `blur scatter` |
| SPECTRAL SPREAD | `blur spread` |

</details>

<details><summary><b>CDP · SPECTRAL · FOCUS · WARP</b> (15)</summary>

| Process | CDP |
|---|---|
| SPECTRAL ACCUMULATE | `focus accu` |
| SPECTRAL EXAGGERATE | `focus exag` |
| FOCUS ON PEAKS | `focus focus` |
| OCTAVE FOLD | `focus fold` |
| FREEZE POINTS | `focus freeze` |
| SPECTRAL HOLD | `focus hold` |
| SPECTRAL STEP | `focus step` |
| BAND FREQ SHIFT | `strange shift` |
| ENVELOPE GLISSANDI | `strange glis` |
| HARMONIC WAVER | `strange waver` |
| PITCH/TIME WARP | `strange warp` |
| SPECTRAL INVERT | `strange invert` |
| SPECTRUM STRETCH | `stretch spectrum` |
| SPECTRAL TIMESTRETCH | `stretch time 1` |
| BIN EXTRACT | `pvoc extract` |

</details>

<details><summary><b>CDP · SPECTRAL · PITCH</b> (12)</summary>

| Process | CDP |
|---|---|
| ALTERNATE HARMONICS | `pitch altharms` |
| OCTAVE MOVE | `pitch octmove` |
| SPLIT TRANSPOSE | `pitch transp` |
| HARMONIC TUNE | `pitch tune` |
| HARMONIC PICK | `pitch pick` |
| FORMANT CHORD | `pitch chordf` |
| SPECTRAL CHORD | `pitch chord` |
| SPECTRAL TRANSPOSE | `repitch transpose` |
| FORMANT TRANSPOSE | `repitch transposef` |
| PITCH SYNTH | `repitch synth` |
| PITCH VOWELS | `repitch vowels` |
| PITCH CONTOUR FX | `repitch exag/invert/quantise/randomise/smooth/vibrato/approx + transposef` |

</details>

<details><summary><b>CDP · SPECTRAL II</b> (26)</summary>

| Process | CDP |
|---|---|
| SPECTRAL FILTER | `hilite filter` |
| SPECTRAL VOWELS | `hilite vowels` |
| SPECTRAL GRAPHIC EQ | `hilite greq 1-2` |
| SPECTRAL BANDS | `hilite band` |
| SPECTRAL ARPEGGIO | `hilite arpeg 1-8` |
| SPECTRAL PLUCK | `hilite pluck` |
| SPECTRAL TRACE | `hilite trace 1-4` |
| BLUR + TRACE | `hilite bltr` |
| SPECTRAL DENOISE | `speclean clean / specnu clean / specnu subtract` |
| HARMONIC REMOVE | `specnu remove 1-2` |
| SPECTRAL PIVOT | `specnu slice 5` |
| WINDOW SHUFFLE | `specnu rand` |
| SPECTRAL SQUEEZE | `specnu squeeze` |
| HIGH BLUR | `caltrain caltrain` |
| FRACTAL TRANSPOSE | `fractal spectrum` |
| CHANGE RINGING | `speculate speculate` |
| PARTIAL SUPPRESS | `suppress partials` |
| SPECTRAL MATRIX | `matrix matrix 1-4` |
| GLISTEN | `glisten glisten` |
| SELF-SIMILAR | `selfsim selfsim` |
| SUPER ACCUMULATE | `superaccu superaccu 1-4` |
| TUNE VARYING | `tunevary tunevary` |
| SPECTRAL FOLD | `specfold specfold 1-3` |
| DECOHERED STRETCH | `spectstr stretch time` |
| STATIC AUTOTUNE | `spectune tune 1-3` |
| PEDAL TUNE | `spectune tune 5` |

</details>

<details><summary><b>CDP · SPECTRAL II · FORMANTS</b> (18)</summary>

| Process | CDP |
|---|---|
| FORMANT NARROW | `specfnu specfnu 1` |
| FORMANT SQUEEZE | `specfnu specfnu 2` |
| FORMANT INVERT | `specfnu specfnu 3` |
| FORMANT ROTATE | `specfnu specfnu 4` |
| SPECTRAL NEGATIVE | `specfnu specfnu 5` |
| FORMANT SUPPRESS | `specfnu specfnu 6` |
| FORMANT MOVE | `specfnu specfnu 8-9` |
| FORMANT ARPEGGIO | `specfnu specfnu 10` |
| FORMANT OCTAVER | `specfnu specfnu 11` |
| FORMANT TRANSPOSE | `specfnu specfnu 12` |
| FORMANT FREQ SHIFT | `specfnu specfnu 13` |
| FORMANT RESPACE | `specfnu specfnu 14` |
| FORMANT PARTIAL RAND | `specfnu specfnu 19` |
| PITCH INVERT | `specfnu specfnu 15` |
| PITCH EXAGGERATE | `specfnu specfnu 16` |
| PITCH QUANTISE | `specfnu specfnu 17` |
| PITCH RANDOMISE | `specfnu specfnu 18` |
| SINE SPEECH | `specfnu specfnu 23` |

</details>

<details><summary><b>CDP · TWO SOUNDS</b> (40)</summary>

| Process | CDP |
|---|---|
| PITCH + FORMANTS | `combine make` |
| SPECTRAL SUM | `combine sum` |
| SPECTRAL SUBTRACT | `combine diff` |
| WINDOW INTERLEAVE | `combine interleave` |
| SPECTRAL MAX | `combine max` |
| SPECTRAL MEAN | `combine mean` |
| SPECTRAL CROSS | `combine cross` |
| FORMANT PUT | `formants put` |
| FORMANT VOCODER | `formants vocode` |
| SPECTRAL GLIDE | `morph glide` |
| SPECTRAL BRIDGE | `morph bridge` |
| SPECTRAL MORPH | `morph morph` |
| WAVECYCLE INTERACT | `distort interact` |
| SOUND SEQUENCER | `extend sequence2` |
| GRAIN ALIGN | `grain align` |
| ENVELOPE TRANSFER | `envel impose 1 / envel replace 1` |
| LEVEL MATCH | `modify loudness 5` |
| CROSS MODULATE | `modify radical 6` |
| SAUSAGE | `modify sausage` |
| CONVOLVE | `modify convolve 1-2 / fastconv` |
| SOUND SWITCH | `sfedit twixt` |
| ONSET SWITCH | `sfedit sphinx` |
| BALANCE MIX | `submix balance` |
| CROSSFADE | `submix crossfade` |
| WAVESET MORPH | `submix inbetween2` |
| SUBTRACT | `subtract` |
| FROZEN FORMANT | `oneform put` |
| PITCH + ONE FORMANT | `oneform combine` |
| PARTIAL MORPH | `specross partials` |
| SPECTRAL ENVELOPE | `specenv specenv` |
| PEAK MORPH | `newmorph newmorph` |
| PEAK TUNE | `newmorph newmorph2` |
| SPECTRAL SPHINX | `specsphinx specsphinx` |
| SPECTRAL TWIN | `spectwin spectwin` |
| FOF IMPOSE | `psow impose` |
| FOF INTERLEAVE | `psow interleave` |
| FOF REPLACE | `psow replace` |
| SYLLABLE SWAP | `extspeak 1-18` |
| PACKET STREAM | `pulser multi` |
| DOUBLE SPIN | `spin quad` |

</details>

<details><summary><b>CDP · WAVESET · PSOLA</b> (54)</summary>

| Process | CDP |
|---|---|
| WAVESET REFORM | `distort reform 1-7` |
| WAVESET EXAGGERATE | `distort reform 8` |
| WAVESET ENVELOPE | `distort envel 1-4` |
| WAVESET AVERAGE | `distort average` |
| WAVESET OMIT | `distort omit` |
| WAVESET MULTIPLY | `distort multiply` |
| WAVESET DIVIDE | `distort divide` |
| WAVESET HARMONICS | `distort harmonic` |
| WAVESET FRACTAL | `distort fractal` |
| WAVESET REVERSE | `distort reverse` |
| WAVESET SHUFFLE | `distort shuffle` |
| WAVESET REPEAT | `distort repeat` |
| WAVESET REPEAT SKIP | `distort repeat2` |
| WAVESET REPEAT LIM | `distort replim` |
| WAVESET INTERPOLATE | `distort interpolate` |
| WAVESET DELETE | `distort delete 1-3` |
| WAVESET REPLACE | `distort replace` |
| WAVESET TELESCOPE | `distort telescope` |
| WAVESET FILTER | `distort filter 1-3` |
| WAVESET PITCH WARP | `distort pitch` |
| OVERLOAD | `distort overload 1-2` |
| PULSE TRAIN | `distort pulsed 1-3` |
| NOISE CUT | `sfedit noisecut` |
| FOF CONSTRUCT | `fofex construct 1-7` |
| GRAIN STRETCH | `psow stretch` |
| GRAIN DUPLICATE | `psow dupl` |
| GRAIN DELETE | `psow delete` |
| GRAIN STRETCH TRANSP | `psow strtrans` |
| GRAIN GRAB | `psow grab` |
| GRAIN INTERPOLATE | `psow interp` |
| VOICE FEATURES | `psow features 1-2` |
| GRAIN SYNTH | `psow synth 1-5` |
| GRAIN SPLIT | `psow split` |
| GRAIN SPACE | `psow space` |
| GRAIN SUSTAIN | `psow sustain` |
| GRAIN SUSTAIN 2 | `psow sustain2` |
| GRAIN HARMONICS | `psow reinforce 1` |
| GRAIN INHARMONIC | `psow reinforce 2` |
| TWEET | `tweet tweet 1-3` |
| CLIP | `clip clip 1` |
| HALF-WAVE CLIP | `clip clip 2` |
| QUIRK | `quirk quirk 1-2` |
| HALF-CYCLE SHIFT | `distshift distshift 1-2` |
| PROGRESSIVE FOLD | `distwarp distwarp 1-2` |
| WAVESET REPEAT SPLICE | `distrep distrep 1-2` |
| WAVESET REPEAT DUR | `distortt repeat` |
| WAVESET OCTAVER | `distmore double` |
| SORT BY BRIGHTNESS | `distmore bright 1-3` |
| SEGMENTS BACKWARDS | `distmore segsbkwd 1-9` |
| SEGMENT ZIGZAG | `distmore segszig 1-3` |
| WAVESET MORPH MARKS | `distmark distmark 1-2` |
| FRACTAL TRANSPOSE | `fractal wave 1` |
| WAVESET SCRAMBLE | `scramble scramble 1-14` |
| SPLINTER | `splinter splinter 1-4` |

</details>

<details><summary><b>CDP · EXTEND · TEXTURE</b> (36)</summary>

| Process | CDP |
|---|---|
| ZIGZAG READ | `extend zigzag` |
| LOOP ADVANCE | `extend loop` |
| SCRAMBLE EXTEND | `extend scramble` |
| FLUID ITERATE | `extend iterate` |
| ITERATIVE FREEZE | `extend freeze` |
| DRUNKEN WALK | `extend drunk` |
| STEP SEQUENCE | `extend sequence` |
| BACK TO BACK | `extend baktobak` |
| DOUBLETS | `extend doublets` |
| REPETITIONS | `extend repetitions` |
| FRACTAL PERMUTE | `hfperm delperm2` |
| TEXTURE SIMPLE | `texture simple / texmchan` |
| TEXTURE TIMED | `texture timed` |
| TEXTURE GROUPS | `texture grouped` |
| TEXTURE TIMED GROUPS | `texture tgrouped` |
| TEXTURE DECORATED | `texture decorated / predecor / postdecor` |
| TEXTURE ORNATE | `texture ornate / preornate / postornate` |
| TEXTURE MOTIFS | `texture motifs / motifsin` |
| TEXTURE TIMED MOTIFS | `texture tmotifs / tmotifsin` |
| FREEZE ITERATE | `freeze freeze` |
| ITERATE SPREAD | `mchiter iter` |
| ZIGZAG SPREAD | `mchzig zag` |
| HOVER | `hover hover` |
| HOVER WAVES | `hover2 hover2` |
| FRACTALISE | `frfractal (fractal fractal)` |
| POLY CYCLES | `ceracu ceracu` |
| DISCRETE ECHO | `sfecho echo` |
| FOF LINE | `iterfof iterfof` |
| ITERATE LINE | `iterline iterline / iterlinef` |
| SPATIAL SYNCOPATE | `madrid madrid` |
| FOCUS SHIFTER | `shifter shifter` |
| SHRINK | `shrink shrink` |
| BOUNCE | `bounce bounce` |
| CASCADE | `cascade cascade 1-10` |
| ELEMENT REPEATER | `repeater repeater 1-3` |
| TESSELATE | `tesselate tesselate` |

</details>

<details><summary><b>CDP · GRAIN · RHYTHM</b> (28)</summary>

| Process | CDP |
|---|---|
| GRAIN OMIT | `grain omit` |
| GRAIN DUPLICATE | `grain duplicate` |
| GRAIN REORDER | `grain reorder` |
| GRAIN REPITCH | `grain repitch 1-2` |
| GRAIN RERHYTHM | `grain rerhythm 1-2` |
| GRAIN REMOTIF | `grain remotif 1-2` |
| GRAIN TIMEWARP | `grain timewarp` |
| GRAIN REPOSITION | `grain reposition` |
| GRAIN REVERSE | `grain reverse` |
| TROUGH GRAINS | `grain grev 1-5, 7` |
| ITERATIVE EXTEND | `grain r_extend 1-2` |
| NOISE EXTEND | `grain noise_extend` |
| GRAIN EXTEND | `grainex extend` |
| PEAK CHOP | `envnu peakchop 1` |
| EVENTS TO TEMPO | `retime retime 1, 4` |
| EVENT SHAVE | `retime retime 3` |
| EVENT SPEED | `retime retime 5` |
| EVENT PATTERN | `retime retime 6-7` |
| EVENT REPEAT | `retime retime 8` |
| EVENT MASK | `retime retime 9` |
| EVENT LEVELS | `retime retime 10` |
| SYLLABLE MACHINE | `envspeak 1-24` |
| FORMANT SORT | `fturanal synth 1-10` |
| WAVE PACKETS | `packet packet 1-2` |
| DVD WIND | `dvdwind dvdwind` |
| ELEMENT SORTER | `sorter sorter 1-5` |
| STUTTER | `stutter stutter` |
| MOTOR | `motor motor 1-9` |

</details>

<details><summary><b>CDP · GRAIN · BRASSAGE</b> (8)</summary>

| Process | CDP |
|---|---|
| BRASSAGE PITCH | `modify brassage 1` |
| BRASSAGE STRETCH | `modify brassage 2` |
| BRASSAGE REVERB | `modify brassage 3` |
| BRASSAGE SCRAMBLE | `modify brassage 4` |
| BRASSAGE GRANULATE | `modify brassage 5` |
| BRASSAGE FULL | `modify brassage 6` |
| BRASSAGE MONTY | `modify brassage 7` |
| WRAPPAGE | `wrappage wrappage` |

</details>

<details><summary><b>CDP · GRAIN · TEXTURE</b> (7)</summary>

| Process | CDP |
|---|---|
| FRACTURE | `fracture fracture 1-2` |
| NEW TEXTURE | `newtex newtex 1-3` |
| BROWNIAN | `brownian motion 1-2` |
| CRYSTAL ROTATE | `crystal rotate 1-9` |
| PULSER SYNTH | `pulser pulser 1` |
| PULSER STREAM | `pulser pulser 2-3` |
| ROTOR | `rotor rotor 1-3` |

</details>

<details><summary><b>CDP · ENVELOPE</b> (23)</summary>

| Process | CDP |
|---|---|
| SHAPED ENVELOPE | `envel impose 3` |
| REPLACE ENVELOPE | `envel replace 3` |
| ENV NORMALISE | `envel warp 1` |
| ENV REVERSE | `envel warp 2` |
| ENV EXAGGERATE | `envel warp 3` |
| ENV ATTENUATE | `envel warp 4` |
| ENV LIFT | `envel warp 5` |
| ENV TIMESTRETCH | `envel warp 6` |
| ENV FLATTEN | `envel warp 7` |
| ENV GATE | `envel warp 8` |
| ENV INVERT | `envel warp 9` |
| ENV LIMIT | `envel warp 10` |
| ENV CORRUGATE | `envel warp 11` |
| ENV EXPAND | `envel warp 12` |
| ENV TRIGGER | `envel warp 13` |
| ENV CEILING | `envel warp 14` |
| ENV DUCK | `envel warp 15` |
| DOVETAIL | `envel dovetail 1` |
| CURTAIL | `envel curtail 1` |
| SWELL | `envel swell` |
| ATTACK BOOST | `envel attack 1` |
| PLUCK | `envel pluck` |
| ENV TREMOLO | `envel tremolo 1` |

</details>

<details><summary><b>CDP · FILTER</b> (10)</summary>

| Process | CDP |
|---|---|
| FIXED EQ | `filter fixed 1` |
| LO/HI PASS | `filter lohi 1` |
| VARIABLE FILTER | `filter variable 2` |
| SWEEP FILTER | `filter sweeping 2` |
| FILTER BANK | `filter bank 1` |
| CHORD BANK | `filter userbank 1` |
| PROGRESSION BANK | `filter varibank 1` |
| PARTIAL BANK | `filter varibank2 1` |
| ITERATED FILTER | `filter iterated 1` |
| PHASING | `filter phasing 2` |

</details>

<details><summary><b>CDP · DYNAMICS</b> (13)</summary>

| Process | CDP |
|---|---|
| LEVEL / PHASE | `modify loudness 2` |
| SHUDDER | `modify shudder` |
| DEGLITCH | `housekeep deglitch` |
| CONSTRICT | `constrict` |
| EXP DECAY | `envnu expdecay` |
| GATE | `gate gate 1` |
| SPIKE | `spike spike` |
| CANTOR HOLES | `cantor set 1` |
| CANTOR WOBBLE | `cantor set 3` |
| SHARP TREMOLO | `tremolo tremolo 1` |
| DECAY TREMOLO | `tremenv` |
| LEVELLER | `flatten flatten` |
| VERGES | `verges verges` |

</details>

<details><summary><b>CDP · SPACE · PAN</b> (19)</summary>

| Process | CDP |
|---|---|
| PAN | `modify space 1` |
| PAN CURVE | `modify scaledpan` |
| STEREO NARROW | `modify space 2 / modify space 4` |
| FLUTTER | `flutter flutter` |
| FRAME ROTATE | `frame shift 1-2` |
| RING PAN | `mchanpan 1` |
| EVENT SWITCH | `mchanpan 2, 10` |
| SPREAD | `mchanpan 3-4` |
| ANTIPHONY | `mchanpan 5-6` |
| RING ROTATE | `mchanpan 9` |
| SPIN | `spin stereo 1-3` |
| B-FORMAT PAN | `abfpan` |
| TANGENT | `tangent onefile / twofiles / sequence / list` |
| CRUMBLE | `crumble sound 1-2` |
| SPACE SHRED | `mchshred shred 1-2` |
| PHASOR | `phasor phasor` |
| MONO TO STEREO | `tostereo tostereo` |
| STEREO ENHANCE | `phase phase 2` |
| PHASE INVERT | `phase phase 1 / chanphase` |

</details>

<details><summary><b>CDP · SPACE · DELAY</b> (9)</summary>

| Process | CDP |
|---|---|
| ECHO / COMB | `modify revecho 1 / delay` |
| MOD DELAY | `modify revecho 2` |
| STADIUM ECHO | `modify revecho 3 / mchanrev` |
| TUNED DELAY | `newdelay newdelay` |
| TAP DELAY | `tapdelay (tdelay)` |
| ALLPASS | `allpass` |
| LP COMB | `lpcomb` |
| REVERB | `reverb` |
| ROOM REVERB | `rmverb` |

</details>

<details><summary><b>CDP · SPACE · MODIFY</b> (10)</summary>

| Process | CDP |
|---|---|
| VARISPEED | `modify speed 1-2 / strans multi 1-2` |
| ACCELERATE | `modify speed 5 / strans multi 3` |
| TAPE VIBRATO | `modify speed 6 / strans multi 4` |
| REVERSE | `modify radical 1` |
| SHRED | `modify radical 2` |
| SCRUB | `modify radical 3` |
| LOSE RESOLUTION | `modify radical 4` |
| RING MOD | `modify radical 5` |
| QUANTISE | `modify radical 7` |
| STACK | `modify stack` |

</details>

**SHAPE controls every process:**
- **IN GAIN** and **REV IN** act before the process.
- After the process:
  - DRY/WET
  - LOW CUT, HIGH CUT
  - stereo WIDTH
  - FADE IN, FADE OUT
  - LOOP XFADE, a crossfade that makes the result loop seamlessly
  - REV OUT, TRIM silence, NORMALIZE
  - OUT GAIN, MAX LENGTH

## DATTRACK (tracker + sync)

Put DATTRACK on a **MIDI track**. The app starts by itself.

- **Your song lives in the Ableton set.** It's saved and loaded with the project automatically.
- **DATTRACK keeps playing when the plugin window is closed.**
- **Audio goes into Ableton.** DATTRACK's output plays on the track (and through Ableton's mixer and effects). The plugin reports its buffer as latency, so Ableton's delay compensation keeps it aligned with the other tracks.
- **MIDI IN:** notes on the track play the instrument selected in DATTRACK (MIDI C4 = DATTRACK C-4).
- **Exports:** SAVE and EXPORT in DATTRACK's PREF tab (WAV / AIFF / FLAC / Opus, stems, .mtk, .MOD) go to `Music/DATTRACK/`. The newest one appears on the tile at the top right; **drag it onto a track.**

### Sync: master and slave

| Mode | Who leads | What happens |
|---|---|---|
| **SLAVE** (default) | Ableton | DATTRACK follows Ableton's **tempo, play / stop and position**. One beat is 4 rows, and the arrangement position maps onto the song, so looping a region in Ableton loops DATTRACK too. Fxx tempo commands are ignored while slaved; speed (ticks per row) changes keep the beat. |
| **MASTER** | DATTRACK | DATTRACK's **tempo and play / stop drive Ableton through Ableton Link**. In Live, open **Settings → Link, Tempo & MIDI**, turn on **Show Link Toggle** and **Start Stop Sync**, then click **LINK** in Live's control bar. Pressing play in DATTRACK now starts Live on the same beat, and changing DATTRACK's BPM changes Live's tempo. Stopping Live stops DATTRACK. The bar shows how many Link peers are connected. |
| **FREE** | nobody | DATTRACK runs on its own clock. |

**Export / Freeze:** Ableton normally renders an export faster than real time, but a web app can only play in real time. When DATTRACK is in the set, the plugin makes Ableton's export wait for it. The export then takes about as long as the song, and nothing is cut out.

**LATENCY** is the buffer between DATTRACK and Ableton. Ableton compensates for it, so it only matters when you play live through MIDI IN. Use 150 ms on a fast machine; raise it to 400 / 700 ms if you hear dropouts. **NUDGE** fine-tunes the sync in milliseconds.

### How it works (in case something goes wrong)

The plugin contains the DATTRACK web app (`dattrack/web/dattrack.html`, pinned from `kcitak04/mobtrack`) and runs it in the system web view: WebView2 on Windows, WKWebView on macOS. The plugin serves it from a private web server on `127.0.0.1` (port 47811 or the next free one). A small bridge script (`dt-bridge.js`) does the rest:

- It creates DATTRACK's AudioContext at Ableton's sample rate **without a speaker output**.
- It taps the master output in an AudioWorklet and streams it into the plugin.
- The plugin resamples by a few hundred ppm to follow any clock difference.
- Each audio packet's reply carries Ableton's transport. The bridge schedules DATTRACK's rows onto that grid.

The automated test, which uses a fake host with the real page in headless Chromium, lands every metronome click within 1 ms of the host's beat, including after a loop jump.

- **"DATTRACK audio is paused"**: on macOS the web view may need one click into the DATTRACK window before audio starts.
- **Port conflicts:** the bar says so if ports 47811–47850 are all taken.
- **RELOAD** restarts the page; the song is kept.

## Build from source

You need CMake ≥ 3.22 and a C++17 compiler:
- **Windows:** Visual Studio 2022 and the WebView2 NuGet package:
  `nuget install Microsoft.Web.WebView2 -Version 1.0.1901.177 -OutputDirectory C:\nuget`
- **macOS:** Xcode.

JUCE 8 and Ableton Link are downloaded automatically.

```bash
cmake -S plugins -B build -DJUCE_WEBVIEW2_PACKAGE_LOCATION=C:/nuget   # (Windows only option)
cmake --build build --config Release --target DATLAB_All DATTRACK_All
```

Tests:

```bash
g++ -O2 -std=c++17 -o lab_cli plugins/tests/lab_cli.cpp plugins/datlab/Source/dsp/*.cpp
node plugins/tests/parity.mjs ./lab_cli /tmp   # C++ vs the DATTRACK JS worker, sample by sample
./lab_cli --smoke [id-prefix]                  # every process and mode renders, registry sanity checks
./lab_cli --list                               # category | id | name | CDP command
./lab_cli --fuzz 20                            # odd inputs and random / NaN parameters (build with -fsanitize=address,undefined)
# end-to-end sync (fake host + headless Chromium via Playwright):
node plugins/tests/dattrack_sync_test.mjs build/dattrack/dattrack_host_artefacts/Release/dattrack_host /tmp slave --follow-page
# regression tests (song survives start-up and reloads, stalls go silent, count-in, offline export):
node plugins/tests/dattrack_regression_test.mjs build/dattrack/dattrack_host_artefacts/Release/dattrack_host /tmp
```

The bundled copy contains one fix that `kcitak04/mobtrack` (and `mobtrack-live`) still need. Since the "Rename to DATTRACK" commit, a comment in `songToObj` swallows `title, bpm, speed, order`, so saved projects and autosaves lose their tempo, title and pattern order.

To refresh the bundled DATTRACK, copy `index.html` from `kcitak04/mobtrack` to `plugins/dattrack/web/dattrack.html`. The bridge uses DATTRACK's internals (`sched`, `player`, `song`, `songToObj`…), so run the sync test after updating.

## Licenses

- The plugin code is original. The CDP collection ports algorithms of the Composers Desktop Project (CDP8, © CDP / Trevor Wishart and contributors, LGPL-2.1).
- **JUCE 8** is used under the AGPLv3.
- **Ableton Link** is used under the GPLv2+.
- The VST3 SDK (MIT) comes with JUCE.

If you distribute these plugin binaries, distribute them under the AGPLv3, or obtain commercial JUCE and Link licenses.
