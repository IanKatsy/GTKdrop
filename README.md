# GTKdrop

A vibecoded C++/GTK rewrite of [Stardrop](https://github.com/Floogen/Stardrop), the Stardew Valley mod manager,
for Linux, with some extra features.

It reads your existing Stardrop data (profiles, settings, collections) once on first start.

## Extra features

- **Nexus Mods downloads without Premium**: its own Nexus window clicks "Slow download" for each file and installs
  them one after another, for collections and for updates. Handles `nxm://` links.
- **Collections**: each one gets its own folder and profile. "Other Available Mods" lists your other installed mods
  to add to it, or to copy in permanently at their current version. Profiles copied from a collection can stay
  collection profiles or become normal ones.
- **Mod library** (`~/.config/gtkdrop/library.json`): remembers which Nexus mod and file each installed folder came
  from, so update checks, "Open Mod Page" and collection checks work even when a mod's manifest has the wrong ID.
- **Add mods** from archives or plain folders. Archives unpack with the system's `unzip`, `7z`, `unrar` or `bsdtar`.
- **Launching through Steam** (overlay, playtime and Cloud saves keep working) or directly for GOG.
- Stardrop's colour themes, plus following the system's light/dark style and accent colour.

## Building

Needs CMake 4.4+, a C++26 compiler, and the development files for GTK 4.16+, libadwaita 1.7+, libsoup 3,
libsecret, WebKitGTK 6.0 and GLib. nlohmann/json is fetched if it isn't installed, and so are GTK and libadwaita if
the system's are too old.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/GTKdrop
```

## License

GPL-3.0, like Stardrop. See [LICENSE](LICENSE).
