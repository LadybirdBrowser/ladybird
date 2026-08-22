/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <LibGC/RootVector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(LocalTraversableNavigable);

LocalTraversableNavigable::LocalTraversableNavigable(GC::Ref<Page> page)
    : LocalNavigable(
          page,
          page->client().is_svg_page_client(),
          Compositor::PagePresentationRegistration::Yes)
    , m_storage_shed(StorageAPI::StorageShed::create())
{
}

LocalTraversableNavigable::~LocalTraversableNavigable() = default;

void LocalTraversableNavigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_emulated_position_data);
    visitor.visit(m_emulated_position_data_observers);
    visitor.visit(m_storage_shed);
    for (auto& operation : m_history_operations) {
        visitor.visit(operation.value.source_snapshot_params);
        visitor.visit(operation.value.pending_document);
        visitor.visit(operation.value.expected_ongoing_navigation_navigable);
        visitor.visit(operation.value.pre_steps);
        visitor.visit(operation.value.on_apply_complete);
        visitor.visit(operation.value.on_complete);
        for (auto& continuation : operation.value.changing_navigable_continuations)
            visitor.visit(continuation.value);
    }
}

static OrderedHashTable<LocalTraversableNavigable*>& user_agent_top_level_traversable_set()
{
    static NeverDestroyed<OrderedHashTable<LocalTraversableNavigable*>> set;
    return *set;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-browsing-context
BrowsingContextAndDocument create_a_new_top_level_browsing_context_and_document(GC::Ref<Page> page)
{
    // 1. Let group and document be the result of creating a new browsing context group and document.
    auto [group, document] = BrowsingContextGroup::create_a_new_browsing_context_group_and_document(page);

    // 2. Return group's browsing context set[0] and document.
    return BrowsingContextAndDocument { **group->browsing_context_set().begin(), document };
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-traversable
GC::Ref<LocalTraversableNavigable> LocalTraversableNavigable::create_a_new_top_level_traversable(GC::Ref<Page> page, GC::Ptr<HTML::BrowsingContext> opener, Utf16String target_name, Optional<CrossProcessId> initial_document_state_id)
{
    auto& vm = Bindings::main_thread_vm();
    page->ensure_compositor_host();

    // 1. Let document be null.
    GC::Ptr<DOM::Document> document = nullptr;

    // 2. If opener is null, then set document to the second return value of creating a new top-level browsing context and document.
    if (!opener) {
        document = create_a_new_top_level_browsing_context_and_document(page).document;
    }

    // 3. Otherwise, set document to the second return value of creating a new auxiliary browsing context and document given opener.
    else {
        document = BrowsingContext::create_a_new_auxiliary_browsing_context_and_document(page, *opener).document;
    }

    // 4. Let documentState be a new document state, with
    if (!initial_document_state_id.has_value())
        initial_document_state_id = page->client().allocate_cross_process_id();
    auto document_state = DocumentState::create(*initial_document_state_id);

    // document: document (now owned by LocalNavigable::m_active_document, not DocumentState)

    // initiator origin: null if opener is null; otherwise, document's origin
    document_state->set_initiator_origin(opener ? document->origin() : Optional<URL::Origin> {});

    // origin: document's origin
    document_state->set_origin(document->origin());

    // navigable target name: targetName
    document_state->set_navigable_target_name(target_name);

    // about base URL: document's about base URL
    document_state->set_about_base_url(document->about_base_url());

    // 5. Let traversable be a new traversable navigable.
    auto traversable = vm.heap().allocate<LocalTraversableNavigable>(page);

    // 6. Initialize the navigable traversable given documentState.
    traversable->initialize_navigable(document_state, nullptr, *document);

    // 7. Let initialHistoryEntry be traversable's active session history entry.
    auto initial_history_entry = traversable->active_session_history_entry();
    VERIFY(initial_history_entry);

    // 8. Set initialHistoryEntry's step to 0.
    initial_history_entry->set_step(0);

    // 9. Append initialHistoryEntry to traversable's session history entries.
    // NB: The UI process performs this step in canonical session history.
    traversable->set_has_session_history_entry_and_ready_for_navigation();

    // 10. If opener is non-null, then legacy-clone a traversable storage shed given opener's top-level traversable and traversable. [STORAGE]
    if (opener) {
        auto opener_traversable = opener->top_level_traversable();
        traversable->storage_shed().legacy_clone(opener_traversable->storage_shed(), page);
    }

    // 11. Append traversable to the user agent's top-level traversable set.
    user_agent_top_level_traversable_set().set(traversable.ptr());

    // 12. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-fresh-top-level-traversable
GC::Ref<LocalTraversableNavigable> LocalTraversableNavigable::create_a_fresh_top_level_traversable(GC::Ref<Page> page, URL::URL const& initial_navigation_url, DocumentResource initial_navigation_post_resource, CrossProcessId initial_document_state_id)
{
    // 1. Let traversable be the result of creating a new top-level traversable given null and the empty string.
    auto traversable = create_a_new_top_level_traversable(page, nullptr, {}, initial_document_state_id);
    page->set_top_level_traversable(traversable);

    // AD-HOC: Deny geolocation until the UI process sends the browser-wide setting via IPC. This prevents a request
    //         from observing the test position during the short window before the initial settings IPC arrives.
    traversable->set_emulated_position_data(Geolocation::GeolocationPositionError::ErrorCode::PermissionDenied);

    // AD-HOC: Mark the about:blank document as finished parsing if we're only going to about:blank
    //         Skip the initial navigation as well. This matches the behavior of the window open steps.

    if (url_matches_about_blank(initial_navigation_url)) {
        auto document = GC::Ref(*traversable->active_document());
        auto completion_token = HTML::HTMLParser::parserless_completion_token(document);
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(traversable->heap(), [document, completion_token, initial_navigation_url] {
            // FIXME: We do this other places too when creating a new about:blank document. Perhaps it's worth a spec issue?
            HTML::HTMLParser::the_end(document, completion_token);

            // FIXME: If we perform the URL and history update steps here, we start hanging tests and the UI process will
            //        try to load() the initial URLs passed on the command line before we finish processing the events here.
            //        However, because we call this before the PageClient is fully initialized... that gets awkward.
        }));
    }

    else {
        // 2. Navigate traversable to initialNavigationURL using traversable's active document, with documentResource set to initialNavigationPostResource.
        MUST(traversable->navigate({ .url = initial_navigation_url,
            .source_document = *traversable->active_document(),
            .document_resource = initial_navigation_post_resource }));
    }

    // 3. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#top-level-traversable
bool LocalTraversableNavigable::is_top_level_traversable() const
{
    // A top-level traversable is a traversable navigable with a null parent.
    return parent() == nullptr;
}

struct SessionHistoryEntryReconstructionState {
    HashMap<CrossProcessId, RefPtr<DocumentState>> document_states;
};

static Vector<NonnullRefPtr<SessionHistoryEntry>> retained_session_history_entries(LocalNavigable& navigable)
{
    Vector<NonnullRefPtr<SessionHistoryEntry>> entries;
    auto append = [&](RefPtr<SessionHistoryEntry> entry) {
        if (!entry)
            return;
        if (entries.find_if([&](auto const& candidate) {
                return candidate.ptr() == entry.ptr();
            })
            != entries.end()) {
            return;
        }
        entries.append(entry.release_nonnull());
    };

    append(navigable.current_session_history_entry());
    append(navigable.active_session_history_entry());

    if (auto window = navigable.active_window()) {
        for (auto const& navigation_entry : window->navigation()->entries())
            append(navigation_entry->session_history_entry());
    }

    return entries;
}

static void prepare_child_navigable_history_reconstruction(LocalNavigable& navigable, SessionHistoryDocumentStateDescriptor const& document_state_descriptor)
{
    Vector<Optional<CrossProcessId>> child_navigable_ids;
    child_navigable_ids.ensure_capacity(document_state_descriptor.nested_histories.size());
    for (auto const& nested_history : document_state_descriptor.nested_histories)
        child_navigable_ids.unchecked_append(nested_history.id);

    auto active_entry = navigable.active_session_history_entry();
    auto active_document = navigable.active_document();
    if (active_entry && active_document
        && active_entry->document_state()->cross_process_id() == document_state_descriptor.id) {
        auto child_navigables = active_document->document_tree_child_navigables();
        if (child_navigables.size() == child_navigable_ids.size()) {
            // FIXME: This is temporary glue for the current load-then-seed ordering.
            //        A replacement WebContent process can create live child navigables
            //        before the UI process sends its canonical session-history tree.
            //        Now that nested history ids are canonical CrossProcessIds, the UI id
            //        must win; retarget the already-created child to match it. The
            //        longer-term model should avoid creating a distinct temporary id
            //        for a child the UI process already knows about.
            for (size_t i = 0; i < child_navigables.size(); ++i) {
                auto canonical_id = *child_navigable_ids[i];
                child_navigables[i]->set_id_for_session_history_reconstruction(canonical_id);
                child_navigable_ids[i].clear();
            }
        }
    }

    navigable.set_child_navigable_history_reconstruction_ids(move(child_navigable_ids));
}

static void apply_session_history_entry_descriptor_from_ui_process(SessionHistoryEntry& entry, SessionHistoryEntryDescriptor& entry_descriptor)
{
    entry.set_url(move(entry_descriptor.url));
    entry.set_step(static_cast<int>(entry_descriptor.step));
    entry.set_classic_history_api_state(move(entry_descriptor.classic_history_api_state));
    entry.set_navigation_api_state(move(entry_descriptor.navigation_api_state));
    entry.set_navigation_api_key(move(entry_descriptor.navigation_api_key));
    entry.set_navigation_api_id(move(entry_descriptor.navigation_api_id));
    entry.set_scroll_restoration_mode(entry_descriptor.scroll_restoration_mode);
    entry.set_scroll_position_data(move(entry_descriptor.scroll_position_data));
}

static void apply_session_history_document_state_descriptor_from_ui_process(DocumentState& document_state, SessionHistoryDocumentStateDescriptor const& document_state_descriptor)
{
    VERIFY(document_state.cross_process_id() == document_state_descriptor.id);
    document_state.set_history_policy_container(document_state_descriptor.history_policy_container);
    document_state.set_request_referrer(document_state_descriptor.request_referrer);
    document_state.set_request_referrer_policy(document_state_descriptor.request_referrer_policy);
    document_state.set_initiator_origin(document_state_descriptor.initiator_origin);
    document_state.set_origin(document_state_descriptor.origin);
    document_state.set_about_base_url(document_state_descriptor.about_base_url);
    document_state.set_resource(document_state_descriptor.resource);
    document_state.set_reload_pending(document_state_descriptor.reload_pending);
    document_state.set_ever_populated(document_state_descriptor.ever_populated);
    document_state.set_navigable_target_name(document_state_descriptor.navigable_target_name);
}

static RefPtr<DocumentState> get_or_create_document_state_from_ui_process(SessionHistoryDocumentStateDescriptor const& document_state_descriptor, SessionHistoryEntryReconstructionState& reconstruction_state)
{
    RefPtr<DocumentState> document_state;
    if (auto existing_document_state = reconstruction_state.document_states.get(document_state_descriptor.id); existing_document_state.has_value())
        document_state = *existing_document_state;

    if (!document_state) {
        document_state = DocumentState::create(document_state_descriptor.id);
        reconstruction_state.document_states.set(document_state_descriptor.id, document_state);
    }

    apply_session_history_document_state_descriptor_from_ui_process(*document_state, document_state_descriptor);
    return document_state;
}

static NonnullRefPtr<SessionHistoryEntry> create_session_history_entry_from_ui_process(SessionHistoryEntryDescriptor entry_descriptor, SessionHistoryEntryReconstructionState& reconstruction_state)
{
    auto entry = SessionHistoryEntry::create();
    apply_session_history_entry_descriptor_from_ui_process(*entry, entry_descriptor);

    auto document_state = get_or_create_document_state_from_ui_process(entry_descriptor.document_state, reconstruction_state);
    VERIFY(document_state);
    entry->set_document_state(move(document_state));
    return entry;
}

enum class PrepareChildHistoryReconstruction {
    No,
    Yes,
};

static NonnullRefPtr<SessionHistoryEntry> resolve_local_session_history_entry(LocalNavigable& navigable, SessionHistoryEntryDescriptor entry_descriptor, PrepareChildHistoryReconstruction prepare_child_history_reconstruction)
{
    auto retained_entries = retained_session_history_entries(navigable);
    auto target_identity = session_history_entry_identity(entry_descriptor);
    for (auto& retained_entry : retained_entries) {
        if (session_history_entry_identity(*retained_entry) == target_identity) {
            apply_session_history_entry_descriptor_from_ui_process(*retained_entry, entry_descriptor);
            apply_session_history_document_state_descriptor_from_ui_process(*retained_entry->document_state(), entry_descriptor.document_state);
            if (prepare_child_history_reconstruction == PrepareChildHistoryReconstruction::Yes) {
                prepare_child_navigable_history_reconstruction(navigable, entry_descriptor.document_state);
            }
            return retained_entry;
        }
    }

    SessionHistoryEntryReconstructionState reconstruction_state;
    for (auto const& retained_entry : retained_entries) {
        auto document_state = retained_entry->document_state();
        if (document_state)
            reconstruction_state.document_states.set(document_state->cross_process_id(), document_state);
    }

    if (prepare_child_history_reconstruction == PrepareChildHistoryReconstruction::Yes) {
        prepare_child_navigable_history_reconstruction(navigable, entry_descriptor.document_state);
    }
    return create_session_history_entry_from_ui_process(move(entry_descriptor), reconstruction_state);
}

static bool is_same_document_push_or_replace(Optional<Bindings::NavigationType> navigation_type, SessionHistoryEntry const& target_entry, Optional<UniqueNodeID> displayed_document_id)
{
    if (navigation_type != Bindings::NavigationType::Push
        && navigation_type != Bindings::NavigationType::Replace) {
        return false;
    }

    return target_entry.document_state()->document_id() == displayed_document_id;
}

static bool expected_ongoing_navigation_was_superseded(Optional<CrossProcessId> navigable_id, Optional<Utf16String> const& expected_navigation_id)
{
    if (!navigable_id.has_value() || !expected_navigation_id.has_value())
        return false;
    auto navigable = local_navigable_with_id(*navigable_id);
    if (!navigable)
        return true;
    if (navigable->has_been_destroyed())
        return true;
    return navigable->ongoing_navigation() != *expected_navigation_id;
}

// AD-HOC: The UI process ran steps 1-4 of attempting to populate the history entry's document. Continue at the
//         algorithm's queued-global-task boundary instead of restarting the algorithm in this process.
void LocalTraversableNavigable::continue_navigation_at_population(NavigationPopulationRequest request, NavigationPopulationResult result)
{
    auto navigable_ptr = local_navigable_with_id(request.navigable_id);
    if (!navigable_ptr
        || navigable_ptr->traversable_navigable().ptr() != this
        || !navigable_ptr->active_document()
        || !navigable_ptr->active_window()) {
        page().client().navigation_population_failed(request.navigable_id, request.navigation_id);
        return;
    }
    auto navigable = GC::Ref { *navigable_ptr };

    SessionHistoryEntryReconstructionState reconstruction_state;
    auto history_entry = create_session_history_entry_from_ui_process(create_session_history_entry_descriptor(move(request.history_entry), 0), reconstruction_state);
    history_entry->set_step(SessionHistoryEntry::Pending::Tag);

    navigable->set_ongoing_navigation(request.navigation_id);

    auto& realm = navigable->active_window()->principal_realm();
    TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto navigation_params_or_error = create_navigation_params_from_descriptor(realm, *navigable, move(result.navigation_params));
    if (navigation_params_or_error.is_error()) {
        navigable->set_ongoing_navigation({});
        navigable->set_delaying_load_events(false);
        page().client().navigation_population_failed(request.navigable_id, request.navigation_id);
        return;
    }
    auto navigation_params = navigation_params_or_error.release_value();

    auto output = navigable->heap().allocate<PopulateSessionHistoryEntryDocumentOutput>();
    output->redirected_url = move(result.redirected_url);
    output->classic_history_api_state = move(result.classic_history_api_state);
    output->resource_cleared = result.resource_cleared;

    // 5. Queue a global task on the navigation and traversal task source, given navigable's active window, to run
    //    these steps:
    auto fetch_client_origin = request.source_snapshot_params.fetch_client.has_value()
        ? Optional<URL::Origin> { request.source_snapshot_params.fetch_client->origin }
        : Optional<URL::Origin> {};

    navigable->queue_navigation_and_traversal_task_for_session_history_entry_population(
        history_entry->url(),
        request.source_snapshot_params.allows_downloading,
        move(fetch_client_origin),
        request.user_involvement,
        request.navigation_id,
        move(navigation_params),
        request.csp_navigation_type,
        output,
        GC::create_function(navigable->heap(), [navigable, history_entry, history_handling = request.history_handling, navigation_id = request.navigation_id, user_involvement = request.user_involvement](GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> output) mutable {
            if (output && output->download_handled) {
                // NB: The UI process ended the recorded load and its transaction when the download adopted this
                //     population's response body.
                navigable->set_ongoing_navigation({});
                navigable->set_delaying_load_events(false);
                return;
            }

            if (output)
                output->apply_to(*history_entry);
            auto pending_document = output ? output->document : GC::Ptr<DOM::Document> {};
            finalize_a_cross_document_navigation(navigable, to_history_handling_behavior(history_handling), user_involvement, history_entry, pending_document, navigation_id, GC::create_function(navigable->heap(), [](HistoryStepResult) { }));
        }));
}

bool LocalTraversableNavigable::adopt_canonical_id_for_child_created_during_history_reconstruction(LocalNavigable& parent, LocalNavigable& child)
{
    VERIFY(child.parent().ptr() == &parent);

    auto parent_document = parent.active_document();
    VERIFY(parent_document);
    if (parent_document->is_completely_loaded())
        return false;

    // The UI-selected entry supplies child identities before a reconstructed document creates its child navigables. Consume the
    // identity at the child's position instead of retaining the nested history entries.
    auto child_navigables = parent_document->document_tree_child_navigables();
    auto child_index = child_navigables.find_first_index(child);
    if (!child_index.has_value())
        return false;

    auto child_id = parent.child_navigable_history_reconstruction_id(*child_index);
    if (!child_id.has_value())
        return false;
    if (local_navigable_with_id(*child_id))
        return false;

    child.set_id_for_session_history_reconstruction(*child_id);
    parent.consume_child_navigable_history_reconstruction_id(*child_index);
    return true;
}

bool LocalTraversableNavigable::route_child_created_during_history_reconstruction(LocalNavigable& parent, LocalNavigable& child, Web::ReconstructedChildNavigation navigation)
{
    VERIFY(child.parent().ptr() == &parent);

    child.prepare_to_populate_reconstructed_history_entry(navigation.target_entry.navigation_api_key);
    prepare_child_navigable_history_reconstruction(child, navigation.target_entry.document_state);

    auto source_snapshot_params = snapshot_source_snapshot_params(nullptr);
    auto request = NavigationPopulationRequest {
        .navigable_id = child.id(),
        .history_entry = create_pending_session_history_entry_descriptor(move(navigation.target_entry)),
        .source_snapshot_params = create_navigation_source_snapshot(source_snapshot_params),
        .target_snapshot_params = snapshot_target_snapshot_params(child),
        .csp_navigation_type = ContentSecurityPolicy::Directives::Directive::NavigationType::Other,
        .history_handling = Bindings::NavigationHistoryBehavior::Replace,
        .user_involvement = UserNavigationInvolvement::BrowserUI,
        .navigation_id = move(navigation.navigation_id),
    };
    child.request_population_for_reconstructed_history_entry(move(request));
    return true;
}

// NB: The UI process sends the reset request at its position on the session history traversal queue and holds the
//     queue until the retained active entry is returned, so this runs with the ordering the replaced algorithms had.
void LocalTraversableNavigable::reset_session_history_for_testing()
{
    auto maybe_active_entry = active_session_history_entry();
    VERIFY(maybe_active_entry);
    auto active_entry = maybe_active_entry.release_nonnull();

    active_entry->set_step(0);
    set_active_session_history_entry(active_entry);
    set_current_session_history_entry(active_entry);
    m_session_history_entry_count = 1;

    auto document = active_document();
    VERIFY(document);
    document->history()->m_index = 0;
    document->history()->m_length = 1;

    Vector<NonnullRefPtr<SessionHistoryEntry>> entries_for_navigation_api { active_entry };
    active_window()->navigation()->initialize_the_navigation_api_entries_for_reconstructed_session_history(entries_for_navigation_api, active_entry);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#deactivate-a-document-for-a-cross-document-navigation
static void deactivate_a_document_for_cross_document_navigation(GC::Ref<DOM::Document> displayed_document, Optional<UserNavigationInvolvement>, NonnullRefPtr<SessionHistoryEntry> target_entry, GC::Ptr<DOM::Document> populated_document, GC::Ref<GC::Function<void()>> after_potential_unloads)
{
    // 1. Let navigable be displayedDocument's node navigable.
    auto navigable = displayed_document->navigable();

    // 2. Let potentiallyTriggerViewTransition be false.
    auto potentially_trigger_view_transition = false;

    // FIXME: 3. Let isBrowserUINavigation be true if userNavigationInvolvement is "browser UI"; otherwise false.

    // FIXME: 4. Set potentiallyTriggerViewTransition to the result of calling can navigation trigger a cross-document
    //           view-transition? given displayedDocument, targetEntry's document, navigationType, and isBrowserUINavigation.

    // 5. If potentiallyTriggerViewTransition is false, then:
    if (!potentially_trigger_view_transition) {
        // FIXME: 1. Let firePageSwapBeforeUnload be the following step
        //            1. Fire the pageswap event given displayedDocument, targetEntry, navigationType, and null.

        // 2. Set the ongoing navigation for navigable to null.
        navigable->set_ongoing_navigation({});

        // 3. Unload a document and its descendants given displayedDocument, targetEntry's document, afterPotentialUnloads, and firePageSwapBeforeUnload.
        (void)target_entry; // FIXME: Used by pageswap and view-transition steps above.
        displayed_document->unload_a_document_and_its_descendants(populated_document, after_potential_unloads);
    }
    // FIXME: 6. Otherwise, queue a global task on the navigation and traversal task source given navigable's active window to run the steps:
    else {
        // FIXME: 1. Let proceedWithNavigationAfterViewTransitionCapture be the following step:
        //            1. Append the following session history traversal steps to navigable's traversable navigable:
        //               1. Set the ongoing navigation for navigable to null.
        //               2. Unload a document and its descendants given displayedDocument, targetEntry's document, and afterPotentialUnloads.

        // FIXME: 2. Let viewTransition be the result of setting up a cross-document view-transition given displayedDocument,
        //           targetEntry's document, navigationType, and proceedWithNavigationAfterViewTransitionCapture.

        // FIXME: 3. Fire the pageswap event given displayedDocument, targetEntry, navigationType, and viewTransition.

        // FIXME: 4. If viewTransition is null, then run proceedWithNavigationAfterViewTransitionCapture.

        TODO();
    }
}

struct ChangingNavigableContinuationState : public JS::Cell {
    GC_CELL(ChangingNavigableContinuationState, JS::Cell);
    GC_DECLARE_ALLOCATOR(ChangingNavigableContinuationState);

    GC::Ptr<DOM::Document> displayed_document;
    Optional<UniqueNodeID> displayed_document_id;
    RefPtr<SessionHistoryEntry> target_entry;
    GC::Ptr<LocalNavigable> navigable;
    bool update_only = false;
    Optional<Bindings::NavigationType> navigation_type;
    LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior { LocalNavigable::NavigationAPIAbortBehavior::Abort };
    UserNavigationInvolvement user_involvement { UserNavigationInvolvement::None };

    GC::Ptr<DOM::Document> pending_document;
    GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> population_output;
    GC::Ptr<DOM::Document> resolved_document;
    Optional<URL::Origin> old_origin;

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(displayed_document);
        visitor.visit(navigable);
        visitor.visit(pending_document);
        visitor.visit(population_output);
        visitor.visit(resolved_document);
    }
};

GC_DEFINE_ALLOCATOR(ChangingNavigableContinuationState);

static void queue_apply_history_step_task(GC::Ref<LocalNavigable> navigable, GC::Ptr<DOM::Document> top_level_document, GC::Ref<GC::Function<void()>> steps)
{
    // AD-HOC: Queue top-level tasks with the active Document instead of using queue_global_task(active_window).
    //         During initial about:blank Window reuse, active_window()->associated_document() can already be the
    //         pending Document, but the apply-history task must run against the current active Document.
    //
    //         Child navigables can destroy or deactivate their active Document before the queued task runs, causing
    //         document-associated tasks to be dropped. Queue child tasks with a null Document so the task remains
    //         runnable, and revalidate the child navigable inside the task.
    auto task_document = navigable->is_top_level_traversable() ? top_level_document : GC::Ptr<DOM::Document> {};
    queue_a_task(Task::Source::NavigationAndTraversal, nullptr, task_document, steps);
}

bool LocalTraversableNavigable::run_changing_navigable_history_step_job_impl(ChangingNavigableHistoryStepJob job, GC::Ptr<SourceSnapshotParams> source_snapshot_params, GC::Ptr<DOM::Document> pending_document, GC::Ref<OnLocalChangingNavigableHistoryStepJobComplete> on_complete)
{
    auto navigable = local_navigable_with_id(job.navigable_id);
    if (!navigable) {
        on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Skipped, nullptr });
        return false;
    }

    // AD-HOC: If the navigable has been destroyed, or has no active window, skip it.
    //         Complete the job here rather than relying on the queued task, because Document::destroy() removes tasks
    //         associated with a document from the task queue, which can cause those tasks to never run.
    if (navigable->has_been_destroyed() || !navigable->active_window()) {
        on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Skipped, nullptr });
        return false;
    }
    // https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-child-navigable
    // NB: The creation/destruction update is the bookkeeping step after the child's nested history has been attached to its canonical
    //     parent document state. If the container's requested navigation has already started, it owns the ongoing navigation ID and
    //     eventual document activation.
    if (!job.navigation_type.has_value() && navigable->ongoing_navigation().has<Utf16String>()) {
        on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Skipped, nullptr });
        return false;
    }

    // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
    // NB: The UI coordinator already ran that step against canonical history and sent the selected entry with this
    //     job. Retain that exact local entry instead of repeating the global-step lookup in WebContent.
    auto claimed_target_entry = job.target_entry;

    auto active_entry = navigable->active_session_history_entry();
    auto applies_same_document_push_or_replace = is_same_document_push_or_replace(
        job.navigation_type, *claimed_target_entry, navigable->active_document_id());

    // A newer synchronous navigation can become active after this operation's exact-entry check. Preserve that visible entry while
    // advancing current entry in queue order; the newer operation advances it again.
    if (applies_same_document_push_or_replace
        && active_entry != claimed_target_entry) {
        navigable->set_current_session_history_entry(claimed_target_entry);
        on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Skipped, nullptr });
        return false;
    }

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#update-for-navigable-creation/destruction
    // AD-HOC: Unconditionally populating a document here could unload and re-navigate a frame because an unrelated
    //         navigable was created or destroyed — which no other engine does. So we skip such applying-the-target-
    //         entry-would-cross-documents navigables here.
    //         https://github.com/whatwg/html/issues/12724
    if (!job.navigation_type.has_value()) {
        bool would_cross_documents = claimed_target_entry->document_state()->document_id() != navigable->active_document_id()
            || claimed_target_entry->document_state()->reload_pending();
        if (would_cross_documents) {
            on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Skipped, nullptr });
            return false;
        }
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#fire-a-traverse-navigate-event
    // The UI process is the canonical coordinator for navigation starts and history operations. Only it can prove
    // that a navigation was admitted after this traversal; the presence of a local navigation ID does not establish
    // that ordering. A proven-newer navigation owns the visible outcome, so abandon the traversal without committing
    // its canonical target step.
    if (job.navigation_type == Bindings::NavigationType::Traverse
        && job.superseded_by_newer_navigation) {
        on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Stale, nullptr });
        return false;
    }

    // 2. Set navigable's current session history entry to targetEntry.
    navigable->set_current_session_history_entry(claimed_target_entry);

    // 3. Set navigable's ongoing navigation to "traversal".
    // AD-HOC: A same-document push or replace can reach this point after a
    //         later cross-document navigation has claimed the navigable. Keep
    //         that navigation's ID; the history update must not cancel it.
    auto preserve_ongoing_navigation = applies_same_document_push_or_replace
        && navigable->ongoing_navigation().has<Utf16String>();
    if (!preserve_ongoing_navigation)
        navigable->set_ongoing_navigation(HTML::LocalNavigable::Traversal::Tag, job.navigation_api_abort_behavior);

    queue_apply_history_step_task(*navigable, navigable->active_document(), GC::create_function(heap(), [this, job = move(job), source_snapshot_params, pending_document, claimed_target_entry = move(claimed_target_entry), navigable, on_complete] {
        // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
        if (navigable->has_been_destroyed() || !navigable->active_window() || !navigable->active_document()) {
            on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Skipped, nullptr });
            return;
        }

        // 1. Let displayedEntry be navigable's active session history entry.
        auto displayed_entry = navigable->active_session_history_entry();

        // 2. Let targetEntry be navigable's current session history entry.
        auto target_entry = navigable->current_session_history_entry();
        if (!target_entry || target_entry != claimed_target_entry) {
            // AD-HOC: The HTML Standard expects the session history traversal queue to serialize this task with
            //         later navigations. Our web-compatible deferral of navigations that arrive during traversal
            //         can let a newer navigation replace the current entry before this task runs. Treat this state
            //         as stale instead of applying its old target step after the newer navigation.
            on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Stale, nullptr });
            return;
        }

        auto displayed_step = displayed_entry ? displayed_entry->step_value() : Optional<int> {};
        auto target_step = target_entry ? target_entry->step_value() : Optional<int> {};
        if (!target_step.has_value()) {
            // NB: Child navigables created during a busy top-level navigation can still have a pending initial
            //     session history entry. The spec's step-based history algorithms operate on used history steps,
            //     so a pending child entry must not block or crash the top-level apply-history step. The queued
            //     child creation/destruction history step will reconcile the child once it has a concrete step.
            on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Skipped, nullptr });
            return;
        }

        // 3. Let changingNavigableContinuation be a changing navigable continuation state with:
        auto changing_navigable_continuation = heap().allocate<ChangingNavigableContinuationState>();
        changing_navigable_continuation->displayed_document = navigable->active_document();
        changing_navigable_continuation->displayed_document_id = navigable->active_document_id();
        changing_navigable_continuation->target_entry = target_entry;
        changing_navigable_continuation->navigable = navigable;
        changing_navigable_continuation->update_only = false;
        changing_navigable_continuation->navigation_type = job.navigation_type;
        changing_navigable_continuation->navigation_api_abort_behavior = job.navigation_api_abort_behavior;
        changing_navigable_continuation->user_involvement = job.user_involvement;
        changing_navigable_continuation->pending_document = pending_document;
        changing_navigable_continuation->population_output = nullptr;

        // 4. If displayedEntry is targetEntry and targetEntry's document state's reload pending is false, then:
        auto traverses_from_initial_about_blank = job.navigation_type == Bindings::NavigationType::Traverse
            && changing_navigable_continuation->displayed_document->is_initial_about_blank();
        bool is_update_only = displayed_entry == target_entry
            && !target_entry->document_state()->reload_pending()
            && !traverses_from_initial_about_blank;
        if (is_same_document_push_or_replace(
                job.navigation_type, *target_entry,
                changing_navigable_continuation->displayed_document_id)) {
            is_update_only = !target_entry->document_state()->reload_pending()
                || displayed_entry == target_entry;
        }
        if (is_update_only) {
            // 1. Set changingNavigableContinuation's update-only to true.
            changing_navigable_continuation->update_only = true;
            changing_navigable_continuation->resolved_document = navigable->active_document();

            // 2. Enqueue changingNavigableContinuation on changingNavigableContinuations.
            on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Ready, changing_navigable_continuation });

            // 3. Abort these steps.
            return;
        }

        // 5. Switch on navigationType:
        if (job.navigation_type.has_value()) {
            switch (job.navigation_type.value()) {
            case Bindings::NavigationType::Reload:
                // - "reload": Assert: targetEntry's document state's reload pending is true.
                VERIFY(target_entry->document_state()->reload_pending());
                break;
            case Bindings::NavigationType::Traverse:
                // - "traverse": Assert: targetEntry's document state's ever populated is true.
                VERIFY(target_entry->document_state()->ever_populated());
                break;
            case Bindings::NavigationType::Replace:
                // FIXME: Add ever populated check
                // - "replace": Assert: targetEntry's step is displayedEntry's step and targetEntry's document state's ever populated is false.
                break;
            case Bindings::NavigationType::Push:
                // FIXME: Add ever populated check, and fix the bug where top level traversable's step is not updated when a child navigable navigates
                // - "push": Assert: targetEntry's step is displayedEntry's step + 1 and targetEntry's document state's ever populated is false.
                if (displayed_step.has_value() && *target_step <= *displayed_step) {
                    // AD-HOC: A queued push can become stale if a later navigation commits before this task runs.
                    //         Browser engines let the later navigation win; do the same and avoid moving the
                    //         traversable's current step back to this push target during completion.
                    on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Stale, nullptr });
                    return;
                }
                VERIFY(target_entry != displayed_entry);
                break;
            }
        }

        // 6. Let oldOrigin be targetEntry's document state's origin.
        auto old_origin = target_entry->document_state()->origin();

        // 7. If all of the following are true:
        //   * navigable is not traversable;
        //   * targetEntry is not navigable's current session history entry; and
        //   * oldOrigin is the same as navigable's current session history entry's document state's origin,
        // then:
        if (!navigable->is_traversable()
            && target_entry != navigable->current_session_history_entry()
            && old_origin == navigable->current_session_history_entry()->document_state()->origin()) {
            // 1. Let navigation be navigable's active window's navigation API.
            auto navigation = navigable->active_window()->navigation();

            // 2. Fire a traverse navigate event at navigation given targetEntry and userInvolvement.
            navigation->fire_a_traverse_navigate_event(*target_entry, job.user_involvement);
        }

        auto after_document_populated = GC::create_function(heap(), [old_origin, changing_navigable_continuation, target_entry, navigable, on_complete](GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> output) mutable {
            changing_navigable_continuation->population_output = output;
            changing_navigable_continuation->old_origin = old_origin;

            // Compute the resolved document: pending document (from the finalize path),
            // population output (from traversal path), or active document (same-document).
            GC::Ptr<DOM::Document> resolved_document;
            if (changing_navigable_continuation->pending_document)
                resolved_document = changing_navigable_continuation->pending_document;
            else if (output && output->document)
                resolved_document = output->document;
            else
                resolved_document = navigable->active_document();
            changing_navigable_continuation->resolved_document = resolved_document;

            // 1. If targetEntry's document is null, then set changingNavigableContinuation's update-only to true.
            bool has_fresh_document = changing_navigable_continuation->pending_document || (output && output->document);
            if (!has_fresh_document && target_entry->document_state()->document_id() != navigable->active_document_id())
                changing_navigable_continuation->update_only = true;

            // 2. If targetEntry's document's origin is not oldOrigin, then set targetEntry's classic history API state to StructuredSerializeForStorage(null).
            // 3. If all of the following are true:
            //     - navigable's parent is null;
            //     - targetEntry's document's browsing context is not an auxiliary browsing context whose opener browsing context is non-null; and
            //     - targetEntry's document's origin is not oldOrigin,
            //    then set targetEntry's document state's navigable target name to the empty string.
            // NOTE: Steps 2-3 are deferred to after_potential_unload to avoid exposing mutations during unload.

            // 4. Enqueue changingNavigableContinuation on changingNavigableContinuations.
            on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Ready, changing_navigable_continuation });
        });

        // 8. If targetEntry's document is null, or targetEntry's document state's reload pending is true, then:
        bool needs_population = !changing_navigable_continuation->pending_document
            && (traverses_from_initial_about_blank
                || target_entry->document_state()->document_id() != navigable->active_document_id()
                || target_entry->document_state()->reload_pending());
        if (needs_population) {
            // FIXME: 1. Let navTimingType be "back_forward" if targetEntry's document is null; otherwise "reload".

            // 2. Let targetSnapshotParams be the result of snapshotting target snapshot params given navigable.
            auto target_snapshot_params = snapshot_target_snapshot_params(*navigable);

            // 3. Let potentiallyTargetSpecificSourceSnapshotParams be sourceSnapshotParams.
            auto potentially_target_specific_source_snapshot_params = source_snapshot_params;

            // 4. If potentiallyTargetSpecificSourceSnapshotParams is null, then set it to the result of snapshotting source snapshot params given navigable's active document.
            if (!potentially_target_specific_source_snapshot_params)
                potentially_target_specific_source_snapshot_params = snapshot_source_snapshot_params(navigable->active_document());

            // 5. Set targetEntry's document state's reload pending to false.
            // AD-HOC: Preserve reload pending for steps 6 and 7 before step 5 clears it.
            // See https://github.com/whatwg/html/issues/12760.
            auto input_reload_pending = target_entry->document_state()->reload_pending();
            target_entry->document_state()->set_reload_pending(false);
            page().client().page_did_set_session_history_entry_document_state_reload_pending(
                navigable->id(), target_entry->navigation_api_key(), false);

            // 6. Let allowPOST be targetEntry's document state's reload pending.
            auto allow_POST = input_reload_pending || traverses_from_initial_about_blank;

            // https://github.com/whatwg/html/issues/9869
            // Population runs in a deferred task, during which sync navigations can mutate
            // the live entry. Snapshot the input fields now so population reads stable values.
            auto input_url = target_entry->url();
            auto input_document_resource = target_entry->document_state()->resource();
            auto input_request_referrer = target_entry->document_state()->request_referrer();
            auto input_request_referrer_policy = target_entry->document_state()->request_referrer_policy();
            auto input_initiator_origin = target_entry->document_state()->initiator_origin();
            auto input_origin = target_entry->document_state()->origin();
            auto input_history_policy_container = target_entry->document_state()->history_policy_container();
            auto input_about_base_url = target_entry->document_state()->about_base_url();
            auto input_navigable_target_name = target_entry->document_state()->navigable_target_name();
            auto input_ever_populated = target_entry->document_state()->ever_populated();

            // 7. In parallel, attempt to populate the history entry's document for targetEntry, given navigable, potentiallyTargetSpecificSourceSnapshotParams,
            //    targetSnapshotParams, userInvolvement, with allowPOST set to allowPOST and completionSteps set to
            //    queue a global task on the navigation and traversal task source given navigable's active window to
            //    run afterDocumentPopulated.
            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [input_url = move(input_url), input_document_resource = move(input_document_resource), input_request_referrer = move(input_request_referrer), input_request_referrer_policy, input_initiator_origin = move(input_initiator_origin), input_origin = move(input_origin), input_history_policy_container = move(input_history_policy_container), input_about_base_url = move(input_about_base_url), input_navigable_target_name = move(input_navigable_target_name), input_reload_pending, input_ever_populated, potentially_target_specific_source_snapshot_params, target_snapshot_params, this, allow_POST, navigable, after_document_populated, user_involvement = job.user_involvement] {
                navigable->populate_session_history_entry_document(
                    move(input_url), move(input_document_resource), move(input_request_referrer),
                    input_request_referrer_policy, move(input_initiator_origin), move(input_origin),
                    input_history_policy_container, move(input_about_base_url), move(input_navigable_target_name),
                    input_reload_pending, input_ever_populated,
                    *potentially_target_specific_source_snapshot_params, target_snapshot_params,
                    user_involvement, {}, LocalNavigable::NullOrError {},
                    ContentSecurityPolicy::Directives::Directive::NavigationType::Other, allow_POST,
                    GC::create_function(this->heap(), [this, after_document_populated, navigable](GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> output) {
                        VERIFY(active_window());
                        // AD-HOC: Queue through the apply-history helper so child completion tasks survive frame
                        //         removal/deactivation. The continuation revalidates the navigable before applying.
                        queue_apply_history_step_task(*navigable, navigable->active_document(), GC::create_function(heap(), [after_document_populated, output]() {
                            after_document_populated->function()(output);
                        }));
                    }));
            }));
        }
        // Otherwise, run afterDocumentPopulated immediately.
        else {
            after_document_populated->function()(nullptr);
        }
    }));
    return true;
}

