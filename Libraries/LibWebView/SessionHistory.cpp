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
static Vector<i32> get_all_used_history_steps(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const& traversable_session_history_entries)
{
    // 1. Assert: this is running within traversable's session history traversal queue.

    // 2. Let steps be an empty ordered set of non-negative integers.
    OrderedHashTable<i32> steps;

    // 3. Let entryLists be the ordered set « traversable's session history entries ».
    Vector<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const*> entry_lists { &traversable_session_history_entries };

    // 4. For each entryList of entryLists:
    for (size_t i = 0; i < entry_lists.size(); ++i) {
        auto const* entry_list = entry_lists[i];

        // 1. For each entry of entryList:
        for (auto const& entry : *entry_list) {
            // 1. Append entry's step to steps.
            steps.set(entry->step);

            // 2. For each nestedHistory of entry's document state's nested histories, append
            //    nestedHistory's entries list to entryLists.
            for (auto const& nested_history : entry->document_state->nested_histories) {
                if (!entry_lists.contains_slow(&nested_history.entries))
                    entry_lists.append(&nested_history.entries);
            }
        }
    }

    // 5. Return steps, sorted.
    auto sorted_steps = steps.values();
    quick_sort(sorted_steps);
    return sorted_steps;
}

static bool entries_have_nested_histories(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const& entries)
{
    for (auto const& entry : entries) {
        if (!entry->document_state->nested_histories.is_empty())
            return true;
    }
    return false;
}

static Optional<size_t> top_level_entry_index_for_step(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const& entries, i32 step)
{
    Optional<size_t> result;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i]->step > step)
            break;
        result = i;
    }
    return result;
}

static ErrorOr<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>> entries_from_descriptors(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& descriptors)
{
    CanonicalSessionHistoryEntry::DocumentStates document_states;
    Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> entries;
    entries.ensure_capacity(descriptors.size());
    for (auto const& descriptor : descriptors)
        entries.unchecked_append(TRY(CanonicalSessionHistoryEntry::create_from_descriptor(descriptor, document_states)));
    return entries;
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

static bool entries_are_valid(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const& entries)
{
    Optional<i32> previous_step;
    for (auto const& entry : entries) {
        if (entry->step < 0)
            return false;
        if (previous_step.has_value() && entry->step <= *previous_step)
            return false;
        for (auto const& nested_history : entry->document_state->nested_histories) {
            if (!entries_are_valid(nested_history.entries))
                return false;
        }
        previous_step = entry->step;
    }
    return true;
}

static bool nesting_depth_is_valid(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, size_t depth = 0)
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

static CanonicalSessionHistoryEntry* entry_for_step_in_entry_list(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const& entries, i32 step)
{
    CanonicalSessionHistoryEntry* result = nullptr;
    for (auto const& entry : entries) {
        if (entry->step > step)
            break;
        result = entry.ptr();
    }
    return result;
}

// A step found only under an inactive nested sibling (off the greatest-step<=target path) is not reachable.
static bool active_path_reaches_step(CanonicalSessionHistoryEntry const& active_entry, i32 step)
{
    if (active_entry.step == step)
        return true;
    for (auto const& nested_history : active_entry.document_state->nested_histories) {
        auto const* active_nested_entry = entry_for_step_in_entry_list(nested_history.entries, step);
        if (active_nested_entry && active_path_reaches_step(*active_nested_entry, step))
            return true;
    }
    return false;
}

ErrorOr<void> validate_snapshot_is_restorable(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& descriptors, Vector<i32> const& used_steps, size_t current_used_step_index)
{
    if (descriptors.is_empty() || used_steps.is_empty() || current_used_step_index >= used_steps.size() || !nesting_depth_is_valid(descriptors))
        return Error::from_string_literal("Session history snapshot is structurally invalid");

    auto entries_or_error = entries_from_descriptors(descriptors);
    if (entries_or_error.is_error())
        return Error::from_string_literal("Session history snapshot is structurally invalid");
    auto entries = entries_or_error.release_value();
    if (!entries_are_valid(entries) || !steps_are_valid(used_steps) || get_all_used_history_steps(entries) != used_steps)
        return Error::from_string_literal("Session history snapshot is structurally invalid");

    for (auto step : used_steps) {
        auto top_level_entry_index = top_level_entry_index_for_step(entries, step);
        if (!top_level_entry_index.has_value() || !active_path_reaches_step(*entries[*top_level_entry_index], step))
            return Error::from_string_literal("Session history snapshot has a used step that is not reachable");
    }

    auto current_top_level_entry_index = top_level_entry_index_for_step(entries, used_steps[current_used_step_index]);
    if (!entries[*current_top_level_entry_index]->document_state->ever_populated)
        return Error::from_string_literal("Session history snapshot's current entry has no document state");

    return {};
}

static void clear_forward_session_history_entries(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>& entries, i32 step)
{
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#clear-the-forward-session-history

    // 1. Assert: this is running within navigable's session history traversal queue.

    // 2. Let step be the navigable's current session history step.

    // 3. Let entryLists be the ordered set « navigable's session history entries ».
    Vector<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>*> entry_lists { &entries };

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto* entry_list = entry_lists.take_first();

        // 1. Remove every session history entry from entryList that has a step greater than step.
        entry_list->remove_all_matching([step](auto const& entry) {
            return entry->step > step;
        });

        // 2. For each entry of entryList:
        for (auto& entry : *entry_list) {
            // 1. For each nestedHistory of entry's document state's nested histories, append
            //    nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry->document_state->nested_histories) {
                if (!entry_lists.contains_slow(&nested_history.entries))
                    entry_lists.append(&nested_history.entries);
            }
        }
    }
}

