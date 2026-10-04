# FactMontage 1.2-rc14: physical Steam Deck acceptance

[Русская версия](ACCEPTANCE_RU.md)

This is an experimental candidate, not a stable release. Work in Desktop Mode, with project copies and permitted local media. This document describes checks to perform; it contains no completed results and does not replace project task tracking. A previous candidate's PASS, an automated build, a screenshot or the procedural demo does not certify this package on your Deck.

## Package and test material

Use FactMontage-1.2-rc14.flatpak and SHA256SUMS.txt from the same candidate directory. Follow INSTALL_EN.txt. The matching VERSIONS.json and VERIFICATION.json describe the supplied source/dependency and automated evidence; physical checks below need their own results. These relative links are intended for the final candidate directory.

Record Deck model, SteamOS version, KDE runtime version, package SHA256, installed Flatpak commit, source SHA from the candidate evidence, screen/scale, storage medium and preview quality. Do not put personal paths or raw logs in the shared summary. Keep the previous verified package and untouched project backups for rollback. Do not use the official editor to test a project containing FactMontage engines.

Prepare a short SDR BT.709 1920×1080, 60 fps project. Preview at 1:2 or 1:4 is allowed; export remains full-size 1080p60. Use visibly different A/B clips with spare source frames around their cuts; a transparent graphic; your own face video with movement and brief loss of the face; a human against a varied background; green/blue chroma material; clear Russian speech with several known pauses and audible phrase endings; a separate permitted music track. Screenshots and procedural geometry contain no real face or speech. Keep source recordings private unless you explicitly intend to share them.

## Installation, layout and storage

| Action | Expected result |
| --- | --- |
| Verify checksums, install rc14 and launch `local.VideoStudio.Kdenlive`. | Package checksum is OK; FactMontage opens and the eight sections are available in one native panel. |
| Record the installed ref/commit of `org.kde.kdenlive`, if already installed; open an unrelated copied plain Kdenlive project there before and after the install cycle. | Official Kdenlive, its settings and unrelated project remain usable and unchanged. Do not install it solely for this test; mark that comparison NOT RUN if absent. |
| At 1280×800, test 100%, 125% and 150% scaling. Repeat on the external monitor used for editing. | Project monitor, timeline, section navigation, main action and all settings remain reachable by normal scrolling. Record actual layout and dimensions. At 150%, close Clip Monitor, Library, Speech Editor and Effect/Composition Stack through View; retain Project Monitor and FactMontage. Do not lower the requested scale to obtain PASS. |
| Repeat open, edit, Save As, reopen and export with project/media on internal storage, then microSD; include spaces and Cyrillic characters in test folder names. | Files remain accessible, resources resolve and outputs play on both media. Denied access or missing microSD is an explicit failure/blocker or NOT RUN, not PASS. |

## Eight sections: visible results

For each row, use the real panel, seek before/during/after the change, perform Undo then Redo, save, close, reopen and inspect a rendered excerpt in a separate player. A successful button press alone is not acceptance.

| Section / action | Expected result |
| --- | --- |
| Cards: image and video, three card looks, rounding 0/4/10/20%, shadow 0/35/100%, entrance and exit; drag position in the monitor; trim, split and copy. | A visible card, correct transparent/outline treatment, synchronised position controls; one Undo for one drag or reset. Entrance and exit belong to the card; the exit follows each resulting clip end. |
| Camera: zoom, pan, framing in the monitor, camera before card; split and seek; sequence cycle 2/6/12 seconds. | Visible motion and selected target, synchronised controls, no unexpected jump at a split or nondeterministic seek. |
| Camera / Face Tracking: select the actual face in the panel still frame, set zoom, analyse, seek and export movement. Repeat cancellation, changed selection and lost face. | Exported video visibly follows the same real face and target; playback and seek agree. A stale analysis does not affect another clip/project. Lost tracking requests a new analysis rather than silently inventing a path. |
| Background / Chroma: pick green and blue on photo/video, composite over a blue lower layer. | Foreground remains visible and removed areas reveal the lower layer. A white, entirely transparent or unchanged-background frame is FAIL. |
| Background / Human: analyse your actual human clip; test blur, transparent output over blue and solid fill; cancel, trim/split, reduced preview, reopen and export. | Person remains visible; edge/output changes match the chosen mode. Cancel preserves the previous valid result. Reverse or speed other than 100% is rejected clearly where unsupported. Missing mask/model produces a clear error; export never silently reveals the original background as success. |
| Transitions: complete the matrix below on adjacent clips, including linked AV and silent video. | Recognisable A before, the chosen style inside, B after; real intermediate rendered frames, no hard-cut substitution. Missing handles or a foreign transition causes no partial edit. |
| Effects: search/category/favourites, apply to one/multiple clips, lens shapes and centre, vintage variants; repeat apply and seek. | Visible selected effect; intended targets only, no accidental duplicates, deterministic seek/export. Batch Undo restores the complete previous state. |
| Colour: manual SDR brightness, temperature, contrast and saturation, supported ready styles and transparent footage. | Visible intended correction with preserved transparency. Reopen and export agree with the monitor. Do not claim HDR, automatic matching or GPU acceleration from this test. |
| Audio: the four modes below on actual speech and separate music. | Audible result, stable ownership and timeline synchronisation, without deleting user tracks or user volume effects. |
| Text and Subtitles: native text plus actual speech recognition, as below. | Visible exported text and correct checked subtitle timing; ordinary text works without ASR. |

## Transitions: all 15, with and without sound

