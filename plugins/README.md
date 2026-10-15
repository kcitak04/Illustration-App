# DATTRACK plugins for Ableton Live

There are two plugins, both VST3 on Windows and VST3 + AU on macOS:

| Plugin | Type | What it does |
|---|---|---|
| **DATLAB** | Audio effect | DATTRACK's **Sample Lab** with all 31 original processes plus **20 new ones**. Every process gets a SHAPE section for more control. You drag the result straight from the plugin window onto an Ableton track. |
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
2. **Pick a process** from the list on the left, then set its knobs. Double-click a knob to reset it, or use **RANDOM** to try something new.
3. **Choose the region to process.** Drag the white handles on the SOURCE waveform.
4. Press **PROCESS**. The result renders in the background, appears in green, and is saved as a 24-bit WAV in `Music/DATLAB/`.
5. **PLAY RESULT** previews it through the track.
6. **Drag the green "DRAG TO ABLETON" tile, or the result waveform itself, onto an Ableton track, clip slot or Drum Rack pad.** Ableton imports the WAV like any other file.
7. **CHAIN** makes the result the new source, so you can stack processes. **UNDO** goes back.

Processes that need a second sample (CONVOLVE, MORPH / VOCODE, MASK) show a row for it. Drop a file on that row, or use **USE RESULT**.

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
./lab_cli --smoke                              # every process and mode renders
# end-to-end sync (fake host + headless Chromium via Playwright):
node plugins/tests/dattrack_sync_test.mjs build/dattrack/dattrack_host_artefacts/Release/dattrack_host /tmp slave --follow-page
```

To refresh the bundled DATTRACK, copy `index.html` from `kcitak04/mobtrack` to `plugins/dattrack/web/dattrack.html`. The bridge uses DATTRACK's internals (`sched`, `player`, `song`, `songToObj`…), so run the sync test after updating.

## Licenses

- The plugin code is original.
- **JUCE 8** is used under the AGPLv3.
- **Ableton Link** is used under the GPLv2+.
- The VST3 SDK (MIT) comes with JUCE.

If you distribute these plugin binaries, distribute them under the AGPLv3, or obtain commercial JUCE and Link licenses.
