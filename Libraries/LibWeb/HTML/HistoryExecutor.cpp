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
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HistoryExecutor.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationParamsDescriptor.h>
#include <LibWeb/HTML/RemoteNavigable.h>
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
    RefPtr<SessionHistoryEntry> target_entry;
    GC::Ptr<LocalNavigable> navigable;
    bool update_only = false;
    Optional<Bindings::NavigationType> navigation_type;

    GC::Ptr<DOM::Document> pending_document;
    GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> population_output;
    GC::Ptr<DOM::Document> target_document;
    Optional<URL::Origin> old_origin;
    UserNavigationInvolvement user_involvement { UserNavigationInvolvement::None };

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(displayed_document);
        visitor.visit(navigable);
        visitor.visit(pending_document);
        visitor.visit(population_output);
        visitor.visit(target_document);
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
        visitor.visit(operation.value.pending_document);
        visitor.visit(operation.value.on_complete);
        visitor.visit(operation.value.on_apply_history_step_complete);
        visitor.visit(operation.value.on_navigation_api_traversal_rejected);
        for (auto& job : operation.value.changing_navigable_jobs) {
            visitor.visit(job.value.continuation);
            visitor.visit(job.value.continue_population);
        }
    }
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

CrossProcessId HistoryExecutor::request_history_operation(HistoryOperationParameters parameters)
{
    return request_history_operation(move(parameters), {});
}

CrossProcessId HistoryExecutor::request_history_operation(HistoryOperationParameters parameters, HistoryOperationState state)
{
    auto operation_id = m_page->client().allocate_cross_process_id();
    m_history_operations.set(operation_id, move(state));
    m_page->client().page_did_request_history_operation(operation_id, move(parameters));
    return operation_id;
}

void HistoryExecutor::revoke_history_operation(CrossProcessId operation_id)
{
    if (m_history_operations.contains(operation_id))
        m_page->client().page_did_revoke_history_operation(operation_id);
}

void HistoryExecutor::complete_ui_history_operation(CrossProcessId operation_id, Optional<i32> target_entry_step)
{
    auto operation = m_history_operations.find(operation_id);
    if (operation == m_history_operations.end())
        return;
    auto changing_navigable_ids = operation->value.changing_navigable_jobs.keys();
    auto local_target_navigable_id = operation->value.local_target_navigable_id;
    auto local_target_entry = move(operation->value.local_target_entry);
    auto on_complete = operation->value.on_complete;
    m_history_operations.remove(operation);

    // AD-HOC: A canceled or stale operation can leave a claimed navigable whose continuation was never applied.
    for (auto navigable_id : changing_navigable_ids) {
        if (auto navigable = local_navigable_with_id(navigable_id))
            navigable->clear_ongoing_history_traversal();
    }

    if (target_entry_step.has_value()
        && local_target_navigable_id.has_value()
        && local_target_entry
        && !local_target_entry->step_value().has_value()) {
        if (auto navigable = local_navigable_with_id(*local_target_navigable_id);
            navigable && !navigable->is_top_level_traversable()) {
            local_target_entry->set_step(*target_entry_step);
        }
    }
    if (on_complete)
        on_complete->function()();
}

void HistoryExecutor::did_apply_ui_history_step(CrossProcessId operation_id, HistoryStepResult result)
{
    auto* operation = find_history_operation(operation_id);
    if (operation && operation->on_apply_history_step_complete)
        operation->on_apply_history_step_complete->function()(result);
}

