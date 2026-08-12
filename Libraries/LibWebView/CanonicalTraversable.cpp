/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

CanonicalTraversable::CanonicalTraversable()
    : CanonicalNavigable({}, {}, nullptr, 0)
{
}

CanonicalNavigable& CanonicalTraversable::insert(WebContentClient& reporting_client, u64 page_id, Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, CanonicalNavigable& fallback_parent)
{
    if (auto existing_navigable = find(frame_id); existing_navigable.has_value())
        remove(*existing_navigable);

    auto navigable = make<CanonicalNavigable>(frame_id, parent_frame_id, &reporting_client, page_id);

    // A frame's parent frame is always created (and thus reported) before the frame
    // itself, so if the parent is not in the index, the parent is the top-level document
    // of the reporting page: the fallback parent.
    auto* parent = &fallback_parent;
    if (auto indexed_parent = find(parent_frame_id); indexed_parent.has_value())
        parent = &*indexed_parent;

    auto& navigable_ref = parent->append_child(move(navigable));
    m_navigable_index.set(navigable_ref.id(), navigable_ref.make_weak_ptr());
    return navigable_ref;
}

Optional<CanonicalNavigable&> CanonicalTraversable::find(Web::HTML::CrossProcessId navigable_id)
{
    if (id() == navigable_id)
        return *this;

    auto navigable = m_navigable_index.get(navigable_id);
    if (!navigable.has_value() || !navigable.value())
        return {};

    return *navigable.value();
}

Optional<CanonicalNavigable const&> CanonicalTraversable::find(Web::HTML::CrossProcessId navigable_id) const
{
    if (id() == navigable_id)
        return *this;

    auto navigable = m_navigable_index.get(navigable_id);
    if (!navigable.has_value() || !navigable.value())
        return {};

    return *navigable.value();
}

void CanonicalTraversable::remove(CanonicalNavigable& navigable)
{
    VERIFY(&navigable != this);
    remove_from_index(navigable);

    auto* parent = navigable.parent();
    VERIFY(parent);
    (void)parent->remove_child(navigable);
}

void CanonicalTraversable::remove_from_index(CanonicalNavigable& navigable)
{
    navigable.for_each_in_inclusive_subtree([&](CanonicalNavigable& child) {
        m_navigable_index.remove(child.id());
        return IterationDecision::Continue;
    });
}

void CanonicalTraversable::prepare_for_reload()
{
    m_session_history.mark_current_entry_reload_pending();
}

void CanonicalTraversable::did_create_top_level_traversable(Web::HTML::SessionHistoryEntryDescriptor initial_history_entry)
{
    if (!m_session_history.entries().is_empty())
        return;
    m_session_history.initialize_with_initial_history_entry(move(initial_history_entry));
}

Optional<Web::HTML::CrossProcessId> CanonicalTraversable::nested_history_id_for(CanonicalNavigable const& navigable) const
{
    if (&navigable == this)
        return {};
    return navigable.id();
}

bool CanonicalTraversable::update_session_history_entry_navigation_api_state(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Web::HTML::StorageSerializationRecord navigation_api_state)
{
    VERIFY(&navigable.top_level_traversable() == this);

    return m_session_history.update_entry(nested_history_id_for(navigable), navigation_api_key, [&](auto& entry) {
        entry.navigation_api_state = navigation_api_state;
    });
}

bool CanonicalTraversable::update_session_history_entry_scroll_restoration_mode(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    VERIFY(&navigable.top_level_traversable() == this);

    return m_session_history.update_entry(nested_history_id_for(navigable), navigation_api_key, [&](auto& entry) {
        entry.scroll_restoration_mode = scroll_restoration_mode;
    });
}

bool CanonicalTraversable::update_session_history_entry_document_state_navigable_target_name(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Utf16String navigable_target_name)
{
    VERIFY(&navigable.top_level_traversable() == this);

    return m_session_history.update_document_state(nested_history_id_for(navigable), navigation_api_key, [&](auto& document_state) {
        document_state.navigable_target_name = navigable_target_name;
    });
}

bool CanonicalTraversable::set_session_history_entry_document_state_reload_pending(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, bool reload_pending)
{
    VERIFY(&navigable.top_level_traversable() == this);

    return m_session_history.update_document_state(nested_history_id_for(navigable), navigation_api_key, [&](auto& document_state) {
        document_state.reload_pending = reload_pending;
    });
}

Optional<i32> CanonicalTraversable::append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id, Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    auto child_navigable = find(child_navigable_id);
    if (!child_navigable.has_value() || child_navigable->parent() != &parent_navigable)
        return {};
    return m_session_history.append_nested_history(parent_navigable, parent_document_state_id, child_navigable_id, move(initial_history_entry));
}

bool CanonicalTraversable::remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    return m_session_history.remove_nested_history(parent_navigable, parent_document_state_id, child_navigable_id);
}

Optional<i32> CanonicalTraversable::navigation_api_traversal_target(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key) const
{
    VERIFY(&navigable.top_level_traversable() == this);

    // 1. Let navigableSHEs be the result of getting session history entries given navigable.
    auto navigable_session_history_entries = m_session_history.get_session_history_entries(navigable);
    if (!navigable_session_history_entries.has_value())
        return {};

    // 2. Let targetSHE be the session history entry in navigableSHEs whose navigation API key is key. If no such entry exists, then:
    auto target_entry = navigable_session_history_entries->find_if([&](auto const& entry) {
        return entry.navigation_api_key == navigation_api_key;
    });
    if (target_entry == navigable_session_history_entries->end())
        return {};

    return target_entry->step;
}

