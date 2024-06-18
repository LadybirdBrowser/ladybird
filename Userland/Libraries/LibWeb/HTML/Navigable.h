/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/String.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Bindings/NavigationPrototype.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/TokenizedFeatures.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/XHR/FormDataEntry.h>

namespace Web::HTML {

enum class CSPNavigationType {
    Other,
    FormSubmission,
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#user-navigation-involvement
enum class UserNavigationInvolvement {
    BrowserUI,
    Activation,
    None,
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#target-snapshot-params
struct TargetSnapshotParams {
    SandboxingFlagSet sandboxing_flags {};
};

// https://html.spec.whatwg.org/multipage/document-sequences.html#navigable
class Navigable : public JS::Cell {
    JS_CELL(Navigable, JS::Cell);
    JS_DECLARE_ALLOCATOR(Navigable);

public:
    virtual ~Navigable() override;

    using NullWithError = StringView;
    using NavigationParamsVariant = Variant<Empty, NullWithError, JS::NonnullGCPtr<NavigationParams>, JS::NonnullGCPtr<NonFetchSchemeNavigationParams>>;

    ErrorOr<void> initialize_navigable(JS::NonnullGCPtr<DocumentState> document_state, JS::GCPtr<Navigable> parent);

    Vector<JS::Handle<Navigable>> child_navigables() const;

    bool is_traversable() const;

    String const& id() const { return m_id; }
    JS::GCPtr<Navigable> parent() const { return m_parent; }
    bool is_ancestor_of(JS::NonnullGCPtr<Navigable>) const;

    bool is_closing() const { return m_closing; }
    void set_closing(bool value) { m_closing = value; }

    void set_delaying_load_events(bool value);
    bool is_delaying_load_events() const { return m_delaying_the_load_event.has_value(); }

    JS::GCPtr<SessionHistoryEntry> active_session_history_entry() const { return m_active_session_history_entry; }
    void set_active_session_history_entry(JS::GCPtr<SessionHistoryEntry> entry) { m_active_session_history_entry = entry; }
    JS::GCPtr<SessionHistoryEntry> current_session_history_entry() const { return m_current_session_history_entry; }
    void set_current_session_history_entry(JS::GCPtr<SessionHistoryEntry> entry) { m_current_session_history_entry = entry; }

    Vector<JS::NonnullGCPtr<SessionHistoryEntry>>& get_session_history_entries() const;

    void activate_history_entry(JS::GCPtr<SessionHistoryEntry>);

    JS::GCPtr<DOM::Document> active_document();
    JS::GCPtr<BrowsingContext> active_browsing_context();
    JS::GCPtr<WindowProxy> active_window_proxy();
    JS::GCPtr<Window> active_window();

    JS::GCPtr<SessionHistoryEntry> get_the_target_history_entry(int target_step) const;

    String target_name() const;

    JS::GCPtr<NavigableContainer> container() const;
    JS::GCPtr<DOM::Document> container_document() const;

    JS::GCPtr<TraversableNavigable> traversable_navigable() const;
    JS::GCPtr<TraversableNavigable> top_level_traversable();

    virtual bool is_top_level_traversable() const { return false; }

    [[nodiscard]] bool is_focused() const;

    enum class WindowType {
        ExistingOrNone,
        NewAndUnrestricted,
        NewWithNoOpener,
    };

    struct ChosenNavigable {
        JS::GCPtr<Navigable> navigable;
        WindowType window_type;
    };

    ChosenNavigable choose_a_navigable(StringView name, TokenizedFeature::NoOpener no_opener, ActivateTab = ActivateTab::Yes, Optional<TokenizedFeature::Map const&> window_features = {});

    static JS::GCPtr<Navigable> navigable_with_active_document(JS::NonnullGCPtr<DOM::Document>);

    enum class Traversal {
        Tag
    };

    Variant<Empty, Traversal, String> ongoing_navigation() const { return m_ongoing_navigation; }
    void set_ongoing_navigation(Variant<Empty, Traversal, String> ongoing_navigation);

    WebIDL::ExceptionOr<void> populate_session_history_entry_document(
        JS::GCPtr<SessionHistoryEntry> entry,
        SourceSnapshotParams const& source_snapshot_params,
        TargetSnapshotParams const& target_snapshot_params,
        Optional<String> navigation_id = {},
        NavigationParamsVariant navigation_params = Empty {},
        CSPNavigationType csp_navigation_type = CSPNavigationType::Other,
        bool allow_POST = false,
        JS::SafeFunction<void()> completion_steps = [] {});

    struct NavigateParams {
        URL::URL const& url;
        JS::NonnullGCPtr<DOM::Document> source_document;
        Variant<Empty, String, POSTResource> document_resource = Empty {};
        JS::GCPtr<Fetch::Infrastructure::Response> response = nullptr;
        bool exceptions_enabled = false;
        Bindings::NavigationHistoryBehavior history_handling = Bindings::NavigationHistoryBehavior::Auto;
        Optional<SerializationRecord> navigation_api_state = {};
        Optional<Vector<XHR::FormDataEntry>&> form_data_entry_list = {};
        ReferrerPolicy::ReferrerPolicy referrer_policy = ReferrerPolicy::ReferrerPolicy::EmptyString;
        UserNavigationInvolvement user_involvement = UserNavigationInvolvement::None;
    };

