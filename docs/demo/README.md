# FactMontage demo project

The demo uses original procedural video, a chroma key scene, a synthesized tone and native text. All eight panel sections are applied in one Kdenlive project. The media and project are created outside the source tree.

[SOURCES.json](SOURCES.json) records their origin and CC0 dedication. The generator records file checksums and the exact media commands in `generated-sources.json`. Screenshots show the real application; CC0 does not change the licences of Kdenlive or its icons.

## Generate the media

Use Python 3 and ffmpeg with libx264, zoompan and lavfi. No Python packages or downloads are needed. The destination must be empty.

```sh
python3 docs/demo/create_demo.py /tmp/factmontage-demo --ffmpeg /app/bin/ffmpeg
```

For a host build, use `--ffmpeg ffmpeg`. The three videos are SDR BT.709, yuv420p, 1920×1080 at 60 fps, six seconds each. The WAV is a six-second 440 Hz tone, PCM at 48 kHz.

## Capture the real dark workspace

The existing Kdenlive regression runner includes the opt-in `[FactMontagePublication]` fixture. Run it alone in a fresh process, using the current FactMontage build and its normal MLT, Qt and resource paths.

A real X11 session or Xvfb at 1920×1200 is required. The fixture selects Breeze Dark through the application's theme menu and checks the palette. Native screen capture includes the monitor's video surface.

```sh
export FACTMONTAGE_DEMO_ROOT=/tmp/factmontage-demo
export QT_QPA_PLATFORM=xcb
export QT_SCALE_FACTOR=1
export STUDIO_PREFIX=/app
export STUDIO_QA_PREFIX=/app
timeout 240s xvfb-run -a -s '-screen 0 1920x1200x24' \
  /path/to/studioregressiontest '[FactMontagePublication]' --reporter compact
```

Replace the runner path with the one from the prepared build. In a Flatpak SDK environment, use the existing QA wrapper and expose the demo directory to it. See the [build guide](../BUILD.md).

The fixture saves `FactMontage-demo.kdenlive`, its managed WAV and STXT assets, nine PNG screenshots and `capture-evidence.json`. It checks real panel actions, effect ownership, native save, monitor frames and the dark palette.

## Review the captures

Open every image before publishing it. Each must show the dark workspace, visible video, timeline and the relevant FactMontage section. The background image must show the orange foreground and white bar over the blue lower layer. The transition must show both sources during the mix.

The background demo uses chroma key. It does not validate person segmentation. The tone demonstrates the loudness workflow; it does not demonstrate voice restoration.

These captures do not replace Undo/Redo, Save/Reopen, Save As, export, installation or Steam Deck acceptance. Those checks need separate evidence from the same candidate.
