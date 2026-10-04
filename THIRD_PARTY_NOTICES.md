# Licences and source materials

FactMontage integrates changes into Kdenlive. The added integration in `Studio/overlay` and the combined modified editor are distributed under **GPL-3.0-only**. The [root licence](LICENSE) contains the unmodified GPL version 3 text.

Original copyright notices remain in place. Older names in author notices, effect identifiers, file signatures and API names identify the original work or a compatibility contract.

## Engines

| Directory | Licence | Role |
| :--- | :--- | :--- |
| [Card_3D](Card_3D/LICENSE) | MIT | Photo and video cards |
| [Camera](Camera/LICENSE) | MIT | Movement and face tracking |
| [Background](Background/LICENSE) | GPL-3.0 | Background processing |
| [Transitions](Transitions/LICENSE) | MIT | Transition renderer |
| [Effects](Effects/LICENSE) | GPL-3.0 | Effects, lens and vintage camera |
| [Color](Color/LICENSE) | GPL-3.0 | SDR colour processing |
| [Audio](Audio/LICENSE) | MIT | Audio worker |
| [Subtitles](Subtitles/LICENSE) | MIT | Subtitle worker and contracts |
| [Text](Text/LICENSE) | MIT | Native text renderer and compiler |
| Studio integration | GPL-3.0-only | Qt panel and host changes |

Use the licence text and SPDX notices in each file for its exact terms. This table does not replace them.

## Editor and dependencies

The pinned Kdenlive version is **26.08.0**. Its source, licence notices and build dependencies are supplied by the official archive and the [pinned upstream manifest](Studio/upstream/org.kde.kdenlive.json). Exact SDK, runtime, editor archive and upstream manifest hashes are in [sources.json](Studio/upstream/sources.json).

The manifest describes the complete dependency set, including MLT, Qt/KDE libraries, FFmpeg, frei0r, OpenCV and OpenTimelineIO. These retain their upstream licences. SDK and runtime components are supplied by their Flatpak runtimes.

Additional source and model notices:

- [Background notices](Background/THIRD_PARTY_NOTICES.md): ONNX Runtime 1.21.0 (MIT), PaddleSeg/PP-HumanSeg (Apache-2.0), Eigen and Protobuf.
- [Subtitle source lock](Subtitles/SOURCES.json): whisper.cpp 1.9.4 and Whisper weights (MIT), Silero VAD 5.1.2 (MIT). Licence copies are in [Subtitles/licenses](Subtitles/licenses).
- [Text notices](Text/THIRD_PARTY_NOTICES.md): native implementation and development tool dependencies. Font files are not bundled by the Text module.
- [Transition sounds](Transitions/sfx/README_RU.md): individual CC0 sources and the unmodified source sound files.

## Corresponding source

A binary release must provide the exact custom sources, the integration and build scripts, the generated Flatpak manifest, upstream source references and patches, model provenance, version records and checksums. The official editor archive is included with the release source materials. The public repository must build without credentials for the former private repository.

The build instructions explain how public dependency sources and pinned models are obtained. Model weights and binary build outputs belong in release assets, not in Git. See [BUILD.md](docs/BUILD.md).

FactMontage is an independent modification. Kdenlive and KDE names identify the upstream project. This build is not an official KDE release.
