/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

CanonicalTraversable::CanonicalTraversable()
    : CanonicalNavigable({}, {}, nullptr, 0)
{
}

static Web::HTML::CrossProcessId allocate_ui_process_document_state_id()
{
    return Application::the().allocate_ui_process_cross_process_id();
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

static Web::HTML::SessionHistoryEntryDescriptor const* target_entry_for_step(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, i32 step)
{
    Web::HTML::SessionHistoryEntryDescriptor const* target_entry = nullptr;
    for (auto const& entry : entries) {
        if (entry.step <= step && (!target_entry || entry.step > target_entry->step))
            target_entry = &entry;
    }
    return target_entry;
}

void CanonicalTraversable::reconcile_navigable_tree_after_session_history_seed()
{
    auto current_step = m_session_history.current_step();
    auto const* current_entry = m_session_history.current_entry();
    if (!current_step.has_value() || !current_entry)
        return;

    reconcile_navigable_subtree_after_session_history_seed(*this, *current_entry, *current_step);
}

void CanonicalTraversable::reconcile_navigable_subtree_after_session_history_seed(CanonicalNavigable& parent, Web::HTML::SessionHistoryEntryDescriptor const& parent_entry, i32 current_step)
{
    auto const& children = parent.children();
    auto const& nested_histories = parent_entry.document_state.nested_histories;
    if (children.size() != nested_histories.size())
        return;

    for (size_t i = 0; i < children.size(); ++i) {
        auto& child = *children[i];
        auto const& nested_history = nested_histories[i];
        auto existing_navigable = find(nested_history.id);
        if (existing_navigable.has_value() && &*existing_navigable != &child)
            return;
    }

    // AD-HOC: A replacement process creates child navigables before receiving the UI-owned history seed. WebContent
    //         retargets those children to the restored nested history IDs, so keep the canonical tree in sync.
    for (size_t i = 0; i < children.size(); ++i) {
        auto& child = *children[i];
        auto const& nested_history = nested_histories[i];
        if (child.id() != nested_history.id) {
            auto previous_id = child.id();
            m_navigable_index.remove(child.id());
            child.set_id(nested_history.id);
            m_navigable_index.set(child.id(), child.make_weak_ptr());
            for (auto& navigable_id : m_pending_web_content_session_history_seed.navigables_to_restore) {
                if (navigable_id == previous_id)
                    navigable_id = child.id();
            }
        }

        if (auto const* child_entry = target_entry_for_step(nested_history.entries, current_step))
            reconcile_navigable_subtree_after_session_history_seed(child, *child_entry, current_step);
    }
}

static Optional<size_t> current_top_level_history_entry_index_for_step(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, Optional<i32> current_step)
{
    if (!current_step.has_value())
        return {};

    Optional<size_t> current_entry_index;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].document_state.navigable_target_name.is_empty())
            continue;

        if (entries[i].step <= *current_step)
            current_entry_index = i;
        if (entries[i].step >= *current_step)
            break;
    }
    return current_entry_index;
}

void CanonicalTraversable::abandon_pending_web_content_session_history_seed()
{
    m_session_history_entry_url_loading_from_ui_process.clear();
    m_pending_web_content_session_history_seed.clear();
}

void CanonicalTraversable::prepare_to_seed_web_content_session_history_from_ui_process()
{
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
    m_pending_session_history_navigation.clear();
    m_pending_web_content_session_history_seed.clear();
    m_pending_web_content_session_history_seed.step_after_loading_top_level_entry = m_session_history.current_step_to_restore_after_loading_top_level_entry();
    m_pending_web_content_session_history_seed.should_send_entries = true;
    m_pending_web_content_session_history_seed.ignore_updates_until_seed = true;
}

static bool can_seed_replacement_process_before_load(TraversableSessionHistory const& session_history, Optional<URL::URL> const& session_history_entry_url_loading_from_ui_process, PendingWebContentSessionHistorySeed const& pending_web_content_session_history_seed)
{
    if (!pending_web_content_session_history_seed.should_send_entries)
        return false;
    if (session_history_entry_url_loading_from_ui_process.has_value())
        return false;
    if (session_history.current_step_to_restore_after_loading_top_level_entry().has_value())
        return false;
    return true;
}

ProcessSwapNavigationPreparation CanonicalTraversable::prepare_for_process_swap_navigation(URL::URL const& url, Web::HTML::DocumentResource document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    ProcessSwapNavigationPreparation result;

    auto ui_session_history_already_points_to_url = false;
    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url)
        ui_session_history_already_points_to_url = true;

    if (m_pending_session_history_traversal.has_value() && m_pending_session_history_traversal->will_replace_web_content_process)
        m_pending_session_history_traversal->stage = PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess;
    if (m_pending_session_history_navigation.has_value())
        m_pending_session_history_navigation->web_content_restore_mode = PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess;
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
    m_pending_web_content_session_history_seed.waiting_for_ack = false;
    m_pending_web_content_session_history_seed.should_send_entries = true;
    m_pending_web_content_session_history_seed.ignore_updates_until_seed = true;

    if (!ui_session_history_already_points_to_url && !m_session_history_entry_url_loading_from_ui_process.has_value()) {
        if (m_session_history.current_entry()) {
            m_pending_session_history_navigation = PendingSessionHistoryNavigation {
                url,
                m_session_history,
                PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess,
            };
        } else {
            m_pending_session_history_navigation.clear();
        }

        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Replace)
            m_session_history.replace_current_entry(url, allocate_ui_process_document_state_id(), move(document_resource));
        else
            m_session_history.navigate(url, allocate_ui_process_document_state_id(), move(document_resource));
        m_current_web_content_session_history_matches_mirror = false;
        result.should_update_navigation_action_state = true;
    }

    if (!m_session_history_entry_url_loading_from_ui_process.has_value())
        m_pending_web_content_session_history_seed.step_after_loading_top_level_entry = m_session_history.current_step_to_restore_after_loading_top_level_entry();

    result.should_seed_web_content_before_load = can_seed_replacement_process_before_load(m_session_history, m_session_history_entry_url_loading_from_ui_process, m_pending_web_content_session_history_seed);
    return result;
}

PageLoadPreparation CanonicalTraversable::prepare_for_page_load(URL::URL const& url, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    PageLoadPreparation result;

    if (m_session_history_entry_url_loading_from_ui_process.has_value())
        return result;

    abandon_pending_web_content_session_history_seed();
    m_pending_session_history_traversal.clear();
    auto const* current_entry = m_session_history.current_entry();
    auto is_javascript_navigation = url.scheme() == "javascript"sv;
    result.should_defer_ui_process_history_update = is_javascript_navigation;
    if (current_entry && !is_javascript_navigation)
        m_pending_session_history_navigation = PendingSessionHistoryNavigation { url, m_session_history };
    else
        m_pending_session_history_navigation.clear();

    if (is_javascript_navigation)
        return result;

    auto ui_process_history_handling = history_handling;
    if (ui_process_history_handling == Web::Bindings::NavigationHistoryBehavior::Auto) {
        // https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
        // If url equals navigable's active document's URL, and
        // initiatorOriginSnapshot is same origin with targetNavigable's
        // active document's origin, then set historyHandling to "replace".
        if (current_entry && current_entry->url == url)
            ui_process_history_handling = Web::Bindings::NavigationHistoryBehavior::Replace;
        else
            ui_process_history_handling = Web::Bindings::NavigationHistoryBehavior::Push;
    }

    if (ui_process_history_handling == Web::Bindings::NavigationHistoryBehavior::Replace)
        m_session_history.replace_current_entry(url, allocate_ui_process_document_state_id(), Empty {});
    else
        m_session_history.navigate(url, allocate_ui_process_document_state_id());
    m_current_web_content_session_history_matches_mirror = false;
    result.should_update_navigation_action_state = true;
    return result;
}

