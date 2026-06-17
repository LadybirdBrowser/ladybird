/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <LibWebView/SessionHistory.h>

namespace WebView {

struct SessionHistoryMergeAnchor {
    size_t local_index { 0 };
    size_t incoming_index { 0 };
};

enum class StepTranslationMode {
    TopLevel,
    Nested,
};

struct DocumentStateIdTranslationState {
    HashMap<u64, u64> translated_document_state_ids;
    u64 next_document_state_id { 1 };
};

struct DocumentStateIdCanonicalizationState {
    HashMap<u64, u64> canonical_document_state_ids;
    u64 next_document_state_id { 1 };
};

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

static bool entries_match(Vector<TraversableSessionHistory::Entry> const& a, Vector<TraversableSessionHistory::Entry> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (Web::HTML::session_history_entry_descriptors_match(a[i], b[i]))
            continue;
        return false;
    }
    return true;
}

static bool seed_ack_nested_histories_match(Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const&, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const&, Optional<size_t>);

static bool current_unknown_entry_seed_ack_matches(TraversableSessionHistory::Entry const& a, TraversableSessionHistory::Entry const& b)
{
    if (a.step != b.step || a.url != b.url)
        return false;
    if (!a.classic_history_api_state.is_empty() && a.classic_history_api_state != b.classic_history_api_state)
        return false;
    if (!a.navigation_api_state.is_empty() && a.navigation_api_state != b.navigation_api_state)
        return false;
    if (!a.navigation_api_key.is_empty() && a.navigation_api_key != b.navigation_api_key)
        return false;
    if (!a.navigation_api_id.is_empty() && a.navigation_api_id != b.navigation_api_id)
        return false;
    if (a.scroll_restoration_mode != b.scroll_restoration_mode)
        return false;
    if (a.scroll_position_data.viewport_scroll_position.has_value() && a.scroll_position_data != b.scroll_position_data)
        return false;
    return seed_ack_nested_histories_match(a.document_state.nested_histories, b.document_state.nested_histories, {});
}

static bool seed_ack_entries_match(Vector<TraversableSessionHistory::Entry> const& a, Vector<TraversableSessionHistory::Entry> const& b, Optional<size_t> current_unknown_entry_index)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (current_unknown_entry_index.has_value() && i == *current_unknown_entry_index && current_unknown_entry_seed_ack_matches(a[i], b[i]))
            continue;
        if (Web::HTML::session_history_entry_descriptors_match_ignoring_document_state_id(a[i], b[i]))
            continue;
        return false;
    }
    return true;
}

static bool seed_ack_nested_histories_match(Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const& a, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> const& b, Optional<size_t> current_unknown_entry_index)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id == b[i].id && seed_ack_entries_match(a[i].entries, b[i].entries, current_unknown_entry_index))
            continue;
        return false;
    }
    return true;
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

static bool entry_steps_match(TraversableSessionHistory::Entry const& a, TraversableSessionHistory::Entry const& b)
{
    if (a.step != b.step)
        return false;
    if (a.document_state.nested_histories.size() != b.document_state.nested_histories.size())
        return false;

    for (size_t i = 0; i < a.document_state.nested_histories.size(); ++i) {
        auto const& a_nested_history = a.document_state.nested_histories[i];
        auto const& b_nested_history = b.document_state.nested_histories[i];
        if (a_nested_history.id != b_nested_history.id || a_nested_history.entries.size() != b_nested_history.entries.size())
            return false;
        for (size_t j = 0; j < a_nested_history.entries.size(); ++j) {
            if (!entry_steps_match(a_nested_history.entries[j], b_nested_history.entries[j]))
                return false;
        }
    }

    return true;
}