void TraversableSessionHistory::clear()
{
    m_entries.clear();
    m_current_session_history_step.clear();
}

bool TraversableSessionHistory::initialize_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    if (entries.is_empty() || current_used_step_index >= used_steps.size())
        return false;
    auto canonical_entries = entries_from_descriptors(entries);
    if (canonical_entries.is_error() || get_all_used_history_steps(canonical_entries.value()) != used_steps)
        return false;

    m_entries = canonical_entries.release_value();
    m_current_session_history_step = used_steps[current_used_step_index];
    return true;
}

void TraversableSessionHistory::initialize_with_initial_history_entry(Web::HTML::SessionHistoryEntryDescriptor const& initial_history_entry)
{
    m_entries.append(MUST(CanonicalSessionHistoryEntry::create_from_descriptor(initial_history_entry)));
    m_current_session_history_step = 0;
}

static void assign_fresh_ids_to_restored_entries(Vector<Web::HTML::SessionHistoryEntryDescriptor>& entries, Function<Web::HTML::CrossProcessId()> const& allocate_cross_process_id, HashMap<Web::HTML::CrossProcessId, Web::HTML::CrossProcessId>& assigned_ids)
{
    for (auto& entry : entries) {
        entry.document_state.id = assigned_ids.ensure(entry.document_state.id, [&] { return allocate_cross_process_id(); });
        for (auto& nested_history : entry.document_state.nested_histories) {
            nested_history.id = assigned_ids.ensure(nested_history.id, [&] { return allocate_cross_process_id(); });
            assign_fresh_ids_to_restored_entries(nested_history.entries, allocate_cross_process_id, assigned_ids);
        }
    }
}

ErrorOr<void> TraversableSessionHistory::restore_from_ui_snapshot(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, Function<Web::HTML::CrossProcessId()> allocate_cross_process_id)
{
    TRY(validate_snapshot_is_restorable(entries, used_steps, current_used_step_index));

    HashMap<Web::HTML::CrossProcessId, Web::HTML::CrossProcessId> assigned_ids;
    assign_fresh_ids_to_restored_entries(entries, allocate_cross_process_id, assigned_ids);
    m_entries = TRY(entries_from_descriptors(entries));
    m_current_session_history_step = used_steps[current_used_step_index];
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
    m_entries[*current_top_level_entry_index]->document_state->reload_pending = true;
}

