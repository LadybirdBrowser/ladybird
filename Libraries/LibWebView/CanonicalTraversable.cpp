/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/StorageJar.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

CanonicalTraversable::CanonicalTraversable()
    : CanonicalNavigable({}, {}, nullptr, 0)
    , m_session_storage(StorageJar::create())
{
}

void CanonicalTraversable::clone_session_storage_from(CanonicalTraversable const& other)
{
    // https://storage.spec.whatwg.org/#legacy-clone-a-traversable-storage-shed
    // 1. For each key → shelf of A's storage shed:
    //    1. Let newShelf be the result of running create a storage shelf with "session".
    //    2. Set newShelf's bucket map["default"]'s bottle map["sessionStorage"]'s map to a clone of
    //       shelf's bucket map["default"]'s bottle map["sessionStorage"]'s map.
    //    3. Set B's storage shed[key] to newShelf.
    m_session_storage->clone_from(*other.m_session_storage);
}

// https://html.spec.whatwg.org/multipage/interaction.html#system-visibility-state
void CanonicalTraversable::set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    if (m_system_visibility_state == visibility_state)
        return;
    m_system_visibility_state = visibility_state;

    // When a user agent determines that the system visibility state for
    // traversable navigable traversable has changed to newState, it must run the following steps:

    // 1. Let navigables be the inclusive descendant navigables of traversable's active document.
    // 2. For each navigable of navigables:
    for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
        // 1. Let document be navigable's active document.
        auto endpoint = history_job_endpoint_for(navigable);
        if (!endpoint.client)
            return IterationDecision::Continue;

        // 2. Queue a global task on the user interaction task source given document's relevant global object
        //    to update the visibility state of document with newState.
        endpoint.client->async_update_visibility_state(endpoint.page_id, navigable.id(), visibility_state);
        return IterationDecision::Continue;
    });
}

CanonicalNavigable& CanonicalTraversable::insert(WebContentClient& reporting_client, u64 page_id, Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, Web::HTML::ReplicatedNavigableState replicated_state, CanonicalNavigable& fallback_parent)
{
    Optional<Web::HTML::SessionHistoryEntryIdentity> current_session_history_entry;
    Vector<CanonicalNavigable::PendingSameDocumentSessionHistoryEntry> pending_same_document_session_history_entries;
    auto existing_navigable = find(frame_id);
    if (existing_navigable.has_value()) {
        current_session_history_entry = existing_navigable->current_session_history_entry_identity();
        pending_same_document_session_history_entries = existing_navigable->take_pending_same_document_session_history_entries();
        remove(*existing_navigable);
    } else {
        current_session_history_entry = replicated_state.active_session_history_entry_identity;
    }

    auto navigable = make<CanonicalNavigable>(frame_id, parent_frame_id, &reporting_client, page_id);
    navigable->set_current_session_history_entry_identity(move(current_session_history_entry));
    navigable->append_pending_same_document_session_history_entries(move(pending_same_document_session_history_entries));
    navigable->set_replicated_state(move(replicated_state));

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
    navigable.clear_ongoing_navigation();
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

void CanonicalTraversable::session_history_changed()
{
    if (on_session_history_changed)
        on_session_history_changed();
}

ByteString CanonicalTraversable::pending_same_document_session_history_entries_for_debug() const
{
    StringBuilder builder;
    builder.append('[');
    bool first = true;
    for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        for (auto const& pending_entry : navigable.pending_same_document_session_history_entries()) {
            if (!first)
                builder.append(", "sv);
            first = false;
            builder.appendff("{{navigable={}, operation={}, url={}, document_state={}, navigation_id={}}}",
                navigable.id(), pending_entry.operation_id, pending_entry.entry.url,
                pending_entry.entry.document_state_id, pending_entry.entry.navigation_api_id);
        }
        return IterationDecision::Continue;
    });
    builder.append(']');
    return builder.to_byte_string();
}

void CanonicalTraversable::prepare_for_reload()
{
    m_session_history.mark_current_entry_reload_pending();
    session_history_changed();
}

void CanonicalTraversable::did_create_top_level_traversable(Web::HTML::SessionHistoryEntryDescriptor initial_history_entry)
{
    if (!m_session_history.entries().is_empty())
        return;
    set_current_session_history_entry(initial_history_entry);
    set_active_session_history_entry(initial_history_entry);
    m_session_history.initialize_with_initial_history_entry(move(initial_history_entry));
    session_history_changed();
}

Optional<Web::HTML::CrossProcessId> CanonicalTraversable::nested_history_id_for(CanonicalNavigable const& navigable) const
{
    if (&navigable == this)
        return {};
    return navigable.id();
}

bool CanonicalTraversable::update_session_history_entry_navigation_api_state(CanonicalNavigable& navigable, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Web::HTML::StorageSerializationRecord navigation_api_state)
{
    VERIFY(&navigable.top_level_traversable() == this);

    auto updated = navigable.update_pending_same_document_session_history_entry(entry_identity, [&](auto& entry) {
        entry.navigation_api_state = navigation_api_state;
    });
    if (!updated) {
        updated = m_session_history.update_entry(nested_history_id_for(navigable), entry_identity, [&](auto& entry) {
            entry.navigation_api_state = navigation_api_state;
        });
    }
    if (updated)
        session_history_changed();
    return updated;
}

bool CanonicalTraversable::update_session_history_entry_scroll_restoration_mode(CanonicalNavigable& navigable, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    VERIFY(&navigable.top_level_traversable() == this);

    auto updated = navigable.update_pending_same_document_session_history_entry(entry_identity, [&](auto& entry) {
        entry.scroll_restoration_mode = scroll_restoration_mode;
    });
    if (!updated) {
        updated = m_session_history.update_entry(nested_history_id_for(navigable), entry_identity, [&](auto& entry) {
            entry.scroll_restoration_mode = scroll_restoration_mode;
        });
    }
    if (updated)
        session_history_changed();
    return updated;
}

bool CanonicalTraversable::update_session_history_entry_persisted_state(CanonicalNavigable& navigable, Web::HTML::SessionHistoryEntryPersistedState const& persisted_state)
{
    VERIFY(&navigable.top_level_traversable() == this);

    auto updated = navigable.update_pending_same_document_session_history_entry(persisted_state.entry_identity, [&](auto& entry) {
        entry.scroll_position_data = persisted_state.scroll_position_data;
    });
    if (!updated)
        updated = m_session_history.update_entry_persisted_state(nested_history_id_for(navigable), persisted_state);
    if (updated)
        session_history_changed();
    return updated;
}

bool CanonicalTraversable::update_session_history_entry_document_state_navigable_target_name(CanonicalNavigable& navigable, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Utf16String navigable_target_name)
{
    VERIFY(&navigable.top_level_traversable() == this);

    auto entry_is_addressable = navigable.has_pending_same_document_session_history_entry(entry_identity);
    if (!entry_is_addressable) {
        auto entries = m_session_history.get_session_history_entries(navigable);
        entry_is_addressable = entries.has_value() && entries->find_if([&](auto const& entry) {
            return Web::HTML::session_history_entry_identity(entry) == entry_identity;
        }) != entries->end();
    }
    if (!entry_is_addressable)
        return false;

    auto updated = m_session_history.update_document_state(entry_identity.document_state_id, [&](auto& document_state) {
        document_state.navigable_target_name = navigable_target_name;
    });
    if (updated)
        session_history_changed();
    return updated;
}

bool CanonicalTraversable::set_session_history_entry_document_state_reload_pending(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, bool reload_pending)
{
    VERIFY(&navigable.top_level_traversable() == this);

    auto updated = m_session_history.update_document_state(nested_history_id_for(navigable), navigation_api_key, [&](auto& document_state) {
        document_state.reload_pending = reload_pending;
    });
    if (updated)
        session_history_changed();
    return updated;
}

Optional<i32> CanonicalTraversable::append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id, Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    auto child_navigable = find(child_navigable_id);
    if (!child_navigable.has_value() || child_navigable->parent() != &parent_navigable)
        return {};
    auto target_step = m_session_history.append_nested_history(parent_navigable, parent_document_state_id, child_navigable_id, move(initial_history_entry));
    if (!target_step.has_value())
        return {};

    session_history_changed();
    return target_step;
}

bool CanonicalTraversable::remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    auto removed = m_session_history.remove_nested_history(parent_navigable, parent_document_state_id, child_navigable_id);
    if (removed)
        session_history_changed();
    return removed;
}

void CanonicalTraversable::traverse_the_history_by_delta(int delta, CheckForCancelation check_for_cancelation, Function<void()> on_ready)
{
    if (m_pending_browser_history_traversal.has_value()
        && m_pending_browser_history_traversal->stage == PendingBrowserHistoryTraversal::Stage::Queued) {
        queue_browser_history_traversal({}, delta, check_for_cancelation, move(on_ready));
        return;
    }

    if (auto* operation = ongoing_browser_history_traversal()) {
        supersede_browser_history_traversal_by_delta(*operation, delta, move(on_ready));
        return;
    }

    queue_browser_history_traversal({}, delta, check_for_cancelation, move(on_ready));
}

void CanonicalTraversable::traverse_the_history_to_step(i32 step, CheckForCancelation check_for_cancelation, Function<void()> on_ready)
{
    if (m_pending_browser_history_traversal.has_value()
        && m_pending_browser_history_traversal->stage == PendingBrowserHistoryTraversal::Stage::Queued) {
        queue_browser_history_traversal(step, {}, check_for_cancelation, move(on_ready));
        return;
    }

    if (auto* operation = ongoing_browser_history_traversal()) {
        auto target = m_session_history.traversal_target_for_step(step);
        if (!target.has_value()) {
            if (on_ready)
                on_ready();
            return;
        }
        supersede_browser_history_traversal(*operation, target.release_value(), move(on_ready));
        return;
    }

    queue_browser_history_traversal(step, {}, check_for_cancelation, move(on_ready));
}

void CanonicalTraversable::reconstruct_the_history_to_step(i32 step)
{
    m_history_traversal_queue.append_session_history_traversal_steps(
        [this, step](NonnullRefPtr<Core::Promise<Empty>> promise) {
            auto target = m_session_history.traversal_target_for_step(step);
            if (!target.has_value()) {
                promise->resolve({});
                return;
            }

            set_current_session_history_entry_identity({});
            traverse_the_history(*target, CheckForCancelation::No, nullptr, move(promise));
        });
}

ErrorOr<URL::URL> CanonicalTraversable::restore_session_history_from_ui_snapshot(SessionHistorySnapshot snapshot)
{
    TRY(m_session_history.restore_from_ui_snapshot(move(snapshot.entries), move(snapshot.used_steps), snapshot.current_used_step_index, [] { return Application::the().allocate_ui_process_cross_process_id(); }));
    session_history_changed();

    auto const* current_entry = m_session_history.current_entry();
    VERIFY(current_entry);
    return current_entry->url;
}

