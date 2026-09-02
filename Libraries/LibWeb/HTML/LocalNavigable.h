/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/HashTable.h>
#include <AK/OwnPtr.h>
#include <AK/Tuple.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibCore/Forward.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/Bindings/Window.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/Compositor/SmoothScrollAnimation.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/InitialInsertion.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigationObserver.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/HTML/TargetSnapshotParams.h>
#include <LibWeb/HTML/TokenizedFeatures.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/HTML/WindowType.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/XHR/FormDataEntry.h>

namespace Web::HTML {

struct PopulateSessionHistoryEntryDocumentOutput;

// https://html.spec.whatwg.org/multipage/document-sequences.html#navigable
class WEB_API LocalNavigable : public Navigable {
    GC_CELL(LocalNavigable, Navigable);
    GC_DECLARE_ALLOCATOR(LocalNavigable);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~LocalNavigable() override;

    using NullOrError = NavigationParamsNullOrError;
    using NavigationParamsVariant = HTML::NavigationParamsVariant;

    void initialize_navigable(NonnullRefPtr<DocumentState> document_state, GC::Ptr<LocalNavigable> parent, GC::Ref<DOM::Document> document, VisibilityState system_visibility_state);
    void set_id_for_session_history_reconstruction(CrossProcessId id) { set_id(id); }

    void register_navigation_observer(Badge<NavigationObserver>, NavigationObserver&);
    void unregister_navigation_observer(Badge<NavigationObserver>, NavigationObserver&);

    Vector<GC::Root<LocalNavigable>> child_navigables() const;

    virtual bool is_traversable() const { return false; }

    bool is_closing() const { return m_closing; }
    void set_closing(bool value) { m_closing = value; }
    bool is_script_closable();

    void stop_loading();

    void set_delaying_load_events(bool value);
    bool is_delaying_load_events() const { return m_delaying_the_load_event.has_value(); }

    void set_navigation_load_event_guard(DOM::Document& parent_doc);
    void clear_navigation_load_event_guard();

    RefPtr<SessionHistoryEntry> active_session_history_entry() const;
    void set_active_session_history_entry(RefPtr<SessionHistoryEntry>);
    RefPtr<SessionHistoryEntry> current_session_history_entry() const;
    void set_current_session_history_entry(RefPtr<SessionHistoryEntry>);

    void set_child_navigable_history_reconstruction_ids(Vector<Optional<CrossProcessId>> ids)
    {
        m_child_navigable_history_reconstruction_ids = move(ids);
    }
    Optional<CrossProcessId> child_navigable_history_reconstruction_id(size_t index) const;
    void consume_child_navigable_history_reconstruction_id(size_t index);

    void activate_history_entry(RefPtr<SessionHistoryEntry>, GC::Ref<DOM::Document>, VisibilityState system_visibility_state);
    void notify_navigation_observers_navigation_complete();

    GC::Ptr<DOM::Document> active_document() const;
    Optional<UniqueNodeID> active_document_id() const;
    void set_active_document(GC::Ptr<DOM::Document>);
    GC::Ptr<BrowsingContext> active_browsing_context();
    virtual GC::Ptr<WindowProxy> active_window_proxy() override;
    GC::Ptr<Window> active_window();

    virtual Optional<URL::URL> active_document_url() const override;
    virtual Optional<URL::Origin> active_document_origin() const override;
    ReplicatedNavigableState replicated_state() const;

    void save_persisted_state_to_active_session_history_entry();
    void restore_persisted_state_from_session_history_entry(SessionHistoryEntry const&);
    void schedule_persisted_state_restoration_retry(SessionHistoryEntry const&);
    void restore_pending_persisted_state_for_completed_document(GC::Ref<DOM::Document>);
    void restore_scroll_position_data(SessionHistoryEntry const&);

    virtual Utf16String const& target_name() const override;