void CanonicalTraversable::did_cancel_navigation()
{
    m_session_history.clear_current_entry_reload_pending();
}

void CanonicalTraversable::did_finish_navigation(URL::URL const& url)
{
    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url)
        m_session_history.clear_current_entry_reload_pending();
}

bool CanonicalTraversable::web_content_can_apply_traversal(TraversableSessionHistory::TraversalTarget const& target) const
{
    auto const* current_entry = m_session_history.current_entry();
    return current_entry
        && current_entry->document_state.id == target.target_top_level_entry->document_state.id;
}

void CanonicalTraversable::traverse_the_history_by_delta(int delta, CheckForCancelation check_for_cancelation, Function<void(HistoryTraversalOutcome)> on_complete, Function<void()> on_top_level_traversal_applied)
{
    m_history_traversal_queue.append_session_history_traversal_steps(
        [this, delta, check_for_cancelation, on_complete = move(on_complete), on_top_level_traversal_applied = move(on_top_level_traversal_applied)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
            auto view = ViewImplementation::find_view_for_traversable(*this);
            VERIFY(view.has_value());
            if (delta < 0
                && check_for_cancelation != CheckForCancelation::No
                && view->has_uncommitted_top_level_navigation()) {
                view->cancel_uncommitted_top_level_navigation_for_browser_traversal();
                if (on_complete)
                    on_complete({ .status = HistoryTraversalStatus::Started });
                promise->resolve({});
                return;
            }

            auto target = m_session_history.traversal_target_for_delta(delta);
            if (!target.has_value()) {
                view->dump_session_history("traverse-no-entry"sv);
                if (on_complete)
                    on_complete({ .status = HistoryTraversalStatus::NoEntry });
                promise->resolve({});
                return;
            }

            traverse_the_history(*target, check_for_cancelation, move(on_complete), move(on_top_level_traversal_applied), move(promise));
        });
}

void CanonicalTraversable::traverse_the_history_to_step(i32 step, CheckForCancelation check_for_cancelation, Function<void(HistoryTraversalOutcome)> on_complete, Function<void()> on_top_level_traversal_applied)
{
    m_history_traversal_queue.append_session_history_traversal_steps(
        [this, step, check_for_cancelation, on_complete = move(on_complete), on_top_level_traversal_applied = move(on_top_level_traversal_applied)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
            auto target = m_session_history.traversal_target_for_step(step);
            if (!target.has_value()) {
                auto view = ViewImplementation::find_view_for_traversable(*this);
                VERIFY(view.has_value());
                view->dump_session_history("traverse-no-entry"sv);
                if (on_complete)
                    on_complete({ .status = HistoryTraversalStatus::NoEntry });
                promise->resolve({});
                return;
            }

            traverse_the_history(*target, check_for_cancelation, move(on_complete), move(on_top_level_traversal_applied), move(promise));
        });
}

void CanonicalTraversable::reconstruct_the_history_to_step(i32 step, bool requires_process_replacement, Function<void()> on_top_level_traversal_applied)
{
    m_history_traversal_queue.append_session_history_traversal_steps(
        [this, step, requires_process_replacement, on_top_level_traversal_applied = move(on_top_level_traversal_applied)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
            auto target = m_session_history.traversal_target_for_step(step);
            if (!target.has_value()) {
                promise->resolve({});
                return;
            }

            traverse_the_history(*target, CheckForCancelation::No, nullptr, move(on_top_level_traversal_applied), move(promise), requires_process_replacement);
        });
}

void CanonicalTraversable::traverse_the_history(TraversableSessionHistory::TraversalTarget const& target, CheckForCancelation check_for_cancelation, Function<void(HistoryTraversalOutcome)> on_complete, Function<void()> on_top_level_traversal_applied, NonnullRefPtr<Core::Promise<Empty>> promise, bool requires_process_replacement)
{
    auto view = ViewImplementation::find_view_for_traversable(*this);
    VERIFY(view.has_value());

    auto web_content_can_apply_target = web_content_can_apply_traversal(target);
    auto const* current_entry = m_session_history.current_entry();
    auto target_changes_document = !current_entry
        || current_entry->document_state.id != target.target_top_level_entry->document_state.id;
    auto will_replace_web_content_process = requires_process_replacement
        || target_changes_document
        || SiteIsolationManager::the().navigation_requires_process_swap(view->url(), target.target_top_level_entry->url);
    auto traversal_state = make_ref_counted<BrowserHistoryTraversalState>();
    traversal_state->will_change_top_level_entry = target.changes_top_level_entry;
    traversal_state->will_replace_web_content_process = will_replace_web_content_process;
    traversal_state->on_traversal_outcome = move(on_complete);
    traversal_state->on_top_level_traversal_applied = move(on_top_level_traversal_applied);

    auto should_check_for_cancelation = check_for_cancelation == CheckForCancelation::Yes
        || (check_for_cancelation == CheckForCancelation::IfWebContentCannotTraverseTarget && !web_content_can_apply_target);
    view->will_apply_history_traversal_step(target.target_top_level_entry->url, true);
    run_ui_history_operation_at_queue_position(
        BrowserHistoryTraversalOperation {
            .target_step = target.target_step,
            .check_for_cancelation = should_check_for_cancelation,
            .reconstructs_web_content_history = !web_content_can_apply_target || will_replace_web_content_process,
            .requires_process_replacement = will_replace_web_content_process,
            .state = traversal_state,
        },
        [this, traversal_state](Web::HTML::HistoryStepResult result, Optional<i32>) {
            auto view = ViewImplementation::find_view_for_traversable(*this);
            VERIFY(view.has_value());
            view->apply_history_traversal_step_result(result, traversal_state);
        },
        move(promise));
    if (!should_check_for_cancelation)
        traversal_state->notify_traversal_outcome(HistoryTraversalStatus::Started);
}

