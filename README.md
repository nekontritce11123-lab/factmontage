<p align="center"><img src="docs/assets/factmontage.svg" width="80" height="80" alt="FactMontage FM logo"></p>

<h1 align="center">FactMontage</h1>
<p align="center">A modified Kdenlive build for Linux, with an integrated editing panel.</p>
<p align="center">
  <a href="https://github.com/nekontritce11123-lab/factmontage/releases">Downloads</a> ·
  <a href="docs/INSTALL.md">Installation</a> ·
  <a href="README_RU.md">Русская версия</a>
</p>

**Release status:** version 1.2 is being prepared. The stable release is pending editing workflow and Steam Deck acceptance. Use a copy of your project when testing a candidate.

[![Dark Kdenlive workspace with the FactMontage panel, project monitor and timeline](docs/screenshots/workspace-dark.png)](docs/screenshots/workspace-dark.png)

One editing workspace, eight panel sections. FactMontage keeps Kdenlive's timeline, project format and export tools. The panel adds controls for cards, camera movement, backgrounds, transitions, effects, colour, audio, text and subtitles.

The current panel is in **Russian**. The first release targets **Linux x86-64**. Steam Deck testing uses Desktop Mode; final device acceptance is still pending.

## What is in the panel

| Section | Tools |
| :--- | :--- |
| Cards | Photo and video cards, borders, shadows, placement, entrance and exit animation. |
| Camera | Zoom, pan, movement presets and analysis of a selected face for tracking. |
| Background | Chroma key and local person segmentation, with transparency, blur or a fill. |
| Transitions | 15 styles for adjacent clips, with optional transition sounds. |
| Effects | A preset catalogue, search, favourites, lens and vintage camera controls. |
| Colour | Eight styles and manual SDR adjustments. |
| Audio | Voice processing, loudness, voice/music balance and pause removal. |
| Text | Native animated titles and local Russian speech transcription with word timing. |

<details>
<summary>View all eight sections</summary>

<table>
  <tr>
    <td><a href="docs/screenshots/cards-dark.png"><img src="docs/screenshots/cards-dark.png" alt="Video card over the lower layer"></a><br><b>Cards</b> · Video card over the lower layer</td>
    <td><a href="docs/screenshots/camera-dark.png"><img src="docs/screenshots/camera-dark.png" alt="Zoom and movement"></a><br><b>Camera</b> · Zoom and movement</td>
  </tr>
  <tr>
    <td><a href="docs/screenshots/background-dark.png"><img src="docs/screenshots/background-dark.png" alt="Foreground over a new background"></a><br><b>Background</b> · Foreground over a new background</td>
    <td><a href="docs/screenshots/transitions-dark.png"><img src="docs/screenshots/transitions-dark.png" alt="An applied transition"></a><br><b>Transitions</b> · An applied transition</td>
  </tr>
  <tr>
    <td><a href="docs/screenshots/effects-dark.png"><img src="docs/screenshots/effects-dark.png" alt="Lens treatment"></a><br><b>Effects</b> · Lens treatment</td>
    <td><a href="docs/screenshots/colour-dark.png"><img src="docs/screenshots/colour-dark.png" alt="An adjusted SDR frame"></a><br><b>Colour</b> · An adjusted SDR frame</td>
  </tr>
  <tr>
    <td><a href="docs/screenshots/audio-dark.png"><img src="docs/screenshots/audio-dark.png" alt="A managed audio result"></a><br><b>Audio</b> · A managed audio result</td>
    <td><a href="docs/screenshots/text-dark.png"><img src="docs/screenshots/text-dark.png" alt="A native title over video"></a><br><b>Text</b> · A native title over video</td>
  </tr>
</table>

Screenshots use one [redistributable demo project](docs/demo/README.md). Click an image to open it at full size.

</details>

## Install

Download the Flatpak and `SHA256SUMS.txt` for the same version from [Releases](https://github.com/nekontritce11123-lab/factmontage/releases). Follow the [Linux installation guide](docs/INSTALL.md) or [инструкцию на русском](docs/INSTALL_RU.md).

FactMontage installs as `local.VideoStudio.Kdenlive`, alongside the official Kdenlive app. This identifier is retained so existing installations and projects can be updated.

## Start editing

1. Launch FactMontage and create a project. The first release is tested with SDR, 8-bit, 1920×1080 footage at 60 fps.
2. Add clips to Kdenlive's timeline and select the clip you want to edit.
3. Open **FactMontage** from the **View** menu if its panel is hidden. Choose a section, adjust its controls and apply the result.
4. Use Kdenlive's Undo/Redo, project save and render controls. For a slower machine, try preview resolution 1:2 or 1:4.

Keep the project's generated resources with it. Use **Save As** or **Archive Project** when moving work, then reopen the copy and check its assets. See [project files and portability](docs/PROJECTS.md).

## Current limits

- Linux x86-64 only for the first release. Windows is a development environment.
- Russian panel and Russian speech transcription.
- SDR and 8-bit processing. HDR, automatic colour matching and a new GPU processing pipeline are outside the release scope.
- Background analysis and speech transcription run locally on the CPU. Their models are included in the package; analysis takes time.
- Preview quality may need to be reduced. A 1080p60 project profile does not promise 60 fps preview on every device.

## Help and development

[Report a problem](https://github.com/nekontritce11123-lab/factmontage/issues/new/choose) with the version, steps to reproduce and a small project you can share. The [build guide](docs/BUILD.md) explains the pinned sources and checks. [Release notes](docs/RELEASE_NOTES.md) describe the candidate and its acceptance requirements.

FactMontage is an independent modification of [Kdenlive](https://kdenlive.org/). KDE does not publish this build.

## Licences

The integration is licensed under [GPL-3.0-only](LICENSE). Individual engines retain their original licences and notices. See [third-party notices](THIRD_PARTY_NOTICES.md) for the module and dependency map. Corresponding source and build materials accompany the Flatpak release.
