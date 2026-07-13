/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <LibWebView/SessionHistory.h>

namespace WebView {

static bool steps_are_valid(Vector<i32> const& steps)
{
    Optional<i32> previous_step;
    for (auto const& step : steps) {
        if (step < 0)
            return false;
        if (previous_step.has_value() && step <= *previous_step)
            return false;
        previous_step = step;
    }
    return true;
}

static bool entries_are_valid(Vector<TraversableSessionHistory::Entry> const& entries)
{
    Optional<i32> previous_step;
    for (auto const& entry : entries) {
        if (entry.step < 0)
            return false;
        if (previous_step.has_value() && entry.step <= *previous_step)
            return false;
        for (auto const& nested_history : entry.document_state.nested_histories) {
            if (!entries_are_valid(nested_history.entries))
                return false;
        }
        previous_step = entry.step;
    }
    return true;
}

struct SessionHistoryEntryMutationResult {
    bool found { false };
    bool rejected { false };
};

struct SameDocumentNavigationMutationResult {
    Vector<TraversableSessionHistory::Entry> entries;
    Vector<i32> used_steps;
    size_t current_used_step_index { 0 };
};

static bool entries_and_used_steps_are_consistent(Vector<TraversableSessionHistory::Entry> const&, Vector<i32> const&);
static Optional<size_t> top_level_entry_index_for_step(Vector<TraversableSessionHistory::Entry> const&, i32 step);
static void recompute_used_steps(Vector<TraversableSessionHistory::Entry> const&, Vector<i32>& used_steps, Optional<size_t>& current_used_step_index, i32 current_step);
static void clear_forward_session_history_entries(Vector<TraversableSessionHistory::Entry>&, i32 step);

static bool is_supported_current_entry_update_kind(Web::HTML::SessionHistoryEntryUpdateKind update_kind)
{
    switch (update_kind) {
    case Web::HTML::SessionHistoryEntryUpdateKind::NavigationAPIState:
    case Web::HTML::SessionHistoryEntryUpdateKind::ScrollRestorationMode:
    case Web::HTML::SessionHistoryEntryUpdateKind::ScrollPositionData:
    case Web::HTML::SessionHistoryEntryUpdateKind::DocumentStateReloadPending:
    case Web::HTML::SessionHistoryEntryUpdateKind::DocumentStatePopulation:
        return true;
    }
    return false;
}

static void apply_document_state_population_update(Web::HTML::SessionHistoryDocumentStateDescriptor& document_state, Web::HTML::SessionHistoryDocumentStateDescriptor const& updated_document_state)
{
    document_state.history_policy_container = updated_document_state.history_policy_container;
    document_state.request_referrer = updated_document_state.request_referrer;
    document_state.origin = updated_document_state.origin;
    document_state.resource = updated_document_state.resource;
    document_state.ever_populated = updated_document_state.ever_populated;
}

static bool entry_matches_ignoring_targeted_field(TraversableSessionHistory::Entry const& stored_entry, TraversableSessionHistory::Entry const& updated_entry, Web::HTML::SessionHistoryEntryUpdateKind update_kind)
{
    auto expected_entry = updated_entry;
    switch (update_kind) {
    case Web::HTML::SessionHistoryEntryUpdateKind::NavigationAPIState:
        expected_entry.navigation_api_state = stored_entry.navigation_api_state;
        break;
    case Web::HTML::SessionHistoryEntryUpdateKind::ScrollRestorationMode:
        expected_entry.scroll_restoration_mode = stored_entry.scroll_restoration_mode;
        break;
    case Web::HTML::SessionHistoryEntryUpdateKind::ScrollPositionData:
        expected_entry.scroll_position_data = stored_entry.scroll_position_data;
        break;
    case Web::HTML::SessionHistoryEntryUpdateKind::DocumentStateReloadPending:
        expected_entry.document_state.reload_pending = stored_entry.document_state.reload_pending;
        break;
    case Web::HTML::SessionHistoryEntryUpdateKind::DocumentStatePopulation:
        apply_document_state_population_update(expected_entry.document_state, stored_entry.document_state);
        break;
    }
    return Web::HTML::session_history_entry_descriptors_match(stored_entry, expected_entry);
}

static bool entry_has_target_identity(TraversableSessionHistory::Entry const& entry, TraversableSessionHistory::Entry const& updated_entry)
{
    return entry.step == updated_entry.step && entry.document_state.id == updated_entry.document_state.id;
}

static bool entry_has_target_document_state_identity(TraversableSessionHistory::Entry const& entry, TraversableSessionHistory::Entry const& updated_entry)
{
    return entry.document_state.id == updated_entry.document_state.id;
}

static bool entry_has_target_step_identity(TraversableSessionHistory::Entry const& entry, TraversableSessionHistory::Entry const& updated_entry)
{
    return entry.step == updated_entry.step && entry.url == updated_entry.url;
}

static bool entry_should_receive_targeted_current_entry_update(TraversableSessionHistory::Entry const& entry, TraversableSessionHistory::Entry const& updated_entry, Web::HTML::SessionHistoryEntryUpdateKind update_kind)
{
    if (update_kind == Web::HTML::SessionHistoryEntryUpdateKind::DocumentStateReloadPending
        || update_kind == Web::HTML::SessionHistoryEntryUpdateKind::DocumentStatePopulation) {
        return entry_has_target_document_state_identity(entry, updated_entry);
    }
    if (update_kind == Web::HTML::SessionHistoryEntryUpdateKind::ScrollPositionData)
        return entry_has_target_identity(entry, updated_entry) || entry_has_target_step_identity(entry, updated_entry);
    return entry_has_target_identity(entry, updated_entry);
}

static void apply_targeted_current_entry_update(TraversableSessionHistory::Entry& entry, TraversableSessionHistory::Entry const& updated_entry, Web::HTML::SessionHistoryEntryUpdateKind update_kind)
{
    switch (update_kind) {
    case Web::HTML::SessionHistoryEntryUpdateKind::NavigationAPIState:
        entry.navigation_api_state = updated_entry.navigation_api_state;
        break;
    case Web::HTML::SessionHistoryEntryUpdateKind::ScrollRestorationMode:
        entry.scroll_restoration_mode = updated_entry.scroll_restoration_mode;
        break;
    case Web::HTML::SessionHistoryEntryUpdateKind::ScrollPositionData:
        entry.scroll_position_data = updated_entry.scroll_position_data;
        break;
    case Web::HTML::SessionHistoryEntryUpdateKind::DocumentStateReloadPending:
        entry.document_state.reload_pending = updated_entry.document_state.reload_pending;
        break;
    case Web::HTML::SessionHistoryEntryUpdateKind::DocumentStatePopulation:
        apply_document_state_population_update(entry.document_state, updated_entry.document_state);
        break;
    }
}

static SessionHistoryEntryMutationResult apply_targeted_current_entry_update(Vector<TraversableSessionHistory::Entry>& entries, TraversableSessionHistory::Entry const& updated_entry, Web::HTML::SessionHistoryEntryUpdateKind update_kind)
{
    SessionHistoryEntryMutationResult result;
    if (!is_supported_current_entry_update_kind(update_kind) || updated_entry.step < 0 || updated_entry.document_state.id.namespace_id == 0 || updated_entry.document_state.id.local_id == 0)
        return { .rejected = true };

    for (auto& entry : entries) {
        auto should_receive_update = entry_should_receive_targeted_current_entry_update(entry, updated_entry, update_kind);
        if (should_receive_update) {
            result.found = true;
            if (entry_has_target_identity(entry, updated_entry) && !entry_matches_ignoring_targeted_field(entry, updated_entry, update_kind)) {
                result.rejected = true;
                continue;
            }
            apply_targeted_current_entry_update(entry, updated_entry, update_kind);
        }

        for (auto& nested_history : entry.document_state.nested_histories) {
            auto nested_result = apply_targeted_current_entry_update(nested_history.entries, updated_entry, update_kind);
            result.found |= nested_result.found;
            result.rejected |= nested_result.rejected;
        }
    }

    return result;
}

static Optional<SameDocumentNavigationMutationResult> finish_current_entry_nested_history_mutation(Vector<TraversableSessionHistory::Entry> entries, i32 current_step)
{
    Vector<i32> used_steps;
    Optional<size_t> current_used_step_index;
    recompute_used_steps(entries, used_steps, current_used_step_index, current_step);
    if (!current_used_step_index.has_value())
        return {};

    if (!entries_are_valid(entries) || !steps_are_valid(used_steps) || !entries_and_used_steps_are_consistent(entries, used_steps))
        return {};

    return SameDocumentNavigationMutationResult {
        .entries = move(entries),
        .used_steps = move(used_steps),
        .current_used_step_index = *current_used_step_index,
    };
}

static Optional<SameDocumentNavigationMutationResult> apply_current_entry_nested_history_update(Vector<TraversableSessionHistory::Entry> entries, i32 previous_current_step, Web::HTML::CrossProcessId document_state_id, Web::HTML::SessionHistoryNestedHistoryDescriptor nested_history, i32 current_step)
{
    auto current_top_level_entry_index = top_level_entry_index_for_step(entries, previous_current_step);
    if (!current_top_level_entry_index.has_value())
        return {};

    if (entries[*current_top_level_entry_index].document_state.id != document_state_id)
        return {};

    if (nested_history.id.namespace_id == 0 || nested_history.id.local_id == 0)
        return {};

    auto& nested_histories = entries[*current_top_level_entry_index].document_state.nested_histories;
    auto existing_nested_history = nested_histories.find_if([&](auto const& existing) {
        return existing.id == nested_history.id;
    });
    if (existing_nested_history == nested_histories.end())
        nested_histories.append(move(nested_history));
    else
        *existing_nested_history = move(nested_history);

    return finish_current_entry_nested_history_mutation(move(entries), current_step);
}

static Optional<SameDocumentNavigationMutationResult> apply_current_entry_nested_history_removal(Vector<TraversableSessionHistory::Entry> entries, i32 previous_current_step, Web::HTML::CrossProcessId document_state_id, Web::HTML::CrossProcessId nested_history_id, i32 current_step)
{
    auto current_top_level_entry_index = top_level_entry_index_for_step(entries, previous_current_step);
    if (!current_top_level_entry_index.has_value())
        return {};

    if (entries[*current_top_level_entry_index].document_state.id != document_state_id)
        return {};

    if (nested_history_id.namespace_id == 0 || nested_history_id.local_id == 0)
        return {};

    auto& nested_histories = entries[*current_top_level_entry_index].document_state.nested_histories;
    if (!nested_histories.remove_all_matching([&](auto const& nested_history) {
            return nested_history.id == nested_history_id;
        }))
        return {};

    return finish_current_entry_nested_history_mutation(move(entries), current_step);
}

static Optional<SameDocumentNavigationMutationResult> apply_top_level_same_document_navigation(Vector<TraversableSessionHistory::Entry> entries, Vector<i32> used_steps, i32 previous_current_step, TraversableSessionHistory::Entry const& entry, Optional<i32> replaced_step, i32 current_step)
{
    auto previous_current_used_step_index = used_steps.find_first_index(previous_current_step);
    if (!previous_current_used_step_index.has_value())
        return {};

    auto current_top_level_entry_index = top_level_entry_index_for_step(entries, previous_current_step);
    if (!current_top_level_entry_index.has_value())
        return {};

    if (replaced_step.has_value()) {
        if (*replaced_step < 0 || entry.step != *replaced_step || current_step != *replaced_step || previous_current_step != current_step)
            return {};
        if (entries[*current_top_level_entry_index].step != *replaced_step)
            return {};
        if (entries[*current_top_level_entry_index].document_state.id != entry.document_state.id)
            return {};
        entries[*current_top_level_entry_index] = entry;
    } else {
        if (current_step != entry.step || entry.step <= previous_current_step)
            return {};
        if (entries[*current_top_level_entry_index].document_state.id != entry.document_state.id)
            return {};

        entries.shrink(*current_top_level_entry_index + 1);
        used_steps.shrink(*previous_current_used_step_index + 1);
        if (used_steps.contains_slow(entry.step))
            return {};

        entries.append(entry);
        used_steps.append(current_step);
    }

    auto current_used_step_index = used_steps.find_first_index(current_step);
    if (!current_used_step_index.has_value())
        return {};

    if (!entries_are_valid(entries) || !steps_are_valid(used_steps) || !entries_and_used_steps_are_consistent(entries, used_steps))
        return {};

    return SameDocumentNavigationMutationResult {
        .entries = move(entries),
        .used_steps = move(used_steps),
        .current_used_step_index = *current_used_step_index,
    };
}

static bool steps_match(Vector<i32> const& a, Vector<i32> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i])
            continue;
        return false;
    }
    return true;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
