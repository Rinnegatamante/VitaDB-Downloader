# VitaDB Downloader
VitaDB Downloader is the official PSVita/PSTV client for [VitaDB](https://vitadb.rinnegatamante.it/), the first database ever made for PSVITA/PSTV homebrew.

## Features
- Searching by author/homebrew name.
- Filtering apps by category.
- Viewing of all available screenshots for apps.
- Sorting apps by different criteria (Most Recent, Oldest, Most Downloaded, Least Downloaded, Alphabetical, etc...)
- Showing of several metadata for apps.
- Download and installation of vpk+data files or vpk only at user discretion. (No more need to redownload data files everytime you want to update an homebrew for which data files are unchanged)
- GUI based on dear ImGui, providing a very robust user experience without sacrificing on fancyness and with high customizability.
- Fast boot time (Only the very first boot will take a bit more due to app icons download. Successive boots will be basically instant)
- Low storage usage (Screenshots are served on demand, the only data that are kept on storage are app icons with a complessive storage usage lower than 10 MBs).
- Tracking of installed apps and of their state (outdated/updated) even when not installed through VitaDB Downloader.
- Background music (You can customize it by changing `ux0:data/VitaDB/bg.ogg` with your own preferred track).
- Background image/video (You can customize it by `changing ux0:data/VitaDB/bg.mp4` or `ux0:data/VitaDB/bg.png`).
- Support for themes (Customization of GUI elements via `ux0:data/VitaDB/themes.ini`) with built-in downloader and manager.
- Support for PSP homebrews.
- Daemon support for homebrews update check in background during normal console usage.

## Themes
You can find some themes usable with this application on [this repository](https://github.com/CatoTheYounger97/vitaDB_themes).
Those themes can also be accessed in the app itself by pressing L. While in Themes Manager mode, you can download themes by pressing X and install themes in two different ways (that can be interchanged by pressing Select):
- Single = A downloaded theme will be installed as active one by pressing X
- Shuffle = Pressing X will mark a theme, you can mark how many themes you want. Once you've finished, press again Select to install a set of themes for shuffling. This means that every time the app is launched, a random theme will be selected from the set and used as active one.

## Homebrew Updater Daemon
Starting with v.1.7, VitaDB Downloader features an optional daemon that allows to check for all your installed homebrews updates in background. When console is booted and every hour after the first boot, updates will be searched and, if found, notifications will be fired to notify the user of its existence.
By default, a couple of homebrews are blacklisted from this process either cause they are nightly builds (for which it's not reliable to checksum the hash on server side to perform the update veerification) or cause the Title ID of the app is being used by two or more applications (making impossible to perform an update check).
It's also possible to add more blacklisted homebrews (for example, if you use a modded build which would be tagged as outdated by VitaDB Downloader). To do so, create the file `ux0:data/VitaDB/daemon_blacklist.txt` and add inside it a list of Title ID of the homebrews you want to blacklist in this format `ABCD12345;ABCD12346;ABCD12347`.

## Changelog

### v.1.8
- Fixed an issue causing libshacccg.suprx extraction to fail under certain circumstances.
- Fixed an issue causing kubridge.skprx to not be activated under certain circumstances.
- Made so that libshacccg.suprx extraction will proceed if the app is launched after only some steps are performed instead of restarting from scratch.
- Added more homebrew offered as Nightly releases to the Daemon blacklist (Xash3D, Nazi Zombies Portable).
- Made so that VitaDB Downloader will automatically cleanup storage for leftover of failed homebrew installs.
- Added a new Manage submenu accessible by pressing Select with different features.
- Moved "View Changelog" feature to Manage submenu.
- Added the possibility to launch PSVita homebrew from the Manage submenu.
- Added the possibility to uninstall PSVita/PSP homebrew from the Manage submenu.
- Added the possibility to view homebrew requirements from the Manage submenu.
- Added the possibility to tag an homebrew as Updated from the Manage submenu.
- Made possible to cancel an homebrew install if it has requirements from the requirements popup.
- Added a new filter: Freeware Apps. It will show all the apps not requiring user to supply game data files manually in order to be used.

### v.1.7
- Added an optional auto-updater daemon for installed Vita homebrews. It will check for any homebrew update every hour and at console boot even with VitaDB Downloader closed and send a notification to quickly perform the update.
- Added an auto-downloader and extractor of libshacccg.suprx if this is missing.
- Added proper support for PSP homebrews over different locations based on Adrenaline settings.
- Fixed a bug causing all PSP homebrews to be categorized as Original Games.
- Now requirements popup won't show up for homebrews requiring only libshacccg.suprx since already present if VitaDB Downloader is being used.
- Added an optional kubridge.skprx updater/installer when attempting to install an homebrew requiring it.

### v.1.6
- Fixed a bug causing VitaDB Downloader to be reported always as Outdated.
- Added shadowing support for texts for themes (TextShadow).
- Enhanced icons loading time. Now scrolling through apps will be considerably faster.
- Fixed a bug causing potential filesystem issues due to how icons were stored internally on storage.
- Fixed a bug that caused app bootup to take more time the more apps got downloaded from the app itself.
- Added support for PSP homebrews download and installation (L will now cycle through Vita Homebrews, PSP Homebrews, Themes).
- Moved version value to top left of the screen.
- Added info about current mode (Vita Homebrews, PSP Homebrews, Themes) on the top right of the screen.
- Improved version checking for installed homebrews made in Unity, Game Maker Studio, Godot or Lua. Now they will be correctly detected as Outdated if they are so.
- Made so that Vita homebrews icons are rendered as rounded.
- Made so that, if connection is lost during a download, the download will get resumed at the point where it stopped instead of failing the download.

### v.1.5
- Fixed a bug causing potential crashes if you had a few specific apps installed with a very big eboot.bin file.
- Fixed a bug causing more than a popup to not always show in certain circmustances.
- Fixed a bug causing the first icon of an app being shown after a sort mode change, a search or a filter change to be wrong.
- Fixed a bug causing cached hash files to be incorrectly generated when installing an app (Resulting in slower boot times).
- Fixed a bug that prevented changelog parser to properly escape " char.
- Made cleanup check for leftover unfinished app installs more robust.
- Added app name and version on changelog viewer titlebar.
- Properly aligned Filter text to Search text.
- Added some padding between Filter and Sort mode.
- Added possibility to customize font.
- Added possibility to properly customize any leftover uncustomizable element of the GUI.
- Moved missing icons download from everytime you hit an app lacking the icon to boot time (for all of them).
- Made so that Sort mode can be cycled only with R.
- Added themes downloader and manager with single theme and shuffling themes support (Reachable with L).
- Moved from SoLoud to SDL2 Mixer as audio backend. (Way faster booting time for the background audio playback).
- Fixed a bug causing a crash when opening the changelog viewer in certain circumstances.

### v.1.4
- Added a check after installing an app wether the installation succeded or failed.
- Added proper cleanup of leftover files when an installation is abruptly aborted or fails.
- Fixed a bug causing wrong icon to be shown when performing a search and moving to the first app of the list.
- Fixed a bug causing app info to be shown also when cursor is not on an app.
- Added requirements popup when attempting to install an app having extra requirements for a proper setup (Eg. Plugin requirements or full data files from original game).
- Added possibility to customize color scheme for all GUI elements (ux0:data/VitaDB/themes.ini).
- Added proper tracking of applications state (Not Installed, Outdated, Updated).
- Speeded up boot time. Now VitaDB Downloader will launch approximately one second faster.
- Added possibility to check changelog for the selected app by pressing Select button.
- Renamed "Category: " option to "Filter: ".
- Added possibility to filter applications by Not Installed/Installed/Outdated criterias.
- Fixed a bug causing page down (Right arrow) to not properly reach end of the list when a filter or search was active in certain circumstances.

### v.1.3
- Made so that fast paging down with right arrow will go as down as the very last entry.
- Made visible on the top menubar the currently in-use filter for the apps list.
- Reworded data files installation question to sound more correct.
- Added Smallest and Largest sorting modes.
- Added a dropmenu to change sorting mode (L / R is still usable for cycling between sorting modes).
- Added possibility to cycle between category filters with Square button.
- Using different granularity (B, KB, MB, GB) for homebrew sizes depending on the size itself.
- Added free and total storage info on bottom right of the screen.
- Aligned to left homebrew names in the apps list.
- Added support for backgrounds (Both static (ux0:data/VitaDB/bg.png) and animated (ux0:data/VitaDB/bg.mp4)).
- Added a check prior downloading an app wether free storage is enough to install it.

### v.1.2
- Added possibility to start a search rapidly by pressing the Triangle button.
- Fixed a bug causing the app to crash if the background music file was missing.
- Fixed a bug preventing the app to be updated from within the app itself.
- Added auto updater.
- Fixed an issue causing crackling and stuttering with audio during archive extractions.

### v.1.1
- Added a check when more than a month passed since last boot. If this happens, provide an option to the user to re-download all app icons at once.
- Added possibility to fast scroll apps list with Left/Right arrows.
- Added possibility to fast scroll apps list by moving the scrollbar with left analog.
- Added possibility to instantly return to the top of the list by pressing Circle (Previously it was Circle + Left).
- Fixed a bug not making scrollbar instantly reposition when going to the top of the list.
- Fixed a bug causing selected app icon to get corrupted temporarily after installing an app.
- Added background music (You can disable it or change the track by removing/replacing ux0:data/VitaDB/bg.ogg)
- Fixed a bug causing selected app to change randomly when changing sort mode.

## Credits
- noname120 for the code related to head.bin generation.
- PrincessOfSleeping for the original code related to notification sendings.
- gl33ntwine for helping reversing a small part of Friends app to understand how to intercept notification boots.
- CatoTheYounger and Brandonheat8 for testing the homebrew.
- Once13One for the Livearea assets.
- [phloam](https://www.youtube.com/channel/UCO-COkqKBV1KeBifq0HMK0g) for the audio track used as base for the background music feature.
