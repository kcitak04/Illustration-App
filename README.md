# Cover Studio

> **Also in this repository:** [`plugins/`](plugins/README.md) has two **Ableton Live plugins** (VST3, plus AU on macOS):
> - **DATLAB**, DATTRACK's Sample Lab with 394 processes (including the whole CDP collection) and drag-and-drop of the result into Ableton.
> - **DATTRACK**, the tracker itself, with master/slave sync.

An Android app for designing album covers with a stylus. It has two studios that share one square artboard:

- **Vector Studio** works like Adobe Illustrator. You draw editable vector artwork with pressure-sensitive strokes, Bézier paths, shapes and type.
- **Photo Studio** works like Photoshop. You bring your illustration in as a layer, then use photos, blend modes, masks, adjustments and filters on it.

Switch between them with the **Vector | Photo** toggle at the top of the editor.

## Install on your phone

1. Open the repository's **Actions** tab on GitHub and pick the latest **Build APK** run.
2. Download the `CoverStudio-apk` artifact and unzip it. Every push to `main` also updates the **latest** release, where you can download `CoverStudio.apk` straight from your phone.
3. Open `CoverStudio.apk` on the phone. If asked, allow *Install unknown apps* for your browser or file manager.

The app needs Android 10 or newer. It uses no network access and asks for no permissions. Exports go to `Pictures/CoverStudio` (PNG/JPEG) and `Download/CoverStudio` (SVG).

## Stylus and gestures

| Action | How |
|---|---|
| Draw | Stylus (pressure controls size or opacity) |
| Zoom and pan | Pinch or drag with two fingers |
| Palm rejection | Default **Auto** mode: once the stylus touches the screen, fingers only navigate |
| Undo / redo | Tap with two fingers / three fingers, or use the buttons |
| Eyedropper | Hold the stylus side button (S Pen and similar) while tapping |
| Erase | Flip the stylus to its eraser end, if it has one |
| Brush cursor | Hover the stylus over the screen |

Input mode, pressure curve and tap gestures are under **⋯ → Stylus & input settings**.

## Vector Studio (Illustrator-style)

- **Tools**
  - Select/transform (move, scale and rotate with handles; snaps to the artboard centre and edges)
  - Direct selection, for editing anchor points and Bézier handles
  - Pen (tap to add a corner, drag to add a curve)
  - Pencil (freehand, smoothed into an editable path)
  - Pressure brush
  - Rectangle (rounded corners), ellipse, polygon/star, line
  - Type
  - Object eraser
  - Eyedropper
  - Hand
- **Styling:** fill and stroke colours (hex, HSV, alpha, swatches), linear and radial gradients, stroke width, opacity and 17 blend modes per object.
- **Type:** 12 system fonts, or import your own `.ttf`/`.otf`. You can set bold, italic, alignment, tracking and leading.
- **Objects:** duplicate, delete, arrange, flip, rotate, align to the artboard, and convert shapes to editable paths.
- **Layers:** show or hide, lock, opacity, reorder, rename, duplicate, and move the selection to another layer. You can also set the artboard background.
- **Export:** PNG at full size or with a transparent background, and **SVG**.
- **Send to Photo Studio** in one of three ways:
  - As a *linked* layer: it re-renders automatically when you change the vector art, like a smart object.
  - As a linked layer with the background included.
  - As a flattened pixel layer.

## Photo Studio (Photoshop-style)

- **Layers:** blend modes, opacity, lock, reorder, duplicate, merge down and flatten. **Layer masks** can be painted, inverted, applied, or made from a selection. **Linked illustration** layers stay connected to your vector art.
- **Tools**
  - Move/free transform (scale, rotate, flip, fit or fill the canvas)
  - Marquee (rectangle or ellipse)
  - Lasso
  - Magic wand
  - Brush and eraser (size, hardness, opacity, flow, pressure → size and opacity)
  - Smudge
  - Clone stamp
  - Gradient (linear or radial, can fade to transparent)
  - Paint bucket
  - Eyedropper
  - Hand
- **Selections:** new, add or subtract; select all, deselect, invert, feather, select layer pixels, layer via copy or cut, delete, and fill.
- **Adjustments** (live preview):
  - Brightness/Contrast, Levels, Hue/Saturation, Vibrance
  - Exposure/Temperature
  - Black & White, Sepia
  - Gradient Map/Duotone, Colour Fill Overlay
  - Invert, Posterize, Threshold
- **Filters** (live preview):
  - Gaussian, Motion and Zoom blur
  - Sharpen (unsharp mask)
  - Film grain/noise
  - Chromatic aberration, Glitch, Halftone
  - Pixelate, Vignette
  - Drop shadow/glow, Emboss

  Filters and adjustments apply only inside the selection when there is one.
- **Place:** photos from your gallery (fill or fit the canvas) and your illustration.
- **Image:** flip the canvas, or rotate a square canvas by 90°.
- **Export:** PNG, JPEG, or a 1400 px JPEG.

## Projects

The home screen lists your covers with thumbnails. Artboard presets are 3000×3000 (the streaming standard), 4000, 2400, 1400, or a custom size. Projects save automatically when you leave the editor or switch apps.

## Building from source

```bash
./gradlew assembleDebug      # app/build/outputs/apk/debug/app-debug.apk
./gradlew assembleRelease    # app/build/outputs/apk/release/app-release.apk (debug-signed)
```

You need JDK 17 and the Android SDK (compileSdk 35). The app uses only the Android framework and Kotlin: no AndroidX and no third-party libraries. All UI is built in code.
