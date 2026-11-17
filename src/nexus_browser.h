#pragma once

// GTKdrop's own Nexus Mods window (WebKitGTK) for downloading without Premium. For each file it opens the file's
// "Mod Manager Download" page, clicks "Slow download" the way the NexusMods Advance extension does, and catches the
// nxm:// link the page then produces. That link's one-time key lets the API hand out the file. The window keeps
// its own Nexus login (signed in once, stored in ~/.local/share/gtkdrop/webkit).

#include <gtk/gtk.h>

#include <functional>
#include <optional>
#include <string>

namespace gtkdrop::ui {

class NexusBrowser {
public:
    using Done = std::function<void(std::optional<std::string> nxm_url, const std::string &error)>;
    using Waiting = std::function<void(const std::string &what)>; // the user has to do something in the window

    explicit NexusBrowser(GtkWindow *parent);
    ~NexusBrowser();
    NexusBrowser(const NexusBrowser &) = delete;
    NexusBrowser &operator=(const NexusBrowser &) = delete;

    // One request at a time; `done` runs once (nullopt when the window was closed or cancel() called).
    void request(const std::string &domain, int mod_id, int file_id, Done done, Waiting waiting);
    void cancel();

private:
    struct Impl;
    Impl *impl_;
};

} // namespace gtkdrop::ui