    GC::Ptr<NavigableContainer> container() const;
    void set_container(Badge<NavigableContainer>, GC::Ptr<NavigableContainer> container) { m_container = container; }
    GC::Ptr<DOM::Document> container_document() const;

    GC::Ptr<LocalTraversableNavigable> traversable_navigable() const;

    virtual bool is_top_level_traversable() const { return false; }

    [[nodiscard]] bool is_focused() const;

    struct ChosenNavigable {
        GC::Ptr<LocalNavigable> navigable;
        WindowType window_type;
    };

    ChosenNavigable choose_a_navigable(Utf16View name, TokenizedFeature::NoOpener no_opener, ActivateTab = ActivateTab::Yes, Optional<TokenizedFeature::Map const&> window_features = {});

    GC::Ptr<LocalNavigable> find_a_navigable_by_target_name(Utf16View name);

    void handle_as_a_download(GC::Ref<Fetch::Infrastructure::Response>, URL::URL const& fallback_url, GC::Ptr<Fetch::Infrastructure::FetchController>, Optional<ByteString> proposed_filename, Optional<URL::Origin> interface_origin);

    void inform_the_navigation_api_about_aborting_navigation();

    enum class Traversal {
        Tag
    };

    enum class NavigationAPIAbortBehavior {
        Abort,
        Preserve
    };

    Variant<Empty, Traversal, Utf16String> ongoing_navigation() const { return m_ongoing_navigation; }
    void set_ongoing_navigation(Variant<Empty, Traversal, Utf16String> ongoing_navigation, NavigationAPIAbortBehavior = NavigationAPIAbortBehavior::Abort);

    bool resume_navigation_params_creation(Utf16String const& navigation_id, Optional<NavigationPopulationRequest>);
    void run_navigation_unload_check(Utf16String const& navigation_id, GC::Ref<GC::Function<void(bool)>> completion_steps);
    void request_population_for_reconstructed_history_entry(NavigationPopulationRequest);

    void populate_session_history_entry_document(
        URL::URL url,
        DocumentResource document_resource,
        Fetch::Infrastructure::Request::ReferrerType request_referrer,
        ReferrerPolicy::ReferrerPolicy request_referrer_policy,
        Optional<URL::Origin> initiator_origin,
        Optional<URL::Origin> origin,
        Variant<SerializedPolicyContainer, DocumentState::Client> history_policy_container,
        Optional<URL::URL> about_base_url,
        Utf16String navigable_target_name,
        bool reload_pending,
        bool ever_populated,
        GC::Ref<SourceSnapshotParams> source_snapshot_params,
        TargetSnapshotParams const& target_snapshot_params,
        UserNavigationInvolvement user_involvement,
        Optional<Utf16String> navigation_id,
        NavigationParamsVariant navigation_params,
        ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type,
        bool allow_POST,
        GC::Ptr<GC::Function<void(GC::Ptr<PopulateSessionHistoryEntryDocumentOutput>)>> completion_steps);

    void queue_navigation_and_traversal_task_for_session_history_entry_population(
        URL::URL url,
        bool source_allows_downloading,
        Optional<URL::Origin> source_interface_origin,
        UserNavigationInvolvement user_involvement,
        Optional<Utf16String> navigation_id,
        NavigationParamsVariant navigation_params,
        ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type,
        Bindings::NavigationTimingType navigation_timing_type,
        GC::Ref<PopulateSessionHistoryEntryDocumentOutput> output,
        GC::Ptr<GC::Function<void(GC::Ptr<PopulateSessionHistoryEntryDocumentOutput>)>> completion_steps);

    void create_navigation_params_for_navigation(NavigationPopulationRequest, GC::Ref<SourceSnapshotParams>, NavigationParamsVariant, Bindings::NavigationTimingType);