static void collect_document_states(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const& entries, CanonicalSessionHistoryEntry::DocumentStates& document_states)
{
    for (auto const& entry : entries) {
        if (document_states.set(entry->document_state->id, entry->document_state, AK::HashSetExistingEntryBehavior::Keep) != HashSetResult::InsertedNewEntry)
            continue;
        for (auto const& nested_history : entry->document_state->nested_histories)
            collect_document_states(nested_history.entries, document_states);
    }
}

CanonicalSessionHistoryEntry::DocumentStates TraversableSessionHistory::document_states() const
{
    CanonicalSessionHistoryEntry::DocumentStates document_states;
    collect_document_states(m_entries, document_states);
    return document_states;
}

RefPtr<CanonicalDocumentState> TraversableSessionHistory::find_document_state(Web::HTML::CrossProcessId document_state_id) const
{
    return document_states().get(document_state_id).value_or(nullptr);
}

// The entry lists of a navigable's nested histories, in document states that entries share.
static void collect_nested_session_history_entries(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>& entries, Web::HTML::CrossProcessId navigable_id, Vector<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>*>& entry_lists)
{
    for (auto& entry : entries) {
        for (auto& nested_history : entry->document_state->nested_histories) {
            if (nested_history.id == navigable_id && !entry_lists.contains_slow(&nested_history.entries))
                entry_lists.append(&nested_history.entries);
            collect_nested_session_history_entries(nested_history.entries, navigable_id, entry_lists);
        }
    }
}

static Vector<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>*> entry_lists_for(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>& entries, Optional<Web::HTML::CrossProcessId> nested_history_id)
{
    if (!nested_history_id.has_value())
        return { &entries };
    Vector<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>*> entry_lists;
    collect_nested_session_history_entries(entries, *nested_history_id, entry_lists);
    return entry_lists;
}

Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>* TraversableSessionHistory::nested_session_history_entries_for_navigable(Web::HTML::CrossProcessId navigable_id)
{
    auto entry_lists = entry_lists_for(m_entries, navigable_id);
    if (entry_lists.is_empty())
        return nullptr;
    return entry_lists.first();
}

bool TraversableSessionHistory::update_entry(Optional<Web::HTML::CrossProcessId> nested_history_id, Utf16String const& navigation_api_key, Function<void(CanonicalSessionHistoryEntry&)> const& update_entry)
{
    auto did_update = false;
    for (auto* entries : entry_lists_for(m_entries, nested_history_id)) {
        for (auto& entry : *entries) {
            if (entry->navigation_api_key == navigation_api_key) {
                update_entry(*entry);
                did_update = true;
            }
        }
    }
    return did_update;
}

bool TraversableSessionHistory::update_entry(Optional<Web::HTML::CrossProcessId> nested_history_id, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Function<void(CanonicalSessionHistoryEntry&)> const& update_entry)
{
    auto did_update = false;
    for (auto* entries : entry_lists_for(m_entries, nested_history_id)) {
        for (auto& entry : *entries) {
            if (entry->identity() == entry_identity) {
                update_entry(*entry);
                did_update = true;
            }
        }
    }
    return did_update;
}

bool TraversableSessionHistory::update_entry_persisted_state(Optional<Web::HTML::CrossProcessId> nested_history_id, Web::HTML::SessionHistoryEntryPersistedState const& persisted_state)
{
    return update_entry(nested_history_id, persisted_state.entry_identity, [&](auto& entry) {
        entry.scroll_position_data = persisted_state.scroll_position_data;
    });
}

bool TraversableSessionHistory::update_document_state(Optional<Web::HTML::CrossProcessId> nested_history_id, Utf16String const& navigation_api_key, Function<void(CanonicalDocumentState&)> const& update_document_state)
{
    Vector<CanonicalDocumentState*> document_states;
    for (auto* entries : entry_lists_for(m_entries, nested_history_id)) {
        RefPtr<CanonicalDocumentState> document_state;
        for (auto const& entry : *entries) {
            if (entry->navigation_api_key == navigation_api_key)
                document_state = entry->document_state;
        }
        if (document_state && !document_states.contains_slow(document_state.ptr()))
            document_states.append(document_state.ptr());
    }
    for (auto* document_state : document_states)
        update_document_state(*document_state);
    return !document_states.is_empty();
}

