# VitaDB Downloader
VitaDB Downloader is the official PSVita/PSTV client for [VitaDB](https://vitadb.rinnegatamante.it/), the first database ever made for PSVITA/PSTV homebrew.

## Features
- Searching by author/homebrew name.
- Filtering apps by category.
- Viewing of all available screenshots for apps.
- Sorting apps by different criteria (Most Recent, Oldest, Most Downloaded, Least Downloaded, Alphabetical)
- Showing of several metadata for apps.
- Download and installation of vpk+data files or vpk only at user discretion. (No more need to redownload data files everytime you want to update an homebrew for which data files are unchanged)
- Minimalistic GUI based on dear ImGui with focus on robustness over fancyness.
- Fast boot time (Only the very first boot will take a bit more due to app icons download. Successive boots will be basically instant)
- Low storage usage (Screenshots are served on demand, the only data that are kept on storage are app icons with a complessive storage usage lower than 10 MBs).
- Background music (You can customize it by changing ux0:data/VitaDB/bg.ogg with your own preferred track).

## Changelog
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
- CatoTheYounger for testing the homebrew.
- Once13One for the Livearea assets.
- [phloam](https://www.youtube.com/channel/UCO-COkqKBV1KeBifq0HMK0g) for the audio track used as base for the background music feature.