static bool changing_navigable_is_still_current(GC::Ptr<LocalNavigable> navigable, Optional<UniqueNodeID> expected_active_document_id, bool allow_ongoing_navigation)
{
    if (!navigable || navigable->has_been_destroyed() || !navigable->active_window())
        return false;

    auto active_document = navigable->active_document();
    if (!active_document || active_document->has_been_destroyed())
        return false;

    if (navigable->active_document_id() != expected_active_document_id)
        return false;

    return navigable->ongoing_navigation().has<Empty>()
        || (allow_ongoing_navigation && navigable->ongoing_navigation().has<Utf16String>());
}

static void clear_ongoing_history_traversal(GC::Ptr<LocalNavigable> navigable, LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior)
{
    if (!navigable || navigable->has_been_destroyed())
        return;

    if (!navigable->ongoing_navigation().has<LocalNavigable::Traversal>())
        return;

    // AD-HOC: The HTML Standard's traversal queue normally reaches one of the per-navigable "Set the ongoing
    //         navigation for navigable to null" steps before this state completes. Our stale-task exits deliberately
    //         skip the rest of the history step so newer navigations win like they do in Chromium, WebKit, and Gecko,
    //         but we still have to remove the traversal sentinel. Use the shared setter so pending navigations queued
    //         behind this traversal are drained in one place.
    navigable->set_ongoing_navigation({}, navigation_api_abort_behavior);
}

