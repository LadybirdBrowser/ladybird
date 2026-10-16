/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullRawPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Forward.h>
#include <LibCore/GeolocationProvider.h>
#include <LibDatabase/Forward.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/Forward.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>
#include <LibMain/Main.h>
#include <LibRequests/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>
#include <LibWebCommon/CSS/PreferredContrast.h>
#include <LibWebCommon/CSS/PreferredMotion.h>
#include <LibWebCommon/Clipboard/SystemClipboard.h>
#include <LibWebCommon/HTML/ActivateTab.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/PreparedNavigationDescriptor.h>
#include <LibWebCommon/HTML/ReplicatedNavigableState.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>
#include <LibWebCommon/HTML/VisibilityState.h>
#include <LibWebCommon/Loader/NavigatorCompatibilityMode.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/BrowserProcess.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/DownloadStore.h>
#include <LibWebView/ExternalURLHandler.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Options.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Profile.h>
#include <LibWebView/Settings.h>
#include <LibWebView/StorageJar.h>
#include <LibWebView/WebDriverSessionConfig.h>
#include <RequestServer/SiteBinding.h>

#if defined(AK_OS_MACOS)
#    include <LibIPC/TransportBootstrapMach.h>
#endif

namespace Compositing {

struct KeyEvent;
struct MouseEvent;
struct PinchEvent;

}

namespace WebView {

struct ApplicationSettingsObserver;
struct ApplicationBookmarkStoreObserver;

class WEBVIEW_API Application : public DevTools::DevToolsDelegate {
    AK_MAKE_NONCOPYABLE(Application);

public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~Application();

    ErrorOr<int> execute();

    static Application& the() { return *s_the; }
    static bool is_initialized() { return s_the != nullptr; }

    static Settings& settings() { return *the().m_settings; }
    static Profile const& profile() { return *the().m_profile; }
    BrowserProcess& browser_process() { return *m_browser_process; }
    bool should_exit_after_profile_coordination() const { return m_should_exit_after_profile_coordination; }
    static AutocompleteService& autocomplete_service() { return *the().m_autocomplete_service; }

    static BrowserOptions const& browser_options() { return the().m_browser_options; }
    static RequestServerOptions const& request_server_options() { return the().m_request_server_options; }
    static WebContentOptions& web_content_options() { return the().m_web_content_options; }

    SpellChecker* spell_checker() { return m_spell_checker.ptr(); }
    void set_spell_checker(OwnPtr<SpellChecker>);
    static FontService& font_service() { return *the().m_font_service; }
    static FontServiceHost& font_service_host() { return *the().m_font_service_host; }
    JsonValue const& site_compatibility_data() const { return m_site_compatibility_data; }
    bool blocks_pop_ups() const;
    ErrorOr<void> reload_site_compatibility_data();

    bool claim_cpu_profiler(ProcessType);
    void set_cpu_profiler_process(Core::Process, OwnPtr<Core::File> control_socket);

    virtual Optional<String> system_font_family() const { return {}; }
    virtual Optional<String> ui_font_family() const { return {}; }

    static RequestServerManager& request_server_manager() { return *the().m_request_server_manager; }
    // The UI process's own client of the session's RequestServer with no site.
    static ErrorOr<NonnullRefPtr<Requests::RequestClient>> request_server_client(IsPrivate = IsPrivate::No);
    // The control connection of the default session's RequestServer with no site.
    static Requests::RequestControlClient& request_server_control_client();

    virtual bool supports_system_menu_bar() const { return false; }
    virtual bool supports_vertical_tabs() const { return false; }
    virtual bool supports_content_blocking_controls() const { return false; }
    virtual bool supports_private_browsing_windows() const { return false; }
    virtual bool supports_client_side_window_decorations() const { return false; }
    virtual bool supports_picture_in_picture() const;

    // What startup recovery does with the windows the last run left open. Only a frontend that shows the launch plan
    // asks for more than None.
    virtual SessionLaunchPolicy launch_restore_policy() const;

    // Returns true if the platform sends momentum scroll events after a touchpad flick.
    // If it does not, the compositor makes the fling.
    virtual bool platform_reports_scroll_momentum() const { return true; }
    // Returns true if a touchpad may pull a scroller past the edges of its scroll range, from where it springs back.
    // This needs the platform to say when the fingers lift from the touchpad, or the scroller would stay stretched.
    virtual bool platform_scrolls_elastically() const { return false; }
    // Returns true if a press on a scrollbar track beside the thumb jumps to that spot, rather than paging toward it.
    virtual bool platform_scrollbar_track_press_jumps() const { return false; }
    // Tells the compositor of a change to what platform_scrollbar_track_press_jumps() returns.
    void platform_scrollbar_track_press_behavior_changed();
    // The languages the user prefers, as the platform lists them (BCP 47 tags, most preferred first).
    virtual Vector<String> platform_preferred_languages() const { return {}; }

    virtual OwnPtr<PictureInPictureWindow> create_picture_in_picture_window(WebContentPage& requesting_page, CanonicalTraversable&, Gfx::IntSize video_size);

    void appearance_changed(Badge<ApplicationSettingsObserver>);
    void tab_settings_changed(Badge<ApplicationSettingsObserver>);
    void languages_changed(Badge<ApplicationSettingsObserver>);

    // The spoofing choices in effect: what every page and worker reports from its navigator.
    String user_agent_string() const;
    Web::NavigatorCompatibilityMode navigator_compatibility_mode() const;

