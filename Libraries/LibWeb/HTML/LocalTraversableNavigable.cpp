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
GC::Ref<LocalTraversableNavigable> LocalTraversableNavigable::create_a_new_top_level_traversable(GC::Ref<Page> page, GC::Ptr<HTML::BrowsingContext> opener, Utf16String target_name, Optional<CrossProcessId> initial_document_state_id, VisibilityState system_visibility_state)
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
    traversable->initialize_navigable(document_state, nullptr, *document, system_visibility_state);

    // 7. Let initialHistoryEntry be traversable's active session history entry.
    auto initial_history_entry = traversable->active_session_history_entry();
    VERIFY(initial_history_entry);

    // 8. Set initialHistoryEntry's step to 0.
    initial_history_entry->set_step(0);

    // 9. Append initialHistoryEntry to traversable's session history entries.
    // NB: The UI process keeps the canonical session history, so report the traversable's creation to it.
    traversable->set_has_session_history_entry_and_ready_for_navigation();
    Optional<CrossProcessId> opener_navigable_id;
    if (opener)
        opener_navigable_id = opener->active_document()->navigable()->id();
    page->client().page_did_create_top_level_traversable(traversable->id(), create_session_history_entry_descriptor(*initial_history_entry), opener_navigable_id);

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
GC::Ref<LocalTraversableNavigable> LocalTraversableNavigable::create_a_fresh_top_level_traversable(GC::Ref<Page> page, URL::URL const& initial_navigation_url, DocumentResource initial_navigation_post_resource, CrossProcessId initial_document_state_id, VisibilityState system_visibility_state)
{
    // 1. Let traversable be the result of creating a new top-level traversable given null and the empty string.
    auto traversable = create_a_new_top_level_traversable(page, nullptr, {}, initial_document_state_id, system_visibility_state);
    page->set_local_root_navigable(traversable);

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

class CheckUnloadingCanceledState : public GC::Cell {
    GC_CELL(CheckUnloadingCanceledState, GC::Cell);
    GC_DECLARE_ALLOCATOR(CheckUnloadingCanceledState);

public:
    using Result = LocalTraversableNavigable::CheckIfUnloadingIsCanceledResult;
    static constexpr int TIMEOUT_MS = 15000;

    CheckUnloadingCanceledState(
        GC::Ptr<LocalTraversableNavigable> traversable,
        Optional<UserNavigationInvolvement> user_involvement,
        UnloadPromptShown unload_prompt_shown,
        GC::Ref<GC::Function<void(Result, UnloadPromptShown)>> callback)
        : m_unload_prompt_shown(unload_prompt_shown)
        , m_traversable(traversable)
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
                    m_unload_prompt_shown = UnloadPromptShown::Yes;

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
                auto [unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document] = document->steps_to_fire_beforeunload(m_unload_prompt_shown == UnloadPromptShown::Yes);

                // 2. If unloadPromptShownForThisDocument is true, then set unloadPromptShown to true.
                if (unload_prompt_shown_for_this_document)
                    m_unload_prompt_shown = UnloadPromptShown::Yes;

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
        m_callback->function()(final_result, m_unload_prompt_shown);
    }

    Result m_final_status { Result::Continue };
    UnloadPromptShown m_unload_prompt_shown { UnloadPromptShown::No };
    bool m_completed { false };
    bool m_needs_beforeunload { false };
    size_t m_remaining_phase2_tasks { 0 };
    Vector<GC::Ref<DOM::Document>> m_phase2_documents;
    GC::Ptr<LocalTraversableNavigable> m_traversable;
    RefPtr<SessionHistoryEntry> m_target_entry;
    Optional<UserNavigationInvolvement> m_user_involvement;
    GC::Ref<GC::Function<void(Result, UnloadPromptShown)>> m_callback;
    GC::Ref<Platform::Timer> m_timeout;
};

GC_DEFINE_ALLOCATOR(CheckUnloadingCanceledState);

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
void LocalTraversableNavigable::check_if_unloading_is_canceled(
    Vector<GC::Root<LocalNavigable>> navigables_that_need_before_unload,
    GC::Ptr<LocalTraversableNavigable> traversable,
    RefPtr<SessionHistoryEntry> target_entry,
    Optional<UserNavigationInvolvement> user_involvement_for_navigate_events,
    UnloadPromptShown unload_prompt_shown,
    GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult, UnloadPromptShown)>> callback)
{
    auto state = heap().allocate<CheckUnloadingCanceledState>(
        traversable,
        user_involvement_for_navigate_events,
        unload_prompt_shown,
        callback);
    state->start(navigables_that_need_before_unload, move(target_entry));
}

void LocalTraversableNavigable::check_if_unloading_is_canceled(Vector<GC::Root<LocalNavigable>> navigables_that_need_before_unload, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback)
{
    check_if_unloading_is_canceled(move(navigables_that_need_before_unload), {}, {}, {}, UnloadPromptShown::No,
        GC::create_function(heap(), [callback](CheckIfUnloadingIsCanceledResult result, UnloadPromptShown) {
            callback->function()(result);
        }));
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
    page().history_executor().request_history_operation(
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
            .serialized_source_snapshot_params = source_snapshot_params ? Optional<NavigationSourceSnapshot> { create_navigation_source_snapshot(*source_snapshot_params) } : Optional<NavigationSourceSnapshot> {},
        });
}