    struct NavigateParams {
        URL::URL url;
        GC::Ptr<DOM::Document> source_document = nullptr;
        DocumentResource document_resource = Empty {};
        GC::Ptr<Fetch::Infrastructure::Response> response = nullptr;
        bool exceptions_enabled = false;
        Bindings::NavigationHistoryBehavior history_handling = Bindings::NavigationHistoryBehavior::Auto;
        Optional<StorageSerializationRecord> navigation_api_state = {};
        Optional<Vector<XHR::FormDataEntry>> form_data_entry_list = {};
        ReferrerPolicy::ReferrerPolicy referrer_policy = ReferrerPolicy::ReferrerPolicy::EmptyString;
        UserNavigationInvolvement user_involvement = UserNavigationInvolvement::None;
        // NB: A load requested by the UI process carries the ID the UI generated when it recorded the
        //     navigation; otherwise step 7 of the navigate algorithm generates one.
        Optional<Utf16String> navigation_id = {};
        GC::Ptr<DOM::Element> source_element = nullptr;
        InitialInsertion initial_insertion = InitialInsertion::No;

        void visit_edges(Cell::Visitor& visitor);
    };

    WebIDL::ExceptionOr<void> navigate(NavigateParams);

    GC::Ptr<DOM::Document> evaluate_javascript_url(URL::URL const&, URL::Origin const& new_document_origin, UserNavigationInvolvement, Utf16String navigation_id);

    bool allowed_by_sandboxing_to_navigate(LocalNavigable const& target, SourceSnapshotParams const&);

    void reload(Optional<StorageSerializationRecord> navigation_api_state = {}, UserNavigationInvolvement = UserNavigationInvolvement::None);

    // https://github.com/whatwg/html/issues/9690
    [[nodiscard]] bool has_been_destroyed() const { return m_has_been_destroyed; }
    void set_has_been_destroyed();
    void remove_from_all_local_navigables();

    CSSPixelPoint to_top_level_position(CSSPixelPoint);
    CSSPixelRect to_top_level_rect(CSSPixelRect const&);

    CSSPixelPoint viewport_scroll_offset() const { return m_viewport_scroll_offset; }
    CSSPixelRect viewport_rect() const { return { m_viewport_scroll_offset, m_viewport_size }; }
    CSSPixelSize viewport_size() const { return m_viewport_size; }
    void set_viewport_size(CSSPixelSize, InvalidateDisplayList = InvalidateDisplayList::No);
    void perform_scroll_of_viewport_scrolling_box(CSSPixelPoint position);
    void adopt_pending_async_scroll_offsets();
    void process_main_thread_smooth_scrolls();
    void wait_for_async_scroll_operation(Compositor::AsyncScrollOperationID, GC::Ref<WebIDL::Promise>);
    void clamp_viewport_scroll_offset();

    // https://html.spec.whatwg.org/multipage/webappapis.html#rendering-opportunity
    [[nodiscard]] bool has_a_rendering_opportunity() const;

    Page& page() { return m_page; }
    Page const& page() const { return m_page; }

    Utf16String selected_text() const;
    Utf16String selected_html_for_clipboard() const;
    Utf16String cut_selected_text() const;
    void select_all();
    void paste(Utf16View);
    void paste_from_clipboard();
    void undo();
    void redo();
    void set_marked_text_from_input_method(Utf16View text);
    void commit_text_from_input_method(Utf16View text, i32 replacement_start = 0, i32 replacement_length = 0);
    void unmark_text_from_input_method();

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
    void clear_pending_navigations();
    void prepare_to_populate_reconstructed_history_entry(Utf16String navigation_api_key);

    bool record_display_list_and_scroll_state(PaintConfig);
    void paint_next_frame();
    void render_screenshot(Gfx::PaintingSurface&, PaintConfig, Function<void()>&& callback);
    Painting::DisplayListResourceStorage& display_list_resource_storage() { return m_display_list_resource_storage; }
    Painting::DisplayListResourceStorage const& display_list_resource_storage() const { return m_display_list_resource_storage; }