static Vector<i32> get_all_used_history_steps(Vector<TraversableSessionHistory::Entry> const& traversable_session_history_entries)
{
    // 1. Assert: this is running within traversable's session history traversal queue.

    // 2. Let steps be an empty ordered set of non-negative integers.
    OrderedHashTable<i32> steps;

    // 3. Let entryLists be the ordered set « traversable's session history entries ».
    Vector<Vector<TraversableSessionHistory::Entry> const*> entry_lists { &traversable_session_history_entries };

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto const* entry_list = entry_lists.take_first();

        // 1. For each entry of entryList:
        for (auto const& entry : *entry_list) {
            // 1. Append entry's step to steps.
            steps.set(entry.step);

            // 2. For each nestedHistory of entry's document state's nested histories, append
            //    nestedHistory's entries list to entryLists.
            for (auto const& nested_history : entry.document_state.nested_histories)
                entry_lists.append(&nested_history.entries);
        }
    }

    // 5. Return steps, sorted.
    auto sorted_steps = steps.values();
    quick_sort(sorted_steps);
    return sorted_steps;
}

static bool entries_and_used_steps_are_consistent(Vector<TraversableSessionHistory::Entry> const& entries, Vector<i32> const& used_steps)
{
    return steps_match(get_all_used_history_steps(entries), used_steps);
}