void CanonicalTraversable::traverse_the_history(TraversableSessionHistory::TraversalTarget const& target, CheckForCancelation check_for_cancelation, Function<void()> on_ready, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    run_browser_history_traversal_at_queue_position(
        Web::TraverseToStepHistoryOperationParameters {
            .traversable_id = id(),
            .target_step = target.target_step,
            .user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI,
        },
        check_for_cancelation == CheckForCancelation::Yes,
        next_sequence_number(),
        move(on_ready),
        nullptr,
        move(promise));
}

void CanonicalTraversable::abandon_after_web_content_process_crash()
{
    abandon_history_operations();
    // The canonical current entry survives, but no live document hosts it. Clearing only the active identity makes
    // the next traversal reconstruct the entry instead of treating it as a same-document update.
    clear_active_session_history_entry_identity();
}

void CanonicalTraversable::reset_session_history_for_testing(
    Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    abandon_history_operations();
    m_session_history.clear();
    set_current_session_history_entry(active_entry);
    set_active_session_history_entry(active_entry);
    m_session_history.initialize_with_initial_history_entry(move(active_entry));
    session_history_changed();
}

bool CanonicalTraversable::initialize_session_history_for_testing(Vector<TraversableSessionHistory::Entry> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    abandon_history_operations();
    if (!m_session_history.initialize_for_testing(move(entries), move(used_steps), current_used_step_index))
        return false;
    auto const* active_entry = m_session_history.current_entry();
    VERIFY(active_entry);
    set_current_session_history_entry(*active_entry);
    set_active_session_history_entry(*active_entry);
    session_history_changed();
    return true;
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
    HistoryOperation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters parameters, RefPtr<WebContentClient> initiating_client, u64 initiating_page_id, u64 sequence_number, OnHistoryOperationComplete on_complete)
        : operation_id(operation_id)
        , parameters(move(parameters))
        , on_complete(move(on_complete))
        , initiating_client(move(initiating_client))
        , initiating_page_id(initiating_page_id)
        , sequence_number(sequence_number)
    {
    }

    Web::HTML::CrossProcessId operation_id;
    Web::HistoryOperationParameters parameters;
    OnHistoryOperationComplete on_complete;
    // The initiating endpoint owns the state parked under the operation id and must remain stable across process
    // replacement. Jobs for individual navigables resolve their endpoints when they are dispatched instead.
    RefPtr<WebContentClient> initiating_client;
    u64 initiating_page_id { 0 };
    Vector<HistoryJobEndpoint> completion_endpoints;
    u64 sequence_number;
    bool was_initiated_by_browser { false };
    bool owns_navigation_transaction { false };
    bool check_for_cancelation { false };
    Function<void()> on_browser_traversal_ready;
    Function<void(Web::HTML::HistoryStepResult)> pending_unload_cancelation;

    struct PendingChangingJob {
        enum class Phase : u8 {
            Dispatched,
            ReadyReported,
            ContinuationDispatched,
            RedispatchedBeforeReady,
            RedispatchedAfterReady,
            RedispatchFailed,
        };

        enum class Purpose : u8 {
            ApplyHistoryStep,
            CrashRecovery,
        };

        PendingChangingJob(ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete)
            : job(move(job))
            , on_complete(move(on_complete))
        {
        }

        ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job;
        Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete;
        Optional<ApplyHistoryStepJobs::ApplyChangingNavigableHistoryStepContinuation> continuation;
        Function<void()> on_continuation_complete;
        Phase phase { Phase::Dispatched };
        Purpose purpose { Purpose::ApplyHistoryStep };
    };
    HashMap<Web::HTML::CrossProcessId, NonnullOwnPtr<PendingChangingJob>> pending_changing_jobs;
    HashMap<Web::HTML::CrossProcessId, HistoryJobEndpoint> changing_job_endpoints;
    struct PendingNonchangingUpdate {
        Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index;
        Function<void()> on_complete;
        HistoryJobEndpoint endpoint;
    };
    HashMap<Web::HTML::CrossProcessId, PendingNonchangingUpdate> pending_nonchanging_updates;
    // Unload recovery resumes ApplyHistoryStep synchronously. Its later endpoint selection must not route a child job
    // back to the process whose unload check just died.
    Vector<HistoryJobEndpoint> unavailable_job_endpoints;
    struct DeferredCompletion {
        Web::HTML::HistoryStepResult result;
        Optional<i32> committed_step;
    };
    Optional<DeferredCompletion> deferred_completion;
    OwnPtr<ApplyHistoryStep> algorithm;
    RefPtr<Core::Promise<Empty>> queue_promise;

    bool is_browser_traversal() const { return was_initiated_by_browser; }
    bool was_initiated_by(WebContentClient const& client, u64 page_id) const
    {
        return initiating_client.ptr() == &client && initiating_page_id == page_id;
    }
};

CanonicalTraversable::~CanonicalTraversable() = default;

Optional<size_t> CanonicalTraversable::effective_current_session_history_step_index() const
{
    if (auto target = pending_browser_history_traversal_target(); target.has_value())
        return target->target_step_index;
    for (auto const& operation : m_history_operations) {
        if (!operation.value->is_browser_traversal())
            continue;
        auto const& parameters = operation.value->parameters.get<Web::TraverseToStepHistoryOperationParameters>();
        auto target = m_session_history.traversal_target_for_step(parameters.target_step);
        if (target.has_value())
            return target->target_step_index;
    }
    return m_session_history.current_used_step_index();
}

CanonicalTraversable::HistoryOperation* CanonicalTraversable::ongoing_browser_history_traversal()
{
    for (auto& operation : m_history_operations) {
        if (operation.value->is_browser_traversal())
            return operation.value.ptr();
    }
    return nullptr;
}

Optional<TraversableSessionHistory::TraversalTarget> CanonicalTraversable::browser_traversal_target_for_delta(Optional<i32> base_step, int delta) const
{
    auto base_index = m_session_history.current_used_step_index();
    if (base_step.has_value()) {
        auto base_target = m_session_history.traversal_target_for_step(*base_step);
        if (!base_target.has_value())
            return {};
        base_index = base_target->target_step_index;
    }
    if (!base_index.has_value())
        return {};

    auto target_index = static_cast<i64>(*base_index) + static_cast<i64>(delta);
    if (target_index < 0 || static_cast<u64>(target_index) >= m_session_history.used_step_count())
        return {};
    auto target_step = m_session_history.step_at(static_cast<size_t>(target_index));
    VERIFY(target_step.has_value());
    return m_session_history.traversal_target_for_step(*target_step);
}

Optional<TraversableSessionHistory::TraversalTarget> CanonicalTraversable::pending_browser_history_traversal_target() const
{
    if (!m_pending_browser_history_traversal.has_value())
        return {};

    auto base_step = m_pending_browser_history_traversal->target_step;
    auto target = base_step.has_value()
        ? m_session_history.traversal_target_for_step(*base_step)
        : Optional<TraversableSessionHistory::TraversalTarget> {};
    if (base_step.has_value() && !target.has_value())
        return {};

    // A press at a history boundary leaves the current pending target unchanged, so later presses still resolve
    // relative to the last target that could be selected.
    for (auto delta : m_pending_browser_history_traversal->deltas) {
        auto next_target = browser_traversal_target_for_delta(base_step, delta);
        if (!next_target.has_value())
            continue;
        base_step = next_target->target_step;
        target = next_target.release_value();
    }
    return target;
}

void CanonicalTraversable::queue_browser_history_traversal(Optional<i32> target_step, Optional<int> delta, CheckForCancelation check_for_cancelation, Function<void()> on_ready)
{
    if (m_pending_browser_history_traversal.has_value()) {
        VERIFY(m_pending_browser_history_traversal->stage == PendingBrowserHistoryTraversal::Stage::Queued);
        m_pending_browser_history_traversal->sequence_number = next_sequence_number();
        if (target_step.has_value()) {
            m_pending_browser_history_traversal->target_step = *target_step;
            m_pending_browser_history_traversal->deltas.clear();
        }
        if (delta.has_value())
            m_pending_browser_history_traversal->deltas.append(*delta);
        if (check_for_cancelation == CheckForCancelation::Yes)
            m_pending_browser_history_traversal->check_for_cancelation = CheckForCancelation::Yes;
        if (on_ready)
            m_pending_browser_history_traversal->on_ready_callbacks.append(move(on_ready));
        return;
    }

    m_pending_browser_history_traversal = PendingBrowserHistoryTraversal {
        .generation = m_next_pending_browser_history_traversal_generation++,
        .sequence_number = next_sequence_number(),
        .deltas = {},
        .target_step = target_step,
        .operation_id = {},
        .on_ready_callbacks = {},
        .check_for_cancelation = check_for_cancelation,
        .stage = PendingBrowserHistoryTraversal::Stage::Queued,
    };
    if (delta.has_value())
        m_pending_browser_history_traversal->deltas.append(*delta);
    if (on_ready)
        m_pending_browser_history_traversal->on_ready_callbacks.append(move(on_ready));

    auto generation = m_pending_browser_history_traversal->generation;
    m_history_traversal_queue.append_session_history_traversal_steps(
        [this, generation](NonnullRefPtr<Core::Promise<Empty>> promise) {
            start_pending_browser_history_traversal(generation, move(promise));
        });
}

void CanonicalTraversable::start_pending_browser_history_traversal(u64 generation, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    if (!m_pending_browser_history_traversal.has_value()
        || m_pending_browser_history_traversal->generation != generation) {
        promise->resolve({});
        return;
    }
    VERIFY(m_pending_browser_history_traversal->stage == PendingBrowserHistoryTraversal::Stage::Queued);

    auto target = pending_browser_history_traversal_target();

    auto view = ViewImplementation::find_view_for_traversable(*this);
    VERIFY(view.has_value());
    auto canceled_replacement_process_navigation = false;
    auto canceled_uncommitted_navigation = m_pending_browser_history_traversal->check_for_cancelation == CheckForCancelation::Yes
        && has_uncommitted_navigation();
    if (canceled_uncommitted_navigation)
        canceled_replacement_process_navigation = view->cancel_uncommitted_top_level_navigation_for_browser_traversal();

    if (!target.has_value() && canceled_replacement_process_navigation) {
        auto current_step = m_session_history.current_step();
        if (current_step.has_value())
            target = m_session_history.traversal_target_for_step(*current_step);
    }

    auto current_step = m_session_history.current_step();
    auto target_is_current_step = target.has_value()
        && current_step.has_value()
        && target->target_step == *current_step;
    if (!target.has_value()
        || (target_is_current_step && !canceled_replacement_process_navigation)) {
        auto reason = canceled_uncommitted_navigation
            ? "traverse-canceled-pending-navigation"sv
            : target_is_current_step ? "traverse-net-zero"sv
                                     : "traverse-no-entry"sv;
        view->dump_session_history(reason);
        auto callbacks = move(m_pending_browser_history_traversal->on_ready_callbacks);
        m_pending_browser_history_traversal.clear();
        for (auto& callback : callbacks)
            callback();
        if (view->on_browser_history_traversal_complete)
            view->on_browser_history_traversal_complete();
        promise->resolve({});
        return;
    }

    if (canceled_replacement_process_navigation)
        set_current_session_history_entry_identity({});
    run_pending_browser_history_traversal(target.release_value(), move(promise));
}