WebContentHistoryStepResult CanonicalTraversable::did_traverse_the_history_to_step(Web::HTML::HistoryStepResult result, RefPtr<BrowserHistoryTraversalState> const& traversal_state)
{
    if (result != Web::HTML::HistoryStepResult::Applied)
        return { .dump_reason = "webcontent-history-step-canceled"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_update_webdriver_pending_navigation_to_current_url = true, .should_reset_webdriver_pending_navigation_completion = true };

    auto should_complete_webdriver_pending_navigation = traversal_state->did_notify_top_level_traversal_applied
        || !traversal_state->will_change_top_level_entry;
    Optional<URL::URL> current_url;
    if (auto const* current_entry = m_session_history.current_entry())
        current_url = current_entry->url;
    return { .dump_reason = "webcontent-history-step-applied"sv, .should_update_navigation_action_state = true, .current_url = move(current_url), .should_complete_webdriver_pending_navigation = should_complete_webdriver_pending_navigation };
}

void CanonicalTraversable::abandon_after_web_content_process_crash()
{
    abandon_history_operations();
}

void CanonicalTraversable::reset_session_history_for_testing(
    Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    abandon_history_operations();
    m_session_history.clear();
    m_session_history.initialize_with_initial_history_entry(move(active_entry));
}

StringView CanonicalTraversable::browser_history_traversal_stage_to_string(BrowserHistoryTraversalDiagnostic::Stage stage)
{
    switch (stage) {
    case BrowserHistoryTraversalDiagnostic::Stage::ApplyingInWebContent:
        return "applying-in-webcontent"sv;
    case BrowserHistoryTraversalDiagnostic::Stage::CheckingCancelation:
        return "checking-cancelation"sv;
    }
    VERIFY_NOT_REACHED();
}

struct CanonicalTraversable::HistoryOperation {
    using Parameters = Variant<Web::HistoryOperationParameters, BrowserHistoryTraversalOperation>;

    HistoryOperation(u64 operation_id, Parameters parameters, Optional<u64> initiation_id, RefPtr<WebContentClient> initiating_client, u64 initiating_page_id, Optional<i32> resolved_step, OnHistoryOperationComplete on_complete)
        : operation_id(operation_id)
        , initiation_id(initiation_id)
        , parameters(move(parameters))
        , on_complete(move(on_complete))
        , initiating_client(move(initiating_client))
        , initiating_page_id(initiating_page_id)
        , resolved_step(resolved_step)
    {
    }

    u64 operation_id { 0 };
    Optional<u64> initiation_id;
    Parameters parameters;
    OnHistoryOperationComplete on_complete;
    // The initiating endpoint owns the state parked under initiation_id and must remain stable across process
    // replacement. Jobs for individual navigables resolve their endpoints when they are dispatched instead.
    RefPtr<WebContentClient> initiating_client;
    u64 initiating_page_id { 0 };
    struct CompletionEndpoint {
        RefPtr<WebContentClient> client;
        u64 page_id { 0 };
        Optional<u64> initiation_id;
    };
    Vector<CompletionEndpoint> completion_endpoints;
    // Delta and Navigation API traversals resolve their canonical target when their queue position is reached.
    Optional<i32> resolved_step;
    // Push and replace receive their canonical target before their process-local finalization steps run.
    Optional<i32> assigned_target_step;
    bool reconstructs_web_content_history { false };
    bool requires_process_replacement { false };
    Function<void(ApplyHistoryStepJobs::InitiatorSandboxingCheckResult)> pending_sandboxing_check;
    Function<void(Web::HTML::HistoryStepResult)> pending_unload_cancelation;
    HashMap<Web::HTML::CrossProcessId, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)>> pending_changing_jobs;
    HashMap<Web::HTML::CrossProcessId, Function<void()>> pending_continuations;
    HashMap<Web::HTML::CrossProcessId, Function<void()>> pending_nonchanging_updates;
    OwnPtr<ApplyHistoryStep> algorithm;
    RefPtr<Core::Promise<Empty>> queue_promise;
};

CanonicalTraversable::~CanonicalTraversable() = default;