void LocalTraversableNavigable::run_ui_history_step_unload_cancelation_job(CrossProcessId operation_id, SessionHistoryEntryDescriptor target_entry_descriptor, Vector<CrossProcessId> navigables_crossing_documents, UserNavigationInvolvement user_involvement, GC::Ref<GC::Function<void(HistoryStepResult, UnloadPromptShown)>> on_complete)
{
    (void)operation_id;

    auto target_entry = resolve_local_session_history_entry(move(target_entry_descriptor), PrepareChildHistoryReconstruction::No);
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
        on_complete->function()(HistoryStepResult::CanceledPendingNavigation, UnloadPromptShown::No);
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
    check_if_unloading_is_canceled(move(navigables), *this, move(target_entry), user_involvement, UnloadPromptShown::No,
        GC::create_function(heap(), [on_complete](CheckIfUnloadingIsCanceledResult result, UnloadPromptShown unload_prompt_shown) {
            switch (result) {
            case CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload:
                on_complete->function()(HistoryStepResult::CanceledByBeforeUnload, unload_prompt_shown);
                return;
            case CheckIfUnloadingIsCanceledResult::CanceledByNavigate:
                on_complete->function()(HistoryStepResult::CanceledByNavigate, unload_prompt_shown);
                return;
            case CheckIfUnloadingIsCanceledResult::Continue:
                on_complete->function()(HistoryStepResult::Applied, unload_prompt_shown);
                return;
            }
            VERIFY_NOT_REACHED();
        }));
}

// Fire beforeunload for the documents hosted by this process.
void LocalTraversableNavigable::run_ui_history_step_beforeunload_check(Vector<CrossProcessId> navigable_ids, UnloadPromptShown unload_prompt_shown, GC::Ref<GC::Function<void(HistoryStepResult, UnloadPromptShown)>> on_complete)
{
    Vector<GC::Root<LocalNavigable>> navigables;
    navigables.ensure_capacity(navigable_ids.size());
    for (auto navigable_id : navigable_ids) {
        if (auto navigable = local_navigable_with_id(navigable_id); navigable && !navigable->has_been_destroyed())
            navigables.append(*navigable);
    }
    check_if_unloading_is_canceled(move(navigables), {}, {}, {}, unload_prompt_shown,
        GC::create_function(heap(), [on_complete](CheckIfUnloadingIsCanceledResult result, UnloadPromptShown unload_prompt_shown) {
            on_complete->function()(
                result == CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload
                    ? HistoryStepResult::CanceledByBeforeUnload
                    : HistoryStepResult::Applied,
                unload_prompt_shown);
        }));
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

    page().history_executor().request_history_operation(
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
    // AD-HOC: A forced close must be able to supersede an in-progress prompted close.
    if (is_closing() && prompt_to_unload == PromptToUnload::Yes)
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
        if (m_close_steps_have_been_appended)
            return;
        m_close_steps_have_been_appended = true;

        // 3. Append the following session history traversal steps to traversable:
        page().history_executor().request_history_operation(
            CloseTopLevelTraversableHistoryOperationParameters { .traversable_id = id() },
            {
                .on_complete = GC::create_function(heap(), [this](HistoryStepResult result) {
                    // NB: An abandoned close never reached its queue position; do not destroy the traversable for it.
                    if (result != HistoryStepResult::Applied) {
                        m_close_steps_have_been_appended = false;
                        set_closing(false);
                        return;
                    }

                    // NB: The UI process runs the traversal steps' unload recursion and delivers the final
                    //     unload-and-destroy task through run_ui_traversable_close_unload_task before this
                    //     completion.
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
            if (!m_close_steps_have_been_appended)
                set_closing(false);
            return;
        }

        // 3. Append the following session history traversal steps to traversable:
        append_close_steps();
    }));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#definitely-close-a-top-level-traversable
void LocalTraversableNavigable::run_ui_traversable_close_unload_task()
{
    // The UI process has already unloaded every descendant subtree.
    queue_a_task(Task::Source::NavigationAndTraversal, nullptr, nullptr,
        GC::create_function(heap(), [this] {
            // 2. Unload document, passing along newDocument if it is not null.
            if (auto document = active_document())
                document->unload();

            // 3. If afterAllUnloads was given, then run it.
            // NB: afterAllUnloads is the close traversal steps' algorithm step which destroys traversable.
            destroy_top_level_traversable();
        }));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-top-level-traversable
void LocalTraversableNavigable::destroy_top_level_traversable()
{
    VERIFY(is_top_level_traversable());

    destroy_local_traversable();
}

// Perform the local teardown shared by top-level traversables and remote iframe page roots.
// A remote iframe page root is not a top-level traversable in the specification, so its discard path calls this
// helper directly instead of the spec-linked wrapper above.
void LocalTraversableNavigable::destroy_local_traversable()
{
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

}