void CanonicalTraversable::supersede_browser_history_traversal(HistoryOperation& operation, TraversableSessionHistory::TraversalTarget target, Function<void()> on_ready)
{
    VERIFY(operation.queue_promise);
    auto promise = operation.queue_promise.release_nonnull();

    if (!m_pending_browser_history_traversal.has_value()) {
        m_pending_browser_history_traversal = PendingBrowserHistoryTraversal {
            .generation = m_next_pending_browser_history_traversal_generation++,
            .sequence_number = next_sequence_number(),
            .deltas = {},
            .target_step = target.target_step,
            .operation_id = operation.operation_id,
            .on_ready_callbacks = {},
            .check_for_cancelation = CheckForCancelation::Yes,
            .stage = PendingBrowserHistoryTraversal::Stage::Running,
        };
    } else {
        m_pending_browser_history_traversal->sequence_number = next_sequence_number();
    }
    VERIFY(m_pending_browser_history_traversal->stage == PendingBrowserHistoryTraversal::Stage::Running);
    m_pending_browser_history_traversal->target_step = target.target_step;
    m_pending_browser_history_traversal->operation_id.clear();

    auto view = ViewImplementation::find_view_for_traversable(*this);
    VERIFY(view.has_value());
    if (has_uncommitted_navigation()) {
        if (view->cancel_uncommitted_top_level_navigation_for_browser_traversal())
            set_current_session_history_entry_identity({});
    }

    finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::CanceledByNavigate, {});
    if (on_ready)
        m_pending_browser_history_traversal->on_ready_callbacks.append(move(on_ready));
    run_pending_browser_history_traversal(move(target), move(promise));
}

void CanonicalTraversable::supersede_browser_history_traversal_by_delta(HistoryOperation& operation, int delta, Function<void()> on_ready)
{
    auto const& parameters = operation.parameters.get<Web::TraverseToStepHistoryOperationParameters>();
    auto target = browser_traversal_target_for_delta(parameters.target_step, delta);
    if (!target.has_value()) {
        if (on_ready)
            on_ready();
        return;
    }
    supersede_browser_history_traversal(operation, target.release_value(), move(on_ready));
}

void CanonicalTraversable::run_pending_browser_history_traversal(TraversableSessionHistory::TraversalTarget target, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    VERIFY(m_pending_browser_history_traversal.has_value());
    m_pending_browser_history_traversal->stage = PendingBrowserHistoryTraversal::Stage::Running;
    m_pending_browser_history_traversal->deltas.clear();
    m_pending_browser_history_traversal->target_step = target.target_step;
    auto check_for_cancelation = m_pending_browser_history_traversal->check_for_cancelation;
    auto on_ready = take_pending_browser_history_traversal_on_ready();
    run_browser_history_traversal_at_queue_position(
        Web::TraverseToStepHistoryOperationParameters {
            .traversable_id = id(),
            .target_step = target.target_step,
            .user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI,
        },
        check_for_cancelation == CheckForCancelation::Yes,
        m_pending_browser_history_traversal->sequence_number,
        move(on_ready),
        nullptr,
        move(promise));
}

Function<void()> CanonicalTraversable::take_pending_browser_history_traversal_on_ready()
{
    VERIFY(m_pending_browser_history_traversal.has_value());
    auto callbacks = move(m_pending_browser_history_traversal->on_ready_callbacks);
    if (callbacks.is_empty())
        return nullptr;
    if (callbacks.size() == 1)
        return callbacks.take_first();
    return [callbacks = move(callbacks)]() mutable {
        for (auto& callback : callbacks)
            callback();
    };
}

Optional<CanonicalTraversable::BrowserHistoryTraversalDiagnostic> CanonicalTraversable::browser_history_traversal_for_testing() const
{
    for (auto const& operation : m_history_operations) {
        if (!operation.value->is_browser_traversal())
            continue;
        VERIFY(operation.value->parameters.has<Web::TraverseToStepHistoryOperationParameters>());
        auto const& parameters = operation.value->parameters.get<Web::TraverseToStepHistoryOperationParameters>();
        auto target = m_session_history.traversal_target_for_step(parameters.target_step);
        if (!target.has_value())
            return {};
        return BrowserHistoryTraversalDiagnostic {
            .target_step = parameters.target_step,
            .target_step_index = target->target_step_index,
            .changes_top_level_entry = target->changes_top_level_entry,
            .stage = operation.value->pending_unload_cancelation
                ? BrowserHistoryTraversalDiagnostic::Stage::CheckingCancelation
                : BrowserHistoryTraversalDiagnostic::Stage::ApplyingInWebContent,
        };
    }
    return {};
}

Web::HTML::SessionHistoryEntryDescriptor const* CanonicalTraversable::ongoing_browser_history_traversal_target_entry() const
{
    for (auto const& operation : m_history_operations) {
        if (!operation.value->is_browser_traversal())
            continue;

        auto const& parameters = operation.value->parameters.get<Web::TraverseToStepHistoryOperationParameters>();
        if (auto target = m_session_history.traversal_target_for_step(parameters.target_step); target.has_value())
            return target->target_top_level_entry;
    }
    return nullptr;
}

void CanonicalTraversable::recover_from_web_content_process_crash(Optional<HistoryJobEndpoint> crashed_endpoint, OnHistoryOperationComplete on_complete)
{
    for (auto& operation : m_history_operations) {
        if (!operation.value->is_browser_traversal())
            continue;

        if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value())
            view->did_resume_history_traversal(operation.value->operation_id);

        auto replacement_endpoint = history_job_endpoint_for(*this);
        VERIFY(replacement_endpoint.client);

        auto endpoint_crashed = [&](HistoryJobEndpoint const& endpoint) {
            return crashed_endpoint.has_value()
                && endpoint.client.ptr() == crashed_endpoint->client.ptr()
                && endpoint.page_id == crashed_endpoint->page_id;
        };
        auto initiating_endpoint_crashed = crashed_endpoint.has_value()
            && operation.value->initiating_client.ptr() == crashed_endpoint->client.ptr()
            && operation.value->initiating_page_id == crashed_endpoint->page_id;

        if (crashed_endpoint.has_value())
            operation.value->unavailable_job_endpoints.append(*crashed_endpoint);

        if (initiating_endpoint_crashed) {
            operation.value->initiating_client = replacement_endpoint.client;
            operation.value->initiating_page_id = replacement_endpoint.page_id;
        }
        auto replaced_completion_endpoint = operation.value->completion_endpoints.remove_all_matching([&](auto const& endpoint) {
            return crashed_endpoint.has_value()
                && endpoint.client.ptr() == crashed_endpoint->client.ptr()
                && endpoint.page_id == crashed_endpoint->page_id;
        });
        if (replaced_completion_endpoint)
            add_history_operation_completion_endpoint(*operation.value, replacement_endpoint);

        if (initiating_endpoint_crashed) {
            if (auto pending = move(operation.value->pending_unload_cancelation)) {
                set_current_session_history_entry_identity({});
                pending(Web::HTML::HistoryStepResult::Applied);
                return;
            }
        }

        Vector<Web::HTML::CrossProcessId> crashed_changing_jobs;
        for (auto const& endpoint : operation.value->changing_job_endpoints) {
            if (endpoint_crashed(endpoint.value))
                crashed_changing_jobs.append(endpoint.key);
        }
        Vector<Web::HTML::CrossProcessId> crashed_nonchanging_updates;
        for (auto const& update : operation.value->pending_nonchanging_updates) {
            if (endpoint_crashed(update.value.endpoint))
                crashed_nonchanging_updates.append(update.key);
        }

        if (crashed_changing_jobs.contains_slow(id())) {
            // Repopulating the retained top-level job reconstructs its descendants. Complete their dead-endpoint jobs
            // exactly once, then resume the top-level job from the phase it had reached.
            crashed_changing_jobs.remove_first_matching([&](auto navigable_id) { return navigable_id == id(); });
            auto pending_job = operation.value->pending_changing_jobs.get(id());
            VERIFY(pending_job.has_value());
            switch (pending_job.value()->phase) {
            case HistoryOperation::PendingChangingJob::Phase::Dispatched:
            case HistoryOperation::PendingChangingJob::Phase::RedispatchedBeforeReady:
                pending_job.value()->phase = HistoryOperation::PendingChangingJob::Phase::RedispatchedBeforeReady;
                break;
            case HistoryOperation::PendingChangingJob::Phase::ReadyReported:
            case HistoryOperation::PendingChangingJob::Phase::ContinuationDispatched:
            case HistoryOperation::PendingChangingJob::Phase::RedispatchedAfterReady:
                pending_job.value()->phase = HistoryOperation::PendingChangingJob::Phase::RedispatchedAfterReady;
                break;
            case HistoryOperation::PendingChangingJob::Phase::RedispatchFailed:
                VERIFY_NOT_REACHED();
            }
            operation.value->changing_job_endpoints.set(id(), replacement_endpoint);
            complete_history_jobs_after_crash(*operation.value, move(crashed_changing_jobs), move(crashed_nonchanging_updates));
            dispatch_changing_navigable_history_step_job(*operation.value, id());
            return;
        }

        if (crashed_nonchanging_updates.contains_slow(id())) {
            // Scalars cannot reconstruct a fresh process. Substitute an entry-addressed changing job for this update,
            // and use its continuation to complete the original nonchanging job.
            crashed_nonchanging_updates.remove_first_matching([&](auto navigable_id) { return navigable_id == id(); });
            auto pending_update = operation.value->pending_nonchanging_updates.take(id());
            VERIFY(pending_update.has_value());
            complete_history_jobs_after_crash(*operation.value, move(crashed_changing_jobs), move(crashed_nonchanging_updates));
            dispatch_crash_recovery_changing_job(*operation.value, replacement_endpoint,
                pending_update->history_object_length_and_index, move(pending_update->on_complete));
            return;
        }

        if (!crashed_changing_jobs.is_empty() || !crashed_nonchanging_updates.is_empty()) {
            auto const& parameters = operation.value->parameters.get<Web::TraverseToStepHistoryOperationParameters>();
            auto history_object_length_and_index = m_session_history.get_the_history_object_length_and_index(parameters.target_step);
            if (!history_object_length_and_index.has_value()) {
                complete_history_jobs_after_crash(*operation.value, move(crashed_changing_jobs), move(crashed_nonchanging_updates));
                return;
            }

            dispatch_crash_recovery_changing_job(*operation.value, replacement_endpoint,
                *history_object_length_and_index,
                [this, operation_id = operation.value->operation_id,
                    changing_jobs = move(crashed_changing_jobs),
                    nonchanging_updates = move(crashed_nonchanging_updates)]() mutable {
                    auto* operation = find_history_operation(operation_id);
                    if (operation)
                        complete_history_jobs_after_crash(*operation, move(changing_jobs), move(nonchanging_updates));
                });
            return;
        }

        if (auto pending = move(operation.value->pending_unload_cancelation)) {
            set_current_session_history_entry_identity({});
            pending(Web::HTML::HistoryStepResult::Applied);
            return;
        }

        if (crashed_endpoint.has_value()) {
            auto const& parameters = operation.value->parameters.get<Web::TraverseToStepHistoryOperationParameters>();
            auto history_object_length_and_index = m_session_history.get_the_history_object_length_and_index(parameters.target_step);
            if (!history_object_length_and_index.has_value()) {
                finish_history_operation(operation.value->operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
                return;
            }
            dispatch_crash_recovery_changing_job(*operation.value, replacement_endpoint, *history_object_length_and_index, nullptr);
            return;
        }

        operation.value->pending_changing_jobs.clear();
        operation.value->changing_job_endpoints.clear();
        operation.value->pending_nonchanging_updates.clear();
        operation.value->algorithm = nullptr;
        operation.value->check_for_cancelation = false;
        set_current_session_history_entry_identity({});
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
    set_current_session_history_entry_identity({});
    enqueue_browser_history_traversal(
        Web::TraverseToStepHistoryOperationParameters {
            .traversable_id = id(),
            .target_step = *current_step,
            .user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI,
        },
        false,
        move(on_complete));
}

CanonicalTraversable::HistoryJobEndpoint CanonicalTraversable::history_job_endpoint_for(CanonicalNavigable const& navigable) const
{
    // A remote child is reported by the process containing its frame, but its active document lives in the
    // embedded page. Apply and commit work must follow the active document.
    if (navigable.has_remote_host())
        return { &navigable.remote_host_client(), navigable.remote_host_page_id() };

    if (auto* client = navigable.reporting_client_if_any())
        return { client, navigable.reporting_page_id() };

    // NB: The traversable is the view's root navigable; the process hosting its documents is the view's client
    //     rather than a reporting client recorded in the tree.
    if (&navigable == this) {
        if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value() && view->m_client_state.client)
            return { view->m_client_state.client, view->m_client_state.page_index };
    }
    return {};
}

