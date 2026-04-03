/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TraversableNavigable);

TraversableNavigable::TraversableNavigable(GC::Ref<Page> page)
    : Navigable(page, page->client().is_svg_page_client())
    , m_storage_shed(StorageAPI::StorageShed::create(page->heap()))
    , m_session_history_traversal_queue(vm().heap().allocate<SessionHistoryTraversalQueue>())
{
}

TraversableNavigable::~TraversableNavigable() = default;

void TraversableNavigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_emulated_position_data.has<GC::Ref<Geolocation::GeolocationCoordinates>>())
        visitor.visit(m_emulated_position_data.get<GC::Ref<Geolocation::GeolocationCoordinates>>());
    visitor.visit(m_session_history_traversal_queue);
    visitor.visit(m_storage_shed);
    visitor.visit(m_apply_history_step_state);
    visitor.visit(m_paused_apply_history_step_state);
}

static OrderedHashTable<TraversableNavigable*>& user_agent_top_level_traversable_set()
{
    static OrderedHashTable<TraversableNavigable*> set;
    return set;
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
GC::Ref<TraversableNavigable> TraversableNavigable::create_a_new_top_level_traversable(GC::Ref<Page> page, GC::Ptr<HTML::BrowsingContext> opener, String target_name)
{
    auto& vm = Bindings::main_thread_vm();

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
    auto document_state = DocumentState::create();

    // document: document (now owned by Navigable::m_active_document, not DocumentState)

    // initiator origin: null if opener is null; otherwise, document's origin
    document_state->set_initiator_origin(opener ? Optional<URL::Origin> {} : document->origin());

    // origin: document's origin
    document_state->set_origin(document->origin());

    // navigable target name: targetName
    document_state->set_navigable_target_name(target_name);

    // about base URL: document's about base URL
    document_state->set_about_base_url(document->about_base_url());

    // 5. Let traversable be a new traversable navigable.
    auto traversable = vm.heap().allocate<TraversableNavigable>(page);

    // 6. Initialize the navigable traversable given documentState.
    traversable->initialize_navigable(document_state, nullptr, *document);

    // 7. Let initialHistoryEntry be traversable's active session history entry.
    auto initial_history_entry = traversable->active_session_history_entry();
    VERIFY(initial_history_entry);

    // 8. Set initialHistoryEntry's step to 0.
    initial_history_entry->set_step(0);

    // 9. Append initialHistoryEntry to traversable's session history entries.
    traversable->m_session_history_entries.append(*initial_history_entry);
    traversable->set_has_session_history_entry_and_ready_for_navigation();

    // FIXME: 10. If opener is non-null, then legacy-clone a traversable storage shed given opener's top-level traversable and traversable. [STORAGE]

    // 11. Append traversable to the user agent's top-level traversable set.
    user_agent_top_level_traversable_set().set(traversable);

    // 12. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-fresh-top-level-traversable
GC::Ref<TraversableNavigable> TraversableNavigable::create_a_fresh_top_level_traversable(GC::Ref<Page> page, URL::URL const& initial_navigation_url, Variant<Empty, String, POSTResource> initial_navigation_post_resource)
{
    // 1. Let traversable be the result of creating a new top-level traversable given null and the empty string.
    auto traversable = create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);

    // AD-HOC: Set the default top-level emulated position data for the traversable, which points to Market St. SF.
    // FIXME: We should not emulate by default, but ask the user what to do. E.g. disable Geolocation, set an emulated
    //        position, or allow Ladybird to engage with the system's geolocation services. This is completely separate
    //        from the permission model for "powerful features" such as Geolocation.
    auto& realm = traversable->active_document()->realm();
    auto emulated_position_coordinates = realm.create<Geolocation::GeolocationCoordinates>(
        realm,
        Geolocation::CoordinatesData {
            .accuracy = 100.0,
            .latitude = 37.7647658,
            .longitude = -122.4345892,
            .altitude = 60.0,
            .altitude_accuracy = 10.0,
            .heading = 0.0,
            .speed = 0.0,
        });
    traversable->set_emulated_position_data(emulated_position_coordinates);

    // AD-HOC: Mark the about:blank document as finished parsing if we're only going to about:blank
    //         Skip the initial navigation as well. This matches the behavior of the window open steps.

    if (url_matches_about_blank(initial_navigation_url)) {
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(traversable->heap(), [traversable, initial_navigation_url] {
            // FIXME: We do this other places too when creating a new about:blank document. Perhaps it's worth a spec issue?
            HTML::HTMLParser::the_end(*traversable->active_document());

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
bool TraversableNavigable::is_top_level_traversable() const
{
    // A top-level traversable is a traversable navigable with a null parent.
    return parent() == nullptr;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
Vector<int> TraversableNavigable::get_all_used_history_steps() const
{
    // FIXME: 1. Assert: this is running within traversable's session history traversal queue.

    // 2. Let steps be an empty ordered set of non-negative integers.
    OrderedHashTable<int> steps;

    // 3. Let entryLists be the ordered set « traversable's session history entries ».
    Vector<Vector<NonnullRefPtr<SessionHistoryEntry>>> entry_lists { session_history_entries() };

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto entry_list = entry_lists.take_first();

        // 1. For each entry of entryList:
        for (auto& entry : entry_list) {
            // 1. Append entry's step to steps.
            steps.set(entry->step().get<int>());

            // 2. For each nestedHistory of entry's document state's nested histories, append nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry->document_state()->nested_histories())
                entry_lists.append(nested_history.entries);
        }
    }

    // 5. Return steps, sorted.
    auto sorted_steps = steps.values();
    quick_sort(sorted_steps);
    return sorted_steps;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-history-object-length-and-index
TraversableNavigable::HistoryObjectLengthAndIndex TraversableNavigable::get_the_history_object_length_and_index(int step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    auto steps = get_all_used_history_steps();

    // 2. Let scriptHistoryLength be the size of steps.
    auto script_history_length = steps.size();

    // 3. Assert: steps contains step.
    VERIFY(steps.contains_slow(step));

    // 4. Let scriptHistoryIndex be the index of step in steps.
    auto script_history_index = *steps.find_first_index(step);

    // 5. Return (scriptHistoryLength, scriptHistoryIndex).
    return HistoryObjectLengthAndIndex {
        .script_history_length = script_history_length,
        .script_history_index = script_history_index
    };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-used-step
int TraversableNavigable::get_the_used_step(int step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    auto steps = get_all_used_history_steps();

    // 2. Return the greatest item in steps that is less than or equal to step.
    VERIFY(!steps.is_empty());
    Optional<int> result;
    for (size_t i = 0; i < steps.size(); i++) {
        if (steps[i] <= step) {
            if (!result.has_value() || (result.value() < steps[i])) {
                result = steps[i];
            }
        }
    }
    return result.value();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#get-all-navigables-whose-current-session-history-entry-will-change-or-reload
Vector<GC::Root<Navigable>> TraversableNavigable::get_all_navigables_whose_current_session_history_entry_will_change_or_reload(int target_step) const
{
    // 1. Let results be an empty list.
    Vector<GC::Root<Navigable>> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<GC::Root<Navigable>> navigables_to_check;
    navigables_to_check.append(const_cast<TraversableNavigable&>(*this));

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. If targetEntry is not navigable's current session history entry or targetEntry's document state's reload
        //    pending is true, then append navigable to results.
        // AD-HOC: We don't want to choose a navigable that has ongoing traversal.
        if ((target_entry != navigable->current_session_history_entry() || target_entry->document_state()->reload_pending()) && !navigable->ongoing_navigation().has<Traversal>()) {
            results.append(*navigable);
        }

        // 3. If targetEntry's document is navigable's document, and targetEntry's document state's reload pending is
        //    false, then extend navigablesToCheck with the child navigables of navigable.
        if (target_entry->document_state()->document_id() == navigable->active_document_id() && !target_entry->document_state()->reload_pending()) {
            navigables_to_check.extend(navigable->child_navigables());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-only-need-history-object-length/index-update
Vector<GC::Root<Navigable>> TraversableNavigable::get_all_navigables_that_only_need_history_object_length_index_update(int target_step) const
{
    // NOTE: Other navigables might not be impacted by the traversal. For example, if the response is a 204, the currently active document will remain.
    //       Additionally, going 'back' after a 204 will change the current session history entry, but the active session history entry will already be correct.

    // 1. Let results be an empty list.
    Vector<GC::Root<Navigable>> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<GC::Root<Navigable>> navigables_to_check;
    navigables_to_check.append(const_cast<TraversableNavigable&>(*this));

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. If targetEntry is navigable's current session history entry and targetEntry's document state's reload pending is false, then:
        if (target_entry == navigable->current_session_history_entry() && !target_entry->document_state()->reload_pending()) {
            // 1.  Append navigable to results.
            results.append(navigable);

            // 2. Extend navigablesToCheck with navigable's child navigables.
            navigables_to_check.extend(navigable->child_navigables());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-might-experience-a-cross-document-traversal
Vector<GC::Root<Navigable>> TraversableNavigable::get_all_navigables_that_might_experience_a_cross_document_traversal(int target_step) const
{
    // NOTE: From traversable's session history traversal queue's perspective, these documents are candidates for going cross-document during the
    //       traversal described by targetStep. They will not experience a cross-document traversal if the status code for their target document is
    //       HTTP 204 No Content.
    //       Note that if a given navigable might experience a cross-document traversal, this algorithm will return navigable but not its child navigables.
    //       Those would end up unloaded, not traversed.

    // 1. Let results be an empty list.
    Vector<GC::Root<Navigable>> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<GC::Root<Navigable>> navigables_to_check;
    navigables_to_check.append(const_cast<TraversableNavigable&>(*this));

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. If targetEntry's document is not navigable's document or targetEntry's document state's reload pending is true, then append navigable to results.
        // NOTE: Although navigable's active history entry can change synchronously, the new entry will always have the same Document,
        //       so accessing navigable's document is reliable.
        if (target_entry->document_state()->document_id() != navigable->active_document_id() || target_entry->document_state()->reload_pending()) {
            results.append(navigable);
        }

        // 3. Otherwise, extend navigablesToCheck with navigable's child navigables.
        //    Adding child navigables to navigablesToCheck means those navigables will also be checked by this loop.
        //    Child navigables are only checked if the navigable's active document will not change as part of this traversal.
        else {
            navigables_to_check.extend(navigable->child_navigables());
        }
    }

    // 4. Return results.
    return results;
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
    GC::Ptr<Navigable> navigable;
    bool update_only = false;

    GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> population_output;
    GC::Ptr<DOM::Document> resolved_document;
    Optional<URL::Origin> old_origin;

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(displayed_document);
        visitor.visit(navigable);
        visitor.visit(population_output);
        visitor.visit(resolved_document);
    }
};

GC_DEFINE_ALLOCATOR(ChangingNavigableContinuationState);

class ApplyHistoryStepState : public GC::Cell {
    GC_CELL(ApplyHistoryStepState, GC::Cell);
    GC_DECLARE_ALLOCATOR(ApplyHistoryStepState);

public:
    static constexpr int TIMEOUT_MS = 15000;

    ApplyHistoryStepState(
        GC::Ref<TraversableNavigable> traversable, int step, int target_step,
        GC::Ptr<SourceSnapshotParams> source_snapshot_params,
        UserNavigationInvolvement user_involvement,
        Optional<Bindings::NavigationType> navigation_type,
        TraversableNavigable::SynchronousNavigation synchronous_navigation,
        GC::Ptr<DOM::Document> pending_document,
        GC::Ref<OnApplyHistoryStepComplete> on_complete)
        : m_traversable(traversable)
        , m_step(step)
        , m_target_step(target_step)
        , m_source_snapshot_params(source_snapshot_params)
        , m_user_involvement(user_involvement)
        , m_navigation_type(navigation_type)
        , m_synchronous_navigation(synchronous_navigation)
        , m_pending_document(pending_document)
        , m_on_complete(on_complete)
        , m_timeout(Platform::Timer::create_single_shot(heap(), TIMEOUT_MS, GC::create_function(heap(), [this] {
            if (m_phase != Phase::Completed) {
                dbgln("FIXME: ApplyHistoryStepState timed out in phase {} step={} changing={}/{} completed={}/{} cont={}/{} non_changing={}/{} url={}",
                    to_underlying(m_phase), m_step,
                    m_changing_navigables.size(), m_changing_navigables.size(),
                    m_completed_change_jobs, m_changing_navigables.size(),
                    m_continuation_index, m_continuations.size(),
                    m_completed_non_changing_jobs, m_non_changing_navigables.size(),
                    m_traversable->active_document() ? m_traversable->active_document()->url() : URL::URL {});
            }
        })))
    {
        m_timeout->start();
    }

    void start();

    void did_receive_continuation(GC::Ref<ChangingNavigableContinuationState> continuation)
    {
        m_continuations.append(continuation);
        signal_progress();
    }

    void signal_progress()
    {
        switch (m_phase) {
        case Phase::WaitingForDocumentPopulation:
            // Population progress is tracked by m_continuations.size() + m_completed_change_jobs.
            // The caller either appended a continuation or incremented m_completed_change_jobs before calling.
            break;
        case Phase::ProcessingContinuations:
        case Phase::WaitingForChangeJobCompletion:
            ++m_completed_change_jobs;
            break;
        case Phase::WaitingForNonChangingJobs:
            ++m_completed_non_changing_jobs;
            break;
        case Phase::Completed:
            return;
        }
        try_advance();
    }

    enum class Phase {
        WaitingForDocumentPopulation,
        ProcessingContinuations,
        WaitingForChangeJobCompletion,
        WaitingForNonChangingJobs,
        Completed,
    };

private:
    void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_traversable);
        visitor.visit(m_source_snapshot_params);
        visitor.visit(m_pending_document);
        visitor.visit(m_on_complete);
        visitor.visit(m_timeout);
        visitor.visit(m_changing_navigables);
        visitor.visit(m_non_changing_navigables);
        visitor.visit(m_continuations);
        for (auto& navigable : m_navigables_that_must_wait_before_handling_sync_navigation)
            visitor.visit(navigable);
    }

    void try_advance()
    {
        switch (m_phase) {
        case Phase::WaitingForDocumentPopulation:
            if (m_continuations.size() + m_completed_change_jobs == m_changing_navigables.size()) {
                m_phase = Phase::ProcessingContinuations;
                process_continuations();
            }
            break;
        case Phase::ProcessingContinuations:
        case Phase::WaitingForChangeJobCompletion:
            if (m_completed_change_jobs == m_changing_navigables.size() && m_continuation_index >= m_continuations.size()) {
                m_phase = Phase::WaitingForNonChangingJobs;
                enter_waiting_for_non_changing_jobs();
            }
            break;
        case Phase::WaitingForNonChangingJobs:
            if (m_completed_non_changing_jobs == m_non_changing_navigables.size())
                complete();
            break;
        case Phase::Completed:
            break;
        }
    }

    void process_continuations();
    void enter_waiting_for_non_changing_jobs();
    void complete();

    Phase m_phase { Phase::WaitingForDocumentPopulation };
    GC::Ref<TraversableNavigable> m_traversable;
    int m_step;
    int m_target_step;
    GC::Ptr<SourceSnapshotParams> m_source_snapshot_params;
    UserNavigationInvolvement m_user_involvement;
    Optional<Bindings::NavigationType> m_navigation_type;
    TraversableNavigable::SynchronousNavigation m_synchronous_navigation;
    GC::Ptr<DOM::Document> m_pending_document;
    GC::Ptr<OnApplyHistoryStepComplete> m_on_complete;
    GC::Ref<Platform::Timer> m_timeout;

    Vector<GC::Ref<Navigable>> m_changing_navigables;
    Vector<GC::Ref<Navigable>> m_non_changing_navigables;

    size_t m_completed_change_jobs { 0 };
    Vector<GC::Ref<ChangingNavigableContinuationState>> m_continuations;
    size_t m_continuation_index { 0 };

    RefPtr<Core::Promise<Empty>> m_pending_sync_nav_promise;
    HashTable<GC::Ref<Navigable>> m_navigables_that_must_wait_before_handling_sync_navigation;

    size_t m_completed_non_changing_jobs { 0 };
};

GC_DEFINE_ALLOCATOR(ApplyHistoryStepState);

void ApplyHistoryStepState::start()
{
    // 7. Let nonchangingNavigablesThatStillNeedUpdates be the result of getting all navigables that only need history object length/index update given traversable and targetStep.
    auto non_changing_navigables = m_traversable->get_all_navigables_that_only_need_history_object_length_index_update(m_target_step);
    for (auto& nav : non_changing_navigables)
        m_non_changing_navigables.append(*nav);

    // 8. For each navigable of changingNavigables:
    auto changing_navigables = m_traversable->get_all_navigables_whose_current_session_history_entry_will_change_or_reload(m_target_step);
    for (auto& navigable : changing_navigables) {
        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(m_target_step);

        // 2. Set navigable's current session history entry to targetEntry.
        navigable->set_current_session_history_entry(target_entry);

        // 3. Set navigable's ongoing navigation to "traversal".
        navigable->set_ongoing_navigation(HTML::Navigable::Traversal::Tag);

        m_changing_navigables.append(*navigable);
    }

    // 12. For each navigable of changingNavigables, queue a global task on the navigation and traversal task source.
    for (auto& navigable : m_changing_navigables) {
        // AD-HOC: If the navigable has been destroyed, or has no active window, skip it.
        //         We must increment completed_change_jobs here rather than relying on the queued
        //         task, because Document::destroy() removes tasks associated with a document from
        //         the task queue, which can cause those tasks to never run.
        if (navigable->has_been_destroyed() || !navigable->active_window()) {
            ++m_completed_change_jobs;
            signal_progress();
            continue;
        }
        queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), GC::create_function(heap(), [this, navigable] {
            // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
            if (navigable->has_been_destroyed()) {
                ++m_completed_change_jobs;
                signal_progress();
                return;
            }

            // 1. Let displayedEntry be navigable's active session history entry.
            auto displayed_entry = navigable->active_session_history_entry();

            // 2. Let targetEntry be navigable's current session history entry.
            auto target_entry = navigable->current_session_history_entry();

            // 3. Let changingNavigableContinuation be a changing navigable continuation state with:
            auto changing_navigable_continuation = heap().allocate<ChangingNavigableContinuationState>();
            changing_navigable_continuation->displayed_document = navigable->active_document();
            changing_navigable_continuation->displayed_document_id = navigable->active_document_id();
            changing_navigable_continuation->target_entry = target_entry;
            changing_navigable_continuation->navigable = navigable;
            changing_navigable_continuation->update_only = false;
            changing_navigable_continuation->population_output = nullptr;

            // 4. If displayedEntry is targetEntry and targetEntry's document state's reload pending is false, then:
            if (m_synchronous_navigation == TraversableNavigable::SynchronousNavigation::Yes && !target_entry->document_state()->reload_pending()) {
                // 1. Set changingNavigableContinuation's update-only to true.
                changing_navigable_continuation->update_only = true;
                changing_navigable_continuation->resolved_document = navigable->active_document();

                // 2. Enqueue changingNavigableContinuation on changingNavigableContinuations.
                did_receive_continuation(changing_navigable_continuation);

                // 3. Abort these steps.
                return;
            }

            // 5. Switch on navigationType:
            if (m_navigation_type.has_value()) {
                switch (m_navigation_type.value()) {
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
                    VERIFY(target_entry->step() == displayed_entry->step());
                    break;
                case Bindings::NavigationType::Push:
                    // FIXME: Add ever populated check, and fix the bug where top level traversable's step is not updated when a child navigable navigates
                    // - "push": Assert: targetEntry's step is displayedEntry's step + 1 and targetEntry's document state's ever populated is false.
                    VERIFY(target_entry != displayed_entry);
                    VERIFY(target_entry->step().get<int>() > displayed_entry->step().get<int>());
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
                auto navigation = m_traversable->active_window()->navigation();

                // 2. Fire a traverse navigate event at navigation given targetEntry and userInvolvement.
                navigation->fire_a_traverse_navigate_event(*target_entry, m_user_involvement);
            }

            auto after_document_populated = GC::create_function(heap(), [this, old_origin, changing_navigable_continuation, target_entry, navigable](GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> output) mutable {
                changing_navigable_continuation->population_output = output;
                changing_navigable_continuation->old_origin = old_origin;

                // Compute the resolved document: pending_document (from finalize path),
                // population output (from traversal path), or active document (same-document).
                GC::Ptr<DOM::Document> resolved_document;
                if (m_pending_document)
                    resolved_document = m_pending_document;
                else if (output && output->document)
                    resolved_document = output->document;
                else
                    resolved_document = navigable->active_document();
                changing_navigable_continuation->resolved_document = resolved_document;

                // 1. If targetEntry's document is null, then set changingNavigableContinuation's update-only to true.
                bool has_fresh_document = m_pending_document || (output && output->document);
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
                did_receive_continuation(changing_navigable_continuation);
            });

            // 8. If targetEntry's document is null, or targetEntry's document state's reload pending is true, then:
            bool needs_population = !m_pending_document
                && (target_entry->document_state()->document_id() != navigable->active_document_id()
                    || target_entry->document_state()->reload_pending());
            if (needs_population) {
                // FIXME: 1. Let navTimingType be "back_forward" if targetEntry's document is null; otherwise "reload".

                // 2. Let targetSnapshotParams be the result of snapshotting target snapshot params given navigable.
                auto target_snapshot_params = navigable->snapshot_target_snapshot_params();

                // 3. Let potentiallyTargetSpecificSourceSnapshotParams be sourceSnapshotParams.
                auto potentially_target_specific_source_snapshot_params = m_source_snapshot_params;

                // 4. If potentiallyTargetSpecificSourceSnapshotParams is null, then set it to the result of snapshotting source snapshot params given navigable's active document.
                if (!potentially_target_specific_source_snapshot_params)
                    potentially_target_specific_source_snapshot_params = navigable->active_document()->snapshot_source_snapshot_params();

                // 5. Set targetEntry's document state's reload pending to false.
                target_entry->document_state()->set_reload_pending(false);

                // 6. Let allowPOST be targetEntry's document state's reload pending.
                auto allow_POST = target_entry->document_state()->reload_pending();

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
                Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [input_url = move(input_url), input_document_resource = move(input_document_resource), input_request_referrer = move(input_request_referrer), input_request_referrer_policy, input_initiator_origin = move(input_initiator_origin), input_origin = move(input_origin), input_history_policy_container = move(input_history_policy_container), input_about_base_url = move(input_about_base_url), input_navigable_target_name = move(input_navigable_target_name), input_ever_populated, potentially_target_specific_source_snapshot_params, target_snapshot_params, this, allow_POST, navigable, after_document_populated, user_involvement = m_user_involvement] {
                    navigable->populate_session_history_entry_document(
                        move(input_url), move(input_document_resource), move(input_request_referrer),
                        input_request_referrer_policy, move(input_initiator_origin), move(input_origin),
                        input_history_policy_container, move(input_about_base_url), move(input_navigable_target_name),
                        false, input_ever_populated,
                        *potentially_target_specific_source_snapshot_params, target_snapshot_params,
                        user_involvement, {}, Navigable::NullOrError {},
                        ContentSecurityPolicy::Directives::Directive::NavigationType::Other, allow_POST,
                        GC::create_function(this->heap(), [this, after_document_populated](GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> output) {
                            VERIFY(m_traversable->active_window());
                            queue_global_task(Task::Source::NavigationAndTraversal, *m_traversable->active_window(), GC::create_function(heap(), [after_document_populated, output]() {
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
    }

    try_advance();
}

void ApplyHistoryStepState::process_continuations()
{
    for (;;) {
        // NOTE: Synchronous navigations that are intended to take place before this traversal jump the queue at this point,
        //       so they can be added to the correct place in traversable's session history entries before this traversal
        //       potentially unloads their document. More details can be found here (https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigation-steps-queue-jumping-examples)
        // 1. If traversable's running nested apply history step is false, then:
        if (!m_traversable->m_paused_apply_history_step_state) {
            // 1. While traversable's session history traversal queue's algorithm set contains one or more synchronous
            //    navigation steps with a target navigable not contained in navigablesThatMustWaitBeforeHandlingSyncNavigation:
            //   1. Let steps be the first item in traversable's session history traversal queue's algorithm set
            //    that is synchronous navigation steps with a target navigable not contained in navigablesThatMustWaitBeforeHandlingSyncNavigation.
            //   2. Remove steps from traversable's session history traversal queue's algorithm set.
            while (true) {
                auto entry = m_traversable->m_session_history_traversal_queue->first_synchronous_navigation_steps_with_target_navigable_not_contained_in(m_navigables_that_must_wait_before_handling_sync_navigation);
                if (!entry)
                    break;

                VERIFY(!m_traversable->m_paused_apply_history_step_state);
                m_traversable->m_paused_apply_history_step_state = this;

                // 4. Run steps.
                auto promise = Core::Promise<Empty>::construct();
                entry->execute_steps(promise);

                // GC safety: `this` is kept alive by m_paused_apply_history_step_state (visited).
                // The promise is kept alive by m_pending_sync_nav_promise (RefPtr).
                VERIFY(!m_pending_sync_nav_promise);
                m_pending_sync_nav_promise = promise;
                promise->when_resolved([this](Empty) {
                    // 5. Set traversable's running nested apply history step to false.
                    VERIFY(m_pending_sync_nav_promise);
                    m_pending_sync_nav_promise = nullptr;
                    m_traversable->m_apply_history_step_state = this;
                    m_traversable->m_paused_apply_history_step_state = nullptr;
                    process_continuations();
                });
                return;
            }
        }

        if (m_continuation_index == m_continuations.size()) {
            if (m_phase == Phase::ProcessingContinuations) {
                m_phase = Phase::WaitingForChangeJobCompletion;
                try_advance();
            }
            return;
        }

        // 3. If changingNavigableContinuation is nothing, then continue.

        auto continuation = m_continuations[m_continuation_index++];

        // 4. Let displayedDocument be changingNavigableContinuation's displayed document.
        auto displayed_document = continuation->displayed_document;

        // 5. Let targetEntry be changingNavigableContinuation's target entry.
        auto population_output = continuation->population_output;
        auto old_origin = continuation->old_origin;

        // 6. Let navigable be changingNavigableContinuation's navigable.
        auto navigable = continuation->navigable;

        // AD-HOC: We should not continue navigation if navigable has been destroyed.
        if (navigable->has_been_destroyed()) {
            signal_progress();
            continue;
        }
        // AD-HOC: The displayed document may have been destroyed during the nested step execution above.
        if (!displayed_document->navigable()) {
            signal_progress();
            continue;
        }

        // AD-HOC: We re-compute targetStep here, since it might have changed since the last time we computed it.
        //         This can happen if navigables are destroyed while we wait for tasks to complete.
        m_target_step = m_traversable->get_the_used_step(m_step);

        // 7. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index given traversable and targetStep.
        auto history_object_length_and_index = m_traversable->get_the_history_object_length_and_index(m_target_step);
        auto script_history_length = history_object_length_and_index.script_history_length;
        auto script_history_index = history_object_length_and_index.script_history_index;

        // 8. Append navigable to navigablesThatMustWaitBeforeHandlingSyncNavigation.
        m_navigables_that_must_wait_before_handling_sync_navigation.set(*navigable);

        // 9. Let entriesForNavigationAPI be the result of getting session history entries for the navigation API given navigable and targetStep.
        auto entries_for_navigation_api = m_traversable->get_session_history_entries_for_the_navigation_api(*navigable, m_target_step);

        // NOTE: Steps 10 and 11 come after step 12.

        // 12. In both cases, let afterPotentialUnloads be the following steps:
        bool const update_only = continuation->update_only;
        RefPtr<SessionHistoryEntry> const target_entry = continuation->target_entry;
        auto const displayed_document_id = continuation->displayed_document_id;
        auto after_potential_unload = GC::create_function(heap(), [this, navigable, update_only, target_entry, continuation, population_output, old_origin, displayed_document_id, script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), navigation_type = m_navigation_type] {
            if (population_output)
                population_output->apply_to(*target_entry);

            // Post-population adjustments — only run when a fresh document was produced
            // (not for 204/205 no-document outcomes where resolved_document is the old active document).
            bool has_fresh_document = m_pending_document || (population_output && population_output->document);
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
                    target_entry->document_state()->set_navigable_target_name(String {});
                }
            }

            // 1. Let previousEntry be navigable's active session history entry.
            auto previous_entry = navigable->active_session_history_entry();

            // 2. If changingNavigableContinuation's update-only is false, then activate history entry targetEntry for navigable.
            auto resolved_document = continuation->resolved_document;
            if (!update_only)
                navigable->activate_history_entry(*target_entry, *resolved_document);

            // 3. Let updateDocument be an algorithm step which performs update document for history step application given
            //    targetEntry's document, targetEntry, changingNavigableContinuation's update-only, scriptHistoryLength,
            //    scriptHistoryIndex, navigationType, entriesForNavigationAPI, and previousEntry.
            auto update_document = [script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), target_entry, update_only, navigation_type, previous_entry, resolved_document] {
                resolved_document->update_for_history_step_application(*target_entry, update_only, script_history_length, script_history_index, navigation_type, entries_for_navigation_api, previous_entry, navigation_type.has_value());
            };

            // 4. If targetEntry's document is equal to displayedDocument, then perform updateDocument.
            // NOTE: We compare against the pre-activation displayed_document_id (not the current
            //       active entry) because activate_history_entry() has already updated the active entry above.
            if (target_entry->document_state()->document_id() == displayed_document_id) {
                update_document();
            }
            // 5. Otherwise, queue a global task on the navigation and traversal task source given targetEntry's document's relevant global object to perform updateDocument
            else {
                queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*resolved_document), GC::create_function(heap(), move(update_document)));
            }

            // 6. Increment completedChangeJobs.
            signal_progress();
        });

        // 10. If changingNavigableContinuation's update-only is true, or targetEntry's document is displayedDocument, then:
        if (continuation->update_only || continuation->resolved_document.ptr() == displayed_document.ptr()) {
            // 1. Set the ongoing navigation for navigable to null.
            navigable->set_ongoing_navigation({});

            // 2. Queue a global task on the navigation and traversal task source given navigable's active window to perform afterPotentialUnloads.
            queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), after_potential_unload);
        }
        // 11. Otherwise:
        else {
            // 1. Assert: navigationType is not null.
            VERIFY(m_navigation_type.has_value());

            // 2. Deactivate displayedDocument, given userInvolvement, targetEntry, navigationType, and afterPotentialUnloads.
            deactivate_a_document_for_cross_document_navigation(*displayed_document, m_user_involvement, *target_entry, continuation->resolved_document, after_potential_unload);
        }
    }
}

void ApplyHistoryStepState::enter_waiting_for_non_changing_jobs()
{
    // AD-HOC: We re-compute targetStep here, since it might have changed since the last time we computed it.
    //         This can happen if navigables are destroyed while we wait for tasks to complete.
    m_target_step = m_traversable->get_the_used_step(m_step);

    // 17. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index given traversable and targetStep.
    auto length_and_index = m_traversable->get_the_history_object_length_and_index(m_target_step);
    auto script_history_length = length_and_index.script_history_length;
    auto script_history_index = length_and_index.script_history_index;

    // 18. For each navigable of nonchangingNavigablesThatStillNeedUpdates, queue a global task on the navigation and traversal task source given navigable's active window to run the steps:
    for (auto& navigable : m_non_changing_navigables) {
        // AD-HOC: This check is not in the spec but we should not continue navigation if navigable has been destroyed,
        //         or if there's no active window.
        if (navigable->has_been_destroyed() || !navigable->active_window()) {
            ++m_completed_non_changing_jobs;
            continue;
        }
        // AD-HOC: Queue with null document instead of using queue_global_task.
        //         Tasks associated with a document are only runnable when fully active.
        //         In the async state machine, documents can become non-fully-active between
        //         queue time and execution, causing the task to be permanently stuck.
        //         A null-document task is always runnable; we check validity inside.
        queue_a_task(Task::Source::NavigationAndTraversal, nullptr, nullptr, GC::create_function(heap(), [this, navigable, script_history_length, script_history_index] {
            if (navigable->has_been_destroyed() || !navigable->active_window() || !navigable->active_document()->is_fully_active()) {
                signal_progress();
                return;
            }

            // 1. Let document be navigable's active document.
            auto document = navigable->active_document();

            // 2. Set document's history object's index to scriptHistoryIndex.
            document->history()->m_index = script_history_index;

            // 3. Set document's history object's length to scriptHistoryLength.
            document->history()->m_length = script_history_length;

            // 4. Increment completedNonchangingJobs.
            signal_progress();
        }));
    }

    try_advance();
}

void ApplyHistoryStepState::complete()
{
    if (m_phase == Phase::Completed)
        return;
    m_phase = Phase::Completed;
    m_timeout->stop();

    // 20. Set traversable's current session history step to targetStep.
    m_traversable->m_current_session_history_step = m_target_step;

    // Not in the spec:
    auto back_enabled = m_traversable->m_current_session_history_step > 0;
    VERIFY(m_traversable->m_session_history_entries.size() > 0);
    auto forward_enabled = m_traversable->can_go_forward();
    m_traversable->page().client().page_did_update_navigation_buttons_state(back_enabled, forward_enabled);
    m_traversable->page().client().page_did_change_url(m_traversable->current_session_history_entry()->url());

    // Clear state BEFORE on_complete, because on_complete may resolve a promise
    // that triggers the next session history traversal queue entry.
    // For nested states, the outer state is restored by the when_resolved callback
    // on the sync nav step's promise in process_continuations().
    m_traversable->m_apply_history_step_state = nullptr;

    // 21. Return "applied".
    if (m_on_complete)
        m_on_complete->function()(HistoryStepResult::Applied);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
void TraversableNavigable::apply_the_history_step(
    int step,
    bool check_for_cancelation,
    GC::Ptr<SourceSnapshotParams> source_snapshot_params,
    GC::Ptr<Navigable> initiator_to_check,
    UserNavigationInvolvement user_involvement,
    Optional<Bindings::NavigationType> navigation_type,
    SynchronousNavigation synchronous_navigation,
    GC::Ptr<DOM::Document> pending_document,
    GC::Ref<OnApplyHistoryStepComplete> on_complete)
{
    // FIXME: 1. Assert: This is running within traversable's session history traversal queue.

    VERIFY(!m_apply_history_step_state || m_paused_apply_history_step_state);

    // 2. Let targetStep be the result of getting the used step given traversable and step.
    auto target_step = get_the_used_step(step);

    // 3. If initiatorToCheck is not null, then:
    if (initiator_to_check != nullptr) {
        // 1. Assert: sourceSnapshotParams is not null.
        VERIFY(source_snapshot_params);

        // 2. For each navigable of get all navigables whose current session history entry will change or reload:
        //    if initiatorToCheck is not allowed by sandboxing to navigate navigable given sourceSnapshotParams, then return "initiator-disallowed".
        for (auto const& navigable : get_all_navigables_whose_current_session_history_entry_will_change_or_reload(target_step)) {
            if (!initiator_to_check->allowed_by_sandboxing_to_navigate(*navigable, *source_snapshot_params)) {
                on_complete->function()(HistoryStepResult::InitiatorDisallowed);
                return;
            }
        }
    }

    // 4. Let navigablesCrossingDocuments be the result of getting all navigables that might experience a cross-document traversal given traversable and targetStep.
    auto navigables_crossing_documents = get_all_navigables_that_might_experience_a_cross_document_traversal(target_step);

    // 5. If checkForCancelation is true, and the result of checking if unloading is canceled given navigablesCrossingDocuments, traversable, targetStep,
    //    and userInvolvement is not "continue", then return that result.
    if (check_for_cancelation) {
        check_if_unloading_is_canceled(navigables_crossing_documents, *this, target_step, user_involvement,
            GC::create_function(heap(), [this, step, target_step, source_snapshot_params, user_involvement, navigation_type, synchronous_navigation, pending_document, on_complete](CheckIfUnloadingIsCanceledResult result) mutable {
                if (result == CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload) {
                    on_complete->function()(HistoryStepResult::CanceledByBeforeUnload);
                    return;
                }
                if (result == CheckIfUnloadingIsCanceledResult::CanceledByNavigate) {
                    on_complete->function()(HistoryStepResult::CanceledByNavigate);
                    return;
                }
                apply_the_history_step_after_unload_check(step, target_step, source_snapshot_params, user_involvement, navigation_type, synchronous_navigation, pending_document, on_complete);
            }));
        return;
    }

    // 6. Let changingNavigables be the result of get all navigables whose current session history entry will change or reload given traversable and targetStep.
    apply_the_history_step_after_unload_check(step, target_step, source_snapshot_params, user_involvement, navigation_type, synchronous_navigation, pending_document, on_complete);
}

void TraversableNavigable::apply_the_history_step_after_unload_check(
    int step,
    int target_step,
    GC::Ptr<SourceSnapshotParams> source_snapshot_params,
    UserNavigationInvolvement user_involvement,
    Optional<Bindings::NavigationType> navigation_type,
    SynchronousNavigation synchronous_navigation,
    GC::Ptr<DOM::Document> pending_document,
    GC::Ref<GC::Function<void(HistoryStepResult)>> on_complete)
{
    auto state = heap().allocate<ApplyHistoryStepState>(*this, step, target_step, source_snapshot_params,
        user_involvement, navigation_type, synchronous_navigation, pending_document, on_complete);

    VERIFY(!m_apply_history_step_state || m_paused_apply_history_step_state);
    m_apply_history_step_state = state;

    state->start();
}

class CheckUnloadingCanceledState : public GC::Cell {
    GC_CELL(CheckUnloadingCanceledState, GC::Cell);
    GC_DECLARE_ALLOCATOR(CheckUnloadingCanceledState);

public:
    using Result = TraversableNavigable::CheckIfUnloadingIsCanceledResult;
    static constexpr int TIMEOUT_MS = 15000;

    CheckUnloadingCanceledState(
        GC::Ptr<TraversableNavigable> traversable,
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
    void start(Vector<GC::Root<Navigable>> const& navigables_that_need_before_unload, Optional<int> target_step)
    {
        // 1. Let documentsToFireBeforeunload be the active document of each item in navigablesThatNeedBeforeUnload.
        for (auto& navigable : navigables_that_need_before_unload)
            m_phase2_documents.append(*navigable->active_document());

        // 2. Let unloadPromptShown be false.

        // 3. Let finalStatus be "continue".

        // 4. If traversable was given, then:
        if (m_traversable) {
            // 1. Assert: targetStep and userInvolvementForNavigateEvent were given.
            // NOTE: This assertion is enforced by the caller.

            // 2. Let targetEntry be the result of getting the target history entry given traversable and targetStep.
            m_target_entry = m_traversable->get_the_target_history_entry(target_step.value());

            // 3. If targetEntry is not traversable's current session history entry, and targetEntry's document state's origin is not the same as
            //    traversable's current session history entry's document state's origin, then:
            if (m_target_entry != m_traversable->current_session_history_entry() && m_target_entry->document_state()->origin() != m_traversable->current_session_history_entry()->document_state()->origin()) {

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
        queue_global_task(Task::Source::NavigationAndTraversal, *m_traversable->active_window(), GC::create_function(heap(), [this] {
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
            queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*document), GC::create_function(heap(), [this, document] {
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
    GC::Ptr<TraversableNavigable> m_traversable;
    RefPtr<SessionHistoryEntry> m_target_entry;
    Optional<UserNavigationInvolvement> m_user_involvement;
    GC::Ref<GC::Function<void(Result)>> m_callback;
    GC::Ref<Platform::Timer> m_timeout;
};

GC_DEFINE_ALLOCATOR(CheckUnloadingCanceledState);

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
void TraversableNavigable::check_if_unloading_is_canceled(
    Vector<GC::Root<Navigable>> navigables_that_need_before_unload,
    GC::Ptr<TraversableNavigable> traversable,
    Optional<int> target_step,
    Optional<UserNavigationInvolvement> user_involvement_for_navigate_events,
    GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback)
{
    auto state = heap().allocate<CheckUnloadingCanceledState>(
        traversable,
        user_involvement_for_navigate_events,
        callback);
    state->start(navigables_that_need_before_unload, target_step);
}

void TraversableNavigable::check_if_unloading_is_canceled(Vector<GC::Root<Navigable>> navigables_that_need_before_unload, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback)
{
    check_if_unloading_is_canceled(move(navigables_that_need_before_unload), {}, {}, {}, callback);
}

Vector<NonnullRefPtr<SessionHistoryEntry>> TraversableNavigable::get_session_history_entries_for_the_navigation_api(GC::Ref<Navigable> navigable, int target_step)
{
    // 1. Let rawEntries be the result of getting session history entries for navigable.
    auto raw_entries = navigable->get_session_history_entries();

    if (raw_entries.is_empty())
        return {};

    // 2. Let entriesForNavigationAPI be a new empty list.
    Vector<NonnullRefPtr<SessionHistoryEntry>> entries_for_navigation_api;

    // 3. Let startingIndex be the index of the session history entry in rawEntries who has the greatest step less than or equal to targetStep.
    // FIXME: Use min/max_element algorithm or some such here
    int starting_index = 0;
    auto max_step = 0;
    for (auto i = 0u; i < raw_entries.size(); ++i) {
        auto const& entry = raw_entries[i];
        if (entry->step().has<int>()) {
            auto step = entry->step().get<int>();
            if (step <= target_step && step > max_step) {
                starting_index = static_cast<int>(i);
            }
        }
    }

    // 4. Append rawEntries[startingIndex] to entriesForNavigationAPI.
    entries_for_navigation_api.append(raw_entries[starting_index]);

    // 5. Let startingOrigin be rawEntries[startingIndex]'s document state's origin.
    auto starting_origin = raw_entries[starting_index]->document_state()->origin();

    // 6. Let i be startingIndex − 1.
    auto i = starting_index - 1;

    // 7. While i > 0:
    while (i > 0) {
        auto& entry = raw_entries[static_cast<unsigned>(i)];
        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto entry_origin = entry->document_state()->origin();
        if (starting_origin.has_value() && entry_origin.has_value() && !entry_origin->is_same_origin(*starting_origin))
            break;

        // 2. Prepend rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.prepend(entry);

        // 3. Set i to i − 1.
        --i;
    }

    // 8. Set i to startingIndex + 1.
    i = starting_index + 1;

    // 9. While i < rawEntries's size:
    while (i < static_cast<int>(raw_entries.size())) {
        auto& entry = raw_entries[static_cast<unsigned>(i)];
        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto entry_origin = entry->document_state()->origin();
        if (starting_origin.has_value() && entry_origin.has_value() && !entry_origin->is_same_origin(*starting_origin))
            break;

        // 2. Append rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.append(entry);

        // 3. Set i to i + 1.
        ++i;
    }

    // 10. Return entriesForNavigationAPI.
    return entries_for_navigation_api;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#clear-the-forward-session-history
void TraversableNavigable::clear_the_forward_session_history()
{
    // FIXME: 1. Assert: this is running within navigable's session history traversal queue.

    // 2. Let step be the navigable's current session history step.
    auto step = current_session_history_step();

    // 3. Let entryLists be the ordered set « navigable's session history entries ».
    Vector<Vector<NonnullRefPtr<SessionHistoryEntry>>&> entry_lists;
    entry_lists.append(session_history_entries());

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto& entry_list = entry_lists.take_first();

        // 1. Remove every session history entry from entryList that has a step greater than step.
        entry_list.remove_all_matching([step](auto& entry) {
            return entry->step().template get<int>() > step;
        });

        // 2. For each entry of entryList:
        for (auto& entry : entry_list) {
            // 1. For each nestedHistory of entry's document state's nested histories, append nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry->document_state()->nested_histories()) {
                entry_lists.append(nested_history.entries);
            }
        }
    }
}

bool TraversableNavigable::can_go_forward() const
{
    auto step = current_session_history_step();

    Vector<Vector<NonnullRefPtr<SessionHistoryEntry>> const&> entry_lists;
    entry_lists.append(session_history_entries());

    while (!entry_lists.is_empty()) {
        auto const& entry_list = entry_lists.take_first();

        for (auto const& entry : entry_list) {
            if (entry->step().template get<int>() > step)
                return true;

            for (auto& nested_history : entry->document_state()->nested_histories())
                entry_lists.append(nested_history.entries);
        }
    }

    return false;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void TraversableNavigable::traverse_the_history_by_delta(int delta, GC::Ptr<DOM::Document> source_document)
{
    // 1. Let sourceSnapshotParams and initiatorToCheck be null.
    GC::Ptr<SourceSnapshotParams> source_snapshot_params = nullptr;
    GC::Ptr<Navigable> initiator_to_check = nullptr;

    // 2. Let userInvolvement be "browser UI".
    UserNavigationInvolvement user_involvement = UserNavigationInvolvement::BrowserUI;

    // 1. If sourceDocument is given, then:
    if (source_document) {
        // 1. Set sourceSnapshotParams to the result of snapshotting source snapshot params given sourceDocument.
        source_snapshot_params = source_document->snapshot_source_snapshot_params();

        // 2. Set initiatorToCheck to sourceDocument's node navigable.
        initiator_to_check = source_document->navigable();

        // 3. Set userInvolvement to "none".
        user_involvement = UserNavigationInvolvement::None;
    }

    // 4. Append the following session history traversal steps to traversable:
    append_session_history_traversal_steps(GC::create_function(heap(), [this, delta, source_snapshot_params, initiator_to_check, user_involvement](NonnullRefPtr<Core::Promise<Empty>> signal) {
        // 1. Let allSteps be the result of getting all used history steps for traversable.
        auto all_steps = get_all_used_history_steps();

        // 2. Let currentStepIndex be the index of traversable's current session history step within allSteps.
        auto current_step_index = *all_steps.find_first_index(current_session_history_step());

        // 3. Let targetStepIndex be currentStepIndex plus delta
        auto target_step_index = current_step_index + delta;

        // 4. If allSteps[targetStepIndex] does not exist, then abort these steps.
        if (target_step_index >= all_steps.size()) {
            signal->resolve({});
            return;
        }

        // 5. Apply the traverse history step allSteps[targetStepIndex] to traversable, given sourceSnapshotParams,
        //    initiatorToCheck, and userInvolvement.
        apply_the_traverse_history_step(all_steps[target_step_index], source_snapshot_params, initiator_to_check, user_involvement,
            GC::create_function(heap(), [signal](HistoryStepResult) {
                signal->resolve({});
            }));
    }));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#update-for-navigable-creation/destruction
void TraversableNavigable::update_for_navigable_creation_or_destruction(GC::Ref<OnApplyHistoryStepComplete> on_complete)
{
    // 1. Let step be traversable's current session history step.
    auto step = current_session_history_step();

    // 2. Return the result of applying the history step to traversable given false, null, null, null, and null.
    apply_the_history_step(step, false, {}, {}, UserNavigationInvolvement::None, {}, SynchronousNavigation::No, nullptr, on_complete);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-reload-history-step
void TraversableNavigable::apply_the_reload_history_step(UserNavigationInvolvement user_involvement, GC::Ref<GC::Function<void(HistoryStepResult)>> on_complete)
{
    // 1. Let step be traversable's current session history step.
    auto step = current_session_history_step();

    // 2. Return the result of applying the history step step to traversable given true, null, null, null, and "reload".
    apply_the_history_step(step, true, {}, {}, user_involvement, Bindings::NavigationType::Reload, SynchronousNavigation::No, nullptr, on_complete);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-push/replace-history-step
void TraversableNavigable::apply_the_push_or_replace_history_step(int step, HistoryHandlingBehavior history_handling, UserNavigationInvolvement user_involvement, SynchronousNavigation synchronous_navigation, GC::Ptr<DOM::Document> pending_document, GC::Ref<OnApplyHistoryStepComplete> on_complete)
{
    // 1. Return the result of applying the history step step to traversable given false, null, null, userInvolvement, and historyHandling.
    auto navigation_type = history_handling == HistoryHandlingBehavior::Replace ? Bindings::NavigationType::Replace : Bindings::NavigationType::Push;
    apply_the_history_step(step, false, {}, {}, user_involvement, navigation_type, synchronous_navigation, pending_document, on_complete);
}

void TraversableNavigable::apply_the_traverse_history_step(int step, GC::Ptr<SourceSnapshotParams> source_snapshot_params, GC::Ptr<Navigable> initiator_to_check, UserNavigationInvolvement user_involvement, GC::Ref<GC::Function<void(HistoryStepResult)>> on_complete)
{
    // 1. Return the result of applying the history step step to traversable given true, sourceSnapshotParams, initiatorToCheck, userInvolvement, and "traverse".
    apply_the_history_step(step, true, source_snapshot_params, initiator_to_check, user_involvement, Bindings::NavigationType::Traverse, SynchronousNavigation::No, nullptr, on_complete);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#close-a-top-level-traversable
void TraversableNavigable::close_top_level_traversable()
{
    // 1. If traversable's is closing is true, then return.
    if (is_closing())
        return;

    // AD-HOC: Set the is closing flag to prevent re-entrant calls from queuing duplicate session history steps.
    set_closing(true);

    // 2. Definitely close traversable.
    definitely_close_top_level_traversable();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#definitely-close-a-top-level-traversable
void TraversableNavigable::definitely_close_top_level_traversable()
{
    VERIFY(is_top_level_traversable());

    // 1. Let toUnload be traversable's active document's inclusive descendant navigables.
    auto to_unload = active_document()->inclusive_descendant_navigables();

    // 2. If the result of checking if unloading is canceled for toUnload is not "continue", then return.
    check_if_unloading_is_canceled(move(to_unload), GC::create_function(heap(), [this](CheckIfUnloadingIsCanceledResult result) {
        if (result != CheckIfUnloadingIsCanceledResult::Continue)
            return;

        // 3. Append the following session history traversal steps to traversable:
        append_session_history_traversal_steps(GC::create_function(heap(), [this](NonnullRefPtr<Core::Promise<Empty>> signal) {
            // 1. Let afterAllUnloads be an algorithm step which destroys traversable.
            auto after_all_unloads = GC::create_function(heap(), [this] {
                destroy_top_level_traversable();
            });

            // 2. Unload a document and its descendants given traversable's active document, null, and afterAllUnloads.
            active_document()->unload_a_document_and_its_descendants({}, after_all_unloads);
            signal->resolve({});
        }));
    }));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-top-level-traversable
void TraversableNavigable::destroy_top_level_traversable()
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
    remove_from_all_navigables();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation
void finalize_a_same_document_navigation(GC::Ref<TraversableNavigable> traversable, GC::Ref<Navigable> target_navigable, NonnullRefPtr<SessionHistoryEntry> target_entry, RefPtr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior history_handling, UserNavigationInvolvement user_involvement, GC::Ref<OnApplyHistoryStepComplete> on_complete)
{
    // NOTE: This is not in the spec but we should not navigate destroyed navigable.
    if (target_navigable->has_been_destroyed()) {
        on_complete->function()(HistoryStepResult::Applied);
        return;
    }

    // FIXME: 1. Assert: this is running on traversable's session history traversal queue.

    // 2. If targetNavigable's active session history entry is not targetEntry, then return.
    // FIXME: This is a workaround for a spec issue where the early return loses replace entries.
    //        Revisit when https://github.com/whatwg/html/issues/10232 is resolved.
    if (target_navigable->active_session_history_entry() != target_entry) {
        if (entry_to_replace) {
            auto& target_entries = target_navigable->get_session_history_entries();
            if (auto it = target_entries.find(*entry_to_replace); it != target_entries.end()) {
                target_entry->set_step(entry_to_replace->step());
                *it = target_entry;
            }
        }
        on_complete->function()(HistoryStepResult::Applied);
        return;
    }

    // 3. Let targetStep be null.
    Optional<int> target_step;

    // 4. Let targetEntries be the result of getting session history entries for targetNavigable.
    auto& target_entries = target_navigable->get_session_history_entries();

    // 5. If entryToReplace is null, then:
    // FIXME: Checking containment of entryToReplace should not be needed.
    //        For more details see https://github.com/whatwg/html/issues/10232#issuecomment-2037543137
    if (!entry_to_replace || !target_entries.contains_slow(NonnullRefPtr { *entry_to_replace })) {
        // 1. Clear the forward session history of traversable.
        traversable->clear_the_forward_session_history();

        // 2. Set targetStep to traversable's current session history step + 1.
        target_step = traversable->current_session_history_step() + 1;

        // 3. Set targetEntry's step to targetStep.
        target_entry->set_step(*target_step);

        // 4. Append targetEntry to targetEntries.
        target_entries.append(target_entry);
    } else {
        // 1. Replace entryToReplace with targetEntry in targetEntries.
        *(target_entries.find(*entry_to_replace)) = target_entry;

        // 2. Set targetEntry's step to entryToReplace's step.
        target_entry->set_step(entry_to_replace->step());

        // 3. Set targetStep to traversable's current session history step.
        target_step = traversable->current_session_history_step();
    }

    // 6. Apply the push/replace history step targetStep to traversable given historyHandling and userInvolvement.
    traversable->apply_the_push_or_replace_history_step(*target_step, history_handling, user_involvement, TraversableNavigable::SynchronousNavigation::Yes, nullptr, on_complete);
}

// https://html.spec.whatwg.org/multipage/interaction.html#system-visibility-state
void TraversableNavigable::set_system_visibility_state(VisibilityState visibility_state)
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
GC::Ptr<DOM::Node> TraversableNavigable::currently_focused_area()
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
        candidate = as<NavigableContainer>(*candidate->focused_area()).content_navigable()->active_document();
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
Geolocation::EmulatedPositionData const& TraversableNavigable::emulated_position_data() const
{
    VERIFY(is_top_level_traversable());
    return m_emulated_position_data;
}

// https://w3c.github.io/geolocation/#dfn-emulated-position-data
void TraversableNavigable::set_emulated_position_data(Geolocation::EmulatedPositionData data)
{
    VERIFY(is_top_level_traversable());
    m_emulated_position_data = data;
}

void TraversableNavigable::process_screenshot_requests()
{
    auto& client = page().client();
    while (!m_screenshot_tasks.is_empty()) {
        auto task = m_screenshot_tasks.dequeue();
        if (task.node_id.has_value()) {
            auto* dom_node = DOM::Node::from_unique_id(*task.node_id);
            if (dom_node)
                dom_node->document().update_layout(DOM::UpdateLayoutReason::ProcessScreenshot);
            if (!dom_node || !dom_node->paintable_box()) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto rect = page().enclosing_device_rect(dom_node->paintable_box()->absolute_border_box_rect());
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
            auto scrollable_overflow_rect = active_document()->layout_node()->paintable_box()->scrollable_overflow_rect();
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