static bool entries_have_nested_histories(Vector<TraversableSessionHistory::Entry> const& entries)
{
    for (auto const& entry : entries) {
        if (!entry.document_state.nested_histories.is_empty())
            return true;
    }
    return false;
}

static void recompute_used_steps(Vector<TraversableSessionHistory::Entry> const& entries, Vector<i32>& used_steps, Optional<size_t>& current_used_step_index, i32 current_step)
{
    used_steps = get_all_used_history_steps(entries);
    current_used_step_index = used_steps.find_first_index(current_step);
}

static Optional<size_t> top_level_entry_index_for_step(Vector<TraversableSessionHistory::Entry> const& entries, i32 step)
{
    Optional<size_t> result;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].step > step)
            break;
        result = i;
    }
    return result;
}

static TraversableSessionHistory::Entry const* entry_for_step_in_entry_list(Vector<TraversableSessionHistory::Entry> const& entries, i32 step)
{
    TraversableSessionHistory::Entry const* result = nullptr;
    for (auto const& entry : entries) {
        if (entry.step > step)
            break;
        result = &entry;
    }
    return result;
}

static bool nested_histories_need_restore_after_loading_entry(TraversableSessionHistory::Entry const& entry, i32 step)
{
    for (auto const& nested_history : entry.document_state.nested_histories) {
        auto target_entry = entry_for_step_in_entry_list(nested_history.entries, step);
        if (!target_entry)
            continue;
        if (target_entry->step != entry.step)
            return true;
        if (nested_histories_need_restore_after_loading_entry(*target_entry, step))
            return true;
    }
    return false;
}

static void clear_forward_session_history_entries(Vector<TraversableSessionHistory::Entry>& entries, i32 step)
{
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#clear-the-forward-session-history

    // 1. Assert: this is running within navigable's session history traversal queue.

    // 2. Let step be the navigable's current session history step.

    // 3. Let entryLists be the ordered set « navigable's session history entries ».
    Vector<Vector<TraversableSessionHistory::Entry>*> entry_lists { &entries };

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto* entry_list = entry_lists.take_first();

        // 1. Remove every session history entry from entryList that has a step greater than step.
        entry_list->remove_all_matching([step](auto const& entry) {
            return entry.step > step;
        });

        // 2. For each entry of entryList:
        for (auto& entry : *entry_list) {
            // 1. For each nestedHistory of entry's document state's nested histories, append
            //    nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry.document_state.nested_histories)
                entry_lists.append(&nested_history.entries);
        }
    }
}

static Web::HTML::SessionHistoryNestedHistoryDescriptor* nested_history_for_parent_document_state(Vector<TraversableSessionHistory::Entry>& entries, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id)
{
    for (auto& entry : entries) {
        if (entry.document_state.id == parent_document_state_id) {
            for (auto& nested_history : entry.document_state.nested_histories) {
                if (nested_history.id == navigable_id)
                    return &nested_history;
            }
            return nullptr;
        }

        for (auto& nested_history : entry.document_state.nested_histories) {
            if (auto* result = nested_history_for_parent_document_state(nested_history.entries, parent_document_state_id, navigable_id))
                return result;
        }
    }
    return nullptr;
}

