# FactMontage 1.2

[Русская версия](RELEASE_NOTES_RU.md) · [Back to FactMontage](../README.md)

**State: release preparation. Stable publication is pending acceptance.**

The candidate starts from the source used to build VideoStudio **1.2-rc13**, commit `c219211a9b069abbf205184195727dd3f024dddc`. All 603 files in its frozen build source archive were compared with that commit before changes.

This preparation introduces the FactMontage display name, FM application icon, English/Russian user documentation and a separate source repository. Existing Flatpak ID, effect IDs, ABI and resource signatures remain unchanged. A Windows-only path assertion in the development runner tests is corrected.

The earlier rc13 passed its automatic release chain and was installed and launched on a Steam Deck. Those results do not certify the renamed candidate. A new package needs its own checks, checksums and source record.

Required release evidence includes FAST, host integration, SMOKE, a build from the public source copy, editing lifecycle and export, install/update/rollback and physical Steam Deck acceptance. The final package must be tied to its source tag and verification records.
