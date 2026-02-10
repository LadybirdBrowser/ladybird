/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/String.h>
#include <AK/Tuple.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Bindings/NavigationPrototype.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/InitialInsertion.h>
#include <LibWeb/HTML/NavigationObserver.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/RenderingThread.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/HTML/TokenizedFeatures.h>
#include <LibWeb/HTML/WindowType.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Painting/BackingStoreManager.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/XHR/FormDataEntry.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#target-snapshot-params
struct TargetSnapshotParams {
    SandboxingFlagSet sandboxing_flags {};
};

// https://html.spec.whatwg.org/multipage/document-sequences.html#navigable
class WEB_API Navigable : public JS::Cell {
    GC_CELL(Navigable, JS::Cell);
    GC_DECLARE_ALLOCATOR(Navigable);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~Navigable() override;

    using NullOrError = Optional<String>;
    using NavigationParamsVariant = Variant<NullOrError, GC::Ref<NavigationParams>, GC::Ref<NonFetchSchemeNavigationParams>>;

    ErrorOr<void> initialize_navigable(GC::Ref<DocumentState> document_state, GC::Ptr<Navigable> parent);

    void register_navigation_observer(Badge<NavigationObserver>, NavigationObserver&);
    void unregister_navigation_observer(Badge<NavigationObserver>, NavigationObserver&);

    Vector<GC::Root<Navigable>> child_navigables() const;

    virtual bool is_traversable() const { return false; }

    String const& id() const { return m_id; }
    GC::Ptr<Navigable> parent() const { return m_parent; }
    bool is_ancestor_of(GC::Ref<Navigable>) const;

    bool is_closing() const { return m_closing; }
    void set_closing(bool value) { m_closing = value; }
    bool is_script_closable();

    void stop_loading();

    void set_delaying_load_events(bool value);
    bool is_delaying_load_events() const { return m_delaying_the_load_event.has_value(); }

    GC::Ptr<SessionHistoryEntry> active_session_history_entry() const { return m_active_session_history_entry; }
    void set_active_session_history_entry(GC::Ptr<SessionHistoryEntry> entry) { m_active_session_history_entry = entry; }
    GC::Ptr<SessionHistoryEntry> current_session_history_entry() const { return m_current_session_history_entry; }
    void set_current_session_history_entry(GC::Ptr<SessionHistoryEntry> entry) { m_current_session_history_entry = entry; }

    Vector<GC::Ref<SessionHistoryEntry>>& get_session_history_entries() const;

    void activate_history_entry(GC::Ptr<SessionHistoryEntry>);

    GC::Ptr<DOM::Document> active_document() const;
    GC::Ptr<BrowsingContext> active_browsing_context();
    GC::Ptr<WindowProxy> active_window_proxy();
    GC::Ptr<Window> active_window();

    GC::Ptr<SessionHistoryEntry> get_the_target_history_entry(int target_step) const;

    String target_name() const;

    GC::Ptr<NavigableContainer> container() const;
    GC::Ptr<DOM::Document> container_document() const;

    GC::Ptr<TraversableNavigable> traversable_navigable() const;
    GC::Ptr<TraversableNavigable> top_level_traversable();

    virtual bool is_top_level_traversable() const { return false; }

    [[nodiscard]] bool is_focused() const;

    struct ChosenNavigable {
        GC::Ptr<Navigable> navigable;
        WindowType window_type;
    };

    ChosenNavigable choose_a_navigable(StringView name, TokenizedFeature::NoOpener no_opener, ActivateTab = ActivateTab::Yes, Optional<TokenizedFeature::Map const&> window_features = {});

    GC::Ptr<Navigable> find_a_navigable_by_target_name(StringView name);

    static GC::Ptr<Navigable> navigable_with_active_document(GC::Ref<DOM::Document>);

    enum class Traversal {
        Tag
    };

    Variant<Empty, Traversal, String> ongoing_navigation() const { return m_ongoing_navigation; }
    void set_ongoing_navigation(Variant<Empty, Traversal, String> ongoing_navigation);