    WebIDL::ExceptionOr<void> navigate(NavigateParams);

    WebIDL::ExceptionOr<void> navigate_to_a_fragment(URL::URL const&, HistoryHandlingBehavior, UserNavigationInvolvement, Optional<SerializationRecord> navigation_api_state, String navigation_id);

    WebIDL::ExceptionOr<JS::GCPtr<DOM::Document>> evaluate_javascript_url(URL::URL const&, Origin const& new_document_origin, String navigation_id);
    WebIDL::ExceptionOr<void> navigate_to_a_javascript_url(URL::URL const&, HistoryHandlingBehavior, Origin const& initiator_origin, CSPNavigationType csp_navigation_type, String navigation_id);

    bool allowed_by_sandboxing_to_navigate(Navigable const& target, SourceSnapshotParams const&);

    void reload();

    // https://github.com/whatwg/html/issues/9690
    [[nodiscard]] bool has_been_destroyed() const { return m_has_been_destroyed; }
    void set_has_been_destroyed() { m_has_been_destroyed = true; }

    CSSPixelPoint to_top_level_position(CSSPixelPoint);
    CSSPixelRect to_top_level_rect(CSSPixelRect const&);

    CSSPixelSize size() const { return m_size; }

    CSSPixelPoint viewport_scroll_offset() const { return m_viewport_scroll_offset; }
    CSSPixelRect viewport_rect() const { return { m_viewport_scroll_offset, m_size }; }
    void set_viewport_size(CSSPixelSize);
    void perform_scroll_of_viewport(CSSPixelPoint position);

    void set_needs_display();
    void set_needs_display(CSSPixelRect const&);

    void set_is_popup(TokenizedFeature::Popup is_popup) { m_is_popup = is_popup; }

    // https://html.spec.whatwg.org/#rendering-opportunity
    [[nodiscard]] bool has_a_rendering_opportunity() const;

    [[nodiscard]] TargetSnapshotParams snapshot_target_snapshot_params();

    [[nodiscard]] bool needs_repaint() const { return m_needs_repaint; }

    struct PaintConfig {
        bool paint_overlay { false };
        bool should_show_line_box_borders { false };
        bool has_focus { false };
    };
    void record_display_list(Painting::DisplayListRecorder& display_list_recorder, PaintConfig);

    Page& page() { return m_page; }
    Page const& page() const { return m_page; }

    String selected_text() const;
    void select_all();
    void paste(String const&);

    Web::EventHandler& event_handler() { return m_event_handler; }
    Web::EventHandler const& event_handler() const { return m_event_handler; }

    void did_edit(Badge<EditEventHandler>);

    JS::GCPtr<DOM::Position> cursor_position() const { return m_cursor_position; }
    void set_cursor_position(JS::NonnullGCPtr<DOM::Position>);
    bool increment_cursor_position_offset();
    bool decrement_cursor_position_offset();

    bool cursor_blink_state() const { return m_cursor_blink_state; }

protected:
    explicit Navigable(JS::NonnullGCPtr<Page>);

    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#ongoing-navigation
    Variant<Empty, Traversal, String> m_ongoing_navigation;

    // https://html.spec.whatwg.org/multipage/browsers.html#is-popup
    TokenizedFeature::Popup m_is_popup { TokenizedFeature::Popup::No };

private:
    void reset_cursor_blink_cycle();

    void scroll_offset_did_change();

    void inform_the_navigation_api_about_aborting_navigation();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-id
    String m_id;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-parent
    JS::GCPtr<Navigable> m_parent;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-current-history-entry
    JS::GCPtr<SessionHistoryEntry> m_current_session_history_entry;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-active-history-entry
    JS::GCPtr<SessionHistoryEntry> m_active_session_history_entry;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-closing
    bool m_closing { false };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#delaying-load-events-mode
    Optional<DOM::DocumentLoadEventDelayer> m_delaying_the_load_event;

    // Implied link between navigable and its container.
    JS::GCPtr<NavigableContainer> m_container;

    JS::NonnullGCPtr<Page> m_page;

    bool m_has_been_destroyed { false };

    CSSPixelSize m_size;
    CSSPixelPoint m_viewport_scroll_offset;

    bool m_needs_repaint { false };

    Web::EventHandler m_event_handler;

    JS::GCPtr<DOM::Position> m_cursor_position;
    RefPtr<Core::Timer> m_cursor_blink_timer;
    bool m_cursor_blink_state { false };
};

HashTable<Navigable*>& all_navigables();

bool navigation_must_be_a_replace(URL::URL const& url, DOM::Document const& document);
void finalize_a_cross_document_navigation(JS::NonnullGCPtr<Navigable>, HistoryHandlingBehavior, JS::NonnullGCPtr<SessionHistoryEntry>);
void perform_url_and_history_update_steps(DOM::Document& document, URL::URL new_url, Optional<SerializationRecord> = {}, HistoryHandlingBehavior history_handling = HistoryHandlingBehavior::Replace);
UserNavigationInvolvement user_navigation_involvement(DOM::Event const&);

}
