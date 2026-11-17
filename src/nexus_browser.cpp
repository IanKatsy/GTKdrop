#include "nexus_browser.h"

#include <adwaita.h>
#include <webkit/webkit.h>

#include <filesystem>
#include <utility>

namespace gtkdrop::ui {

namespace {

// Runs on Nexus file pages. The download button lives in the <mod-file-download> element's shadow root, which
// plain document queries don't see. Reports the nxm:// link if one shows up as a link instead of a navigation.
constexpr char kClickScript[] = R"JS(
(function () {
  if (!/[?&]file_id=\d+/.test(location.href) || !/nmm=1/.test(location.href)) return;
  const post = (m) => window.webkit.messageHandlers.gtkdrop.postMessage(JSON.stringify(m));
  let clicked = false, ticks = 0;
  const timer = setInterval(() => {
    ticks++;
    const root = document.querySelector('mod-file-download')?.shadowRoot;
    const buttons = root ? Array.from(root.querySelectorAll('button')) : [];
    const slow = buttons.find(b => b.textContent.trim().toLowerCase().includes('slow download'));
    const links = Array.from(document.querySelectorAll('a')).concat(root ? Array.from(root.querySelectorAll('a')) : []);
    const nxm = links.find(a => a.href && a.href.startsWith('nxm://'));
    if (nxm) { clearInterval(timer); post({type: 'nxm', url: nxm.href}); return; }
    if (slow && !clicked) { clicked = true; post({type: 'clicked'}); slow.click(); }
    if (ticks === 300) post({type: 'stuck', clicked: clicked}); // 30 s
  }, 100);
})();
)JS";

std::filesystem::path data_dir() { return std::filesystem::path(g_get_user_data_dir()) / "gtkdrop" / "webkit"; }

} // namespace

struct NexusBrowser::Impl {
    GtkWindow *window = nullptr;
    WebKitWebView *view = nullptr;
    AdwBanner *banner = nullptr;
    std::string page; // the file page of the current request
    Done done;
    Waiting waiting;
    bool saw_login = false;

    void finish(std::optional<std::string> nxm, const std::string &error) {
        if (!done)
            return;
        auto cb = std::exchange(done, nullptr);
        waiting = nullptr;
        adw_banner_set_revealed(banner, FALSE);
        cb(std::move(nxm), error);
    }

    void tell(const std::string &what) {
        adw_banner_set_title(banner, what.c_str());
        adw_banner_set_revealed(banner, TRUE);
        if (waiting)
            waiting(what);
    }

    void got_nxm(const std::string &url) {
        webkit_web_view_stop_loading(view);
        finish(url, {});
    }
};