    bool needs_repaint() const { return m_needs_repaint; }
    void set_needs_repaint() { m_needs_repaint = true; }
    bool needs_to_record_display_list() const { return m_needs_to_record_display_list; }
    void set_needs_to_record_display_list() { m_needs_to_record_display_list = true; }
    void repaint_after_compositor_process_reconnect();

    [[nodiscard]] bool has_inclusive_ancestor_with_visibility_hidden() const;

    Compositor::CompositorContextHandle& compositor_context()
    {
        VERIFY(m_compositor_context);
        return *m_compositor_context;
    }
    Compositor::CompositorContextHandle const& compositor_context() const
    {
        VERIFY(m_compositor_context);
        return *m_compositor_context;
    }
    bool has_compositor_context() const { return m_compositor_context; }

    void set_pending_set_browser_zoom_request(bool value) { m_pending_set_browser_zoom_request = value; }
    bool pending_set_browser_zoom_request() const { return m_pending_set_browser_zoom_request; }

    void set_should_show_line_box_borders(bool);
    void set_should_show_caret_hit_test_debug_overlay(bool);
    bool should_show_caret_hit_test_debug_overlay() const { return m_should_show_caret_hit_test_debug_overlay; }

    bool is_svg_page() const { return m_is_svg_page; }

    template<typename T>
    bool fast_is() const = delete;

    enum class ScrollTrigger {
        Programmatic,
        UserInput,
    };

    // Whether the snap position a gesture ends at is selected as each of its scrolls runs, or once from the offsets
    // the whole gesture traveled between.
    enum class SnapPositionSelection {
        AtGestureEnd,
        PerScroll,
    };

    // How long the offset a gesture's input deltas have reached goes on being the offset its next step travels from.
    // A wheel gesture stays latched between its steps, so its deltas keep accumulating until it settles, and crossing a
    // snap position costs the distance between them however slowly the steps arrive. Keys are separate commands rather
    // than one gesture, so each press travels from the scrolling box itself.
    enum class SnapStepAccumulation {
        UntilScrollFinishes,
        UntilGestureSettles,
    };

    enum class SmoothScrollAbortCause {
        ReplacedByNewScroll,
        TakenOverByUserInput,
    };

    enum class AsyncScrollCompletion {
        Finished,
        TakenOverByUserInput,
    };

    GC::Ref<WebIDL::Promise> scroll_viewport_by_delta(CSSPixelPoint delta, Bindings::ScrollBehavior = Bindings::ScrollBehavior::Instant);
    GC::Ref<WebIDL::Promise> perform_a_scroll_of_the_viewport(CSSPixelPoint position, Bindings::ScrollBehavior = Bindings::ScrollBehavior::Auto, ScrollTrigger = ScrollTrigger::Programmatic, Optional<CSSPixelPoint> relative_displacement = {});
    GC::Ref<WebIDL::Promise> perform_a_scroll_of_an_element(DOM::Element&, CSSPixelPoint position, Bindings::ScrollBehavior, Optional<CSSPixelPoint> relative_displacement = {});
    bool perform_a_snapped_relative_user_scroll(Layout::Node&, CSSPixelPoint delta, Painting::SnapSelectionStrategy::Type, SnapStepAccumulation, Compositor::ScrollAnimationKind = Compositor::ScrollAnimationKind::SmoothScroll);
    bool perform_a_snapped_momentum_scroll(Layout::Node&, CSSPixelPoint momentum_delta);
    void re_snap_scroll_containers_after_layout_change();
    void abort_in_flight_smooth_scrolls(Compositor::AsyncScrollNodeStableID, SmoothScrollAbortCause);
    void abort_in_flight_smooth_scrolls_taken_over_by_user_input(Compositor::AsyncScrollNodeStableID, CSSPixelPoint scroll_offset_at_gesture_start);
    void queue_scrollend_event_after_user_scroll(GC::Ref<DOM::EventTarget>, Optional<Compositor::AsyncScrollNodeStableID>, Optional<CSSPixelPoint> scroll_offset_before_scroll = {}, SnapPositionSelection = SnapPositionSelection::AtGestureEnd);
    void note_user_scroll_input_intent(Painting::SnapSelectionStrategy::Type);
    void note_user_scroll_gesture_phase(ScrollGesturePhase);
    void defer_user_scroll_settlement();
    void snap_user_scroll_gestures_that_awaited_layout();
    void begin_user_scroll_gesture_hold(Badge<UserScrollGestureHold>);
    void end_user_scroll_gesture_hold(Badge<UserScrollGestureHold>);
    void reset_zoom();

protected:
    explicit LocalNavigable(
        GC::Ref<Page>,
        bool is_svg_page,
        Compositor::PagePresentationRegistration = Compositor::PagePresentationRegistration::No);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#ongoing-navigation
    Variant<Empty, Traversal, Utf16String> m_ongoing_navigation;

private:
    enum class PendingNavigationBehavior {
        Append,
        Replace
    };

