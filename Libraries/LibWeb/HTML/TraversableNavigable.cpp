/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TraversableNavigable);

TraversableNavigable::TraversableNavigable(GC::Ref<Page> page)
    : Navigable(page)
    , m_session_history_traversal_queue(vm().heap().allocate<SessionHistoryTraversalQueue>())
{
#ifdef AK_OS_MACOS
    auto display_list_player_type = page->client().display_list_player_type();
    if (display_list_player_type == DisplayListPlayerType::SkiaGPUIfAvailable) {
        m_metal_context = Gfx::get_metal_context();
        m_skia_backend_context = Gfx::SkiaBackendContext::create_metal_context(*m_metal_context);
    }
#endif

#ifdef USE_VULKAN
    auto display_list_player_type = page->client().display_list_player_type();
    if (display_list_player_type == DisplayListPlayerType::SkiaGPUIfAvailable) {
        auto maybe_vulkan_context = Gfx::create_vulkan_context();
        if (!maybe_vulkan_context.is_error()) {
            auto vulkan_context = maybe_vulkan_context.release_value();
            m_skia_backend_context = Gfx::SkiaBackendContext::create_vulkan_context(vulkan_context);
        } else {
            dbgln("Vulkan context creation failed: {}", maybe_vulkan_context.error());
        }
    }
#endif
}

TraversableNavigable::~TraversableNavigable() = default;

void TraversableNavigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_session_history_entries);
    visitor.visit(m_session_history_traversal_queue);
}