NexusBrowser::NexusBrowser(GtkWindow *parent) : impl_(new Impl) {
    auto *d = impl_;
    const auto dir = data_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    WebKitNetworkSession *session = webkit_network_session_new((dir / "data").c_str(), (dir / "cache").c_str());
    webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(session),
                                                 (dir / "cookies.sqlite").c_str(),
                                                 WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);

    WebKitUserContentManager *content = webkit_user_content_manager_new();
    WebKitUserScript *script = webkit_user_script_new(kClickScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, nullptr, nullptr);
    webkit_user_content_manager_add_script(content, script);
    webkit_user_script_unref(script);
    webkit_user_content_manager_register_script_message_handler(content, "gtkdrop", nullptr);
    g_signal_connect(content, "script-message-received::gtkdrop",
                     G_CALLBACK(+[](WebKitUserContentManager *, JSCValue *value, gpointer p) {
        auto *d = static_cast<Impl *>(p);
        char *text = jsc_value_to_string(value);
        const std::string message = text ? text : "";
        g_free(text);
        if (message.find("\"type\":\"nxm\"") != std::string::npos) {
            const auto start = message.find("nxm://");
            const auto end = message.find('"', start);
            if (start != std::string::npos && end != std::string::npos)
                d->got_nxm(message.substr(start, end - start));
        } else if (message.find("\"type\":\"clicked\"") != std::string::npos) {
            adw_banner_set_revealed(d->banner, FALSE);
        } else if (message.find("\"type\":\"stuck\"") != std::string::npos && d->done) {
            d->tell("Couldn’t start this download by itself. Click “Slow download” on the page.");
        }
    }), d);

    d->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session,
                                           "user-content-manager", content, nullptr));
    g_object_unref(session);
    g_object_unref(content);
    gtk_widget_set_vexpand(GTK_WIDGET(d->view), TRUE);
    // Only buttons to click here: no audio/video. Nexus' ads play media, and WebKit's media needs GStreamer plugins
    // many systems lack (a missing audio sink leaves its web process stuck).
    WebKitSettings *settings = webkit_web_view_get_settings(d->view);
    webkit_settings_set_enable_media(settings, FALSE);
    webkit_settings_set_enable_webaudio(settings, FALSE);
    webkit_settings_set_enable_media_stream(settings, FALSE);
    webkit_settings_set_enable_webrtc(settings, FALSE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, TRUE);

    // The page's process crashed or hung up: start it again on the same file instead of sitting on a blank window.
    g_signal_connect(d->view, "web-process-terminated", G_CALLBACK(+[](WebKitWebView *view,
                                                                       WebKitWebProcessTerminationReason reason,
                                                                       gpointer p) {
        auto *d = static_cast<Impl *>(p);
        g_warning("Nexus window: the page's process ended (%s); reloading",
                  reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed" : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT
                                                                             ? "out of memory" : "terminated");
        if (!d->page.empty() && d->done)
            webkit_web_view_load_uri(view, d->page.c_str());
    }), d);

    // The page hands the link over by navigating to nxm:// (a top-level or frame navigation, or a new window).
    g_signal_connect(d->view, "decide-policy", G_CALLBACK(+[](WebKitWebView *, WebKitPolicyDecision *decision,
                                                              WebKitPolicyDecisionType type, gpointer p) -> gboolean {
        if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
            return FALSE;
        WebKitNavigationAction *action =
                webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
        if (!uri || g_ascii_strncasecmp(uri, "nxm://", 6) != 0)
            return FALSE;
        webkit_policy_decision_ignore(decision);
        static_cast<Impl *>(p)->got_nxm(uri);
        return TRUE;
    }), d);

    // Signed out: Nexus sends the window to its login page. Once signed in, go back to the file.
    g_signal_connect(d->view, "load-changed", G_CALLBACK(+[](WebKitWebView *view, WebKitLoadEvent event, gpointer p) {
        auto *d = static_cast<Impl *>(p);
        if (event != WEBKIT_LOAD_FINISHED || !d->done)
            return;
        const char *uri = webkit_web_view_get_uri(view);
        const std::string u = uri ? uri : "";
        if (u.find("users.nexusmods.com") != std::string::npos) {
            d->saw_login = true;
            d->tell("Sign in to Nexus Mods here once. GTKdrop remembers it and then carries on.");
        } else if (d->saw_login && u.find("www.nexusmods.com") != std::string::npos && u != d->page) {
            d->saw_login = false;
            adw_banner_set_revealed(d->banner, FALSE);
            webkit_web_view_load_uri(view, d->page.c_str());
        }
    }), d);

    d->window = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(d->window, "Nexus Mods Downloads");
    gtk_window_set_default_size(d->window, 1000, 760);
    if (parent)
        gtk_window_set_transient_for(d->window, parent);
    gtk_window_set_hide_on_close(d->window, TRUE);
    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
    d->banner = ADW_BANNER(adw_banner_new(""));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(d->banner));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(d->view));
    adw_window_set_content(ADW_WINDOW(d->window), toolbar);
    g_object_ref_sink(d->window);

    // Closing the window stops the download in progress.
    g_signal_connect(d->window, "close-request", G_CALLBACK(+[](GtkWindow *, gpointer p) -> gboolean {
        static_cast<Impl *>(p)->finish(std::nullopt, "the Nexus Mods window was closed");
        return FALSE; // hide (hide-on-close)
    }), d);
}

NexusBrowser::~NexusBrowser() {
    impl_->done = nullptr;
    gtk_window_destroy(impl_->window);
    g_object_unref(impl_->window);
    delete impl_;
}

void NexusBrowser::request(const std::string &domain, int mod_id, int file_id, Done done, Waiting waiting) {
    auto *d = impl_;
    d->finish(std::nullopt, "replaced by another download");
    d->done = std::move(done);
    d->waiting = std::move(waiting);
    d->saw_login = false;
    // GTKDROP_NEXUS_WEB_BASE: tests point the window at a local copy of a file page.
    const char *base = g_getenv("GTKDROP_NEXUS_WEB_BASE");
    d->page = std::string(base ? base : "https://www.nexusmods.com") + "/" + domain + "/mods/" + std::to_string(mod_id) +
              "?tab=files&file_id=" + std::to_string(file_id) + "&nmm=1";
    adw_banner_set_revealed(d->banner, FALSE);
    gtk_window_present(d->window);
    webkit_web_view_load_uri(d->view, d->page.c_str());
}

void NexusBrowser::cancel() {
    // The callback may delete this browser: nothing of it is touched after finish().
    webkit_web_view_stop_loading(impl_->view);
    gtk_widget_set_visible(GTK_WIDGET(impl_->window), FALSE);
    impl_->finish(std::nullopt, "cancelled");
}

} // namespace gtkdrop::ui