static bool entries_have_matching_nonzero_document_state_id(TraversableSessionHistory::Entry const& a, TraversableSessionHistory::Entry const& b)
{
    return a.document_state.id != 0 && a.document_state.id == b.document_state.id;
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

static TraversableSessionHistory::Entry const* top_level_entry_for_step(Vector<TraversableSessionHistory::Entry> const& entries, i32 step)
{
    auto index = top_level_entry_index_for_step(entries, step);
    if (!index.has_value())
        return nullptr;
    return &entries[*index];
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

static void update_next_document_state_id(Vector<TraversableSessionHistory::Entry> const& entries, u64& next_document_state_id)
{
    for (auto const& entry : entries) {
        if (entry.document_state.id >= next_document_state_id)
            next_document_state_id = entry.document_state.id + 1;
        for (auto const& nested_history : entry.document_state.nested_histories)
            update_next_document_state_id(nested_history.entries, next_document_state_id);
    }
}

static u64 translate_incoming_document_state_id(u64 incoming_document_state_id, DocumentStateIdTranslationState& translation_state)
{
    if (incoming_document_state_id == 0)
        return 0;

    if (auto translated_document_state_id = translation_state.translated_document_state_ids.get(incoming_document_state_id); translated_document_state_id.has_value())
        return *translated_document_state_id;

    auto translated_document_state_id = translation_state.next_document_state_id++;
    VERIFY(translated_document_state_id != 0);
    translation_state.translated_document_state_ids.set(incoming_document_state_id, translated_document_state_id);
    return translated_document_state_id;
}

static void canonicalize_document_state_ids(Vector<TraversableSessionHistory::Entry>& entries, DocumentStateIdCanonicalizationState& canonicalization_state)
{
    for (auto& entry : entries) {
        if (entry.document_state.id != 0) {
            auto canonical_document_state_id = canonicalization_state.canonical_document_state_ids.get(entry.document_state.id);
            if (canonical_document_state_id.has_value()) {
                entry.document_state.id = *canonical_document_state_id;
            } else {
                auto new_document_state_id = canonicalization_state.next_document_state_id++;
                VERIFY(new_document_state_id != 0);
                canonicalization_state.canonical_document_state_ids.set(entry.document_state.id, new_document_state_id);
                entry.document_state.id = new_document_state_id;
            }
        }

        for (auto& nested_history : entry.document_state.nested_histories)
            canonicalize_document_state_ids(nested_history.entries, canonicalization_state);
    }
}

static void canonicalize_document_state_ids(Vector<TraversableSessionHistory::Entry>& entries)
{
    DocumentStateIdCanonicalizationState canonicalization_state;
    canonicalize_document_state_ids(entries, canonicalization_state);
}

static Optional<i32> translate_incoming_step(i32 incoming_step, i32 incoming_anchor_step, i32 local_anchor_step, StepTranslationMode mode = StepTranslationMode::TopLevel, Optional<i32> nested_history_step_floor = {})
{
    // AD-HOC: WebContent snapshots can describe only the history slice known to the current process. When merging
    //         such a partial snapshot into the UI-owned traversable session history mirror, translate the incoming
    //         step coordinates around the entry that anchors both histories.
    if (mode == StepTranslationMode::Nested && incoming_step < incoming_anchor_step && (!nested_history_step_floor.has_value() || incoming_step < *nested_history_step_floor))
        return local_anchor_step;

    auto step_delta = static_cast<i64>(incoming_step) - static_cast<i64>(incoming_anchor_step);
    if (step_delta < 0) {
        if (mode != StepTranslationMode::Nested || !nested_history_step_floor.has_value() || incoming_step < *nested_history_step_floor)
            return {};
        if (-step_delta > local_anchor_step)
            return {};
    }
    if (step_delta > NumericLimits<i32>::max() - local_anchor_step)
        return {};
    return local_anchor_step + static_cast<i32>(step_delta);
}

static Optional<Web::HTML::SessionHistoryNestedHistoryDescriptor> translate_incoming_nested_history(Web::HTML::SessionHistoryNestedHistoryDescriptor const&, i32 incoming_anchor_step, i32 local_anchor_step, DocumentStateIdTranslationState&, Optional<i32> nested_history_step_floor);

static Optional<Web::HTML::SessionHistoryDocumentStateDescriptor> translate_incoming_document_state_descriptor(Web::HTML::SessionHistoryDocumentStateDescriptor document_state, i32 incoming_anchor_step, i32 local_anchor_step, DocumentStateIdTranslationState& document_state_id_translation_state, Optional<i32> nested_history_step_floor)
{
    document_state.id = translate_incoming_document_state_id(document_state.id, document_state_id_translation_state);

    Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> nested_histories;
    nested_histories.ensure_capacity(document_state.nested_histories.size());
    for (auto const& nested_history : document_state.nested_histories) {
        auto translated_nested_history = translate_incoming_nested_history(nested_history, incoming_anchor_step, local_anchor_step, document_state_id_translation_state, nested_history_step_floor);
        if (!translated_nested_history.has_value())
            return {};
        nested_histories.unchecked_append(translated_nested_history.release_value());
    }

    document_state.nested_histories = move(nested_histories);
    return document_state;
}

static Optional<TraversableSessionHistory::Entry> translate_incoming_entry(TraversableSessionHistory::Entry const& entry, i32 incoming_anchor_step, i32 local_anchor_step, DocumentStateIdTranslationState& document_state_id_translation_state, StepTranslationMode mode = StepTranslationMode::TopLevel, Optional<i32> nested_history_step_floor = {})
{
    auto translated_step = translate_incoming_step(entry.step, incoming_anchor_step, local_anchor_step, mode, nested_history_step_floor);
    if (!translated_step.has_value())
        return {};
    auto translated_document_state = translate_incoming_document_state_descriptor(entry.document_state, incoming_anchor_step, local_anchor_step, document_state_id_translation_state, nested_history_step_floor);
    if (!translated_document_state.has_value())
        return {};

    return TraversableSessionHistory::Entry {
        .step = *translated_step,
        .url = entry.url,
        .document_state = translated_document_state.release_value(),
        .classic_history_api_state = entry.classic_history_api_state,
        .navigation_api_state = entry.navigation_api_state,
        .navigation_api_key = entry.navigation_api_key,
        .navigation_api_id = entry.navigation_api_id,
        .scroll_restoration_mode = entry.scroll_restoration_mode,
        .scroll_position_data = entry.scroll_position_data,
    };
}

static Optional<Web::HTML::SessionHistoryNestedHistoryDescriptor> translate_incoming_nested_history(Web::HTML::SessionHistoryNestedHistoryDescriptor const& nested_history, i32 incoming_anchor_step, i32 local_anchor_step, DocumentStateIdTranslationState& document_state_id_translation_state, Optional<i32> nested_history_step_floor)
{
    Vector<TraversableSessionHistory::Entry> entries;
    entries.ensure_capacity(nested_history.entries.size());
    Optional<i32> previous_step;
    for (auto const& entry : nested_history.entries) {
        auto translated_entry = translate_incoming_entry(entry, incoming_anchor_step, local_anchor_step, document_state_id_translation_state, StepTranslationMode::Nested, nested_history_step_floor);
        if (!translated_entry.has_value())
            return {};
        if (previous_step.has_value() && translated_entry->step <= *previous_step)
            return {};
        previous_step = translated_entry->step;
        entries.unchecked_append(translated_entry.release_value());
    }

    return Web::HTML::SessionHistoryNestedHistoryDescriptor {
        .id = nested_history.id,
        .entries = move(entries),
    };
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

static TraversableSessionHistory::Entry create_ui_process_session_history_entry(
    i32 step,
    URL::URL url,
    Variant<Empty, String, Web::HTML::POSTResource> document_resource)
{
    return {
        .step = step,
        .url = move(url),
        .document_state = {
            .id = 0,
            .history_policy_container = Web::HTML::DocumentState::Client::Tag,
            .request_referrer = Web::Fetch::Infrastructure::Request::Referrer::Client,
            .request_referrer_policy = Web::ReferrerPolicy::DEFAULT_REFERRER_POLICY,
            .initiator_origin = {},
            .origin = {},
            .about_base_url = {},
            .resource = move(document_resource),
            .reload_pending = false,
            .ever_populated = false,
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

void TraversableSessionHistory::navigate(URL::URL url)
{
    navigate(move(url), Empty {});
}

void TraversableSessionHistory::navigate(URL::URL url, Variant<Empty, String, Web::HTML::POSTResource> document_resource)
{
    forget_web_content_state();

    if (!m_current_used_step_index.has_value()) {
        m_entries.clear();
        m_used_steps.clear();
        m_entries.append(create_ui_process_session_history_entry(0, move(url), move(document_resource)));
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
    m_entries.append(create_ui_process_session_history_entry(step, move(url), move(document_resource)));
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

void TraversableSessionHistory::replace_current_entry_url(URL::URL url)
{
    forget_web_content_state();

    if (!m_current_used_step_index.has_value()) {
        navigate(move(url));
        return;
    }

    auto current_top_level_entry_index = this->current_top_level_entry_index();
    VERIFY(current_top_level_entry_index.has_value());
    m_entries[*current_top_level_entry_index].url = move(url);
}

void TraversableSessionHistory::replace_current_entry(URL::URL url, Variant<Empty, String, Web::HTML::POSTResource> document_resource)
{
    forget_web_content_state();

    if (!m_current_used_step_index.has_value()) {
        navigate(move(url), move(document_resource));
        return;
    }

    auto current_top_level_entry_index = this->current_top_level_entry_index();
    VERIFY(current_top_level_entry_index.has_value());

    auto current_step = m_used_steps[*m_current_used_step_index];
    m_entries[*current_top_level_entry_index] = create_ui_process_session_history_entry(
        current_step, move(url), move(document_resource));
    recompute_used_steps(m_entries, m_used_steps, m_current_used_step_index, current_step);
    VERIFY(m_current_used_step_index.has_value());
}

void TraversableSessionHistory::mark_current_entry_reload_pending()
{
    forget_web_content_state();

    auto current_top_level_entry_index = this->current_top_level_entry_index();
    if (!current_top_level_entry_index.has_value())
        return;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#reload
    // Set navigable's active session history entry's document state's reload
    // pending to true.
    m_entries[*current_top_level_entry_index].document_state.reload_pending = true;
}

void TraversableSessionHistory::clear_current_entry_reload_pending()
{
    auto current_top_level_entry_index = this->current_top_level_entry_index();
    if (!current_top_level_entry_index.has_value())
        return;

    m_entries[*current_top_level_entry_index].document_state.reload_pending = false;
}

Optional<size_t> TraversableSessionHistory::current_top_level_entry_index() const
{
    if (!m_current_used_step_index.has_value())
        return {};
    return top_level_entry_index_for_step(m_entries, m_used_steps[*m_current_used_step_index]);
}

TraversableSessionHistory::UpdateResult TraversableSessionHistory::update_from_web_content(Vector<Entry> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    auto invalid_snapshot = [&] {
        forget_web_content_state();
        return UpdateResult::InvalidSnapshot;
    };

    if (entries.is_empty() || used_steps.is_empty() || current_used_step_index >= used_steps.size() || !entries_are_valid(entries) || !steps_are_valid(used_steps) || !entries_and_used_steps_are_consistent(entries, used_steps))
        return invalid_snapshot();

    auto incoming_current_top_level_entry_index = top_level_entry_index_for_step(entries, used_steps[current_used_step_index]);
    if (!incoming_current_top_level_entry_index.has_value())
        return invalid_snapshot();

    if (m_entries.is_empty()) {
        canonicalize_document_state_ids(entries);
        m_entries.clear_with_capacity();
        m_entries.ensure_capacity(entries.size());
        for (auto& entry : entries)
            m_entries.unchecked_append(move(entry));
        m_used_steps = move(used_steps);
        m_current_used_step_index = current_used_step_index;
        m_web_content_known_entries = m_entries;
        m_web_content_known_used_steps = m_used_steps;
        m_web_content_current_step = m_used_steps[*m_current_used_step_index];
        m_web_content_uses_ui_step_coordinates = true;
        return UpdateResult::CompleteSnapshot;
    }

    VERIFY(m_current_used_step_index.has_value());
    VERIFY(!m_used_steps.is_empty());

    if (m_entries.size() == entries.size() && m_used_steps.size() == used_steps.size()) {
        if (entries_match(m_entries, entries) && steps_match(m_used_steps, used_steps)) {
            m_current_used_step_index = current_used_step_index;
            m_web_content_known_entries = m_entries;
            m_web_content_known_used_steps = m_used_steps;
            m_web_content_current_step = m_used_steps[*m_current_used_step_index];
            m_web_content_uses_ui_step_coordinates = true;
            return UpdateResult::CompleteSnapshot;
        }
    }

    auto local_current_top_level_entry_index = current_top_level_entry_index();
    if (!local_current_top_level_entry_index.has_value())
        return invalid_snapshot();

    if (m_entries.size() == entries.size() + 1
        && m_used_steps.size() == used_steps.size() + 1
        && *local_current_top_level_entry_index == m_entries.size() - 1
        && *m_current_used_step_index == m_used_steps.size() - 1
        && *local_current_top_level_entry_index > 0
        && *incoming_current_top_level_entry_index > 0
        && m_entries[*local_current_top_level_entry_index - 1].step == used_steps[current_used_step_index]
        && m_entries[*local_current_top_level_entry_index].url == entries[*incoming_current_top_level_entry_index].url) {
        canonicalize_document_state_ids(entries);
        m_entries = move(entries);
        m_used_steps = move(used_steps);
        m_current_used_step_index = current_used_step_index;
        m_web_content_known_entries = m_entries;
        m_web_content_known_used_steps = m_used_steps;
        m_web_content_current_step = m_used_steps[*m_current_used_step_index];
        m_web_content_uses_ui_step_coordinates = true;
        return UpdateResult::CompleteSnapshot;
    }

    auto find_merge_anchor = [&](auto entry_can_anchor) -> Optional<SessionHistoryMergeAnchor> {
        for (size_t local_index = *local_current_top_level_entry_index + 1; local_index > 0; --local_index) {
            auto candidate_local_index = local_index - 1;

            for (size_t incoming_index = *incoming_current_top_level_entry_index + 1; incoming_index > 0; --incoming_index) {
                auto candidate_incoming_index = incoming_index - 1;
                if (!entry_can_anchor(m_entries[candidate_local_index], entries[candidate_incoming_index]))
                    continue;

                return SessionHistoryMergeAnchor {
                    .local_index = candidate_local_index,
                    .incoming_index = candidate_incoming_index,
                };
            }
        }
        return {};
    };

    auto merge_anchor = Optional<SessionHistoryMergeAnchor> {};
    if (m_web_content_uses_ui_step_coordinates && m_web_content_current_step.has_value()) {
        merge_anchor = find_merge_anchor([&](Entry const& local_entry, Entry const& incoming_entry) {
            if (local_entry.step != incoming_entry.step)
                return false;
            if (local_entry.url == incoming_entry.url)
                return false;
            if (local_entry.step != *m_web_content_current_step)
                return false;
            if (!m_web_content_known_used_steps.contains_slow(local_entry.step))
                return false;
            auto const* web_content_known_entry = WebView::top_level_entry_for_step(m_web_content_known_entries, local_entry.step);
            return web_content_known_entry && web_content_known_entry->step == local_entry.step;
        });
    }

    // AD-HOC: The URL-keyed anchor searches are the common case. But find_merge_anchor compares every local entry
    //         against every incoming entry — and each URL comparison serializes both URLs. A burst of same-document
    //         navigations (such as a pushState flood) can push the top-level entry count into the hundreds while this
    //         runs once per history update — so, a quadratic scan with per-comparison serialization becomes a
    //         UI-process bottleneck. Index the incoming entries by serialized URL once — keyed the same way URL
    //         equality compares (full serialization, fragment included) — so each URL-keyed search is linear.
    if (!merge_anchor.has_value()) {
        HashMap<String, Vector<size_t>> incoming_indices_by_url;
        for (size_t incoming_index = *incoming_current_top_level_entry_index + 1; incoming_index > 0; --incoming_index) {
            auto candidate_incoming_index = incoming_index - 1;
            incoming_indices_by_url.ensure(entries[candidate_incoming_index].url.serialize()).append(candidate_incoming_index);
        }

        // The incoming index lists are built highest-first. So, this reproduces find_merge_anchor's choice among the
        // local/incoming entry pairs whose URLs are equal and who satisfy the predicate: The highest local entry — and
        // for it, the highest matching incoming entry.
        auto find_url_merge_anchor = [&](auto predicate) -> Optional<SessionHistoryMergeAnchor> {
            for (size_t local_index = *local_current_top_level_entry_index + 1; local_index > 0; --local_index) {
                auto candidate_local_index = local_index - 1;
                auto incoming_candidates = incoming_indices_by_url.find(m_entries[candidate_local_index].url.serialize());
                if (incoming_candidates == incoming_indices_by_url.end())
                    continue;
                for (auto candidate_incoming_index : incoming_candidates->value) {
                    if (!predicate(m_entries[candidate_local_index], entries[candidate_incoming_index]))
                        continue;
                    return SessionHistoryMergeAnchor {
                        .local_index = candidate_local_index,
                        .incoming_index = candidate_incoming_index,
                    };
                }
            }
            return {};
        };

        merge_anchor = find_url_merge_anchor([](Entry const& local_entry, Entry const& incoming_entry) {
            return entries_have_matching_nonzero_document_state_id(local_entry, incoming_entry);
        });
        if (!merge_anchor.has_value()) {
            merge_anchor = find_url_merge_anchor([](Entry const&, Entry const&) {
                return true;
            });
        }
    }

    if (!merge_anchor.has_value())
        return invalid_snapshot();

    // AD-HOC: The HTML algorithms operate on one traversable session history. Ladybird's UI process can instead
    //         receive a valid partial WebContent snapshot after a process swap or fallback load, so merge the suffix
    //         WebContent knows about into the durable UI mirror while preserving any still-valid forward history.
    auto local_index = merge_anchor->local_index;
    auto incoming_index = merge_anchor->incoming_index;
    size_t common_suffix_size = 0;
    while (local_index + common_suffix_size < m_entries.size() && incoming_index + common_suffix_size < entries.size()) {
        if (m_entries[local_index + common_suffix_size].url != entries[incoming_index + common_suffix_size].url)
            break;
        ++common_suffix_size;
    }
    if (common_suffix_size == 0 && m_web_content_uses_ui_step_coordinates && m_entries[merge_anchor->local_index].step == entries[merge_anchor->incoming_index].step)
        common_suffix_size = 1;

    auto local_anchor_step = m_entries[merge_anchor->local_index].step;
    auto incoming_anchor_step = entries[merge_anchor->incoming_index].step;
    Optional<i32> nested_history_step_floor;
    if (auto incoming_anchor_document_state_id = entries[merge_anchor->incoming_index].document_state.id; incoming_anchor_document_state_id != 0) {
        for (size_t i = 0; i < merge_anchor->incoming_index; ++i) {
            if (entries[i].document_state.id != incoming_anchor_document_state_id)
                continue;
            nested_history_step_floor = entries[i].step;
            break;
        }
    }
    DocumentStateIdTranslationState document_state_id_translation_state;
    update_next_document_state_id(m_entries, document_state_id_translation_state.next_document_state_id);
    if (entries[merge_anchor->incoming_index].document_state.id != 0 && m_entries[merge_anchor->local_index].document_state.id != 0)
        document_state_id_translation_state.translated_document_state_ids.set(entries[merge_anchor->incoming_index].document_state.id, m_entries[merge_anchor->local_index].document_state.id);

    auto incoming_has_used_steps_after_anchor = false;
    for (auto const& used_step : used_steps) {
        if (used_step > incoming_anchor_step) {
            incoming_has_used_steps_after_anchor = true;
            break;
        }
    }

    Vector<Entry> merged_entries;
    auto web_content_uses_ui_step_coordinates = true;
    auto incoming_suffix_size = entries.size() - incoming_index;
    auto preserves_existing_forward_history = common_suffix_size == incoming_suffix_size && !incoming_has_used_steps_after_anchor;
    auto merged_size = preserves_existing_forward_history ? m_entries.size() : merge_anchor->local_index + incoming_suffix_size;
    merged_entries.ensure_capacity(merged_size);

    for (size_t i = 0; i < merge_anchor->local_index; ++i)
        merged_entries.unchecked_append(m_entries[i]);

    for (size_t i = merge_anchor->incoming_index; i < entries.size(); ++i) {
        auto translated_entry = translate_incoming_entry(entries[i], incoming_anchor_step, local_anchor_step, document_state_id_translation_state, StepTranslationMode::TopLevel, nested_history_step_floor);
        if (!translated_entry.has_value())
            return invalid_snapshot();
        if (!entry_steps_match(entries[i], *translated_entry))
            web_content_uses_ui_step_coordinates = false;
        merged_entries.unchecked_append(translated_entry.release_value());
    }

    if (preserves_existing_forward_history) {
        for (size_t i = merge_anchor->local_index + incoming_suffix_size; i < m_entries.size(); ++i)
            merged_entries.unchecked_append(m_entries[i]);
    }

    Vector<i32> merged_used_steps;
    if (preserves_existing_forward_history) {
        merged_used_steps.ensure_capacity(m_used_steps.size());
        for (auto const& used_step : m_used_steps)
            merged_used_steps.unchecked_append(used_step);
    } else {
        merged_used_steps.ensure_capacity(m_used_steps.size() + used_steps.size());
        for (auto const& used_step : m_used_steps) {
            if (used_step > local_anchor_step)
                break;
            merged_used_steps.append(used_step);
        }

        for (auto const& incoming_used_step : used_steps) {
            if (incoming_used_step <= incoming_anchor_step)
                continue;

            auto translated_step = translate_incoming_step(incoming_used_step, incoming_anchor_step, local_anchor_step);
            if (!translated_step.has_value())
                return invalid_snapshot();
            if (*translated_step != incoming_used_step)
                web_content_uses_ui_step_coordinates = false;
            if (!merged_used_steps.is_empty() && *translated_step <= merged_used_steps.last())
                return invalid_snapshot();
            merged_used_steps.append(*translated_step);
        }
    }

    Vector<i32> translated_web_content_used_steps;
    translated_web_content_used_steps.ensure_capacity(used_steps.size());
    for (auto const& incoming_used_step : used_steps) {
        auto translation_mode = StepTranslationMode::TopLevel;
        if (incoming_used_step < incoming_anchor_step) {
            if (!nested_history_step_floor.has_value() || incoming_used_step < *nested_history_step_floor)
                continue;
            translation_mode = StepTranslationMode::Nested;
        }
        auto translated_step = translate_incoming_step(incoming_used_step, incoming_anchor_step, local_anchor_step, translation_mode, nested_history_step_floor);
        if (!translated_step.has_value())
            return invalid_snapshot();
        if (*translated_step != incoming_used_step)
            web_content_uses_ui_step_coordinates = false;
        if (!translated_web_content_used_steps.is_empty() && *translated_step <= translated_web_content_used_steps.last())
            return invalid_snapshot();
        translated_web_content_used_steps.append(*translated_step);
    }

    auto current_step_translation_mode = StepTranslationMode::TopLevel;
    if (used_steps[current_used_step_index] < incoming_anchor_step && nested_history_step_floor.has_value() && used_steps[current_used_step_index] >= *nested_history_step_floor)
        current_step_translation_mode = StepTranslationMode::Nested;
    auto translated_current_step = translate_incoming_step(used_steps[current_used_step_index], incoming_anchor_step, local_anchor_step, current_step_translation_mode, nested_history_step_floor);
    if (!translated_current_step.has_value())
        return invalid_snapshot();
    if (*translated_current_step != used_steps[current_used_step_index])
        web_content_uses_ui_step_coordinates = false;
    if (!translated_web_content_used_steps.contains_slow(*translated_current_step))
        return invalid_snapshot();
    auto current_used_step_index_after_merge = merged_used_steps.find_first_index(*translated_current_step);
    if (!current_used_step_index_after_merge.has_value())
        return invalid_snapshot();
    if (!entries_and_used_steps_are_consistent(merged_entries, merged_used_steps))
        return invalid_snapshot();
    canonicalize_document_state_ids(merged_entries);

    Vector<Entry> translated_web_content_entries;
    auto web_content_known_entries_start_index = merge_anchor->local_index;
    if (nested_history_step_floor.has_value()) {
        auto same_document_entry_index = top_level_entry_index_for_step(merged_entries, *nested_history_step_floor);
        if (same_document_entry_index.has_value())
            web_content_known_entries_start_index = *same_document_entry_index;
    }
    translated_web_content_entries.ensure_capacity(merge_anchor->local_index + incoming_suffix_size - web_content_known_entries_start_index);
    for (size_t i = web_content_known_entries_start_index; i < merge_anchor->local_index + incoming_suffix_size; ++i)
        translated_web_content_entries.unchecked_append(merged_entries[i]);

    auto entries_in_ui_document_state_id_order = entries;
    canonicalize_document_state_ids(entries_in_ui_document_state_id_order);
    auto web_content_matches_mirror = entries_match(merged_entries, entries_in_ui_document_state_id_order) && steps_match(merged_used_steps, used_steps);

    m_entries = move(merged_entries);
    m_used_steps = move(merged_used_steps);
    m_current_used_step_index = *current_used_step_index_after_merge;
    VERIFY(*m_current_used_step_index < m_used_steps.size());
    VERIFY(current_top_level_entry_index().has_value());

    if (web_content_matches_mirror) {
        m_web_content_known_entries = m_entries;
        m_web_content_known_used_steps = m_used_steps;
        m_web_content_uses_ui_step_coordinates = true;
    } else {
        m_web_content_known_entries = move(translated_web_content_entries);
        m_web_content_known_used_steps = move(translated_web_content_used_steps);
        m_web_content_uses_ui_step_coordinates = web_content_uses_ui_step_coordinates;
    }
    m_web_content_current_step = *translated_current_step;

    return web_content_matches_mirror ? UpdateResult::CompleteSnapshot : UpdateResult::MergedPartialSnapshot;
}

void TraversableSessionHistory::did_seed_web_content_from_ui_process(size_t current_top_level_entry_index)
{
    VERIFY(current_top_level_entry_index < m_entries.size());
    m_web_content_known_entries = m_entries;
    m_web_content_known_used_steps = m_used_steps;
    m_web_content_current_step = m_entries[current_top_level_entry_index].step;
    m_web_content_uses_ui_step_coordinates = true;
}

bool TraversableSessionHistory::did_seed_web_content_from_ui_process(Vector<Entry> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    if (m_entries.is_empty() || entries.is_empty() || used_steps.is_empty() || current_used_step_index >= used_steps.size() || !entries_are_valid(entries) || !steps_are_valid(used_steps) || !entries_and_used_steps_are_consistent(entries, used_steps))
        return false;

    auto current_top_level_entry_index = this->current_top_level_entry_index();
    if (!current_top_level_entry_index.has_value())
        return false;

    if (used_steps[current_used_step_index] != m_entries[*current_top_level_entry_index].step)
        return false;

    auto entries_from_web_content = entries;
    auto expected_entries = m_entries;
    canonicalize_document_state_ids(expected_entries);
    canonicalize_document_state_ids(entries);

    if (!steps_match(m_used_steps, used_steps))
        return false;

    if (!entries_match(expected_entries, entries)) {
        Optional<size_t> current_unknown_entry_index;
        if (m_entries[*current_top_level_entry_index].document_state.id == 0)
            current_unknown_entry_index = *current_top_level_entry_index;
        if (!seed_ack_entries_match(m_entries, entries_from_web_content, current_unknown_entry_index))
            return false;
        m_entries = move(entries_from_web_content);
    }

    m_web_content_known_entries = m_entries;
    m_web_content_known_used_steps = m_used_steps;
    m_web_content_current_step = used_steps[current_used_step_index];
    m_web_content_uses_ui_step_coordinates = true;
    return true;
}

bool TraversableSessionHistory::did_restore_web_content_to_current_step(i32 step)
{
    if (!m_current_used_step_index.has_value())
        return false;
    if (m_used_steps[*m_current_used_step_index] != step)
        return false;
    if (!m_web_content_uses_ui_step_coordinates)
        return false;
    if (!m_web_content_known_used_steps.contains_slow(step))
        return false;

    auto const* web_content_current_top_level_entry = WebView::top_level_entry_for_step(m_web_content_known_entries, step);
    auto const* ui_current_top_level_entry = current_entry();
    if (!web_content_current_top_level_entry || !ui_current_top_level_entry)
        return false;
    if (!Web::HTML::session_history_entry_descriptors_match(*web_content_current_top_level_entry, *ui_current_top_level_entry))
        return false;

    m_web_content_current_step = step;
    return true;
}

bool TraversableSessionHistory::did_apply_web_content_traversal_to_step(i32 step)
{
    auto target = traversal_target_for_step(step);
    if (!target.has_value())
        return false;

    if (!web_content_can_traverse_to(*target))
        return false;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-history-step
    // Set traversable's current session history step to targetStep.
    m_current_used_step_index = target->target_step_index;
    m_web_content_current_step = step;
    return true;
}

bool TraversableSessionHistory::web_content_history_matches_mirror() const
{
    if (!m_web_content_current_step.has_value() || !m_current_used_step_index.has_value())
        return false;
    if (!m_web_content_uses_ui_step_coordinates)
        return false;
    if (*m_web_content_current_step != m_used_steps[*m_current_used_step_index])
        return false;
    return entries_match(m_entries, m_web_content_known_entries) && steps_match(m_used_steps, m_web_content_known_used_steps);
}

void TraversableSessionHistory::forget_web_content_state()
{
    m_web_content_known_entries.clear();
    m_web_content_known_used_steps.clear();
    m_web_content_current_step.clear();
    m_web_content_uses_ui_step_coordinates = false;
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

Vector<TraversableSessionHistory::Entry> TraversableSessionHistory::web_content_known_entries() const
{
    return m_web_content_known_entries;
}

Vector<i32> TraversableSessionHistory::web_content_known_used_steps() const
{
    return m_web_content_known_used_steps;
}

Optional<i32> TraversableSessionHistory::web_content_current_step() const
{
    return m_web_content_current_step;
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
    if (!m_current_used_step_index.has_value() || !m_web_content_current_step.has_value())
        return false;
    if (!m_web_content_uses_ui_step_coordinates)
        return false;
    if (*m_web_content_current_step != m_used_steps[*m_current_used_step_index])
        return false;
    if (!m_web_content_known_used_steps.contains_slow(target.target_step))
        return false;

    auto const* web_content_target_top_level_entry = WebView::top_level_entry_for_step(m_web_content_known_entries, target.target_step);
    if (!web_content_target_top_level_entry || !target.target_top_level_entry)
        return false;
    return Web::HTML::session_history_entry_descriptors_match(*web_content_target_top_level_entry, *target.target_top_level_entry);
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