    // Values produced by steps 1-6 of the navigate algorithm. Keep these when navigation is parked so resumption
    // continues at step 7 instead of snapshotting a different document state.
    struct PreparedNavigation {
        NavigateParams params;
        ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type;
        GC::Ref<SourceSnapshotParams> source_snapshot_params;
        URL::Origin initiator_origin_snapshot;
        URL::URL initiator_base_url_snapshot;

        void visit_edges(Cell::Visitor& visitor)
        {
            params.visit_edges(visitor);
            visitor.visit(source_snapshot_params);
        }
    };

    struct PendingNavigation {
        Optional<PreparedNavigation> navigation;
        Optional<Utf16String> population_navigation_id;
        GC::Ptr<GC::Function<void(Optional<PreparedNavigation>, Optional<NavigationPopulationRequest>)>> continue_steps;
    };

    void begin_navigation(PreparedNavigation);
    void continue_navigation_after_population_dispatch(PreparedNavigation, NavigationPopulationRequest);
    void queue_pending_navigation(PreparedNavigation, PendingNavigationBehavior);
    void park_navigation_for_population(Utf16String navigation_id, Optional<PreparedNavigation>, GC::Ref<GC::Function<void(Optional<PreparedNavigation>, Optional<NavigationPopulationRequest>)>> continue_steps);
    Optional<PendingNavigation> take_navigation_parked_for_population(Utf16String const& navigation_id);
    void process_pending_navigations();
    void navigate_to_a_fragment(URL::URL const&, HistoryHandlingBehavior, UserNavigationInvolvement, GC::Ptr<DOM::Element> source_element, Optional<StorageSerializationRecord> navigation_api_state, Utf16String navigation_id);
    void navigate_to_a_javascript_url(URL::URL const&, HistoryHandlingBehavior, GC::Ref<SourceSnapshotParams>, URL::Origin const& initiator_origin, UserNavigationInvolvement, ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type, InitialInsertion, Utf16String navigation_id);

    void reset_cursor_blink_cycle();

    void scroll_offset_did_change();
    void clear_parent_compositor_context();
    void destroy_compositor_context();

    void start_download_for_response(GC::Ref<Fetch::Infrastructure::Response>, URL::URL const& download_url, ByteString suggested_filename, GC::Ptr<Fetch::Infrastructure::FetchController>);

    // A request toward the position a scroll is already headed for joins that scroll rather than restarting it, so one
    // scroll can owe more than one promise.
    using ScrollPromises = Vector<GC::Ref<WebIDL::Promise>, 1>;