static Optional<SameDocumentNavigationMutationResult> apply_nested_same_document_navigation(Vector<TraversableSessionHistory::Entry> entries, Vector<i32> used_steps, i32 previous_current_step, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, TraversableSessionHistory::Entry const& entry, Optional<i32> replaced_step, i32 current_step)
{
    auto previous_current_used_step_index = used_steps.find_first_index(previous_current_step);
    if (!previous_current_used_step_index.has_value())
        return {};

    auto* nested_history = nested_history_for_parent_document_state(entries, parent_document_state_id, navigable_id);
    if (!nested_history)
        return {};

    auto current_entry_index = top_level_entry_index_for_step(nested_history->entries, previous_current_step);
    if (!current_entry_index.has_value())
        return {};

    if (replaced_step.has_value()) {
        if (*replaced_step < 0 || entry.step != *replaced_step || current_step != *replaced_step || previous_current_step != current_step)
            return {};
        if (nested_history->entries[*current_entry_index].step != *replaced_step)
            return {};
        if (nested_history->entries[*current_entry_index].document_state.id != entry.document_state.id)
            return {};
        nested_history->entries[*current_entry_index] = entry;
    } else {
        if (current_step != entry.step || entry.step <= previous_current_step)
            return {};
        if (used_steps.contains_slow(entry.step))
            return {};
        if (nested_history->entries[*current_entry_index].document_state.id != entry.document_state.id)
            return {};

        clear_forward_session_history_entries(entries, previous_current_step);
        used_steps.shrink(*previous_current_used_step_index + 1);

        nested_history = nested_history_for_parent_document_state(entries, parent_document_state_id, navigable_id);
        if (!nested_history)
            return {};
        nested_history->entries.append(entry);
        used_steps.append(current_step);
    }

    auto current_used_step_index = used_steps.find_first_index(current_step);
    if (!current_used_step_index.has_value())
        return {};

    if (!entries_are_valid(entries) || !steps_are_valid(used_steps) || !entries_and_used_steps_are_consistent(entries, used_steps))
        return {};

    return SameDocumentNavigationMutationResult {
        .entries = move(entries),
        .used_steps = move(used_steps),
        .current_used_step_index = *current_used_step_index,
    };
}

static Optional<SameDocumentNavigationMutationResult> apply_nested_cross_document_navigation(Vector<TraversableSessionHistory::Entry> entries, Vector<i32> used_steps, i32 previous_current_step, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, TraversableSessionHistory::Entry const& entry, i32 current_step)
{
    auto previous_current_used_step_index = used_steps.find_first_index(previous_current_step);
    if (!previous_current_used_step_index.has_value())
        return {};

    auto* nested_history = nested_history_for_parent_document_state(entries, parent_document_state_id, navigable_id);
    if (!nested_history)
        return {};

    auto current_entry_index = top_level_entry_index_for_step(nested_history->entries, previous_current_step);
    if (!current_entry_index.has_value())
        return {};

    if (entry.step != current_step)
        return {};

    if (!entry.document_state.nested_histories.is_empty())
        return {};

    if (current_step == previous_current_step) {
        if (nested_history->entries[*current_entry_index].step != previous_current_step)
            return {};
        nested_history->entries[*current_entry_index] = entry;
    } else {
        if (current_step <= previous_current_step)
            return {};
        if (used_steps.contains_slow(current_step))
            return {};

        clear_forward_session_history_entries(entries, previous_current_step);
        used_steps.shrink(*previous_current_used_step_index + 1);

        nested_history = nested_history_for_parent_document_state(entries, parent_document_state_id, navigable_id);
        if (!nested_history)
            return {};
        nested_history->entries.append(entry);
        used_steps.append(current_step);
    }

    auto current_used_step_index = used_steps.find_first_index(current_step);
    if (!current_used_step_index.has_value())
        return {};

    if (!entries_are_valid(entries) || !steps_are_valid(used_steps) || !entries_and_used_steps_are_consistent(entries, used_steps))
        return {};

    return SameDocumentNavigationMutationResult {
        .entries = move(entries),
        .used_steps = move(used_steps),
        .current_used_step_index = *current_used_step_index,
    };
}

static Optional<SameDocumentNavigationMutationResult> apply_top_level_cross_document_navigation(Vector<TraversableSessionHistory::Entry> entries, Vector<i32> used_steps, i32 previous_current_step, TraversableSessionHistory::Entry const& entry, i32 current_step)
{
    auto previous_current_used_step_index = used_steps.find_first_index(previous_current_step);
    if (!previous_current_used_step_index.has_value())
        return {};

    auto previous_current_top_level_entry_index = top_level_entry_index_for_step(entries, previous_current_step);
    if (!previous_current_top_level_entry_index.has_value())
        return {};

    if (entry.step != current_step)
        return {};

    if (current_step == previous_current_step) {
        if (entries[*previous_current_top_level_entry_index].step != previous_current_step)
            return {};
        entries[*previous_current_top_level_entry_index] = entry;
        used_steps = get_all_used_history_steps(entries);
    } else {
        if (current_step <= previous_current_step)
            return {};

        clear_forward_session_history_entries(entries, previous_current_step);
        used_steps.shrink(*previous_current_used_step_index + 1);
        if (used_steps.contains_slow(current_step))
            return {};

        entries.append(entry);
        used_steps.append(current_step);
    }

    auto current_used_step_index = used_steps.find_first_index(current_step);
    if (!current_used_step_index.has_value())
        return {};

    if (!entries_are_valid(entries) || !steps_are_valid(used_steps) || !entries_and_used_steps_are_consistent(entries, used_steps))
        return {};

    return SameDocumentNavigationMutationResult {
        .entries = move(entries),
        .used_steps = move(used_steps),
        .current_used_step_index = *current_used_step_index,
    };
}