CanonicalTraversable::HistoryOperation* CanonicalTraversable::find_history_operation(Web::HTML::CrossProcessId operation_id)
{
    auto operation = m_history_operations.find(operation_id);
    if (operation == m_history_operations.end())
        return nullptr;
    return operation->value.ptr();
}

bool CanonicalTraversable::navigation_transaction_matches(HistoryOperation const& operation, WebContentClient const& client, u64 page_id, Optional<Web::HTML::CrossProcessId> reply_navigable_id) const
{
    if (!operation.parameters.has<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>())
        return true;

    auto const& parameters = operation.parameters.get<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>();
    if (reply_navigable_id.has_value() && *reply_navigable_id != parameters.navigable_id)
        return true;
    auto navigable = find(parameters.navigable_id);
    if (!navigable.has_value())
        return false;

    // A javascript: navigation clears the spec's ongoing-navigation value before finalization and therefore has no
    // UI-owned population transaction. It must still come from the process hosting the active document.
    if (!parameters.navigation_id.has_value()) {
        auto endpoint = history_job_endpoint_for(*navigable);
        return endpoint.client.ptr() == &client && endpoint.page_id == page_id;
    }

    return navigable->navigation_transaction_matches(*parameters.navigation_id, client, page_id);
}

void CanonicalTraversable::add_history_operation_completion_endpoint(HistoryOperation& operation, HistoryJobEndpoint endpoint)
{
    VERIFY(endpoint.client);
    for (auto const& existing : operation.completion_endpoints) {
        if (existing.client.ptr() == endpoint.client.ptr() && existing.page_id == endpoint.page_id)
            return;
    }
    operation.completion_endpoints.append(move(endpoint));
}

bool CanonicalTraversable::select_changing_navigable_history_step_job_endpoint(HistoryOperation& operation, ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob& job)
{
    auto navigable = find(job.navigable_id);
    if (!navigable.has_value())
        return false;

    if (job.navigation_type == Web::Bindings::NavigationType::Traverse) {
        if (auto& ongoing_navigation = navigable->ongoing_navigation(); ongoing_navigation.has_value()) {
            auto traversal_crosses_documents = !navigable->active_document_is(job.target_entry)
                || job.target_entry.document_state.reload_pending;
            if (ongoing_navigation->sequence_number > operation.sequence_number
                && !traversal_crosses_documents) {
                // A newer navigation takes precedence over a same-document traversal. Cross-document traversals
                // set the ongoing navigation to "traversal" before their changing jobs run, so newer navigations
                // are blocked instead of superseding them.
                job.superseded_by_newer_navigation = true;
            } else {
                // Supersede the older navigation, or reject a navigation admitted after a cross-document traversal,
                // so its late callbacks cannot replace the traversal's target document.
                navigable->clear_ongoing_navigation();
            }
        }
    }

    if (navigable->is_top_level_traversable() && job.navigation_type == Web::Bindings::NavigationType::Traverse) {
        auto view = ViewImplementation::find_view_for_traversable(*this);
        if (!view.has_value())
            return false;

        auto source_url = view->top_level_process_site_url().value_or(URL::about_blank());
        if (SiteIsolationManager::the().navigation_requires_process_swap(source_url, job.target_entry.url))
            view->replace_web_content_process_for_history_traversal(job.target_entry.document_state.id, job.target_entry.url);
    }

    auto endpoint = history_job_endpoint_for(*navigable);
    if (!endpoint.client)
        return false;
    for (auto const& unavailable_endpoint : operation.unavailable_job_endpoints) {
        if (endpoint.client.ptr() == unavailable_endpoint.client.ptr() && endpoint.page_id == unavailable_endpoint.page_id)
            return false;
    }

    if (navigable->is_top_level_traversable()) {
        auto callback = move(operation.on_browser_traversal_ready);
        if (callback)
            callback();
    }

    VERIFY(!operation.changing_job_endpoints.contains(job.navigable_id));
    operation.changing_job_endpoints.set(job.navigable_id, endpoint);
    add_history_operation_completion_endpoint(operation, endpoint);

    // A reload's top-level job repopulates the view's document; begin the recorded load where the job is
    // dispatched instead of having the job echo it back.
    if (navigable->is_top_level_traversable()
        && operation.parameters.has<Web::ReloadHistoryOperationParameters>()) {
        if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value())
            endpoint.client->begin_top_level_load(*view, endpoint.page_id, {}, job.target_entry.url);
    }
    return true;
}

void CanonicalTraversable::dispatch_changing_navigable_history_step_job(HistoryOperation& operation, Web::HTML::CrossProcessId navigable_id)
{
    auto endpoint = operation.changing_job_endpoints.get(navigable_id);
    VERIFY(endpoint.has_value());
    auto pending_job = operation.pending_changing_jobs.get(navigable_id);
    VERIFY(pending_job.has_value());

    auto target_entry = pending_job.value()->job.target_entry;
    endpoint->client->async_run_changing_navigable_history_job(
        endpoint->page_id, operation.operation_id, navigable_id,
        move(target_entry), pending_job.value()->job.user_involvement,
        pending_job.value()->job.navigation_type,
        pending_job.value()->job.navigation_api_abort_behavior,
        pending_job.value()->job.superseded_by_newer_navigation);
}

void CanonicalTraversable::dispatch_changing_navigable_history_step_continuation(HistoryOperation& operation, Web::HTML::CrossProcessId navigable_id)
{
    auto endpoint = operation.changing_job_endpoints.get(navigable_id);
    VERIFY(endpoint.has_value());
    auto pending_job = operation.pending_changing_jobs.get(navigable_id);
    VERIFY(pending_job.has_value());
    VERIFY(pending_job.value()->continuation.has_value());
    VERIFY(pending_job.value()->phase == HistoryOperation::PendingChangingJob::Phase::ReadyReported);
    pending_job.value()->phase = HistoryOperation::PendingChangingJob::Phase::ContinuationDispatched;

    auto continuation = *pending_job.value()->continuation;
    endpoint->client->async_apply_changing_navigable_continuation(
        endpoint->page_id, operation.operation_id, navigable_id,
        continuation.history_object_length_and_index.script_history_length,
        continuation.history_object_length_and_index.script_history_index,
        move(continuation.entries_for_navigation_api),
        system_visibility_state());
}

void CanonicalTraversable::dispatch_crash_recovery_changing_job(HistoryOperation& operation, HistoryJobEndpoint endpoint, Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index, Function<void()> on_complete)
{
    auto const& parameters = operation.parameters.get<Web::TraverseToStepHistoryOperationParameters>();
    auto const* target_entry = m_session_history.get_the_target_history_entry(*this, parameters.target_step);
    auto entries_for_navigation_api = m_session_history.get_session_history_entries_for_the_navigation_api(*this, parameters.target_step);
    VERIFY(target_entry);
    VERIFY(entries_for_navigation_api.has_value());

    set_current_session_history_entry(*target_entry);
    operation.changing_job_endpoints.set(id(), endpoint);
    add_history_operation_completion_endpoint(operation, endpoint);
    auto pending_job = make<HistoryOperation::PendingChangingJob>(
        ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob {
            .navigable_id = id(),
            .target_entry = *target_entry,
            .user_involvement = parameters.user_involvement,
            .navigation_type = Web::Bindings::NavigationType::Traverse,
            .navigation_api_abort_behavior = Web::HTML::LocalNavigable::NavigationAPIAbortBehavior::Abort,
        },
        [this, operation_id = operation.operation_id, navigable_id = id()](Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition) {
            if (disposition != Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready)
                return;
            auto* operation = find_history_operation(operation_id);
            if (operation)
                dispatch_changing_navigable_history_step_continuation(*operation, navigable_id);
        });
    pending_job->continuation = ApplyHistoryStepJobs::ApplyChangingNavigableHistoryStepContinuation {
        .navigable_id = id(),
        .history_object_length_and_index = history_object_length_and_index,
        .entries_for_navigation_api = entries_for_navigation_api.release_value(),
    };
    pending_job->on_continuation_complete = move(on_complete);
    pending_job->purpose = HistoryOperation::PendingChangingJob::Purpose::CrashRecovery;
    operation.pending_changing_jobs.set(id(), move(pending_job));
    dispatch_changing_navigable_history_step_job(operation, id());
}