bool TraversableSessionHistory::update_document_state(Web::HTML::CrossProcessId document_state_id, Function<void(CanonicalDocumentState&)> const& update_document_state)
{
    auto document_state = find_document_state(document_state_id);
    if (!document_state)
        return false;
    update_document_state(*document_state);
    return true;
}

Optional<i32> TraversableSessionHistory::append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id, Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry)
{
    if (!m_current_session_history_step.has_value())
        return {};

    // https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-new-child-navigable
    // These are steps 1-6 of the traversal steps appended by "create a new child navigable". WebContent supplies the
    // identity of parentDocState, whose live object it obtained from parentNavigable's active entry. The canonical
    // entry list supplies targetStepSHE and therefore owns the concrete step assigned here.
    auto* parent_entries = parent_navigable.is_top_level_traversable()
        ? &m_entries
        : nested_session_history_entries_for_navigable(parent_navigable.id());
    if (!parent_entries)
        return {};

    auto target_step_entry = parent_entries->find_if([&](auto const& entry) {
        return entry->document_state->id == parent_document_state_id;
    });
    if (target_step_entry == parent_entries->end())
        return {};

    auto target_step = (*target_step_entry)->step;
    auto& parent_document_state = *(*target_step_entry)->document_state;

    // Append nestedHistory to parentDocState's nested histories.
    auto existing_nested_history = parent_document_state.nested_histories.find_if([&](auto const& existing_nested_history) {
        return existing_nested_history.id == child_navigable_id;
    });
    if (existing_nested_history == parent_document_state.nested_histories.end()) {
        auto entry = CanonicalSessionHistoryEntry::create_from_descriptor(Web::HTML::create_session_history_entry_descriptor(move(initial_history_entry), target_step));
        if (entry.is_error())
            return {};
        parent_document_state.nested_histories.append({ .id = child_navigable_id, .entries = { entry.release_value() } });
    }

    return target_step;
}

bool TraversableSessionHistory::remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id)
{
    if (!m_current_session_history_step.has_value())
        return false;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-child-navigable
    // Let parentDocState be container's node navigable's active session history entry's document state. The live
    // parent entry was read before these traversal steps were appended, so use the reported stable document-state
    // identity instead of resolving the UI's current step again when the IPC request arrives.
    auto* parent_entries = parent_navigable.is_top_level_traversable()
        ? &m_entries
        : nested_session_history_entries_for_navigable(parent_navigable.id());
    if (!parent_entries)
        return false;
    auto parent_entry = parent_entries->find_if([&](auto const& entry) { return entry->document_state->id == parent_document_state_id; });
    if (parent_entry == parent_entries->end())
        return false;

    // Remove the nested history from parentDocState's nested histories whose id equals navigable's id.
    (*parent_entry)->document_state->nested_histories.remove_all_matching([child_navigable_id](auto const& nested_history) {
        return nested_history.id == child_navigable_id;
    });

    return true;
}

