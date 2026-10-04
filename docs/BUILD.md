# Build FactMontage

[Русская версия](BUILD_RU.md) · [Back to FactMontage](../README.md)

Build on a native Linux x86-64 filesystem as a non-root user. WSL2 can be used for development. Flatpak output, caches, downloaded sources and models stay outside Git. The build has two independent outputs: an unchanged reference editor and the modified editor.

## Pinned inputs

- Kdenlive 26.08.0 and the upstream Flatpak manifest: [Studio/upstream/sources.json](../Studio/upstream/sources.json).
- KDE Platform and SDK 6.10, LLVM 21 extension 25.08, each at the commit in that file.
- Additional libraries and models: [Background/SOURCES.json](../Background/SOURCES.json) and [Subtitles/SOURCES.json](../Subtitles/SOURCES.json).
- Test-only Python dependencies: [test-requirements.txt](../Studio/scripts/test-requirements.txt).

The source URLs are public. No access to the former private repository is required.

## Prepare the environment explicitly

Install Git, Python 3, CMake, Ninja, a C/C++ compiler, pkg-config, Flatpak and flatpak-builder using your distribution. Install the KDE runtime, SDK and LLVM extension from Flathub in the build user's installation:

```sh
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.10 org.kde.Sdk//6.10 org.freedesktop.Sdk.Extension.llvm21//25.08
flatpak install --user flathub org.freedesktop.Platform.GL.default//25.08
```

Use `flatpak update --user --commit=<commit from sources.json> <ref>` for each of those three refs. The build refuses a different SDK/runtime revision. Read the lock file; do not bypass that check.

The GL extension is needed for the real editor window. GUI checks use the finished app's runtime and keep Flatpak's driver search path. Record its installed commit with the test evidence.

Clone the public source repository onto the Linux filesystem, then download the official editor archive explicitly:

```sh
git clone https://github.com/nekontritce11123-lab/factmontage.git
cd factmontage
python3 -B Studio/scripts/dev.py bootstrap --timeout 300
```

Until the repository is opened publicly, this staging clone requires its owner's access. This restriction must be removed before stable publication.

For the background model, either take the pinned ONNX from the matching release source materials or export it using [export_humanseg_onnx.py](../Background/scripts/export_humanseg_onnx.py). The exporter requires the public PaddleSeg commit `3c4db66de1d9d59d0628ed87590b6308a2f4aa2a`, PaddlePaddle 2.6.2 and Paddle2ONNX 1.3.1 in a compatible separate Python environment. The original checkpoint URL and archive hash are in `Background/SOURCES.json`; the extracted `.pdparams` hash must match the exporter. It downloads nothing implicitly.

```sh
python Background/scripts/export_humanseg_onnx.py --paddleseg /path/to/PaddleSeg --weights /path/to/model.pdparams --output /path/outside/git/pp_humansegv2_lite_portrait_static.onnx
```

The ONNX hash must be `2047b95bea344af15575f4279d6fac44bb07a42e7d537cf8d3265f15fe95cc82`.

## Check the sources

FAST is offline. Bootstrap and dependency installation are separate operations.

```sh
python3 -B Studio/scripts/dev.py test all --jobs 2 --timeout 180
```

For contracts without native compilation, add `--contracts-only`. That result does not certify an editor build, GUI or export. See the [validation guide](TESTING.md) for the affected integration and SMOKE commands.

## Build and package

Choose one persistent build root on the Linux filesystem and set the model path:

```sh
export STUDIO_BUILD_ROOT="$HOME/factmontage-build"
export STUDIO_BUILD_JOBS=2
export STUDIO_HUMANSEG_ONNX=/path/outside/git/pp_humansegv2_lite_portrait_static.onnx
export CCACHE_DIR="$STUDIO_BUILD_ROOT/ccache"
export CCACHE_MAXSIZE=2G
bash Studio/scripts/build.sh baseline
bash Studio/scripts/build.sh studio
bash Studio/scripts/check-sdk.sh
```

The baseline is required once for the same inputs and is reused. The SDK checks include ABI, sanitizers, engines, MLT, host tests and deployment. Set `STUDIO_PREVIOUS_BUNDLE` to a verified previous package for the isolated install/update/rollback check. A first release maintainer supplies that previous candidate separately.

After all automatic checks pass, package a **free** candidate version:

```sh
bash Studio/scripts/package.sh 1.2-rc14
```

The package script rejects an occupied version and changed inputs. It emits the bundle, source archives, versions, verification files and checksums under `$STUDIO_BUILD_ROOT/dist/<version>`. Physical Steam Deck and interactive editing acceptance are separate gates. Packaging a candidate does not authorise a stable release.

For the existing maintainer's retained WSL environment, all heavy stages use the existing disk/deadline guard: 120 GiB VHD budget, 32 GiB free space reserve, two jobs and a 2 GiB compiler cache.