void HistoryExecutor::reject_ui_navigation_api_traversal(CrossProcessId operation_id)
{
    auto* operation = find_history_operation(operation_id);
    if (operation && operation->on_navigation_api_traversal_rejected)
        operation->on_navigation_api_traversal_rejected->function()();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void HistoryExecutor::traverse_the_history_by_delta(int delta, DOM::Document& source_document)
{
    // 1. Let sourceSnapshotParams and initiatorToCheck be null.
    // 2. Let userInvolvement be "browser UI".
    // 3. If sourceDocument is given, then:
    // NB: sourceDocument is always given here. The UI process traverses the history for the browser UI.

    // 1. Set sourceSnapshotParams to the result of snapshotting source snapshot params given sourceDocument.
    auto source_snapshot_params = snapshot_source_snapshot_params(source_document);

    // 2. Set initiatorToCheck to sourceDocument's node navigable.
    auto initiator_to_check = source_document.navigable();

    // 3. Set userInvolvement to "none".
    auto user_involvement = UserNavigationInvolvement::None;

    // 4. Append the following session history traversal steps to traversable:
    request_history_operation(
        TraverseByDeltaHistoryOperationParameters {
            .delta = delta,
            .initiator_to_check = initiator_to_check ? Optional<CrossProcessId> { initiator_to_check->id() } : OptionalNone {},
            .source_snapshot_params = create_navigation_source_snapshot(*source_snapshot_params),
            .user_involvement = user_involvement,
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

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
// NB: The UI process has each page hosting documents to check run this for them. The traversable's host is also given
//     the target entry, and runs step 4 for the traversable.
void HistoryExecutor::run_ui_beforeunload_check(Vector<CrossProcessId> navigable_ids, Optional<SessionHistoryEntryDescriptor> target_entry_descriptor, Optional<UserNavigationInvolvement> user_involvement_for_navigate_event, UnloadPromptShown unload_prompt_shown, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult, UnloadPromptShown)>> on_complete)
{
    GC::Ptr<LocalTraversableNavigable> traversable;
    RefPtr<SessionHistoryEntry> target_entry;
    if (target_entry_descriptor.has_value()) {
        traversable = as<LocalTraversableNavigable>(*m_page->local_traversable());
        target_entry = traversable->resolve_local_session_history_entry(target_entry_descriptor.release_value());
    }

    Vector<GC::Root<LocalNavigable>> navigables;
    navigables.ensure_capacity(navigable_ids.size());
    for (auto navigable_id : navigable_ids) {
        if (auto navigable = local_navigable_with_id(navigable_id); navigable && !navigable->has_been_destroyed() && navigable->active_document())
            navigables.append(*navigable);
    }
    check_if_unloading_is_canceled(move(navigables), traversable, traversable, move(target_entry), user_involvement_for_navigate_event, unload_prompt_shown, on_complete);
}

static bool is_same_document_push_or_replace(Optional<Bindings::NavigationType> navigation_type, SessionHistoryEntry const& target_entry, DOM::Document const& displayed_document)
{
    if (navigation_type != Bindings::NavigationType::Push
        && navigation_type != Bindings::NavigationType::Replace) {
        return false;
    }

    return target_entry.document_state()->document_id() == displayed_document.unique_id();
}

static void queue_apply_history_step_task(GC::Ref<LocalNavigable> navigable, GC::Ref<GC::Function<void()>> steps)
{
    // AD-HOC: Queue the task with navigable's active Document instead of using queue_global_task(active_window). During
    //         initial about:blank Window reuse, active_window()->associated_document() can already be the pending
    //         Document, but the apply-history task must run against the current active Document.
    // NB: The task does not run while the Document is not fully active, as once its navigable is destroyed, which leaves
    //     its job without a reply. The UI process ends the jobs of a navigable it removes, and of a document another
    //     operation replaces.
    queue_a_task(Task::Source::NavigationAndTraversal, nullptr, navigable->active_document(), steps);
}

bool HistoryExecutor::resume_history_navigation_population(CrossProcessId operation_id, HistoryNavigationPopulation&& population)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation)
        return false;
    auto job = operation->changing_navigable_jobs.get(population.request.navigable_id);
    if (!job.has_value() || !job->continue_population)
        return false;
    GC::Ref continue_population = *exchange(job->continue_population, nullptr);
    continue_population->function()(move(population));
    return true;
}

void HistoryExecutor::enqueue_changing_navigable_continuation(CrossProcessId operation_id, GC::Ref<ChangingNavigableContinuationState> continuation, GC::Ref<OnChangingNavigableHistoryStepJobComplete> on_complete)
{
    if (auto* operation = find_history_operation(operation_id))
        operation->changing_navigable_jobs.get(continuation->navigable->id())->continuation = continuation;

    // NB: The UI process applies the continuation as step 14.10 or step 14.11 of apply the history step says, given
    //     its update-only and targetEntry's document.
    auto target_document_id = continuation->target_document ? continuation->target_document->id() : Optional<CrossProcessId> {};
    on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Ready, continuation->update_only, target_document_id);
}