    void resolve_async_scroll_operation(Compositor::AsyncScrollOperationID, AsyncScrollCompletion = AsyncScrollCompletion::Finished);
    void resolve_all_pending_async_scroll_operations();
    void resolve_pending_smooth_scrolls(Compositor::AsyncScrollNodeStableID, SmoothScrollAbortCause);
    // Whether a programmatic scroll still needs a snap position selected for its destination, or was given a
    // destination that snap position selection already produced.
    enum class DestinationSnapping {
        SelectSnapPosition,
        DestinationIsSnapPosition,
    };
    GC::Ref<WebIDL::Promise> perform_a_scroll_of_a_scrolling_box(Compositor::AsyncScrollNodeStableID, CSSPixelPoint position, Bindings::ScrollBehavior, GC::Ptr<DOM::Element> associated_element, ScrollTrigger, Optional<CSSPixelPoint> relative_displacement = {}, DestinationSnapping = DestinationSnapping::SelectSnapPosition, Compositor::ScrollAnimationKind = Compositor::ScrollAnimationKind::SmoothScroll);
    Optional<CSSPixelPoint> scroll_offset_for(Compositor::AsyncScrollNodeStableID) const;
    bool set_scroll_offset_for(Compositor::AsyncScrollNodeStableID, CSSPixelPoint);
    void queue_scrollend_event(Compositor::AsyncScrollNodeStableID, ScrollTrigger, Optional<CSSPixelPoint> scroll_offset_before_scroll = {});
    void queue_scrollend_event(DOM::Document&, GC::Ref<DOM::EventTarget>, Optional<Compositor::AsyncScrollNodeStableID>, ScrollTrigger, Optional<CSSPixelPoint> scroll_offset_before_scroll = {});
    void queue_scrollend_event_for_finished_scroll(Compositor::AsyncScrollNodeStableID, ScrollTrigger, Optional<CSSPixelPoint> scroll_offset_before_scroll);
    void queue_scrollend_event_and_promise_resolution_for_finished_scroll(Optional<Compositor::AsyncScrollNodeStableID>, ScrollTrigger, Optional<CSSPixelPoint> scroll_offset_before_scroll, ScrollPromises const&);
    ScrollPromises* promises_of_smooth_scroll_in_flight_toward(Compositor::AsyncScrollNodeStableID, CSSPixelPoint position, ScrollTrigger);
    // The scroll a new input to a scrolling box would interact with; a scroll driven by user input is reported over
    // any programmatic scroll also in flight.
    struct InFlightScroll {
        ScrollTrigger trigger { ScrollTrigger::Programmatic };
        Optional<CSSPixelPoint> destination_scroll_offset;
    };
    Optional<InFlightScroll> in_flight_scroll_for(Optional<Compositor::AsyncScrollNodeStableID> const&) const;
    struct PendingUserScrollendTarget {
        GC::Ref<DOM::EventTarget> target;
        Optional<Compositor::AsyncScrollNodeStableID> stable_node_id;
        Optional<CSSPixelPoint> scroll_offset_at_gesture_start;
        Optional<CSSPixelPoint> unsnapped_scroll_destination;
        Painting::SnapSelectionStrategy::Type intent { Painting::SnapSelectionStrategy::Type::EndPosition };
        bool travels_under_momentum { false };
        SnapPositionSelection snap_position_selection { SnapPositionSelection::AtGestureEnd };
        bool awaits_layout_for_snapping { false };
    };
    PendingUserScrollendTarget* latched_user_scroll_gesture_for(GC::Ref<DOM::EventTarget>, Optional<Compositor::AsyncScrollNodeStableID> const&);
    void abandon_snapping_of_user_scroll_gesture(Compositor::AsyncScrollNodeStableID);
    void settle_user_scroll_gesture();
    void settle_user_scroll_gesture_if_input_deadline_passed();
    void reset_momentum_fling_state();
    // Which of the latched gestures a settlement is for: every gesture that ran out of input, or only those left
    // waiting for layout by an earlier settlement.
    enum class UserScrollSettlement {
        GestureRanOutOfInput,
        SnappingDeferredUntilLayout,
    };
    void user_scroll_did_settle(UserScrollSettlement = UserScrollSettlement::GestureRanOutOfInput);
    void cancel_user_scroll_settlement();
    void schedule_hover_update_after_async_scroll();
    void update_hover_after_async_scroll_stops();
    void cancel_hover_update_after_async_scroll();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-current-history-entry
    RefPtr<SessionHistoryEntry> m_current_session_history_entry;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-active-history-entry
    RefPtr<SessionHistoryEntry> m_active_session_history_entry;