void CanonicalTraversable::prepare_for_non_history_page_load()
{
    abandon_pending_web_content_session_history_seed();
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
}

void CanonicalTraversable::prepare_for_reload()
{
    abandon_pending_web_content_session_history_seed();
    m_session_history.mark_current_entry_reload_pending();
    m_current_web_content_session_history_matches_mirror = false;
}

WebContentSessionHistoryUpdateDecision CanonicalTraversable::did_receive_web_content_session_history_update_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, URL::URL const& current_url)
{
    // NB: dumpUIProcessSessionHistory() first sends WebContent's current snapshot to the UI process, then returns
    //     the UI mirror. If a stale seed ack is still pending, normal async snapshots are intentionally ignored, so
    //     use the same convergence path as a rejected seed ack to make this testing hook deterministic.
    if (m_pending_web_content_session_history_seed.waiting_for_ack) {
        auto update = adopt_web_content_session_history_after_rejected_seed(move(entries), move(used_steps), current_used_step_index, current_url);
        if (update.update_result == TraversableSessionHistory::UpdateResult::InvalidSnapshot)
            return { .ignore_reason = "ignored-session-history-for-testing-before-ui-seed-ack"sv };
        return { .update = move(update) };
    }

    return {
        .update = update_session_history_from_web_content(move(entries), move(used_steps), current_used_step_index, false, true, current_url),
    };
}

void CanonicalTraversable::did_create_top_level_traversable(Web::HTML::SessionHistoryEntryDescriptor initial_history_entry)
{
    m_session_history.initialize_with_initial_history_entry(move(initial_history_entry));
    m_current_web_content_session_history_matches_mirror = true;
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

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed)
        return false;

    return m_session_history.update_entry(nested_history_id_for(navigable), navigation_api_key, [&](auto& entry) {
        entry.navigation_api_state = navigation_api_state;
    });
}

bool CanonicalTraversable::update_session_history_entry_scroll_restoration_mode(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    VERIFY(&navigable.top_level_traversable() == this);

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed)
        return false;

    return m_session_history.update_entry(nested_history_id_for(navigable), navigation_api_key, [&](auto& entry) {
        entry.scroll_restoration_mode = scroll_restoration_mode;
    });
}

bool CanonicalTraversable::update_session_history_entry_scroll_position_data(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Web::HTML::SessionHistoryEntryScrollPositionData scroll_position_data)
{
    VERIFY(&navigable.top_level_traversable() == this);

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed)
        return false;

    return m_session_history.update_entry(nested_history_id_for(navigable), navigation_api_key, [&](auto& entry) {
        entry.scroll_position_data = scroll_position_data;
    });
}

bool CanonicalTraversable::update_session_history_entry_document_state_navigable_target_name(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Utf16String navigable_target_name)
{
    VERIFY(&navigable.top_level_traversable() == this);

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed)
        return false;

    return m_session_history.update_document_state(nested_history_id_for(navigable), navigation_api_key, [&](auto& document_state) {
        document_state.navigable_target_name = navigable_target_name;
    });
}

bool CanonicalTraversable::set_session_history_entry_document_state_reload_pending(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, bool reload_pending)
{
    VERIFY(&navigable.top_level_traversable() == this);

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed)
        return false;

    auto did_update = m_session_history.update_document_state(nested_history_id_for(navigable), navigation_api_key, [&](auto& document_state) {
        document_state.reload_pending = reload_pending;
    });
    if (did_update)
        m_current_web_content_session_history_matches_mirror = m_session_history.web_content_history_matches_mirror();
    return did_update;
}

bool CanonicalTraversable::append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::SessionHistoryNestedHistoryDescriptor nested_history)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed)
        return false;

    auto child_navigable = find(nested_history.id);
    if (!child_navigable.has_value() || child_navigable->parent() != &parent_navigable)
        return false;
    return m_session_history.append_nested_history(parent_navigable, move(nested_history));
}

bool CanonicalTraversable::remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId child_navigable_id)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed)
        return false;

    return m_session_history.remove_nested_history(parent_navigable, child_navigable_id);
}

Optional<TraversableSessionHistory::SameDocumentNavigationFinalization> CanonicalTraversable::request_to_finalize_same_document_navigation(CanonicalNavigable const& navigable, Web::HTML::SameDocumentNavigationEntry target_entry, bool replaces_current_entry, Web::HTML::HistoryHandlingBehavior history_handling, Web::HTML::UserNavigationInvolvement user_involvement, bool applies_history_step_in_coordinator)
{
    VERIFY(&navigable.top_level_traversable() == this);

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed)
        return {};

    auto finalization = m_session_history.finalize_same_document_navigation(navigable, move(target_entry), replaces_current_entry, history_handling, user_involvement, maximum_claimed_session_history_step());

    if (finalization.has_value() && applies_history_step_in_coordinator && !replaces_current_entry)
        claim_step_for_pending_same_document_history_operation(navigable.id(), finalization->target_step);

    // AD-HOC: A synchronous same-document commit that does not run through the coordinated apply-history-step
    //         operation still moves the canonical current step; record it as the newest commit so an operation
    //         still in flight cannot move the current step backwards past it when it completes.
    //         See https://github.com/whatwg/html/issues/12576.
    if (finalization.has_value() && !applies_history_step_in_coordinator)
        m_apply_history_step_traversable_state.committed_generation = ++m_apply_history_step_traversable_state.generation_counter;

    // WebContent has already committed this navigation locally, so a request the canonical history could not apply
    // leaves the two out of sync until the mirror is reconciled.
    m_current_web_content_session_history_matches_mirror = finalization.has_value() && m_session_history.web_content_history_matches_mirror();

    return finalization;
}

bool CanonicalTraversable::finalize_cross_document_navigation(CanonicalNavigable const& navigable, Web::HTML::SessionHistoryEntryDescriptor history_entry, Optional<Utf16String> entry_to_replace_navigation_api_key)
{
    VERIFY(&navigable.top_level_traversable() == this);

    auto maximum_claimed_step = claim_step_for_pending_cross_document_history_operation(navigable.id(), history_entry.step);

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed) {
        record_finalized_entry_for_pending_history_operation(navigable.id(), move(history_entry));
        return false;
    }

    auto did_finalize = m_session_history.finalize_cross_document_navigation(nested_history_id_for(navigable), move(history_entry), move(entry_to_replace_navigation_api_key), maximum_claimed_step);
    if (did_finalize)
        m_current_web_content_session_history_matches_mirror = false;
    return did_finalize;
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

WebContentSessionHistoryUpdateResult CanonicalTraversable::update_session_history_from_web_content(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, bool pending_step_after_fallback_load_was_restored, bool seed_web_content_on_invalid_snapshot, URL::URL const& current_url)
{
    auto update_result = m_session_history.update_from_web_content(move(entries), move(used_steps), current_used_step_index);
    m_current_web_content_session_history_matches_mirror = update_result == TraversableSessionHistory::UpdateResult::CompleteSnapshot
        && m_session_history.web_content_history_matches_mirror();

    WebContentSessionHistoryUpdateResult result {
        .update_result = update_result,
    };

    if (update_result != TraversableSessionHistory::UpdateResult::InvalidSnapshot) {
        if (update_result == TraversableSessionHistory::UpdateResult::CompleteSnapshot)
            m_pending_session_history_navigation.clear();
        if (auto* current_entry = m_session_history.current_entry())
            result.current_url = current_entry->url;
        if (pending_step_after_fallback_load_was_restored)
            m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.clear();
    } else if (seed_web_content_on_invalid_snapshot) {
        if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == current_url) {
            prepare_to_seed_web_content_session_history_from_ui_process();
            result.should_seed_web_content = true;
        }
    }

    return result;
}