void HistoryExecutor::run_ui_changing_navigable_history_job(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryDescriptor target_entry_descriptor, UserNavigationInvolvement user_involvement, Optional<Bindings::NavigationType> navigation_type, Optional<Utf16String> finalized_navigation_id, bool ongoing_navigation_is_traversal, Optional<NavigationSourceSnapshot> navigation_source_snapshot, GC::Ref<OnChangingNavigableHistoryStepJobComplete> on_complete, Optional<HistoryNavigationPopulation> population)
{
    auto& operation = ensure_history_operation(operation_id);
    // The job for the navigation this process finalizes claims the entry it created, with the document it populated.
    // NB: The update for a navigable's creation targets an entry the navigable retains, not the one the request gave.
    RefPtr<SessionHistoryEntry> local_target_entry;
    GC::Ptr<DOM::Document> pending_document;
    if (operation.local_target_navigable_id == navigable_id && navigation_type.has_value()) {
        VERIFY(operation.local_target_entry);
        local_target_entry = operation.local_target_entry;
        pending_document = operation.pending_document;
    }

    // AD-HOC: This process can destroy a navigable, or unload its document, before the UI process removes it, which
    //         leaves no active window to queue the task on.
    auto navigable = local_navigable_with_id(navigable_id);
    if (!navigable || navigable->has_been_destroyed() || !navigable->active_window()) {
        on_complete->function()(ChangingNavigableHistoryStepJobDisposition::Skipped, false, {});
        return;
    }

    if (local_target_entry) {
        apply_session_history_entry_descriptor_from_ui_process(*local_target_entry, target_entry_descriptor);
        apply_session_history_document_state_descriptor_from_ui_process(*local_target_entry->document_state(), target_entry_descriptor.document_state);
    } else {
        local_target_entry = navigable->resolve_local_session_history_entry(move(target_entry_descriptor));
    }

    GC::Ptr<SourceSnapshotParams> source_snapshot_params;
    if (navigation_source_snapshot.has_value())
        source_snapshot_params = create_source_snapshot_params_from_navigation_source_snapshot(relevant_realm(*navigable->active_window()), *navigation_source_snapshot);

    // 8.1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
    // NB: The UI coordinator already ran that step against canonical history and sent the selected entry with this
    //     job. Retain that exact local entry instead of repeating the global-step lookup in WebContent.
    NonnullRefPtr claimed_target_entry = local_target_entry.release_nonnull();

    // 8.2. Set navigable's current session history entry to targetEntry.
    navigable->set_current_session_history_entry(claimed_target_entry);

    // NB: The UI process ran step 8.4 of apply the history step, "Set navigable's ongoing navigation to 'traversal'".
    //     Mirror its result here.
    // AD-HOC: A push or replace leaves a newer navigation's ID in place, as that navigation goes on and commits after
    //         this one, and nulls the ID of the navigation it finalizes. See https://github.com/whatwg/html/issues/12581.
    if (ongoing_navigation_is_traversal) {
        navigable->set_ongoing_navigation_without_informing_navigation_api(HTML::LocalNavigable::Traversal::Tag);

        // AD-HOC: Only step 11 of navigate stops an iframe's lazy loading, but the update for a navigable's creation
        //         also traverses a child that a repopulated document created, to the entry its restored nested history
        //         holds. That entry's document takes the place of the one the iframe's src would load, so the iframe
        //         stops lazy loading here too — or its lazy load resumption steps would navigate the restored
        //         document away once the iframe is scrolled into view.
        if (!navigation_type.has_value()) {
            if (auto container = navigable->container(); container && container->is_html_iframe_element())
                static_cast<HTMLIFrameElement&>(*container).stop_lazy_loading();
        }
    } else if (finalized_navigation_id.has_value() && navigable->ongoing_navigation() == *finalized_navigation_id) {
        navigable->set_ongoing_navigation_without_informing_navigation_api(Empty {});
    }

    operation.changing_navigable_jobs.ensure(navigable_id);

    queue_apply_history_step_task(*navigable, GC::create_function(heap(), [this, operation_id, navigation_type, user_involvement, population = move(population), source_snapshot_params, pending_document, claimed_target_entry = move(claimed_target_entry), navigable, on_complete]() mutable {
        // 1. Let displayedEntry be navigable's active session history entry.
        auto displayed_entry = navigable->active_session_history_entry();

        // 2. Let targetEntry be navigable's current session history entry.
        // AD-HOC: The entry this job claimed, rather. A synchronous navigation that jumps the queue during this run's
        //         apply-history-step re-claims the navigable before this task runs, and the run still applies its
        //         claimed target entry once the jumped navigation's nested run completes.
        RefPtr<SessionHistoryEntry> target_entry = claimed_target_entry;

        // 3. Let changingNavigableContinuation be a changing navigable continuation state with:
        auto changing_navigable_continuation = heap().allocate<ChangingNavigableContinuationState>();
        changing_navigable_continuation->displayed_document = navigable->active_document();
        changing_navigable_continuation->target_entry = target_entry;
        changing_navigable_continuation->navigable = navigable;
        changing_navigable_continuation->update_only = false;
        changing_navigable_continuation->navigation_type = navigation_type;
        changing_navigable_continuation->user_involvement = user_involvement;
        changing_navigable_continuation->pending_document = pending_document;
        changing_navigable_continuation->population_output = nullptr;

        // NB: targetEntry's document, as this process has it: the document the finalized navigation populated, or the
        //     active document if targetEntry's document state has it. Any other is null until population.
        changing_navigable_continuation->target_document = pending_document;
        if (!pending_document && target_entry->document_state()->document_id() == navigable->active_document_id())
            changing_navigable_continuation->target_document = navigable->active_document();

        // 4. If displayedEntry is targetEntry and targetEntry's document state's reload pending is false, then:
        // AD-HOC: A same-document push or replace is update-only too. Its document displays its entry already, or a
        //         newer one a synchronous navigation that jumped the queue pushed or replaced, which the continuation
        //         then leaves displayed.
        if ((displayed_entry == target_entry && !target_entry->document_state()->reload_pending())
            || is_same_document_push_or_replace(navigation_type, *target_entry, *changing_navigable_continuation->displayed_document)) {
            // 1. Set changingNavigableContinuation's update-only to true.
            changing_navigable_continuation->update_only = true;

            // 2. Enqueue changingNavigableContinuation on changingNavigableContinuations.
            enqueue_changing_navigable_continuation(operation_id, *changing_navigable_continuation, on_complete);

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
                break;
            case Bindings::NavigationType::Push:
                // FIXME: Add ever populated check, and fix the bug where top level traversable's step is not updated when a child navigable navigates
                // - "push": Assert: targetEntry's step is displayedEntry's step + 1 and targetEntry's document state's ever populated is false.
                // NB: A pushState() that jumps the traversal queue while this job is paused takes targetEntry's step:
                //     It clears targetEntry away as forward history, and appends its own entry at that step. So, the
                //     two steps can be equal here — and that's no reason to drop the push. The UI process appends
                //     targetEntry again, after the pushed entry, when it processes this job's continuation
                //     (ApplyHistoryStep::process_changing_navigable_continuations). Gecko/WebKit/Blink keep such a load
                //     as well, and add its entry after the pushed one when it commits: A pushState() leaves the pending
                //     load alone in Gecko nsDocShell::UpdateURLAndHistory, WebKit FrameLoader::updateURLAndHistory, and
                //     Blink RenderFrameHostImpl::DidCommitSameDocumentNavigation.
                VERIFY(target_entry != displayed_entry);
                break;
            }
        }

        // 6. Let oldOrigin be targetEntry's document state's origin.
        auto old_origin = target_entry->document_state()->origin();

        // 7. If all of the following are true:
        //   * navigationType is "traverse";
        //   * navigable is not traversable;
        //   * targetEntry is not displayedEntry; and
        //   * oldOrigin is the same as displayedEntry's document state's origin,
        // then:
        if (navigation_type == Bindings::NavigationType::Traverse
            && !navigable->is_traversable()
            && target_entry != displayed_entry
            && old_origin == displayed_entry->document_state()->origin()) {
            // 1. Let navigation be navigable's active window's navigation API.
            auto navigation = navigable->active_window()->navigation();

            // 2. Fire a traverse navigate event at navigation given targetEntry and userInvolvement.
            navigation->fire_a_traverse_navigate_event(*target_entry, user_involvement);
        }

        auto after_document_populated = GC::create_function(heap(), [this, operation_id, old_origin, changing_navigable_continuation, on_complete](GC::Ptr<PopulateSessionHistoryEntryDocumentOutput> output) mutable {
            changing_navigable_continuation->population_output = output;
            changing_navigable_continuation->old_origin = old_origin;

            // NB: Population that produces no document leaves targetEntry's document as it was.
            if (output && output->document)
                changing_navigable_continuation->target_document = output->document;

            // 1. If targetEntry's document is null, then set changingNavigableContinuation's update-only to true.
            if (!changing_navigable_continuation->target_document)
                changing_navigable_continuation->update_only = true;

            // 2. If targetEntry's document's origin is not oldOrigin, then set targetEntry's classic history API state to StructuredSerializeForStorage(null).
            // 3. If all of the following are true:
            //     - navigable's parent is null;
            //     - targetEntry's document's browsing context is not an auxiliary browsing context whose opener browsing context is non-null; and
            //     - targetEntry's document's origin is not oldOrigin,
            //    then set targetEntry's document state's navigable target name to the empty string.
            // NOTE: Steps 2-3 are deferred to after_potential_unload to avoid exposing mutations during unload.

            // 4. Enqueue changingNavigableContinuation on changingNavigableContinuations.
            enqueue_changing_navigable_continuation(operation_id, *changing_navigable_continuation, on_complete);
        });

        // 8. If targetEntry's document is null, or targetEntry's document state's reload pending is true, then:
        bool needs_population = !changing_navigable_continuation->target_document || target_entry->document_state()->reload_pending();
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
                output->redirect_replaced_fragment = result.redirect_replaced_fragment;
                output->classic_history_api_state = move(result.classic_history_api_state);
                output->resource_cleared = result.resource_cleared;
                output->inline_content_origin = move(result.inline_content_origin);
                output->document_id = result.document_id;
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
                        queue_apply_history_step_task(*navigable, GC::create_function(navigable->heap(), [after_document_populated, output] {
                            after_document_populated->function()(output);
                        }));
                    }));
            });
            if (population.has_value()) {
                continue_population->function()(population.release_value());
                return;
            }
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            operation->changing_navigable_jobs.ensure(navigable->id()).continue_population = continue_population;

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
            auto input_reload_bypasses_cache = target_entry->document_state()->reload_bypasses_cache();
            target_entry->document_state()->set_reload_pending(false);
            target_entry->document_state()->set_reload_bypasses_cache(false);
            m_page->client().page_did_set_session_history_entry_document_state_reload_pending(
                navigable->id(), target_entry->navigation_api_key(), false);

            // 6. Let allowPOST be targetEntry's document state's reload pending.
            // AD-HOC: A document another page displayed, or a crashed process lost, is loaded again with its POST
            //         resource, as the process that displayed it would have kept it.
            auto allow_POST = input_reload_pending || navigable->is_provisional();

            // NB: Population runs in a deferred task, during which sync navigations can change targetEntry. The request
            //     holds the entry as it is now. See https://github.com/whatwg/html/issues/9869.
            auto request = NavigationPopulationRequest {
                .navigable_id = navigable->id(),
                .history_entry = create_pending_session_history_entry_descriptor(create_session_history_entry_descriptor(*target_entry)),
                .source_snapshot_params = create_navigation_source_snapshot(*potentially_target_specific_source_snapshot_params),
                .target_snapshot_params = target_snapshot_params,
                .csp_navigation_type = ContentSecurityPolicy::Directives::Directive::NavigationType::Other,
                .history_handling = Bindings::NavigationHistoryBehavior::Replace,
                .user_involvement = user_involvement,
                .navigation_id = {},
            };
            request.history_entry.document_state.reload_pending = input_reload_pending;
            request.history_entry.document_state.reload_bypasses_cache = input_reload_bypasses_cache;

            // 7. In parallel, attempt to populate the history entry's document for targetEntry, given navigable, potentiallyTargetSpecificSourceSnapshotParams,
            //    targetSnapshotParams, userInvolvement, with allowPOST set to allowPOST and completionSteps set to
            //    queue a global task on the navigation and traversal task source given navigable's active window to
            //    run afterDocumentPopulated.
            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this, operation_id, request = move(request), potentially_target_specific_source_snapshot_params, allow_POST, navigable] {
                navigable->populate_session_history_entry_document(request, *potentially_target_specific_source_snapshot_params, allow_POST, GC::create_function(heap(), [this, operation_id, request](NavigationPopulationResult result) mutable {
                    m_page->client().history_navigation_params_creation_finished(operation_id, { move(request), move(result) });
                }));
            }));
        }
        // Otherwise, run afterDocumentPopulated immediately.
        else {
            after_document_populated->function()(nullptr);
        }
    }));
}