    static BookmarkStore& bookmark_store() { return *the().m_bookmark_store; }
    void update_bookmark_action_for_current_web_view();
    void update_actions_for_active_web_view();
    void bookmarks_changed(Badge<ApplicationBookmarkStoreObserver>);

    void background_networking_settings_changed(Badge<ApplicationSettingsObserver>);

    bool content_blocking_enabled() const;
    bool has_content_blocking_rules() const;
    void apply_content_blocking_policy(WebContentPage&);
    void content_blocker_settings_changed(Badge<ApplicationSettingsObserver>);

    bool content_blocker_list_update_in_progress() const;
    void update_content_blocker_lists(Badge<SettingsUI>);
    void download_content_blocker_list_if_needed(Badge<Application, SettingsUI>, StringView identifier);
    ErrorOr<void> import_local_content_blocker_list(String name, String contents);
    void remove_content_blocker_list(Badge<SettingsUI>, StringView identifier);
    Optional<UnixDateTime> content_blocker_list_last_updated_at(StringView identifier) const;

    struct BookmarkID {
        String id;
        Optional<String> target_folder_id;
        Optional<String> parent_folder_id;
    };
    virtual Optional<BookmarkID> bookmark_item_id_for_context_menu() const { return {}; }
    virtual void show_bookmark_context_menu(Gfx::IntPoint, Optional<BookmarkItem const&>, [[maybe_unused]] Optional<String const&> target_folder_id, [[maybe_unused]] Optional<String const&> parent_folder_id) { }

    struct BookmarkDialogResult {
        Variant<BookmarkItem::Bookmark, BookmarkItem::Folder> data;
        Optional<String> target_folder_id;
    };
    using BookmarkPromise = Core::Promise<BookmarkDialogResult>;

    virtual NonnullRefPtr<BookmarkPromise> display_add_bookmark_dialog(Optional<String const&> target_folder_id = {}) const;
    virtual NonnullRefPtr<BookmarkPromise> display_edit_bookmark_dialog([[maybe_unused]] BookmarkItem const& current_bookmark, Optional<String const&> parent_folder_id) const;

    virtual NonnullRefPtr<BookmarkPromise> display_add_bookmark_folder_dialog(Optional<String const&> default_title = {}, Optional<String const&> target_folder_id = {}) const;
    virtual NonnullRefPtr<BookmarkPromise> display_edit_bookmark_folder_dialog([[maybe_unused]] BookmarkItem const& current_folder, Optional<String const&> parent_folder_id) const;

    static BrowsingSession& default_session() { return *the().m_default_session; }

    static NonnullRefPtr<BrowsingSession> session_for_new_view(IsPrivate);

    static RefPtr<BrowsingSession> existing_session(IsPrivate);

    // A RequestServer client uses the cookies of the browsing session it was created for, whatever RequestServer says.
    void did_connect_request_server_client(int client_id, BrowsingSession&, RequestServer::SiteBinding);
    void did_disconnect_request_server_client(int client_id);
    // Installs what every RequestServer asks the UI process for: cookies, and where to put the HSTS policies and
    // cookies of responses.
    void configure_request_server_control_client(Requests::RequestControlClient&);
    RefPtr<BrowsingSession> session_for_request_server_client(int client_id) const;
    Vector<int> request_server_client_ids_for_testing(BrowsingSession const&) const;

    // NB: Null once that session has ended, so a closed private tab is not offered back afterwards.
    static SessionStore* session_store(IsPrivate);

    static ProcessManager& process_manager() { return *the().m_process_manager; }
#if defined(AK_OS_WINDOWS)
    static IPC::HandleBrokerServer& handle_broker_server() { return *the().m_handle_broker_server; }
#endif

    // Runs the handler when the ImageDecoder with the given child ID exits.
    void set_image_decoder_exit_handler(u64 child_id, Function<void()>);
#if defined(AK_OS_MACOS)
    static IPC::TransportBootstrapMachServer& transport_bootstrap_server() { return the().m_transport_bootstrap_server; }
    void set_browser_process_transport_handler(Function<void(NonnullOwnPtr<IPC::Transport>)> handler);
#endif

    ErrorOr<Core::GeolocationProvider::RequestId, Core::GeolocationError> request_geolocation_position(Core::GeolocationProvider::SuccessCallback on_success, Core::GeolocationProvider::ErrorCallback on_error);
    void cancel_geolocation_position_request(Core::GeolocationProvider::RequestId);
    ErrorOr<Core::GeolocationProvider::WatchId, Core::GeolocationError> start_watching_geolocation_position(Core::GeolocationProvider::SuccessCallback on_success, Core::GeolocationProvider::ErrorCallback on_error);
    void stop_watching_geolocation_position(Core::GeolocationProvider::WatchId);

    ErrorOr<NonnullRefPtr<WebContentPage>> open_page_for_new_tab(IsPrivate);
    struct ChildFrameWebContentProcess {
        NonnullRefPtr<WebContentClient> client;
        Web::PageId page_id { 0 };
    };
    ErrorOr<ChildFrameWebContentProcess> launch_child_frame_web_content_process(IsPrivate, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables, Web::HTML::CrossProcessId root_navigable_id, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, Web::HTML::VisibilityState system_visibility_state);
    Web::PageId allocate_page_id();
    Web::HTML::CrossProcessIdAllocator allocate_cross_process_id_allocator();
    Web::HTML::CrossProcessId allocate_ui_process_cross_process_id();