void LocalTraversableNavigable::apply_changing_navigable_history_step_continuation_impl(GC::Ref<ChangingNavigableContinuationState> continuation, LocalApplyChangingNavigableHistoryStepContinuation command, GC::Ref<GC::Function<void(Optional<ReplicatedNavigableState>, Optional<SessionHistoryEntryPersistedState>)>> on_complete)
{
    // 4. Let displayedDocument be changingNavigableContinuation's displayed document.
    auto displayed_document = continuation->displayed_document;

    // 5. Let targetEntry be changingNavigableContinuation's target entry.
    auto population_output = continuation->population_output;
    auto old_origin = continuation->old_origin;

    // 6. Let navigable be changingNavigableContinuation's navigable.
    auto navigable = continuation->navigable;

    // AD-HOC: We should not continue navigation if navigable has been destroyed.
    if (navigable->has_been_destroyed()) {
        on_complete->function()({}, {});
        return;
    }
    // AD-HOC: The displayed document may have been destroyed during the nested step execution above.
    if (!displayed_document->navigable()) {
        on_complete->function()({}, {});
        return;
    }

    // 7. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index given traversable and targetStep.
    auto script_history_length = command.history_object_length_and_index.script_history_length;
    auto script_history_index = command.history_object_length_and_index.script_history_index;

    // 9. Let entriesForNavigationAPI be the result of getting session history entries for the navigation API given navigable and targetStep.

    // NOTE: Steps 10 and 11 come after step 12.

    // 12. In both cases, let afterPotentialUnloads be the following steps:
    bool const update_only = continuation->update_only;
    RefPtr<SessionHistoryEntry> const target_entry = continuation->target_entry;
    auto const displayed_document_id = continuation->displayed_document_id;
    auto after_potential_unload = GC::create_function(heap(), [this, navigable, update_only, target_entry, continuation, population_output, old_origin, displayed_document_id, script_history_length, script_history_index, entries_for_navigation_api = move(command.entries_for_navigation_api), navigation_type = continuation->navigation_type, navigation_api_abort_behavior = continuation->navigation_api_abort_behavior, on_complete] {
        if (update_only || continuation->resolved_document.ptr() == continuation->displayed_document.ptr()) {
            auto applies_same_document_push_or_replace = is_same_document_push_or_replace(
                navigation_type, *target_entry, displayed_document_id);

            if (applies_same_document_push_or_replace
                && navigable->active_session_history_entry() != target_entry) {
                clear_ongoing_history_traversal(navigable, navigation_api_abort_behavior);
                on_complete->function()({}, {});
                return;
            }

            // AD-HOC: Child navigable same-document/update-only tasks are queued without an associated Document so
            //         they can survive the old active Document being deactivated. That also lets them run after the
            //         child frame was destroyed or after a newer navigation claimed the frame. Browser engines let
            //         the newer frame state win, so skip this stale continuation in that case.
            if (!changing_navigable_is_still_current(navigable, displayed_document_id, applies_same_document_push_or_replace)) {
                clear_ongoing_history_traversal(navigable, navigation_api_abort_behavior);
                on_complete->function()({}, {});
                return;
            }
        }

        if (population_output)
            population_output->apply_to(*target_entry);

        // Post-population adjustments — only run when a fresh document was produced
        // (not for 204/205 no-document outcomes where resolved_document is the old active document).
        bool has_fresh_document = continuation->pending_document || (population_output && population_output->document);
        if (has_fresh_document) {
            auto resolved_document = continuation->resolved_document;
            // 2. If targetEntry's document's origin is not oldOrigin, then set targetEntry's classic history API state to StructuredSerializeForStorage(null).
            if (resolved_document->origin() != old_origin) {
                auto& vm = navigable->vm();
                target_entry->set_classic_history_api_state(MUST(structured_serialize_for_storage(vm, JS::js_null())));
            }

            // 3. If all of the following are true:
            //     - navigable's parent is null;
            //     - targetEntry's document's browsing context is not an auxiliary browsing context whose opener browsing context is non-null; and
            //     - targetEntry's document's origin is not oldOrigin,
            //    then set targetEntry's document state's navigable target name to the empty string.
            if (navigable->parent() == nullptr
                && !(resolved_document->browsing_context()->is_auxiliary() && resolved_document->browsing_context()->opener_browsing_context() != nullptr)
                && target_entry->document_state()->origin() != old_origin) {
                target_entry->document_state()->set_navigable_target_name(Utf16String {});
            }
        }

        // 1. Let previousEntry be navigable's active session history entry.
        auto previous_entry = navigable->active_session_history_entry();

        // NB: A fresh replacement endpoint temporarily installs targetEntry on its initial about:blank Document.
        //     Preserve the UI-selected target state across activation instead of treating that initial Document's
        //     viewport as outgoing state for targetEntry.
        auto target_entry_persisted_state = !update_only
                && previous_entry == target_entry
                && navigation_type == Bindings::NavigationType::Traverse
                && continuation->displayed_document->is_initial_about_blank()
            ? create_session_history_entry_persisted_state(*target_entry)
            : Optional<SessionHistoryEntryPersistedState> {};

        // 2. If changingNavigableContinuation's update-only is false, then activate history entry targetEntry for navigable.
        auto resolved_document = continuation->resolved_document;
        Optional<ReplicatedNavigableState> activated_navigable_state;
        if (!update_only) {
            navigable->activate_history_entry(*target_entry, *resolved_document);
            activated_navigable_state = navigable->replicated_state();
        }
        if (target_entry_persisted_state.has_value())
            target_entry->set_scroll_position_data(move(target_entry_persisted_state->scroll_position_data));
        auto previous_entry_persisted_state = !update_only && previous_entry && !target_entry_persisted_state.has_value()
            ? create_session_history_entry_persisted_state(*previous_entry)
            : Optional<SessionHistoryEntryPersistedState> {};

        // 3. Let updateDocument be an algorithm step which performs update document for history step application given
        //    targetEntry's document, targetEntry, changingNavigableContinuation's update-only, scriptHistoryLength,
        //    scriptHistoryIndex, navigationType, entriesForNavigationAPI, and previousEntry.
        auto update_document = [script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), target_entry, update_only, navigation_type, previous_entry, resolved_document, navigable] {
            // NB: The specification initializes the navigation API entries for every newly activated document.
            //     Gating this on a non-null navigationType left documents activated by a creation/destruction
            //     update without an initialized navigation API entry list, which crashes the first same-document
            //     update on them (for example a document.open() on a child that finished loading while the
            //     creation update was still queued).
            resolved_document->update_for_history_step_application(*target_entry, update_only, script_history_length, script_history_index, navigation_type, entries_for_navigation_api, previous_entry, true);

            if (update_only)
                navigable->notify_navigation_observers_navigation_complete();
        };

        // 4. If targetEntry's document is equal to displayedDocument, then perform updateDocument.
        // NOTE: We compare against the pre-activation displayed_document_id (not the current
        //       active entry) because activate_history_entry() has already updated the active entry above.
        if (target_entry->document_state()->document_id() == displayed_document_id) {
            update_document();
        }
        // AD-HOC: When the document already has its parser pre-loaded with in-memory data (currently set up
        //         only for about:srcdoc), perform updateDocument synchronously instead of queueing it.
        //         updateDocument calls Document::set_ready_to_run_scripts(), which kicks off the deferred
        //         parser. Running it in the same task as activation guarantees the body element exists before
        //         script in the parent navigable can observe the new document — matching Chrome and Firefox
        //         behavior for srcdoc iframes.
        else if (resolved_document->has_deferred_parser_start()) {
            update_document();
        }
        // 5. Otherwise, queue a global task on the navigation and traversal task source given targetEntry's document's relevant global object to perform updateDocument
        else {
            queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*resolved_document), GC::create_function(heap(), move(update_document)));
        }

        // 6. Increment completedChangeJobs.
        on_complete->function()(move(activated_navigable_state), move(previous_entry_persisted_state));
    });

    // 10. If changingNavigableContinuation's update-only is true, or targetEntry's document is displayedDocument, then:
    if (continuation->update_only || continuation->resolved_document.ptr() == displayed_document.ptr()) {
        // 1. Set the ongoing navigation for navigable to null.
        // AD-HOC: Only clear the traversal marker installed by this history
        //         operation. A pending cross-document navigation can already
        //         own the navigable when a synchronous update reaches here.
        clear_ongoing_history_traversal(navigable, continuation->navigation_api_abort_behavior);

        // 2. Queue a global task on the navigation and traversal task source given navigable's active window to perform afterPotentialUnloads.
        queue_apply_history_step_task(*navigable, navigable->active_document(), after_potential_unload);
    }
    // 11. Otherwise:
    else {
        // 1. Assert: navigationType is not null.
        VERIFY(continuation->navigation_type.has_value());

        // 2. Deactivate displayedDocument, given userInvolvement, targetEntry, navigationType, and afterPotentialUnloads.
        deactivate_a_document_for_cross_document_navigation(*displayed_document, continuation->user_involvement, *target_entry, continuation->resolved_document, after_potential_unload);
    }
}