// Step 5.2 of deactivate a document for a cross-document navigation: set navigable's ongoing navigation to null.
// NB: This mirrors it for the navigable whose job this operation's continuation is, and leaves a newer navigation's.
void HistoryExecutor::clear_ui_changing_navigable_ongoing_traversal(CrossProcessId operation_id, CrossProcessId navigable_id)
{
    auto* operation = find_history_operation(operation_id);
    auto job = operation ? operation->changing_navigable_jobs.get(navigable_id) : OptionalNone {};
    if (!job.has_value() || !job->continuation)
        return;
    job->continuation->navigable->clear_ongoing_history_traversal();
}

// Steps 14.4 to 14.9 and step 14.12 of apply the history step, for the changing navigable continuation the UI process
// dequeued.
Optional<HistoryExecutor::DequeuedChangingNavigableContinuation> HistoryExecutor::dequeue_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryIdentity target_entry_identity, HistoryObjectLengthAndIndex history_object_length_and_index, Vector<SessionHistoryEntryDescriptor> entry_descriptors_for_navigation_api, GC::Ref<OnChangingNavigableContinuationApplied> on_complete)
{
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    auto job = operation->changing_navigable_jobs.take(navigable_id);
    VERIFY(job.has_value() && job->continuation);
    GC::Ref continuation = *job->continuation;

    // 4. Let displayedDocument be changingNavigableContinuation's displayed document.
    auto displayed_document = continuation->displayed_document;

    // 5. Let targetEntry be changingNavigableContinuation's target entry.
    auto& target_entry = continuation->target_entry;

    // 6. Let navigable be changingNavigableContinuation's navigable.
    auto navigable = continuation->navigable;

    // AD-HOC: This process can destroy the navigable, an ancestor of it, or the displayed document, before the UI
    //         process removes it.
    if (navigable->is_in_a_destroyed_subtree() || !displayed_document->navigable())
        return {};

    // 7. Let (scriptHistoryLength, scriptHistoryIndex) be the result of getting the history object length and index given traversable and targetStep.
    auto script_history_length = history_object_length_and_index.script_history_length;
    auto script_history_index = history_object_length_and_index.script_history_index;

    // 9. Let entriesForNavigationAPI be the result of getting session history entries for the navigation API given navigable and targetStep.
    // NB: The UI process got these, and sent their descriptors.
    auto entries_for_navigation_api = navigable->session_history_entries_for_navigation_api_from_ui_process(move(entry_descriptors_for_navigation_api), *target_entry);

    // AD-HOC: The UI process looked targetEntry up again, and follows the entry a sync navigation that jumped the queue
    //         put in its slot. That entry is in entriesForNavigationAPI. See https://github.com/whatwg/html/issues/12961.
    if (session_history_entry_identity(*target_entry) != target_entry_identity) {
        auto updated_target_entry = entries_for_navigation_api.find_if([&](auto const& entry) {
            return session_history_entry_identity(*entry) == target_entry_identity;
        });
        VERIFY(updated_target_entry != entries_for_navigation_api.end());
        target_entry = *updated_target_entry;
    }

    auto population_output = continuation->population_output;
    auto old_origin = continuation->old_origin;

    // 12. In both cases, let afterPotentialUnloads be the following steps:
    bool const update_only = continuation->update_only;
    auto after_potential_unload = GC::create_function(heap(), [this, navigable, update_only, target_entry, continuation, population_output, old_origin, displayed_document = GC::Ref { *displayed_document }, script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), navigation_type = continuation->navigation_type, on_complete] {
        // AD-HOC: A provisional navigable stands in for a navigable another process hosts, which is destroyed as soon
        //         as its container is removed, e.g. while displayedDocument unloads. The provisional navigable is only
        //         discarded once that navigable's subtree has unloaded, and has no container to take over until then.
        if (auto provisional_for = navigable->provisional_for(); provisional_for && provisional_for->is_in_a_destroyed_subtree()) {
            on_complete->function()({}, {});
            return;
        }

        // NB: Only step 14.10 applies a same-document push or replace: its targetEntry's document is displayedDocument.
        if (is_same_document_push_or_replace(navigation_type, *target_entry, displayed_document)
            && navigable->active_session_history_entry() != target_entry) {
            on_complete->function()({}, {});
            return;
        }

        if (population_output)
            population_output->apply_to(*target_entry);

        // Post-population adjustments — only run when a fresh document was produced
        // (not for 204/205 no-document outcomes where targetEntry's document is null).
        bool has_fresh_document = continuation->pending_document || (population_output && population_output->document);
        if (has_fresh_document) {
            auto target_document = continuation->target_document;
            // 2. If targetEntry's document's origin is not oldOrigin, then set targetEntry's classic history API state to StructuredSerializeForStorage(null).
            if (target_document->origin() != old_origin) {
                auto& vm = navigable->vm();
                target_entry->set_classic_history_api_state(MUST(structured_serialize_for_storage(vm, JS::js_null())));
            }

            // 3. If all of the following are true:
            //     - navigable's parent is null;
            //     - targetEntry's document's browsing context is not an auxiliary browsing context whose opener browsing context is non-null; and
            //     - targetEntry's document's origin is not oldOrigin,
            //    then set targetEntry's document state's navigable target name to the empty string.
            if (navigable->parent() == nullptr
                && !(target_document->browsing_context()->is_auxiliary() && target_document->browsing_context()->opener_browsing_context_window_proxy())
                && target_entry->document_state()->origin() != old_origin) {
                target_entry->document_state()->set_navigable_target_name(Utf16String {});
            }
        }

        // 1. Let previousEntry be navigable's active session history entry.
        auto previous_entry = navigable->active_session_history_entry();

        // NB: previousEntry's document is unloaded before updateDocument runs, so the fact navigation.activation needs
        //     from it is captured from the document that was displayed when this step began. A provisional navigable's
        //     entry is its stand-in's, which, like an initial about:blank's, gives navigation.activation no old entry.
        auto previous_entry_document_is_initial_about_blank = displayed_document->is_initial_about_blank() || navigable->is_provisional();

        // AD-HOC: targetEntry's document is null when population produced no document, which made the continuation
        //         update-only. The spec then updates that null document. Update displayedDocument instead.
        GC::Ref<DOM::Document> resolved_document = continuation->target_document ? GC::Ref { *continuation->target_document } : displayed_document;

        // 2. If changingNavigableContinuation's update-only is false, then activate history entry targetEntry for navigable.
        Optional<HostedNavigableState> activated_navigable_state;
        if (!update_only) {
            navigable->activate_history_entry(*target_entry, *resolved_document);

            // https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation
            // 2. Set navigable's is delaying load events to false.
            // AD-HOC: Not until the navigation's document is active. Before, the container document sees the displayed
            //         document instead, and an initial about:blank is ready for post-load tasks, so nothing would delay
            //         the container's load event.
            if (continuation->pending_document)
                navigable->set_delaying_load_events(false);

            activated_navigable_state = navigable->hosted_state();
        }
        auto previous_entry_persisted_state = !update_only && previous_entry
            ? create_session_history_entry_persisted_state(*previous_entry)
            : Optional<SessionHistoryEntryPersistedState> {};

        // 3. Let updateDocument be an algorithm step which performs update document for history step application given
        //    targetEntry's document, targetEntry, changingNavigableContinuation's update-only, scriptHistoryLength,
        //    scriptHistoryIndex, navigationType, entriesForNavigationAPI, and previousEntry.
        auto update_document = [script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), target_entry, update_only, navigation_type, previous_entry, previous_entry_document_is_initial_about_blank, resolved_document, navigable, continuation] {
            // NB: The specification initializes the navigation API entries for every newly activated document.
            //     Gating this on a non-null navigationType left documents activated by a creation/destruction
            //     update without an initialized navigation API entry list, which crashes the first same-document
            //     update on them (for example a document.open() on a child that finished loading while the
            //     creation update was still queued).
            resolved_document->update_for_history_step_application(*target_entry, update_only, script_history_length, script_history_index, navigation_type, entries_for_navigation_api, previous_entry, previous_entry_document_is_initial_about_blank, false, target_entry->document_state()->initiator_origin(), continuation->user_involvement);

            if (update_only)
                navigable->notify_navigation_observers_navigation_complete();
        };

        // 4. If targetEntry's document is equal to displayedDocument, then perform updateDocument.
        if (target_entry->document_state()->document_id() == displayed_document->unique_id()) {
            update_document();
        }
        // 5. Otherwise, queue a global task on the navigation and traversal task source given targetEntry's document's relevant global object to perform updateDocument
        else {
            queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*resolved_document), GC::create_function(heap(), move(update_document)));
        }

        // 6. Increment completedChangeJobs.
        on_complete->function()(move(activated_navigable_state), move(previous_entry_persisted_state));
    });

    return DequeuedChangingNavigableContinuation { continuation, after_potential_unload };
}