    // Child navigable identities retained only while reconstructing the active document from canonical session history.
    Vector<Optional<CrossProcessId>> m_child_navigable_history_reconstruction_ids;

    // AD-HOC: Direct reference to the active document, decoupled from session history.
    //         This is the authoritative source for active_document().
    GC::Ptr<DOM::Document> m_active_document;

    // AD-HOC: Active IME composition state. While a composition is in progress, m_input_method_composition_node and
    //         m_input_method_composition_offset record the start of the marked (preedit) text; the marked text spans
    //         from there to the caret. A null node means no composition is in progress.
    void replace_input_method_marked_text(Utf16View text);
    bool apply_input_method_commit_replacement(Utf16View text, i32 replacement_start, i32 replacement_length);
    GC::Ptr<DOM::Node> m_input_method_composition_node;
    size_t m_input_method_composition_offset { 0 };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-closing
    bool m_closing { false };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#delaying-load-events-mode
    Optional<DOM::DocumentLoadEventDelayer> m_delaying_the_load_event;

    // AD-HOC: Guards the parent document's load event delay count during cross-document navigation.
    Optional<DOM::DocumentLoadEventDelayer> m_navigation_load_event_guard;

    // Implied link between navigable and its container.
    GC::Ptr<NavigableContainer> m_container;

    GC::Ref<Page> m_page;

    NavigationObserver::NavigationObserversList m_navigation_observers;

    bool m_has_been_destroyed { false };

    CSSPixelSize m_viewport_size;
    CSSPixelPoint m_viewport_scroll_offset;
    struct PendingPersistedStateRestoration {
        GC::Weak<DOM::Document> document;
        CrossProcessId document_state_id;
        Utf16String navigation_api_key;
    };
    Optional<PendingPersistedStateRestoration> m_pending_persisted_state_restoration;

    Web::EventHandler m_event_handler;

    bool m_has_session_history_entry_and_ready_for_navigation { false };

    Vector<PendingNavigation> m_pending_navigations;

    bool m_is_svg_page { false };
    bool m_needs_repaint { true };
    bool m_needs_to_record_display_list { true };
    bool m_pending_set_browser_zoom_request { false };
    bool m_should_show_line_box_borders { false };
    bool m_should_show_caret_hit_test_debug_overlay { false };
    Optional<PaintConfig> m_compositor_display_list_paint_config;
    RefPtr<Painting::DisplayList> m_compositor_display_list;
    u64 m_compositor_display_list_visual_context_tree_structural_epoch { 0 };
    Painting::DisplayListResourceStorage m_display_list_resource_storage;
    Painting::DisplayListResourceSet m_compositor_display_list_resources;
    Painting::DisplayListResourceSet m_compositor_display_list_command_resources;
    OwnPtr<Compositor::CompositorContextHandle> m_compositor_context;
    RefPtr<Core::Timer> m_async_scroll_hover_update_timer;
    Vector<PendingUserScrollendTarget> m_pending_user_scrollend_targets;
    RefPtr<Core::Timer> m_user_scroll_settle_timer;
    OwnPtr<UserScrollGestureHold> m_compositor_user_scroll_gesture_hold;
    OwnPtr<UserScrollGestureHold> m_wheel_user_scroll_gesture_hold;
    size_t m_user_scroll_gesture_hold_count { 0 };
    Painting::SnapSelectionStrategy::Type m_user_scroll_input_intent { Painting::SnapSelectionStrategy::Type::EndPosition };
    bool m_user_scroll_gesture_travels_under_momentum { false };
    // Momentum that selects no snap position is scrolled by for the rest of the gesture rather than being asked again
    // for each delta it produces.
    enum class MomentumSnapPositionSelection : u8 {
        NotSelectedYet,
        ScrollingToSelectedPosition,
        NoPositionSelected,
    };
    MomentumSnapPositionSelection m_momentum_snap_position_selection { MomentumSnapPositionSelection::NotSelectedYet };
    Painting::MomentumFlingEstimator m_momentum_fling_estimator;
    size_t m_scrolls_being_started { 0 };
    bool m_user_scroll_settlement_awaits_scroll_start { false };
    bool m_is_re_snapping_scroll_containers { false };