    void reset_private_browsing_session();

    void notify_webdriver_window_created(String const& handle);
    void notify_webdriver_window_closed(String const& handle);
    void webdriver_browser_connection_died(Badge<WebDriverBrowserConnection>);
    void push_webdriver_session_config(ViewImplementation&);
    void push_webdriver_session_config(WebContentPage&);
    void update_webdriver_session_config(Badge<WebDriverBrowserConnection>, Function<void(WebDriverSessionConfig&)> update);
    Optional<u64> webdriver_page_load_timeout() const;
    void complete_webdriver_content_command(u64 command_id, Web::WebDriver::Response);

    Web::CompositorContextId allocate_compositor_context_id();
    ErrorOr<void> connect_web_content_to_compositor(WebContentClient&);
    // Hands the Compositor its end of a video presentation channel for the renderer. False if the renderer has no
    // Compositor connection to pair it with.
    bool connect_video_presentation_to_compositor(WebContentClient&, IPC::TransportHandle compositor_end);
    ErrorOr<IPC::TransportHandle> connect_new_compositor_canvas_client();
    void register_compositor_context(WebContentClient&, Web::CompositorContextId, Optional<Web::PageId> page_id);
    ErrorOr<void> try_register_compositor_context(WebContentClient&, Web::CompositorContextId, Optional<Web::PageId> page_id);
    void update_compositor_viewport(Web::CompositorContextId, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress = Compositing::WindowResizingInProgress::No);
    void update_compositor_ui_overlays(Web::CompositorContextId, Compositing::UIOverlays const&);
    void update_compositor_display_metadata(Web::CompositorContextId, Optional<u64> display_id, double refresh_rate);
    void update_compositor_context_visibility(Web::CompositorContextId, Web::HTML::VisibilityState);
    bool handle_and_dispatch_key_event_in_compositor(Web::CompositorContextId, Web::KeyEvent const&, bool allow_keyboard_scroll);
    void handle_pinch_event_in_compositor(Web::CompositorContextId, Web::PinchEvent const&);
    bool handle_and_dispatch_mouse_event_in_compositor(Web::CompositorContextId, Web::MouseEvent const&);
    void notify_compositor_presented_bitmap_ready_to_paint(Web::CompositorContextId, i32 bitmap_id);

    Function<void()> on_compositor_process_death;

    virtual Optional<ViewImplementation&> active_web_view() const { return {}; }
    virtual Vector<ViewImplementation&> active_window_web_views() const { return {}; }
    virtual Optional<ViewImplementation&> activate_tab_with_url(URL::URL const&, URL::ExcludeFragment) const { return {}; }

    virtual Optional<ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const { return {}; }
    virtual void open_url_in_new_tab(URL::URL const&, Web::HTML::ActivateTab) const;
    virtual void open_urls_in_new_tabs(ReadonlySpan<URL::URL>) const;
    virtual void open_url_in_new_window(URL::URL const&, IsPrivate) { }
    virtual void open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor, Web::HTML::ActivateTab) const;
    virtual void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor, IsPrivate) { }

    virtual void resolve_external_url_handler(URL::URL const&, ExternalURLHandlerCallback callback) const { callback(nullptr); }

    void activate_or_open_tab_with_url(URL::URL const&) const;
    void open_bookmark_in_new_tab(String const& bookmark_id, Web::HTML::ActivateTab) const;
    void open_bookmark_folder_in_new_tabs(String const& folder_id) const;
    void open_bookmark_in_new_window(String const& bookmark_id, IsPrivate);

    Main::Arguments const& command_line_arguments() const { return m_arguments; }

    ChildIdentity add_child_process(Process&&);

    // FIXME: Should these methods be part of Application, instead of deferring to ProcessManager?
#if defined(AK_OS_MACH)
    void set_process_task_name_port(pid_t, Core::MachPort&&);