Optional<CanonicalTraversable::BrowserHistoryTraversalDiagnostic> CanonicalTraversable::browser_history_traversal_for_testing() const
{
    for (auto const& operation : m_history_operations) {
        if (!operation.value->parameters.has<BrowserHistoryTraversalOperation>())
            continue;
        auto const& parameters = operation.value->parameters.get<BrowserHistoryTraversalOperation>();
        auto target = m_session_history.traversal_target_for_step(parameters.target_step);
        if (!target.has_value())
            return {};
        return BrowserHistoryTraversalDiagnostic {
            .target_step = parameters.target_step,
            .target_step_index = target->target_step_index,
            .will_change_top_level_entry = parameters.state ? parameters.state->will_change_top_level_entry : target->changes_top_level_entry,
            .will_replace_web_content_process = parameters.state ? parameters.state->will_replace_web_content_process : parameters.requires_process_replacement,
            .stage = operation.value->pending_unload_cancelation
                ? BrowserHistoryTraversalDiagnostic::Stage::CheckingCancelation
                : BrowserHistoryTraversalDiagnostic::Stage::ApplyingInWebContent,
        };
    }
    return {};
}

void CanonicalTraversable::recover_from_web_content_process_crash(OnHistoryOperationComplete on_complete)
{
    for (auto& operation : m_history_operations) {
        if (!operation.value->parameters.has<BrowserHistoryTraversalOperation>())
            continue;

        auto endpoint = history_job_endpoint_for(*this);
        VERIFY(endpoint.client);
        operation.value->pending_sandboxing_check = nullptr;
        operation.value->pending_unload_cancelation = nullptr;
        operation.value->pending_changing_jobs.clear();
        operation.value->pending_continuations.clear();
        operation.value->pending_nonchanging_updates.clear();
        operation.value->algorithm = nullptr;
        operation.value->initiating_client = endpoint.client;
        operation.value->initiating_page_id = endpoint.page_id;
        operation.value->completion_endpoints.clear();
        auto& parameters = operation.value->parameters.get<BrowserHistoryTraversalOperation>();
        if (parameters.state)
            parameters.state->notify_traversal_outcome(HistoryTraversalStatus::Started);
        parameters.check_for_cancelation = false;
        parameters.reconstructs_web_content_history = true;
        parameters.restores_replacement_process = true;
        VERIFY(operation.value->queue_promise);
        start_history_operation(*operation.value, *operation.value->queue_promise);
        return;
    }

    abandon_history_operations();

    auto current_step = m_session_history.current_step();
    if (!current_step.has_value()) {
        if (on_complete)
            on_complete(Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
        return;
    }
    enqueue_history_operation(
        BrowserHistoryTraversalOperation {
            .target_step = *current_step,
            .check_for_cancelation = false,
            .reconstructs_web_content_history = true,
            .restores_replacement_process = true,
        },
        move(on_complete));
}

CanonicalTraversable::HistoryJobEndpoint CanonicalTraversable::history_job_endpoint_for(CanonicalNavigable const& navigable) const
{
    if (auto* client = navigable.reporting_client_if_any())
        return { client, navigable.reporting_page_id() };

    // NB: The traversable is the view's root navigable; the process hosting its documents is the view's client
    //     rather than a reporting client recorded in the tree.
    if (&navigable == this) {
        if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value())
            return { &view->client(), view->page_id() };
    }
    return {};
}

CanonicalTraversable::HistoryOperation* CanonicalTraversable::find_history_operation(u64 operation_id)
{
    auto operation = m_history_operations.find(operation_id);
    if (operation == m_history_operations.end())
        return nullptr;
    return operation->value.ptr();
}

void CanonicalTraversable::add_history_operation_completion_endpoint(HistoryOperation& operation, HistoryJobEndpoint endpoint, Optional<u64> initiation_id)
{
    VERIFY(endpoint.client);
    for (auto& existing : operation.completion_endpoints) {
        if (existing.client.ptr() != endpoint.client || existing.page_id != endpoint.page_id)
            continue;
        if (initiation_id.has_value())
            existing.initiation_id = initiation_id;
        return;
    }
    operation.completion_endpoints.append({ endpoint.client, endpoint.page_id, initiation_id });
}

