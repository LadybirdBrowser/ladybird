/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/SessionHistory.h>

namespace WebView {

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

static bool entries_have_nested_histories(Vector<TraversableSessionHistory::Entry> const& entries)
{
    for (auto const& entry : entries) {
        if (!entry.document_state.nested_histories.is_empty())
            return true;
    }
    return false;
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

static bool nesting_depth_is_valid(Vector<TraversableSessionHistory::Entry> const& entries, size_t depth = 0)
{
    if (depth > MAX_NESTED_HISTORY_DEPTH)
        return false;

    for (auto const& entry : entries) {
        for (auto const& nested_history : entry.document_state.nested_histories) {
            if (!nesting_depth_is_valid(nested_history.entries, depth + 1))
                return false;
        }
    }
    return true;
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

// A step found only under an inactive nested sibling (off the greatest-step<=target path) is not reachable.
static bool active_path_reaches_step(TraversableSessionHistory::Entry const& active_entry, i32 step)
{
    if (active_entry.step == step)
        return true;
    for (auto const& nested_history : active_entry.document_state.nested_histories) {
        auto const* active_nested_entry = entry_for_step_in_entry_list(nested_history.entries, step);
        if (active_nested_entry && active_path_reaches_step(*active_nested_entry, step))
            return true;
    }
    return false;
}

ErrorOr<void> validate_snapshot_is_restorable(Vector<TraversableSessionHistory::Entry> const& entries, Vector<i32> const& used_steps, size_t current_used_step_index)
{
    if (entries.is_empty() || used_steps.is_empty() || current_used_step_index >= used_steps.size()
        || !nesting_depth_is_valid(entries) || !entries_are_valid(entries) || !steps_are_valid(used_steps)
        || get_all_used_history_steps(entries) != used_steps)
        return Error::from_string_literal("Session history snapshot is structurally invalid");

    for (auto step : used_steps) {
        auto top_level_entry_index = top_level_entry_index_for_step(entries, step);
        if (!top_level_entry_index.has_value() || !active_path_reaches_step(entries[*top_level_entry_index], step))
            return Error::from_string_literal("Session history snapshot has a used step that is not reachable");
    }

    auto current_top_level_entry_index = top_level_entry_index_for_step(entries, used_steps[current_used_step_index]);
    if (!entries[*current_top_level_entry_index].document_state.ever_populated)
        return Error::from_string_literal("Session history snapshot's current entry has no document state");

    return {};
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

void TraversableSessionHistory::clear()
{
    m_entries.clear();
    m_used_steps.clear();
    m_current_used_step_index.clear();
}

bool TraversableSessionHistory::initialize_for_testing(Vector<Entry> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    if (entries.is_empty() || current_used_step_index >= used_steps.size())
        return false;
    if (get_all_used_history_steps(entries) != used_steps)
        return false;

    m_entries = move(entries);
    m_used_steps = move(used_steps);
    m_current_used_step_index = current_used_step_index;
    return true;
}

void TraversableSessionHistory::initialize_with_initial_history_entry(Entry initial_history_entry)
{
    m_entries.append(move(initial_history_entry));
    m_used_steps.append(0);
    m_current_used_step_index = 0;
}

static void assign_fresh_ids_to_restored_entries(Vector<TraversableSessionHistory::Entry>& entries, Function<Web::HTML::CrossProcessId()> const& allocate_cross_process_id, HashMap<Web::HTML::CrossProcessId, Web::HTML::CrossProcessId>& assigned_ids)
{
    for (auto& entry : entries) {
        entry.document_state.id = assigned_ids.ensure(entry.document_state.id, [&] { return allocate_cross_process_id(); });
        for (auto& nested_history : entry.document_state.nested_histories) {
            nested_history.id = assigned_ids.ensure(nested_history.id, [&] { return allocate_cross_process_id(); });
            assign_fresh_ids_to_restored_entries(nested_history.entries, allocate_cross_process_id, assigned_ids);
        }
    }
}

ErrorOr<void> TraversableSessionHistory::restore_from_ui_snapshot(Vector<Entry> entries, Vector<i32> used_steps, size_t current_used_step_index, Function<Web::HTML::CrossProcessId()> allocate_cross_process_id)
{
    TRY(validate_snapshot_is_restorable(entries, used_steps, current_used_step_index));

    HashMap<Web::HTML::CrossProcessId, Web::HTML::CrossProcessId> assigned_ids;
    assign_fresh_ids_to_restored_entries(entries, allocate_cross_process_id, assigned_ids);
    m_entries = move(entries);
    m_used_steps = move(used_steps);
    m_current_used_step_index = current_used_step_index;
    return {};
}

void TraversableSessionHistory::mark_current_entry_reload_pending()
{
    auto current_top_level_entry_index = this->current_top_level_entry_index();
    if (!current_top_level_entry_index.has_value())
        return;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#reload
    // Set navigable's active session history entry's document state's reload
    // pending to true.
    auto document_state_id = m_entries[*current_top_level_entry_index].document_state.id;
    for (auto& entry : m_entries) {
        if (entry.document_state.id == document_state_id)
            entry.document_state.reload_pending = true;
    }
}

template<typename UpdateEntry>
static bool update_session_history_entry_by_navigation_api_key(Vector<TraversableSessionHistory::Entry>& entries, Utf16String const& navigation_api_key, UpdateEntry const& update_entry)
{
    auto did_update = false;
    for (auto& entry : entries) {
        if (entry.navigation_api_key == navigation_api_key) {
            update_entry(entry);
            did_update = true;
        }
    }
    return did_update;
}

template<typename UpdateEntry>
static bool update_nested_session_history_entries_by_navigation_api_key(Vector<TraversableSessionHistory::Entry>& entries, Web::HTML::CrossProcessId navigable_id, Utf16String const& navigation_api_key, UpdateEntry const& update_entry)
{
    auto did_update = false;
    for (auto& entry : entries) {
        for (auto& nested_history : entry.document_state.nested_histories) {
            if (nested_history.id == navigable_id)
                did_update |= update_session_history_entry_by_navigation_api_key(nested_history.entries, navigation_api_key, update_entry);
            did_update |= update_nested_session_history_entries_by_navigation_api_key(nested_history.entries, navigable_id, navigation_api_key, update_entry);
        }
    }
    return did_update;
}

bool TraversableSessionHistory::update_entry(Optional<Web::HTML::CrossProcessId> nested_history_id, Utf16String const& navigation_api_key, Function<void(Entry&)> const& update_entry)
{
    if (!nested_history_id.has_value())
        return update_session_history_entry_by_navigation_api_key(m_entries, navigation_api_key, update_entry);
    return update_nested_session_history_entries_by_navigation_api_key(m_entries, *nested_history_id, navigation_api_key, update_entry);
}

bool TraversableSessionHistory::update_entry_persisted_state(Optional<Web::HTML::CrossProcessId> nested_history_id, Web::HTML::SessionHistoryEntryPersistedState const& persisted_state)
{
    auto did_update = false;
    update_entry(nested_history_id, persisted_state.navigation_api_key, [&](auto& entry) {
        if (entry.document_state.id != persisted_state.document_state_id)
            return;
        entry.scroll_position_data = persisted_state.scroll_position_data;
        did_update = true;
    });
    return did_update;
}

template<typename UpdateDocumentState>
static bool update_session_history_document_state_by_navigation_api_key(Vector<TraversableSessionHistory::Entry>& entries, Utf16String const& navigation_api_key, UpdateDocumentState const& update_document_state)
{
    Optional<Web::HTML::CrossProcessId> document_state_id;
    for (auto const& entry : entries) {
        if (entry.navigation_api_key == navigation_api_key)
            document_state_id = entry.document_state.id;
    }
    if (!document_state_id.has_value())
        return false;

    for (auto& entry : entries) {
        if (entry.document_state.id == *document_state_id)
            update_document_state(entry.document_state);
    }
    return true;
}

template<typename UpdateDocumentState>
static bool update_nested_session_history_document_state_by_navigation_api_key(Vector<TraversableSessionHistory::Entry>& entries, Web::HTML::CrossProcessId nested_history_id, Utf16String const& navigation_api_key, UpdateDocumentState const& update_document_state)
{
    auto did_update = false;
    for (auto& entry : entries) {
        for (auto& nested_history : entry.document_state.nested_histories) {
            if (nested_history.id == nested_history_id)
                did_update |= update_session_history_document_state_by_navigation_api_key(nested_history.entries, navigation_api_key, update_document_state);
            did_update |= update_nested_session_history_document_state_by_navigation_api_key(nested_history.entries, nested_history_id, navigation_api_key, update_document_state);
        }
    }
    return did_update;
}

bool TraversableSessionHistory::update_document_state(Optional<Web::HTML::CrossProcessId> nested_history_id, Utf16String const& navigation_api_key, Function<void(Web::HTML::SessionHistoryDocumentStateDescriptor&)> const& update_document_state)
{
    if (!nested_history_id.has_value()) {
        return update_session_history_document_state_by_navigation_api_key(m_entries, navigation_api_key, update_document_state);
    }

    return update_nested_session_history_document_state_by_navigation_api_key(m_entries, *nested_history_id, navigation_api_key, update_document_state);
}

static Vector<TraversableSessionHistory::Entry>* nested_session_history_entries_for_navigable(Vector<TraversableSessionHistory::Entry>& entries, Web::HTML::CrossProcessId navigable_id)
{
    for (auto& entry : entries) {
        for (auto& nested_history : entry.document_state.nested_histories) {
            if (nested_history.id == navigable_id)
                return &nested_history.entries;
            if (auto* nested_entries = nested_session_history_entries_for_navigable(nested_history.entries, navigable_id))
                return nested_entries;
        }
    }
    return nullptr;
}

static TraversableSessionHistory::Entry* target_history_entry(Vector<TraversableSessionHistory::Entry>& entries, i32 step)
{
    TraversableSessionHistory::Entry* target_entry = nullptr;
    for (auto& entry : entries) {
        if (entry.step > step)
            break;
        target_entry = &entry;
    }
    return target_entry;
}

template<typename UpdateDocumentState>
static bool update_session_history_document_state_by_id(Vector<TraversableSessionHistory::Entry>& entries, Web::HTML::CrossProcessId document_state_id, UpdateDocumentState const& update_document_state)
{
    auto did_update = false;
    for (auto& entry : entries) {
        if (entry.document_state.id == document_state_id) {
            update_document_state(entry.document_state);
            did_update = true;
        }
        for (auto& nested_history : entry.document_state.nested_histories)
            did_update |= update_session_history_document_state_by_id(nested_history.entries, document_state_id, update_document_state);
    }
    return did_update;
}

Optional<i32> TraversableSessionHistory::append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id, Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry)
{
    if (!m_current_used_step_index.has_value())
        return {};

    // https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-new-child-navigable
    // These are steps 1-6 of the traversal steps appended by "create a new child navigable". WebContent supplies the
    // identity of parentDocState, whose live object it obtained from parentNavigable's active entry. The canonical
    // entry list supplies targetStepSHE and therefore owns the concrete step assigned here.
    auto current_step = m_used_steps[*m_current_used_step_index];
    auto* parent_entries = parent_navigable.is_top_level_traversable()
        ? &m_entries
        : nested_session_history_entries_for_navigable(m_entries, parent_navigable.id());
    if (!parent_entries)
        return {};

    auto target_step_entry = parent_entries->find_if([&](auto const& entry) {
        return entry.document_state.id == parent_document_state_id;
    });
    if (target_step_entry == parent_entries->end())
        return {};

    auto target_step = target_step_entry->step;
    Web::HTML::SessionHistoryNestedHistoryDescriptor nested_history {
        .id = child_navigable_id,
        .entries { Web::HTML::create_session_history_entry_descriptor(move(initial_history_entry), target_step) },
    };

    // Append nestedHistory to parentDocState's nested histories.
    auto append_to_parent_document_state = [&](auto& parent_document_state) {
        auto existing_nested_history = parent_document_state.nested_histories.find_if([&](auto const& existing_nested_history) {
            return existing_nested_history.id == nested_history.id;
        });
        if (existing_nested_history != parent_document_state.nested_histories.end())
            return;
        parent_document_state.nested_histories.append(nested_history);
    };
    if (!update_session_history_document_state_by_id(m_entries, parent_document_state_id, append_to_parent_document_state))
        return {};

    m_used_steps = get_all_used_history_steps(m_entries);
    m_current_used_step_index = m_used_steps.find_first_index(current_step);
    VERIFY(m_current_used_step_index.has_value());
    return target_step;
}

bool TraversableSessionHistory::remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id)
{
    if (!m_current_used_step_index.has_value())
        return false;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-child-navigable
    // Let parentDocState be container's node navigable's active session history entry's document state. The live
    // parent entry was read before these traversal steps were appended, so use the reported stable document-state
    // identity instead of resolving the UI's current step again when the IPC request arrives.
    auto current_step = m_used_steps[*m_current_used_step_index];
    auto* parent_entries = parent_navigable.is_top_level_traversable()
        ? &m_entries
        : nested_session_history_entries_for_navigable(m_entries, parent_navigable.id());
    if (!parent_entries)
        return false;
    if (parent_entries->find_if([&](auto const& entry) { return entry.document_state.id == parent_document_state_id; }) == parent_entries->end())
        return false;

    // Remove the nested history from parentDocState's nested histories whose id equals navigable's id.
    auto remove_from_parent_document_state = [child_navigable_id](auto& parent_document_state) {
        parent_document_state.nested_histories.remove_all_matching([child_navigable_id](auto const& nested_history) {
            return nested_history.id == child_navigable_id;
        });
    };
    if (!update_session_history_document_state_by_id(m_entries, parent_document_state_id, remove_from_parent_document_state))
        return false;

    m_used_steps = get_all_used_history_steps(m_entries);
    auto used_current_step = current_step;
    if (!m_used_steps.contains_slow(current_step)) {
        for (auto used_step : m_used_steps) {
            if (used_step > current_step)
                break;
            used_current_step = used_step;
        }
    }
    m_current_used_step_index = m_used_steps.find_first_index(used_current_step);
    VERIFY(m_current_used_step_index.has_value());
    return true;
}

template<typename UpdateEntries>
static bool update_nested_session_history_entries_for_navigable(Vector<TraversableSessionHistory::Entry>& entries, Web::HTML::CrossProcessId navigable_id, UpdateEntries const& update_entries)
{
    auto did_update = false;
    for (auto& entry : entries) {
        for (auto& nested_history : entry.document_state.nested_histories) {
            if (nested_history.id == navigable_id)
                did_update |= update_entries(nested_history.entries);
            did_update |= update_nested_session_history_entries_for_navigable(nested_history.entries, navigable_id, update_entries);
        }
    }
    return did_update;
}

template<typename UpdateEntries>
static bool update_session_history_entries_for_navigable(Vector<TraversableSessionHistory::Entry>& entries, Optional<Web::HTML::CrossProcessId> nested_history_id, UpdateEntries const& update_entries)
{
    if (!nested_history_id.has_value())
        return update_entries(entries);
    return update_nested_session_history_entries_for_navigable(entries, *nested_history_id, update_entries);
}

static bool append_or_replace_session_history_entry(Vector<TraversableSessionHistory::Entry>& entries, TraversableSessionHistory::Entry const& entry, Optional<Utf16String> const& entry_to_replace_navigation_api_key, bool may_replace_initial_entry = false)
{
    if (!entry_to_replace_navigation_api_key.has_value()) {
        entries.append(entry);
        return true;
    }

    auto entry_to_replace = entries.find_if([&](auto const& existing_entry) {
        return existing_entry.navigation_api_key == *entry_to_replace_navigation_api_key;
    });
    if (entry_to_replace == entries.end()
        && may_replace_initial_entry
        && entries.size() == 1
        && entries.first().url == URL::about_blank()
        && !entries.first().document_state.ever_populated) {
        entry_to_replace = entries.begin();
    }
    if (entry_to_replace == entries.end())
        return false;

    auto replacement_entry = entry;
    replacement_entry.step = entry_to_replace->step;
    *entry_to_replace = move(replacement_entry);
    return true;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation
Optional<i32> TraversableSessionHistory::finalize_same_document_navigation(CanonicalNavigable const& target_navigable, Web::HTML::SameDocumentNavigationEntry target_entry, Optional<Utf16String> entry_to_replace_navigation_api_key)
{
    // 1. Assert: this is running on traversable's session history traversal queue.
    if (!m_current_used_step_index.has_value())
        return {};

    auto current_step = m_used_steps[*m_current_used_step_index];
    auto nested_history_id = target_navigable.is_top_level_traversable() ? Optional<Web::HTML::CrossProcessId> {} : target_navigable.id();

    auto* target_entries = nested_history_id.has_value() ? nested_session_history_entries_for_navigable(m_entries, *nested_history_id) : &m_entries;
    if (!target_entries)
        return {};

    // 2. If targetNavigable's active session history entry is not targetEntry, then return.
    // NB: The process hosting targetNavigable checks object identity before sending the finalization for this operation.
    Entry* source_entry = nullptr;
    if (entry_to_replace_navigation_api_key.has_value()) {
        auto source_entry_iterator = target_entries->find_if([&](auto const& entry) {
            return entry.navigation_api_key == *entry_to_replace_navigation_api_key;
        });
        if (source_entry_iterator != target_entries->end())
            source_entry = &*source_entry_iterator;
    } else {
        source_entry = target_history_entry(*target_entries, current_step);
    }
    if (!source_entry || source_entry->document_state.id != target_entry.document_state_id)
        return {};

    auto canonical_target_entry = Entry {
        .step = source_entry->step,
        .url = move(target_entry.url),
        .document_state = source_entry->document_state,
        .classic_history_api_state = move(target_entry.classic_history_api_state),
        .navigation_api_state = move(target_entry.navigation_api_state),
        .navigation_api_key = move(target_entry.navigation_api_key),
        .navigation_api_id = move(target_entry.navigation_api_id),
        .scroll_restoration_mode = target_entry.scroll_restoration_mode,
        .scroll_position_data = move(target_entry.scroll_position_data),
    };

    // 3. Let targetStep be null.
    i32 target_step = 0;

    // 4. Let targetEntries be the result of getting session history entries for targetNavigable.

    // 5. If entryToReplace is null:
    if (!entry_to_replace_navigation_api_key.has_value()) {
        // 1. Clear the forward session history of traversable.
        clear_forward_session_history_entries(m_entries, current_step);

        // 2. Set targetStep to traversable's current session history step + 1.
        target_step = current_step + 1;

        // 3. Set targetEntry's step to targetStep.
        canonical_target_entry.step = target_step;

        // 4. Append targetEntry to targetEntries.
    }
    // Otherwise:
    else {
        // 1. Replace entryToReplace with targetEntry in targetEntries.
        // 2. Set targetEntry's step to entryToReplace's step.
        // NB: Seeded from entryToReplace above.

        // 3. Set targetStep to traversable's current session history step.
        target_step = current_step;
    }

    auto did_update = update_session_history_entries_for_navigable(m_entries, nested_history_id, [&](auto& entries) {
        return append_or_replace_session_history_entry(entries, canonical_target_entry, entry_to_replace_navigation_api_key);
    });
    if (!did_update)
        return {};

    m_used_steps = get_all_used_history_steps(m_entries);
    m_current_used_step_index = m_used_steps.find_first_index(current_step);
    VERIFY(m_current_used_step_index.has_value());

    // 6. Apply the push/replace history step targetStep to traversable given historyHandling and userInvolvement.
    return target_step;
}

Optional<i32> TraversableSessionHistory::finalize_cross_document_navigation(Optional<Web::HTML::CrossProcessId> nested_history_id, Web::HTML::PendingSessionHistoryEntryDescriptor history_entry, Optional<Utf16String> entry_to_replace_navigation_api_key)
{
    // AD-HOC: The initial about:blank entry is not reported when the browser creates its first WebContent process. Its first
    // committed navigation therefore initializes the canonical history at this queue position.
    if (!m_current_used_step_index.has_value()) {
        if (nested_history_id.has_value())
            return {};
        m_entries.append(Web::HTML::create_session_history_entry_descriptor(move(history_entry), 0));
        m_used_steps.append(0);
        m_current_used_step_index = 0;
        return 0;
    }

    auto current_step = m_used_steps[*m_current_used_step_index];

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation
    // 7. Let targetStep be null.
    Optional<i32> target_step;

    // 8. Let targetEntries be the result of getting session history entries for navigable.
    auto* target_entries = nested_history_id.has_value()
        ? nested_session_history_entries_for_navigable(m_entries, *nested_history_id)
        : &m_entries;
    if (!target_entries)
        return {};

    Entry canonical_target_entry;

    // 9. If entryToReplace is null, then:
    if (!entry_to_replace_navigation_api_key.has_value()) {
        // 1. Clear the forward session history of traversable.
        clear_forward_session_history_entries(m_entries, current_step);

        // 2. Set targetStep to traversable's current session history step + 1.
        VERIFY(current_step < NumericLimits<i32>::max());
        target_step = current_step + 1;

        // 3. Set historyEntry's step to targetStep.
        canonical_target_entry = Web::HTML::create_session_history_entry_descriptor(move(history_entry), *target_step);

        // 4. Append historyEntry to targetEntries.
    } else {
        // 1. Replace entryToReplace with historyEntry in targetEntries.
        auto entry_to_replace = target_entries->find_if([&](auto const& entry) {
            return entry.navigation_api_key == *entry_to_replace_navigation_api_key;
        });
        if (entry_to_replace == target_entries->end()
            && nested_history_id.has_value()
            && target_entries->size() == 1
            && target_entries->first().url == URL::about_blank()
            && !target_entries->first().document_state.ever_populated) {
            entry_to_replace = target_entries->begin();
        }
        if (entry_to_replace == target_entries->end())
            return {};

        // 2. Set historyEntry's step to entryToReplace's step.
        canonical_target_entry = Web::HTML::create_session_history_entry_descriptor(move(history_entry), entry_to_replace->step);

        // 3. If historyEntry's document state's origin is same origin with entryToReplace's document state's origin,
        //    then set historyEntry's navigation API key to entryToReplace's navigation API key.
        if (canonical_target_entry.document_state.origin.has_value()
            && entry_to_replace->document_state.origin.has_value()
            && canonical_target_entry.document_state.origin->is_same_origin(*entry_to_replace->document_state.origin)) {
            canonical_target_entry.navigation_api_key = entry_to_replace->navigation_api_key;
        }

        // 4. Set targetStep to traversable's current session history step.
        target_step = current_step;
    }

    // AD-HOC: The UI mirror serializes a shared document state's nested histories into each same-document entry.
    // Apply the finalization to every copy of targetEntries so they remain equivalent.
    auto did_update = update_session_history_entries_for_navigable(m_entries, nested_history_id, [&](auto& entries) {
        return append_or_replace_session_history_entry(entries, canonical_target_entry, entry_to_replace_navigation_api_key, nested_history_id.has_value());
    });
    if (!did_update)
        return {};

    m_used_steps = get_all_used_history_steps(m_entries);
    m_current_used_step_index = m_used_steps.find_first_index(current_step);
    VERIFY(m_current_used_step_index.has_value());
    return target_step;
}

Optional<size_t> TraversableSessionHistory::current_top_level_entry_index() const
{
    if (!m_current_used_step_index.has_value())
        return {};
    return top_level_entry_index_for_step(m_entries, m_used_steps[*m_current_used_step_index]);
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

Optional<TraversableSessionHistory::TraversalTarget> TraversableSessionHistory::traversal_target_for_delta(int delta) const
{
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta

    // 1. Let allSteps be the result of getting all used history steps for traversable.
    // NB: m_used_steps is the cached result for the canonical traversable session history.

    // 2. Let currentStepIndex be the index of traversable's current session history step within allSteps.

    // 3. Let targetStepIndex be currentStepIndex plus delta.
    auto target_step_index = target_step_index_for_delta(delta);

    // 4. If allSteps[targetStepIndex] does not exist, then abort these steps.
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

    auto target_top_level_entry_index = top_level_entry_index_for_step(m_entries, step);
    VERIFY(target_top_level_entry_index.has_value());
    auto const* target_top_level_entry = &m_entries[*target_top_level_entry_index];
    auto const* current_top_level_entry = current_entry();
    VERIFY(current_top_level_entry);

    return TraversalTarget {
        .target_step_index = *target_step_index,
        .target_step = step,
        .target_top_level_entry_index = *target_top_level_entry_index,
        .target_top_level_entry = target_top_level_entry,
        .target_step_is_top_level_entry = entry_for_step(step) != nullptr,
        .changes_top_level_entry = target_top_level_entry != current_top_level_entry,
    };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-session-history-entries
Optional<Vector<TraversableSessionHistory::Entry> const&> TraversableSessionHistory::get_session_history_entries(CanonicalNavigable const& navigable) const
{
    // 1. Let traversable be navigable's traversable navigable.
    // NB: The caller has already resolved navigable through its CanonicalTraversable.

    // FIXME: 2. Assert: this is running within traversable's session history traversal queue.

    // 3. If navigable is traversable, return traversable's session history entries.
    if (navigable.is_top_level_traversable())
        return m_entries;

    // 4. Let docStates be an empty ordered set of document states.
    Vector<Web::HTML::SessionHistoryDocumentStateDescriptor const*> document_states;
    OrderedHashTable<Web::HTML::CrossProcessId> document_state_ids;
    auto append_document_state = [&](Web::HTML::SessionHistoryDocumentStateDescriptor const& document_state) {
        if (document_state_ids.set(document_state.id, AK::HashSetExistingEntryBehavior::Keep) == HashSetResult::InsertedNewEntry)
            document_states.append(&document_state);
    };

    // 5. For each entry of traversable's session history entries, append entry's document state to docStates.
    for (auto const& entry : m_entries)
        append_document_state(entry.document_state);

    // 6. For each docState of docStates:
    for (size_t i = 0; i < document_states.size(); ++i) {
        auto const& document_state = *document_states[i];

        // 1. For each nestedHistory of docState's nested histories:
        for (auto const& nested_history : document_state.nested_histories) {
            // 1. If nestedHistory's id equals navigable's id, return nestedHistory's entries.
            if (nested_history.id == navigable.id())
                return nested_history.entries;

            // 2. For each entry of nestedHistory's entries, append entry's document state to docStates.
            for (auto const& entry : nested_history.entries)
                append_document_state(entry.document_state);
        }
    }

    // FIXME: The UI mirror can temporarily lack a newly-created navigable's nested history while WebContent and the
    //        UI process converge. Once navigable creation is ordered with session history updates, apply the
    //        specification's final assertion.
    return {};
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
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-used-step
Optional<i32> TraversableSessionHistory::get_the_used_step(i32 step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    // 2. Return the greatest item in steps that is less than or equal to step.
    Optional<i32> used_step;
    for (auto candidate : m_used_steps) {
        if (candidate <= step && (!used_step.has_value() || candidate > *used_step))
            used_step = candidate;
    }
    return used_step;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-target-history-entry
TraversableSessionHistory::Entry const* TraversableSessionHistory::get_the_target_history_entry(CanonicalNavigable const& navigable, i32 step) const
{
    // 1. Let entries be the result of getting session history entries for navigable.
    auto entries = get_session_history_entries(navigable);
    if (!entries.has_value())
        return nullptr;

    // 2. Return the item in entries that has the greatest step less than or equal to step.
    Entry const* target_entry = nullptr;
    for (auto const& entry : *entries) {
        if (entry.step <= step && (!target_entry || entry.step > target_entry->step))
            target_entry = &entry;
    }
    return target_entry;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-history-object-length-and-index
Optional<Web::HTML::HistoryObjectLengthAndIndex> TraversableSessionHistory::get_the_history_object_length_and_index(i32 step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    // 2. Let scriptHistoryLength be the size of steps.
    auto script_history_length = m_used_steps.size();

    // 3. Assert: steps contains step.
    // AD-HOC: The canonical mirror can be reconciling a removed child navigable, so answer nothing instead of
    //         asserting; the caller treats it as a failed job.
    auto script_history_index = m_used_steps.find_first_index(step);
    if (!script_history_index.has_value())
        return {};

    // 4. Let scriptHistoryIndex be the index of step in steps.
    // 5. Return (scriptHistoryLength, scriptHistoryIndex).
    return Web::HTML::HistoryObjectLengthAndIndex {
        .script_history_length = script_history_length,
        .script_history_index = *script_history_index,
    };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-session-history-entries-for-the-navigation-api
Optional<Vector<TraversableSessionHistory::Entry>> TraversableSessionHistory::get_session_history_entries_for_the_navigation_api(CanonicalNavigable const& navigable, i32 target_step) const
{
    // 1. Let rawEntries be the result of getting session history entries for navigable.
    auto raw_entries = get_session_history_entries(navigable);
    if (!raw_entries.has_value())
        return {};

    // 2. Let entriesForNavigationAPI be a new empty list.
    Vector<Entry> entries_for_navigation_api;

    // 3. Let startingIndex be the index of the session history entry in rawEntries who has the greatest step less
    //    than or equal to targetStep.
    Optional<size_t> starting_index;
    Optional<i32> greatest_step;
    for (size_t i = 0; i < raw_entries->size(); ++i) {
        auto const& entry = raw_entries->at(i);
        if (entry.step <= target_step && (!greatest_step.has_value() || entry.step > *greatest_step)) {
            starting_index = i;
            greatest_step = entry.step;
        }
    }
    if (!starting_index.has_value())
        return entries_for_navigation_api;

    // 4. Append rawEntries[startingIndex] to entriesForNavigationAPI.
    entries_for_navigation_api.append(raw_entries->at(*starting_index));

    // 5. Let startingOrigin be rawEntries[startingIndex]'s document state's origin.
    auto const& starting_entry = raw_entries->at(*starting_index);
    auto const& starting_origin = starting_entry.document_state.origin;

    // 6. Let i be startingIndex − 1.
    auto i = static_cast<i64>(*starting_index) - 1;

    // 7. While i > 0:
    // AD-HOC: Implement "while i >= 0" to avoid dropping a same-origin rawEntries[0].
    //         https://github.com/whatwg/html/issues/12644
    while (i >= 0) {
        auto const& entry = raw_entries->at(static_cast<size_t>(i));

        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto const& entry_origin = entry.document_state.origin;
        if (entry.document_state.id != starting_entry.document_state.id
            && (!starting_origin.has_value() || !entry_origin.has_value()
                || !entry_origin->is_same_origin(*starting_origin))) {
            break;
        }

        // 2. Prepend rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.prepend(entry);

        // 3. Set i to i − 1.
        --i;
    }

    // 8. Set i to startingIndex + 1.
    i = static_cast<i64>(*starting_index) + 1;

    // 9. While i < rawEntries's size:
    while (i < static_cast<i64>(raw_entries->size())) {
        auto const& entry = raw_entries->at(static_cast<size_t>(i));

        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto const& entry_origin = entry.document_state.origin;
        if (entry.document_state.id != starting_entry.document_state.id
            && (!starting_origin.has_value() || !entry_origin.has_value()
                || !entry_origin->is_same_origin(*starting_origin))) {
            break;
        }

        // 2. Append rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.append(entry);

        // 3. Set i to i + 1.
        ++i;
    }

    // 10. Return entriesForNavigationAPI.
    return entries_for_navigation_api;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#get-all-navigables-whose-current-session-history-entry-will-change-or-reload
Vector<Web::HTML::CrossProcessId> TraversableSessionHistory::get_all_navigables_whose_current_session_history_entry_will_change_or_reload(CanonicalNavigable const& traversable, i32 target_step) const
{
    // 1. Let results be an empty list.
    Vector<Web::HTML::CrossProcessId> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<CanonicalNavigable const*> navigables_to_check { &traversable };

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto const* navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto const* target_entry = get_the_target_history_entry(*navigable, target_step);
        if (!target_entry)
            continue;

        // 2. If targetEntry is not navigable's current session history entry or targetEntry's document state's reload
        //    pending is true, then append navigable to results.
        if (!navigable->current_session_history_entry_is(*target_entry) || target_entry->document_state.reload_pending)
            results.append(navigable->id());

        // 3. If targetEntry's document is navigable's document, and targetEntry's document state's reload pending is
        //    false, then extend navigablesToCheck with the child navigables of navigable.
        if (navigable->active_document_is(*target_entry) && !target_entry->document_state.reload_pending) {
            for (auto const& child : navigable->children())
                navigables_to_check.append(child.ptr());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-might-experience-a-cross-document-traversal
Vector<Web::HTML::CrossProcessId> TraversableSessionHistory::get_all_navigables_that_might_experience_a_cross_document_traversal(CanonicalNavigable const& traversable, i32 target_step) const
{
    // 1. Let results be an empty list.
    Vector<Web::HTML::CrossProcessId> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<CanonicalNavigable const*> navigables_to_check { &traversable };

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto const* navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto const* target_entry = get_the_target_history_entry(*navigable, target_step);
        if (!target_entry)
            continue;

        // 2. If targetEntry's document is not navigable's document or targetEntry's document state's reload pending
        //    is true, then append navigable to results.
        if (!navigable->active_document_is(*target_entry) || target_entry->document_state.reload_pending) {
            results.append(navigable->id());
        }

        // 3. Otherwise, extend navigablesToCheck with navigable's child navigables.
        else {
            for (auto const& child : navigable->children())
                navigables_to_check.append(child.ptr());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-only-need-history-object-length/index-update
Vector<Web::HTML::CrossProcessId> TraversableSessionHistory::get_all_navigables_that_only_need_history_object_length_index_update(CanonicalNavigable const& traversable, i32 target_step) const
{
    // 1. Let results be an empty list.
    Vector<Web::HTML::CrossProcessId> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<CanonicalNavigable const*> navigables_to_check { &traversable };

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto const* navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto const* target_entry = get_the_target_history_entry(*navigable, target_step);
        if (!target_entry)
            continue;

        // 2. If targetEntry is navigable's current session history entry and targetEntry's document state's reload
        //    pending is false:
        if (navigable->current_session_history_entry_is(*target_entry) && !target_entry->document_state.reload_pending) {
            // 1. Append navigable to results.
            results.append(navigable->id());

            // 2. Extend navigablesToCheck with navigable's child navigables.
            for (auto const& child : navigable->children())
                navigables_to_check.append(child.ptr());
        }
    }

    // 4. Return results.
    return results;
}

}