    struct PendingAsyncScrollOperation {
        Compositor::AsyncScrollOperationID operation_id { 0 };
        ScrollPromises promises;
        Optional<Compositor::AsyncScrollNodeStableID> stable_node_id;
        Optional<CSSPixelPoint> initial_scroll_offset;
        Optional<CSSPixelPoint> destination_scroll_offset;
        ScrollTrigger trigger { ScrollTrigger::Programmatic };
    };
    Vector<PendingAsyncScrollOperation> m_pending_async_scroll_operations;

    struct MainThreadSmoothScroll {
        Compositor::AsyncScrollNodeStableID stable_node_id;
        Compositor::SmoothScrollAnimation animation;
        MonotonicTime last_tick;
        AK::Duration elapsed;
        CSSPixelPoint initial_scroll_offset;
        CSSPixelPoint destination_scroll_offset;
        ScrollPromises promises;
        ScrollTrigger trigger { ScrollTrigger::Programmatic };
    };
    Vector<MainThreadSmoothScroll> m_main_thread_smooth_scrolls;
};

class WEB_API UserScrollGestureHold {
    AK_MAKE_NONCOPYABLE(UserScrollGestureHold);
    AK_MAKE_NONMOVABLE(UserScrollGestureHold);

public:
    explicit UserScrollGestureHold(LocalNavigable&);
    ~UserScrollGestureHold();

private:
    GC::Weak<LocalNavigable> m_navigable;
};

struct PopulateSessionHistoryEntryDocumentOutput final : public JS::Cell {
    GC_CELL(PopulateSessionHistoryEntryDocumentOutput, JS::Cell);
    GC_DECLARE_ALLOCATOR(PopulateSessionHistoryEntryDocumentOutput);

public:
    GC::Ptr<DOM::Document> document;

    LocalNavigable::NavigationParamsVariant navigation_params { LocalNavigable::NullOrError {} };
    bool save_extra_document_state = true;
    bool download_handled = false;

    Optional<URL::URL> redirected_url;
    Optional<StorageSerializationRecord> classic_history_api_state;
    RefPtr<DocumentState> replacement_document_state;
    bool resource_cleared = false;

    void apply_to(NonnullRefPtr<SessionHistoryEntry> entry);

private:
    virtual void visit_edges(Cell::Visitor&) override;
};

WEB_API HashTable<GC::RawRef<LocalNavigable>>& all_local_navigables();
WEB_API GC::Ptr<LocalNavigable> local_navigable_with_id(CrossProcessId);

bool navigation_must_be_a_replace(URL::URL const& url, DOM::Document const& document);
void finalize_a_cross_document_navigation(GC::Ref<LocalNavigable>, HistoryHandlingBehavior, UserNavigationInvolvement, NonnullRefPtr<SessionHistoryEntry>, GC::Ptr<DOM::Document> pending_document, Optional<Utf16String> expected_ongoing_navigation_id, GC::Ref<OnApplyHistoryStepComplete> on_complete);
void perform_url_and_history_update_steps(DOM::Document& document, URL::URL new_url, Optional<StorageSerializationRecord> = {}, HistoryHandlingBehavior history_handling = HistoryHandlingBehavior::Replace);

}