void CanonicalTraversable::finish_deferred_history_operation_after_crash_recovery(Web::HTML::CrossProcessId operation_id)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation || !operation->deferred_completion.has_value())
        return;
    auto completion = operation->deferred_completion.release_value();
    finish_history_operation(operation_id, completion.result, completion.committed_step);
}

void CanonicalTraversable::complete_history_jobs_after_crash(HistoryOperation& operation, Vector<Web::HTML::CrossProcessId> changing_jobs, Vector<Web::HTML::CrossProcessId> nonchanging_updates)
{
    Vector<Function<void()>> completions;
    for (auto navigable_id : changing_jobs) {
        auto pending_job = operation.pending_changing_jobs.take(navigable_id);
        operation.changing_job_endpoints.remove(navigable_id);
        if (!pending_job.has_value())
            continue;

        switch (pending_job.value()->phase) {
        case HistoryOperation::PendingChangingJob::Phase::Dispatched:
        case HistoryOperation::PendingChangingJob::Phase::RedispatchedBeforeReady:
            completions.append([on_complete = move(pending_job.value()->on_complete)]() mutable {
                on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);
            });
            break;
        case HistoryOperation::PendingChangingJob::Phase::ReadyReported:
        case HistoryOperation::PendingChangingJob::Phase::RedispatchedAfterReady:
            // ApplyHistoryStep already retained this navigable in its continuation queue. If it has not supplied the
            // continuation yet, removing the retained job makes that future application complete locally.
            if (pending_job.value()->continuation.has_value())
                completions.append(move(pending_job.value()->on_continuation_complete));
            break;
        case HistoryOperation::PendingChangingJob::Phase::ContinuationDispatched:
        case HistoryOperation::PendingChangingJob::Phase::RedispatchFailed:
            completions.append(move(pending_job.value()->on_continuation_complete));
            break;
        }
    }
    for (auto navigable_id : nonchanging_updates) {
        if (auto pending_update = operation.pending_nonchanging_updates.take(navigable_id); pending_update.has_value())
            completions.append(move(pending_update->on_complete));
    }
    for (auto& completion : completions) {
        if (completion)
            completion();
    }
}

ApplyHistoryStepJobs CanonicalTraversable::create_apply_history_step_jobs(Web::HTML::CrossProcessId operation_id)
{
    return {
        .run_unload_cancelation_job = [this, operation_id](ApplyHistoryStepJobs::UnloadCancelationJob job, Function<void(Web::HTML::HistoryStepResult)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            VERIFY(operation->initiating_client);
            operation->pending_unload_cancelation = move(on_complete);
            operation->initiating_client->async_run_history_step_unload_cancelation_job(operation->initiating_page_id, operation_id, move(job.target_entry), move(job.navigables_crossing_documents), job.user_involvement); },
        .select_changing_navigable_history_step_job_endpoint = [this, operation_id](ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob& job) {
            auto* operation = find_history_operation(operation_id);
            return operation && select_changing_navigable_history_step_job_endpoint(*operation, job); },
        .run_changing_navigable_history_step_job = [this, operation_id](ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable_id = job.navigable_id;
            operation->pending_changing_jobs.set(navigable_id, make<HistoryOperation::PendingChangingJob>(move(job), move(on_complete)));
            dispatch_changing_navigable_history_step_job(*operation, navigable_id); },
        .apply_changing_navigable_history_step_continuation = [this, operation_id](ApplyHistoryStepJobs::ApplyChangingNavigableHistoryStepContinuation continuation, Function<void()> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable_id = continuation.navigable_id;
            auto pending_job = operation->pending_changing_jobs.get(navigable_id);
            if (!pending_job.has_value()) {
                on_complete();
                return;
            }
            pending_job.value()->continuation = move(continuation);
            pending_job.value()->on_continuation_complete = move(on_complete);
            if (pending_job.value()->phase == HistoryOperation::PendingChangingJob::Phase::RedispatchFailed) {
                auto taken_job = operation->pending_changing_jobs.take(navigable_id);
                operation->changing_job_endpoints.remove(navigable_id);
                taken_job.value()->on_continuation_complete();
                return;
            }
            if (pending_job.value()->phase == HistoryOperation::PendingChangingJob::Phase::ReadyReported)
                dispatch_changing_navigable_history_step_continuation(*operation, navigable_id); },
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
            for (auto const& unavailable_endpoint : operation->unavailable_job_endpoints) {
                if (endpoint.client.ptr() == unavailable_endpoint.client.ptr()
                    && endpoint.page_id == unavailable_endpoint.page_id) {
                    on_complete();
                    return;
                }
            }
            add_history_operation_completion_endpoint(*operation, endpoint);
            operation->pending_nonchanging_updates.set(navigable_id,
                HistoryOperation::PendingNonchangingUpdate {
                    history_object_length_and_index,
                    move(on_complete),
                    endpoint,
                });
            endpoint.client->async_update_nonchanging_navigable_history_state(endpoint.page_id, operation_id, navigable_id,
                history_object_length_and_index.script_history_length, history_object_length_and_index.script_history_index); },
    };
}

void CanonicalTraversable::run_history_operation_at_queue_position(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters request, WebContentClient* requesting_client, u64 requesting_page_id, u64 sequence_number, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    // Operation ids are namespaced per initiating process, so a requested id that is already live can only come
    // from a misbehaving process. Drop the request rather than let it alias the existing operation.
    if (m_history_operations.contains(operation_id)) {
        dbgln("Refusing history operation with duplicate id {}", operation_id);
        if (discard_pending_same_document_session_history_entries_for_operation(operation_id, request))
            session_history_changed();
        promise->resolve({});
        return;
    }
    m_history_operations.set(operation_id, make<HistoryOperation>(operation_id, move(request), requesting_client, requesting_page_id, sequence_number, move(on_complete)));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    start_history_operation(*operation, promise);
}

void CanonicalTraversable::run_browser_history_traversal_at_queue_position(Web::TraverseToStepHistoryOperationParameters parameters, bool check_for_cancelation, u64 sequence_number, Function<void()> on_ready, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = Application::the().allocate_ui_process_cross_process_id();
    auto owned_operation = make<HistoryOperation>(operation_id, Web::HistoryOperationParameters { move(parameters) }, nullptr, 0, sequence_number, move(on_complete));
    owned_operation->was_initiated_by_browser = true;
    owned_operation->check_for_cancelation = check_for_cancelation;
    owned_operation->on_browser_traversal_ready = move(on_ready);
    m_history_operations.set(operation_id, move(owned_operation));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    if (m_pending_browser_history_traversal.has_value()
        && m_pending_browser_history_traversal->stage == PendingBrowserHistoryTraversal::Stage::Running
        && m_pending_browser_history_traversal->target_step == operation->parameters.get<Web::TraverseToStepHistoryOperationParameters>().target_step
        && !m_pending_browser_history_traversal->operation_id.has_value()) {
        m_pending_browser_history_traversal->operation_id = operation_id;
    }
    auto view = ViewImplementation::find_view_for_traversable(*this);
    VERIFY(view.has_value());
    view->will_apply_history_traversal_step(operation_id);
    start_history_operation(*operation, promise);
}