WebContentSessionHistoryUpdateResult CanonicalTraversable::adopt_web_content_session_history_after_rejected_seed(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, URL::URL const& current_url)
{
    if (entries.is_empty())
        return {};

    auto entries_from_web_content = entries;
    auto used_steps_from_web_content = used_steps;
    auto update = update_session_history_from_web_content(move(entries), move(used_steps), current_used_step_index, false, false, current_url);
    if (update.update_result == TraversableSessionHistory::UpdateResult::InvalidSnapshot && current_used_step_index < used_steps_from_web_content.size()) {
        auto current_top_level_entry_index = current_top_level_history_entry_index_for_step(entries_from_web_content, used_steps_from_web_content[current_used_step_index]);
        if (current_top_level_entry_index.has_value() && entries_from_web_content[*current_top_level_entry_index].url == current_url) {
            m_session_history.clear();
            update = update_session_history_from_web_content(move(entries_from_web_content), move(used_steps_from_web_content), current_used_step_index, false, false, current_url);
        }
    }
    if (update.update_result == TraversableSessionHistory::UpdateResult::InvalidSnapshot)
        return update;

    m_pending_web_content_session_history_seed.clear();
    m_pending_session_history_traversal.clear();
    return update;
}

WebContentSessionHistorySeedAckResult CanonicalTraversable::did_receive_web_content_session_history_seed_ack(bool accepted, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, URL::URL const& current_url)
{
    if (!m_pending_web_content_session_history_seed.waiting_for_ack)
        return { .ignored = true, .dump_reason = "ignored-webcontent-session-history-seed-ack"sv };

    WebContentSessionHistorySeedAckResult result;
    result.should_update_navigation_action_state = true;

    if (!accepted) {
        auto update = adopt_web_content_session_history_after_rejected_seed(move(entries), move(used_steps), current_used_step_index, current_url);
        if (update.update_result != TraversableSessionHistory::UpdateResult::InvalidSnapshot) {
            result.dump_reason = "webcontent-session-history-seed-rejected-with-current-snapshot"sv;
            result.current_url = move(update.current_url);
            // NB: Applying the adopted snapshot's current URL already refreshes the navigation actions.
            result.should_update_navigation_action_state = false;
            return result;
        }

        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        result.dump_reason = "webcontent-session-history-seed-rejected"sv;
        return result;
    }

    if (!m_session_history.did_seed_web_content_from_ui_process(move(entries), move(used_steps), current_used_step_index)) {
        if (m_pending_web_content_session_history_seed.should_reseed_after_current_history_load) {
            m_pending_web_content_session_history_seed.waiting_for_ack = false;
            m_pending_web_content_session_history_seed.should_send_entries = true;
            m_pending_web_content_session_history_seed.ignore_updates_until_seed = true;
            m_current_web_content_session_history_matches_mirror = false;
            result.dump_reason = "webcontent-session-history-preload-seed-ack-mismatch"sv;
            return result;
        }

        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        result.dump_reason = "webcontent-session-history-seed-ack-mismatch"sv;
        return result;
    }

    reconcile_navigable_tree_after_session_history_seed();

    m_pending_web_content_session_history_seed.waiting_for_ack = false;
    if (m_pending_web_content_session_history_seed.should_reseed_after_current_history_load) {
        m_pending_web_content_session_history_seed.should_send_entries = true;
        m_pending_web_content_session_history_seed.ignore_updates_until_seed = true;
        m_current_web_content_session_history_matches_mirror = false;
        result.dump_reason = "webcontent-session-history-preload-seed-ack"sv;
        return result;
    }

    auto should_notify_top_level_traversal_applied = !m_pending_session_history_navigation.has_value()
        && m_pending_session_history_traversal.has_value()
        && (m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess
            || m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess);
    // The final post-load seed restores the active top-level entry's persisted state before acknowledging it. Any
    // nested-history repair may continue afterward without keeping observers of the top-level traversal waiting.
    auto did_notify_top_level_traversal_applied = should_notify_top_level_traversal_applied
        && notify_top_level_traversal_applied();
    m_pending_web_content_session_history_seed.ignore_updates_until_seed = false;
    m_current_web_content_session_history_matches_mirror = !m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()
        && !m_pending_session_history_navigation.has_value();
    if (m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()) {
        if (m_pending_session_history_traversal.has_value())
            m_pending_session_history_traversal->stage = PendingSessionHistoryTraversal::Stage::RestoringNestedStepAfterSeed;
        result.step_to_traverse = *m_pending_web_content_session_history_seed.step_after_loading_top_level_entry;
        if (!m_pending_web_content_session_history_seed.navigables_to_restore.is_empty()) {
            result.current_step = m_session_history.web_content_current_step();
            result.navigables_to_restore = m_pending_web_content_session_history_seed.navigables_to_restore;
        }
    } else {
        auto is_waiting_for_history_step_cancelation_check = m_pending_session_history_traversal.has_value()
            && m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::CheckingCancelation;
        if (!is_waiting_for_history_step_cancelation_check) {
            m_pending_session_history_traversal.clear();
            result.should_complete_webdriver_pending_navigation = !did_notify_top_level_traversal_applied
                && !m_pending_session_history_navigation.has_value();
        }
    }

    result.dump_reason = "webcontent-session-history-seed-ack"sv;
    return result;
}

NavigationStartResult CanonicalTraversable::did_start_navigation(URL::URL const& url, Web::HTML::DocumentResource document_resource, bool is_redirect, Web::Bindings::NavigationHistoryBehavior history_handling, bool is_showing_crash_page)
{
    if (m_session_history_entry_url_loading_from_ui_process.has_value()) {
        if (*m_session_history_entry_url_loading_from_ui_process != url)
            return { .dump_reason = "ignored-stale-ui-history-load-start"sv };

        auto should_keep_preseeded_web_content_history = m_pending_web_content_session_history_seed.waiting_for_ack || m_session_history.web_content_uses_ui_step_coordinates();
        m_session_history_entry_url_loading_from_ui_process.clear();
        if (!should_keep_preseeded_web_content_history) {
            m_current_web_content_session_history_matches_mirror = false;
            m_session_history.forget_web_content_state();
        }
        return { .dump_reason = "did-start-navigation-from-ui-history-load"sv };
    }

    if (m_pending_web_content_session_history_seed.should_send_entries || m_pending_web_content_session_history_seed.ignore_updates_until_seed || m_pending_web_content_session_history_seed.waiting_for_ack) {
        if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url != url)
            return { .dump_reason = "ignored-navigation-start-before-ui-history-seed"sv };
    }

    if (is_showing_crash_page) {
        if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
            prepare_to_seed_web_content_session_history_from_ui_process();
            return { .dump_reason = "did-start-navigation-from-crash-page"sv, .did_clear_crash_page = true };
        }
    }

    if (is_redirect) {
        m_session_history.replace_current_entry_url(url, allocate_ui_process_document_state_id());
        if (m_pending_session_history_navigation.has_value())
            m_pending_session_history_navigation->url = url;
        m_current_web_content_session_history_matches_mirror = false;
        return { .dump_reason = "did-start-navigation-redirect"sv, .should_update_navigation_action_state = true, .should_update_webdriver_pending_navigation_url = true, .did_clear_crash_page = is_showing_crash_page };
    }

    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
        if (m_pending_session_history_navigation.has_value() && m_pending_session_history_navigation->url == url)
            return { .did_clear_crash_page = is_showing_crash_page };

        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Push && m_current_web_content_session_history_matches_mirror)
            m_pending_session_history_navigation = PendingSessionHistoryNavigation { url, m_session_history };
        else
            m_pending_session_history_navigation.clear();

        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Replace) {
            m_session_history.replace_current_entry(url, allocate_ui_process_document_state_id(), move(document_resource));
            m_current_web_content_session_history_matches_mirror = false;
            return { .dump_reason = "did-start-navigation-replace-current-url"sv, .should_update_navigation_action_state = true, .did_clear_crash_page = is_showing_crash_page };
        }
        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Push) {
            m_session_history.navigate(url, allocate_ui_process_document_state_id(), move(document_resource));
            m_current_web_content_session_history_matches_mirror = false;
            return { .dump_reason = "did-start-navigation-push-current-url"sv, .should_update_navigation_action_state = true, .did_clear_crash_page = is_showing_crash_page };
        }
        return { .did_clear_crash_page = is_showing_crash_page };
    }

    if (m_session_history.current_entry())
        m_pending_session_history_navigation = PendingSessionHistoryNavigation { url, m_session_history };
    else
        m_pending_session_history_navigation.clear();
    if (history_handling == Web::Bindings::NavigationHistoryBehavior::Replace)
        m_session_history.replace_current_entry(url, allocate_ui_process_document_state_id(), move(document_resource));
    else
        m_session_history.navigate(url, allocate_ui_process_document_state_id(), move(document_resource));
    m_current_web_content_session_history_matches_mirror = false;
    return { .dump_reason = "did-start-navigation"sv, .should_update_navigation_action_state = true, .did_clear_crash_page = is_showing_crash_page };
}