Test every named style: `Двойное отдаление`, `Проезд вперёд`, `Мягкий взмах`, `Плавный проезд`, `Через расфокус`, `Диагональное раскрытие`, `Лёгкий поворот`, `Живой наплыв`, `Круговое раскрытие`, `Мягкое разделение`, `Наклон и зум`, `Мягкая шторка`, `Световой засвет`, `Раскрытие по яркости`, `Цифровой сдвиг`. These are the current Russian panel labels.

For each style record its own result. Inspect a frame before, several frames inside and a frame after the mix, including in export. Test sound off, automatic sound on, level adjustment, sound off again, and style change. Only the enabled chosen sound should be heard at the intended cut. Repeat apply and Save/Reopen: no extra managed sound clips, tracks or transition instances. User music, source audio and volume effects remain intact. Check linked-audio dip, duration/alignment changes, removal, moving the pair and deleting one clip, each with Undo/Redo. A missing bundled sound must report an error rather than create a silent success.

## Audio and subtitles

| Action | Expected result |
| --- | --- |
| Voice (`Голос`): listen before/after; compare processing strength and noise off/on, then cancel a new job. | Speech remains intelligible and timing unchanged; previous valid output survives cancellation. |
| Loudness (`Громкость`): process quiet/loud fragments together and export. | Audible level correction without clipping or per-cut pumping. Any quantitative LUFS/true-peak claim needs an actual measurement of that export. |
| Music (`Музыка`): assign separate voice/music roles, change music presence and voice priority. | Background music ducks around speech and recovers in pauses; voice stays clear and timing is preserved. |
| Repeat these output modes on the same inputs, then Save/Reopen and repeat. Remove a processed WAV only in a disposable copy and use recovery. | At most one managed audio track and one current result for the source set, one managed mute per source; no growth on repeats. User tracks/effects remain. Recovery is explicit and produces one valid result. |
| Pauses (`Паузы`): include linked video/audio, another populated track, subtitles and markers; listen at each removal boundary. Undo/Redo, reopen and export. | Only detected intended pauses are removed; video, speech, other tracks, subtitle positions and markers stay aligned. No clipped phrase endings or new WAV/result track for pause removal. Unsupported/conflicting selection is rejected without partial changes. |
| Text: Cyrillic and Latin, two lines, font/size/spacing, entrance/life/exit; edit, trim and split. | Native title clip remains legible, seek is repeatable, animation/clip duration is correct. A text edit, reset or drag is undone as one operation. |
| Subtitles: recognise your Russian speech, compare known phrase/word starts and ends with source audio; check a word near a VAD boundary, overlaps and silence. Edit text, split/merge, change line length, seek and export. | Original speech timestamps are preserved; words do not end prematurely or extend through removed pauses. Conflicts are reported for correction. No invented subtitles on silence. Reformatting does not rerun recognition. Export contains the checked, burned-in subtitles. |

## Lifecycle, isolation and measurements

Save the combined project, close normally using File/Quit, confirm the process exits, relaunch and reopen. Repeat closure while an analysis is pending: cancellation/confirmation must finish cleanly, with no crash, hang or later application of a stale result. Closing the FactMontage panel must stop its preview animation load. Seek and export again after reopening.

Use Save As and Archive Project to create a portable copy containing media, `.stxt`, `.scam`, `.sbg`, processed WAV and subtitle assets. On a disposable copy, temporarily make the old folder unavailable, then reopen and export the new copy from internal storage and microSD. No generated asset should depend on the inaccessible original folder. Do not delete an asset still needed by a project or Undo history. A deliberately missing asset should yield an understandable failure, not altered output reported as success.

Render the complete short project and separate face-tracking, human-background, transition/sound and subtitle excerpts. Verify output dimensions/frame rate, audio, full duration and final frames in an independent player. Record visible and audible differences from the monitor.

Close FactMontage; update from the previous verified package to rc14, reopen a copied project, then roll back by reinstalling that previous package and check a compatible copied project. Follow the supplied installation instructions. Confirm the official application's ref/settings remain unchanged. If no previous verified package is available, mark update/rollback NOT RUN and name the dependency. Optional uninstall removes only `local.VideoStudio.Kdenlive`, without `--delete-data` or removal of project folders.

Run a 30-minute editing session. Measure on this physical Deck: memory, dropped frames/frame time, warmed one/two-layer preview at 1:1/1:2/1:4, face/background/subtitle analysis time, export time and storage used, on internal disk and microSD. Record power mode and measurement method. Compare a baseline only when measured on the same device/settings; do not borrow FPS or memory numbers from another machine. Do not assign invented performance thresholds or report an unmeasured estimate as PASS.

## Return a compact result

Use PASS, FAIL or NOT RUN for each scenario, including all 15 transitions; NOT RUN needs a reason and the next physical check. Keep private clips and raw logs local. Share a concise, sanitised summary with relevant screenshots/short permitted exports and their hashes:

```text
Candidate: 1.2-rc14 | package SHA256: <hash> | installed commit: <commit>
Source SHA: <SHA from candidate evidence> | Deck/SteamOS/runtime: <versions>
Layout/storage/preview: <actual settings>
Passed: <scenarios and evidence>
Failed: <action, expected/actual, reproducible steps>
Not run: <scenario, reason, next check>
Measured: <method and values, no estimates>
Evidence: <sanitised filenames and SHA256>
```

Stable acceptance remains pending until every required scenario has evidence for this same package; a summary containing required NOT RUN items is partial acceptance.