void TraversableSessionHistory::clear_the_forward_session_history()
{
    VERIFY(m_current_session_history_step.has_value());
    clear_forward_session_history_entries(m_entries, *m_current_session_history_step);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation, step 9
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation, step 5
Optional<i32> TraversableSessionHistory::push_session_history_entry(CanonicalNavigable const& navigable, NonnullRefPtr<CanonicalSessionHistoryEntry> entry)
{
    auto current_step = this->current_step();
    if (!current_step.has_value())
        return {};

    // NB: The entries of a navigable whose document is active are among those clearing the forward session history
    //     keeps, as the entry of the document containing it is at or before the current step.
    //     FIXME: Assert that instead, once navigable creation and destruction are ordered with the session history.
    if (!session_history_entries_at_or_before(navigable, *current_step).has_value())
        return {};

    // 1. Clear the forward session history of traversable.
    clear_the_forward_session_history();

    // 2. Set targetStep to traversable's current session history step + 1.
    VERIFY(*current_step < NumericLimits<i32>::max());
    auto target_step = *current_step + 1;

    // 3. Set historyEntry's step to targetStep.
    entry->step = target_step;

    // 4. Append historyEntry to targetEntries.
    auto target_entries = get_session_history_entries(navigable);
    VERIFY(target_entries.has_value());
    // NB: targetEntries is a list of this session history.
    const_cast<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>&>(*target_entries).append(move(entry));

    return target_step;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation, step 10
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation, step 5
bool TraversableSessionHistory::replace_session_history_entry(CanonicalNavigable const& navigable, CanonicalSessionHistoryEntry const& entry_to_replace, NonnullRefPtr<CanonicalSessionHistoryEntry> entry)
{
    auto target_entries = get_session_history_entries(navigable);
    if (!target_entries.has_value())
        return false;
    // NB: targetEntries is a list of this session history.
    auto& entries = const_cast<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>>&>(*target_entries);
    auto existing_entry = entries.find_if([&](auto const& entry) { return entry.ptr() == &entry_to_replace; });
    if (existing_entry == entries.end())
        return false;

    // 1. Replace entryToReplace with historyEntry in targetEntries.
    // 2. Set historyEntry's step to entryToReplace's step.
    entry->step = entry_to_replace.step;
    *existing_entry = move(entry);

    return true;
}

Optional<size_t> TraversableSessionHistory::current_top_level_entry_index() const
{
    if (!m_current_session_history_step.has_value())
        return {};
    return top_level_entry_index_for_step(m_entries, *m_current_session_history_step);
}

Optional<size_t> TraversableSessionHistory::current_used_step_index() const
{
    if (!m_current_session_history_step.has_value())
        return {};
    auto used_step = get_the_used_step(*m_current_session_history_step);
    if (!used_step.has_value())
        return {};
    return used_steps().find_first_index(*used_step);
}

Vector<Web::HTML::SessionHistoryEntryDescriptor> TraversableSessionHistory::entries() const
{
    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries;
    entries.ensure_capacity(m_entries.size());
    for (auto const& entry : m_entries)
        entries.unchecked_append(entry->descriptor());
    return entries;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
Vector<i32> TraversableSessionHistory::used_steps() const
{
    return get_all_used_history_steps(m_entries);
}

bool TraversableSessionHistory::can_go_back() const
{
    auto current_used_step_index = this->current_used_step_index();
    return current_used_step_index.has_value() && *current_used_step_index > 0;
}

bool TraversableSessionHistory::can_go_forward() const
{
    auto current_used_step_index = this->current_used_step_index();
    return current_used_step_index.has_value() && *current_used_step_index + 1 < used_step_count();
}

bool TraversableSessionHistory::has_only_top_level_used_steps() const
{
    if (entries_have_nested_histories(m_entries))
        return false;

    auto used_steps = this->used_steps();
    if (m_entries.size() != used_steps.size())
        return false;

    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i]->step != used_steps[i])
            return false;
    }
    return true;
}

Optional<TraversableSessionHistory::TraversalTarget> TraversableSessionHistory::traversal_target_for_delta(int delta) const
{
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta

    // 1. Let allSteps be the result of getting all used history steps for traversable.

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
    auto target_step_index = used_steps().find_first_index(step);
    if (!target_step_index.has_value())
        return {};

    auto target_top_level_entry_index = top_level_entry_index_for_step(m_entries, step);
    VERIFY(target_top_level_entry_index.has_value());
    auto const* target_top_level_entry = m_entries[*target_top_level_entry_index].ptr();
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
static Optional<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const&> get_session_history_entries(Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const& traversable_session_history_entries, CanonicalNavigable const& navigable, Optional<i32> at_or_before_step)
{
    // 1. Let traversable be navigable's traversable navigable.
    // NB: The caller has already resolved navigable through its CanonicalTraversable.

    // FIXME: 2. Assert: this is running within traversable's session history traversal queue.

    // 3. If navigable is traversable, return traversable's session history entries.
    if (navigable.is_top_level_traversable())
        return traversable_session_history_entries;

    // 4. Let docStates be an empty ordered set of document states.
    Vector<CanonicalDocumentState const*> document_states;
    auto append_document_state = [&](CanonicalSessionHistoryEntry const& entry) {
        if (at_or_before_step.has_value() && entry.step > *at_or_before_step)
            return;
        if (!document_states.contains_slow(entry.document_state.ptr()))
            document_states.append(entry.document_state.ptr());
    };

    // 5. For each entry of traversable's session history entries, append entry's document state to docStates.
    for (auto const& entry : traversable_session_history_entries)
        append_document_state(*entry);

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
                append_document_state(*entry);
        }
    }

    // FIXME: The UI mirror can temporarily lack a newly-created navigable's nested history while WebContent and the
    //        UI process converge. Once navigable creation is ordered with session history updates, apply the
    //        specification's final assertion.
    return {};
}