NavigationCancelResult CanonicalTraversable::did_cancel_navigation(URL::URL const& url, bool has_webdriver_pending_navigation)
{
    if (m_pending_session_history_navigation.has_value() && m_pending_session_history_navigation->url == url)
        return { .status = NavigationCancelStatus::RestorePendingSessionHistoryNavigation };

    if (m_session_history_entry_url_loading_from_ui_process.has_value() && *m_session_history_entry_url_loading_from_ui_process == url) {
        m_session_history_entry_url_loading_from_ui_process.clear();
        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        return { .status = NavigationCancelStatus::CanceledUIHistoryLoad };
    }

    if (has_webdriver_pending_navigation) {
        m_session_history.clear_current_entry_reload_pending();
        return { .status = NavigationCancelStatus::CompleteWebdriverPendingNavigation };
    }

    return {};
}

NavigationFinishResult CanonicalTraversable::did_finish_navigation(URL::URL const& url)
{
    NavigationFinishResult result;
    if (m_pending_session_history_navigation.has_value()) {
        if (m_pending_session_history_navigation->url == url) {
            m_pending_session_history_navigation.clear();
        } else if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
            m_pending_session_history_navigation.clear();
            result.should_update_webdriver_pending_navigation_url = true;
        }
    }

    if (!m_pending_web_content_session_history_seed.should_send_entries)
        return result;

    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
        m_session_history.clear_current_entry_reload_pending();
        auto allow_current_entry_reconstruction = m_pending_web_content_session_history_seed.should_reseed_after_current_history_load;
        m_pending_web_content_session_history_seed.should_reseed_after_current_history_load = false;
        result.should_seed_web_content = true;
        result.allow_current_entry_reconstruction = allow_current_entry_reconstruction;
        return result;
    }

    // NB: The first finish notification from a fresh WebContent process can still report about:blank before the
    //     traversed-to entry is ready. Keep the pending seed state intact so partial snapshots remain ignored
    //     until we can seed the full UI-owned history.
    result.dump_reason = "skip-seed-webcontent-session-history"sv;
    return result;
}

RestorePendingSessionHistoryNavigationResult CanonicalTraversable::restore_pending_session_history_navigation()
{
    if (!m_pending_session_history_navigation.has_value())
        return {};

    auto web_content_restore_mode = m_pending_session_history_navigation->web_content_restore_mode;
    m_session_history = move(m_pending_session_history_navigation->previous_session_history);
    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal.clear();

    RestorePendingSessionHistoryNavigationResult result { .restored = true, .web_content_restore_mode = web_content_restore_mode };
    if (auto* current_entry = m_session_history.current_entry()) {
        result.current_url = current_entry->url;
        if (web_content_restore_mode == PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState) {
            m_session_history_entry_url_loading_from_ui_process.clear();
            abandon_pending_web_content_session_history_seed();
            m_current_web_content_session_history_matches_mirror = m_session_history.web_content_history_matches_mirror();
        }
    } else {
        m_current_web_content_session_history_matches_mirror = false;
    }
    return result;
}

bool CanonicalTraversable::web_content_can_apply_traversal(TraversableSessionHistory::TraversalTarget const& target) const
{
    return !m_pending_web_content_session_history_seed.should_send_entries
        && !m_pending_web_content_session_history_seed.ignore_updates_until_seed
        && !m_pending_web_content_session_history_seed.waiting_for_ack
        && !m_session_history_entry_url_loading_from_ui_process.has_value()
        && !m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()
        && m_session_history.web_content_can_traverse_to(target);
}

bool CanonicalTraversable::traversal_requires_process_replacement(TraversableSessionHistory::TraversalTarget const& target, URL::URL const& current_url) const
{
    return SiteIsolationManager::the().navigation_requires_process_swap(current_url, target.target_top_level_entry->url);
}

HistoryTraversalDecision CanonicalTraversable::traverse_the_history_by_delta(int delta, CheckForCancelation check_for_cancelation, URL::URL const& current_url, Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete, Function<void()> on_top_level_traversal_applied)
{
    auto target = m_session_history.traversal_target_for_delta(delta);
    if (!target.has_value())
        return { .outcome = { .status = HistoryTraversalStatus::NoEntry } };

    return traverse_the_history(*target, check_for_cancelation, current_url, move(on_cancelation_check_complete), move(on_top_level_traversal_applied));
}

HistoryTraversalDecision CanonicalTraversable::traverse_the_history_to_step(i32 step, CheckForCancelation check_for_cancelation, URL::URL const& current_url, Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete, Function<void()> on_top_level_traversal_applied)
{
    auto target = m_session_history.traversal_target_for_step(step);
    if (!target.has_value())
        return { .outcome = { .status = HistoryTraversalStatus::NoEntry } };

    return traverse_the_history(*target, check_for_cancelation, current_url, move(on_cancelation_check_complete), move(on_top_level_traversal_applied));
}