    void populate_session_history_entry_document(
        GC::Ptr<SessionHistoryEntry> entry,
        SourceSnapshotParams const& source_snapshot_params,
        TargetSnapshotParams const& target_snapshot_params,
        UserNavigationInvolvement user_involvement,
        NonnullRefPtr<Core::Promise<Empty>> signal_to_continue_session_history_processing,
        Optional<String> navigation_id = {},
        NavigationParamsVariant navigation_params = Navigable::NullOrError {},
        ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type = ContentSecurityPolicy::Directives::Directive::NavigationType::Other,
        bool allow_POST = false,
        GC::Ptr<GC::Function<void()>> completion_steps = {});

    struct NavigateParams {
        URL::URL url;
        // FIXME: source_document should now be nullable, and default to nullptr.
        GC::Ref<DOM::Document> source_document;
        Variant<Empty, String, POSTResource> document_resource = Empty {};
        GC::Ptr<Fetch::Infrastructure::Response> response = nullptr;
        bool exceptions_enabled = false;
        Bindings::NavigationHistoryBehavior history_handling = Bindings::NavigationHistoryBehavior::Auto;
        Optional<SerializationRecord> navigation_api_state = {};
        Optional<Vector<XHR::FormDataEntry>> form_data_entry_list = {};
        ReferrerPolicy::ReferrerPolicy referrer_policy = ReferrerPolicy::ReferrerPolicy::EmptyString;
        UserNavigationInvolvement user_involvement = UserNavigationInvolvement::None;
        GC::Ptr<DOM::Element> source_element = nullptr;
        InitialInsertion initial_insertion = InitialInsertion::No;

        void visit_edges(Cell::Visitor& visitor);
    };

    WebIDL::ExceptionOr<void> navigate(NavigateParams);

    GC::Ptr<DOM::Document> evaluate_javascript_url(URL::URL const&, URL::Origin const& new_document_origin, UserNavigationInvolvement, String navigation_id);

    bool allowed_by_sandboxing_to_navigate(Navigable const& target, SourceSnapshotParams const&);

    void reload(Optional<SerializationRecord> navigation_api_state = {}, UserNavigationInvolvement = UserNavigationInvolvement::None);

    // https://github.com/whatwg/html/issues/9690
    [[nodiscard]] bool has_been_destroyed() const { return m_has_been_destroyed; }
    void set_has_been_destroyed() { m_has_been_destroyed = true; }

    CSSPixelPoint to_top_level_position(CSSPixelPoint);
    CSSPixelRect to_top_level_rect(CSSPixelRect const&);

    CSSPixelPoint viewport_scroll_offset() const { return m_viewport_scroll_offset; }
    CSSPixelRect viewport_rect() const { return { m_viewport_scroll_offset, m_viewport_size }; }
    CSSPixelSize viewport_size() const { return m_viewport_size; }
    void set_viewport_size(CSSPixelSize);
    void perform_scroll_of_viewport_scrolling_box(CSSPixelPoint position);

