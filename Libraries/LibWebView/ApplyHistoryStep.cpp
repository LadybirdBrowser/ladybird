/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/ApplyHistoryStep.h>
#include <LibWebView/CanonicalNavigable.h>

namespace WebView {

ApplyHistoryStep::ApplyHistoryStep(
    TraversableSessionHistory& session_history,
    CanonicalNavigable& traversable_navigable,
    SessionHistoryTraversalQueue& session_history_traversal_queue,
    TraversableApplyHistoryStepState& traversable_state,
    ApplyHistoryStepJobs jobs,
    i32 step,
    bool check_for_cancelation,
    Optional<Web::HTML::CrossProcessId> initiator_to_check,
    Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot,
    Web::HTML::UserNavigationInvolvement user_involvement,
    Optional<Web::Bindings::NavigationType> navigation_type,
    Function<void(Web::HTML::HistoryStepResult)> on_complete)
    : m_session_history(session_history)
    , m_traversable_navigable(traversable_navigable)
    , m_session_history_traversal_queue(session_history_traversal_queue)
    , m_traversable_state(traversable_state)
    , m_jobs(move(jobs))
    , m_step(step)
    , m_check_for_cancelation(check_for_cancelation)
    , m_initiator_to_check(initiator_to_check)
    , m_initiator_source_snapshot(initiator_source_snapshot)
    , m_user_involvement(user_involvement)
    , m_navigation_type(navigation_type)
    , m_on_complete(move(on_complete))
    , m_generation(++traversable_state.generation_counter)
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
    auto target_step = m_session_history.get_the_used_step(m_step);
    if (!target_step.has_value()) {
        // AD-HOC: An empty canonical session history has no used steps to resolve against.
        return_result(Web::HTML::HistoryStepResult::CanceledByMissingPage);
        return;
    }
    m_target_step = *target_step;

    // 3. If initiatorToCheck is not null, then:
    if (m_initiator_to_check.has_value()) {
        // 1. Assert: sourceSnapshotParams is not null.
        // NB: A traversal with an initiator must also include its sandboxing state. Reject an incomplete request.
        if (!m_initiator_source_snapshot.has_value()) {
            return_result(Web::HTML::HistoryStepResult::InitiatorDisallowed);
            return;
        }

        // 2. For each navigable of get all navigables whose current session history entry will change or reload:
        //    if initiatorToCheck is not allowed by sandboxing to navigate navigable given sourceSnapshotParams, then
        //    return "initiator-disallowed".
        auto const* initiator = find_navigable(*m_initiator_to_check);
        for (auto navigable_id : m_session_history.get_all_navigables_whose_current_session_history_entry_will_change_or_reload(m_traversable_navigable, m_target_step)) {
            auto const* navigable = find_navigable(navigable_id);
            if (!navigable)
                continue;

            // NB: A removed initiator is no longer related to any navigable in the canonical tree. Apply the sandboxed
            //     navigation flag instead of treating the missing relationship as permission.
            auto allowed = initiator
                ? initiator->allowed_by_sandboxing_to_navigate(*navigable, *m_initiator_source_snapshot)
                : !has_flag(m_initiator_source_snapshot->sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedNavigation);
            if (!allowed) {
                return_result(Web::HTML::HistoryStepResult::InitiatorDisallowed);
                return;
            }
        }
    }

    // 4. Let navigablesCrossingDocuments be the result of getting all navigables that might experience a
    //    cross-document traversal given traversable and targetStep.
    auto navigables_crossing_documents = m_session_history.get_all_navigables_that_might_experience_a_cross_document_traversal(m_traversable_navigable, m_target_step);

    // 5. If checkForCancelation is true, and the result of checking if unloading is canceled given
    //    navigablesCrossingDocuments, traversable, targetStep, and userInvolvement is not "continue", then return
    //    that result.
    if (!m_check_for_cancelation) {
        get_changing_and_nonchanging_navigables();
        return;
    }

    auto const* target_entry = m_session_history.get_the_target_history_entry(m_traversable_navigable, m_target_step);
    if (!target_entry) {
        return_result(Web::HTML::HistoryStepResult::CanceledByMissingPage);
        return;
    }

    m_jobs.run_unload_cancelation_job({
                                          .target_entry = *target_entry,
                                          .navigables_crossing_documents = move(navigables_crossing_documents),
                                          .user_involvement = m_user_involvement,
                                      },
        [this](Web::HTML::HistoryStepResult result) {
            if (m_completed)
                return;
            if (result != Web::HTML::HistoryStepResult::Applied) {
                return_result(result);
                return;
            }
            get_changing_and_nonchanging_navigables();
        });
}