static TraversableSessionHistory::Entry create_ui_process_session_history_entry(
    i32 step,
    URL::URL url,
    Web::HTML::CrossProcessId document_state_id,
    Web::HTML::DocumentResource document_resource)
{
    return {
        .step = step,
        .url = move(url),
        .document_state = {
            .id = document_state_id,
            .history_policy_container = Web::HTML::DocumentState::Client::Tag,
            .request_referrer = Web::Fetch::Infrastructure::Request::Referrer::Client,
            .request_referrer_policy = Web::ReferrerPolicy::DEFAULT_REFERRER_POLICY,
            .initiator_origin = {},
            .origin = {},
            .about_base_url = {},
            .resource = move(document_resource),
            .reload_pending = false,
            .ever_populated = false,
            .is_provisional = true,
            .navigable_target_name = {},
            .nested_histories = {},
        },
        .classic_history_api_state = {},
        .navigation_api_state = {},
        .navigation_api_key = {},
        .navigation_api_id = {},
        .scroll_restoration_mode = Web::HTML::ScrollRestorationMode::Auto,
        .scroll_position_data = {},
    };
}

void TraversableSessionHistory::navigate(URL::URL url, Web::HTML::CrossProcessId document_state_id)
{
    navigate(move(url), document_state_id, Empty {});
}

void TraversableSessionHistory::navigate(URL::URL url, Web::HTML::CrossProcessId document_state_id, Web::HTML::DocumentResource document_resource)
{
    mark_web_content_history_match_unproven();

    if (!m_current_used_step_index.has_value()) {
        m_entries.clear();
        m_used_steps.clear();
        m_entries.append(create_ui_process_session_history_entry(0, move(url), document_state_id, move(document_resource)));
        m_used_steps.append(0);
        m_current_used_step_index = 0;
        return;
    }

    auto current_step = m_used_steps[*m_current_used_step_index];
    VERIFY(current_step < NumericLimits<i32>::max());
    clear_forward_session_history_entries(m_entries, current_step);
    auto step = current_step + 1;
    m_used_steps.remove_all_matching([current_step](auto const& used_step) {
        return used_step > current_step;
    });
    m_entries.append(create_ui_process_session_history_entry(step, move(url), document_state_id, move(document_resource)));
    m_used_steps.append(step);
    m_current_used_step_index = m_used_steps.size() - 1;
}

void TraversableSessionHistory::clear()
{
    m_entries.clear();
    m_used_steps.clear();
    m_current_used_step_index.clear();
    forget_web_content_state();
}

void TraversableSessionHistory::replace_current_entry_url(URL::URL url, Web::HTML::CrossProcessId document_state_id)
{
    mark_web_content_history_match_unproven();

    if (!m_current_used_step_index.has_value()) {
        navigate(move(url), document_state_id);
        return;
    }

    auto current_top_level_entry_index = this->current_top_level_entry_index();
    VERIFY(current_top_level_entry_index.has_value());
    m_entries[*current_top_level_entry_index].url = move(url);
}

void TraversableSessionHistory::replace_current_entry(URL::URL url, Web::HTML::CrossProcessId document_state_id, Web::HTML::DocumentResource document_resource)
{
    mark_web_content_history_match_unproven();

    if (!m_current_used_step_index.has_value()) {
        navigate(move(url), document_state_id, move(document_resource));
        return;
    }

    auto current_top_level_entry_index = this->current_top_level_entry_index();
    VERIFY(current_top_level_entry_index.has_value());

    auto current_step = m_used_steps[*m_current_used_step_index];
    m_entries[*current_top_level_entry_index] = create_ui_process_session_history_entry(
        current_step, move(url), document_state_id, move(document_resource));
    recompute_used_steps(m_entries, m_used_steps, m_current_used_step_index, current_step);
    VERIFY(m_current_used_step_index.has_value());
}

void TraversableSessionHistory::mark_current_entry_reload_pending()
{
    auto current_top_level_entry_index = this->current_top_level_entry_index();
    if (!current_top_level_entry_index.has_value())
        return;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#reload
    // Set navigable's active session history entry's document state's reload
    // pending to true.
    m_entries[*current_top_level_entry_index].document_state.reload_pending = true;
    mark_web_content_history_match_unproven();
}

void TraversableSessionHistory::clear_current_entry_reload_pending()
{
    auto current_top_level_entry_index = this->current_top_level_entry_index();
    if (!current_top_level_entry_index.has_value())
        return;

    m_entries[*current_top_level_entry_index].document_state.reload_pending = false;
    mark_web_content_history_match_unproven();
}

Optional<size_t> TraversableSessionHistory::current_top_level_entry_index() const
{
    if (!m_current_used_step_index.has_value())
        return {};
    return top_level_entry_index_for_step(m_entries, m_used_steps[*m_current_used_step_index]);
}

bool TraversableSessionHistory::update_current_entry_from_web_content(Web::HTML::SessionHistoryEntryUpdateKind update_kind, Entry updated_entry)
{
    auto entries = m_entries;
    auto mutation_result = apply_targeted_current_entry_update(entries, updated_entry, update_kind);
    if (!mutation_result.found || mutation_result.rejected)
        return false;

    m_entries = move(entries);
    return true;
}