ApplyHistoryStepJobs CanonicalTraversable::create_apply_history_step_jobs(u64 operation_id)
{
    return {
        .run_initiator_sandboxing_check_job = [this, operation_id](Web::HTML::CrossProcessId initiator_to_check, Vector<Web::HTML::CrossProcessId> navigables, Function<void(ApplyHistoryStepJobs::InitiatorSandboxingCheckResult)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;

            // sourceSnapshotParams remains parked under this operation's initiation ID in the initiating process.
            VERIFY(operation->initiating_client);
            VERIFY(operation->initiation_id.has_value());
            operation->pending_sandboxing_check = move(on_complete);
            operation->initiating_client->async_run_initiator_sandboxing_check_job(operation->initiating_page_id, operation_id, initiator_to_check, move(navigables), *operation->initiation_id); },
        .run_unload_cancelation_job = [this, operation_id](ApplyHistoryStepJobs::UnloadCancelationJob job, Function<void(Web::HTML::HistoryStepResult)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            VERIFY(operation->initiating_client);
            operation->pending_unload_cancelation = move(on_complete);
            operation->initiating_client->async_run_history_step_unload_cancelation_job(operation->initiating_page_id, operation_id, move(job.target_entry), move(job.navigables_crossing_documents), job.user_involvement); },
        .run_changing_navigable_history_step_job = [this, operation_id](ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            if (job.navigable_id == id() && job.navigation_type == Web::Bindings::NavigationType::Traverse) {
                auto reconstructs_web_content_history = operation->reconstructs_web_content_history;
                auto requires_process_replacement = operation->requires_process_replacement
                    || (operation->parameters.has<BrowserHistoryTraversalOperation>()
                        && operation->parameters.get<BrowserHistoryTraversalOperation>().requires_process_replacement);
                auto restores_replacement_process = operation->parameters.has<BrowserHistoryTraversalOperation>()
                    && operation->parameters.get<BrowserHistoryTraversalOperation>().restores_replacement_process;
                if (!restores_replacement_process) {
                    auto view = ViewImplementation::find_view_for_traversable(*this);
                    if (requires_process_replacement
                        || (view.has_value() && SiteIsolationManager::the().navigation_requires_process_swap(view->url(), job.target_entry.url))) {
                        VERIFY(view.has_value());
                        view->replace_web_content_process_for_history_traversal(job.target_entry.document_state.id);
                        restores_replacement_process = true;
                        reconstructs_web_content_history = true;
                        operation->reconstructs_web_content_history = true;
                        if (!operation->initiation_id.has_value())
                            operation->completion_endpoints.clear();
                    }
                }
                if (reconstructs_web_content_history) {
                    job.reconstructs_replacement_process = true;
                }
            }
            auto navigable = find(job.navigable_id);
            auto endpoint = navigable.has_value() ? history_job_endpoint_for(*navigable) : HistoryJobEndpoint {};
            if (!endpoint.client) {
                on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);
                return;
            }
            add_history_operation_completion_endpoint(*operation, endpoint);
            auto navigable_id = job.navigable_id;
            auto initiation_id = endpoint.client == operation->initiating_client.ptr() && endpoint.page_id == operation->initiating_page_id
                ? operation->initiation_id
                : Optional<u64> {};
            operation->pending_changing_jobs.set(navigable_id, move(on_complete));
            endpoint.client->async_run_changing_navigable_history_job(endpoint.page_id, operation_id, navigable_id, move(job.target_entry), job.user_involvement, job.navigation_type, job.synchronous_navigation == Web::HTML::SynchronousNavigation::Yes, job.navigation_api_abort_behavior, initiation_id, job.reconstructs_replacement_process); },
        .apply_changing_navigable_history_step_continuation = [this, operation_id](ApplyHistoryStepJobs::ApplyChangingNavigableHistoryStepContinuation continuation, Function<void()> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable = find(continuation.navigable_id);
            auto endpoint = navigable.has_value() ? history_job_endpoint_for(*navigable) : HistoryJobEndpoint {};
            if (!endpoint.client) {
                on_complete();
                return;
            }
            add_history_operation_completion_endpoint(*operation, endpoint);
            auto navigable_id = continuation.navigable_id;
            operation->pending_continuations.set(navigable_id, move(on_complete));
            endpoint.client->async_apply_changing_navigable_continuation(endpoint.page_id, operation_id, navigable_id,
                continuation.history_object_length_and_index.script_history_length, continuation.history_object_length_and_index.script_history_index,
                move(continuation.entries_for_navigation_api)); },
        .update_nonchanging_navigable_history_step_state = [this, operation_id](Web::HTML::CrossProcessId navigable_id, Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index, Function<void()> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable = find(navigable_id);
            auto endpoint = navigable.has_value() ? history_job_endpoint_for(*navigable) : HistoryJobEndpoint {};
            if (!endpoint.client) {
                on_complete();
                return;
            }
            add_history_operation_completion_endpoint(*operation, endpoint);
            operation->pending_nonchanging_updates.set(navigable_id, move(on_complete));
            endpoint.client->async_update_nonchanging_navigable_history_state(endpoint.page_id, operation_id, navigable_id,
                history_object_length_and_index.script_history_length, history_object_length_and_index.script_history_index); },
    };
}

void CanonicalTraversable::run_history_operation_at_queue_position(u64 initiation_id, Web::HistoryOperationParameters request, WebContentClient& requesting_client, u64 requesting_page_id, Optional<i32> resolved_step, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = m_next_history_operation_id++;
    m_history_operations.set(operation_id, make<HistoryOperation>(operation_id, HistoryOperation::Parameters { move(request) }, initiation_id, &requesting_client, requesting_page_id, resolved_step, move(on_complete)));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    start_history_operation(*operation, promise);
}

void CanonicalTraversable::run_ui_history_operation_at_queue_position(BrowserHistoryTraversalOperation parameters, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = m_next_history_operation_id++;
    m_history_operations.set(operation_id, make<HistoryOperation>(operation_id, HistoryOperation::Parameters { move(parameters) }, Optional<u64> {}, nullptr, 0, Optional<i32> {}, move(on_complete)));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    start_history_operation(*operation, promise);
}

void CanonicalTraversable::append_history_queue_steps(SessionHistoryTraversalSteps steps)
{
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::enqueue_history_operation(u64 initiation_id, Web::HistoryOperationParameters request, WebContentClient& requesting_client, u64 requesting_page_id, OnHistoryOperationComplete on_complete)
{
    // https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-child-navigable
    // Steps 6-7 remove the nested history before step 9 appends traversal steps. Apply the canonical counterpart while
    // admitting the destruction request, rather than deferring it to the operation's eventual queue position.
    if (request.has<Web::NavigableDestructionHistoryOperationParameters>()) {
        auto const& parameters = request.get<Web::NavigableDestructionHistoryOperationParameters>();
        if (auto parent_navigable = find(parameters.parent_navigable_id); parent_navigable.has_value())
            remove_nested_history(*parent_navigable, parameters.parent_document_state_id, parameters.navigable_id);
    }

    Optional<Web::HTML::CrossProcessId> synchronous_navigation_target;
    if (request.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>()) {
        auto const& parameters = request.get<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>();
        synchronous_navigation_target = parameters.navigable_id;
        if (parameters.previous_entry_persisted_state.has_value()) {
            if (auto target_navigable = find(parameters.navigable_id); target_navigable.has_value()) {
                m_session_history.update_entry_persisted_state(
                    nested_history_id_for(*target_navigable),
                    *parameters.previous_entry_persisted_state);
            }
        }
    }

    NonnullRefPtr<WebContentClient> requesting_client_ref { requesting_client };
    auto steps = [this, initiation_id, request = move(request), requesting_client = move(requesting_client_ref), requesting_page_id, on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_history_operation_at_queue_position(initiation_id, move(request), *requesting_client, requesting_page_id, {}, move(on_complete), move(promise));
    };

    if (synchronous_navigation_target.has_value())
        m_history_traversal_queue.append_session_history_synchronous_navigation_steps(*synchronous_navigation_target, move(steps));
    else
        m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::enqueue_history_operation(BrowserHistoryTraversalOperation parameters, OnHistoryOperationComplete on_complete)
{
    auto steps = [this, parameters = move(parameters), on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_ui_history_operation_at_queue_position(move(parameters), move(on_complete), move(promise));
    };
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::apply_history_step(HistoryOperation& operation, i32 step, bool check_for_cancelation, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement user_involvement, Optional<Web::Bindings::NavigationType> navigation_type, Web::HTML::SynchronousNavigation synchronous_navigation, Optional<Web::HTML::CrossProcessId> navigable_with_finalized_entry)
{
    VERIFY(!operation.algorithm);
    auto operation_id = operation.operation_id;
    operation.algorithm = make<ApplyHistoryStep>(
        m_session_history, *this, m_history_traversal_queue, m_apply_history_step_traversable_state, create_apply_history_step_jobs(operation_id),
        step, check_for_cancelation, initiator_to_check, user_involvement, navigation_type, synchronous_navigation,
        navigable_with_finalized_entry,
        [this, operation_id](Web::HTML::HistoryStepResult result) {
            auto* operation = find_history_operation(operation_id);
            auto committed_step = operation && operation->algorithm ? operation->algorithm->committed_step() : Optional<i32> {};
            finish_history_operation(operation_id, result, committed_step);
        });
    operation.algorithm->apply_the_history_step();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#update-for-navigable-creation/destruction
void CanonicalTraversable::update_for_navigable_creation_or_destruction(HistoryOperation& operation)
{
    // 1. Let step be traversable's current session history step.
    auto step = m_session_history.current_step();
    if (!step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 2. Return the result of applying the history step step to traversable given false, null, null, "none", and null.
    apply_history_step(operation, *step, false, {}, Web::HTML::UserNavigationInvolvement::None, {}, Web::HTML::SynchronousNavigation::No, {});
}

void CanonicalTraversable::start_history_operation(HistoryOperation& operation, NonnullRefPtr<Core::Promise<Empty>>)
{
    if (!operation.initiating_client) {
        auto endpoint = history_job_endpoint_for(*this);
        operation.initiating_client = endpoint.client;
        operation.initiating_page_id = endpoint.page_id;
    }

    if (!operation.initiating_client) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
        return;
    }

    add_history_operation_completion_endpoint(operation, {
                                                             operation.initiating_client,
                                                             operation.initiating_page_id,
                                                         },
        operation.initiation_id);

    operation.parameters.visit(
        [&](Web::HistoryOperationParameters const& parameters) {
            VERIFY(operation.initiation_id.has_value());
            parameters.visit(
                [&](Web::PushHistoryOperationParameters const&) {
                    auto current_step = m_session_history.current_step();
                    if (!current_step.has_value()) {
                        operation.assigned_target_step = 0;
                        return;
                    }

                    VERIFY(*current_step < NumericLimits<i32>::max());
                    operation.assigned_target_step = *current_step + 1;
                },
                [&](Web::ReplaceHistoryOperationParameters const&) {
                    auto current_step = m_session_history.current_step();
                    operation.assigned_target_step = current_step.value_or(0);
                },
                [](auto const&) {});
            operation.initiating_client->async_history_operation_started(operation.initiating_page_id, operation.operation_id, *operation.initiation_id, operation.assigned_target_step);
        },
        [&](BrowserHistoryTraversalOperation& parameters) {
            operation.reconstructs_web_content_history = parameters.reconstructs_web_content_history
                || parameters.restores_replacement_process;
            operation.requires_process_replacement = parameters.requires_process_replacement;
            apply_history_step(operation, parameters.target_step, parameters.check_for_cancelation, {}, Web::HTML::UserNavigationInvolvement::BrowserUI, Web::Bindings::NavigationType::Traverse, Web::HTML::SynchronousNavigation::No,
                operation.reconstructs_web_content_history ? Optional<Web::HTML::CrossProcessId> { id() } : Optional<Web::HTML::CrossProcessId> {});
        });
}

void CanonicalTraversable::did_receive_history_operation_ready(u64 operation_id, bool proceed, Optional<Web::HTML::CrossProcessId> creation_parent_document_state_id, Optional<Web::HTML::SameDocumentNavigationEntry> same_document_navigation_finalization, Optional<Web::CrossDocumentNavigationFinalization> cross_document_navigation_finalization, Web::HTML::HistoryStepResult abandon_result)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation || operation->algorithm)
        return;

    if (!proceed) {
        finish_history_operation(operation_id, abandon_result, {});
        return;
    }

    auto apply_current_step = [&](bool check_for_cancelation, Web::HTML::UserNavigationInvolvement user_involvement,
                                  Optional<Web::Bindings::NavigationType> navigation_type, Optional<Web::HTML::CrossProcessId> navigable_with_finalized_entry) {
        auto step = m_session_history.current_step();
        if (!step.has_value()) {
            finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }
        apply_history_step(*operation, *step, check_for_cancelation, {}, user_involvement, navigation_type,
            Web::HTML::SynchronousNavigation::No, navigable_with_finalized_entry);
    };

    VERIFY(operation->parameters.has<Web::HistoryOperationParameters>());
    auto const& request = operation->parameters.get<Web::HistoryOperationParameters>();
    if (!request.has<Web::NavigableCreationHistoryOperationParameters>())
        VERIFY(!creation_parent_document_state_id.has_value());
    if (!request.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>())
        VERIFY(!same_document_navigation_finalization.has_value());
    if (!request.has<Web::PushHistoryOperationParameters>() && !request.has<Web::ReplaceHistoryOperationParameters>())
        VERIFY(!cross_document_navigation_finalization.has_value());

    auto finalize_cross_document_navigation = [&](Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId pending_document_state_id, Optional<i32> expected_entry_step) {
        if (!cross_document_navigation_finalization.has_value()) {
            finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return false;
        }

        auto finalization = cross_document_navigation_finalization.release_value();
        if (finalization.history_entry.document_state.id != pending_document_state_id
            || (expected_entry_step.has_value() && finalization.history_entry.step != *expected_entry_step)) {
            finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return false;
        }

        auto navigable = find(navigable_id);
        if (!navigable.has_value()
            || !m_session_history.finalize_cross_document_navigation(
                nested_history_id_for(*navigable), move(finalization.history_entry),
                move(finalization.entry_to_replace_navigation_api_key))) {
            finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return false;
        }

        return true;
    };

    request.visit(
        [&](Web::PushHistoryOperationParameters const& parameters) {
            VERIFY(operation->assigned_target_step.has_value());
            if (!finalize_cross_document_navigation(parameters.navigable_id, parameters.pending_document_state_id, operation->assigned_target_step))
                return;
            apply_history_step(*operation, *operation->assigned_target_step, false, {}, parameters.user_involvement, Web::Bindings::NavigationType::Push,
                Web::HTML::SynchronousNavigation::No, parameters.navigable_id);
        },
        [&](Web::ReplaceHistoryOperationParameters const& parameters) {
            VERIFY(operation->assigned_target_step.has_value());
            if (!finalize_cross_document_navigation(parameters.navigable_id, parameters.pending_document_state_id, {}))
                return;
            apply_history_step(*operation, *operation->assigned_target_step, false, {}, parameters.user_involvement, Web::Bindings::NavigationType::Replace,
                Web::HTML::SynchronousNavigation::No, parameters.navigable_id);
        },
        [&](Web::ReloadHistoryOperationParameters const& parameters) {
            apply_current_step(true, parameters.user_involvement, Web::Bindings::NavigationType::Reload, parameters.navigable_id);
        },
        [&](Web::TraverseByDeltaHistoryOperationParameters const& parameters) {
            VERIFY(operation->resolved_step.has_value());
            apply_history_step(*operation, *operation->resolved_step, true, parameters.initiator_to_check, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::TraverseToStepHistoryOperationParameters const& parameters) {
            apply_history_step(*operation, parameters.target_step, true, {}, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::NavigationAPITraverseHistoryOperationParameters const& parameters) {
            VERIFY(operation->resolved_step.has_value());
            apply_history_step(*operation, *operation->resolved_step, true, parameters.navigable_id, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::ResumeTraverseHistoryOperationParameters const& parameters) {
            apply_history_step(*operation, parameters.target_step, false, {}, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::NavigableCreationHistoryOperationParameters const& parameters) {
            VERIFY(creation_parent_document_state_id.has_value());
            auto parent_navigable = find(parameters.parent_navigable_id);
            if (!parent_navigable.has_value()
                || !append_nested_history(*parent_navigable, *creation_parent_document_state_id, parameters.navigable_id, parameters.initial_history_entry).has_value()) {
                finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
                return;
            }

            // Steps 1-6 of create-a-new-child-navigable's queued work are complete. Resume at step 7.
            update_for_navigable_creation_or_destruction(*operation);
        },
        [&](Web::NavigableDestructionHistoryOperationParameters const&) {
            update_for_navigable_creation_or_destruction(*operation);
        },
        [&](Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& parameters) {
            VERIFY(parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Push || parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace);
            if (!same_document_navigation_finalization.has_value()
                || same_document_navigation_finalization->document_state_id != parameters.target_entry.document_state_id
                || same_document_navigation_finalization->navigation_api_key != parameters.target_entry.navigation_api_key
                || same_document_navigation_finalization->navigation_api_id != parameters.target_entry.navigation_api_id) {
                finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
                return;
            }

            auto navigable = find(parameters.navigable_id);
            if (!navigable.has_value()) {
                finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
                return;
            }

            auto target_step = m_session_history.finalize_same_document_navigation(
                *navigable, same_document_navigation_finalization.release_value(),
                parameters.entry_to_replace_navigation_api_key);
            if (!target_step.has_value()) {
                finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
                return;
            }

            auto navigation_type = parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace
                ? Web::Bindings::NavigationType::Replace
                : Web::Bindings::NavigationType::Push;
            apply_history_step(*operation, *target_step, false, {}, parameters.user_involvement, navigation_type,
                Web::HTML::SynchronousNavigation::Yes, parameters.navigable_id);
        },
        [&](Web::CloseTopLevelTraversableHistoryOperationParameters const&) {
            // Close runs entirely in the requesting process at this queue position and must complete with proceed=false.
            VERIFY_NOT_REACHED();
        },
        [&](Web::ResetSessionHistoryForTestingOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::FlushSessionHistoryTraversalQueueOperationParameters const&) {
            VERIFY_NOT_REACHED();
        });
}

void CanonicalTraversable::finish_history_operation(u64 operation_id, Web::HTML::HistoryStepResult result, Optional<i32> committed_step)
{
    auto operation = m_history_operations.take(operation_id);
    if (!operation.has_value())
        return;
    auto& taken_operation = **operation;

    for (auto& endpoint : taken_operation.completion_endpoints)
        endpoint.client->async_complete_history_operation(endpoint.page_id, operation_id, result, committed_step, endpoint.initiation_id);

    if (taken_operation.on_complete)
        taken_operation.on_complete(result, committed_step);

    if (taken_operation.parameters.has<BrowserHistoryTraversalOperation>()) {
        auto const& parameters = taken_operation.parameters.get<BrowserHistoryTraversalOperation>();
        if (parameters.state) {
            auto status = result == Web::HTML::HistoryStepResult::Applied
                ? HistoryTraversalStatus::Started
                : HistoryTraversalStatus::Canceled;
            parameters.state->notify_traversal_outcome(status);
        }
    }

    // NB: Resolving the queue promise can synchronously start the next queued operation.
    if (taken_operation.queue_promise)
        taken_operation.queue_promise->resolve({});

    // The completion callback that brought us here can be running inside the algorithm object; destroy the
    // operation only once the stack has unwound.
    Core::deferred_invoke([operation = operation.release_value()] { });
}

void CanonicalTraversable::abandon_history_operations()
{
    while (!m_history_operations.is_empty()) {
        auto operation_id = m_history_operations.begin()->key;
        finish_history_operation(operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
    }
}

void CanonicalTraversable::did_receive_initiator_sandboxing_check_result(u64 operation_id, bool allowed)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (auto pending = move(operation->pending_sandboxing_check))
            pending(allowed ? ApplyHistoryStepJobs::InitiatorSandboxingCheckResult::Allowed : ApplyHistoryStepJobs::InitiatorSandboxingCheckResult::Disallowed);
    }
}

void CanonicalTraversable::did_receive_history_step_unload_cancelation_result(u64 operation_id, Web::HTML::HistoryStepResult result)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (operation->parameters.has<BrowserHistoryTraversalOperation>()) {
            auto const& parameters = operation->parameters.get<BrowserHistoryTraversalOperation>();
            if (result == Web::HTML::HistoryStepResult::CanceledPendingNavigation)
                result = Web::HTML::HistoryStepResult::Applied;
            if (result == Web::HTML::HistoryStepResult::Applied && parameters.state)
                parameters.state->notify_traversal_outcome(HistoryTraversalStatus::Started);
        }
        if (auto pending = move(operation->pending_unload_cancelation))
            pending(result);
    }
}

void CanonicalTraversable::did_receive_changing_navigable_history_job_ready(u64 operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (auto pending = operation->pending_changing_jobs.take(navigable_id); pending.has_value())
            (*pending)(disposition);
    }
}

void CanonicalTraversable::did_receive_changing_navigable_continuation_applied(u64 operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (auto pending = operation->pending_continuations.take(navigable_id); pending.has_value()) {
            if (previous_entry_persisted_state.has_value()) {
                auto navigable = find(navigable_id);
                if (navigable.has_value()) {
                    m_session_history.update_entry_persisted_state(
                        nested_history_id_for(*navigable),
                        *previous_entry_persisted_state);
                }
            }
            // A replacement document's creation operation also applies a top-level continuation, but it runs before
            // the document has accepted the UI-owned history state. Only the browser traversal itself reaches the
            // observable top-level completion point here.
            if (navigable_id == id() && operation->parameters.has<BrowserHistoryTraversalOperation>()) {
                auto const& parameters = operation->parameters.get<BrowserHistoryTraversalOperation>();
                if (parameters.state)
                    parameters.state->notify_top_level_traversal_applied();
            }
            (*pending)();
        }
    }
}

void CanonicalTraversable::did_receive_nonchanging_navigable_history_state_updated(u64 operation_id, Web::HTML::CrossProcessId navigable_id)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (auto pending = operation->pending_nonchanging_updates.take(navigable_id); pending.has_value())
            (*pending)();
    }
}

}