static OrderedHashTable<TraversableNavigable*>& user_agent_top_level_traversable_set()
{
    static OrderedHashTable<TraversableNavigable*> set;
    return set;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-browsing-context
WebIDL::ExceptionOr<BrowsingContextAndDocument> create_a_new_top_level_browsing_context_and_document(GC::Ref<Page> page)
{
    // 1. Let group and document be the result of creating a new browsing context group and document.
    auto [group, document] = TRY(BrowsingContextGroup::create_a_new_browsing_context_group_and_document(page));

    // 2. Return group's browsing context set[0] and document.
    return BrowsingContextAndDocument { **group->browsing_context_set().begin(), document };
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-traversable
WebIDL::ExceptionOr<GC::Ref<TraversableNavigable>> TraversableNavigable::create_a_new_top_level_traversable(GC::Ref<Page> page, GC::Ptr<HTML::BrowsingContext> opener, String target_name)
{
    auto& vm = Bindings::main_thread_vm();

    // 1. Let document be null.
    GC::Ptr<DOM::Document> document = nullptr;

    // 2. If opener is null, then set document to the second return value of creating a new top-level browsing context and document.
    if (!opener) {
        document = TRY(create_a_new_top_level_browsing_context_and_document(page)).document;
    }

    // 3. Otherwise, set document to the second return value of creating a new auxiliary browsing context and document given opener.
    else {
        document = TRY(BrowsingContext::create_a_new_auxiliary_browsing_context_and_document(page, *opener)).document;
    }

    // 4. Let documentState be a new document state, with
    auto document_state = vm.heap().allocate<DocumentState>();

    // document: document
    document_state->set_document(document);

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
    TRY_OR_THROW_OOM(vm, traversable->initialize_navigable(document_state, nullptr));

    // 7. Let initialHistoryEntry be traversable's active session history entry.
    auto initial_history_entry = traversable->active_session_history_entry();
    VERIFY(initial_history_entry);

    // 8. Set initialHistoryEntry's step to 0.
    initial_history_entry->set_step(0);

    // 9. Append initialHistoryEntry to traversable's session history entries.
    traversable->m_session_history_entries.append(*initial_history_entry);

    // FIXME: 10. If opener is non-null, then legacy-clone a traversable storage shed given opener's top-level traversable and traversable. [STORAGE]

    // 11. Append traversable to the user agent's top-level traversable set.
    user_agent_top_level_traversable_set().set(traversable);

    // 12. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-fresh-top-level-traversable
WebIDL::ExceptionOr<GC::Ref<TraversableNavigable>> TraversableNavigable::create_a_fresh_top_level_traversable(GC::Ref<Page> page, URL::URL const& initial_navigation_url, Variant<Empty, String, POSTResource> initial_navigation_post_resource)
{
    // 1. Let traversable be the result of creating a new top-level traversable given null and the empty string.
    auto traversable = TRY(create_a_new_top_level_traversable(page, nullptr, {}));
    page->set_top_level_traversable(traversable);

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
        TRY(traversable->navigate({ .url = initial_navigation_url,
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
    Vector<Vector<GC::Ref<SessionHistoryEntry>>> entry_lists { session_history_entries() };

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

        // 2. If targetEntry is not navigable's current session history entry or targetEntry's document state's reload pending is true, then append navigable to results.
        if (target_entry != navigable->current_session_history_entry() || target_entry->document_state()->reload_pending()) {
            results.append(*navigable);
        }

        // 3. If targetEntry's document is navigable's document, and targetEntry's document state's reload pending is false, then extend navigablesToCheck with the child navigables of navigable.
        if (target_entry->document() == navigable->active_document() && !target_entry->document_state()->reload_pending()) {
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
        if (target_entry->document() != navigable->active_document() || target_entry->document_state()->reload_pending()) {
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
static void deactivate_a_document_for_cross_document_navigation(GC::Ref<DOM::Document> displayed_document, Optional<UserNavigationInvolvement>, GC::Ref<SessionHistoryEntry> target_entry, GC::Ref<GC::Function<void()>> after_potential_unloads)
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
        // FIXME 1. Let firePageSwapBeforeUnload be the following step
        //            1. Fire the pageswap event given displayedDocument, targetEntry, navigationType, and null.

        // 2. Set the ongoing navigation for navigable to null.
        navigable->set_ongoing_navigation({});

        // 3. Unload a document and its descendants given displayedDocument, targetEntry's document, afterPotentialUnloads, and firePageSwapBeforeUnload.
        displayed_document->unload_a_document_and_its_descendants(target_entry->document(), after_potential_unloads);
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
    GC::Ptr<SessionHistoryEntry> target_entry;
    GC::Ptr<Navigable> navigable;
    bool update_only = false;

    GC::Ptr<SessionHistoryEntry> populated_target_entry;
    bool populated_cloned_target_session_history_entry = false;

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(displayed_document);
        visitor.visit(target_entry);
        visitor.visit(navigable);
        visitor.visit(populated_target_entry);
    }
};

GC_DEFINE_ALLOCATOR(ChangingNavigableContinuationState);

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
TraversableNavigable::HistoryStepResult TraversableNavigable::apply_the_history_step(
    int step,
    bool check_for_cancelation,
    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<SourceSnapshotParams> source_snapshot_params,
    GC::Ptr<Navigable> initiator_to_check,
    IGNORE_USE_IN_ESCAPING_LAMBDA UserNavigationInvolvement user_involvement,
    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<Bindings::NavigationType> navigation_type,
    IGNORE_USE_IN_ESCAPING_LAMBDA SynchronousNavigation synchronous_navigation)
{
    auto& vm = this->vm();
    // FIXME: 1. Assert: This is running within traversable's session history traversal queue.

    // 2. Let targetStep be the result of getting the used step given traversable and step.
    auto target_step = get_the_used_step(step);

    // Note: Calling this early so we can re-use the same list in 3.2 and 6.
    auto change_or_reload_navigables = get_all_navigables_whose_current_session_history_entry_will_change_or_reload(target_step);

    // 3. If initiatorToCheck is not null, then:
    if (initiator_to_check != nullptr) {
        // 1. Assert: sourceSnapshotParams is not null.
        VERIFY(source_snapshot_params.has_value());

        // 2. For each navigable of get all navigables whose current session history entry will change or reload:
        //    if initiatorToCheck is not allowed by sandboxing to navigate navigable given sourceSnapshotParams, then return "initiator-disallowed".
        for (auto const& navigable : change_or_reload_navigables) {
            if (!initiator_to_check->allowed_by_sandboxing_to_navigate(*navigable, *source_snapshot_params))
                return HistoryStepResult::InitiatorDisallowed;
        }
    }

    // 4. Let navigablesCrossingDocuments be the result of getting all navigables that might experience a cross-document traversal given traversable and targetStep.
    auto navigables_crossing_documents = get_all_navigables_that_might_experience_a_cross_document_traversal(target_step);

    // 5. If checkForCancelation is true, and the result of checking if unloading is canceled given navigablesCrossingDocuments, traversable, targetStep,
    //    and userInvolvement is not "continue", then return that result.
    if (check_for_cancelation) {
        auto result = check_if_unloading_is_canceled(navigables_crossing_documents, *this, target_step, user_involvement);
        if (result == CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload)
            return HistoryStepResult::CanceledByBeforeUnload;
        if (result == CheckIfUnloadingIsCanceledResult::CanceledByNavigate)
            return HistoryStepResult::CanceledByNavigate;
    }

    // 6. Let changingNavigables be the result of get all navigables whose current session history entry will change or reload given traversable and targetStep.
    auto changing_navigables = move(change_or_reload_navigables);

    // 7. Let nonchangingNavigablesThatStillNeedUpdates be the result of getting all navigables that only need history object length/index update given traversable and targetStep.
    auto non_changing_navigables_that_still_need_updates = get_all_navigables_that_only_need_history_object_length_index_update(target_step);

    // 8. For each navigable of changingNavigables:
    for (auto& navigable : changing_navigables) {
        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. Set navigable's current session history entry to targetEntry.
        navigable->set_current_session_history_entry(target_entry);

        // 3. Set navigable's ongoing navigation to "traversal".
        navigable->set_ongoing_navigation(Traversal::Tag);
    }

    // 9. Let totalChangeJobs be the size of changingNavigables.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto total_change_jobs = changing_navigables.size();

    // 10. Let completedChangeJobs be 0.
    IGNORE_USE_IN_ESCAPING_LAMBDA size_t completed_change_jobs = 0;

    // 11. Let changingNavigableContinuations be an empty queue of changing navigable continuation states.
    // NOTE: This queue is used to split the operations on changingNavigables into two parts. Specifically, changingNavigableContinuations holds data for the second part.
    IGNORE_USE_IN_ESCAPING_LAMBDA Queue<GC::Root<ChangingNavigableContinuationState>> changing_navigable_continuations;

    // 12. For each navigable of changingNavigables, queue a global task on the navigation and traversal task source of navigable's active window to run the steps:
    for (auto& navigable : changing_navigables) {
        if (!navigable->active_window())
            continue;
        queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), GC::create_function(heap(), [&] {
            // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
            if (navigable->has_been_destroyed()) {
                completed_change_jobs++;
                return;
            }

            // 1. Let displayedEntry be navigable's active session history entry.
            auto displayed_entry = navigable->active_session_history_entry();

            // 2. Let targetEntry be navigable's current session history entry.
            auto target_entry = navigable->current_session_history_entry();

            // 3. Let changingNavigableContinuation be a changing navigable continuation state with:
            auto changing_navigable_continuation = vm.heap().allocate<ChangingNavigableContinuationState>();
            changing_navigable_continuation->displayed_document = displayed_entry->document();
            changing_navigable_continuation->target_entry = target_entry;
            changing_navigable_continuation->navigable = navigable;
            changing_navigable_continuation->update_only = false;
            changing_navigable_continuation->populated_target_entry = nullptr;
            changing_navigable_continuation->populated_cloned_target_session_history_entry = false;

            // 4. If displayedEntry is targetEntry and targetEntry's document state's reload pending is false, then:
            if (synchronous_navigation == SynchronousNavigation::Yes && !target_entry->document_state()->reload_pending()) {
                // 1. Set changingNavigableContinuation's update-only to true.
                changing_navigable_continuation->update_only = true;

                // 2. Enqueue changingNavigableContinuation on changingNavigableContinuations.
                changing_navigable_continuations.enqueue(move(changing_navigable_continuation));

                // 3. Abort these steps.
                return;
            }

            // 5. Switch on navigationType:
            if (navigation_type.has_value()) {
                switch (navigation_type.value()) {
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
                auto navigation = active_window()->navigation();

                // 2. Fire a traverse navigate event at navigation given targetEntry and userInvolvement.
                navigation->fire_a_traverse_navigate_event(*target_entry, user_involvement);
            }

            auto after_document_populated = [old_origin, changing_navigable_continuation, &changing_navigable_continuations, &vm, &navigable](bool populated_cloned_target_she, GC::Ref<SessionHistoryEntry> target_entry) mutable {
                changing_navigable_continuation->populated_target_entry = target_entry;
                changing_navigable_continuation->populated_cloned_target_session_history_entry = populated_cloned_target_she;

                // 1. If targetEntry's document is null, then set changingNavigableContinuation's update-only to true.
                if (!target_entry->document()) {
                    changing_navigable_continuation->update_only = true;
                }

                else {
                    // 2. If targetEntry's document's origin is not oldOrigin, then set targetEntry's classic history API state to StructuredSerializeForStorage(null).
                    if (target_entry->document()->origin() != old_origin) {
                        target_entry->set_classic_history_api_state(MUST(structured_serialize_for_storage(vm, JS::js_null())));
                    }

                    // 3. If all of the following are true:
                    //     - navigable's parent is null;
                    //     - targetEntry's document's browsing context is not an auxiliary browsing context whose opener browsing context is non-null; and
                    //     - targetEntry's document's origin is not oldOrigin,
                    //    then set targetEntry's document state's navigable target name to the empty string.
                    if (navigable->parent() == nullptr
                        && !(target_entry->document()->browsing_context()->is_auxiliary() && target_entry->document()->browsing_context()->opener_browsing_context() != nullptr)
                        && target_entry->document_state()->origin() != old_origin) {
                        target_entry->document_state()->set_navigable_target_name(String {});
                    }
                }

                // 4. Enqueue changingNavigableContinuation on changingNavigableContinuations.
                changing_navigable_continuations.enqueue(move(changing_navigable_continuation));
            };

            // 8. If targetEntry's document is null, or targetEntry's document state's reload pending is true, then:
            if (!target_entry->document() || target_entry->document_state()->reload_pending()) {
                // FIXME: 1. Let navTimingType be "back_forward" if targetEntry's document is null; otherwise "reload".

                // 2. Let targetSnapshotParams be the result of snapshotting target snapshot params given navigable.
                auto target_snapshot_params = navigable->snapshot_target_snapshot_params();

                // 3. Let potentiallyTargetSpecificSourceSnapshotParams be sourceSnapshotParams.
                Optional<SourceSnapshotParams> potentially_target_specific_source_snapshot_params = source_snapshot_params;

                // 4. If potentiallyTargetSpecificSourceSnapshotParams is null, then set it to the result of snapshotting source snapshot params given navigable's active document.
                if (!potentially_target_specific_source_snapshot_params.has_value()) {
                    potentially_target_specific_source_snapshot_params = navigable->active_document()->snapshot_source_snapshot_params();
                }

                // 5. Set targetEntry's document state's reload pending to false.
                target_entry->document_state()->set_reload_pending(false);

                // 6. Let allowPOST be targetEntry's document state's reload pending.
                auto allow_POST = target_entry->document_state()->reload_pending();

                // https://github.com/whatwg/html/issues/9869
                // Reloading requires population of the active session history entry, making it inactive.
                // This results in a situation where tasks that unload the previous document and activate a new
                // document cannot run. To resolve this, the target entry is cloned before it is populated.
                // After the unloading of the previous document is completed, all fields potentially affected by the
                // population are copied from the cloned target entry to the actual target entry.
                auto populated_target_entry = target_entry->clone();

                // 7. In parallel, attempt to populate the history entry's document for targetEntry, given navigable, potentiallyTargetSpecificSourceSnapshotParams,
                //    targetSnapshotParams, userInvolvement, with allowPOST set to allowPOST and completionSteps set to
                //    queue a global task on the navigation and traversal task source given navigable's active window to
                //    run afterDocumentPopulated.
                Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(this->heap(), [populated_target_entry, potentially_target_specific_source_snapshot_params, target_snapshot_params, this, allow_POST, navigable, after_document_populated = GC::create_function(this->heap(), move(after_document_populated)), user_involvement] {
                    navigable->populate_session_history_entry_document(populated_target_entry, *potentially_target_specific_source_snapshot_params, target_snapshot_params, user_involvement, {}, Navigable::NullOrError {}, CSPNavigationType::Other, allow_POST, GC::create_function(this->heap(), [this, after_document_populated, populated_target_entry]() mutable {
                                 VERIFY(active_window());
                                 queue_global_task(Task::Source::NavigationAndTraversal, *active_window(), GC::create_function(this->heap(), [after_document_populated, populated_target_entry]() mutable {
                                     after_document_populated->function()(true, populated_target_entry);
                                 }));
                             }))
                        .release_value_but_fixme_should_propagate_errors();
                }));
            }
            // Otherwise, run afterDocumentPopulated immediately.
            else {
                after_document_populated(false, *target_entry);
            }
        }));
    }

    auto check_if_document_population_tasks_completed = GC::create_function(heap(), [&] {
        return changing_navigable_continuations.size() + completed_change_jobs == total_change_jobs;
    });

    if (synchronous_navigation == SynchronousNavigation::Yes) {
        // NOTE: Synchronous navigation should never require document population, so it is safe to process only NavigationAndTraversal source.
        main_thread_event_loop().spin_processing_tasks_with_source_until(Task::Source::NavigationAndTraversal, check_if_document_population_tasks_completed);
    } else {
        // NOTE: Process all task sources while waiting because reloading or back/forward navigation might require fetching to populate a document.
        main_thread_event_loop().spin_until(check_if_document_population_tasks_completed);
    }

    // 13. Let navigablesThatMustWaitBeforeHandlingSyncNavigation be an empty set.
    HashTable<GC::Ref<Navigable>> navigables_that_must_wait_before_handling_sync_navigation;

    // 14. While completedChangeJobs does not equal totalChangeJobs:
    while (!changing_navigable_continuations.is_empty()) {
        // NOTE: Synchronous navigations that are intended to take place before this traversal jump the queue at this point,
        //       so they can be added to the correct place in traversable's session history entries before this traversal
        //       potentially unloads their document. More details can be found here (https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigation-steps-queue-jumping-examples)
        // 1. If traversable's running nested apply history step is false, then:
        if (!m_running_nested_apply_history_step) {
            // 1. While traversable's session history traversal queue's algorithm set contains one or more synchronous
            //    navigation steps with a target navigable not contained in navigablesThatMustWaitBeforeHandlingSyncNavigation:
            //   1. Let steps be the first item in traversable's session history traversal queue's algorithm set
            //    that is synchronous navigation steps with a target navigable not contained in navigablesThatMustWaitBeforeHandlingSyncNavigation.
            //   2. Remove steps from traversable's session history traversal queue's algorithm set.
            for (auto entry = m_session_history_traversal_queue->first_synchronous_navigation_steps_with_target_navigable_not_contained_in(navigables_that_must_wait_before_handling_sync_navigation);
                entry;
                entry = m_session_history_traversal_queue->first_synchronous_navigation_steps_with_target_navigable_not_contained_in(navigables_that_must_wait_before_handling_sync_navigation)) {

                // 3. Set traversable's running nested apply history step to true.
                m_running_nested_apply_history_step = true;

                // 4. Run steps.
                entry->execute_steps();

                // 5. Set traversable's running nested apply history step to false.
                m_running_nested_apply_history_step = false;
            }
        }

        // 2. Let changingNavigableContinuation be the result of dequeuing from changingNavigableContinuations.
        auto changing_navigable_continuation = changing_navigable_continuations.dequeue();

        // 3. If changingNavigableContinuation is nothing, then continue.

        // 4. Let displayedDocument be changingNavigableContinuation's displayed document.
        auto displayed_document = changing_navigable_continuation->displayed_document;

        // 5. Let targetEntry be changingNavigableContinuation's target entry.
        GC::Ptr<SessionHistoryEntry> const populated_target_entry = changing_navigable_continuation->populated_target_entry;

        // 6. Let navigable be changingNavigableContinuation's navigable.
        auto navigable = changing_navigable_continuation->navigable;

        // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
        if (navigable->has_been_destroyed())
            continue;

        // AD-HOC: We re-compute targetStep here, since it might have changed since the last time we computed it.
        //         This can happen if navigables are destroyed while we wait for tasks to complete.
        target_step = get_the_used_step(step);

        // 7. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index given traversable and targetStep.
        auto history_object_length_and_index = get_the_history_object_length_and_index(target_step);
        auto script_history_length = history_object_length_and_index.script_history_length;
        auto script_history_index = history_object_length_and_index.script_history_index;

        // 8. Append navigable to navigablesThatMustWaitBeforeHandlingSyncNavigation.
        navigables_that_must_wait_before_handling_sync_navigation.set(*navigable);

        // 9. Let entriesForNavigationAPI be the result of getting session history entries for the navigation API given navigable and targetStep.
        auto entries_for_navigation_api = get_session_history_entries_for_the_navigation_api(*navigable, target_step);

        // NOTE: Steps 10 and 11 come after step 12.

        // 12. In both cases, let afterPotentialUnloads be the following steps:
        bool const update_only = changing_navigable_continuation->update_only;
        GC::Ptr<SessionHistoryEntry> const target_entry = changing_navigable_continuation->target_entry;
        bool const populated_cloned_target_session_history_entry = changing_navigable_continuation->populated_cloned_target_session_history_entry;
        auto after_potential_unload = GC::create_function(this->heap(), [navigable, update_only, target_entry, populated_target_entry, populated_cloned_target_session_history_entry, displayed_document, &completed_change_jobs, script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), &heap = this->heap(), navigation_type] {
            if (populated_cloned_target_session_history_entry) {
                target_entry->set_document_state(populated_target_entry->document_state());
                target_entry->set_url(populated_target_entry->url());
                target_entry->set_classic_history_api_state(populated_target_entry->classic_history_api_state());
            }

            // 1. Let previousEntry be navigable's active session history entry.
            GC::Ptr<SessionHistoryEntry> const previous_entry = navigable->active_session_history_entry();

            // 2. If changingNavigableContinuation's update-only is false, then activate history entry targetEntry for navigable.
            if (!update_only)
                navigable->activate_history_entry(*target_entry);

            // 3. Let updateDocument be an algorithm step which performs update document for history step application given
            //    targetEntry's document, targetEntry, changingNavigableContinuation's update-only, scriptHistoryLength,
            //    scriptHistoryIndex, navigationType, entriesForNavigationAPI, and previousEntry.
            auto update_document = [script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), target_entry, update_only, navigation_type, previous_entry] {
                target_entry->document()->update_for_history_step_application(*target_entry, update_only, script_history_length, script_history_index, navigation_type, entries_for_navigation_api, previous_entry);
            };

            // 3. If targetEntry's document is equal to displayedDocument, then perform updateDocument.
            if (target_entry->document().ptr() == displayed_document.ptr()) {
                update_document();
            }
            // 5. Otherwise, queue a global task on the navigation and traversal task source given targetEntry's document's relevant global object to perform updateDocument
            else {
                queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*target_entry->document()), GC::create_function(heap, move(update_document)));
            }

            // 6. Increment completedChangeJobs.
            completed_change_jobs++;
        });

        // 10. If changingNavigableContinuation's update-only is true, or targetEntry's document is displayedDocument, then:
        if (changing_navigable_continuation->update_only || populated_target_entry->document().ptr() == displayed_document.ptr()) {
            // 1. Set the ongoing navigation for navigable to null.
            navigable->set_ongoing_navigation({});

            // 2. Queue a global task on the navigation and traversal task source given navigable's active window to perform afterPotentialUnloads.
            VERIFY(navigable->active_window());
            queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), after_potential_unload);
        }
        // 11. Otherwise:
        else {
            // 1. Assert: navigationType is not null.
            VERIFY(navigation_type.has_value());

            // 2. Deactivate displayedDocument, given userInvolvement, targetEntry, navigationType, and afterPotentialUnloads.
            deactivate_a_document_for_cross_document_navigation(*displayed_document, user_involvement, *populated_target_entry, after_potential_unload);
        }
    }

    main_thread_event_loop().spin_processing_tasks_with_source_until(Task::Source::NavigationAndTraversal, GC::create_function(heap(), [&] {
        return completed_change_jobs == total_change_jobs;
    }));

    // 15. Let totalNonchangingJobs be the size of nonchangingNavigablesThatStillNeedUpdates.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto total_non_changing_jobs = non_changing_navigables_that_still_need_updates.size();

    // 16. Let completedNonchangingJobs be 0.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto completed_non_changing_jobs = 0u;

    // AD-HOC: We re-compute targetStep here, since it might have changed since the last time we computed it.
    //         This can happen if navigables are destroyed while we wait for tasks to complete.
    target_step = get_the_used_step(step);

    // 17. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index given traversable and targetStep.
    auto length_and_index = get_the_history_object_length_and_index(target_step);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto script_history_length = length_and_index.script_history_length;
    IGNORE_USE_IN_ESCAPING_LAMBDA auto script_history_index = length_and_index.script_history_index;

    // 18. For each navigable of nonchangingNavigablesThatStillNeedUpdates, queue a global task on the navigation and traversal task source given navigable's active window to run the steps:
    for (auto& navigable : non_changing_navigables_that_still_need_updates) {
        // AD-HOC: This check is not in the spec but we should not continue navigation if navigable has been destroyed,
        //         or if there's no active window.
        if (navigable->has_been_destroyed() || !navigable->active_window()) {
            ++completed_non_changing_jobs;
            continue;
        }

        queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), GC::create_function(heap(), [&] {
            // AD-HOC: This check is not in the spec but we should not continue navigation if navigable has been destroyed,
            //         or if there's no active window.
            if (navigable->has_been_destroyed() || !navigable->active_window()) {
                ++completed_non_changing_jobs;
                return;
            }

            // 1. Let document be navigable's active document.
            auto document = navigable->active_document();

            // 2. Set document's history object's index to scriptHistoryIndex.
            document->history()->m_index = script_history_index;

            // 3. Set document's history object's length to scriptHistoryLength.
            document->history()->m_length = script_history_length;

            // 4. Increment completedNonchangingJobs.
            ++completed_non_changing_jobs;
        }));
    }

    // 19. Wait for completedNonchangingJobs to equal totalNonchangingJobs.
    // AD-HOC: Since currently populate_session_history_entry_document does not run in parallel
    //         we call spin_until to interrupt execution of this function and let document population
    //         to complete.
    main_thread_event_loop().spin_processing_tasks_with_source_until(Task::Source::NavigationAndTraversal, GC::create_function(heap(), [&] {
        return completed_non_changing_jobs == total_non_changing_jobs;
    }));

    // 20. Set traversable's current session history step to targetStep.
    m_current_session_history_step = target_step;

    // Not in the spec:
    auto back_enabled = m_current_session_history_step > 0;
    VERIFY(m_session_history_entries.size() > 0);
    auto forward_enabled = can_go_forward();
    page().client().page_did_update_navigation_buttons_state(back_enabled, forward_enabled);

    page().client().page_did_change_url(current_session_history_entry()->url());

    // 21. Return "applied".
    return HistoryStepResult::Applied;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