void LocalTraversableNavigable::update_nonchanging_navigable_history_step_state(CrossProcessId navigable_id, HistoryObjectLengthAndIndex history_object_length_and_index, GC::Ref<GC::Function<void()>> on_complete)
{
    auto navigable = local_navigable_with_id(navigable_id);

    // AD-HOC: This check is not in the spec but we should not continue navigation if navigable has been destroyed,
    //         or if there's no active window.
    if (!navigable || navigable->has_been_destroyed() || !navigable->active_window()) {
        on_complete->function()();
        return;
    }

    // AD-HOC: Queue with null document instead of using queue_global_task.
    //         Tasks associated with a document are only runnable when fully active.
    //         In the async state machine, documents can become non-fully-active between
    //         queue time and execution, causing the task to be permanently stuck.
    //         A null-document task is always runnable; we check validity inside.
    queue_a_task(Task::Source::NavigationAndTraversal, nullptr, nullptr, GC::create_function(heap(), [navigable = GC::Ref { *navigable }, history_object_length_and_index, on_complete] {
        if (navigable->has_been_destroyed() || !navigable->active_window() || !navigable->active_document()->is_fully_active()) {
            on_complete->function()();
            return;
        }

        // 1. Let document be navigable's active document.
        auto document = navigable->active_document();

        // 2. Set document's history object's index to scriptHistoryIndex.
        document->history()->m_index = history_object_length_and_index.script_history_index;

        // 3. Set document's history object's length to scriptHistoryLength.
        document->history()->m_length = history_object_length_and_index.script_history_length;

        // 4. Increment completedNonchangingJobs.
        on_complete->function()();
    }));
}