bool TraversableSessionHistory::apply_top_level_same_document_navigation_from_web_content(Entry entry, Optional<i32> replaced_step, i32 current_step)
{
    if (entry.step < 0 || current_step < 0 || entry.document_state.id.namespace_id == 0 || entry.document_state.id.local_id == 0)
        return false;

    if (m_entries.is_empty() || m_used_steps.is_empty() || !m_current_used_step_index.has_value())
        return false;

    auto previous_authoritative_step = m_used_steps[*m_current_used_step_index];
    auto authoritative_mutation = apply_top_level_same_document_navigation(m_entries, m_used_steps, previous_authoritative_step, entry, replaced_step, current_step);
    if (!authoritative_mutation.has_value())
        return false;

    m_entries = move(authoritative_mutation->entries);
    m_used_steps = move(authoritative_mutation->used_steps);
    m_current_used_step_index = authoritative_mutation->current_used_step_index;
    return true;
}

bool TraversableSessionHistory::apply_nested_same_document_navigation_from_web_content(Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Entry entry, Optional<i32> replaced_step, i32 current_step)
{
    if (parent_document_state_id.namespace_id == 0 || parent_document_state_id.local_id == 0)
        return false;
    if (navigable_id.namespace_id == 0 || navigable_id.local_id == 0)
        return false;
    if (entry.step < 0 || current_step < 0 || entry.document_state.id.namespace_id == 0 || entry.document_state.id.local_id == 0)
        return false;

    if (m_entries.is_empty() || m_used_steps.is_empty() || !m_current_used_step_index.has_value())
        return false;

    auto authoritative_mutation = apply_nested_same_document_navigation(m_entries, m_used_steps, m_used_steps[*m_current_used_step_index], parent_document_state_id, navigable_id, entry, replaced_step, current_step);
    if (!authoritative_mutation.has_value())
        return false;

    m_entries = move(authoritative_mutation->entries);
    m_used_steps = move(authoritative_mutation->used_steps);
    m_current_used_step_index = authoritative_mutation->current_used_step_index;
    return true;
}

bool TraversableSessionHistory::apply_nested_cross_document_navigation_from_web_content(Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Entry entry, i32 current_step)
{
    if (parent_document_state_id.namespace_id == 0 || parent_document_state_id.local_id == 0)
        return false;
    if (navigable_id.namespace_id == 0 || navigable_id.local_id == 0)
        return false;
    if (entry.step < 0 || current_step < 0 || entry.document_state.id.namespace_id == 0 || entry.document_state.id.local_id == 0)
        return false;

    if (m_entries.is_empty() || m_used_steps.is_empty() || !m_current_used_step_index.has_value())
        return false;

    auto authoritative_mutation = apply_nested_cross_document_navigation(m_entries, m_used_steps, m_used_steps[*m_current_used_step_index], parent_document_state_id, navigable_id, entry, current_step);
    if (!authoritative_mutation.has_value())
        return false;

    m_entries = move(authoritative_mutation->entries);
    m_used_steps = move(authoritative_mutation->used_steps);
    m_current_used_step_index = authoritative_mutation->current_used_step_index;
    return true;
}

bool TraversableSessionHistory::update_current_entry_nested_history_from_web_content(Web::HTML::CrossProcessId document_state_id, Web::HTML::SessionHistoryNestedHistoryDescriptor nested_history, i32 current_step)
{
    if (document_state_id.namespace_id == 0 || document_state_id.local_id == 0)
        return false;
    if (current_step < 0)
        return false;

    if (m_entries.is_empty() || m_used_steps.is_empty() || !m_current_used_step_index.has_value())
        return false;

    auto previous_authoritative_step = m_used_steps[*m_current_used_step_index];

    auto authoritative_mutation = apply_current_entry_nested_history_update(m_entries, previous_authoritative_step, document_state_id, nested_history, current_step);
    if (!authoritative_mutation.has_value())
        return false;

    m_entries = move(authoritative_mutation->entries);
    m_used_steps = move(authoritative_mutation->used_steps);
    m_current_used_step_index = authoritative_mutation->current_used_step_index;
    return true;
}

bool TraversableSessionHistory::remove_current_entry_nested_history_from_web_content(Web::HTML::CrossProcessId document_state_id, Web::HTML::CrossProcessId nested_history_id, i32 current_step)
{
    if (document_state_id.namespace_id == 0 || document_state_id.local_id == 0)
        return false;
    if (nested_history_id.namespace_id == 0 || nested_history_id.local_id == 0)
        return false;
    if (current_step < 0)
        return false;

    if (m_entries.is_empty() || m_used_steps.is_empty() || !m_current_used_step_index.has_value())
        return false;

    auto previous_authoritative_step = m_used_steps[*m_current_used_step_index];

    auto authoritative_mutation = apply_current_entry_nested_history_removal(m_entries, previous_authoritative_step, document_state_id, nested_history_id, current_step);
    if (!authoritative_mutation.has_value())
        return false;

    m_entries = move(authoritative_mutation->entries);
    m_used_steps = move(authoritative_mutation->used_steps);
    m_current_used_step_index = authoritative_mutation->current_used_step_index;
    return true;
}