HistoryTraversalDecision CanonicalTraversable::traverse_the_history(TraversableSessionHistory::TraversalTarget const& target, CheckForCancelation check_for_cancelation, URL::URL const& current_url, Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete, Function<void()> on_top_level_traversal_applied)
{
    // FIXME: The UI-owned algorithm still asks the process currently hosting a navigable to populate its target
    //        entry. Until a changing-navigable job can select a replacement process itself, predict top-level process
    //        replacement here and route that traversal through the UI-driven load path.
    auto will_replace_web_content_process = SiteIsolationManager::the().navigation_requires_process_swap(current_url, target.target_top_level_entry->url);
    auto pending_traversal = PendingSessionHistoryTraversal {
        .target_step = target.target_step,
        .target_step_index = target.target_step_index,
        .will_change_top_level_entry = target.changes_top_level_entry,
        .will_replace_web_content_process = will_replace_web_content_process,
        .on_cancelation_check_complete = nullptr,
        .on_top_level_traversal_applied = move(on_top_level_traversal_applied),
    };

    if (web_content_can_apply_traversal(target) && !will_replace_web_content_process) {
        // NB: web_content_can_apply_history_traversal mirrors this condition for requested traversals.
        m_pending_session_history_traversal = move(pending_traversal);
        auto webdriver_pending_navigation_completes_with_session_history_update = false;
        if (auto const* current_entry = m_session_history.current_entry()) {
            webdriver_pending_navigation_completes_with_session_history_update = current_entry->document_state.id == target.target_top_level_entry->document_state.id;
        }
        if (!webdriver_pending_navigation_completes_with_session_history_update)
            m_pending_session_history_traversal->on_top_level_traversal_applied = nullptr;
        return {
            .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target.changes_top_level_entry },
            .action = HistoryTraversalAction::TraverseInWebContent,
            .target_step = target.target_step,
            .webdriver_pending_navigation_url = target.target_top_level_entry->url,
            .webdriver_pending_navigation_completes_with_session_history_update = webdriver_pending_navigation_completes_with_session_history_update,
        };
    }

    auto needs_cancelation_check = check_for_cancelation == CheckForCancelation::Yes
        || (check_for_cancelation == CheckForCancelation::IfWebContentCannotTraverseTarget && !web_content_can_apply_traversal(target));
    if (needs_cancelation_check) {
        pending_traversal.stage = PendingSessionHistoryTraversal::Stage::CheckingCancelation;
        pending_traversal.cancelation_check_request_id = m_next_traverse_history_step_cancelation_check_request_id++;
        pending_traversal.on_cancelation_check_complete = move(on_cancelation_check_complete);
        auto request_id = pending_traversal.cancelation_check_request_id;
        m_pending_session_history_traversal = move(pending_traversal);
        return {
            .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target.changes_top_level_entry, .waiting_for_cancelation_check = true },
            .action = HistoryTraversalAction::CheckForCancelation,
            .target_step = target.target_step,
            .cancelation_check_request_id = request_id,
        };
    }

    pending_traversal.stage = PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess;
    m_pending_session_history_traversal = move(pending_traversal);
    prepare_to_load_session_history_traversal_target_from_ui_process(target, current_url);
    return {
        .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target.changes_top_level_entry },
        .action = HistoryTraversalAction::LoadCurrentEntryFromUIProcess,
        .webdriver_pending_navigation_url = target.target_top_level_entry->url,
        .webdriver_pending_navigation_completes_with_session_history_update = true,
    };
}

bool CanonicalTraversable::notify_top_level_traversal_applied()
{
    if (!m_pending_session_history_traversal.has_value())
        return false;

    auto callback = move(m_pending_session_history_traversal->on_top_level_traversal_applied);
    if (!callback)
        return false;

    callback();
    return true;
}

URL::URL CanonicalTraversable::prepare_to_load_session_history_traversal_target_from_ui_process(TraversableSessionHistory::TraversalTarget const& target, URL::URL const& current_url)
{
    if (!m_pending_session_history_traversal.has_value() || m_pending_session_history_traversal->target_step != target.target_step) {
        m_pending_session_history_traversal = PendingSessionHistoryTraversal {
            .target_step = target.target_step,
            .target_step_index = target.target_step_index,
            .will_change_top_level_entry = target.changes_top_level_entry,
            .will_replace_web_content_process = SiteIsolationManager::the().navigation_requires_process_swap(current_url, target.target_top_level_entry->url),
            .stage = PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess,
            .on_cancelation_check_complete = nullptr,
            .on_top_level_traversal_applied = nullptr,
        };
    } else {
        m_pending_session_history_traversal->stage = PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess;
    }

    auto target_url = target.target_top_level_entry->url;
    auto previous_session_history = m_session_history;
    m_session_history.traverse_to(target.target_step_index);
    prepare_to_seed_web_content_session_history_from_ui_process();
    m_pending_session_history_navigation = PendingSessionHistoryNavigation { target_url, move(previous_session_history) };
    return target_url;
}

WebContentHistoryStepResult CanonicalTraversable::did_traverse_the_history_to_step(i32 step, bool step_was_available, Web::HTML::HistoryStepResult result)
{
    if (!m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()) {
        if (!m_pending_session_history_traversal.has_value() || m_pending_session_history_traversal->target_step != step)
            return { .dump_reason = "ignored-stale-webcontent-history-step-result"sv };

        if (!step_was_available) {
            auto target = m_session_history.traversal_target_for_step(step);
            if (target.has_value())
                return { .dump_reason = "webcontent-history-step-unavailable-fallback-load"sv, .fallback_target = *target };
            m_current_web_content_session_history_matches_mirror = false;
            m_session_history.forget_web_content_state();
            m_pending_session_history_traversal.clear();
            return { .dump_reason = "webcontent-history-step-unavailable"sv, .should_update_navigation_action_state = true };
        }

        if (result != Web::HTML::HistoryStepResult::Applied) {
            m_pending_session_history_traversal.clear();
            return { .dump_reason = "webcontent-history-step-canceled"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_update_webdriver_pending_navigation_to_current_url = true, .should_reset_webdriver_pending_navigation_completion = true };
        }

        if (!m_session_history.did_apply_web_content_traversal_to_step(step)) {
            if (auto target = m_session_history.traversal_target_for_step(step); target.has_value())
                return { .dump_reason = "webcontent-history-step-applied-with-stale-mirror-fallback-load"sv, .fallback_target = *target };
            m_current_web_content_session_history_matches_mirror = false;
            m_session_history.forget_web_content_state();
            m_pending_session_history_traversal.clear();
            return { .dump_reason = "webcontent-history-step-applied-without-ui-target"sv, .should_update_navigation_action_state = true };
        }

        m_current_web_content_session_history_matches_mirror = true;
        auto did_notify_top_level_traversal_applied = notify_top_level_traversal_applied();
        auto should_complete_webdriver_pending_navigation = !did_notify_top_level_traversal_applied
            && !m_pending_session_history_traversal->will_change_top_level_entry;
        Optional<URL::URL> current_url;
        if (auto const* current_entry = m_session_history.current_entry())
            current_url = current_entry->url;
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "webcontent-history-step-applied"sv, .should_update_navigation_action_state = true, .current_url = move(current_url), .should_complete_webdriver_pending_navigation = should_complete_webdriver_pending_navigation };
    }

    if (*m_pending_web_content_session_history_seed.step_after_loading_top_level_entry != step)
        return { .dump_reason = "ignored-stale-webcontent-history-step-result"sv };

    if (step_was_available && result == Web::HTML::HistoryStepResult::Applied) {
        m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.clear();
        m_pending_web_content_session_history_seed.navigables_to_restore.clear();
        m_current_web_content_session_history_matches_mirror = m_session_history.did_restore_web_content_to_current_step(step);
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "webcontent-history-step-restored"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true };
    }

    auto pending_step_dump_reason = step_was_available ? "webcontent-pending-history-step-canceled"sv : "webcontent-history-step-unavailable"sv;
    if (m_pending_session_history_navigation.has_value())
        return { .dump_reason = pending_step_dump_reason, .should_restore_pending_navigation = true };

    m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.clear();
    m_pending_web_content_session_history_seed.navigables_to_restore.clear();
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
    m_pending_session_history_traversal.clear();
    return { .dump_reason = pending_step_dump_reason, .should_update_navigation_action_state = true };
}

