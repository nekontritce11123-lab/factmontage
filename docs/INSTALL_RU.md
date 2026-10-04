# Установка FactMontage в Linux

[English](INSTALL.md) · [На главную](../README_RU.md)

Первый выпуск рассчитан на Linux x86-64. На Steam Deck используй Desktop Mode. Версия 1.2 ещё проходит приёмку; стабильный пакет пока не опубликован.

## Первая установка

Если Flatpak отсутствует, установи его средствами своего дистрибутива. В Desktop Mode SteamOS он уже есть.

Добавь Flathub для установки runtime KDE:

```sh
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.10
```

Скачай Flatpak и `SHA256SUMS.txt` из **одного** [выпуска](https://github.com/nekontritce11123-lab/factmontage/releases). Открой терминал в папке загрузки и проверь файлы:

```sh
sha256sum --ignore-missing --check SHA256SUMS.txt
```

Строка пакета Flatpak должна завершаться `OK`. Используй точное имя пакета из выбранного выпуска. Например, для версии 1.2:

```sh
flatpak install --user ./FactMontage-1.2.flatpak
flatpak run local.VideoStudio.Kdenlive
```

FactMontage появится в меню приложений. Его идентификатор Flatpak и папка настроек отделены от `org.kde.kdenlive`.

## Обновление и откат

Сохрани предыдущий Flatpak и резервную копию проектов. Закрой FactMontage перед сменой установленной версии.

Проверь новый пакет и установи его поверх существующего приложения:

```sh
flatpak install --user --reinstall ./FactMontage-1.2.flatpak
```

Для отката выполни ту же команду с именем **предыдущего проверенного** пакета. Ранние кандидаты VideoStudio используют тот же идентификатор приложения. Проект с изменениями новой версии может потребовать эту версию для открытия; сначала проверь копию.

## Удаление

```sh
flatpak uninstall --user local.VideoStudio.Kdenlive
```

Не добавляй `--delete-data`, если хочешь сохранить настройки приложения. Папки проектов и официальный Kdenlive остаются отдельно.

## Первый запуск и файлы проекта

Если панель скрыта, открой **FactMontage** через меню **Вид**. Сейчас панель работает на русском. Перед переносом проекта с надписями, трекингом, масками или звуком прочитай [правила переноса](PROJECTS_RU.md).

Если runtime отсутствует, проверь подключение Flathub и установи `org.kde.Platform//6.10` командой выше. При ошибке приложения [сообщи версию и шаги воспроизведения](https://github.com/nekontritce11123-lab/factmontage/issues/new/choose).