bool TraversableSessionHistory::apply_top_level_cross_document_navigation_from_web_content(Entry entry, i32 current_step)
{
    if (entry.step < 0 || current_step < 0 || entry.document_state.id.namespace_id == 0 || entry.document_state.id.local_id == 0)
        return false;

    if (entry.step != current_step)
        return false;

    if (!entry.document_state.nested_histories.is_empty())
        return false;

    if (m_entries.is_empty() || m_used_steps.is_empty() || !m_current_used_step_index.has_value())
        return false;

    if (m_used_steps[*m_current_used_step_index] == current_step) {
        auto current_top_level_entry_index = this->current_top_level_entry_index();
        if (!current_top_level_entry_index.has_value())
            return false;

        auto const& predicted_entry = m_entries[*current_top_level_entry_index];
        if (predicted_entry.step == entry.step && predicted_entry.document_state.nested_histories.is_empty()) {
            auto entries = m_entries;
            entries[*current_top_level_entry_index] = entry;
            if (!entries_are_valid(entries) || !entries_and_used_steps_are_consistent(entries, m_used_steps))
                return false;

            m_entries = move(entries);
            return true;
        }
    }

    auto const previous_authoritative_step = m_used_steps[*m_current_used_step_index];
    if (previous_authoritative_step == current_step) {
        auto current_top_level_entry_index = this->current_top_level_entry_index();
        if (!current_top_level_entry_index.has_value())
            return false;

        auto const& predicted_entry = m_entries[*current_top_level_entry_index];
        if (predicted_entry.step != entry.step || !predicted_entry.document_state.nested_histories.is_empty())
            return false;
    }

    auto authoritative_mutation = apply_top_level_cross_document_navigation(m_entries, m_used_steps, previous_authoritative_step, entry, current_step);
    if (!authoritative_mutation.has_value())
        return false;

    m_entries = move(authoritative_mutation->entries);
    m_used_steps = move(authoritative_mutation->used_steps);
    m_current_used_step_index = authoritative_mutation->current_used_step_index;
    return true;
}

TraversableSessionHistory::WebContentMutationResult TraversableSessionHistory::apply_web_content_mutation(WebContentMutation mutation)
{
    auto const mirror_was_complete_before_mutation = web_content_history_matches_mirror();
    auto accepted_mutation_result = [&]() {
        if (!mirror_was_complete_before_mutation)
            m_web_content_mirror_state = WebContentMirrorState::Unknown;
        return WebContentMutationResult {
            .accepted = true,
            .web_content_history_matches_mirror = web_content_history_matches_mirror(),
        };
    };
    switch (mutation.type) {
    case WebContentMutationType::CurrentEntryUpdate: {
        if (!update_current_entry_from_web_content(mutation.current_entry_update_kind, move(mutation.entry)))
            return {};
        return accepted_mutation_result();
    }
    case WebContentMutationType::CurrentEntryNestedHistoryUpdate:
        if (!update_current_entry_nested_history_from_web_content(mutation.document_state_id, move(mutation.nested_history), mutation.current_step))
            return {};
        return accepted_mutation_result();
    case WebContentMutationType::CurrentEntryNestedHistoryRemoval:
        if (!remove_current_entry_nested_history_from_web_content(mutation.document_state_id, mutation.nested_history_id, mutation.current_step))
            return {};
        return accepted_mutation_result();
    case WebContentMutationType::TopLevelSameDocumentNavigation:
        if (!apply_top_level_same_document_navigation_from_web_content(move(mutation.entry), mutation.replaced_step, mutation.current_step))
            return {};
        return accepted_mutation_result();
    case WebContentMutationType::NestedSameDocumentNavigation:
        if (!apply_nested_same_document_navigation_from_web_content(mutation.parent_document_state_id, mutation.navigable_id, move(mutation.entry), mutation.replaced_step, mutation.current_step))
            return {};
        return accepted_mutation_result();
    case WebContentMutationType::NestedCrossDocumentNavigation:
        if (!apply_nested_cross_document_navigation_from_web_content(mutation.parent_document_state_id, mutation.navigable_id, move(mutation.entry), mutation.current_step))
            return {};
        return accepted_mutation_result();
    case WebContentMutationType::TopLevelCrossDocumentNavigation:
        if (!apply_top_level_cross_document_navigation_from_web_content(move(mutation.entry), mutation.current_step))
            return {};
        return accepted_mutation_result();
    case WebContentMutationType::AppliedTraversal:
        if (!did_apply_web_content_traversal_to_step(mutation.current_step))
            return {};
        return {
            .accepted = true,
            .web_content_history_matches_mirror = web_content_history_matches_mirror(),
        };
    case WebContentMutationType::RestoredCurrentStep:
        if (!did_restore_web_content_to_current_step(mutation.current_step))
            return {};
        return {
            .accepted = true,
            .web_content_history_matches_mirror = web_content_history_matches_mirror(),
        };
    }

    VERIFY_NOT_REACHED();
}

void TraversableSessionHistory::record_web_content_seeded_from_ui_process(i32 current_step)
{
    VERIFY(m_used_steps.contains_slow(current_step));
    if (!m_current_used_step_index.has_value() || m_used_steps[*m_current_used_step_index] != current_step) {
        m_web_content_mirror_state = WebContentMirrorState::Unknown;
        return;
    }
    m_web_content_mirror_state = WebContentMirrorState::CompleteMirror;
}

void TraversableSessionHistory::record_web_content_mirror_matches_ui_process()
{
    if (!m_current_used_step_index.has_value()) {
        m_web_content_mirror_state = WebContentMirrorState::Unknown;
        return;
    }

    m_web_content_mirror_state = WebContentMirrorState::CompleteMirror;
}

bool TraversableSessionHistory::did_restore_web_content_to_current_step(i32 step)
{
    if (!m_current_used_step_index.has_value())
        return false;
    if (m_used_steps[*m_current_used_step_index] != step)
        return false;

    m_web_content_mirror_state = WebContentMirrorState::CompleteMirror;
    return true;
}

bool TraversableSessionHistory::did_apply_web_content_traversal_to_step(i32 step)
{
    auto const mirror_was_complete_before_traversal = web_content_history_matches_mirror();
    auto target = traversal_target_for_step(step);
    if (!target.has_value())
        return false;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
    // Set traversable's current session history step to targetStep.
    m_current_used_step_index = target->target_step_index;
    m_web_content_mirror_state = mirror_was_complete_before_traversal ? WebContentMirrorState::CompleteMirror : WebContentMirrorState::Unknown;
    return true;
}

