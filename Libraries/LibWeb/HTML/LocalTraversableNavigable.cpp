/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
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
#include <LibWeb/HTML/RemoteNavigable.h>
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
          Compositing::PagePresentationRegistration::Yes)
{
}

LocalTraversableNavigable::~LocalTraversableNavigable() = default;

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-browsing-context
BrowsingContextAndDocument create_a_new_top_level_browsing_context_and_document(GC::Ref<Page> page, GC::Ptr<WindowProxy> existing_window_proxy)
{
    // 1. Let group and document be the result of creating a new browsing context group and document.
    auto [group, document] = BrowsingContextGroup::create_a_new_browsing_context_group_and_document(page, existing_window_proxy);

    // 2. Return group's browsing context set[0] and document.
    return BrowsingContextAndDocument { **group->browsing_context_set().begin(), document };
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-traversable
GC::Ref<LocalTraversableNavigable> LocalTraversableNavigable::create_a_new_top_level_traversable(GC::Ref<Page> page, GC::Ptr<HTML::BrowsingContext> opener, Optional<SessionHistoryEntryDescriptor> initial_history_entry_from_owner, VisibilityState system_visibility_state)
{
    auto& vm = Bindings::main_thread_vm();
    page->ensure_compositor_host();

    // NB: A traversable with no owning process, such as an SVG image's, mints its own entry
    auto initial_entry = initial_history_entry_from_owner.has_value()
        ? initial_history_entry_from_owner.release_value()
        : create_initial_session_history_entry_descriptor(page->client().allocate_cross_process_id(),
              opener ? Optional<URL::URL> { opener->active_document()->base_url() } : Optional<URL::URL> {}, {});

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
    auto document_state = DocumentState::create(initial_entry.document_state.id);

    // document: document (now owned by LocalNavigable::m_active_document, not DocumentState)

    // initiator origin: null if opener is null; otherwise, document's origin
    document_state->set_initiator_origin(opener ? document->origin() : Optional<URL::Origin> {});

    // origin: document's origin
    document_state->set_origin(document->origin());

    // navigable target name: targetName
    document_state->set_navigable_target_name(initial_entry.document_state.navigable_target_name);

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

    // NB: The owner's copy of this entry and this one must be the same entry, so take its identity
    initial_history_entry->set_navigation_api_key(initial_entry.navigation_api_key);
    initial_history_entry->set_navigation_api_id(initial_entry.navigation_api_id);

    // 9. Append initialHistoryEntry to traversable's session history entries.
    // NB: A traversable's owner keeps the canonical session history; this entry is the owner's initial entry.
    traversable->set_has_session_history_entry_and_ready_for_navigation();

    // 10. If opener is non-null, then legacy-clone a traversable storage shed given opener's top-level traversable and traversable. [STORAGE]
    // NB: This is done by the canonical traversable.

    // 11. Append traversable to the user agent's top-level traversable set.
    // NB: The UI process holds the user agent's top-level traversable set.

    // 12. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-fresh-top-level-traversable
GC::Ref<LocalTraversableNavigable> LocalTraversableNavigable::create_a_fresh_top_level_traversable(GC::Ref<Page> page, SessionHistoryEntryDescriptor initial_history_entry, VisibilityState system_visibility_state)
{
    // 1. Let traversable be the result of creating a new top-level traversable given null and the empty string.
    auto traversable = create_a_new_top_level_traversable(page, nullptr, move(initial_history_entry), system_visibility_state);
    page->set_top_level_traversable(traversable);

    // AD-HOC: Mark the about:blank document as finished parsing. This matches the behavior of the window open steps.
    auto document = GC::Ref(*traversable->active_document());
    auto completion_token = HTML::HTMLParser::parserless_completion_token(document);
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(traversable->heap(), [document, completion_token] {
        // FIXME: We do this other places too when creating a new about:blank document. Perhaps it's worth a spec issue?
        HTML::HTMLParser::the_end(document, completion_token);
    }));

    // 2. Navigate traversable to initialNavigationURL using traversable's active document, with documentResource set to initialNavigationPostResource.
    // NB: The UI process navigates the canonical traversable.

    // 3. Return traversable.
    return traversable;
}

GC::Ref<LocalTraversableNavigable> LocalTraversableNavigable::create_stand_in(Badge<Page>, RemoteNavigable& remote_navigable, SessionHistoryEntryDescriptor const& current_history_entry, VisibilityState system_visibility_state)
{
    VERIFY(!remote_navigable.parent());
    auto& page = remote_navigable.page();
    page.ensure_compositor_host();

    // The stand-in's document is a top-level browsing context's, in a group of its own, as a fresh traversable's is.
    // The WindowProxy scripts hold for the tab's document is its browsing context's.
    auto [browsing_context, document] = create_a_new_top_level_browsing_context_and_document(page, remote_navigable.window_proxy());

    auto traversable = Bindings::main_thread_vm().heap().allocate<LocalTraversableNavigable>(page);
    traversable->initialize_stand_in(remote_navigable, current_history_entry, browsing_context, document, system_visibility_state);
    traversable->set_has_session_history_entry_and_ready_for_navigation();

    // The stand-in displays the tab until the document it populates does: its document completes as a fresh
    // traversable's.
    auto completion_token = HTML::HTMLParser::parserless_completion_token(document);
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(traversable->heap(), [document = GC::Ref { document }, completion_token] {
        HTML::HTMLParser::the_end(document, completion_token);
    }));
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

    auto target_entry = resolve_local_session_history_entry(move(target_entry_descriptor));
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
// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-window-close
// AD-HOC: Step 6 of window.close(), requested through the UI process by a document another process hosts, whose
//         navigable found itself familiar with this traversable there. source is the incumbent global object's navigable.
void LocalTraversableNavigable::close_top_level_traversable_from_script(Navigable const& source)
{
    // 1. Let thisTraversable be this's navigable.
    auto& this_traversable = *this;

    // 2. If thisTraversable is not a top-level traversable, then return.
    if (!this_traversable.is_top_level_traversable())
        return;

    // 3. If thisTraversable's is closing is true, then return.
    if (this_traversable.is_closing())
        return;

    // 5. Let sourceSnapshotParams be the result of snapshotting source snapshot params given thisTraversable's active document.
    auto source_snapshot_params = snapshot_source_snapshot_params(this_traversable.active_document());

    // 6. If all the following are true:
    //    - thisTraversable is script-closable;
    //    - the incumbent global object's browsing context is familiar with browsingContext; and
    //    - the incumbent global object's navigable is allowed by sandboxing to navigate thisTraversable, given sourceSnapshotParams,
    if (!this_traversable.is_script_closable() || !source.allowed_by_sandboxing_to_navigate(this_traversable, source_snapshot_params))
        return;

    // then:
    // 1. Set thisTraversable's is closing to true.
    this_traversable.set_closing(true);

    // 2. Queue a task on the DOM manipulation task source to definitely close thisTraversable.
    queue_a_task(Task::Source::DOMManipulation, nullptr, nullptr, GC::create_function(heap(), [this] {
        definitely_close_top_level_traversable();
    }));
}

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
    // 2. If the result of checking if unloading is canceled for toUnload is not "continue", then return.
    // NB: The documents of toUnload are hosted by this page and by others. The UI process runs the check in each
    //     page hosting one, this one included, with the prompt shown at most once, and reports the result.
    page().client().page_did_request_unload_check(id(), GC::create_function(heap(), [this, append_close_steps = move(append_close_steps)](CheckIfUnloadingIsCanceledResult result) {
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

    // 1. Let browsingContext be traversable's active browsing context.
    auto browsing_context = active_browsing_context();

    // 2. For each historyEntry in traversable's session history entries:
    // NOTE: Without bfcache, only the active document is alive, so we only need to destroy it.
    if (active_document())
        active_document()->destroy_a_document_and_its_descendants();

    // 3. Remove browsingContext.
    if (!browsing_context) {
        dbgln("LocalTraversableNavigable::destroy_top_level_traversable: No browsing context?");
    } else {
        browsing_context->remove();
    }

    // 4. Remove traversable from the user interface (e.g., close or hide its tab in a tabbed browser).
    page().client().page_did_close();

    // 5. Remove traversable from the user agent's top-level traversable set.
    // NB: The UI process holds the user agent's top-level traversable set.

    // FIXME: 6. Invoke WebDriver BiDi navigable destroyed with traversable.

    // FIXME: Figure out why we need to do this... we shouldn't be leaking Navigables for all time.
    //        However, without this, we can keep stale destroyed navigables around.
    set_has_been_destroyed();
    remove_from_all_local_navigables();
}

}
