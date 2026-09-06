/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HistoryExecutor.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationParamsDescriptor.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/TargetSnapshotParams.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::HTML {

// The state a changing navigable's history step job retains for its continuation.
struct ChangingNavigableContinuationState : public GC::Cell {
    GC_CELL(ChangingNavigableContinuationState, GC::Cell);
    GC_DECLARE_ALLOCATOR(ChangingNavigableContinuationState);

    GC::Ptr<DOM::Document> displayed_document;
    Optional<UniqueNodeID> displayed_document_id;
    RefPtr<SessionHistoryEntry> target_entry;
    GC::Ptr<LocalNavigable> navigable;
    bool update_only = false;
    Optional<Bindings::NavigationType> navigation_type;
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
GC_DEFINE_ALLOCATOR(HistoryExecutor);

HistoryExecutor::HistoryExecutor(Page& page)
    : m_page(page)
{
}

void HistoryExecutor::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    for (auto& operation : m_history_operations) {
        visitor.visit(operation.value.source_snapshot_params);
        visitor.visit(operation.value.pending_document);
        visitor.visit(operation.value.expected_ongoing_navigation_navigable);
        visitor.visit(operation.value.pre_steps);
        visitor.visit(operation.value.on_apply_complete);
        visitor.visit(operation.value.on_complete);
        for (auto& population : operation.value.pending_populations)
            visitor.visit(population.value);
        for (auto& continuation : operation.value.changing_navigable_continuations)
            visitor.visit(continuation.value);
    }
}

bool HistoryExecutor::HistoryOperationState::expected_ongoing_navigation_was_superseded() const
{
    if (!expected_ongoing_navigation_navigable || !expected_ongoing_navigation_id.has_value())
        return false;
    auto navigable = local_navigable_with_id(expected_ongoing_navigation_navigable->id());
    if (!navigable)
        return true;
    if (navigable->has_been_destroyed())
        return true;
    return navigable->ongoing_navigation() != *expected_ongoing_navigation_id;
}

HistoryExecutor::HistoryOperationState* HistoryExecutor::find_history_operation(CrossProcessId operation_id)
{
    auto operation = m_history_operations.find(operation_id);
    if (operation == m_history_operations.end())
        return nullptr;
    return &operation->value;
}

HistoryExecutor::HistoryOperationState& HistoryExecutor::ensure_history_operation(CrossProcessId operation_id)
{
    return m_history_operations.ensure(operation_id);
}

void HistoryExecutor::request_history_operation(HistoryOperationParameters parameters)
{
    request_history_operation(move(parameters), {});
}

void HistoryExecutor::request_history_operation(HistoryOperationParameters parameters, HistoryOperationState state)
{
    auto operation_id = m_page->client().allocate_cross_process_id();
    m_history_operations.set(operation_id, move(state));
    m_page->client().page_did_request_history_operation(operation_id, move(parameters));
}

void HistoryExecutor::handle_ui_history_operation_started(CrossProcessId operation_id, Optional<Web::ReconstructedChildNavigation> reconstructed_child_navigation, GC::Ref<OnHistoryOperationReady> ready)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation) {
        ready->function()(HistoryStepResult::Applied);
        return;
    }

    // NB: A cross-document navigation can be superseded after its document has populated but before its queued
    //     history-step application runs. The navigate algorithm's earlier navigation ID check caught the same
    //     condition before requesting this operation; this re-check keeps a stale finalization from claiming
    //     "traversal" and canceling the newer navigation.
    if (operation->expected_ongoing_navigation_was_superseded()) {
        ready->function()(HistoryStepResult::Applied);
        return;
    }

    if (operation->pre_steps) {
        operation->pre_steps->function()(move(reconstructed_child_navigation), ready);
        return;
    }
    ready->function()(Empty {});
}