TraversableNavigable::CheckIfUnloadingIsCanceledResult TraversableNavigable::check_if_unloading_is_canceled(
    Vector<GC::Root<Navigable>> navigables_that_need_before_unload,
    GC::Ptr<TraversableNavigable> traversable,
    Optional<int> target_step,
    Optional<UserNavigationInvolvement> user_involvement_for_navigate_events)
{
    // 1. Let documentsToFireBeforeunload be the active document of each item in navigablesThatNeedBeforeUnload.
    Vector<GC::Root<DOM::Document>> documents_to_fire_beforeunload;
    for (auto& navigable : navigables_that_need_before_unload)
        documents_to_fire_beforeunload.append(navigable->active_document());

    // 2. Let unloadPromptShown be false.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto unload_prompt_shown = false;

    // 3. Let finalStatus be "continue".
    IGNORE_USE_IN_ESCAPING_LAMBDA auto final_status = CheckIfUnloadingIsCanceledResult::Continue;

    // 4. If traversable was given, then:
    if (traversable) {
        // 1. Assert: targetStep and userInvolvementForNavigateEvent were given.
        // NOTE: This assertion is enforced by the caller.

        // 2. Let targetEntry be the result of getting the target history entry given traversable and targetStep.
        auto target_entry = traversable->get_the_target_history_entry(target_step.value());

        // 3. If targetEntry is not traversable's current session history entry, and targetEntry's document state's origin is the same as
        //    traversable's current session history entry's document state's origin, then:
        if (target_entry != traversable->current_session_history_entry() && target_entry->document_state()->origin() != traversable->current_session_history_entry()->document_state()->origin()) {
            // 1. Let eventsFired be false.
            IGNORE_USE_IN_ESCAPING_LAMBDA auto events_fired = false;

            // 2. Let needsBeforeunload be true if navigablesThatNeedBeforeUnload contains traversable; otherwise false.
            auto it = navigables_that_need_before_unload.find_if([&traversable](GC::Root<Navigable> navigable) {
                return navigable.ptr() == traversable.ptr();
            });
            auto needs_beforeunload = it != navigables_that_need_before_unload.end();

            // 3. If needsBeforeunload is true, then remove traversable's active document from documentsToFireBeforeunload.
            if (needs_beforeunload) {
                documents_to_fire_beforeunload.remove_first_matching([&](auto& document) {
                    return document.ptr() == traversable->active_document().ptr();
                });
            }

            // 4. Queue a global task on the navigation and traversal task source given traversable's active window to perform the following steps:
            VERIFY(traversable->active_window());
            queue_global_task(Task::Source::NavigationAndTraversal, *traversable->active_window(), GC::create_function(heap(), [needs_beforeunload, user_involvement_for_navigate_events, traversable, target_entry, &final_status, &unload_prompt_shown, &events_fired] {
                // 1. if needsBeforeunload is true, then:
                if (needs_beforeunload) {
                    // 1. Let (unloadPromptShownForThisDocument, unloadPromptCanceledByThisDocument) be the result of running the steps to fire beforeunload given traversable's active document and false.
                    auto [unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document] = traversable->active_document()->steps_to_fire_beforeunload(false);

                    // 2. If unloadPromptShownForThisDocument is true, then set unloadPromptShown to true.
                    if (unload_prompt_shown_for_this_document)
                        unload_prompt_shown = true;

                    // 3. If unloadPromptCanceledByThisDocument is true, then set finalStatus to "canceled-by-beforeunload".
                    if (unload_prompt_canceled_by_this_document)
                        final_status = CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload;
                }

                // 2. If finalStatus is "canceled-by-beforeunload", then abort these steps.
                if (final_status == CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload)
                    return;

                // 3. Let navigation be traversable's active window's navigation API.
                VERIFY(traversable->active_window());
                auto navigation = traversable->active_window()->navigation();

                // 4. Let navigateEventResult be the result of firing a traverse navigate event at navigation given targetEntry and userInvolvementForNavigateEvent.
                VERIFY(target_entry);
                auto navigate_event_result = navigation->fire_a_traverse_navigate_event(*target_entry, *user_involvement_for_navigate_events);

                // 5. If navigateEventResult is false, then set finalStatus to "canceled-by-navigate".
                if (!navigate_event_result)
                    final_status = CheckIfUnloadingIsCanceledResult::CanceledByNavigate;

                // 6. Set eventsFired to true.
                events_fired = true;
            }));

            // 5. Wait for eventsFired to be true.
            main_thread_event_loop().spin_until(GC::create_function(heap(), [&] {
                return events_fired;
            }));

            // 6. If finalStatus is not "continue", then return finalStatus.
            if (final_status != CheckIfUnloadingIsCanceledResult::Continue)
                return final_status;
        }
    }

    // 5. Let totalTasks be the size of documentsToFireBeforeunload.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto total_tasks = documents_to_fire_beforeunload.size();

    // 6. Let completedTasks be 0.
    IGNORE_USE_IN_ESCAPING_LAMBDA size_t completed_tasks = 0;

    // 7. For each document of documentsToFireBeforeunload, queue a global task on the navigation and traversal task source given document's relevant global object to run the steps:
    for (auto& document : documents_to_fire_beforeunload) {
        queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*document), GC::create_function(heap(), [document, &final_status, &completed_tasks, &unload_prompt_shown] {
            // 1. Let (unloadPromptShownForThisDocument, unloadPromptCanceledByThisDocument) be the result of running the steps to fire beforeunload given document and unloadPromptShown.
            auto [unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document] = document->steps_to_fire_beforeunload(unload_prompt_shown);

            // 2. If unloadPromptShownForThisDocument is true, then set unloadPromptShown to true.
            if (unload_prompt_shown_for_this_document)
                unload_prompt_shown = true;

            // 3. If unloadPromptCanceledByThisDocument is true, then set finalStatus to "canceled-by-beforeunload".
            if (unload_prompt_canceled_by_this_document)
                final_status = CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload;

            // 4. Increment completedTasks.
            completed_tasks++;
        }));
    }

    // 8. Wait for completedTasks to be totalTasks.
    main_thread_event_loop().spin_until(GC::create_function(heap(), [&] {
        return completed_tasks == total_tasks;
    }));

    // 9. Return finalStatus.
    return final_status;
}