// 10. If changingNavigableContinuation's update-only is true, or targetEntry's document is displayedDocument:
void HistoryExecutor::apply_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryIdentity target_entry_identity, HistoryObjectLengthAndIndex history_object_length_and_index, Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api, GC::Ref<OnChangingNavigableContinuationApplied> on_complete)
{
    auto dequeued = dequeue_ui_changing_navigable_continuation(operation_id, navigable_id, target_entry_identity, history_object_length_and_index, move(entries_for_navigation_api), on_complete);
    if (!dequeued.has_value()) {
        on_complete->function()({}, {});
        return;
    }
    auto navigable = dequeued->continuation->navigable;

    // 1. Set navigable's ongoing navigation to null.
    // NB: This mirrors the UI process's transition in the local projection.
    navigable->clear_ongoing_history_traversal();

    // 2. Queue a global task on the navigation and traversal task source given navigable's active window to perform
    //    afterPotentialUnloads.
    queue_apply_history_step_task(*navigable, dequeued->after_potential_unloads);
}

// 11. Otherwise:
//     2. Deactivate displayedDocument, given userInvolvement, targetEntry, navigationType, and afterPotentialUnloads.
// NB: The UI process has unloaded every descendant subtree of displayedDocument. This is the task that unload a document
//     and its descendants queues last, for displayedDocument.
void HistoryExecutor::unload_displayed_document_and_apply_ui_changing_navigable_continuation(CrossProcessId operation_id, CrossProcessId navigable_id, SessionHistoryEntryIdentity target_entry_identity, HistoryObjectLengthAndIndex history_object_length_and_index, Vector<SessionHistoryEntryDescriptor> entries_for_navigation_api, GC::Ref<OnChangingNavigableContinuationApplied> on_complete)
{
    auto dequeued = dequeue_ui_changing_navigable_continuation(operation_id, navigable_id, target_entry_identity, history_object_length_and_index, move(entries_for_navigation_api), on_complete);
    if (!dequeued.has_value()) {
        on_complete->function()({}, {});
        return;
    }

    // 6. Queue a global task on the navigation and traversal task source given document's relevant global object to
    //    perform the following steps:
    // NB: Use a null document so this remains runnable after displayedDocument becomes non-fully-active.
    queue_a_task(Task::Source::NavigationAndTraversal, nullptr, nullptr,
        GC::create_function(heap(), [displayed_document = GC::Ref { *dequeued->continuation->displayed_document }, new_document = dequeued->continuation->target_document, after_potential_unloads = dequeued->after_potential_unloads] {
            // FIXME: 1. If firePageSwapSteps is given, then run firePageSwapSteps.

            // 2. Unload document, passing along newDocument if it is not null.
            displayed_document->unload(new_document);

            // 3. If afterAllUnloads was given, then run it.
            after_potential_unloads->function()();
        }));
}

}