Optional<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const&> TraversableSessionHistory::get_session_history_entries(CanonicalNavigable const& navigable) const
{
    return WebView::get_session_history_entries(m_entries, navigable, {});
}

Optional<Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> const&> TraversableSessionHistory::session_history_entries_at_or_before(CanonicalNavigable const& navigable, i32 step) const
{
    return WebView::get_session_history_entries(m_entries, navigable, step);
}

Optional<size_t> TraversableSessionHistory::target_step_index_for_delta(int delta) const
{
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
    // Let allSteps be the result of getting all used history steps. Let
    // targetStepIndex be currentStepIndex plus delta. If allSteps[targetStepIndex]
    // does not exist, then abort these steps.
    auto current_used_step_index = this->current_used_step_index();
    if (!current_used_step_index.has_value() || delta == 0)
        return {};

    if (delta < 0) {
        auto magnitude = static_cast<size_t>(-static_cast<i64>(delta));
        if (magnitude > *current_used_step_index)
            return {};
        return *current_used_step_index - magnitude;
    }

    auto target_index = *current_used_step_index + static_cast<size_t>(delta);
    if (target_index >= used_step_count())
        return {};
    return target_index;
}

Optional<i32> TraversableSessionHistory::step_at(size_t index) const
{
    auto used_steps = this->used_steps();
    if (index >= used_steps.size())
        return {};
    return used_steps[index];
}

CanonicalSessionHistoryEntry* TraversableSessionHistory::current_entry() const
{
    if (!m_current_session_history_step.has_value())
        return nullptr;
    return top_level_entry_for_step(*m_current_session_history_step);
}

CanonicalSessionHistoryEntry* TraversableSessionHistory::entry_at(size_t index) const
{
    if (index >= m_entries.size())
        return nullptr;
    return m_entries[index].ptr();
}

CanonicalSessionHistoryEntry* TraversableSessionHistory::entry_for_step(i32 step) const
{
    for (auto const& entry : m_entries) {
        if (entry->step == step)
            return entry.ptr();
    }
    return nullptr;
}

CanonicalSessionHistoryEntry* TraversableSessionHistory::top_level_entry_for_step(i32 step) const
{
    auto index = top_level_entry_index_for_step(m_entries, step);
    if (!index.has_value())
        return nullptr;
    return m_entries[*index].ptr();
}