class CheckUnloadingCanceledState : public GC::Cell {
    GC_CELL(CheckUnloadingCanceledState, GC::Cell);
    GC_DECLARE_ALLOCATOR(CheckUnloadingCanceledState);

public:
    using Result = LocalTraversableNavigable::CheckIfUnloadingIsCanceledResult;
    static constexpr int TIMEOUT_MS = 15000;

    CheckUnloadingCanceledState(
        GC::Ptr<LocalTraversableNavigable> traversable,
        Optional<UserNavigationInvolvement> user_involvement,
        GC::Ref<GC::Function<void(Result)>> callback)
        : m_traversable(traversable)
        , m_user_involvement(user_involvement)
        , m_callback(callback)
        , m_timeout(Platform::Timer::create_single_shot(heap(), TIMEOUT_MS, GC::create_function(heap(), [this] {
            if (!m_completed) {
                dbgln("FIXME: check_if_unloading_is_canceled timed out");
                finish(Result::Continue);
            }
        })))
    {
        m_timeout->start();
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        for (auto& doc : m_phase2_documents)
            visitor.visit(doc);
        visitor.visit(m_traversable);
        visitor.visit(m_callback);
        visitor.visit(m_timeout);
    }

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
    void start(Vector<GC::Root<LocalNavigable>> const& navigables_that_need_before_unload, RefPtr<SessionHistoryEntry> target_entry)
    {
        // 1. Let documentsToFireBeforeunload be the active document of each item in navigablesThatNeedBeforeUnload.
        for (auto& navigable : navigables_that_need_before_unload)
            m_phase2_documents.append(*navigable->active_document());

        // 2. Let unloadPromptShown be false.

        // 3. Let finalStatus be "continue".

        // 4. If traversable was given, then:
        if (m_traversable) {
            // 1. Assert: targetStep and userInvolvementForNavigateEvent were given.
            VERIFY(target_entry);
            VERIFY(m_user_involvement.has_value());

            // 2. Let targetEntry be the result of getting the target history entry given traversable and targetStep.
            m_target_entry = move(target_entry);

            // 3. If targetEntry is not traversable's current session history entry, and targetEntry's document state's origin is the same as
            //    traversable's current session history entry's document state's origin:
            if (m_target_entry != m_traversable->current_session_history_entry() && m_target_entry->document_state()->origin() == m_traversable->current_session_history_entry()->document_state()->origin()) {

                // 1. Let eventsFired be false.

                // 2. Let needsBeforeunload be true if navigablesThatNeedBeforeUnload contains traversable; otherwise false.
                m_needs_beforeunload = navigables_that_need_before_unload.find_if([this](auto const& navigable) {
                    return navigable.ptr() == m_traversable.ptr();
                }) != navigables_that_need_before_unload.end();

                // 3. If needsBeforeunload is true, then remove traversable's active document from documentsToFireBeforeunload.
                if (m_needs_beforeunload) {
                    m_phase2_documents.remove_first_matching([this](auto& document) {
                        return document.ptr() == m_traversable->active_document().ptr();
                    });
                }

                start_phase1();
                return;
            }
        }

        start_phase2();
    }

private:
    void start_phase1()
    {
        // 4. Queue a global task on the navigation and traversal task source given traversable's active window to perform the following steps:
        VERIFY(m_traversable->active_window());
        queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*m_traversable->active_window()), GC::create_function(GC::Heap::the(), [this] {
            // 1. if needsBeforeunload is true, then:
            if (m_needs_beforeunload) {
                // 1. Let (unloadPromptShownForThisDocument, unloadPromptCanceledByThisDocument) be the result of running the steps to fire beforeunload given traversable's active document and false.
                auto [unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document] = m_traversable->active_document()->steps_to_fire_beforeunload(false);

                // 2. If unloadPromptShownForThisDocument is true, then set unloadPromptShown to true.
                if (unload_prompt_shown_for_this_document)
                    m_unload_prompt_shown = true;

                // 3. If unloadPromptCanceledByThisDocument is true, then set finalStatus to "canceled-by-beforeunload".
                if (unload_prompt_canceled_by_this_document)
                    m_final_status = Result::CanceledByBeforeUnload;
            }

            // 2. If finalStatus is "canceled-by-beforeunload", then abort these steps.
            if (m_final_status == Result::CanceledByBeforeUnload) {
                finish(m_final_status);
                return;
            }

            // 3. Let navigation be traversable's active window's navigation API.
            VERIFY(m_traversable->active_window());
            auto navigation = m_traversable->active_window()->navigation();

            // 4. Let navigateEventResult be the result of firing a traverse navigate event at navigation given targetEntry and userInvolvementForNavigateEvent.
            VERIFY(m_target_entry);
            auto navigate_event_result = navigation->fire_a_traverse_navigate_event(*m_target_entry, *m_user_involvement);

            // 5. If navigateEventResult is false, then set finalStatus to "canceled-by-navigate".
            if (!navigate_event_result)
                m_final_status = Result::CanceledByNavigate;

            // 6. Set eventsFired to true.

            phase1_completed();
        }));
    }

    void phase1_completed()
    {
        // 5. Wait for eventsFired to be true.

        // 6. If finalStatus is not "continue", then return finalStatus.
        if (m_final_status != Result::Continue) {
            finish(m_final_status);
            return;
        }
        start_phase2();
    }

    void start_phase2()
    {
        if (m_phase2_documents.is_empty()) {
            finish(m_final_status);
            return;
        }

        // 5. Let totalTasks be the size of documentsToFireBeforeunload.

        // 6. Let completedTasks be 0.
        m_remaining_phase2_tasks = m_phase2_documents.size();

        // 7. For each document of documentsToFireBeforeunload, queue a global task on the navigation and traversal task source given document's relevant global object to run the steps:
        for (auto& document : m_phase2_documents) {
            // AD-HOC: Queue with a null document instead of using queue_global_task. Tasks associated with a document
            //         are only runnable when fully active. In the async state machine, documents can become non
            //         fully-active between queue and execution time, causing the task to be permanently stuck.
            //         A null-document task is always runnable; we check validity inside.
            queue_a_task(Task::Source::NavigationAndTraversal, nullptr, nullptr, GC::create_function(heap(), [this, document] {
                if (document->has_been_destroyed() || !document->is_fully_active()) {
                    did_complete_phase2_task();
                    return;
                }

                // 1. Let (unloadPromptShownForThisDocument, unloadPromptCanceledByThisDocument) be the result of running the steps to fire beforeunload given document and unloadPromptShown.
                auto [unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document] = document->steps_to_fire_beforeunload(m_unload_prompt_shown);

                // 2. If unloadPromptShownForThisDocument is true, then set unloadPromptShown to true.
                if (unload_prompt_shown_for_this_document)
                    m_unload_prompt_shown = true;

                // 3. If unloadPromptCanceledByThisDocument is true, then set finalStatus to "canceled-by-beforeunload".
                if (unload_prompt_canceled_by_this_document)
                    m_final_status = Result::CanceledByBeforeUnload;

                // 4. Increment completedTasks.
                did_complete_phase2_task();
            }));
        }
    }

    void did_complete_phase2_task()
    {
        VERIFY(m_remaining_phase2_tasks > 0);
        if (--m_remaining_phase2_tasks > 0)
            return;

        // 8. Wait for completedTasks to be totalTasks.

        // 9. Return finalStatus.
        finish(m_final_status);
    }

    void finish(Result final_result)
    {
        if (m_completed)
            return;
        m_completed = true;
        m_timeout->stop();
        m_callback->function()(final_result);
    }

    Result m_final_status { Result::Continue };
    bool m_unload_prompt_shown { false };
    bool m_completed { false };
    bool m_needs_beforeunload { false };
    size_t m_remaining_phase2_tasks { 0 };
    Vector<GC::Ref<DOM::Document>> m_phase2_documents;
    GC::Ptr<LocalTraversableNavigable> m_traversable;
    RefPtr<SessionHistoryEntry> m_target_entry;
    Optional<UserNavigationInvolvement> m_user_involvement;
    GC::Ref<GC::Function<void(Result)>> m_callback;
    GC::Ref<Platform::Timer> m_timeout;
};

