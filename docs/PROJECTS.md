# Project files and portability

[Русская версия](PROJECTS_RU.md) · [Back to FactMontage](../README.md)

Kdenlive's `.kdenlive` file is the project. FactMontage uses its normal timeline and Undo/Redo history.

Text scenes (`.stxt`), camera tracks (`.scam`), background masks (`.sbg`) and processed WAV files are generated project assets. Keep them with the project. A saved project file alone is not a backup of the source footage or assets.

Use **Save As** to create a project copy, or Kdenlive's **Archive Project** to collect files. Reopen the copy, seek through affected clips and render a short section before removing the original folder. Test paths with spaces and non-Latin characters when transferring between machines.

Preserved effect identifiers and resource formats allow older FactMontage/VideoStudio projects to be read. The official Kdenlive build does not include these added engines. Opening such a project there cannot replace a compatibility check in FactMontage.

When testing a candidate, work with copies. Keep a verified previous package for rollback. Do not delete an asset while a project or its Undo history still uses it.