HistoryStepCancelationCheckResult CanonicalTraversable::did_check_if_traverse_history_step_is_canceled(u64 request_id, i32 step, Web::HTML::HistoryStepResult result)
{
    if (!m_pending_session_history_traversal.has_value()
        || m_pending_session_history_traversal->stage != PendingSessionHistoryTraversal::Stage::CheckingCancelation
        || m_pending_session_history_traversal->cancelation_check_request_id != request_id
        || m_pending_session_history_traversal->target_step != step)
        return { .dump_reason = "ignored-stale-history-step-cancelation-check-result"sv };

    if (result == Web::HTML::HistoryStepResult::CanceledPendingNavigation) {
        auto target = m_session_history.traversal_target_for_step(step);
        auto const* previous_current_entry = m_pending_session_history_navigation.has_value()
            ? m_pending_session_history_navigation->previous_session_history.current_entry()
            : nullptr;
        // INTEROP: WebContent handled this browser UI traversal as stop loading rather than applying a history
        //          step. If it preserved the active document, discard the UI process's uncommitted speculative
        //          entry so both processes continue to expose that document as the current history entry.
        if (target.has_value()
            && previous_current_entry
            && m_pending_session_history_navigation->web_content_restore_mode == PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState
            && target->target_top_level_entry->document_state.id == previous_current_entry->document_state.id) {
            auto on_cancelation_check_complete = move(m_pending_session_history_traversal->on_cancelation_check_complete);
            m_pending_session_history_traversal.clear();
            return {
                .dump_reason = "traverse-canceled-pending-navigation"sv,
                .on_cancelation_check_complete = move(on_cancelation_check_complete),
                .outcome = { .status = HistoryTraversalStatus::Started },
                .should_restore_pending_navigation = true,
            };
        }
        result = Web::HTML::HistoryStepResult::Applied;
    }

    if (result != Web::HTML::HistoryStepResult::Applied) {
        auto on_cancelation_check_complete = move(m_pending_session_history_traversal->on_cancelation_check_complete);
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "traverse-fallback-canceled-by-webcontent"sv, .on_cancelation_check_complete = move(on_cancelation_check_complete), .outcome = { .status = HistoryTraversalStatus::Canceled }, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_update_webdriver_pending_navigation_to_current_url = true, .should_reset_webdriver_pending_navigation_completion = true };
    }

    auto target = m_session_history.traversal_target_for_step(step);
    if (!target.has_value()) {
        auto on_cancelation_check_complete = move(m_pending_session_history_traversal->on_cancelation_check_complete);
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "traverse-fallback-cancelation-check-without-ui-target"sv, .on_cancelation_check_complete = move(on_cancelation_check_complete), .outcome = { .status = HistoryTraversalStatus::NoEntry }, .should_update_navigation_action_state = true };
    }

    auto on_cancelation_check_complete = move(m_pending_session_history_traversal->on_cancelation_check_complete);
    return {
        .dump_reason = "traverse-fallback-load-after-cancelation-check"sv,
        .on_cancelation_check_complete = move(on_cancelation_check_complete),
        .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = m_pending_session_history_traversal->will_replace_web_content_process, .will_change_top_level_entry = m_pending_session_history_traversal->will_change_top_level_entry },
        .target = *target,
    };
}

Optional<WebContentSessionHistorySeed> CanonicalTraversable::prepare_web_content_session_history_seed(bool allow_current_entry_reconstruction)
{
    auto current_top_level_entry_index = m_session_history.current_top_level_entry_index();
    if (!current_top_level_entry_index.has_value()) {
        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        return {};
    }

    auto entries = m_session_history.entries();
    if (entries.is_empty()) {
        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        return {};
    }

    auto is_restoring_traversal_target = m_pending_session_history_traversal.has_value()
        && (m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess
            || m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess
            || m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::RestoringNestedStepAfterSeed);
    auto allow_reconstructing_current_entry = is_restoring_traversal_target
        || m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()
        || allow_current_entry_reconstruction;

    return WebContentSessionHistorySeed {
        .entries = move(entries),
        .current_top_level_entry_index = *current_top_level_entry_index,
        .allow_current_entry_reconstruction = allow_reconstructing_current_entry,
    };
}

void CanonicalTraversable::did_send_web_content_session_history_seed()
{
    m_pending_web_content_session_history_seed.waiting_for_ack = true;
    m_pending_web_content_session_history_seed.should_send_entries = false;
}

bool CanonicalTraversable::prepare_to_restore_current_session_history_entry_from_ui_process()
{
    auto should_seed = !m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value();
    if (should_seed)
        m_pending_web_content_session_history_seed.should_reseed_after_current_history_load = true;
    return should_seed;
}

CurrentSessionHistoryEntryLoad CanonicalTraversable::prepare_current_session_history_entry_load(URL::URL const& current_url)
{
    auto const* current_entry = m_session_history.current_entry();
    if (!current_entry) {
        m_session_history_entry_url_loading_from_ui_process = current_url;
        return { .url = current_url, .document_resource = Empty {}, .history_handling = Web::Bindings::NavigationHistoryBehavior::Auto };
    }

    m_session_history_entry_url_loading_from_ui_process = current_entry->url;
    auto history_handling = m_pending_web_content_session_history_seed.waiting_for_ack || m_session_history.web_content_uses_ui_step_coordinates()
        ? Web::Bindings::NavigationHistoryBehavior::Replace
        : Web::Bindings::NavigationHistoryBehavior::Auto;
    return { .url = current_entry->url, .document_resource = current_entry->document_state.resource, .history_handling = history_handling };
}

void CanonicalTraversable::did_crash_requiring_web_content_session_history_seed()
{
    // NB: Replies from the crashed process will never arrive; complete the in-flight operations so the traversal
    //     queue can serve the recovered process.
    abandon_history_operations();
    m_session_history_entry_url_loading_from_ui_process.clear();
    prepare_to_seed_web_content_session_history_from_ui_process();
}

void CanonicalTraversable::reset_session_history_for_testing()
{
    abandon_history_operations();
    m_session_history.clear();
    m_current_web_content_session_history_matches_mirror = false;
    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal.clear();
    m_session_history_entry_url_loading_from_ui_process.clear();
    abandon_pending_web_content_session_history_seed();
}

void CanonicalTraversable::mark_web_content_session_history_stale_for_testing()
{
    m_current_web_content_session_history_matches_mirror = false;
}

