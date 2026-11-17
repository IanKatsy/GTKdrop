GTKdrop @VERSION@ - Stardew Valley mod manager for Linux
A vibecoded C++/GTK rewrite of Stardrop: https://github.com/IanKatsy/GTKdrop

Needs (Arch: sudo pacman -S gtk4 libadwaita libsoup3 libsecret webkitgtk-6.0):
  GTK 4.16+, libadwaita 1.7+, libsoup 3, libsecret, WebKitGTK 6.0
  Built on Arch Linux, so it needs a recent distro (recent glibc and libstdc++).
  Optional, to unpack mods: unzip, 7zip, unrar (or libarchive's bsdtar)

Run:  ./GTKdrop        (keep it anywhere, e.g. ~/Applications/GTKdrop)
  It reads your Stardrop profiles and settings once on first start.

Downloading a collection:
 1. Settings -> Nexus Mods: paste your personal API key
    (https://www.nexusmods.com/users/myaccount?tab=api) and turn on
    "Handle Nexus Mods Links".
 2. On the collection's Nexus page click "Mod Manager Download". GTKdrop makes a
    profile for it and a download list.
 3. Menu -> Collections -> press Download on the list. Sign in to Nexus once in
    the "Nexus Mods Downloads" window; it then downloads one file at a time and
    installs them when it finishes.

Launching through Steam: the first time, GTKdrop shows what to paste into the
game's Launch Options in Steam (menu -> File -> Steam Launch Options shows it
again).

Files: ~/.config/gtkdrop (settings, profiles, library.json, downloads)
Log:   ~/.config/gtkdrop/log/gtkdrop.log

GPL-3.0, like Stardrop. See LICENSE.