#endif
    Optional<Process&> find_process(pid_t);

    virtual bool should_capture_web_content_output() const { return false; }

    virtual ErrorOr<LexicalPath> default_path_for_downloaded_file(ByteString const& file) const;
    ErrorOr<LexicalPath> path_for_downloaded_file(ByteString const& file) const;

    virtual void display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const;
    virtual void display_error_dialog(StringView error_message) const;
    ErrorOr<String> download_file_path_for_frontend_action(FileDownloader::Download const&) const;
    ErrorOr<String> download_directory_path_for_frontend_action(FileDownloader::Download const&) const;
    virtual void open_download(FileDownloader::Download const&) const;
    virtual void show_download_in_folder(FileDownloader::Download const&) const;

    void offer_newest_pending_crash_report();

    // FIXME: We should implement UI-agnostic platform APIs to interact with the system clipboard.
    enum class ClipboardType : u8 {
        Text,
        Selection,
    };
    virtual bool supports_clipboard_type(ClipboardType) const;
    virtual Utf16String clipboard_text(ClipboardType = ClipboardType::Text) const;
    virtual void set_clipboard_text(String, ClipboardType = ClipboardType::Text);

    virtual Web::Clipboard::SystemClipboardItem clipboard_item() const { return m_clipboard; }
    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem item) { m_clipboard = move(item); }
    void insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation);

    struct BrowsingDataSizes {
        u64 cache_size_since_requested_time { 0 };
        u64 total_cache_size { 0 };

        u64 site_data_size_since_requested_time { 0 };
        u64 total_site_data_size { 0 };
    };
    NonnullRefPtr<Core::Promise<BrowsingDataSizes>> estimate_browsing_data_size_accessed_since(UnixDateTime since);

    struct ClearBrowsingDataOptions {
        enum class Delete {
            No,
            Yes,
        };

        UnixDateTime since { UnixDateTime::earliest() };
        Delete delete_cached_files { Delete::No };
        Delete delete_history { Delete::No };
        Delete delete_download_history { Delete::No };
        Delete delete_site_data { Delete::No };
    };
    NonnullRefPtr<Core::Promise<Empty>> clear_browsing_data(ClearBrowsingDataOptions const&);

    Action& reload_action() { return *m_reload_action; }
    Action& copy_selection_action() { return *m_copy_selection_action; }
    Action& undo_action() { return *m_undo_action; }
    Action& redo_action() { return *m_redo_action; }

    // Reflects the active view's undo/redo availability in the shared actions; call when the
    // active view changes.
    void update_editing_history_actions();
    Action& cut_selection_action() { return *m_cut_selection_action; }
    Action& paste_action() { return *m_paste_action; }
    Action& select_all_action() { return *m_select_all_action; }

    Action& open_about_page_action() { return *m_open_about_page_action; }
    Action& open_downloads_page_action() { return *m_open_downloads_page_action; }
    Action& open_settings_page_action() { return *m_open_settings_page_action; }

    Menu& zoom_menu() { return *m_zoom_menu; }
    Action& zoom_in_action() { return *m_zoom_in_action; }
    Action& zoom_out_action() { return *m_zoom_out_action; }
    Action& reset_zoom_action() { return *m_reset_zoom_action; }

    Menu& color_scheme_menu() { return *m_color_scheme_menu; }
    Menu& contrast_menu() { return *m_contrast_menu; }
    Menu& motion_menu() { return *m_motion_menu; }

    Action& toggle_vertical_tabs_expanded_action() { return *m_toggle_vertical_tabs_expanded_action; }

    Action& toggle_menu_bar_action() { return *m_toggle_menu_bar_action; }

    Menu& bookmarks_menu() { return *m_bookmarks_menu; }
    void toggle_bookmark_for_view(ViewImplementation&);
    Action& toggle_bookmark_bar_action() { return *m_toggle_bookmark_bar_action; }

    Menu& history_menu() { return *m_history_menu; }

    Menu& inspect_menu() { return *m_inspect_menu; }
    Action& view_source_action() { return *m_view_source_action; }

    Action& report_website_issue_action() { return *m_report_website_issue_action; }
    void update_report_website_issue_action();

    Menu& debug_menu() { return *m_debug_menu; }

    FileDownloader& file_downloader() { return m_file_downloader; }

    void apply_view_options(Badge<ViewImplementation>, ViewImplementation&, WebContentPage&);

    ErrorOr<void> toggle_devtools_enabled();
    ErrorOr<void> launch_devtools_client();
    void refresh_tab_list();

    Optional<Core::TimeZoneWatcher&> time_zone_watcher();

    // Called by the UI when it turns out it cannot present GPU textures shared by the Compositor
    // (e.g. importing a shared Direct3D texture failed), so the Compositor falls back to
    // CPU-shared backing stores.
    void notify_compositor_gpu_presentation_unavailable();

    // Runs the callback with the GPU that hardware decoders must output to for the Compositor to display their frames,
    // if there is one, as soon as the Compositor has reported it.
    using CompositorGpuDeviceIdentityCallback = Function<void(Optional<Gfx::GpuDeviceIdentity> const&)>;
    void when_compositor_gpu_device_identity_is_known(CompositorGpuDeviceIdentityCallback);

protected:
    explicit Application(Optional<ByteString> ladybird_binary_path = {});

    ErrorOr<void> initialize(Main::Arguments const&);

    virtual void process_did_exit(Process&&, Optional<int> exit_status);

    virtual void create_platform_arguments(Core::ArgsParser&) { }
    virtual void create_platform_options(BrowserOptions&, RequestServerOptions&, WebContentOptions&) { }
    virtual void create_platform_actions() { }
    virtual Core::EventLoop& create_platform_event_loop();

    virtual bool should_coordinate_browser_process() const { return true; }
    // An application whose state must not leak between runs — or into a developer's own browsing state.
    virtual bool should_use_temporary_profile_by_default() const { return false; }
    virtual SiteIsolationMode default_site_isolation_mode() const { return SiteIsolationMode::IFrame; }

    virtual Optional<ByteString> ask_user_for_download_path([[maybe_unused]] ByteString const& file) const { return {}; }

    virtual void display_crash_report_notification([[maybe_unused]] ByteString const& report_name) { }

    virtual void update_tabs_display() const { }

    virtual void rebuild_bookmarks_menu() const { }

    virtual void on_recently_closed_entries_changed() const { }

    virtual void on_devtools_enabled() const;
    virtual void on_devtools_disabled() const;

    Main::Arguments& arguments() { return m_arguments; }

    bool has_spare_web_content_process() const { return m_spare_web_content_process; }