void TraversableSessionHistory::traverse_to(size_t index)
{
    auto used_steps = this->used_steps();
    VERIFY(index < used_steps.size());
    m_current_session_history_step = used_steps[index];
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-used-step
Optional<i32> TraversableSessionHistory::get_the_used_step(i32 step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    // 2. Return the greatest item in steps that is less than or equal to step.
    Optional<i32> used_step;
    for (auto candidate : used_steps()) {
        if (candidate <= step && (!used_step.has_value() || candidate > *used_step))
            used_step = candidate;
    }
    return used_step;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-target-history-entry
CanonicalSessionHistoryEntry* TraversableSessionHistory::get_the_target_history_entry(CanonicalNavigable const& navigable, i32 step) const
{
    // 1. Let entries be the result of getting session history entries for navigable.
    auto entries = get_session_history_entries(navigable);
    if (!entries.has_value())
        return nullptr;

    // 2. Return the item in entries that has the greatest step less than or equal to step.
    CanonicalSessionHistoryEntry* target_entry = nullptr;
    for (auto const& entry : *entries) {
        if (entry->step <= step && (!target_entry || entry->step > target_entry->step))
            target_entry = entry.ptr();
    }
    return target_entry;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-history-object-length-and-index
Optional<Web::HTML::HistoryObjectLengthAndIndex> TraversableSessionHistory::get_the_history_object_length_and_index(i32 step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    auto steps = used_steps();

    // 2. Let scriptHistoryLength be the size of steps.
    auto script_history_length = steps.size();

    // 3. Assert: steps contains step.
    // AD-HOC: The canonical mirror can be reconciling a removed child navigable, so answer nothing instead of
    //         asserting; the caller treats it as a failed job.
    auto script_history_index = steps.find_first_index(step);
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
Optional<Vector<Web::HTML::SessionHistoryEntryDescriptor>> TraversableSessionHistory::get_session_history_entries_for_the_navigation_api(CanonicalNavigable const& navigable, i32 target_step) const
{
    // 1. Let rawEntries be the result of getting session history entries for navigable.
    auto raw_entries = get_session_history_entries(navigable);
    if (!raw_entries.has_value())
        return {};

    // 2. Let entriesForNavigationAPI be a new empty list.
    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries_for_navigation_api;

    // 3. Let startingIndex be the index of the session history entry in rawEntries who has the greatest step less
    //    than or equal to targetStep.
    Optional<size_t> starting_index;
    Optional<i32> greatest_step;
    for (size_t i = 0; i < raw_entries->size(); ++i) {
        auto const& entry = raw_entries->at(i);
        if (entry->step <= target_step && (!greatest_step.has_value() || entry->step > *greatest_step)) {
            starting_index = i;
            greatest_step = entry->step;
        }
    }
    if (!starting_index.has_value())
        return entries_for_navigation_api;

    // 4. Append rawEntries[startingIndex] to entriesForNavigationAPI.
    entries_for_navigation_api.append(raw_entries->at(*starting_index)->descriptor());

    // 5. Let startingOrigin be rawEntries[startingIndex]'s document state's origin.
    auto const& starting_entry = *raw_entries->at(*starting_index);
    auto const& starting_origin = starting_entry.document_state->origin;

    // 6. Let i be startingIndex − 1.
    auto i = static_cast<i64>(*starting_index) - 1;

    // 7. While i > 0:
    // AD-HOC: Implement "while i >= 0" to avoid dropping a same-origin rawEntries[0].
    //         https://github.com/whatwg/html/issues/12644
    while (i >= 0) {
        auto const& entry = *raw_entries->at(static_cast<size_t>(i));

        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto const& entry_origin = entry.document_state->origin;
        if (entry.document_state != starting_entry.document_state
            && (!starting_origin.has_value() || !entry_origin.has_value()
                || !entry_origin->is_same_origin(*starting_origin))) {
            break;
        }

        // 2. Prepend rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.prepend(entry.descriptor());

        // 3. Set i to i − 1.
        --i;
    }

    // 8. Set i to startingIndex + 1.
    i = static_cast<i64>(*starting_index) + 1;

    // 9. While i < rawEntries's size:
    while (i < static_cast<i64>(raw_entries->size())) {
        auto const& entry = *raw_entries->at(static_cast<size_t>(i));

        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto const& entry_origin = entry.document_state->origin;
        if (entry.document_state != starting_entry.document_state
            && (!starting_origin.has_value() || !entry_origin.has_value()
                || !entry_origin->is_same_origin(*starting_origin))) {
            break;
        }

        // 2. Append rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.append(entry.descriptor());

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
        if (!navigable->current_session_history_entry_is(*target_entry) || target_entry->document_state->reload_pending)
            results.append(navigable->id());

        // 3. If targetEntry's document is navigable's document, and targetEntry's document state's reload pending is
        //    false, then extend navigablesToCheck with the child navigables of navigable.
        if (navigable->active_document_is(*target_entry) && !target_entry->document_state->reload_pending) {
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
        if (!navigable->active_document_is(*target_entry) || target_entry->document_state->reload_pending) {
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
        if (navigable->current_session_history_entry_is(*target_entry) && !target_entry->document_state->reload_pending) {
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