void ApplyHistoryStep::get_changing_and_nonchanging_navigables()
{
    // 6. Let changingNavigables be the result of get all navigables whose current session history entry will change
    //    or reload given traversable and targetStep.
    m_changing_navigables = m_session_history.get_all_navigables_whose_current_session_history_entry_will_change_or_reload(m_traversable_navigable, m_target_step);

    // 7. Let nonchangingNavigablesThatStillNeedUpdates be the result of getting all navigables that only need
    //    history object length/index update given traversable and targetStep.
    m_nonchanging_navigables_that_still_need_updates = m_session_history.get_all_navigables_that_only_need_history_object_length_index_update(m_traversable_navigable, m_target_step);

    // 8. For each navigable of changingNavigables:
    for (auto navigable_id : m_changing_navigables) {
        auto* navigable = find_navigable(navigable_id);

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto const* target_entry = navigable ? m_session_history.get_the_target_history_entry(*navigable, m_target_step) : nullptr;
        if (!target_entry)
            continue;

        // 2. Set navigable's current session history entry to targetEntry.
        navigable->set_current_session_history_entry(*target_entry);
    }

    // NB: Steps 8.3 and 8.4 happen inside each dispatched job, because they mutate state owned by the process running
    //     the job. The changing set is complete before the first job is dispatched, so job completions cannot advance
    //     the algorithm early.
    run_changing_navigable_jobs();
}

void ApplyHistoryStep::run_changing_navigable_jobs()
{
    auto navigation_api_abort_behavior = Web::HTML::LocalNavigable::NavigationAPIAbortBehavior::Abort;
    // Same-document traversals finish their NavigateEvent while updating the Navigation API entry. This decision is
    // traversable-wide, so derive it from canonical history before dispatching per-document jobs.
    if (m_navigation_type == Web::Bindings::NavigationType::Traverse
        && m_session_history.get_all_navigables_that_might_experience_a_cross_document_traversal(m_traversable_navigable, m_target_step).is_empty())
        navigation_api_abort_behavior = Web::HTML::LocalNavigable::NavigationAPIAbortBehavior::Preserve;

    // 12. For each navigable of changingNavigables, queue a global task on the navigation and traversal task source.
    for (auto navigable_id : m_changing_navigables) {
        auto const* navigable = find_navigable(navigable_id);
        auto const* target_entry = navigable ? m_session_history.get_the_target_history_entry(*navigable, m_target_step) : nullptr;
        if (!target_entry) {
            // AD-HOC: The canonical mirror can briefly disagree with the live navigable tree while a created or
            //         removed child navigable is reconciled; complete the job as skipped instead of dispatching it.
            changing_navigable_job_completed(navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);
            continue;
        }

        ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job {
            .navigable_id = navigable_id,
            .target_entry = *target_entry,
            .user_involvement = m_user_involvement,
            .navigation_type = m_navigation_type,
            .navigation_api_abort_behavior = navigation_api_abort_behavior,
        };
        if (!m_jobs.select_changing_navigable_history_step_job_endpoint(job)) {
            changing_navigable_job_completed(navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);
            continue;
        }

        m_jobs.run_changing_navigable_history_step_job(
            move(job),
            [this, navigable_id](Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition) {
                changing_navigable_job_completed(navigable_id, disposition);
            });
    }

    if (m_changing_navigables.is_empty())
        process_changing_navigable_continuations();
}

void ApplyHistoryStep::changing_navigable_job_completed(Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition)
{
    if (m_completed)
        return;

    switch (disposition) {
    case Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready:
        // 4. Enqueue changingNavigableContinuation on changingNavigableContinuations.
        m_changing_navigable_continuations.append(navigable_id);
        break;
    case Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped:
        m_completed_change_jobs++;
        break;
    case Web::HTML::ChangingNavigableHistoryStepJobDisposition::Stale:
        // NB: A stale run completes with "applied" so callers waiting on it proceed; the newer navigation that made
        //     it stale owns the user-visible outcome.
        return_result(Web::HTML::HistoryStepResult::Applied);
        return;
    }

    // NB: Continuations are processed once every changing navigable's job has reported. The specification's loop can
    //     interleave them with still-populating jobs; keeping population and application in separate rounds preserves
    //     the ordering the WebContent-local coordinator used.
    if (m_changing_navigable_continuations.size() + m_completed_change_jobs == m_changing_navigables.size())
        process_changing_navigable_continuations();
}

