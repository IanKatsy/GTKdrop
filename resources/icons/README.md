# Fallback icons

Symbolic icons GTKdrop uses that some icon themes lack (Breeze has no `view-pin-symbolic` or
`web-browser-symbolic`; Adwaita has no `external-link-symbolic`). They're compiled into the GResource under
`/dev/gtkdrop/GTKdrop/icons`, which GTK searches as part of the `hicolor` fallback theme: the user's own theme
still wins whenever it has the icon.

- `view-pin-symbolic.svg`, `web-browser-symbolic.svg`: from the Adwaita icon theme (GNOME, CC-BY-SA 3.0 / LGPL-3.0).
- `external-link-symbolic.svg`: libadwaita's `adw-external-link-symbolic` (LGPL-2.1-or-later).

When adding an icon to the code, check it exists in both Adwaita and Breeze, or add a fallback here.