TraversableNavigable::CheckIfUnloadingIsCanceledResult TraversableNavigable::check_if_unloading_is_canceled(Vector<GC::Root<Navigable>> navigables_that_need_before_unload)
{
    return check_if_unloading_is_canceled(navigables_that_need_before_unload, {}, {}, {});
}

Vector<GC::Ref<SessionHistoryEntry>> TraversableNavigable::get_session_history_entries_for_the_navigation_api(GC::Ref<Navigable> navigable, int target_step)
{
    // 1. Let rawEntries be the result of getting session history entries for navigable.
    auto raw_entries = navigable->get_session_history_entries();

    if (raw_entries.is_empty())
        return {};

    // 2. Let entriesForNavigationAPI be a new empty list.
    Vector<GC::Ref<SessionHistoryEntry>> entries_for_navigation_api;

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
    Vector<Vector<GC::Ref<SessionHistoryEntry>>&> entry_lists;
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

    Vector<Vector<GC::Ref<SessionHistoryEntry>> const&> entry_lists;
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
void TraversableNavigable::traverse_the_history_by_delta(int delta, Optional<DOM::Document&> source_document)
{
    // 1. Let sourceSnapshotParams and initiatorToCheck be null.
    Optional<SourceSnapshotParams> source_snapshot_params = {};
    GC::Ptr<Navigable> initiator_to_check = nullptr;

    // 2. Let userInvolvement be "browser UI".
    UserNavigationInvolvement user_involvement = UserNavigationInvolvement::BrowserUI;

    // 1. If sourceDocument is given, then:
    if (source_document.has_value()) {
        // 1. Set sourceSnapshotParams to the result of snapshotting source snapshot params given sourceDocument.
        source_snapshot_params = source_document->snapshot_source_snapshot_params();

        // 2. Set initiatorToCheck to sourceDocument's node navigable.
        initiator_to_check = source_document->navigable();

        // 3. Set userInvolvement to "none".
        user_involvement = UserNavigationInvolvement::None;
    }

    // 4. Append the following session history traversal steps to traversable:
    append_session_history_traversal_steps(GC::create_function(heap(), [this, delta, source_snapshot_params = move(source_snapshot_params), initiator_to_check, user_involvement] {
        // 1. Let allSteps be the result of getting all used history steps for traversable.
        auto all_steps = get_all_used_history_steps();

        // 2. Let currentStepIndex be the index of traversable's current session history step within allSteps.
        auto current_step_index = *all_steps.find_first_index(current_session_history_step());

        // 3. Let targetStepIndex be currentStepIndex plus delta
        auto target_step_index = current_step_index + delta;

        // 4. If allSteps[targetStepIndex] does not exist, then abort these steps.
        if (target_step_index >= all_steps.size()) {
            return;
        }

        // 5. Apply the traverse history step allSteps[targetStepIndex] to traversable, given sourceSnapshotParams,
        //    initiatorToCheck, and userInvolvement.
        apply_the_traverse_history_step(all_steps[target_step_index], source_snapshot_params, initiator_to_check, user_involvement);
    }));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#update-for-navigable-creation/destruction
TraversableNavigable::HistoryStepResult TraversableNavigable::update_for_navigable_creation_or_destruction()
{
    // 1. Let step be traversable's current session history step.
    auto step = current_session_history_step();

    // 2. Return the result of applying the history step to traversable given false, null, null, null, and null.
    return apply_the_history_step(step, false, {}, {}, UserNavigationInvolvement::None, {}, SynchronousNavigation::No);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-reload-history-step
TraversableNavigable::HistoryStepResult TraversableNavigable::apply_the_reload_history_step(UserNavigationInvolvement user_involvement)
{
    // 1. Let step be traversable's current session history step.
    auto step = current_session_history_step();

    // 2. Return the result of applying the history step step to traversable given true, null, null, null, and "reload".
    return apply_the_history_step(step, true, {}, {}, user_involvement, Bindings::NavigationType::Reload, SynchronousNavigation::No);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-push/replace-history-step
TraversableNavigable::HistoryStepResult TraversableNavigable::apply_the_push_or_replace_history_step(int step, HistoryHandlingBehavior history_handling, UserNavigationInvolvement user_involvement, SynchronousNavigation synchronous_navigation)
{
    // 1. Return the result of applying the history step step to traversable given false, null, null, userInvolvement, and historyHandling.
    auto navigation_type = history_handling == HistoryHandlingBehavior::Replace ? Bindings::NavigationType::Replace : Bindings::NavigationType::Push;
    return apply_the_history_step(step, false, {}, {}, user_involvement, navigation_type, synchronous_navigation);
}

TraversableNavigable::HistoryStepResult TraversableNavigable::apply_the_traverse_history_step(int step, Optional<SourceSnapshotParams> source_snapshot_params, GC::Ptr<Navigable> initiator_to_check, UserNavigationInvolvement user_involvement)
{
    // 1. Return the result of applying the history step step to traversable given true, sourceSnapshotParams, initiatorToCheck, userInvolvement, and "traverse".
    return apply_the_history_step(step, true, move(source_snapshot_params), initiator_to_check, user_involvement, Bindings::NavigationType::Traverse, SynchronousNavigation::No);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#close-a-top-level-traversable
void TraversableNavigable::close_top_level_traversable()
{
    // 1. If traversable's is closing is true, then return.
    if (is_closing())
        return;

    // 2. Definitely close traversable.
    definitely_close_top_level_traversable();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#definitely-close-a-top-level-traversable
void TraversableNavigable::definitely_close_top_level_traversable()
{
    VERIFY(is_top_level_traversable());

    // 1. Let toUnload be traversable's active document's inclusive descendant navigables.
    auto to_unload = active_document()->inclusive_descendant_navigables();

    // 2. If the result of checking if unloading is canceled for toUnload is true, then return.
    if (check_if_unloading_is_canceled(to_unload) != CheckIfUnloadingIsCanceledResult::Continue)
        return;

    // 3. Append the following session history traversal steps to traversable:
    append_session_history_traversal_steps(GC::create_function(heap(), [this] {
        // 1. Let afterAllUnloads be an algorithm step which destroys traversable.
        auto after_all_unloads = GC::create_function(heap(), [this] {
            destroy_top_level_traversable();
        });

        // 2. Unload a document and its descendants given traversable's active document, null, and afterAllUnloads.
        active_document()->unload_a_document_and_its_descendants({}, after_all_unloads);
    }));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-top-level-traversable
void TraversableNavigable::destroy_top_level_traversable()
{
    VERIFY(is_top_level_traversable());

    // 1. Let browsingContext be traversable's active browsing context.
    auto browsing_context = active_browsing_context();

    // 2. For each historyEntry in traversable's session history entries:
    for (auto& history_entry : m_session_history_entries) {
        // 1. Let document be historyEntry's document.
        auto document = history_entry->document();

        // 2. If document is not null, then destroy document.
        if (document)
            document->destroy();
    }

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

    // FIXME: Figure out why we need to do this... we shouldn't be leaking Navigables for all time.
    //        However, without this, we can keep stale destroyed traversables around.
    set_has_been_destroyed();
    all_navigables().remove(*this);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation
void finalize_a_same_document_navigation(GC::Ref<TraversableNavigable> traversable, GC::Ref<Navigable> target_navigable, GC::Ref<SessionHistoryEntry> target_entry, GC::Ptr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior history_handling, UserNavigationInvolvement user_involvement)
{
    // NOTE: This is not in the spec but we should not navigate destroyed navigable.
    if (target_navigable->has_been_destroyed())
        return;

    // FIXME: 1. Assert: this is running on traversable's session history traversal queue.

    // 2. If targetNavigable's active session history entry is not targetEntry, then return.
    if (target_navigable->active_session_history_entry() != target_entry) {
        return;
    }

    // 3. Let targetStep be null.
    Optional<int> target_step;

    // 4. Let targetEntries be the result of getting session history entries for targetNavigable.
    auto& target_entries = target_navigable->get_session_history_entries();

    // 5. If entryToReplace is null, then:
    // FIXME: Checking containment of entryToReplace should not be needed.
    //        For more details see https://github.com/whatwg/html/issues/10232#issuecomment-2037543137
    if (!entry_to_replace || !target_entries.contains_slow(GC::Ref { *entry_to_replace })) {
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
    traversable->apply_the_push_or_replace_history_step(*target_step, history_handling, user_involvement, TraversableNavigable::SynchronousNavigation::Yes);
}

// https://html.spec.whatwg.org/multipage/interaction.html#system-visibility-state
void TraversableNavigable::set_system_visibility_state(VisibilityState visibility_state)
{
    if (m_system_visibility_state == visibility_state)
        return;
    m_system_visibility_state = visibility_state;

    // When a user-agent determines that the system visibility state for
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
    while (candidate->focused_element()
        && is<HTML::NavigableContainer>(candidate->focused_element())
        && static_cast<HTML::NavigableContainer&>(*candidate->focused_element()).content_navigable()) {
        candidate = static_cast<HTML::NavigableContainer&>(*candidate->focused_element()).content_navigable()->active_document();
    }

    // 4. If candidate's focused area is non-null, set candidate to candidate's focused area.
    if (candidate->focused_element()) {
        // NOTE: We return right away here instead of assigning to candidate,
        //       since that would require compromising type safety.
        return candidate->focused_element();
    }

    // 5. Return candidate.
    return candidate;
}

void TraversableNavigable::paint(DevicePixelRect const& content_rect, Painting::BackingStore& target, PaintOptions paint_options)
{
    auto document = active_document();
    if (!document)
        return;

    for (auto& navigable : all_navigables()) {
        if (auto active_document = navigable->active_document(); active_document && active_document->paintable()) {
            active_document->paintable()->refresh_scroll_state();
        }
    }

    DOM::Document::PaintConfig paint_config;
    paint_config.paint_overlay = paint_options.paint_overlay == PaintOptions::PaintOverlay::Yes;
    paint_config.should_show_line_box_borders = paint_options.should_show_line_box_borders;
    paint_config.has_focus = paint_options.has_focus;
    paint_config.canvas_fill_rect = Gfx::IntRect { {}, content_rect.size() };
    auto display_list = document->record_display_list(paint_config);
    if (!display_list) {
        return;
    }

    switch (page().client().display_list_player_type()) {
    case DisplayListPlayerType::SkiaGPUIfAvailable: {
#ifdef AK_OS_MACOS
        if (m_metal_context && m_skia_backend_context && is<Painting::IOSurfaceBackingStore>(target)) {
            auto& iosurface_backing_store = static_cast<Painting::IOSurfaceBackingStore&>(target);
            auto painting_surface = Gfx::PaintingSurface::wrap_iosurface(iosurface_backing_store.iosurface_handle(), *m_skia_backend_context);
            Painting::DisplayListPlayerSkia player(*m_skia_backend_context, painting_surface);
            player.execute(*display_list);
            return;
        }
#endif

#ifdef USE_VULKAN
        if (m_skia_backend_context) {
            Painting::DisplayListPlayerSkia player(*m_skia_backend_context, target.bitmap());
            player.execute(*display_list);
            return;
        }
#endif

        // Fallback to CPU backend if GPU is not available
        Painting::DisplayListPlayerSkia player(target.bitmap());
        player.execute(*display_list);
        break;
    }
    case DisplayListPlayerType::SkiaCPU: {
        Painting::DisplayListPlayerSkia player(target.bitmap());
        player.execute(*display_list);
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

}