StringView CanonicalTraversable::pending_session_history_navigation_web_content_restore_mode_to_string(PendingSessionHistoryNavigation::WebContentRestoreMode mode)
{
    switch (mode) {
    case PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState:
        return "preserve-current-process-state"sv;
    case PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess:
        return "restore-from-ui-process"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView CanonicalTraversable::pending_session_history_traversal_stage_to_string(PendingSessionHistoryTraversal::Stage stage)
{
    switch (stage) {
    case PendingSessionHistoryTraversal::Stage::ApplyingInWebContent:
        return "applying-in-webcontent"sv;
    case PendingSessionHistoryTraversal::Stage::CheckingCancelation:
        return "checking-cancelation"sv;
    case PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess:
        return "loading-entry-from-ui-process"sv;
    case PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess:
        return "replacing-webcontent-process"sv;
    case PendingSessionHistoryTraversal::Stage::RestoringNestedStepAfterSeed:
        return "restoring-nested-step-after-seed"sv;
    }
    VERIFY_NOT_REACHED();
}

struct CanonicalTraversable::HistoryOperation {
    using Parameters = Variant<Web::HistoryOperationParameters, BrowserHistoryTraversalOperation, HistoryStepCancelationCheckOperation>;

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
    // Delta and Navigation API traversals resolve their canonical target when their queue position is reached.
    Optional<i32> resolved_step;
    // A push operation claims its new entry's step before apply-the-history-step commits it. Keeping that claim on
    // the operation makes its lifetime match the operation automatically, including cancellation and abandonment.
    Optional<i32> claimed_step;
    // A replacement process can finalize an entry while its canonical history seed is still pending. Preserve the
    // entry and the process's current step so this operation can apply what that process actually displays.
    Optional<Web::HTML::SessionHistoryEntryDescriptor> finalized_entry;
    bool update_canonical_current_step { true };
    Optional<i32> current_step;
    Vector<Web::HTML::CrossProcessId> navigables_to_restore;
    Function<void(ApplyHistoryStepJobs::InitiatorSandboxingCheckResult)> pending_sandboxing_check;
    Function<void(Web::HTML::HistoryStepResult)> pending_unload_cancelation;
    HashMap<Web::HTML::CrossProcessId, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)>> pending_changing_jobs;
    HashMap<Web::HTML::CrossProcessId, Function<void()>> pending_continuations;
    HashMap<Web::HTML::CrossProcessId, Function<void()>> pending_nonchanging_updates;
    OwnPtr<ApplyHistoryStep> algorithm;
    RefPtr<Core::Promise<Empty>> queue_promise;
};

CanonicalTraversable::~CanonicalTraversable() = default;

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

Optional<i32> CanonicalTraversable::maximum_claimed_session_history_step() const
{
    Optional<i32> maximum_claimed_step;
    for (auto const& operation : m_history_operations) {
        if (!operation.value->claimed_step.has_value())
            continue;
        if (!maximum_claimed_step.has_value() || *operation.value->claimed_step > *maximum_claimed_step)
            maximum_claimed_step = operation.value->claimed_step;
    }
    return maximum_claimed_step;
}

Optional<i32> CanonicalTraversable::claim_step_for_pending_cross_document_history_operation(Web::HTML::CrossProcessId navigable_id, i32 claimed_step)
{
    for (auto& operation : m_history_operations) {
        if (!operation.value->parameters.has<Web::HistoryOperationParameters>() || operation.value->algorithm)
            continue;
        auto const& request = operation.value->parameters.get<Web::HistoryOperationParameters>();
        if (!request.has<Web::PushHistoryOperationParameters>() || request.get<Web::PushHistoryOperationParameters>().navigable_id != navigable_id)
            continue;

        operation.value->claimed_step = claimed_step;
        return maximum_claimed_session_history_step();
    }
    return {};
}

void CanonicalTraversable::claim_step_for_pending_same_document_history_operation(Web::HTML::CrossProcessId navigable_id, i32 claimed_step)
{
    for (auto& operation : m_history_operations) {
        if (!operation.value->parameters.has<Web::HistoryOperationParameters>() || operation.value->algorithm)
            continue;
        auto const& request = operation.value->parameters.get<Web::HistoryOperationParameters>();
        if (!request.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>())
            continue;
        auto const& parameters = request.get<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>();
        if (parameters.navigable_id != navigable_id || parameters.history_handling != Web::HTML::HistoryHandlingBehavior::Push)
            continue;

        operation.value->claimed_step = claimed_step;
        return;
    }
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
        .run_unload_cancelation_job = [this, operation_id](i32 target_step, Vector<Web::HTML::CrossProcessId> navigables_crossing_documents, Web::HTML::UserNavigationInvolvement user_involvement, Function<void(Web::HTML::HistoryStepResult)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            VERIFY(operation->initiating_client);
            operation->pending_unload_cancelation = move(on_complete);
            operation->initiating_client->async_run_history_step_unload_cancelation_job(operation->initiating_page_id, operation_id, target_step, move(navigables_crossing_documents), user_involvement); },
        .run_changing_navigable_history_step_job = [this, operation_id](ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable = find(job.navigable_id);
            auto endpoint = navigable.has_value() ? history_job_endpoint_for(*navigable) : HistoryJobEndpoint {};
            if (!endpoint.client) {
                on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);
                return;
            }
            auto navigable_id = job.navigable_id;
            operation->pending_changing_jobs.set(navigable_id, move(on_complete));
            endpoint.client->async_run_changing_navigable_history_job(endpoint.page_id, operation_id, navigable_id, job.target_step, move(job.target_entry), job.user_involvement, job.navigation_type, job.synchronous_navigation == Web::HTML::SynchronousNavigation::Yes, operation->initiation_id); },
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
            operation->pending_nonchanging_updates.set(navigable_id, move(on_complete));
            endpoint.client->async_update_nonchanging_navigable_history_state(endpoint.page_id, operation_id, navigable_id,
                history_object_length_and_index.script_history_length, history_object_length_and_index.script_history_index); },
    };
}