void ApplyHistoryStep::process_changing_navigable_continuations()
{
    // 14. While completedChangeJobs does not equal totalChangeJobs:
    for (;;) {
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
                promise->when_resolved([weak_this = make_weak_ptr()](Empty&) {
                    if (!weak_this)
                        return;
                    weak_this->m_traversable_state.running_nested_apply_history_step = false;
                    weak_this->m_running_synchronous_navigation_steps = nullptr;
                    weak_this->process_changing_navigable_continuations();
                });
                return;
            }

            m_synchronous_navigation_steps_to_jump_through.clear();
        }

        // 3. Let changingNavigableContinuation be the result of dequeuing from changingNavigableContinuations.
        //    If nothing was dequeued, then continue.
        // NB: Every changing navigable's job has reported by the time this loop runs, so an empty queue means
        //     completedChangeJobs equals totalChangeJobs.
        if (m_changing_navigable_continuations.is_empty()) {
            update_nonchanging_navigables();
            return;
        }
        auto navigable_id = m_changing_navigable_continuations.take_first();

        // NB: targetStep was computed before the asynchronous parts of this algorithm. Re-normalize it in case a
        //     child navigable's removal made it unused meanwhile.
        m_target_step = m_session_history.get_the_used_step(m_step).value_or(m_target_step);

        // 7. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and
        //    index given traversable and targetStep.
        auto const* navigable = find_navigable(navigable_id);
        auto history_object_length_and_index = m_session_history.get_the_history_object_length_and_index(m_target_step);

        // 8. Append navigable to navigablesThatMustWaitBeforeHandlingSyncNavigation.
        m_navigables_that_must_wait_before_handling_sync_navigation.set(navigable_id);

        // 9. Let entriesForNavigationAPI be the result of getting session history entries for the navigation API
        //    given navigable and targetStep.
        auto entries_for_navigation_api = navigable ? m_session_history.get_session_history_entries_for_the_navigation_api(*navigable, m_target_step) : Optional<Vector<TraversableSessionHistory::Entry>> {};
        if (!navigable || !history_object_length_and_index.has_value() || !entries_for_navigation_api.has_value()) {
            // AD-HOC: The navigable is gone, or the canonical mirror is reconciling it. Its continuation cannot be
            //         applied; count the job as completed and move on.
            m_completed_change_jobs++;
            continue;
        }

        // NB: One continuation is applied at a time, so synchronous navigations queued by its application are in the
        //     traversal queue before the next continuation checks for them.
        m_jobs.apply_changing_navigable_history_step_continuation(
            {
                .navigable_id = navigable_id,
                .history_object_length_and_index = *history_object_length_and_index,
                .entries_for_navigation_api = entries_for_navigation_api.release_value(),
            },
            [this] {
                if (m_completed)
                    return;
                // 10. Increment completedChangeJobs.
                m_completed_change_jobs++;
                process_changing_navigable_continuations();
            });
        return;
    }
}

void ApplyHistoryStep::update_nonchanging_navigables()
{
    // NB: targetStep is re-normalized once more; the continuation loop can have removed child navigables.
    m_target_step = m_session_history.get_the_used_step(m_step).value_or(m_target_step);

    // 17. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index
    //     given traversable and targetStep.
    auto history_object_length_and_index = m_session_history.get_the_history_object_length_and_index(m_target_step);
    if (!history_object_length_and_index.has_value() || m_nonchanging_navigables_that_still_need_updates.is_empty()) {
        set_current_session_history_step();
        return;
    }

    // 18. For each navigable of nonchangingNavigablesThatStillNeedUpdates, queue a global task on the navigation and
    //     traversal task source given navigable's active window to run the steps:
    for (auto navigable_id : m_nonchanging_navigables_that_still_need_updates) {
        m_jobs.update_nonchanging_navigable_history_step_state(navigable_id, *history_object_length_and_index, [this] {
            if (m_completed)
                return;
            // 4. Increment completedNonchangingJobs.
            m_completed_nonchanging_jobs++;

            // 19. Wait for completedNonchangingJobs to equal totalNonchangingJobs.
            if (m_completed_nonchanging_jobs == m_nonchanging_navigables_that_still_need_updates.size())
                set_current_session_history_step();
        });
    }
}

void ApplyHistoryStep::set_current_session_history_step()
{
    // AD-HOC: Commit the target step only if no newer apply history step has committed one. A synchronous navigation
    //         jumping the queue while this step was paused has a newer target step; moving the current step back past
    //         it would let the next push assign a step number that an existing entry already holds.
    //         See https://github.com/whatwg/html/issues/12576.
    if (m_generation > m_traversable_state.committed_generation) {
        m_traversable_state.committed_generation = m_generation;

        // 20. Set traversable's current session history step to targetStep.
        auto used_target_step = m_session_history.get_the_used_step(m_target_step);
        if (used_target_step.has_value()) {
            m_session_history.set_current_session_history_step(*used_target_step);
            m_committed_step = *used_target_step;
        }
    }

    // 21. Return "applied".
    return_result(Web::HTML::HistoryStepResult::Applied);
}

void ApplyHistoryStep::return_result(Web::HTML::HistoryStepResult result)
{
    if (m_completed)
        return;
    m_completed = true;
    if (m_on_complete)
        m_on_complete(result);
}

CanonicalNavigable* ApplyHistoryStep::find_navigable(Web::HTML::CrossProcessId navigable_id)
{
    CanonicalNavigable* result = nullptr;
    m_traversable_navigable.for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
        if (navigable.id() != navigable_id)
            return IterationDecision::Continue;
        result = &navigable;
        return IterationDecision::Break;
    });
    return result;
}

}