GC_DEFINE_ALLOCATOR(CheckUnloadingCanceledState);

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
void LocalTraversableNavigable::check_if_unloading_is_canceled(
    Vector<GC::Root<LocalNavigable>> navigables_that_need_before_unload,
    GC::Ptr<LocalTraversableNavigable> traversable,
    RefPtr<SessionHistoryEntry> target_entry,
    Optional<UserNavigationInvolvement> user_involvement_for_navigate_events,
    GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback)
{
    auto state = heap().allocate<CheckUnloadingCanceledState>(
        traversable,
        user_involvement_for_navigate_events,
        callback);
    state->start(navigables_that_need_before_unload, move(target_entry));
}

void LocalTraversableNavigable::check_if_unloading_is_canceled(Vector<GC::Root<LocalNavigable>> navigables_that_need_before_unload, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback)
{
    check_if_unloading_is_canceled(move(navigables_that_need_before_unload), {}, {}, {}, callback);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void LocalTraversableNavigable::traverse_the_history_by_delta(int delta, GC::Ptr<DOM::Document> source_document)
{
    // 1. Let sourceSnapshotParams and initiatorToCheck be null.
    GC::Ptr<SourceSnapshotParams> source_snapshot_params = nullptr;
    GC::Ptr<LocalNavigable> initiator_to_check = nullptr;

    // 2. Let userInvolvement be "browser UI".
    UserNavigationInvolvement user_involvement = UserNavigationInvolvement::BrowserUI;

    // 3. If sourceDocument is given, then:
    if (source_document) {
        // 1. Set sourceSnapshotParams to the result of snapshotting source snapshot params given sourceDocument.
        source_snapshot_params = snapshot_source_snapshot_params(source_document);

        // 2. Set initiatorToCheck to sourceDocument's node navigable.
        initiator_to_check = source_document->navigable();

        // 3. Set userInvolvement to "none".
        user_involvement = UserNavigationInvolvement::None;
    }

    // 4. Append the following session history traversal steps to traversable:
    request_history_operation(
        TraverseByDeltaHistoryOperationParameters {
            .traversable_id = id(),
            .delta = delta,
            .initiator_to_check = initiator_to_check ? Optional<CrossProcessId> { initiator_to_check->id() } : OptionalNone {},
            .initiator_source_snapshot = source_snapshot_params
                ? Optional<Web::InitiatorSourceSnapshot> { { .sandboxing_flags = source_snapshot_params->sandboxing_flags, .has_transient_activation = source_snapshot_params->has_transient_activation } }
                : OptionalNone {},
            .user_involvement = user_involvement,
        },
        {
            .source_snapshot_params = source_snapshot_params,
        });
}

void LocalTraversableNavigable::request_history_operation(HistoryOperationParameters parameters)
{
    request_history_operation(move(parameters), {});
}

void LocalTraversableNavigable::request_history_operation(HistoryOperationParameters parameters, HistoryOperationState state)
{
    if (parameters.has<ReloadHistoryOperationParameters>()) {
        VERIFY(!state.on_apply_complete);
        state.on_apply_complete = GC::create_function(heap(), [this](HistoryStepResult result) {
            if (result == HistoryStepResult::Applied)
                return;

            if (auto current_entry = current_session_history_entry(); current_entry && current_entry->document_state()->reload_pending()) {
                current_entry->document_state()->set_reload_pending(false);
                page().client().page_did_set_session_history_entry_document_state_reload_pending(
                    id(), current_entry->navigation_api_key(), false);
            }
        });
    }

    auto operation_id = page().client().allocate_cross_process_id();
    m_history_operations.set(operation_id, move(state));
    page().client().page_did_request_history_operation(operation_id, move(parameters));
}

void LocalTraversableNavigable::handle_ui_history_operation_started(CrossProcessId operation_id, Optional<Web::ReconstructedChildNavigation> reconstructed_child_navigation, GC::Ref<OnHistoryOperationReady> ready)
{
    auto operation = m_history_operations.find(operation_id);
    if (operation == m_history_operations.end()) {
        ready->function()(HistoryStepResult::Applied);
        return;
    }

    // NB: A cross-document navigation can be superseded after its document has populated but before its queued
    //     history-step application runs. The navigate algorithm's earlier navigation ID check caught the same
    //     condition before requesting this operation; this re-check keeps a stale finalization from claiming
    //     "traversal" and canceling the newer navigation.
    if (expected_ongoing_navigation_was_superseded(
            operation->value.expected_ongoing_navigation_navigable ? Optional<CrossProcessId> { operation->value.expected_ongoing_navigation_navigable->id() } : OptionalNone {},
            operation->value.expected_ongoing_navigation_id)) {
        ready->function()(HistoryStepResult::Applied);
        return;
    }

    if (operation->value.pre_steps) {
        operation->value.pre_steps->function()(move(reconstructed_child_navigation), ready);
        return;
    }
    ready->function()(Empty {});
}

void LocalTraversableNavigable::run_ui_history_step_unload_cancelation_job(CrossProcessId operation_id, SessionHistoryEntryDescriptor target_entry_descriptor, Vector<CrossProcessId> navigables_crossing_documents, UserNavigationInvolvement user_involvement, GC::Ref<GC::Function<void(HistoryStepResult)>> on_complete)
{
    (void)operation_id;

    auto target_entry = resolve_local_session_history_entry(
        *this, move(target_entry_descriptor),
        PrepareChildHistoryReconstruction::No);
    if (user_involvement == UserNavigationInvolvement::BrowserUI
        && ongoing_navigation().has<Utf16String>()
        && target_entry == current_session_history_entry()
        && target_entry == active_session_history_entry()
        && !target_entry->document_state()->reload_pending()) {
        // https://html.spec.whatwg.org/multipage/browsing-the-web.html#nav-traversal-ui
        // https://html.spec.whatwg.org/multipage/document-lifecycle.html#stop-document-loading
        // INTEROP: A browser UI traversal back to the still-active entry while a new document is loading
        //          cancels the pending navigation before entering the specified apply the history step algorithm.
        //          The standard describes browser UI traversal and stopping loading separately, but does not
        //          prescribe how Back interacts with an uncommitted navigation. Chromium, WebKit, and Gecko all
        //          stop the uncommitted load in this situation.
        stop_loading();
        on_complete->function()(HistoryStepResult::CanceledPendingNavigation);
        return;
    }

    // 5. If checkForCancelation is true, and the result of checking if unloading is canceled given
    //    navigablesCrossingDocuments, traversable, targetStep, and userInvolvement is not "continue", then return
    //    that result.
    Vector<GC::Root<LocalNavigable>> navigables;
    navigables.ensure_capacity(navigables_crossing_documents.size());
    for (auto navigable_id : navigables_crossing_documents) {
        if (auto navigable = local_navigable_with_id(navigable_id); navigable && !navigable->has_been_destroyed())
            navigables.append(*navigable);
    }
    check_if_unloading_is_canceled(move(navigables), *this, move(target_entry), user_involvement,
        GC::create_function(heap(), [on_complete](CheckIfUnloadingIsCanceledResult result) {
            switch (result) {
            case CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload:
                on_complete->function()(HistoryStepResult::CanceledByBeforeUnload);
                return;
            case CheckIfUnloadingIsCanceledResult::CanceledByNavigate:
                on_complete->function()(HistoryStepResult::CanceledByNavigate);
                return;
            case CheckIfUnloadingIsCanceledResult::Continue:
                on_complete->function()(HistoryStepResult::Applied);
                return;
            }
            VERIFY_NOT_REACHED();
        }));
}

void LocalTraversableNavigable::run_ui_changing_navigable_history_job(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryDescriptor target_entry, UserNavigationInvolvement user_involvement, Optional<Bindings::NavigationType> navigation_type, LocalNavigable::NavigationAPIAbortBehavior navigation_api_abort_behavior, bool superseded_by_newer_navigation, GC::Ref<OnChangingNavigableHistoryStepJobComplete> on_complete)
{
    auto& operation = m_history_operations.ensure(operation_id);
    auto source_snapshot_params = operation.source_snapshot_params;
    auto pending_document = operation.pending_document;
    RefPtr<SessionHistoryEntry> local_target_entry;
    if (operation.local_target_navigable_id == navigable_id) {
        VERIFY(operation.local_target_entry);
        local_target_entry = operation.local_target_entry;
    }
    if (expected_ongoing_navigation_was_superseded(
            operation.expected_ongoing_navigation_navigable ? Optional<CrossProcessId> { operation.expected_ongoing_navigation_navigable->id() } : OptionalNone {},
            operation.expected_ongoing_navigation_id)) {
        on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Stale);
        return;
    }

    auto navigable = local_navigable_with_id(navigable_id);
    if (!navigable) {
        on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Skipped);
        return;
    }
    if (local_target_entry) {
        auto document_state = local_target_entry->document_state();
        if (!document_state
            || document_state->cross_process_id() != target_entry.document_state.id) {
            on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Stale);
            return;
        }

        apply_session_history_entry_descriptor_from_ui_process(*local_target_entry, target_entry);
        apply_session_history_document_state_descriptor_from_ui_process(*document_state, target_entry.document_state);
        prepare_child_navigable_history_reconstruction(*navigable, target_entry.document_state);
    } else {
        local_target_entry = resolve_local_session_history_entry(
            *navigable, move(target_entry),
            PrepareChildHistoryReconstruction::Yes);
    }
    if (!local_target_entry) {
        on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Stale);
        return;
    }
    auto did_claim_navigable = run_changing_navigable_history_step_job_impl(
        {
            .navigable_id = navigable_id,
            .target_entry = local_target_entry.release_nonnull(),
            .user_involvement = user_involvement,
            .navigation_type = navigation_type,
            .navigation_api_abort_behavior = navigation_api_abort_behavior,
            .superseded_by_newer_navigation = superseded_by_newer_navigation,
        },
        source_snapshot_params, pending_document,
        GC::create_function(heap(), [this, operation_id, navigable_id, navigation_api_abort_behavior, on_complete](LocalChangingNavigableHistoryStepJobResult result) {
            if (auto operation = m_history_operations.find(operation_id); operation != m_history_operations.end()) {
                if (result.disposition == ChangingNavigableHistoryStepJobDisposition::Ready) {
                    VERIFY(result.continuation);
                    operation->value.changing_navigable_continuations.set(navigable_id, *result.continuation);
                } else {
                    operation->value.claimed_navigables_awaiting_continuation.remove(navigable_id);
                }
            }
            // NB: A job can have claimed its navigable before becoming stale, or can finish after its operation was
            //     abandoned. Release the claim so nothing remains blocked behind its "traversal" sentinel.
            if (result.disposition != ChangingNavigableHistoryStepJobDisposition::Ready
                || !m_history_operations.contains(operation_id))
                clear_ongoing_history_traversal(local_navigable_with_id(navigable_id), navigation_api_abort_behavior);
            on_complete->function()(result.disposition);
        }));
    if (did_claim_navigable)
        operation.claimed_navigables_awaiting_continuation.set(navigable_id, navigation_api_abort_behavior);
}