void CanonicalTraversable::record_finalized_entry_for_pending_history_operation(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor history_entry)
{
    for (auto& operation : m_history_operations) {
        if (!operation.value->parameters.has<Web::HistoryOperationParameters>())
            continue;
        auto const& request = operation.value->parameters.get<Web::HistoryOperationParameters>();
        Optional<Web::HTML::CrossProcessId> finalized_navigable_id;
        if (request.has<Web::PushHistoryOperationParameters>())
            finalized_navigable_id = request.get<Web::PushHistoryOperationParameters>().navigable_id;
        else if (request.has<Web::ReplaceHistoryOperationParameters>())
            finalized_navigable_id = request.get<Web::ReplaceHistoryOperationParameters>().navigable_id;

        if (!operation.value->algorithm && finalized_navigable_id == navigable_id) {
            operation.value->current_step = history_entry.step;
            operation.value->finalized_entry = move(history_entry);
            operation.value->update_canonical_current_step = false;
            if (navigable_id != id() && !m_pending_web_content_session_history_seed.navigables_to_restore.contains_slow(navigable_id))
                m_pending_web_content_session_history_seed.navigables_to_restore.append(navigable_id);
            return;
        }
    }
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

void CanonicalTraversable::run_history_operation_at_queue_position(u64 initiation_id, HistoryStepCancelationCheckOperation parameters, WebContentClient& requesting_client, u64 requesting_page_id, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = m_next_history_operation_id++;
    m_history_operations.set(operation_id, make<HistoryOperation>(operation_id, HistoryOperation::Parameters { move(parameters) }, initiation_id, &requesting_client, requesting_page_id, Optional<i32> {}, move(on_complete)));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    start_history_operation(*operation, promise);
}

void CanonicalTraversable::run_ui_history_operation_at_queue_position(Variant<BrowserHistoryTraversalOperation, HistoryStepCancelationCheckOperation> parameters, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = m_next_history_operation_id++;
    HistoryOperation::Parameters operation_parameters = parameters.visit(
        [](BrowserHistoryTraversalOperation& parameters) -> HistoryOperation::Parameters { return move(parameters); },
        [](HistoryStepCancelationCheckOperation& parameters) -> HistoryOperation::Parameters { return move(parameters); });
    m_history_operations.set(operation_id, make<HistoryOperation>(operation_id, move(operation_parameters), Optional<u64> {}, nullptr, 0, Optional<i32> {}, move(on_complete)));
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
    Optional<Web::HTML::CrossProcessId> synchronous_navigation_target;
    if (request.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>())
        synchronous_navigation_target = request.get<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>().navigable_id;

    NonnullRefPtr<WebContentClient> requesting_client_ref { requesting_client };
    auto steps = [this, initiation_id, request = move(request), requesting_client = move(requesting_client_ref), requesting_page_id, on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_history_operation_at_queue_position(initiation_id, move(request), *requesting_client, requesting_page_id, {}, move(on_complete), move(promise));
    };

    if (synchronous_navigation_target.has_value())
        m_history_traversal_queue.append_session_history_synchronous_navigation_steps(*synchronous_navigation_target, move(steps));
    else
        m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::enqueue_ui_history_operation(Variant<BrowserHistoryTraversalOperation, HistoryStepCancelationCheckOperation> parameters, OnHistoryOperationComplete on_complete)
{
    auto steps = [this, parameters = move(parameters), on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_ui_history_operation_at_queue_position(move(parameters), move(on_complete), move(promise));
    };
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::enqueue_history_operation(BrowserHistoryTraversalOperation parameters, OnHistoryOperationComplete on_complete)
{
    enqueue_ui_history_operation(move(parameters), move(on_complete));
}

void CanonicalTraversable::enqueue_history_operation(HistoryStepCancelationCheckOperation parameters, OnHistoryOperationComplete on_complete)
{
    enqueue_ui_history_operation(move(parameters), move(on_complete));
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
        },
        move(operation.finalized_entry), operation.update_canonical_current_step, operation.current_step,
        move(operation.navigables_to_restore));
    operation.algorithm->apply_the_history_step();
}

void CanonicalTraversable::check_history_step_cancelation(HistoryOperation& operation, HistoryStepCancelationCheckOperation const& parameters)
{
    auto operation_id = operation.operation_id;
    auto run_unload_cancelation_job = [this, operation_id] {
        auto* operation = find_history_operation(operation_id);
        if (!operation)
            return;
        auto const& parameters = operation->parameters.get<HistoryStepCancelationCheckOperation>();
        auto navigables_crossing_documents = m_session_history.get_all_navigables_that_might_experience_a_cross_document_traversal(*this, parameters.target_step);
        auto jobs = create_apply_history_step_jobs(operation_id);
        jobs.run_unload_cancelation_job(parameters.target_step, move(navigables_crossing_documents), parameters.user_involvement,
            [this, operation_id](Web::HTML::HistoryStepResult result) {
                finish_history_operation(operation_id, result, {});
            });
    };

    if (!parameters.initiator_to_check.has_value()) {
        run_unload_cancelation_job();
        return;
    }

    VERIFY(operation.initiation_id.has_value());
    // AD-HOC: ApplyHistoryStep normally derives this set while preparing its changing-navigable jobs. A
    // cancelation-only operation stops before those jobs, so derive the same target set here for the spec's
    // initiator sandboxing check.
    auto navigables = m_session_history.get_all_navigables_whose_current_session_history_entry_will_change_or_reload(*this, parameters.target_step);
    auto jobs = create_apply_history_step_jobs(operation_id);
    jobs.run_initiator_sandboxing_check_job(*parameters.initiator_to_check, move(navigables),
        [this, operation_id, run_unload_cancelation_job](ApplyHistoryStepJobs::InitiatorSandboxingCheckResult result) {
            if (result == ApplyHistoryStepJobs::InitiatorSandboxingCheckResult::Disallowed) {
                finish_history_operation(operation_id, Web::HTML::HistoryStepResult::InitiatorDisallowed, {});
                return;
            }
            run_unload_cancelation_job();
        });
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

    operation.parameters.visit(
        [&](Web::HistoryOperationParameters const&) {
            VERIFY(operation.initiation_id.has_value());
            operation.initiating_client->async_history_operation_started(operation.initiating_page_id, operation.operation_id, *operation.initiation_id);
        },
        [&](BrowserHistoryTraversalOperation& parameters) {
            operation.current_step = parameters.current_step;
            operation.navigables_to_restore = move(parameters.navigables_to_restore);
            apply_history_step(operation, parameters.target_step, true, {}, Web::HTML::UserNavigationInvolvement::BrowserUI, Web::Bindings::NavigationType::Traverse, Web::HTML::SynchronousNavigation::No, {});
        },
        [&](HistoryStepCancelationCheckOperation const& parameters) {
            check_history_step_cancelation(operation, parameters);
        });
}

void CanonicalTraversable::did_receive_history_operation_ready(u64 operation_id, bool proceed, Optional<i32> step_override, Web::HTML::HistoryStepResult abandon_result)
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
    operation->parameters.get<Web::HistoryOperationParameters>().visit(
        [&](Web::PushHistoryOperationParameters const& parameters) {
            VERIFY(step_override.has_value());
            apply_history_step(*operation, *step_override, false, {}, parameters.user_involvement, Web::Bindings::NavigationType::Push,
                Web::HTML::SynchronousNavigation::No, parameters.navigable_id);
        },
        [&](Web::ReplaceHistoryOperationParameters const& parameters) {
            VERIFY(step_override.has_value());
            apply_history_step(*operation, *step_override, false, {}, parameters.user_involvement, Web::Bindings::NavigationType::Replace,
                Web::HTML::SynchronousNavigation::No, parameters.navigable_id);
        },
        [&](Web::ReloadHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            apply_current_step(true, parameters.user_involvement, Web::Bindings::NavigationType::Reload, parameters.navigable_id);
        },
        [&](Web::TraverseByDeltaHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            VERIFY(operation->resolved_step.has_value());
            apply_history_step(*operation, *operation->resolved_step, true, parameters.initiator_to_check, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::TraverseToStepHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            apply_history_step(*operation, parameters.target_step, true, {}, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::NavigationAPITraverseHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            VERIFY(operation->resolved_step.has_value());
            apply_history_step(*operation, *operation->resolved_step, true, parameters.navigable_id, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::ResumeTraverseHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            apply_history_step(*operation, parameters.target_step, false, {}, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::NavigableCreationHistoryOperationParameters const&) {
            VERIFY(!step_override.has_value());
            apply_current_step(false, Web::HTML::UserNavigationInvolvement::None, {}, {});
        },
        [&](Web::NavigableDestructionHistoryOperationParameters const&) {
            VERIFY(!step_override.has_value());
            apply_current_step(false, Web::HTML::UserNavigationInvolvement::None, {}, {});
        },
        [&](Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& parameters) {
            VERIFY(step_override.has_value());
            VERIFY(parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Push || parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace);
            auto navigation_type = parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace
                ? Web::Bindings::NavigationType::Replace
                : Web::Bindings::NavigationType::Push;
            apply_history_step(*operation, *step_override, false, {}, parameters.user_involvement, navigation_type,
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

    if (taken_operation.initiating_client)
        taken_operation.initiating_client->async_complete_history_operation(taken_operation.initiating_page_id, operation_id, result, committed_step, taken_operation.initiation_id);

    // The completion installs the committed step as WebContent's current session history step; record that in the
    // mirror bookkeeping, which used to learn it from a WebContent notification.
    if (committed_step.has_value() && !m_pending_web_content_session_history_seed.ignore_updates_until_seed) {
        if (m_session_history.did_set_web_content_current_session_history_step(*committed_step))
            m_current_web_content_session_history_matches_mirror = m_session_history.web_content_history_matches_mirror();
    }

    if (taken_operation.on_complete)
        taken_operation.on_complete(result, committed_step);

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

void CanonicalTraversable::did_receive_changing_navigable_continuation_applied(u64 operation_id, Web::HTML::CrossProcessId navigable_id)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (auto pending = operation->pending_continuations.take(navigable_id); pending.has_value()) {
            // A replacement document's creation operation also applies a top-level continuation, but it runs before
            // the document has accepted the UI-owned history state. Only the browser traversal itself reaches the
            // observable top-level completion point here.
            auto is_top_level_pending_browser_history_traversal = navigable_id == id()
                && operation->parameters.has<BrowserHistoryTraversalOperation>()
                && m_pending_session_history_traversal.has_value()
                && operation->parameters.get<BrowserHistoryTraversalOperation>().target_step == m_pending_session_history_traversal->target_step;
            if (is_top_level_pending_browser_history_traversal)
                notify_top_level_traversal_applied();
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
