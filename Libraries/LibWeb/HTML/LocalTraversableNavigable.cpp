/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/HistoryExecutor.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

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
        if (auto navigable = local_navigable_with_id(navigable_id); navigable && !navigable->has_been_destroyed() && navigable->active_document())
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
            CloseTopLevelTraversableHistoryOperationParameters {},
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