    Painting::BackingStoreManager& backing_store_manager() { return *m_backing_store_manager; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#rendering-opportunity
    [[nodiscard]] bool has_a_rendering_opportunity() const;

    [[nodiscard]] TargetSnapshotParams snapshot_target_snapshot_params();

    Page& page() { return m_page; }
    Page const& page() const { return m_page; }

    String selected_text() const;
    void select_all();
    void paste(Utf16String const&);

    Web::EventHandler& event_handler() { return m_event_handler; }
    Web::EventHandler const& event_handler() const { return m_event_handler; }

    // https://drafts.csswg.org/css-view-transitions-1/#snapshot-containing-block
    CSSPixelRect snapshot_containing_block();
    // https://drafts.csswg.org/css-view-transitions-1/#snapshot-containing-block-size
    CSSPixelSize snapshot_containing_block_size();

    bool has_session_history_entry_and_ready_for_navigation() const { return m_has_session_history_entry_and_ready_for_navigation; }
    void set_has_session_history_entry_and_ready_for_navigation();

    void inform_the_navigation_api_about_child_navigable_destruction();

    bool has_pending_navigations() const { return !m_pending_navigations.is_empty(); }

    void ready_to_paint();
    void record_display_list_and_scroll_state(PaintConfig);
    void paint_next_frame();
    void render_screenshot(Gfx::PaintingSurface&, PaintConfig, Function<void()>&& callback);

    bool needs_repaint() const { return m_needs_repaint; }
    void set_needs_repaint() { m_needs_repaint = true; }

    [[nodiscard]] bool has_inclusive_ancestor_with_visibility_hidden() const;

    RefPtr<Gfx::SkiaBackendContext> skia_backend_context() const;

    RenderingThread& rendering_thread() { return m_rendering_thread; }

    void set_pending_set_browser_zoom_request(bool value) { m_pending_set_browser_zoom_request = value; }
    bool pending_set_browser_zoom_request() const { return m_pending_set_browser_zoom_request; }

    void set_should_show_line_box_borders(bool value) { m_should_show_line_box_borders = value; }

    bool is_svg_page() const { return m_is_svg_page; }

    template<typename T>
    bool fast_is() const = delete;

    GC::Ref<WebIDL::Promise> scroll_viewport_by_delta(CSSPixelPoint delta);
    GC::Ref<WebIDL::Promise> perform_a_scroll_of_the_viewport(CSSPixelPoint position);
    void reset_zoom();

protected:
    explicit Navigable(GC::Ref<Page>, bool is_svg_page);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#ongoing-navigation
    Variant<Empty, Traversal, String> m_ongoing_navigation;

private:
    void begin_navigation(NavigateParams);
    void navigate_to_a_fragment(URL::URL const&, HistoryHandlingBehavior, UserNavigationInvolvement, GC::Ptr<DOM::Element> source_element, Optional<SerializationRecord> navigation_api_state, String navigation_id);
    void navigate_to_a_javascript_url(URL::URL const&, HistoryHandlingBehavior, GC::Ref<SourceSnapshotParams>, URL::Origin const& initiator_origin, UserNavigationInvolvement, ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type, InitialInsertion, String navigation_id);

    void reset_cursor_blink_cycle();

    void scroll_offset_did_change();

    void inform_the_navigation_api_about_aborting_navigation();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-id
    String m_id;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-parent
    GC::Ptr<Navigable> m_parent;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-current-history-entry
    GC::Ptr<SessionHistoryEntry> m_current_session_history_entry;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-active-history-entry
    GC::Ptr<SessionHistoryEntry> m_active_session_history_entry;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-closing
    bool m_closing { false };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#delaying-load-events-mode
    Optional<DOM::DocumentLoadEventDelayer> m_delaying_the_load_event;

    // Implied link between navigable and its container.
    GC::Ptr<NavigableContainer> m_container;

    GC::Ref<Page> m_page;

    NavigationObserver::NavigationObserversList m_navigation_observers;

    bool m_has_been_destroyed { false };

    CSSPixelSize m_viewport_size;
    CSSPixelPoint m_viewport_scroll_offset;

    Web::EventHandler m_event_handler;

    bool m_has_session_history_entry_and_ready_for_navigation { false };

    Vector<NavigateParams> m_pending_navigations;

    bool m_is_svg_page { false };
    bool m_needs_repaint { true };
    bool m_pending_set_browser_zoom_request { false };
    bool m_should_show_line_box_borders { false };
    GC::Ref<Painting::BackingStoreManager> m_backing_store_manager;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    RenderingThread m_rendering_thread;
};

WEB_API HashTable<GC::RawRef<Navigable>>& all_navigables();

bool navigation_must_be_a_replace(URL::URL const& url, DOM::Document const& document);
void finalize_a_cross_document_navigation(GC::Ref<Navigable>, HistoryHandlingBehavior, UserNavigationInvolvement, GC::Ref<SessionHistoryEntry>);
void perform_url_and_history_update_steps(DOM::Document& document, URL::URL new_url, Optional<SerializationRecord> = {}, HistoryHandlingBehavior history_handling = HistoryHandlingBehavior::Replace);

}