static Vector<NonnullRefPtr<SessionHistoryEntry>> session_history_entries_for_navigation_api_from_ui_process(LocalNavigable& navigable, Vector<SessionHistoryEntryDescriptor> entry_descriptors)
{
    auto retained_entries = retained_session_history_entries(navigable);
    SessionHistoryEntryReconstructionState reconstruction_state;
    for (auto const& retained_entry : retained_entries) {
        auto document_state = retained_entry->document_state();
        if (document_state)
            reconstruction_state.document_states.set(document_state->cross_process_id(), document_state);
    }

    Vector<NonnullRefPtr<SessionHistoryEntry>> entries;
    entries.ensure_capacity(entry_descriptors.size());

    for (auto& entry_descriptor : entry_descriptors) {
        RefPtr<SessionHistoryEntry> local_entry;
        auto entry_identity = session_history_entry_identity(entry_descriptor);
        for (auto const& retained_entry : retained_entries) {
            if (session_history_entry_identity(*retained_entry) == entry_identity) {
                local_entry = retained_entry;
                break;
            }
        }

        if (local_entry) {
            apply_session_history_entry_descriptor_from_ui_process(*local_entry, entry_descriptor);
            apply_session_history_document_state_descriptor_from_ui_process(*local_entry->document_state(), entry_descriptor.document_state);
            entries.append(local_entry.release_nonnull());
            continue;
        }

        auto entry = SessionHistoryEntry::create();
        apply_session_history_entry_descriptor_from_ui_process(*entry, entry_descriptor);
        entry->set_document_state(get_or_create_document_state_from_ui_process(entry_descriptor.document_state, reconstruction_state));
        entries.append(move(entry));
    }

    return entries;
}

void LocalTraversableNavigable::apply_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, HistoryObjectLengthAndIndex history_object_length_and_index, Vector<SessionHistoryEntryDescriptor> entry_descriptors_for_navigation_api, GC::Ref<GC::Function<void(Optional<ReplicatedNavigableState>, Optional<SessionHistoryEntryPersistedState>)>> on_complete)
{
    auto operation = m_history_operations.find(operation_id);
    if (operation == m_history_operations.end()) {
        on_complete->function()({}, {});
        return;
    }
    auto continuation = operation->value.changing_navigable_continuations.take(navigable_id);
    if (!continuation.has_value()) {
        on_complete->function()({}, {});
        return;
    }
    operation->value.claimed_navigables_awaiting_continuation.remove(navigable_id);

    Vector<NonnullRefPtr<SessionHistoryEntry>> entries_for_navigation_api;
    if (auto navigable = local_navigable_with_id(navigable_id); navigable && !navigable->has_been_destroyed())
        entries_for_navigation_api = session_history_entries_for_navigation_api_from_ui_process(*navigable, move(entry_descriptors_for_navigation_api));

    apply_changing_navigable_history_step_continuation_impl(
        *continuation,
        {
            .history_object_length_and_index = history_object_length_and_index,
            .entries_for_navigation_api = move(entries_for_navigation_api),
        },
        on_complete);
}

void LocalTraversableNavigable::complete_ui_history_operation(CrossProcessId operation_id, HistoryStepResult result, Optional<i32> committed_step, u64 session_history_entry_count)
{
    auto operation = m_history_operations.take(operation_id);
    m_session_history_entry_count = session_history_entry_count;
    if (!operation.has_value())
        return;

    // AD-HOC: A canceled or stale operation can leave claimed navigables behind whose continuations never
    //         applied; release their "traversal" sentinels so newer navigations are not blocked.
    for (auto const& claim : operation->claimed_navigables_awaiting_continuation)
        clear_ongoing_history_traversal(local_navigable_with_id(claim.key), claim.value);

    if (committed_step.has_value()
        && operation->local_target_navigable_id.has_value()
        && operation->local_target_entry
        && !operation->local_target_entry->step_value().has_value()) {
        if (auto navigable = local_navigable_with_id(*operation->local_target_navigable_id);
            navigable && !navigable->is_top_level_traversable()) {
            operation->local_target_entry->set_step(*committed_step);
        }
    }
    if (operation->on_apply_complete)
        operation->on_apply_complete->function()(result);
    if (operation->on_complete)
        operation->on_complete->function()(result);
}