private:
    ErrorOr<NonnullRefPtr<WebContentClient>> create_web_content_client(IsPrivate, Web::PageId initial_page_id, Optional<Web::HTML::CrossProcessId> navigable_to_adopt = {}, Optional<Web::HTML::CrossProcessId> initial_document_state_id = {}, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables = {}, Optional<Web::HTML::SessionHistoryEntryDescriptor> canonical_initial_history_entry = {}, Web::HTML::VisibilityState system_visibility_state = Web::HTML::VisibilityState::Hidden);
    ErrorOr<void> launch_services();
    void launch_spare_web_content_process();
    ErrorOr<void> launch_compositor_process();
    void compositor_reported_gpu_device_identity(Optional<Gfx::GpuDeviceIdentity>);
    void handle_compositor_process_death();
    void recover_compositor_process();
    void crash_compositor_process();
    void offer_crash_report(ByteString const& report_name);
    ErrorOr<void> launch_request_server();
    ErrorOr<void> launch_devtools_server();
    // Returns whether the combined rules changed.
    ErrorOr<bool> load_content_blocker_lists();
    void apply_content_blocker_settings();
    void rebuild_content_blocker_list_paths();
    ByteString content_blocker_list_path(StringView identifier) const;
    ErrorOr<void> save_content_blocker_list(ByteString const& path, ReadonlyBytes);
    enum class ContentBlockerListUpdateTrigger {
        Automatic,
        UserInitiated,
    };
    void start_content_blocker_list_update(ContentBlockerListUpdateTrigger, Optional<StringView> requested_identifier = {});
    void start_next_content_blocker_list_update();
    void did_receive_content_blocker_list_update_data(ReadonlyBytes);
    void stop_current_content_blocker_list_update_and_continue();
    void finish_current_content_blocker_list_update();
    ErrorOr<NonnullRawPtr<Core::GeolocationProvider>> ensure_geolocation_provider();

    void initialize_actions();
    void update_vertical_tabs_action();

    WebDriverBrowserConnection* webdriver_browser_connection();

    struct MenuData {
        Menu& menu;
        ReadonlySpan<BookmarkItem> items;
        Optional<String const&> target_folder_id;
    };
    void create_bookmark_menu_items(Optional<MenuData> = {});

    virtual Vector<BookmarkItem::Bookmark> bookmarks_for_all_tabs_in_current_window() const;

    virtual Vector<DevTools::TabDescription> tab_list() const override;
    virtual Vector<DevTools::CSSProperty> css_property_list() const override;
    virtual void navigate_tab(DevTools::TabDescription const&, String const&) const override;
    virtual void reload_tab(DevTools::TabDescription const&, bool) const override;
    virtual void traverse_the_history_by_delta(DevTools::TabDescription const&, int) const override;
    virtual Vector<HTTP::Cookie::Cookie> cookies(DevTools::TabDescription const&) const override;
    virtual ErrorOr<void> set_cookie(DevTools::TabDescription const&, Optional<HTTP::Cookie::Cookie>, HTTP::Cookie::Cookie) const override;
    virtual void delete_cookies(DevTools::TabDescription const&, Vector<HTTP::Cookie::Cookie>) const override;
    virtual void listen_for_host_cookie_changes(DevTools::TabDescription const&, OnHostCookieChange) const override;
    virtual void stop_listening_for_host_cookie_changes(DevTools::TabDescription const&) const override;
    virtual void inspect_storage(DevTools::TabDescription const&, Web::StorageAPI::StorageEndpointType, OnStorageItemsReceived) const override;
    virtual ErrorOr<Optional<String>> set_storage_item(DevTools::TabDescription const&, Web::StorageAPI::StorageEndpointType, String const&, String const&, String const&) const override;
    virtual ErrorOr<Optional<String>> remove_storage_item(DevTools::TabDescription const&, Web::StorageAPI::StorageEndpointType, String const&, String const&) const override;
    virtual ErrorOr<void> clear_storage(DevTools::TabDescription const&, Web::StorageAPI::StorageEndpointType, String const&) const override;
    virtual u64 add_storage_change_listener(DevTools::TabDescription const&, OnStorageChange) const override;
    virtual void remove_storage_change_listener(DevTools::TabDescription const&, u64) const override;
    virtual void inspect_indexed_database_storage(DevTools::TabDescription const&, OnIndexedDBInspectionComplete) const override;
    virtual void inspect_indexed_database_objects(DevTools::TabDescription const&, String const&, Optional<JsonArray>, JsonObject, OnIndexedDBInspectionComplete) const override;
    virtual void delete_indexed_database(DevTools::TabDescription const&, String const&, String const&, OnIndexedDBInspectionComplete) const override;
    virtual void clear_indexed_database_object_store(DevTools::TabDescription const&, String const&, String const&, OnIndexedDBInspectionComplete) const override;
    virtual void delete_indexed_database_record(DevTools::TabDescription const&, String const&, String const&, OnIndexedDBInspectionComplete) const override;
    virtual u64 add_indexed_database_change_listener(DevTools::TabDescription const&, OnIndexedDatabaseChange) const override;
    virtual void remove_indexed_database_change_listener(DevTools::TabDescription const&, u64) const override;
    virtual void inspect_tab(DevTools::TabDescription const&, OnTabInspectionComplete) const override;
    virtual void inspect_accessibility_tree(DevTools::TabDescription const&, OnAccessibilityTreeInspectionComplete) const override;
    virtual void listen_for_dom_properties(DevTools::TabDescription const&, OnDOMNodePropertiesReceived) const override;
    virtual void stop_listening_for_dom_properties(DevTools::TabDescription const&) const override;
    virtual void inspect_dom_node(DevTools::TabDescription const&, DOMNodeProperties::Type, Web::UniqueNodeID, Optional<Web::CSS::PseudoElement>, JsonObject options = {}) const override;
    virtual void clear_inspected_dom_node(DevTools::TabDescription const&) const override;
    virtual void start_node_picker(DevTools::TabDescription const&, OnNodePickerEvent) const override;
    virtual void stop_node_picker(DevTools::TabDescription const&) const override;
    virtual void clear_node_picker(DevTools::TabDescription const&) const override;
    virtual void inspect_grid_layouts(DevTools::TabDescription const&, Web::UniqueNodeID, OnGridLayoutsReceived) const override;
    virtual void inspect_current_grid(DevTools::TabDescription const&, Web::UniqueNodeID, OnCurrentGridReceived) const override;
    virtual void inspect_current_flexbox(DevTools::TabDescription const&, Web::UniqueNodeID, bool, OnCurrentFlexboxReceived) const override;
    virtual void highlight_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID, Optional<Web::CSS::PseudoElement>) const override;
    virtual void clear_highlighted_dom_node(DevTools::TabDescription const&) const override;
    virtual void highlight_flexbox(DevTools::TabDescription const&, Web::UniqueNodeID, JsonValue) const override;
    virtual void clear_flexbox_highlight(DevTools::TabDescription const&, Web::UniqueNodeID) const override;
    virtual void highlight_grid(DevTools::TabDescription const&, Web::UniqueNodeID, JsonValue) const override;
    virtual void clear_grid_highlight(DevTools::TabDescription const&, Web::UniqueNodeID) const override;
    virtual void listen_for_dom_mutations(DevTools::TabDescription const&, OnDOMMutationReceived) const override;
    virtual void stop_listening_for_dom_mutations(DevTools::TabDescription const&) const override;
    virtual void get_dom_node_inner_html(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeHTMLReceived) const override;
    virtual void get_dom_node_outer_html(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeHTMLReceived) const override;
    virtual void set_dom_node_outer_html(DevTools::TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const override;
    virtual void set_dom_node_text(DevTools::TabDescription const&, Web::UniqueNodeID, String const&, OnDOMNodeEditComplete) const override;
    virtual void set_dom_node_tag(DevTools::TabDescription const&, Web::UniqueNodeID, Utf16FlyString const&, OnDOMNodeEditComplete) const override;
    virtual void add_dom_node_attributes(DevTools::TabDescription const&, Web::UniqueNodeID, ReadonlySpan<Attribute>, OnDOMNodeEditComplete) const override;
    virtual void replace_dom_node_attribute(DevTools::TabDescription const&, Web::UniqueNodeID, Utf16FlyString const&, ReadonlySpan<Attribute>, OnDOMNodeEditComplete) const override;
    virtual void create_child_element(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const override;
    virtual void insert_dom_node_before(DevTools::TabDescription const&, Web::UniqueNodeID, Web::UniqueNodeID, Optional<Web::UniqueNodeID>, OnDOMNodeEditComplete) const override;
    virtual void clone_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const override;
    virtual void remove_dom_node(DevTools::TabDescription const&, Web::UniqueNodeID, OnDOMNodeEditComplete) const override;
    virtual void retrieve_style_sheets(DevTools::TabDescription const&, OnStyleSheetsReceived) const override;
    virtual void retrieve_style_sheet_source(DevTools::TabDescription const&, Web::CSS::StyleSheetIdentifier const&) const override;
    virtual void listen_for_style_sheet_sources(DevTools::TabDescription const&, OnStyleSheetSourceReceived) const override;
    virtual void stop_listening_for_style_sheet_sources(DevTools::TabDescription const&) const override;
    virtual void retrieve_sources(DevTools::TabDescription const&, OnSourcesReceived) const override;
    virtual void retrieve_source(DevTools::TabDescription const&, Web::HTML::ScriptRegistryIdentifier, OnSourceReceived) const override;
    virtual void listen_for_sources(DevTools::TabDescription const&, OnSourceAvailable) const override;
    virtual void stop_listening_for_sources(DevTools::TabDescription const&) const override;
    virtual void attach_debugger(DevTools::TabDescription const&, OnDebuggerPaused, OnDebuggerResumed) const override;
    virtual void configure_debugger(DevTools::TabDescription const&, DebuggerConfiguration) const override;
    virtual void detach_debugger(DevTools::TabDescription const&) const override;
    virtual void interrupt_debugger(DevTools::TabDescription const&) const override;
    virtual void resume_debugger(DevTools::TabDescription const&, DebuggerResumeMode) const override;
    virtual void update_debugger_blackboxing(DevTools::TabDescription const&, Utf16String, Vector<DebuggerBlackboxRange>, DebuggerBlackboxingOperation) const override;
    virtual void set_debugger_breakpoint(DevTools::TabDescription const&, DebuggerBreakpointLocation, DebuggerBreakpointOptions, OnDebuggerBreakpointOperationComplete) const override;
    virtual void remove_debugger_breakpoint(DevTools::TabDescription const&, DebuggerBreakpointLocation, OnDebuggerBreakpointOperationComplete) const override;
    virtual void retrieve_debugger_environments(DevTools::TabDescription const&, u64 frame_id, OnDebuggerEnvironmentsReceived) const override;
    virtual void evaluate_javascript_in_debugger_frame(DevTools::TabDescription const&, u64 frame_id, String const&, OnDebuggerEvaluationComplete) const override;
    virtual void retrieve_debugger_object_properties(DevTools::TabDescription const&, u64 object_id, OnDebuggerObjectPropertiesReceived) const override;
    virtual void retrieve_debugger_source_positions(DevTools::TabDescription const&, Web::HTML::ScriptRegistryIdentifier, OnDebuggerSourcePositionsReceived) const override;
    virtual void resolve_dom_node_url(DevTools::TabDescription const&, Optional<Web::UniqueNodeID>, String const&, OnResolvedURLReceived) const override;
    virtual void evaluate_javascript(DevTools::TabDescription const&, String const&, OnScriptEvaluationComplete) const override;
    virtual void listen_for_console_messages(DevTools::TabDescription const&, OnConsoleMessage) const override;
    virtual void stop_listening_for_console_messages(DevTools::TabDescription const&) const override;
    virtual void listen_for_network_events(DevTools::TabDescription const&, OnNetworkRequestStarted, OnNetworkResponseHeadersReceived, OnNetworkResponseBodyReceived, OnNetworkRequestFinished) const override;
    virtual void stop_listening_for_network_events(DevTools::TabDescription const&) const override;
    virtual void listen_for_navigation_events(DevTools::TabDescription const&, OnNavigationStarted, OnNavigationFinished) const override;
    virtual void stop_listening_for_navigation_events(DevTools::TabDescription const&) const override;
    virtual void did_connect_devtools_client(DevTools::TabDescription const&) const override;
    virtual void did_disconnect_devtools_client(DevTools::TabDescription const&) const override;
    virtual void did_close_devtools_connection() override;

    static Application* s_the;

    Optional<Profile> m_profile;
    OwnPtr<Settings> m_settings;
    OwnPtr<BrowserProcess> m_browser_process;
    bool m_should_exit_after_profile_coordination { false };
    OwnPtr<ApplicationSettingsObserver> m_settings_observer;

    OwnPtr<BookmarkStore> m_bookmark_store;
    OwnPtr<ApplicationBookmarkStoreObserver> m_bookmark_store_observer;

    OwnPtr<AutocompleteService> m_autocomplete_service;

    Main::Arguments m_arguments;
    OwnPtr<Core::StatsLog> m_stats_log;
    BrowserOptions m_browser_options;
    Optional<Core::Process> m_cpu_profiler_process;
    OwnPtr<Core::File> m_cpu_profiler_control_socket;
    bool m_cpu_profiler_claimed { false };
    Vector<int> m_cpu_profiler_signal_handlers;
    Vector<int> m_session_signal_handlers;
    RequestServerOptions m_request_server_options;
    WebContentOptions m_web_content_options;
    RefPtr<FontService> m_font_service;
    OwnPtr<FontServiceHost> m_font_service_host;
    JsonValue m_site_compatibility_data;
    Optional<Core::AnonymousBuffer> m_content_blocker_list_buffer;
    RefPtr<Core::Timer> m_content_blocker_list_update_timer;
    struct PendingContentBlockerListUpdate {
        String identifier;
        String name;
        URL::URL url;
        ByteString path;
        ContentBlockerListUpdateTrigger trigger;
        u8 redirect_count { 0 };
    };
    Vector<ByteString> m_explicit_content_blocker_list_paths;
    ByteString m_content_blocker_lists_directory;
    Vector<PendingContentBlockerListUpdate> m_pending_content_blocker_list_updates;
    Optional<PendingContentBlockerListUpdate> m_active_content_blocker_list_update;
    ByteBuffer m_content_blocker_list_update_payload;
    bool m_content_blocker_list_update_response_ok { false };
    RefPtr<Requests::Request> m_content_blocker_list_update_request;
    bool m_content_blocker_list_update_cancelled { false };
    bool m_content_blocker_list_update_had_success { false };

    RefPtr<WebDriverBrowserConnection> m_webdriver_browser_connection;
    bool m_webdriver_browser_connection_failed { false };
    WebDriverSessionConfig m_webdriver_session_config;

    OwnPtr<RequestServerManager> m_request_server_manager;
    HashMap<u64, Function<void()>> m_image_decoder_exit_handlers;
    RefPtr<CompositorClient> m_compositor_client;
    RefPtr<CompositorPresentationClient> m_compositor_presentation_client;
    u64 m_compositor_presentation_id { 0 };
    bool m_reported_compositor_gpu_presentation_unavailable { false };
    // The last identity a Compositor reported, which outlives that Compositor until its replacement reports.
    Optional<Optional<Gfx::GpuDeviceIdentity>> m_compositor_gpu_device_identity;
    Vector<CompositorGpuDeviceIdentityCallback> m_callbacks_awaiting_compositor_gpu_device_identity;
    size_t m_compositor_restart_count { 0 };
    enum class CompositorRecoveryState {
        Idle,
        Queued,
        Recovering,
    };
    CompositorRecoveryState m_compositor_recovery_state { CompositorRecoveryState::Idle };

    RefPtr<WebContentClient> m_spare_web_content_process;
    // Every WebContent client, from its launch until its process exits. A page keeps no hold on its client.
    HashTable<NonnullRefPtr<WebContentClient>> m_web_content_clients;
    bool m_has_queued_task_to_launch_spare_web_content_process { false };
    u64 m_next_page_or_compositor_context_id { 1 };
    u64 m_next_cross_process_id_namespace { 1 };
    Web::HTML::CrossProcessIdAllocator m_ui_process_cross_process_id_allocator;

    OwnPtr<DownloadStore> m_download_store;
    RefPtr<BrowsingSession> m_default_session;
    WeakPtr<BrowsingSession> m_private_session;
    HashMap<int, WeakPtr<BrowsingSession>> m_request_server_client_sessions;

    // The RequestServer clients of the UI process itself, which may use the cookies of any site.
    HashTable<int> m_unrestricted_request_server_clients;

    // Whether a RequestServer client may use cookies in the given context for a request for url.
    bool request_server_client_may_use_cookies_in(int client_id, URL::URL const&, Optional<HTTP::Cookie::PartitionContext> const&) const;

    OwnPtr<Core::GeolocationProvider> m_geolocation_provider;
    OwnPtr<SpellChecker> m_spell_checker;
    OwnPtr<Core::TimeZoneWatcher> m_time_zone_watcher;

    Core::EventLoop* m_event_loop { nullptr };
#if defined(AK_OS_WINDOWS)
    OwnPtr<IPC::HandleBrokerServer> m_handle_broker_server;
#endif
    OwnPtr<ProcessManager> m_process_manager;

    RefPtr<Action> m_reload_action;
    RefPtr<Action> m_copy_selection_action;
    RefPtr<Action> m_undo_action;
    RefPtr<Action> m_redo_action;
    RefPtr<Action> m_cut_selection_action;
    RefPtr<Action> m_paste_action;
    RefPtr<Action> m_select_all_action;

    RefPtr<Action> m_open_about_page_action;
    RefPtr<Action> m_open_downloads_page_action;
    RefPtr<Action> m_open_settings_page_action;

    RefPtr<Menu> m_zoom_menu;
    RefPtr<Action> m_zoom_in_action;
    RefPtr<Action> m_zoom_out_action;
    RefPtr<Action> m_reset_zoom_action;

    RefPtr<Menu> m_color_scheme_menu;
    Web::CSS::PreferredColorScheme m_color_scheme { Web::CSS::PreferredColorScheme::Auto };

    RefPtr<Menu> m_contrast_menu;
    Web::CSS::PreferredContrast m_contrast { Web::CSS::PreferredContrast::Auto };

    RefPtr<Menu> m_motion_menu;
    Web::CSS::PreferredMotion m_motion { Web::CSS::PreferredMotion::Auto };

    RefPtr<Action> m_toggle_vertical_tabs_expanded_action;

    RefPtr<Action> m_toggle_menu_bar_action;

    RefPtr<Menu> m_bookmarks_menu;
    RefPtr<Action> m_toggle_bookmark_action;
    RefPtr<Action> m_toggle_bookmark_bar_action;
    RefPtr<Menu> m_show_bookmarks_bar_menu;
    RefPtr<Action> m_show_bookmarks_bar_always_action;
    RefPtr<Action> m_show_bookmarks_bar_never_action;
    RefPtr<Action> m_show_bookmarks_bar_on_hover_action;
    size_t m_bookmarks_menu_static_size { 0 };

    RefPtr<Menu> m_history_menu;

    RefPtr<Menu> m_inspect_menu;
    RefPtr<Action> m_view_source_action;
    RefPtr<Action> m_toggle_devtools_action;

    RefPtr<Action> m_report_website_issue_action;

    RefPtr<Menu> m_debug_menu;
    RefPtr<Action> m_show_line_box_borders_action;
    RefPtr<Action> m_show_caret_hit_test_debug_overlay_action;
    RefPtr<Action> m_enable_scripting_action;
    RefPtr<Action> m_block_pop_ups_action;
    StringView m_user_agent_string;
    StringView m_navigator_compatibility_mode;

    Web::Clipboard::SystemClipboardItem m_clipboard;

    FileDownloader m_file_downloader;

#if defined(AK_OS_MACOS)
    OwnPtr<IPC::MachBootstrapListener> m_mach_port_server;
    IPC::TransportBootstrapMachServer m_transport_bootstrap_server;
    Function<void(NonnullOwnPtr<IPC::Transport>)> m_on_browser_process_transport;
#endif

    OwnPtr<DevTools::DevToolsServer> m_devtools;
    OwnPtr<DevTools::FirefoxClient> m_devtools_client;

    mutable HashMap<u64, u64> m_navigation_listener_ids;
};

}

#define WEB_VIEW_APPLICATION(ApplicationType)                                                                                                \
public:                                                                                                                                      \
    template<typename... ApplicationArguments>                                                                                               \
    static ErrorOr<NonnullOwnPtr<ApplicationType>> create(Main::Arguments const& arguments, ApplicationArguments&&... application_arguments) \
    {                                                                                                                                        \
        auto app = adopt_own(*new ApplicationType { forward<ApplicationArguments>(application_arguments)... });                              \
        TRY(app->initialize(arguments));                                                                                                     \
        return app;                                                                                                                          \
    }                                                                                                                                        \
                                                                                                                                             \
    static ApplicationType& the()                                                                                                            \
    {                                                                                                                                        \
        return static_cast<ApplicationType&>(WebView::Application::the());                                                                   \
    }                                                                                                                                        \
                                                                                                                                             \
private:
