/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/HistoryExecutor.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ChangingNavigableContinuationState);
GC_DEFINE_ALLOCATOR(HistoryExecutor);

void ChangingNavigableContinuationState::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(displayed_document);
    visitor.visit(navigable);
    visitor.visit(pending_document);
    visitor.visit(population_output);
    visitor.visit(resolved_document);
}

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

}