void LocalTraversableNavigable::finalize_same_document_navigation(GC::Ref<LocalNavigable> target_navigable, NonnullRefPtr<SessionHistoryEntry> target_entry, RefPtr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior history_handling, UserNavigationInvolvement user_involvement, Optional<SessionHistoryEntryPersistedState> previous_entry_persisted_state)
{
    if (target_navigable->has_been_destroyed())
        return;

    // 2. If targetNavigable's active session history entry is not targetEntry, then return.
    if (target_navigable->active_session_history_entry() != target_entry)
        return;

    Optional<SessionHistoryEntryIdentity> entry_to_replace_identity;
    if (entry_to_replace)
        entry_to_replace_identity = session_history_entry_identity(*entry_to_replace);

    auto parameters = FinalizeSameDocumentNavigationHistoryOperationParameters {
        .navigable_id = target_navigable->id(),
        .target_entry = create_same_document_navigation_entry(target_entry),
        .entry_to_replace = move(entry_to_replace_identity),
        .previous_entry_persisted_state = move(previous_entry_persisted_state),
        .history_handling = history_handling,
        .user_involvement = user_involvement,
    };

    request_history_operation(
        move(parameters),
        {
            .local_target_navigable_id = target_navigable->id(),
            .local_target_entry = target_entry,
        });
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#close-a-top-level-traversable
void LocalTraversableNavigable::close_top_level_traversable(PromptToUnload prompt_to_unload)
{
    // 1. If traversable's is closing is true, then return.
    if (is_closing())
        return;

    // AD-HOC: Set the is closing flag to prevent re-entrant calls from queuing duplicate session history steps.
    set_closing(true);

    // 2. Definitely close traversable.
    definitely_close_top_level_traversable(prompt_to_unload);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#definitely-close-a-top-level-traversable
void LocalTraversableNavigable::definitely_close_top_level_traversable(PromptToUnload prompt_to_unload)
{
    VERIFY(is_top_level_traversable());

    auto append_close_steps = [this] {
        // 3. Append the following session history traversal steps to traversable:
        request_history_operation(
            CloseTopLevelTraversableHistoryOperationParameters { .traversable_id = id() },
            {
                .on_complete = GC::create_function(heap(), [this](HistoryStepResult result) {
                    // NB: An abandoned close never reached its queue position; do not destroy the traversable for it.
                    if (result != HistoryStepResult::Applied)
                        return;

                    // 1. Let afterAllUnloads be an algorithm step which destroys traversable.
                    auto after_all_unloads = GC::create_function(heap(), [this] {
                        destroy_top_level_traversable();
                    });

                    // 2. Unload a document and its descendants given traversable's active document, null, and afterAllUnloads.
                    active_document()->unload_a_document_and_its_descendants({}, after_all_unloads);
                }),
            });
    };

    if (prompt_to_unload == PromptToUnload::No) {
        append_close_steps();
        return;
    }

    // 1. Let toUnload be traversable's active document's inclusive descendant navigables.
    auto to_unload = active_document()->inclusive_descendant_navigables();

    // 2. If the result of checking if unloading is canceled for toUnload is not "continue", then return.
    check_if_unloading_is_canceled(move(to_unload), GC::create_function(heap(), [this, append_close_steps = move(append_close_steps)](CheckIfUnloadingIsCanceledResult result) {
        if (result != CheckIfUnloadingIsCanceledResult::Continue) {
            // AD-HOC: Allow a later close attempt if this one was canceled.
            set_closing(false);
            return;
        }

        // 3. Append the following session history traversal steps to traversable:
        append_close_steps();
    }));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-top-level-traversable
void LocalTraversableNavigable::destroy_top_level_traversable()
{
    VERIFY(is_top_level_traversable());

    // 1. Let browsingContext be traversable's active browsing context.
    auto browsing_context = active_browsing_context();

    // 2. For each historyEntry in traversable's session history entries:
    // NOTE: Without bfcache, only the active document is alive, so we only need to destroy it.
    if (active_document())
        active_document()->destroy_a_document_and_its_descendants();

    // 3. Remove browsingContext.
    if (!browsing_context) {
        dbgln("TraversableNavigable::destroy_top_level_traversable: No browsing context?");
    } else {
        browsing_context->remove();
    }

    // 4. Remove traversable from the user interface (e.g., close or hide its tab in a tabbed browser).
    page().client().page_did_close_top_level_traversable();

    // 5. Remove traversable from the user agent's top-level traversable set.
    user_agent_top_level_traversable_set().remove(this);

    // FIXME: 6. Invoke WebDriver BiDi navigable destroyed with traversable.

    // FIXME: Figure out why we need to do this... we shouldn't be leaking Navigables for all time.
    //        However, without this, we can keep stale destroyed traversables around.
    set_has_been_destroyed();
    remove_from_all_local_navigables();
}

// https://html.spec.whatwg.org/multipage/interaction.html#system-visibility-state
void LocalTraversableNavigable::set_system_visibility_state(VisibilityState visibility_state)
{
    if (m_system_visibility_state == visibility_state)
        return;
    m_system_visibility_state = visibility_state;

    // When a user agent determines that the system visibility state for
    // traversable navigable traversable has changed to newState, it must run the following steps:

    // 1. Let navigables be the inclusive descendant navigables of traversable's active document.
    auto navigables = active_document()->inclusive_descendant_navigables();

    // 2. For each navigable of navigables:
    for (auto& navigable : navigables) {
        // 1. Let document be navigable's active document.
        auto document = navigable->active_document();
        VERIFY(document);

        // 2. Queue a global task on the user interaction task source given document's relevant global object
        //    to update the visibility state of document with newState.
        queue_global_task(Task::Source::UserInteraction, relevant_global_object(*document), GC::create_function(heap(), [visibility_state, document] {
            document->update_the_visibility_state(visibility_state);
        }));
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#currently-focused-area-of-a-top-level-traversable
GC::Ptr<DOM::Node> LocalTraversableNavigable::currently_focused_area()
{
    // 1. If traversable does not have system focus, then return null.
    if (!is_focused())
        return nullptr;

    // 2. Let candidate be traversable's active document.
    auto candidate = active_document();

    // 3. While candidate's focused area is a navigable container with a non-null content navigable:
    //    set candidate to the active document of that navigable container's content navigable.
    while (candidate->focused_area()
        && is<NavigableContainer>(candidate->focused_area().ptr())
        && as<NavigableContainer>(*candidate->focused_area()).content_navigable()) {
        candidate = as<LocalNavigable>(*as<NavigableContainer>(*candidate->focused_area()).content_navigable()).active_document();
    }

    // 4. If candidate's focused area is non-null, set candidate to candidate's focused area.
    if (candidate->focused_area()) {
        // NOTE: We return right away here instead of assigning to candidate,
        //       since that would require compromising type safety.
        return candidate->focused_area();
    }

    // 5. Return candidate.
    return candidate;
}

// https://w3c.github.io/geolocation/#dfn-emulated-position-data
Geolocation::EmulatedPositionData const& LocalTraversableNavigable::emulated_position_data() const
{
    VERIFY(is_top_level_traversable());
    return m_emulated_position_data;
}

// https://w3c.github.io/geolocation/#dfn-emulated-position-data
void LocalTraversableNavigable::set_emulated_position_data(Geolocation::EmulatedPositionData data)
{
    VERIFY(is_top_level_traversable());
    m_emulated_position_data = data;

    GC::RootVector<GC::Ref<GC::Function<void()>>> observers;
    for (auto& observer : m_emulated_position_data_observers)
        observers.append(observer.value);
    for (auto& observer : observers)
        observer->function()();
}

void LocalTraversableNavigable::set_emulated_position_data(Geolocation::CoordinatesData coordinates_data)
{
    VERIFY(is_top_level_traversable());
    auto coords = GC::Heap::the().allocate<Geolocation::GeolocationCoordinates>(move(coordinates_data));
    set_emulated_position_data(coords);
}

u64 LocalTraversableNavigable::register_emulated_position_data_observer(GC::Ref<GC::Function<void()>> observer)
{
    VERIFY(is_top_level_traversable());
    auto observer_id = m_next_emulated_position_data_observer_id++;
    m_emulated_position_data_observers.set(observer_id, observer);
    return observer_id;
}

void LocalTraversableNavigable::unregister_emulated_position_data_observer(u64 observer_id)
{
    VERIFY(is_top_level_traversable());
    m_emulated_position_data_observers.remove(observer_id);
}

void LocalTraversableNavigable::process_screenshot_requests()
{
    auto& client = page().client();
    while (!m_screenshot_tasks.is_empty()) {
        auto task = m_screenshot_tasks.dequeue();
        if (task.node_id.has_value()) {
            auto* dom_node = DOM::Node::from_unique_id(*task.node_id);
            if (dom_node)
                dom_node->document().update_layout(DOM::UpdateLayoutReason::ProcessScreenshot);
            auto const* layout_node = dom_node ? dom_node->layout_node() : nullptr;
            if (!layout_node || !Painting::has_committed_box(*layout_node)) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto rect = page().enclosing_device_rect(Painting::absolute_border_box_rect(*layout_node));
            auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, rect.size().to_type<int>());
            if (bitmap_or_error.is_error()) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto bitmap = bitmap_or_error.release_value();
            auto painting_surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
            PaintConfig paint_config { .canvas_fill_rect = rect.to_type<int>() };
            render_screenshot(painting_surface, paint_config, [bitmap, &client] {
                client.page_did_take_screenshot(bitmap->to_shareable_bitmap());
            });
        } else {
            active_document()->update_layout(DOM::UpdateLayoutReason::ProcessScreenshot);
            auto const* layout_node = active_document()->layout_node();
            VERIFY(layout_node && Painting::has_committed_box(*layout_node));
            auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(*layout_node);
            auto rect = page().enclosing_device_rect(scrollable_overflow_rect.value());
            auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, rect.size().to_type<int>());
            if (bitmap_or_error.is_error()) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto bitmap = bitmap_or_error.release_value();
            auto painting_surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
            PaintConfig paint_config { .paint_overlay = true, .canvas_fill_rect = rect.to_type<int>() };
            render_screenshot(painting_surface, paint_config, [bitmap, &client] {
                client.page_did_take_screenshot(bitmap->to_shareable_bitmap());
            });
        }
    }
}

}