void HistoryExecutor::complete_ui_history_operation(CrossProcessId operation_id, HistoryStepResult result, Optional<i32> committed_step)
{
    auto operation = m_history_operations.take(operation_id);
    if (!operation.has_value())
        return;

    // AD-HOC: A canceled or stale operation can leave a claimed navigable whose continuation was never applied.
    for (auto navigable_id : operation->claimed_navigables_awaiting_continuation) {
        if (auto navigable = local_navigable_with_id(navigable_id))
            navigable->clear_ongoing_history_traversal();
    }

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

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void HistoryExecutor::traverse_the_history_by_delta(int delta, GC::Ptr<DOM::Document> source_document)
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

void HistoryExecutor::finalize_same_document_navigation(GC::Ref<LocalNavigable> target_navigable, NonnullRefPtr<SessionHistoryEntry> target_entry, RefPtr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior history_handling, UserNavigationInvolvement user_involvement, Optional<SessionHistoryEntryPersistedState> previous_entry_persisted_state)
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

// Fire beforeunload for the documents hosted by this process.
void HistoryExecutor::run_ui_history_step_beforeunload_check(Vector<CrossProcessId> navigable_ids, UnloadPromptShown unload_prompt_shown, GC::Ref<GC::Function<void(HistoryStepResult, UnloadPromptShown)>> on_complete)
{
    Vector<GC::Root<LocalNavigable>> navigables;
    navigables.ensure_capacity(navigable_ids.size());
    for (auto navigable_id : navigable_ids) {
        if (auto navigable = local_navigable_with_id(navigable_id); navigable && !navigable->has_been_destroyed() && navigable->active_document())
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

static bool is_same_document_push_or_replace(Optional<Bindings::NavigationType> navigation_type, SessionHistoryEntry const& target_entry, Optional<UniqueNodeID> displayed_document_id)
{
    if (navigation_type != Bindings::NavigationType::Push
        && navigation_type != Bindings::NavigationType::Replace) {
        return false;
    }

    return target_entry.document_state()->document_id() == displayed_document_id;
}

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

bool HistoryExecutor::run_changing_navigable_history_step_job_impl(ChangingNavigableHistoryStepJob job, GC::Ptr<SourceSnapshotParams> source_snapshot_params, GC::Ptr<DOM::Document> pending_document, GC::Ref<OnLocalChangingNavigableHistoryStepJobComplete> on_complete)
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

    // AD-HOC: The UI operation owns the authoritative traversal tracker. LocalNavigable still uses this process-local
    //         value to serialize pending navigations, so mirror it when the already-required changing job begins. A
    //         same-document push or replace can arrive after a newer navigation installed its local ID, in which case
    //         that navigation wins.
    auto preserve_ongoing_navigation = applies_same_document_push_or_replace
        && navigable->ongoing_navigation().has<Utf16String>();
    if (!preserve_ongoing_navigation)
        navigable->set_ongoing_navigation_without_informing_navigation_api(HTML::LocalNavigable::Traversal::Tag);

    queue_apply_history_step_task(*navigable, navigable->active_document(), GC::create_function(heap(), [this, job = move(job), source_snapshot_params, pending_document, claimed_target_entry = move(claimed_target_entry), navigable, on_complete]() mutable {
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
            //
            //         A synchronous navigation that jumps the queue during this run's apply-history-step re-claims
            //         the navigable the same way, but does not supersede a traversal, reload, or cross-document
            //         navigation: those runs still apply their claimed target entry once the jumped navigation's
            //         nested run completes. A navigation proven newer completes the job as stale before this task
            //         is queued.
            auto reclaimed_by_cooperating_synchronous_navigation = target_entry
                && job.navigation_type.has_value()
                && !is_same_document_push_or_replace(job.navigation_type, *claimed_target_entry, navigable->active_document_id());
            if (!reclaimed_by_cooperating_synchronous_navigation) {
                on_complete->function()({ ChangingNavigableHistoryStepJobDisposition::Stale, nullptr });
                return;
            }
            target_entry = claimed_target_entry;
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
            auto continue_population = GC::create_function(heap(), [this, navigable, after_document_populated](HistoryNavigationPopulation population) {
                if (navigable->has_been_destroyed() || !navigable->active_window()) {
                    after_document_populated->function()(nullptr);
                    return;
                }
                auto& request = population.request;
                auto& result = population.result;
                auto& realm = navigable->active_window()->principal_realm();
                TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
                auto params = create_navigation_params_from_descriptor(realm, *navigable, move(result.navigation_params));
                if (params.is_error()) {
                    after_document_populated->function()(nullptr);
                    return;
                }
                auto output = heap().allocate<PopulateSessionHistoryEntryDocumentOutput>();
                output->redirected_url = move(result.redirected_url);
                output->classic_history_api_state = move(result.classic_history_api_state);
                output->resource_cleared = result.resource_cleared;
                if (result.replacement_document_state.has_value()) {
                    output->replacement_document_state = DocumentState::create(result.replacement_document_state->id);
                    apply_session_history_document_state_descriptor_from_ui_process(*output->replacement_document_state, *result.replacement_document_state);
                }
                auto fetch_client_origin = request.source_snapshot_params.fetch_client.has_value()
                    ? Optional<URL::Origin> { request.source_snapshot_params.fetch_client->origin }
                    : Optional<URL::Origin> {};
                navigable->queue_navigation_and_traversal_task_for_session_history_entry_population(
                    request.history_entry.url, request.source_snapshot_params.allows_downloading, move(fetch_client_origin),
                    request.user_involvement, {}, params.release_value(), request.csp_navigation_type,
                    request.history_entry.document_state.reload_pending ? Bindings::NavigationTimingType::Reload : Bindings::NavigationTimingType::BackForward,
                    output, GC::create_function(heap(), [navigable, after_document_populated](GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> output) {
                        queue_apply_history_step_task(*navigable, navigable->active_document(), GC::create_function(navigable->heap(), [after_document_populated, output] {
                            after_document_populated->function()(output);
                        }));
                    }));
            });
            if (job.population.has_value()) {
                continue_population->function()(job.population.release_value());
                return;
            }
            auto* operation = find_history_operation(job.operation_id);
            if (!operation)
                return;
            operation->pending_populations.set(navigable->id(), continue_population);

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
            m_page->client().page_did_set_session_history_entry_document_state_reload_pending(
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

            auto request = NavigationPopulationRequest {
                .navigable_id = navigable->id(),
                .history_entry = create_pending_session_history_entry_descriptor(create_session_history_entry_descriptor(*target_entry)),
                .source_snapshot_params = job.source_snapshot.has_value() ? job.source_snapshot.release_value() : create_navigation_source_snapshot(*potentially_target_specific_source_snapshot_params),
                .target_snapshot_params = target_snapshot_params,
                .csp_navigation_type = ContentSecurityPolicy::Directives::Directive::NavigationType::Other,
                .history_handling = Bindings::NavigationHistoryBehavior::Replace,
                .user_involvement = job.user_involvement,
                .navigation_id = {},
            };
            request.history_entry.document_state.reload_pending = input_reload_pending;

            // 7. In parallel, attempt to populate the history entry's document for targetEntry, given navigable, potentiallyTargetSpecificSourceSnapshotParams,
            //    targetSnapshotParams, userInvolvement, with allowPOST set to allowPOST and completionSteps set to
            //    queue a global task on the navigation and traversal task source given navigable's active window to
            //    run afterDocumentPopulated.
            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [operation_id = job.operation_id, request = move(request), input_url = move(input_url), input_document_resource = move(input_document_resource), input_request_referrer = move(input_request_referrer), input_request_referrer_policy, input_initiator_origin = move(input_initiator_origin), input_origin = move(input_origin), input_history_policy_container = move(input_history_policy_container), input_about_base_url = move(input_about_base_url), input_navigable_target_name = move(input_navigable_target_name), input_reload_pending, input_ever_populated, potentially_target_specific_source_snapshot_params, target_snapshot_params, this, allow_POST, navigable, user_involvement = job.user_involvement] {
                navigable->populate_session_history_entry_document(
                    move(input_url), move(input_document_resource), move(input_request_referrer),
                    input_request_referrer_policy, move(input_initiator_origin), move(input_origin),
                    input_history_policy_container, move(input_about_base_url), move(input_navigable_target_name),
                    input_reload_pending, input_ever_populated,
                    *potentially_target_specific_source_snapshot_params, target_snapshot_params,
                    user_involvement, {}, LocalNavigable::NullOrError {},
                    ContentSecurityPolicy::Directives::Directive::NavigationType::Other, allow_POST,
                    {}, GC::create_function(heap(), [this, operation_id, request](NavigationPopulationResult result) mutable {
                        m_page->client().history_navigation_params_creation_finished(operation_id, { move(request), move(result) });
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

void HistoryExecutor::apply_changing_navigable_history_step_continuation_impl(GC::Ref<ChangingNavigableContinuationState> continuation, LocalApplyChangingNavigableHistoryStepContinuation command, UnloadDisplayedDocument unload_displayed_document_choice, GC::Ref<GC::Function<void(Optional<ReplicatedNavigableState>, Optional<SessionHistoryEntryPersistedState>)>> on_complete)
{
    // 4. Let displayedDocument be changingNavigableContinuation's displayed document.
    auto displayed_document = continuation->displayed_document;

    // 5. Let targetEntry be changingNavigableContinuation's target entry.
    RefPtr<SessionHistoryEntry> const target_entry = continuation->target_entry;

    auto population_output = continuation->population_output;
    auto old_origin = continuation->old_origin;

    // 6. Let navigable be changingNavigableContinuation's navigable.
    auto navigable = continuation->navigable;

    if (unload_displayed_document_choice == UnloadDisplayedDocument::Yes) {
        VERIFY(!continuation->update_only);
        VERIFY(continuation->resolved_document.ptr() != displayed_document.ptr());
    }

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
    auto entries_for_navigation_api = move(command.entries_for_navigation_api);

    // NOTE: Steps 10 and 11 come after step 12.

    // 12. In both cases, let afterPotentialUnloads be the following steps:
    bool const update_only = continuation->update_only;
    auto const displayed_document_id = continuation->displayed_document_id;
    auto after_potential_unload = GC::create_function(heap(), [this, navigable, update_only, unload_displayed_document_choice, target_entry, continuation, population_output, old_origin, displayed_document_id, script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), system_visibility_state = command.system_visibility_state, navigation_type = continuation->navigation_type, on_complete] {
        if (unload_displayed_document_choice == UnloadDisplayedDocument::No) {
            auto applies_same_document_push_or_replace = is_same_document_push_or_replace(
                navigation_type, *target_entry, displayed_document_id);

            if (applies_same_document_push_or_replace
                && navigable->active_session_history_entry() != target_entry) {
                navigable->clear_ongoing_history_traversal();
                on_complete->function()({}, {});
                return;
            }

            // AD-HOC: Child navigable same-document/update-only tasks are queued without an associated Document so
            //         they can survive the old active Document being deactivated. That also lets them run after the
            //         child frame was destroyed or after a newer navigation claimed the frame. Browser engines let
            //         the newer frame state win, so skip this stale continuation in that case.
            if (!changing_navigable_is_still_current(navigable, displayed_document_id, applies_same_document_push_or_replace)) {
                navigable->clear_ongoing_history_traversal();
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
            navigable->activate_history_entry(*target_entry, *resolved_document, system_visibility_state);
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

    if (unload_displayed_document_choice == UnloadDisplayedDocument::No) {
        // 10.2. Queue a global task on the navigation and traversal task source given navigable's active window to perform afterPotentialUnloads.
        // Mirror the traversal queue's ongoing-navigation transition in the local projection.
        navigable->clear_ongoing_history_traversal();
        queue_apply_history_step_task(*navigable, navigable->active_document(), after_potential_unload);
    } else {
        // 11. Otherwise:
        // The UI process has already unloaded every descendant subtree of displayedDocument.
        // Queue the final task to unload displayedDocument and run afterPotentialUnloads.
        // 6. Queue a global task on the navigation and traversal task source given document's relevant global object to perform the following steps:
        // NB: Use a null document so this remains runnable after displayedDocument becomes non-fully-active.
        queue_a_task(Task::Source::NavigationAndTraversal, nullptr, nullptr,
            GC::create_function(heap(), [displayed_document = GC::Ref { *displayed_document }, new_document = continuation->resolved_document, after_potential_unload] {
                // FIXME: 1. If firePageSwapSteps is given, then run firePageSwapSteps.

                // 2. Unload document, passing along newDocument if it is not null.
                displayed_document->unload(new_document);

                // 3. If afterAllUnloads was given, then run it.
                after_potential_unload->function()();
            }));
    }
}

bool HistoryExecutor::resume_history_navigation_population(CrossProcessId operation_id, HistoryNavigationPopulation&& population)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation)
        return false;
    auto continuation = operation->pending_populations.take(population.request.navigable_id);
    if (!continuation.has_value())
        return false;
    continuation.value()->function()(move(population));
    return true;
}

void HistoryExecutor::run_ui_changing_navigable_history_job(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryDescriptor target_entry, UserNavigationInvolvement user_involvement, Optional<Bindings::NavigationType> navigation_type, bool superseded_by_newer_navigation, GC::Ref<OnChangingNavigableHistoryStepJobComplete> on_complete, Optional<HistoryNavigationPopulation> population)
{
    auto& operation = ensure_history_operation(operation_id);
    auto source_snapshot_params = operation.source_snapshot_params;
    auto pending_document = operation.pending_document;
    RefPtr<SessionHistoryEntry> local_target_entry;
    if (operation.local_target_navigable_id == navigable_id) {
        VERIFY(operation.local_target_entry);
        local_target_entry = operation.local_target_entry;
    }
    if (operation.expected_ongoing_navigation_was_superseded()) {
        on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Stale, UnloadDisplayedDocument::No);
        return;
    }

    auto navigable = local_navigable_with_id(navigable_id);
    if (!navigable) {
        on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Skipped, UnloadDisplayedDocument::No);
        return;
    }
    if (local_target_entry) {
        auto document_state = local_target_entry->document_state();
        if (!document_state
            || document_state->cross_process_id() != target_entry.document_state.id) {
            on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Stale, UnloadDisplayedDocument::No);
            return;
        }

        apply_session_history_entry_descriptor_from_ui_process(*local_target_entry, target_entry);
        apply_session_history_document_state_descriptor_from_ui_process(*document_state, target_entry.document_state);
        navigable->prepare_child_navigable_history_reconstruction(target_entry.document_state);
    } else {
        local_target_entry = navigable->resolve_local_session_history_entry(move(target_entry), LocalNavigable::PrepareChildHistoryReconstruction::Yes);
    }
    if (!local_target_entry) {
        on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Stale, UnloadDisplayedDocument::No);
        return;
    }
    auto did_claim_navigable = run_changing_navigable_history_step_job_impl(
        {
            .operation_id = operation_id,
            .navigable_id = navigable_id,
            .target_entry = local_target_entry.release_nonnull(),
            .user_involvement = user_involvement,
            .navigation_type = navigation_type,
            .superseded_by_newer_navigation = superseded_by_newer_navigation,
            .source_snapshot = operation.serialized_source_snapshot_params,
            .population = move(population),
        },
        source_snapshot_params, pending_document,
        GC::create_function(heap(), [this, operation_id, navigable_id, on_complete](LocalChangingNavigableHistoryStepJobResult result) {
            auto* operation = find_history_operation(operation_id);
            if (operation) {
                if (result.disposition == ChangingNavigableHistoryStepJobDisposition::Ready) {
                    VERIFY(result.continuation);
                    operation->changing_navigable_continuations.set(navigable_id, *result.continuation);
                } else {
                    operation->claimed_navigables_awaiting_continuation.remove(navigable_id);
                }
            }
            // AD-HOC: A job can claim its navigable before becoming stale, or finish after its operation was
            //         abandoned. Release the document-local projection so pending navigations can resume.
            if (result.disposition != ChangingNavigableHistoryStepJobDisposition::Ready || !operation) {
                if (auto navigable = local_navigable_with_id(navigable_id))
                    navigable->clear_ongoing_history_traversal();
            }
            auto unload_displayed_document = result.continuation
                    && !result.continuation->update_only
                    && result.continuation->resolved_document.ptr() != result.continuation->displayed_document.ptr()
                ? UnloadDisplayedDocument::Yes
                : UnloadDisplayedDocument::No;
            on_complete->function()(result.disposition, unload_displayed_document);
        }));
    if (did_claim_navigable)
        operation.claimed_navigables_awaiting_continuation.set(navigable_id);
}

void HistoryExecutor::prepare_ui_changing_navigable_for_unload(CrossProcessId operation_id, CrossProcessId navigable_id, GC::Ref<GC::Function<void()>> on_complete)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation || !operation->changing_navigable_continuations.contains(navigable_id)) {
        on_complete->function()();
        return;
    }

    // Step 5.2 of deactivate a document for a cross-document navigation: set navigable's ongoing navigation to null.
    if (auto navigable = local_navigable_with_id(navigable_id))
        navigable->clear_ongoing_history_traversal();
    on_complete->function()();
}

void HistoryExecutor::apply_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, HistoryObjectLengthAndIndex history_object_length_and_index, Vector<SessionHistoryEntryDescriptor> entry_descriptors_for_navigation_api, VisibilityState system_visibility_state, UnloadDisplayedDocument unload_displayed_document, GC::Ref<GC::Function<void(Optional<ReplicatedNavigableState>, Optional<SessionHistoryEntryPersistedState>)>> on_complete)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation) {
        on_complete->function()({}, {});
        return;
    }
    auto continuation = operation->changing_navigable_continuations.take(navigable_id);
    if (!continuation.has_value()) {
        on_complete->function()({}, {});
        return;
    }
    operation->claimed_navigables_awaiting_continuation.remove(navigable_id);

    Vector<NonnullRefPtr<SessionHistoryEntry>> entries_for_navigation_api;
    if (auto navigable = local_navigable_with_id(navigable_id); navigable && !navigable->has_been_destroyed())
        entries_for_navigation_api = navigable->session_history_entries_for_navigation_api_from_ui_process(move(entry_descriptors_for_navigation_api));

    apply_changing_navigable_history_step_continuation_impl(
        *continuation,
        {
            .history_object_length_and_index = history_object_length_and_index,
            .entries_for_navigation_api = move(entries_for_navigation_api),
            .system_visibility_state = system_visibility_state,
        },
        unload_displayed_document,
        on_complete);
}

}