void CanonicalTraversable::append_history_queue_steps(SessionHistoryTraversalSteps steps)
{
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::enqueue_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters request, WebContentClient& requesting_client, u64 requesting_page_id, u64 sequence_number, OnHistoryOperationComplete on_complete)
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

        // AD-HOC: The canonical tree stages same-document entries when WebContent admits their finalization request.
        // This makes the entry addressable during the interval before the spec's queued finalization steps run.
        auto target_navigable = requesting_client.hosted_navigable_for_page(requesting_page_id, parameters.navigable_id);
        if (target_navigable.has_value() && &target_navigable->top_level_traversable() == this) {
            if (parameters.previous_entry_persisted_state.has_value())
                update_session_history_entry_persisted_state(*target_navigable, *parameters.previous_entry_persisted_state);
            target_navigable->stage_same_document_session_history_entry(operation_id, parameters.target_entry);
            session_history_changed();
        }
    }

    NonnullRefPtr<WebContentClient> requesting_client_ref { requesting_client };
    auto steps = [this, operation_id, request = move(request), requesting_client = move(requesting_client_ref), requesting_page_id, sequence_number, on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_history_operation_at_queue_position(operation_id, move(request), requesting_client.ptr(), requesting_page_id, sequence_number, move(on_complete), move(promise));
    };

    if (synchronous_navigation_target.has_value())
        m_history_traversal_queue.append_session_history_synchronous_navigation_steps(*synchronous_navigation_target, move(steps));
    else
        m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::enqueue_browser_history_traversal(Web::TraverseToStepHistoryOperationParameters parameters, bool check_for_cancelation, OnHistoryOperationComplete on_complete)
{
    auto sequence_number = next_sequence_number();
    auto steps = [this, parameters = move(parameters), check_for_cancelation, sequence_number, on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_browser_history_traversal_at_queue_position(move(parameters), check_for_cancelation, sequence_number, nullptr, move(on_complete), move(promise));
    };
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::apply_history_step(HistoryOperation& operation, i32 step, bool check_for_cancelation, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement user_involvement, Optional<Web::Bindings::NavigationType> navigation_type, Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot)
{
    VERIFY(!operation.algorithm);
    auto operation_id = operation.operation_id;
    operation.algorithm = make<ApplyHistoryStep>(
        m_session_history, *this, m_history_traversal_queue, m_apply_history_step_traversable_state, create_apply_history_step_jobs(operation_id),
        step, check_for_cancelation, initiator_to_check, initiator_source_snapshot, user_involvement, navigation_type,
        [this, operation_id](Web::HTML::HistoryStepResult result) {
            auto* operation = find_history_operation(operation_id);
            auto committed_step = operation && operation->algorithm ? operation->algorithm->committed_step() : Optional<i32> {};
            if (operation) {
                auto recovery_job = operation->pending_changing_jobs.get(id());
                if (recovery_job.has_value()
                    && recovery_job.value()->purpose == HistoryOperation::PendingChangingJob::Purpose::CrashRecovery) {
                    operation->deferred_completion = HistoryOperation::DeferredCompletion {
                        .result = result,
                        .committed_step = committed_step,
                    };
                    return;
                }
            }
            finish_history_operation(operation_id, result, committed_step);
        });
    operation.algorithm->apply_the_history_step();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-push/replace-history-step
void CanonicalTraversable::apply_the_push_or_replace_history_step(HistoryOperation& operation, i32 step, Web::HTML::HistoryHandlingBehavior history_handling, Web::HTML::UserNavigationInvolvement user_involvement)
{
    auto navigation_type = history_handling == Web::HTML::HistoryHandlingBehavior::Push
        ? Web::Bindings::NavigationType::Push
        : Web::Bindings::NavigationType::Replace;

    // 1. Return the result of applying the history step step to traversable given false, null, null, userInvolvement, and historyHandling.
    apply_history_step(operation, step, false, {}, user_involvement, navigation_type);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-reload-history-step
void CanonicalTraversable::apply_the_reload_history_step(HistoryOperation& operation, Web::HTML::UserNavigationInvolvement user_involvement)
{
    // 1. Let step be traversable's current session history step.
    auto step = m_session_history.current_step();
    if (!step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 2. Return the result of applying the history step step to traversable given true, null, null, userInvolvement, and "reload".
    apply_history_step(operation, *step, true, {}, user_involvement, Web::Bindings::NavigationType::Reload);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-traverse-history-step
void CanonicalTraversable::apply_the_traverse_history_step(HistoryOperation& operation, i32 step, Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement user_involvement)
{
    // 1. Return the result of applying the history step step to traversable given true, sourceSnapshotParams, initiatorToCheck, userInvolvement, and "traverse".
    apply_history_step(operation, step, true, initiator_to_check, user_involvement, Web::Bindings::NavigationType::Traverse, move(initiator_source_snapshot));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#resume-applying-the-traverse-history-step
void CanonicalTraversable::resume_applying_the_traverse_history_step(HistoryOperation& operation, i32 step, Web::HTML::UserNavigationInvolvement user_involvement)
{
    // Apply step to traversable given false, null, null, userInvolvement, and "traverse".
    apply_history_step(operation, step, false, {}, user_involvement, Web::Bindings::NavigationType::Traverse);
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
    apply_history_step(operation, *step, false, {}, Web::HTML::UserNavigationInvolvement::None, {});
}

// Direct operations have their complete input in canonical state, so they enter apply-the-history-step at their
// queue position without a WebContent preparation round trip.
static bool history_operation_is_direct(Web::HistoryOperationParameters const& parameters)
{
    return parameters.has<Web::ReloadHistoryOperationParameters>()
        || parameters.has<Web::TraverseByDeltaHistoryOperationParameters>()
        || parameters.has<Web::TraverseToStepHistoryOperationParameters>()
        || parameters.has<Web::NavigationAPITraverseHistoryOperationParameters>()
        || parameters.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>()
        || parameters.has<Web::NavigableDestructionHistoryOperationParameters>()
        || parameters.has<Web::CloseTopLevelTraversableHistoryOperationParameters>()
        || parameters.has<Web::FlushSessionHistoryTraversalQueueOperationParameters>();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void CanonicalTraversable::traverse_the_history_by_a_delta_at_queue_position(HistoryOperation& operation, Web::TraverseByDeltaHistoryOperationParameters const& request)
{
    // Steps 1-3 of traverse the history by a delta were performed by the source process before it appended these
    // session history traversal steps.

    // 1. Let allSteps be the result of getting all used history steps for traversable.
    auto all_steps = m_session_history.used_steps();

    // 2. Let currentStepIndex be the index of traversable's current session history step within allSteps.
    auto current_step = m_session_history.current_step();
    if (!current_step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
        return;
    }
    auto current_step_index = all_steps.find_first_index(*current_step);
    VERIFY(current_step_index.has_value());

    // 3. Let targetStepIndex be currentStepIndex plus delta.
    auto target_step_index = static_cast<i64>(*current_step_index) + static_cast<i64>(request.delta);

    // 4. If allSteps[targetStepIndex] does not exist, then abort these steps.
    if (target_step_index < 0 || static_cast<size_t>(target_step_index) >= all_steps.size()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
        return;
    }

    // 5. Apply the traverse history step allSteps[targetStepIndex] to traversable, given sourceSnapshotParams,
    //    initiatorToCheck, and userInvolvement.
    apply_the_traverse_history_step(operation, all_steps[static_cast<size_t>(target_step_index)], request.initiator_source_snapshot, request.initiator_to_check, request.user_involvement);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#performing-a-navigation-api-traversal
void CanonicalTraversable::perform_a_navigation_api_traversal_at_queue_position(HistoryOperation& operation, Web::NavigationAPITraverseHistoryOperationParameters const& request)
{
    auto navigable = find(request.navigable_id);
    if (!navigable.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 12.1. Let navigableSHEs be the result of getting session history entries given navigable.
    auto navigable_session_history_entries = m_session_history.get_session_history_entries(*navigable);
    if (!navigable_session_history_entries.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 12.2. Let targetSHE be the session history entry in navigableSHEs whose navigation API key is key. If no
    //       such entry exists, queue rejection of the finished promise and abort these steps.
    auto target_entry = navigable_session_history_entries->find_if([&](auto const& entry) {
        return entry.navigation_api_key == request.key;
    });
    if (target_entry == navigable_session_history_entries->end()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 12.3. If targetSHE is navigable's active session history entry, queue rejection of the finished promise and
    //       abort these steps.
    if (navigable->active_session_history_entry_identity() == Web::HTML::session_history_entry_identity(*target_entry)) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 12.4. Let result be the result of applying the traverse history step targetSHE's step to traversable, given
    //       sourceSnapshotParams, navigable, and "none".
    apply_the_traverse_history_step(operation, target_entry->step, request.initiator_source_snapshot, request.navigable_id, Web::HTML::UserNavigationInvolvement::None);

    // Steps 12.5-12.6 are handled in Navigation's relevant realm when the operation completes.
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation
void CanonicalTraversable::finalize_a_same_document_navigation(HistoryOperation& operation, Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& request)
{
    VERIFY(request.entry_to_replace.has_value() == (request.history_handling == Web::HTML::HistoryHandlingBehavior::Replace));
    auto target_navigable = find(request.navigable_id);
    if (!target_navigable.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 1. Assert: this is running on traversable's session history traversal queue.
    VERIFY(operation.queue_promise);
    VERIFY(!operation.queue_promise->is_resolved() && !operation.queue_promise->is_rejected());
    VERIFY(&target_navigable->top_level_traversable() == this);

    // 2. If targetNavigable's active session history entry is not targetEntry, then return.
    // AD-HOC: WebContent performs this object-identity check synchronously before enqueueing. Repeating
    // it against the later canonical active entry would incorrectly discard back-to-back synchronous pushState()
    // calls; Firefox and Chromium preserve both entries.

    // AD-HOC: Admission staged targetEntry so entry-addressed updates made before this queue position are retained.
    auto target_entry = target_navigable->take_pending_same_document_session_history_entry(
        operation.operation_id, Web::HTML::session_history_entry_identity(request.target_entry));
    if (!target_entry.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 3. Let targetStep be null.
    Optional<i32> target_step;

    // 4. Let targetEntries be the result of getting session history entries for targetNavigable.
    auto target_entries = m_session_history.get_session_history_entries(*target_navigable);
    if (!target_entries.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // NB: WebContent sends only the same-document fields. Reuse the canonical document state, which is shared
    // by all same-document entries and owns the serialized nested histories.
    auto target_document_state = target_entries->find_if([&](auto const& entry) {
        return entry.document_state.id == target_entry->document_state_id;
    });
    if (target_document_state == target_entries->end()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    auto canonical_target_entry = TraversableSessionHistory::Entry {
        .step = 0,
        .url = target_entry->url,
        .document_state = target_document_state->document_state,
        .classic_history_api_state = target_entry->classic_history_api_state,
        .navigation_api_state = target_entry->navigation_api_state,
        .navigation_api_key = target_entry->navigation_api_key,
        .navigation_api_id = target_entry->navigation_api_id,
        .scroll_restoration_mode = target_entry->scroll_restoration_mode,
        .scroll_position_data = target_entry->scroll_position_data,
    };

    auto current_step = m_session_history.current_step();
    if (!current_step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // AD-HOC: Apply steps 5.1-5.4 to a copy so a stale nested navigable cannot leave canonical history partially
    // mutated if clearing the forward history removes its serialized targetEntries.
    auto updated_session_history = m_session_history;

    // 5. If entryToReplace is null:
    if (!request.entry_to_replace.has_value()) {
        // 5.1. Clear the forward session history of traversable.
        if (!updated_session_history.clear_the_forward_session_history()) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }

        // 5.2. Set targetStep to traversable's current session history step + 1.
        VERIFY(*current_step < NumericLimits<i32>::max());
        target_step = *current_step + 1;

        // 5.3. Set targetEntry's step to targetStep.
        canonical_target_entry.step = *target_step;

        // 5.4. Append targetEntry to targetEntries.
        if (!updated_session_history.append_or_replace_session_history_entry(*target_navigable, canonical_target_entry, {})) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }
    }
    // Otherwise:
    else {
        auto entry_to_replace = target_entries->find_if([&](auto const& entry) {
            return Web::HTML::session_history_entry_identity(entry) == *request.entry_to_replace;
        });
        if (entry_to_replace == target_entries->end()) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }

        // 5.1. Replace entryToReplace with targetEntry in targetEntries.
        // 5.2. Set targetEntry's step to entryToReplace's step.
        canonical_target_entry.step = entry_to_replace->step;
        if (!updated_session_history.append_or_replace_session_history_entry(*target_navigable, canonical_target_entry, request.entry_to_replace)) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }

        // 5.3. Set targetStep to traversable's current session history step.
        target_step = *current_step;
    }

    m_session_history = move(updated_session_history);
    target_navigable->set_active_session_history_entry_identity(Web::HTML::session_history_entry_identity(*target_entry));

    // 6. Apply the push/replace history step targetStep to traversable given historyHandling and userInvolvement.
    apply_the_push_or_replace_history_step(operation, *target_step, request.history_handling, request.user_involvement);
}

void CanonicalTraversable::run_direct_history_operation(HistoryOperation& operation)
{
    operation.parameters.visit(
        [&](Web::ReloadHistoryOperationParameters const& parameters) {
            auto current_step = m_session_history.current_step();
            auto navigable = find(parameters.navigable_id);
            auto const* target_entry = current_step.has_value() && navigable.has_value()
                ? m_session_history.get_the_target_history_entry(*navigable, *current_step)
                : nullptr;
            if (!target_entry
                || !set_session_history_entry_document_state_reload_pending(
                    *navigable, target_entry->navigation_api_key, true)) {
                finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
                return;
            }
            apply_the_reload_history_step(operation, parameters.user_involvement);
        },
        [&](Web::TraverseByDeltaHistoryOperationParameters const& request) {
            traverse_the_history_by_a_delta_at_queue_position(operation, request);
        },
        [&](Web::TraverseToStepHistoryOperationParameters const& parameters) {
            apply_the_traverse_history_step(operation, parameters.target_step, {}, {}, parameters.user_involvement);
        },
        [&](Web::NavigationAPITraverseHistoryOperationParameters const& request) {
            perform_a_navigation_api_traversal_at_queue_position(operation, request);
        },
        [&](Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& request) {
            finalize_a_same_document_navigation(operation, request);
        },
        [&](Web::NavigableDestructionHistoryOperationParameters const&) {
            update_for_navigable_creation_or_destruction(operation);
        },
        [&](Web::CloseTopLevelTraversableHistoryOperationParameters const&) {
            // Close applies no history step. Completing at the queue position is the one-way command that runs
            // the unload and destruction steps in the requesting process.
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
        },
        [&](Web::FlushSessionHistoryTraversalQueueOperationParameters const&) {
            // Flush is a queue barrier; completing at the queue position is the whole operation.
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
        },
        [&](auto const&) {
            VERIFY_NOT_REACHED();
        });
}

void CanonicalTraversable::start_history_operation(HistoryOperation& operation, NonnullRefPtr<Core::Promise<Empty>>)
{
    if (!operation.initiating_client) {
        auto endpoint = history_job_endpoint_for(*this);
        operation.initiating_client = endpoint.client;
        operation.initiating_page_id = endpoint.page_id;
    }

    if (operation.is_browser_traversal()) {
        if (!operation.initiating_client) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
            return;
        }
        VERIFY(operation.parameters.has<Web::TraverseToStepHistoryOperationParameters>());
        auto const& parameters = operation.parameters.get<Web::TraverseToStepHistoryOperationParameters>();
        apply_history_step(operation, parameters.target_step, operation.check_for_cancelation, {}, parameters.user_involvement, Web::Bindings::NavigationType::Traverse);
        return;
    }

    if (operation.initiating_client) {
        add_history_operation_completion_endpoint(operation, {
                                                                 operation.initiating_client,
                                                                 operation.initiating_page_id,
                                                             });
    }

    if (history_operation_is_direct(operation.parameters)) {
        run_direct_history_operation(operation);
        return;
    }

    if (!operation.initiating_client) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
        return;
    }

    if (operation.parameters.has<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>()) {
        operation.owns_navigation_transaction = navigation_transaction_matches(
            operation, *operation.initiating_client, operation.initiating_page_id);
        if (!operation.owns_navigation_transaction) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
            return;
        }
    }

    Optional<Web::ReconstructedChildNavigation> reconstructed_child_navigation;
    if (operation.parameters.has<Web::NavigableCreationHistoryOperationParameters>()) {
        auto const& parameters = operation.parameters.get<Web::NavigableCreationHistoryOperationParameters>();
        auto child_navigable = find(parameters.navigable_id);
        auto current_step = m_session_history.current_step();
        if (child_navigable.has_value() && current_step.has_value()) {
            if (auto const* target_entry = m_session_history.get_the_target_history_entry(*child_navigable, *current_step)) {
                auto uuid = Web::Crypto::generate_random_uuid();
                auto navigation_id = Utf16String::from_ascii_without_validation(uuid.bytes());
                auto ongoing_navigation = CanonicalNavigable::OngoingNavigation {
                    .url = target_entry->url,
                    .navigation_id = navigation_id,
                    .sequence_number = next_sequence_number(),
                    .has_started = true,
                    .phase = CanonicalNavigable::OngoingNavigation::Phase::Populating,
                };
                child_navigable->set_ongoing_navigation(move(ongoing_navigation));
                child_navigable->set_navigation_host(*operation.initiating_client, operation.initiating_page_id);
                child_navigable->set_active_session_history_entry(*target_entry);
                reconstructed_child_navigation = Web::ReconstructedChildNavigation {
                    .target_entry = *target_entry,
                    .navigation_id = move(navigation_id),
                };
            }
        }
    }
    operation.initiating_client->async_history_operation_started(
        operation.initiating_page_id, operation.operation_id,
        move(reconstructed_child_navigation));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation
void CanonicalTraversable::finalize_a_cross_document_navigation(HistoryOperation& operation, Web::CrossDocumentNavigationFinalizationHostState host_state)
{
    auto const& parameters = operation.parameters.get<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>();
    auto navigable = find(parameters.navigable_id);
    if (!navigable.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 1. Assert: this is running on navigable's traversable navigable's session history traversal queue.
    VERIFY(operation.queue_promise);
    VERIFY(!operation.queue_promise->is_resolved() && !operation.queue_promise->is_rejected());
    VERIFY(&navigable->top_level_traversable() == this);

    // 2. Set navigable's is delaying load events to false.
    // NB: The process hosting navigable performed this step before sending host_state. The reply continues this
    //     UI-owned algorithm at the same session history traversal queue position.

    if (host_state.pending_document_origin.has_value() != host_state.active_document_origin.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 3. If historyEntry's document is null, then return.
    if (!host_state.pending_document_origin.has_value()) {
        if (navigable->is_top_level_traversable()) {
            if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value())
                view->did_cancel_loading(parameters.navigation_id);
        }
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
        return;
    }

    auto pending_history_entry = parameters.history_entry;

    // 4. If all of the following are true:
    //    - navigable's parent is null;
    //    - historyEntry's document's browsing context is not an auxiliary browsing context whose opener browsing
    //      context is non-null; and
    //    - historyEntry's document's origin is not navigable's active document's origin,
    //    then set historyEntry's document state's navigable target name to the empty string.
    if (navigable->parent() == nullptr
        && !host_state.pending_document_is_in_auxiliary_browsing_context_with_opener
        && *host_state.pending_document_origin != *host_state.active_document_origin) {
        pending_history_entry.document_state.navigable_target_name = {};
    }

    auto current_step = m_session_history.current_step();
    if (!current_step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 5. Let entryToReplace be navigable's active session history entry if historyHandling is "replace", otherwise null.
    auto entry_to_replace = parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace
        ? navigable->active_session_history_entry_identity()
        : Optional<Web::HTML::SessionHistoryEntryIdentity> {};
    auto view = ViewImplementation::find_view_for_traversable(*this);
    auto may_append_from_replacement_initial_document = !entry_to_replace.has_value()
        && navigable->is_top_level_traversable()
        && view.has_value()
        && !view->m_client_state.hosts_committed_entry;
    // A replacement renderer navigates from its non-canonical initial about:blank with "replace" handling. With no
    // canonical active entry to replace, that specific navigation must append after the preserved current entry.
    if (parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace
        && !entry_to_replace.has_value()
        && !may_append_from_replacement_initial_document) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 6. Let traversable be navigable's traversable navigable.
    // NB: This CanonicalTraversable is navigable's traversable navigable.

    // 7. Let targetStep be null.
    Optional<i32> target_step;

    // 8. Let targetEntries be the result of getting session history entries for navigable.
    auto target_entries = m_session_history.get_session_history_entries(*navigable);
    if (!target_entries.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 9. If entryToReplace is null:
    if (!entry_to_replace.has_value()) {
        // AD-HOC: Roll back steps 9.1 through 9.4 if the serialized targetEntries disappear during the update.
        auto session_history_before_append = m_session_history;

        // 1. Clear the forward session history of traversable.
        if (!m_session_history.clear_the_forward_session_history()) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }

        // 2. Set targetStep to traversable's current session history step + 1.
        VERIFY(*current_step < NumericLimits<i32>::max());
        target_step = *current_step + 1;

        // 3. Set historyEntry's step to targetStep.
        auto history_entry = Web::HTML::create_session_history_entry_descriptor(move(pending_history_entry), *target_step);

        // 4. Append historyEntry to targetEntries.
        if (!m_session_history.append_or_replace_session_history_entry(*navigable, history_entry, {})) {
            m_session_history = move(session_history_before_append);
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }
    }
    // Otherwise:
    else {
        // 1. Replace entryToReplace with historyEntry in targetEntries.
        auto canonical_entry_to_replace = target_entries->find_if([&](auto const& entry) {
            return Web::HTML::session_history_entry_identity(entry) == *entry_to_replace;
        });
        if (canonical_entry_to_replace == target_entries->end()) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }

        // 2. Set historyEntry's step to entryToReplace's step.
        auto history_entry = Web::HTML::create_session_history_entry_descriptor(move(pending_history_entry), canonical_entry_to_replace->step);

        // 3. If historyEntry's document state's origin is same origin with entryToReplace's document state's origin,
        //    then set historyEntry's navigation API key to entryToReplace's navigation API key.
        if (history_entry.document_state.origin.has_value()
            && canonical_entry_to_replace->document_state.origin.has_value()
            && history_entry.document_state.origin->is_same_origin(*canonical_entry_to_replace->document_state.origin)) {
            history_entry.navigation_api_key = canonical_entry_to_replace->navigation_api_key;
        }

        if (!m_session_history.append_or_replace_session_history_entry(*navigable, history_entry, entry_to_replace)) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }

        // 4. Set targetStep to traversable's current session history step.
        target_step = *current_step;
    }

    // 10. Apply the push/replace history step targetStep to traversable given historyHandling and userInvolvement.
    apply_the_push_or_replace_history_step(operation, *target_step, parameters.history_handling, parameters.user_involvement);
}

void CanonicalTraversable::did_receive_history_operation_ready(WebContentClient& source_client, u64 source_page_id, Web::HTML::CrossProcessId operation_id, Web::HistoryOperationReadyResult result)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation || operation->algorithm)
        return;
    if (history_operation_is_direct(operation->parameters))
        return;
    if (!operation->was_initiated_by(source_client, source_page_id))
        return;
    if (!navigation_transaction_matches(*operation, source_client, source_page_id)) {
        finish_history_operation(operation_id, Web::HTML::HistoryStepResult::Applied, {});
        return;
    }

    if (result.has<Web::HTML::HistoryStepResult>()) {
        finish_history_operation(operation_id, result.get<Web::HTML::HistoryStepResult>(), {});
        return;
    }

    VERIFY(!operation->is_browser_traversal());
    auto const& request = operation->parameters;
    auto result_matches_request = request.visit(
        [&](Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const&) { return result.has<Web::CrossDocumentNavigationFinalizationHostState>(); },
        [&](Web::NavigableCreationHistoryOperationParameters const&) { return result.has<Web::HTML::CrossProcessId>(); },
        [&](Web::FinalizeSameDocumentNavigationHistoryOperationParameters const&) { return false; },
        [&](Web::CloseTopLevelTraversableHistoryOperationParameters const&) { return false; },
        [&](Web::FlushSessionHistoryTraversalQueueOperationParameters const&) { return false; },
        [&](auto const&) { return result.has<Empty>(); });
    if (!result_matches_request) {
        finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    request.visit(
        [&](Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const&) {
            auto host_state = move(result.get<Web::CrossDocumentNavigationFinalizationHostState>());
            finalize_a_cross_document_navigation(*operation, move(host_state));
        },
        [&](Web::ReloadHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::TraverseByDeltaHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::TraverseToStepHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::NavigationAPITraverseHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::ResumeTraverseHistoryOperationParameters const& parameters) {
            resume_applying_the_traverse_history_step(*operation, parameters.target_step, parameters.user_involvement);
        },
        [&](Web::NavigableCreationHistoryOperationParameters const& parameters) {
            auto parent_document_state_id = result.get<Web::HTML::CrossProcessId>();
            auto parent_navigable = find(parameters.parent_navigable_id);
            if (!parent_navigable.has_value()
                || !append_nested_history(*parent_navigable, parent_document_state_id, parameters.navigable_id, parameters.initial_history_entry).has_value()) {
                finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
                return;
            }

            // Steps 1-6 of create-a-new-child-navigable's queued work are complete. Resume at step 7.
            update_for_navigable_creation_or_destruction(*operation);
        },
        [&](Web::NavigableDestructionHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::FinalizeSameDocumentNavigationHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::CloseTopLevelTraversableHistoryOperationParameters const&) {
            // Close runs entirely in the requesting process at this queue position and must complete with proceed=false.
            VERIFY_NOT_REACHED();
        },
        [&](Web::FlushSessionHistoryTraversalQueueOperationParameters const&) {
            VERIFY_NOT_REACHED();
        });
}

void CanonicalTraversable::finish_history_operation(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Optional<i32> committed_step)
{
    auto operation = m_history_operations.take(operation_id);
    if (!operation.has_value())
        return;
    auto& taken_operation = **operation;
    (void)discard_pending_same_document_session_history_entries_for_operation(operation_id, taken_operation.parameters);
    if (m_pending_browser_history_traversal.has_value()
        && m_pending_browser_history_traversal->operation_id == operation_id) {
        VERIFY(m_pending_browser_history_traversal->on_ready_callbacks.is_empty());
        m_pending_browser_history_traversal.clear();
    }

    if (taken_operation.owns_navigation_transaction) {
        auto const& parameters = taken_operation.parameters.get<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>();
        if (auto navigable = find(parameters.navigable_id); navigable.has_value())
            navigable->did_finish_navigation_transaction(parameters.navigation_id, result);
    }

    if (committed_step.has_value()) {
        if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value()) {
            if (auto const* current_entry = m_session_history.current_entry())
                view->set_url(current_entry->url);
        }
    }

    for (auto& endpoint : taken_operation.completion_endpoints)
        endpoint.client->async_complete_history_operation(
            endpoint.page_id, operation_id, result, committed_step,
            m_session_history.size());

    // All apply-driven mutations have settled at operation completion.
    session_history_changed();

    if (taken_operation.on_complete)
        taken_operation.on_complete(result, committed_step);

    if (taken_operation.is_browser_traversal()) {
        auto callback = move(taken_operation.on_browser_traversal_ready);
        if (callback)
            callback();
        if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value())
            view->did_finish_history_traversal(operation_id, result);
    }

    // NB: Resolving the queue promise can synchronously start the next queued operation.
    if (taken_operation.queue_promise)
        taken_operation.queue_promise->resolve({});

    // The completion callback that brought us here can be running inside the algorithm object; destroy the
    // operation only once the stack has unwound.
    Core::deferred_invoke([operation = operation.release_value()] { });
}

bool CanonicalTraversable::discard_pending_same_document_session_history_entries_for_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters const& request)
{
    if (!request.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>())
        return false;
    auto navigable = find(request.get<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>().navigable_id);
    if (navigable.has_value()) {
        auto entry_count = navigable->pending_same_document_session_history_entries().size();
        navigable->remove_pending_same_document_session_history_entries(operation_id);
        return entry_count != navigable->pending_same_document_session_history_entries().size();
    }
    return false;
}

void CanonicalTraversable::abandon_history_operations()
{
    while (!m_history_operations.is_empty()) {
        auto operation_id = m_history_operations.begin()->key;
        finish_history_operation(operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
    }
    if (m_pending_browser_history_traversal.has_value()) {
        auto callbacks = move(m_pending_browser_history_traversal->on_ready_callbacks);
        m_pending_browser_history_traversal.clear();
        for (auto& callback : callbacks)
            callback();
    }
    bool discarded_pending_entries = false;
    for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
        discarded_pending_entries |= !navigable.pending_same_document_session_history_entries().is_empty();
        (void)navigable.take_pending_same_document_session_history_entries();
        return IterationDecision::Continue;
    });
    if (discarded_pending_entries)
        session_history_changed();
}

void CanonicalTraversable::did_receive_history_step_unload_cancelation_result(WebContentClient& source_client, u64 source_page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (!operation->was_initiated_by(source_client, source_page_id))
            return;
        if (operation->is_browser_traversal() && result == Web::HTML::HistoryStepResult::CanceledPendingNavigation)
            result = Web::HTML::HistoryStepResult::Applied;
        if (auto pending = move(operation->pending_unload_cancelation))
            pending(result);
    }
}

void CanonicalTraversable::did_receive_changing_navigable_history_job_ready(WebContentClient& source_client, u64 source_page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition)
{
    if (auto* operation = find_history_operation(operation_id)) {
        auto pending_job = operation->pending_changing_jobs.get(navigable_id);
        if (!pending_job.has_value())
            return;
        auto endpoint = operation->changing_job_endpoints.get(navigable_id);
        if (!endpoint.has_value() || endpoint->client.ptr() != &source_client || endpoint->page_id != source_page_id)
            return;

        if (!navigation_transaction_matches(*operation, source_client, source_page_id, navigable_id))
            disposition = Web::HTML::ChangingNavigableHistoryStepJobDisposition::Stale;

        switch (pending_job.value()->phase) {
        case HistoryOperation::PendingChangingJob::Phase::RedispatchedAfterReady:
            if (disposition == Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready) {
                pending_job.value()->phase = HistoryOperation::PendingChangingJob::Phase::ReadyReported;
                if (pending_job.value()->continuation.has_value())
                    dispatch_changing_navigable_history_step_continuation(*operation, navigable_id);
                return;
            }
            pending_job.value()->phase = HistoryOperation::PendingChangingJob::Phase::RedispatchFailed;
            operation->changing_job_endpoints.remove(navigable_id);
            if (pending_job.value()->continuation.has_value()) {
                auto taken_job = operation->pending_changing_jobs.take(navigable_id);
                auto purpose = taken_job.value()->purpose;
                if (taken_job.value()->on_continuation_complete)
                    taken_job.value()->on_continuation_complete();
                if (purpose == HistoryOperation::PendingChangingJob::Purpose::CrashRecovery)
                    finish_deferred_history_operation_after_crash_recovery(operation_id);
            }
            return;
        case HistoryOperation::PendingChangingJob::Phase::Dispatched:
        case HistoryOperation::PendingChangingJob::Phase::RedispatchedBeforeReady:
            break;
        case HistoryOperation::PendingChangingJob::Phase::ReadyReported:
        case HistoryOperation::PendingChangingJob::Phase::ContinuationDispatched:
        case HistoryOperation::PendingChangingJob::Phase::RedispatchFailed:
            return;
        }

        auto on_complete = move(pending_job.value()->on_complete);
        if (disposition == Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready) {
            pending_job.value()->phase = HistoryOperation::PendingChangingJob::Phase::ReadyReported;
            on_complete(disposition);
            return;
        }

        auto purpose = pending_job.value()->purpose;
        auto on_continuation_complete = move(pending_job.value()->on_continuation_complete);
        operation->pending_changing_jobs.remove(navigable_id);
        operation->changing_job_endpoints.remove(navigable_id);
        if (purpose != HistoryOperation::PendingChangingJob::Purpose::ApplyHistoryStep) {
            if (on_continuation_complete)
                on_continuation_complete();
            finish_deferred_history_operation_after_crash_recovery(operation_id);
            return;
        }
        on_complete(disposition);
    }
}

void CanonicalTraversable::did_receive_changing_navigable_continuation_applied(WebContentClient& source_client, u64 source_page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::ReplicatedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state)
{
    if (auto* operation = find_history_operation(operation_id)) {
        auto endpoint = operation->changing_job_endpoints.get(navigable_id);
        if (!endpoint.has_value() || endpoint->client.ptr() != &source_client || endpoint->page_id != source_page_id)
            return;
        auto pending_job = operation->pending_changing_jobs.take(navigable_id);
        if (!pending_job.has_value())
            return;
        operation->changing_job_endpoints.remove(navigable_id);
        if (activated_navigable_state.has_value()) {
            auto navigable = find(navigable_id);
            if (navigable.has_value()) {
                auto navigation_id = operation->parameters.visit(
                    [](Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const& parameters) { return parameters.navigation_id; },
                    [](auto const&) { return Optional<Utf16String> {}; });
                activated_navigable_state->active_session_history_entry_identity = Web::HTML::session_history_entry_identity(pending_job.value()->job.target_entry);
                auto active_document_url = activated_navigable_state->active_document_url;
                navigable->did_commit_navigation(activated_navigable_state.release_value(), navigation_id);

                if (navigable_id == id()) {
                    if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value()) {
                        if (!Web::HTML::url_matches_about_blank(active_document_url) || !view->m_client_state.site_url.has_value())
                            view->m_client_state.site_url = move(active_document_url);
                        view->m_client_state.hosts_committed_entry = true;
                        view->m_external_url_request_policy.clear_page_request_allowance();
                        if (view->on_top_level_navigation_commit)
                            view->on_top_level_navigation_commit();
                    }
                }
            }
        }
        if (previous_entry_persisted_state.has_value()) {
            auto navigable = find(navigable_id);
            if (navigable.has_value())
                update_session_history_entry_persisted_state(*navigable, *previous_entry_persisted_state);
        }
        // A replacement document's creation operation also applies a top-level continuation, but it runs before
        // the document has accepted the UI-owned history state. Only the browser traversal itself reaches the
        // observable top-level completion point here.
        if (navigable_id == id() && operation->is_browser_traversal()) {
            if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value())
                view->did_apply_top_level_history_traversal_step(operation_id);
        }
        auto purpose = pending_job.value()->purpose;
        if (pending_job.value()->on_continuation_complete)
            pending_job.value()->on_continuation_complete();
        if (purpose == HistoryOperation::PendingChangingJob::Purpose::CrashRecovery)
            finish_deferred_history_operation_after_crash_recovery(operation_id);
    }
}

void CanonicalTraversable::did_receive_nonchanging_navigable_history_state_updated(WebContentClient& source_client, u64 source_page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id)
{
    if (auto* operation = find_history_operation(operation_id)) {
        auto pending = operation->pending_nonchanging_updates.get(navigable_id);
        if (!pending.has_value() || pending->endpoint.client.ptr() != &source_client || pending->endpoint.page_id != source_page_id)
            return;
        auto taken_pending = operation->pending_nonchanging_updates.take(navigable_id);
        taken_pending->on_complete();
    }
}

}
