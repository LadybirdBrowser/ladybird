/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/ApplyHistoryStep.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/NavigationLoader.h>

namespace WebView {

ApplyHistoryStep::ApplyHistoryStep(
    CanonicalTraversable& traversable,
    HistoryOperation& operation,
    Web::HTML::CrossProcessId operation_id,
    u64 operation_sequence_number,
    i32 step,
    bool check_for_cancelation,
    Optional<Web::HTML::CrossProcessId> initiator_to_check,
    Optional<Web::HTML::NavigationSourceSnapshot> source_snapshot_params,
    Web::HTML::UserNavigationInvolvement user_involvement,
    Optional<Web::Bindings::NavigationType> navigation_type)
    : m_traversable(traversable)
    , m_session_history(traversable.m_session_history)
    , m_session_history_traversal_queue(traversable.m_history_traversal_queue)
    , m_traversable_state(traversable.m_apply_history_step_traversable_state)
    , m_operation(operation)
    , m_operation_id(operation_id)
    , m_operation_sequence_number(operation_sequence_number)
    , m_step(step)
    , m_check_for_cancelation(check_for_cancelation)
    , m_initiator_to_check(initiator_to_check)
    , m_source_snapshot_params(move(source_snapshot_params))
    , m_user_involvement(user_involvement)
    , m_navigation_type(navigation_type)
    , m_generation(++m_traversable_state.generation_counter)
{
}

ApplyHistoryStep::~ApplyHistoryStep()
{
    if (m_running_synchronous_navigation_steps)
        m_traversable_state.running_nested_apply_history_step = false;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
void ApplyHistoryStep::apply_the_history_step()
{
    // FIXME: 1. Assert: This is running within traversable's session history traversal queue.

    // 2. Let targetStep be the result of getting the used step given traversable and step.
    m_target_step = m_session_history.get_the_used_step(m_step).value();

    // 3. If initiatorToCheck is not null, then:
    if (m_initiator_to_check.has_value()) {
        // 1. Assert: sourceSnapshotParams is not null.
        // NB: A traversal with an initiator must also include its sandboxing state. Reject an incomplete request.
        if (!m_source_snapshot_params.has_value()) {
            return_result(Web::HTML::HistoryStepResult::InitiatorDisallowed);
            return;
        }

        // 2. For each navigable of get all navigables whose current session history entry will change or reload:
        //    if initiatorToCheck is not allowed by sandboxing to navigate navigable given sourceSnapshotParams, then
        //    return "initiator-disallowed".
        auto initiator = m_traversable.find(*m_initiator_to_check);
        // NB: sourceSnapshotParams's sandboxing flags are its source document's, which the UI process holds too.
        Web::InitiatorSourceSnapshot source_snapshot_params {
            .sandboxing_flags = m_source_snapshot_params->sandboxing_flags,
            .has_transient_activation = m_source_snapshot_params->has_transient_activation,
        };
        if (initiator.has_value())
            source_snapshot_params.sandboxing_flags |= initiator->active_document().active_sandboxing_flag_set();
        for (auto& navigable : m_session_history.get_all_navigables_whose_current_session_history_entry_will_change_or_reload(m_traversable, m_target_step)) {
            // NB: A removed initiator is no longer related to any navigable in the canonical tree. Apply the sandboxed
            //     navigation flag instead of treating the missing relationship as permission.
            auto allowed = initiator.has_value()
                ? initiator->allowed_by_sandboxing_to_navigate(navigable, source_snapshot_params)
                : !has_flag(source_snapshot_params.sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedNavigation);
            if (!allowed) {
                return_result(Web::HTML::HistoryStepResult::InitiatorDisallowed);
                return;
            }
        }
    }

    // 4. Let navigablesCrossingDocuments be the result of getting all navigables that might experience a
    //    cross-document traversal given traversable and targetStep.
    auto navigables_crossing_documents = m_session_history.get_all_navigables_that_might_experience_a_cross_document_traversal(m_traversable, m_target_step);

    // 5. If checkForCancelation is true, and the result of checking if unloading is canceled given
    //    navigablesCrossingDocuments, traversable, targetStep, and userInvolvement is not "continue", then return
    //    that result.
    if (!m_check_for_cancelation) {
        get_changing_and_nonchanging_navigables();
        return;
    }

    auto* target_entry = m_session_history.get_the_target_history_entry(m_traversable, m_target_step);
    VERIFY(target_entry);

    m_traversable.check_if_unloading_is_canceled(*this, move(navigables_crossing_documents), *target_entry, m_user_involvement,
        [this](Web::HTML::CheckIfUnloadingIsCanceledResult result) {
            switch (result) {
            case Web::HTML::CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload:
                return_result(Web::HTML::HistoryStepResult::CanceledByBeforeUnload);
                return;
            case Web::HTML::CheckIfUnloadingIsCanceledResult::CanceledByNavigate:
                return_result(Web::HTML::HistoryStepResult::CanceledByNavigate);
                return;
            case Web::HTML::CheckIfUnloadingIsCanceledResult::Continue:
                get_changing_and_nonchanging_navigables();
                return;
            }
        });
}

void ApplyHistoryStep::get_changing_and_nonchanging_navigables()
{
    // 6. Let changingNavigables be the result of get all navigables whose current session history entry will change
    //    or reload given traversable and targetStep.
    auto changing_navigables = m_session_history.get_all_navigables_whose_current_session_history_entry_will_change_or_reload(m_traversable, m_target_step);

    // AD-HOC: Unconditionally populating a document here could unload and re-navigate a frame because an unrelated
    //         navigable was created or destroyed, which no other engine does. So an update for a navigable's creation
    //         or destruction leaves a navigable alone if a navigation has claimed it, or if the update would change its
    //         document. A navigable its creation marked as traversing is the exception: it populates the entry its
    //         nested history kept. See https://github.com/whatwg/html/issues/12724.
    if (!m_navigation_type.has_value()) {
        changing_navigables.remove_all_matching([&](CanonicalNavigable const& navigable) {
            auto const& target_entry = *m_session_history.get_the_target_history_entry(navigable, m_target_step);
            auto crosses_documents = !navigable.active_document_is(target_entry) || target_entry.document_state->reload_pending;
            return navigable.ongoing_navigation_id().has_value() || (crosses_documents && !navigable.ongoing_navigation().has<CanonicalNavigable::Traversal>());
        });
    }

    // 7. Let nonchangingNavigablesThatStillNeedUpdates be the result of getting all navigables that only need
    //    history object length/index update given traversable and targetStep.
    m_nonchanging_navigables_that_still_need_updates = m_session_history.get_all_navigables_that_only_need_history_object_length_index_update(m_traversable, m_target_step);

    // 8. For each navigable of changingNavigables:
    for (auto& navigable : changing_navigables) {
        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto* target_entry = m_session_history.get_the_target_history_entry(navigable, m_target_step);
        VERIFY(target_entry);

        m_changing_navigables.append(make<ChangingNavigable>(navigable, *target_entry));
        auto& changing_navigable = *m_changing_navigables.last();

        // 2. Set navigable's current session history entry to targetEntry.
        navigable.set_current_session_history_entry(target_entry);

        // 3. If targetEntry's document is not navigable's active document, then queue a global task on the navigation
        //    and traversal task source of navigable's active window to run these steps:
        if (!navigable.active_document_is(*target_entry))
            m_traversable.queue_navigation_api_state_clear_task(*this, changing_navigable);

        // 4. Set navigable's ongoing navigation to "traversal".
        set_ongoing_navigation_to_traversal(navigable, *target_entry);
    }

    run_changing_navigable_jobs();
}

void ApplyHistoryStep::run_changing_navigable_jobs()
{
    // 12. For each navigable of changingNavigables, queue a global task on the navigation and traversal task source of
    //     navigable's active window to run the steps:
    for (auto& changing_navigable : m_changing_navigables) {
        changing_navigable->target_entry_reload_pending = changing_navigable->target_entry->document_state->reload_pending;
        changing_navigable->target_entry_reload_bypasses_cache = changing_navigable->target_entry->document_state->reload_bypasses_cache;
        changing_navigable->displayed_document = changing_navigable->navigable().active_document();
        if (!m_traversable.run_changing_navigable_history_step_job(*this, *changing_navigable))
            end_changing_navigable_job(*changing_navigable);
    }

    process_changing_navigable_continuations();
}

void ApplyHistoryStep::changing_navigable_job_completed(ChangingNavigable& changing_navigable, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition)
{
    switch (disposition) {
    case Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready:
        // 4. Enqueue changingNavigableContinuation on changingNavigableContinuations.
        m_changing_navigable_continuations.append(&changing_navigable);
        break;
    case Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped:
        end_changing_navigable_job(changing_navigable);
        break;
    case Web::HTML::ChangingNavigableHistoryStepJobDisposition::Stale:
        // NB: A stale run completes with "applied" so callers waiting on it proceed; the newer navigation that made
        //     it stale owns the user-visible outcome.
        return_result(Web::HTML::HistoryStepResult::Applied);
        return;
    }

    process_changing_navigable_continuations();
}

// The job ends, without activating the document it populated.
void ApplyHistoryStep::end_changing_navigable_job(ChangingNavigable& changing_navigable)
{
    auto& navigable = changing_navigable.navigable();
    changing_navigable.job_ended = true;
    if (auto population_loader = move(changing_navigable.population_loader))
        population_loader->reclaim_response_body_after_failed_handoff();
    if (auto document = move(changing_navigable.populated_document))
        navigable.abandon_populated_document(changing_navigable.target_entry->document_state, *document);
    navigable.clear_ongoing_navigation_traversal(m_operation_id);

    m_changing_navigable_continuations.remove_first_matching([&](auto* continuation) { return continuation == &changing_navigable; });
    if (m_continuation_being_applied == &changing_navigable)
        m_continuation_being_applied = nullptr;

    // 14.12.6. Increment completedChangeJobs.
    // NB: Also for a job that ends without its continuation being applied.
    m_completed_change_jobs++;
}

// NB: The page each update was sent to owes its reply, so an update completes once.
void ApplyHistoryStep::nonchanging_navigable_updated()
{
    // 18.4. Increment completedNonchangingJobs.
    m_completed_nonchanging_jobs++;

    // 19. Wait for completedNonchangingJobs to equal totalNonchangingJobs.
    if (m_completed_nonchanging_jobs == m_nonchanging_navigables_that_still_need_updates.size())
        set_current_session_history_step();
}

void ApplyHistoryStep::process_changing_navigable_continuations()
{
    if (m_continuation_being_applied || m_running_synchronous_navigation_steps)
        return;

    // 14. While completedChangeJobs does not equal totalChangeJobs:
    while (m_completed_change_jobs != m_changing_navigables.size()) {
        // NOTE: Synchronous navigations that are intended to take place before this traversal jump the queue at this
        //       point, so they can be added to the correct place in traversable's session history entries before this
        //       traversal potentially unloads their document. More details can be found here:
        //       https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigation-steps-queue-jumping-examples
        // 1. If traversable's running nested apply history step is false, then:
        // AD-HOC: Applying synchronous navigation steps can continue asynchronously. Do not let a later synchronous
        //         navigation jump into that application. Both pushes could otherwise derive their target from the same
        //         current history step and the later push could remove the earlier entry as forward history.
        if (!m_session_history_traversal_queue.current_item_is_synchronous_navigation_steps()
            && !m_traversable_state.running_nested_apply_history_step) {
            // IPC allows WebContent to append more steps while a nested run is pending. Limit this pass to the steps present at its
            // start so a ready continuation cannot be starved.
            if (!m_synchronous_navigation_steps_to_jump_through.has_value())
                m_synchronous_navigation_steps_to_jump_through = m_session_history_traversal_queue.last_enqueued_sequence_number();

            // 1. While traversable's session history traversal queue's algorithm set contains one or more synchronous
            //    navigation steps with a target navigable not contained in navigablesThatMustWaitBeforeHandlingSyncNavigation:
            //    1. Let steps be the first item in traversable's session history traversal queue's algorithm set
            //       that is synchronous navigation steps with a target navigable not contained in
            //       navigablesThatMustWaitBeforeHandlingSyncNavigation.
            //    2. Remove steps from traversable's session history traversal queue's algorithm set.
            for (;;) {
                auto item = m_session_history_traversal_queue.take_first_synchronous_navigation_steps_not_targeting(
                    m_navigables_that_must_wait_before_handling_sync_navigation,
                    *m_synchronous_navigation_steps_to_jump_through);
                if (!item.has_value())
                    break;

                // 3. Set traversable's running nested apply history step to true.
                m_traversable_state.running_nested_apply_history_step = true;

                // 4. Run steps.
                auto promise = Core::Promise<Empty>::construct();
                item->steps(promise);

                // 5. Set traversable's running nested apply history step to false.
                if (promise->is_resolved()) {
                    m_traversable_state.running_nested_apply_history_step = false;
                    continue;
                }
                m_running_synchronous_navigation_steps = promise;
                promise->when_resolved([&traversable = m_traversable, operation_id = m_operation_id](Empty&) {
                    auto* run = traversable.find_history_step_run(operation_id);
                    if (!run)
                        return;
                    run->m_traversable_state.running_nested_apply_history_step = false;
                    run->m_running_synchronous_navigation_steps = nullptr;
                    run->process_changing_navigable_continuations();
                });
                return;
            }

            m_synchronous_navigation_steps_to_jump_through.clear();
        }

        // 2. Let changingNavigableContinuation be the result of dequeuing from changingNavigableContinuations.
        // 3. If changingNavigableContinuation is nothing, then continue.
        if (m_changing_navigable_continuations.is_empty())
            return;
        auto& changing_navigable_continuation = *m_changing_navigable_continuations.take_first();

        // NB: targetStep was computed before the asynchronous parts of this algorithm. Re-normalize it in case a
        //     child navigable's removal made it unused meanwhile.
        m_target_step = m_session_history.get_the_used_step(m_step).value();

        auto& navigable = changing_navigable_continuation.navigable();

        // AD-HOC: The spec has the continuation activate the target entry the job's task retained, without checking it
        //         against the session history again. But the sync navigations that jumped the queue above ran while the
        //         job was paused, and can have taken that entry out of the session history: A replace (replaceState
        //         during a reload, e.g.) puts a new entry in its slot, and a push clears it away with the rest of the
        //         forward history (pushState during a forward traversal, or while a navigation's own new entry is still
        //         ahead of the current step, e.g.). Activating such an entry gives its doc a navigation API entry list
        //         without the document's own entry in it, and leaves the navigable on an entry that the session history
        //         doesn't have. So this looks the target entry up again. A replacement in the claimed slot is the entry
        //         to activate now. A traversal whose slot is gone is stale — the push that cleared the slot owns the
        //         visible outcome. And a navigation whose new entry is gone appends it again, after the pushed entry.
        //         See https://github.com/whatwg/html/issues/12961.
        //
        //         Gecko/WebKit/Blink don't look a traversal's target up til the traversal runs, which is after any such
        //         pushState(): Gecko ChildSHistory::Go, WebKit ScheduledHistoryNavigation::fire, Blink
        //         NavigationControllerImpl::GoToOffsetFromRenderer. So in those, too, the push wins. And they don't add
        //         a navigation's entry til the navigation commits, which is after any such pushState() as well: Gecko
        //         CanonicalBrowsingContext::SessionHistoryCommit, WebKit HistoryController::updateForStandardLoad,
        //         Blink NavigationControllerImpl::RendererDidNavigateToNewEntry.
        // NB: navigable's own sync navigations wait from step 8 on. So, none of them can change the entry after this.
        if (auto* target_entry = m_session_history.get_the_target_history_entry(navigable, m_target_step)) {
            NonnullRefPtr claimed_target_entry = changing_navigable_continuation.target_entry;
            auto is_in_claimed_slot = target_entry->document_state == claimed_target_entry->document_state
                && target_entry->navigation_api_key == claimed_target_entry->navigation_api_key;
            if (is_in_claimed_slot) {
                // NB: The job follows a replacement, so that the entry recorded as the navigable's active one is the
                //     entry as the session history has it.
                changing_navigable_continuation.target_entry = *target_entry;
            } else if (m_navigation_type == Web::Bindings::NavigationType::Traverse) {
                return_result(Web::HTML::HistoryStepResult::Applied);
                return;
            } else if (m_navigation_type == Web::Bindings::NavigationType::Push) {
                append_the_claimed_target_entry_again(navigable, *claimed_target_entry);
            }
        }

        // 7. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and
        //    index given traversable and targetStep.
        auto history_object_length_and_index = m_session_history.get_the_history_object_length_and_index(m_target_step);

        // 8. Append navigable to navigablesThatMustWaitBeforeHandlingSyncNavigation.
        m_navigables_that_must_wait_before_handling_sync_navigation.set(changing_navigable_continuation.id);

        // 9. Let entriesForNavigationAPI be the result of getting session history entries for the navigation API
        //    given navigable and targetStep.
        auto entries_for_navigation_api = m_session_history.get_session_history_entries_for_the_navigation_api(navigable, m_target_step);
        if (!entries_for_navigation_api.has_value()) {
            // AD-HOC: The canonical mirror is reconciling the navigable, so its continuation cannot be applied.
            end_changing_navigable_job(changing_navigable_continuation);
            continue;
        }

        m_continuation_being_applied = &changing_navigable_continuation;
        changing_navigable_continuation.continuation = ChangingNavigable::Continuation {
            .history_object_length_and_index = history_object_length_and_index,
            .entries_for_navigation_api = entries_for_navigation_api.release_value(),
        };
        m_traversable.apply_changing_navigable_history_step_continuation(*this, changing_navigable_continuation);
        return;
    }

    update_nonchanging_navigables();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation
// A pushState that jumped the queue cleared the entry a navigation appended, as forward history, and took its step. So,
// we re-run the appending steps of finalizing a cross-doc navigation, against the session history as the push left it.
void ApplyHistoryStep::append_the_claimed_target_entry_again(CanonicalNavigable& navigable, CanonicalSessionHistoryEntry& claimed_target_entry)
{
    // 9.1. Clear the forward session history of traversable.
    // 9.2. Set targetStep to traversable's current session history step + 1.
    // 9.3. Set historyEntry's step to targetStep.
    // 9.4. Append historyEntry to targetEntries.
    auto target_step = m_session_history.push_session_history_entry(navigable, claimed_target_entry);

    // NB: The run now targets a step past the push-committed one. So it's the newest run again and may commit its step.
    m_step = target_step;
    m_target_step = target_step;
    m_generation = ++m_traversable_state.generation_counter;

    // The push also took the navigable's current session history entry over.
    navigable.set_current_session_history_entry(claimed_target_entry);
}

void ApplyHistoryStep::update_nonchanging_navigables()
{
    // NB: targetStep is re-normalized once more; the continuation loop can have removed child navigables.
    m_target_step = m_session_history.get_the_used_step(m_step).value();

    // 17. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index
    //     given traversable and targetStep.
    auto history_object_length_and_index = m_session_history.get_the_history_object_length_and_index(m_target_step);

    // 18. For each navigable of nonchangingNavigablesThatStillNeedUpdates, queue a global task on the navigation and
    //     traversal task source given navigable's active window to run the steps:
    for (auto navigable_id : m_nonchanging_navigables_that_still_need_updates) {
        if (!m_traversable.update_nonchanging_navigable_history_step_state(*this, navigable_id, history_object_length_and_index))
            m_completed_nonchanging_jobs++;
    }

    // 19. Wait for completedNonchangingJobs to equal totalNonchangingJobs.
    if (m_completed_nonchanging_jobs == m_nonchanging_navigables_that_still_need_updates.size())
        set_current_session_history_step();
}

void ApplyHistoryStep::set_current_session_history_step()
{
    // AD-HOC: Commit the target step only if no newer apply history step has moved the current step. A synchronous
    //         push jumping the queue while this step was paused has a newer target step; moving the current step back
    //         past it would let the next push assign a step number that an existing entry already holds. A synchronous
    //         replace that jumped the queue re-commits the step it started from and moves nothing, so it doesn't stand
    //         in the way: A cross-document navigation that was paused while the page it's unloading replaced an entry
    //         (a document.open() in one of its iframes, or a replaceState() call) still commits its own step.
    //         See https://github.com/whatwg/html/issues/12576.
    if (m_generation > m_traversable_state.committed_generation) {
        // 20. Set traversable's current session history step to targetStep.
        auto used_target_step = m_session_history.get_the_used_step(m_target_step).value();
        if (m_session_history.current_step() != used_target_step)
            m_traversable_state.committed_generation = m_generation;
        m_session_history.set_current_session_history_step(used_target_step);
        m_committed_step = used_target_step;
    }

    // 21. Return "applied".
    return_result(Web::HTML::HistoryStepResult::Applied);
}

void ApplyHistoryStep::abandon()
{
    for (auto& changing_navigable : m_changing_navigables) {
        if (changing_navigable->has_pending_job())
            end_changing_navigable_job(*changing_navigable);
    }
}

void ApplyHistoryStep::return_result(Web::HTML::HistoryStepResult result)
{
    m_traversable.finish_history_operation(m_operation_id, result);
}

Web::HTML::SessionHistoryEntryDescriptor ChangingNavigable::target_entry_descriptor() const
{
    auto descriptor = target_entry->descriptor();
    descriptor.document_state.reload_pending = target_entry_reload_pending;
    descriptor.document_state.reload_bypasses_cache = target_entry_reload_bypasses_cache;
    return descriptor;
}

void ApplyHistoryStep::set_ongoing_navigation_to_traversal(CanonicalNavigable& navigable, CanonicalSessionHistoryEntry const& target_entry)
{
    // AD-HOC: As in browsers, a push or replace leaves the ongoing navigation alone: a newer navigation goes on, and
    //         commits after it. So does a same-document traversal, where the spec sets "traversal" and so drops the
    //         navigation. See https://github.com/whatwg/html/issues/12581.
    // NB: An update for a navigable's creation or destruction changes the document of no navigable but one its creation
    //     already marked as traversing.
    if (m_navigation_type != Web::Bindings::NavigationType::Traverse && m_navigation_type != Web::Bindings::NavigationType::Reload)
        return;
    if (navigable.active_document_is(target_entry) && !target_entry.document_state->reload_pending)
        return;

    // AD-HOC: A navigation admitted after a reload goes on, as the newest, and the reload leaves the navigable to it,
    //         its target entry being current already. A traversal that crosses documents has made current an entry
    //         whose document only it activates, and aborts the navigation, as specified. The specification's traversal
    //         queue does not model Ladybird's independently admitted UI-process navigations.
    //         See https://github.com/whatwg/html/issues/12581.
    if (m_navigation_type == Web::Bindings::NavigationType::Reload) {
        auto ongoing_navigation_id = navigable.ongoing_navigation_id();
        auto ongoing_navigation = ongoing_navigation_id.has_value() ? navigable.navigation_with_id(*ongoing_navigation_id) : nullptr;
        if (ongoing_navigation && ongoing_navigation->sequence_number > m_operation_sequence_number)
            return;
    }

    // The marker cancels navigation-start requests that arrive while this operation is on the traversal queue, matching
    // navigate()'s "if navigable's ongoing navigation is 'traversal', then return".
    navigable.set_ongoing_navigation_to_traversal(m_operation_id);
}

ChangingNavigable* ApplyHistoryStep::changing_navigable(Web::HTML::CrossProcessId navigable_id) const
{
    auto it = m_changing_navigables.find_if([&](auto const& changing_navigable) { return changing_navigable->id == navigable_id; });
    return it == m_changing_navigables.end() ? nullptr : it->ptr();
}

}
