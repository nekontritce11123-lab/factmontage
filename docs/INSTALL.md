# Install FactMontage on Linux

[Русская версия](INSTALL_RU.md) · [Back to FactMontage](../README.md)

The first release targets Linux x86-64. On Steam Deck use Desktop Mode. Version 1.2 is still in acceptance; no stable package is published yet.

## First installation

Install Flatpak through your Linux distribution if it is missing. SteamOS Desktop Mode already includes it.

Add Flathub for the KDE runtime:

```sh
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.10
```

Download the Flatpak and `SHA256SUMS.txt` from the **same** [release](https://github.com/nekontritce11123-lab/factmontage/releases). Open a terminal in the download folder and verify the files:

```sh
sha256sum --ignore-missing --check SHA256SUMS.txt
```

The line for the Flatpak must say `OK`. Use the exact package filename from that release. For example, for a 1.2 release:

```sh
flatpak install --user ./FactMontage-1.2.flatpak
flatpak run local.VideoStudio.Kdenlive
```

FactMontage appears in the application menu. It uses a separate Flatpak application ID and settings directory from `org.kde.kdenlive`.

## Update or roll back

Keep the previous Flatpak and a backup copy of your projects. Close FactMontage before changing its installed version.

Verify the new download, then install it over the existing application:

```sh
flatpak install --user --reinstall ./FactMontage-1.2.flatpak
```

To roll back, run the same command with the **previous verified** package filename. Earlier VideoStudio candidate packages use the same application ID. Changes made with a newer version may require that version to reopen; test a copy before resuming work.

## Remove

```sh
flatpak uninstall --user local.VideoStudio.Kdenlive
```

Do not add `--delete-data` if you want to keep the application's saved settings. Your project folders and the official Kdenlive app remain separate.

## First launch and files

Open the **FactMontage** panel from **View** if it is hidden. The panel is currently in Russian. Use [the project guide](PROJECTS.md) before moving projects with generated text, tracking, masks or audio.

If the runtime is missing, check that Flathub is registered and install `org.kde.Platform//6.10` using the command above. For an application problem, [report the version and steps](https://github.com/nekontritce11123-lab/factmontage/issues/new/choose).