bool TraversableSessionHistory::web_content_history_matches_mirror() const
{
    return m_web_content_mirror_state == WebContentMirrorState::CompleteMirror;
}

void TraversableSessionHistory::forget_web_content_state()
{
    m_web_content_mirror_state = WebContentMirrorState::Unknown;
}

void TraversableSessionHistory::mark_web_content_history_match_unproven()
{
    m_web_content_mirror_state = WebContentMirrorState::Unknown;
}

Vector<TraversableSessionHistory::Entry> TraversableSessionHistory::entries() const
{
    Vector<Entry> entries;
    entries.ensure_capacity(m_entries.size());
    for (auto const& entry : m_entries)
        entries.unchecked_append(entry);
    return entries;
}

Vector<i32> TraversableSessionHistory::used_steps() const
{
    return m_used_steps;
}

bool TraversableSessionHistory::can_go_back() const
{
    return m_current_used_step_index.has_value() && *m_current_used_step_index > 0;
}

bool TraversableSessionHistory::can_go_forward() const
{
    return m_current_used_step_index.has_value() && *m_current_used_step_index + 1 < m_used_steps.size();
}

bool TraversableSessionHistory::has_only_top_level_used_steps() const
{
    if (entries_have_nested_histories(m_entries))
        return false;

    if (m_entries.size() != m_used_steps.size())
        return false;

    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].step != m_used_steps[i])
            return false;
    }
    return true;
}

bool TraversableSessionHistory::current_step_is_top_level_entry() const
{
    if (!m_current_used_step_index.has_value())
        return false;
    return entry_for_step(m_used_steps[*m_current_used_step_index]) != nullptr;
}

Optional<i32> TraversableSessionHistory::current_step_to_restore_after_loading_top_level_entry() const
{
    if (!m_current_used_step_index.has_value())
        return {};

    auto current_step = m_used_steps[*m_current_used_step_index];
    auto const* current_entry = entry_for_step(current_step);
    if (current_entry) {
        if (nested_histories_need_restore_after_loading_entry(*current_entry, current_step))
            return current_step;
        return {};
    }

    return current_step;
}

bool TraversableSessionHistory::web_content_can_traverse_to(TraversalTarget const& target) const
{
    if (!web_content_history_matches_mirror())
        return false;
    return m_used_steps.contains_slow(target.target_step);
}

Optional<TraversableSessionHistory::TraversalTarget> TraversableSessionHistory::traversal_target_for_delta(int delta) const
{
    auto target_step_index = target_step_index_for_delta(delta);
    if (!target_step_index.has_value())
        return {};

    auto target_step = step_at(*target_step_index);
    VERIFY(target_step.has_value());
    return traversal_target_for_step(*target_step);
}

Optional<TraversableSessionHistory::TraversalTarget> TraversableSessionHistory::traversal_target_for_step(i32 step) const
{
    auto target_step_index = m_used_steps.find_first_index(step);
    if (!target_step_index.has_value())
        return {};

    auto const* target_top_level_entry = top_level_entry_for_step(step);
    VERIFY(target_top_level_entry);
    auto const* current_top_level_entry = current_entry();
    VERIFY(current_top_level_entry);

    return TraversalTarget {
        .target_step_index = *target_step_index,
        .target_step = step,
        .target_top_level_entry = target_top_level_entry,
        .target_step_is_top_level_entry = entry_for_step(step) != nullptr,
        .changes_top_level_entry = target_top_level_entry != current_top_level_entry,
    };
}

Optional<size_t> TraversableSessionHistory::target_step_index_for_delta(int delta) const
{
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
    // Let allSteps be the result of getting all used history steps. Let
    // targetStepIndex be currentStepIndex plus delta. If allSteps[targetStepIndex]
    // does not exist, then abort these steps.
    if (!m_current_used_step_index.has_value() || delta == 0)
        return {};

    if (delta < 0) {
        auto magnitude = static_cast<size_t>(-static_cast<i64>(delta));
        if (magnitude > *m_current_used_step_index)
            return {};
        return *m_current_used_step_index - magnitude;
    }

    auto target_index = *m_current_used_step_index + static_cast<size_t>(delta);
    if (target_index >= m_used_steps.size())
        return {};
    return target_index;
}

Optional<i32> TraversableSessionHistory::step_at(size_t index) const
{
    if (index >= m_used_steps.size())
        return {};
    return m_used_steps[index];
}

TraversableSessionHistory::Entry const* TraversableSessionHistory::current_entry() const
{
    if (!m_current_used_step_index.has_value())
        return nullptr;
    return top_level_entry_for_step(m_used_steps[*m_current_used_step_index]);
}

TraversableSessionHistory::Entry const* TraversableSessionHistory::entry_at(size_t index) const
{
    if (index >= m_entries.size())
        return nullptr;
    return &m_entries[index];
}

TraversableSessionHistory::Entry const* TraversableSessionHistory::entry_for_step(i32 step) const
{
    for (auto const& entry : m_entries) {
        if (entry.step == step)
            return &entry;
    }
    return nullptr;
}

TraversableSessionHistory::Entry const* TraversableSessionHistory::top_level_entry_for_step(i32 step) const
{
    auto index = top_level_entry_index_for_step(m_entries, step);
    if (!index.has_value())
        return nullptr;
    return &m_entries[*index];
}

void TraversableSessionHistory::traverse_to(size_t index)
{
    VERIFY(index < m_used_steps.size());
    m_current_used_step_index = index;
    forget_web_content_state();
}

}
