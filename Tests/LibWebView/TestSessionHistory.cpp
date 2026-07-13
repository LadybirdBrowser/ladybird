/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/SessionHistory.h>

using Web::HTML::SessionHistoryEntryUpdateKind;

static Web::HTML::CrossProcessId navigable_id(StringView id)
{
    if (id == "frame-1"sv)
        return { 1, 1 };
    if (id == "frame"sv)
        return { 1, 2 };
    if (id == "1"sv)
        return { 2, 1 };
    VERIFY_NOT_REACHED();
}

static Web::HTML::CrossProcessId test_document_state_id(u64 local_id)
{
    VERIFY(local_id > 0);
    return { 3, local_id };
}

static Web::HTML::CrossProcessId allocate_test_ui_process_document_state_id()
{
    static Web::HTML::CrossProcessIdAllocator allocator { .namespace_id = 4 };
    return allocator.allocate();
}

static Web::HTML::SessionHistoryNestedHistoryDescriptor nested_history(StringView id, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries)
{
    return {
        .id = navigable_id(id),
        .entries = move(entries),
    };
}

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

static Web::HTML::StorageSerializationRecord state_record(u8 byte)
{
    return Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::copy({ &byte, 1 })) };
}

static Web::HTML::SessionHistoryEntryDescriptor create_test_entry(i32 step, URL::URL url)
{
    return {
        .step = step,
        .url = move(url),
        .document_state = {
            .id = test_document_state_id(1000 + static_cast<u64>(step)),
            .history_policy_container = Web::HTML::DocumentState::Client::Tag,
            .request_referrer = Web::Fetch::Infrastructure::Request::Referrer::Client,
            .request_referrer_policy = Web::ReferrerPolicy::DEFAULT_REFERRER_POLICY,
            .initiator_origin = {},
            .origin = {},
            .about_base_url = {},
            .resource = {},
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

static Web::HTML::SessionHistoryEntryDescriptor entry(i32 step, StringView url)
{
    return create_test_entry(step, parse_url(url));
}

static Web::HTML::SessionHistoryEntryDescriptor entry(i32 step, StringView url, u8 classic_history_api_state, u8 navigation_api_state, StringView navigation_api_key, StringView navigation_api_id, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.classic_history_api_state = state_record(classic_history_api_state);
    entry.navigation_api_state = state_record(navigation_api_state);
    entry.navigation_api_key = Utf16String::from_utf8(navigation_api_key);
    entry.navigation_api_id = Utf16String::from_utf8(navigation_api_id);
    entry.scroll_restoration_mode = scroll_restoration_mode;
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry(i32 step, StringView url, u64 document_state_id, StringView navigable_target_name)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.document_state.id = test_document_state_id(document_state_id);
    entry.document_state.ever_populated = true;
    entry.document_state.navigable_target_name = Utf16String::from_utf8(navigable_target_name);
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry_with_reload_pending(i32 step, StringView url, u64 document_state_id, StringView navigable_target_name, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> nested_histories)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.document_state.id = test_document_state_id(document_state_id);
    entry.document_state.reload_pending = true;
    entry.document_state.ever_populated = true;
    entry.document_state.navigable_target_name = Utf16String::from_utf8(navigable_target_name);
    entry.document_state.nested_histories = move(nested_histories);
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry_with_unpopulated_document_state(i32 step, StringView url, u64 document_state_id, StringView navigable_target_name)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.document_state.id = test_document_state_id(document_state_id);
    entry.document_state.navigable_target_name = Utf16String::from_utf8(navigable_target_name);
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry_with_post_resource(i32 step, StringView url)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.document_state.resource = Web::HTML::POSTResource {
        .request_body = MUST(ByteBuffer::copy("field=value"sv.bytes())),
        .request_content_type = Web::HTML::POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded,
    };
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry(i32 step, StringView url, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> nested_histories)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.document_state.nested_histories = move(nested_histories);
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry(i32 step, StringView url, u64 document_state_id, StringView navigable_target_name, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> nested_histories)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.document_state.id = test_document_state_id(document_state_id);
    entry.document_state.ever_populated = true;
    entry.document_state.navigable_target_name = Utf16String::from_utf8(navigable_target_name);
    entry.document_state.nested_histories = move(nested_histories);
    return entry;
}

static void expect_entry(WebView::TraversableSessionHistory const& history, size_t index, i32 expected_step, StringView expected_url)
{
    auto* entry = history.entry_at(index);
    VERIFY(entry);
    EXPECT_EQ(entry->step, expected_step);
    EXPECT_EQ(entry->url, parse_url(expected_url));
}

static void expect_current_entry(WebView::TraversableSessionHistory const& history, i32 expected_step, StringView expected_url)
{
    auto* entry = history.current_entry();
    VERIFY(entry);
    EXPECT_EQ(entry->step, expected_step);
    EXPECT_EQ(entry->url, parse_url(expected_url));
}

static void record_web_content_state_installed_at_current_top_level_entry(WebView::TraversableSessionHistory& history)
{
    auto* entry = history.current_entry();
    VERIFY(entry);
    history.record_web_content_session_history_state_installed(entry->step);
}

static bool apply_current_entry_update(WebView::TraversableSessionHistory& history, SessionHistoryEntryUpdateKind update_kind, Web::HTML::SessionHistoryEntryDescriptor entry)
{
    return history.apply_web_content_mutation(
                      WebView::TraversableSessionHistory::WebContentMutation::current_entry_update(update_kind, move(entry)))
        .accepted;
}

static Web::HTML::SameDocumentSessionHistoryNavigation same_document_navigation_details(Web::HTML::SessionHistoryEntryDescriptor entry, Optional<i32> replaced_step, i32 current_step)
{
    return {
        .url = move(entry.url),
        .document_state = move(entry.document_state),
        .classic_history_api_state = move(entry.classic_history_api_state),
        .navigation_api_state = move(entry.navigation_api_state),
        .navigation_api_key = move(entry.navigation_api_key),
        .navigation_api_id = move(entry.navigation_api_id),
        .scroll_restoration_mode = entry.scroll_restoration_mode,
        .scroll_position_data = move(entry.scroll_position_data),
        .replaced_step = replaced_step,
        .current_step = current_step,
    };
}

static Web::HTML::TopLevelCrossDocumentSessionHistoryNavigation top_level_cross_document_navigation_details(Web::HTML::SessionHistoryEntryDescriptor entry, i32 current_step)
{
    return {
        .url = move(entry.url),
        .document_state = move(entry.document_state),
        .classic_history_api_state = move(entry.classic_history_api_state),
        .navigation_api_state = move(entry.navigation_api_state),
        .navigation_api_key = move(entry.navigation_api_key),
        .navigation_api_id = move(entry.navigation_api_id),
        .scroll_restoration_mode = entry.scroll_restoration_mode,
        .scroll_position_data = move(entry.scroll_position_data),
        .current_step = current_step,
    };
}

static Web::HTML::NestedSameDocumentSessionHistoryNavigation nested_same_document_navigation_details(Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor entry, Optional<i32> replaced_step, i32 current_step)
{
    return {
        .parent_document_state_id = parent_document_state_id,
        .navigable_id = navigable_id,
        .url = move(entry.url),
        .document_state = move(entry.document_state),
        .classic_history_api_state = move(entry.classic_history_api_state),
        .navigation_api_state = move(entry.navigation_api_state),
        .navigation_api_key = move(entry.navigation_api_key),
        .navigation_api_id = move(entry.navigation_api_id),
        .scroll_restoration_mode = entry.scroll_restoration_mode,
        .scroll_position_data = move(entry.scroll_position_data),
        .replaced_step = replaced_step,
        .current_step = current_step,
    };
}

static Web::HTML::NestedCrossDocumentSessionHistoryNavigation nested_cross_document_navigation_details(Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor entry, i32 current_step)
{
    return {
        .parent_document_state_id = parent_document_state_id,
        .navigable_id = navigable_id,
        .url = move(entry.url),
        .document_state = move(entry.document_state),
        .classic_history_api_state = move(entry.classic_history_api_state),
        .navigation_api_state = move(entry.navigation_api_state),
        .navigation_api_key = move(entry.navigation_api_key),
        .navigation_api_id = move(entry.navigation_api_id),
        .scroll_restoration_mode = entry.scroll_restoration_mode,
        .scroll_position_data = move(entry.scroll_position_data),
        .current_step = current_step,
    };
}

static bool apply_top_level_same_document_navigation(WebView::TraversableSessionHistory& history, Web::HTML::SessionHistoryEntryDescriptor entry, Optional<i32> replaced_step, i32 current_step)
{
    return history.apply_top_level_same_document_navigation(same_document_navigation_details(move(entry), replaced_step, current_step));
}

static bool apply_nested_same_document_navigation(WebView::TraversableSessionHistory& history, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor entry, Optional<i32> replaced_step, i32 current_step)
{
    return history.apply_nested_same_document_navigation(nested_same_document_navigation_details(parent_document_state_id, navigable_id, move(entry), replaced_step, current_step));
}

static bool apply_nested_cross_document_navigation(WebView::TraversableSessionHistory& history, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor entry, i32 current_step)
{
    return history.apply_nested_cross_document_navigation_commit(nested_cross_document_navigation_details(parent_document_state_id, navigable_id, move(entry), current_step));
}

static bool apply_top_level_cross_document_navigation(WebView::TraversableSessionHistory& history, Web::HTML::SessionHistoryEntryDescriptor entry, i32 current_step)
{
    return history.apply_top_level_cross_document_navigation_commit(top_level_cross_document_navigation_details(move(entry), current_step));
}

static bool apply_child_navigable_creation(WebView::TraversableSessionHistory& history, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor initial_entry, i32 current_step)
{
    return history.apply_child_navigable_creation({
        .parent_document_state_id = parent_document_state_id,
        .navigable_id = navigable_id,
        .initial_entry = move(initial_entry),
        .current_step = current_step,
    });
}

static bool apply_child_navigable_destruction(WebView::TraversableSessionHistory& history, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id, i32 current_step)
{
    return history.apply_child_navigable_destruction({
        .parent_document_state_id = parent_document_state_id,
        .navigable_id = navigable_id,
        .current_step = current_step,
    });
}

static bool apply_traversal_current_step(WebView::TraversableSessionHistory& history, i32 step)
{
    return history.apply_traversal_to_step(step);
}

static bool document_state_has_nested_history(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId navigable_id)
{
    for (auto const& entry : entries) {
        if (entry.document_state.id == parent_document_state_id) {
            for (auto const& nested_history : entry.document_state.nested_histories) {
                if (nested_history.id == navigable_id)
                    return true;
            }
        }

        for (auto const& nested_history : entry.document_state.nested_histories) {
            if (document_state_has_nested_history(nested_history.entries, parent_document_state_id, navigable_id))
                return true;
        }
    }
    return false;
}

static bool is_default_test_document_state_id(Web::HTML::CrossProcessId id)
{
    return id.namespace_id == 3 && id.local_id >= 1000;
}

static void assign_unique_document_state_ids_to_default_nested_entries(Vector<Web::HTML::SessionHistoryEntryDescriptor>& entries)
{
    for (auto& entry : entries) {
        if (is_default_test_document_state_id(entry.document_state.id))
            entry.document_state.id = allocate_test_ui_process_document_state_id();

        for (auto& nested_history : entry.document_state.nested_histories)
            assign_unique_document_state_ids_to_default_nested_entries(nested_history.entries);
    }
}

static void apply_nested_histories_for_test(WebView::TraversableSessionHistory& history, Web::HTML::CrossProcessId parent_document_state_id, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> nested_histories)
{
    for (auto& nested_history : nested_histories) {
        VERIFY(!nested_history.entries.is_empty());
        if (document_state_has_nested_history(history.entries(), parent_document_state_id, nested_history.id))
            continue;

        assign_unique_document_state_ids_to_default_nested_entries(nested_history.entries);

        auto initial_entry = nested_history.entries.take_first();
        auto initial_entry_document_state_id = initial_entry.document_state.id;
        auto initial_entry_nested_histories = move(initial_entry.document_state.nested_histories);
        initial_entry.document_state.nested_histories.clear();

        auto initial_entry_step = initial_entry.step;
        VERIFY(apply_child_navigable_creation(history, parent_document_state_id, nested_history.id, move(initial_entry), initial_entry_step));
        apply_nested_histories_for_test(history, initial_entry_document_state_id, move(initial_entry_nested_histories));

        for (auto& nested_entry : nested_history.entries) {
            auto nested_entry_document_state_id = nested_entry.document_state.id;
            auto child_nested_histories = move(nested_entry.document_state.nested_histories);
            nested_entry.document_state.nested_histories.clear();

            VERIFY(apply_nested_cross_document_navigation(history, parent_document_state_id, nested_history.id, move(nested_entry), nested_entry.step));
            apply_nested_histories_for_test(history, nested_entry_document_state_id, move(child_nested_histories));
        }
    }
}

static void initialize_history_from_ui_entries(WebView::TraversableSessionHistory& history, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    VERIFY(!entries.is_empty());
    VERIFY(!used_steps.is_empty());
    VERIFY(current_used_step_index < used_steps.size());

    history.clear();
    for (auto entry : entries) {
        auto nested_histories = move(entry.document_state.nested_histories);
        entry.document_state.nested_histories.clear();

        auto const* current_entry = history.current_entry();
        auto is_same_document_navigation = current_entry && current_entry->document_state.id == entry.document_state.id;
        if (is_same_document_navigation) {
            VERIFY(apply_top_level_same_document_navigation(history, entry, {}, entry.step));
        } else {
            history.navigate(entry.url, entry.document_state.id, entry.document_state.resource);
            VERIFY(apply_top_level_cross_document_navigation(history, entry, entry.step));
        }

        apply_nested_histories_for_test(history, entry.document_state.id, move(nested_histories));
    }

    VERIFY(history.used_step_count() == used_steps.size());
    for (size_t i = 0; i < used_steps.size(); ++i) {
        auto step = history.step_at(i);
        VERIFY(step.has_value());
        VERIFY(*step == used_steps[i]);
    }

    history.traverse_to(current_used_step_index);
    history.record_web_content_session_history_state_installed(used_steps[current_used_step_index]);
}

static void initialize_canonical_traversable_from_ui_entries(WebView::CanonicalTraversable& traversable, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, URL::URL const&)
{
    auto& history = const_cast<WebView::TraversableSessionHistory&>(traversable.session_history());
    initialize_history_from_ui_entries(history, move(entries), move(used_steps), current_used_step_index);
}

static void expect_entry_state(Web::HTML::SessionHistoryEntryDescriptor const& entry, u8 expected_classic_history_api_state, u8 expected_navigation_api_state, StringView expected_navigation_api_key, StringView expected_navigation_api_id, Web::HTML::ScrollRestorationMode expected_scroll_restoration_mode)
{
    EXPECT(entry.classic_history_api_state == state_record(expected_classic_history_api_state));
    EXPECT(entry.navigation_api_state == state_record(expected_navigation_api_state));
    EXPECT_EQ(entry.navigation_api_key, Utf16String::from_utf8(expected_navigation_api_key));
    EXPECT_EQ(entry.navigation_api_id, Utf16String::from_utf8(expected_navigation_api_id));
    EXPECT_EQ(entry.scroll_restoration_mode, expected_scroll_restoration_mode);
}

static void expect_entry_document_state(Web::HTML::SessionHistoryEntryDescriptor const& entry, u64 expected_document_state_id, StringView expected_navigable_target_name)
{
    EXPECT_EQ(entry.document_state.id, test_document_state_id(expected_document_state_id));
    EXPECT_EQ(entry.document_state.navigable_target_name, Utf16String::from_utf8(expected_navigable_target_name));
}

static void expect_entry_viewport_scroll_position(Web::HTML::SessionHistoryEntryDescriptor const& entry, Web::CSSPixelPoint expected_viewport_scroll_position)
{
    VERIFY(entry.scroll_position_data.viewport_scroll_position.has_value());
    EXPECT_EQ(*entry.scroll_position_data.viewport_scroll_position, expected_viewport_scroll_position);
}

static void expect_entry_resource(Web::HTML::SessionHistoryEntryDescriptor const& entry, StringView expected_resource)
{
    if (expected_resource == "post"sv) {
        EXPECT(entry.document_state.resource.has<Web::HTML::POSTResource>());
        return;
    }

    if (expected_resource == "string"sv) {
        EXPECT(entry.document_state.resource.has<Utf16String>());
        return;
    }

    VERIFY(expected_resource == "none"sv);
    EXPECT(entry.document_state.resource.has<Empty>());
}

static void expect_used_step(WebView::TraversableSessionHistory const& history, size_t index, i32 expected_step)
{
    auto step = history.step_at(index);
    VERIFY(step.has_value());
    EXPECT_EQ(*step, expected_step);
}

static void expect_step_to_restore(Optional<i32> step, i32 expected_step)
{
    VERIFY(step.has_value());
    EXPECT_EQ(*step, expected_step);
}

static void expect_nested_history(Web::HTML::SessionHistoryEntryDescriptor const& entry, size_t index, StringView expected_id, size_t expected_size)
{
    VERIFY(index < entry.document_state.nested_histories.size());
    EXPECT_EQ(entry.document_state.nested_histories[index].id, navigable_id(expected_id));
    EXPECT_EQ(entry.document_state.nested_histories[index].entries.size(), expected_size);
}

static void expect_nested_entry(Web::HTML::SessionHistoryNestedHistoryDescriptor const& nested_history, size_t index, i32 expected_step, StringView expected_url)
{
    VERIFY(index < nested_history.entries.size());
    EXPECT_EQ(nested_history.entries[index].step, expected_step);
    EXPECT_EQ(nested_history.entries[index].url, parse_url(expected_url));
}

TEST_CASE(targeted_current_entry_update_updates_navigation_api_state)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                },
        { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto updated_entry = entry(0, "https://a.example/"sv, 1, 9, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    EXPECT(apply_current_entry_update(history, SessionHistoryEntryUpdateKind::NavigationAPIState, move(updated_entry)));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_state(*current_entry, 1, 9, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_current_entry_update_rejects_structural_change)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                },
        { 0 }, 0);

    auto updated_entry = entry(0, "https://a.example/"sv, 1, 9, "key-b"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    EXPECT(!apply_current_entry_update(history, SessionHistoryEntryUpdateKind::NavigationAPIState, move(updated_entry)));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_state(*current_entry, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
}

TEST_CASE(accepted_targeted_current_entry_update_keeps_web_content_state_synchronized)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                                  },
        { 0 }, 0, parse_url("https://a.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto updated_entry = entry(0, "https://a.example/"sv, 1, 9, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::current_entry_update(SessionHistoryEntryUpdateKind::NavigationAPIState, move(updated_entry)));
    EXPECT(update.accepted);
    EXPECT_EQ(update.dump_reason, "did-update-current-entry"sv);
    EXPECT(!update.should_update_navigation_action_state);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(rejected_targeted_current_entry_update_marks_web_content_state_stale)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                                  },
        { 0 }, 0, parse_url("https://a.example/"sv));

    auto updated_entry = entry(0, "https://a.example/"sv, 1, 9, "key-b"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::current_entry_update(SessionHistoryEntryUpdateKind::NavigationAPIState, move(updated_entry)));
    EXPECT(!update.accepted);
    EXPECT_EQ(update.dump_reason, "rejected-current-entry-update"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(child_navigable_destruction_recomputes_used_steps)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://top.example/"sv, 1, "main"sv, {
                                                                                                        nested_history("frame-1"sv, {
                                                                                                                                        entry(0, "https://frame.example/a"sv, 2, "frame"sv),
                                                                                                                                        entry(1, "https://frame.example/b"sv, 2, "frame"sv),
                                                                                                                                    }),
                                                                                                    }),
                                                },
        { 0, 1 }, 1);
    EXPECT(history.web_content_history_is_synchronized());

    EXPECT(apply_child_navigable_destruction(history, test_document_state_id(1), navigable_id("frame-1"sv), 0));

    EXPECT_EQ(history.used_step_count(), 1uz);
    expect_used_step(history, 0, 0);
    expect_current_entry(history, 0, "https://top.example/"sv);
    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.nested_histories.is_empty());
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(accepted_child_navigable_creation_keeps_web_content_state_synchronized)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://top.example/"sv, 1, "main"sv),
                                                                  },
        { 0 }, 0, parse_url("https://top.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::child_navigable_created({
        .parent_document_state_id = test_document_state_id(1),
        .navigable_id = navigable_id("frame-1"sv),
        .initial_entry = entry(0, "https://frame.example/"sv, 2, "frame"sv),
        .current_step = 0,
    }));
    EXPECT(update.accepted);
    EXPECT_EQ(update.dump_reason, "did-create-child-navigable-session-history"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto* current_entry = traversable.session_history().current_entry();
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 1);
}

TEST_CASE(rejected_child_navigable_creation_marks_web_content_state_stale)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://top.example/"sv, 1, "main"sv),
                                                                  },
        { 0 }, 0, parse_url("https://top.example/"sv));

    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::child_navigable_created({
        .parent_document_state_id = test_document_state_id(99),
        .navigable_id = navigable_id("frame-1"sv),
        .initial_entry = entry(0, "https://frame.example/"sv, 2, "frame"sv),
        .current_step = 0,
    }));
    EXPECT(!update.accepted);
    EXPECT_EQ(update.dump_reason, "rejected-child-navigable-session-history-creation"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(targeted_current_entry_update_updates_scroll_restoration_mode)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                },
        { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto updated_entry = entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Manual);
    EXPECT(apply_current_entry_update(history, SessionHistoryEntryUpdateKind::ScrollRestorationMode, move(updated_entry)));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_state(*current_entry, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Manual);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_scroll_restoration_update_rejects_other_state_change)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                },
        { 0 }, 0);

    auto updated_entry = entry(0, "https://a.example/"sv, 1, 9, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Manual);
    EXPECT(!apply_current_entry_update(history, SessionHistoryEntryUpdateKind::ScrollRestorationMode, move(updated_entry)));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_state(*current_entry, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
}

TEST_CASE(targeted_current_entry_update_updates_scroll_position_data)
{
    WebView::TraversableSessionHistory history;

    auto initial_entry = entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    initial_entry.scroll_position_data.viewport_scroll_position = { 0, 100 };
    initialize_history_from_ui_entries(history, { initial_entry }, { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto updated_entry = initial_entry;
    updated_entry.scroll_position_data.viewport_scroll_position = { 0, 300 };
    EXPECT(apply_current_entry_update(history, SessionHistoryEntryUpdateKind::ScrollPositionData, move(updated_entry)));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_viewport_scroll_position(*current_entry, { 0, 300 });
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_scroll_position_update_accepts_reconstructed_current_entry_identity)
{
    WebView::TraversableSessionHistory history;

    auto initial_entry = entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    initial_entry.scroll_position_data.viewport_scroll_position = { 0, 100 };
    initialize_history_from_ui_entries(history, { initial_entry }, { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto updated_entry = initial_entry;
    updated_entry.document_state.id = test_document_state_id(99);
    updated_entry.navigation_api_state = state_record(9);
    updated_entry.scroll_position_data.viewport_scroll_position = { 0, 300 };
    EXPECT(apply_current_entry_update(history, SessionHistoryEntryUpdateKind::ScrollPositionData, move(updated_entry)));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_viewport_scroll_position(*current_entry, { 0, 300 });
    expect_entry_state(*current_entry, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_scroll_position_update_rejects_other_state_change)
{
    WebView::TraversableSessionHistory history;

    auto initial_entry = entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    initial_entry.scroll_position_data.viewport_scroll_position = { 0, 100 };
    initialize_history_from_ui_entries(history, { initial_entry }, { 0 }, 0);

    auto updated_entry = initial_entry;
    updated_entry.navigation_api_state = state_record(9);
    updated_entry.scroll_position_data.viewport_scroll_position = { 0, 300 };
    EXPECT(!apply_current_entry_update(history, SessionHistoryEntryUpdateKind::ScrollPositionData, move(updated_entry)));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_viewport_scroll_position(*current_entry, { 0, 100 });
    expect_entry_state(*current_entry, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
}

TEST_CASE(targeted_reload_pending_update_updates_same_document_entries)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://b.example/"sv),
                                                    entry(2, "https://c.example/"sv),
                                                    entry(3, "https://same-document.example/"sv, 6, "main"sv),
                                                    entry(5, "https://same-document.example/current"sv, 6, "main"sv),
                                                },
        { 0, 1, 2, 3, 5 }, 4);

    auto updated_entry = entry_with_reload_pending(5, "https://same-document.example/current"sv, 6, "main"sv, {});
    EXPECT(apply_current_entry_update(history, SessionHistoryEntryUpdateKind::DocumentStateReloadPending, move(updated_entry)));

    auto* first_same_document_entry = history.entry_at(3);
    VERIFY(first_same_document_entry);
    EXPECT(first_same_document_entry->document_state.reload_pending);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.reload_pending);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_reload_pending_update_clears_same_document_entries)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://b.example/"sv),
                                                    entry(2, "https://c.example/"sv),
                                                    entry_with_reload_pending(3, "https://same-document.example/"sv, 6, "main"sv, {}),
                                                    entry_with_reload_pending(5, "https://same-document.example/current"sv, 6, "main"sv, {}),
                                                },
        { 0, 1, 2, 3, 5 }, 4);

    auto updated_entry = entry(5, "https://same-document.example/current"sv, 6, "main"sv);
    EXPECT(apply_current_entry_update(history, SessionHistoryEntryUpdateKind::DocumentStateReloadPending, move(updated_entry)));

    auto* first_same_document_entry = history.entry_at(3);
    VERIFY(first_same_document_entry);
    EXPECT(!first_same_document_entry->document_state.reload_pending);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(!current_entry->document_state.reload_pending);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(accepted_multiple_reload_pending_updates_keep_web_content_state_synchronized)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry_with_reload_pending(0, "https://top.example/"sv, 1, "main"sv, {
                                                                                                                                              nested_history("frame-1"sv, {
                                                                                                                                                                              entry_with_reload_pending(0, "https://frame.example/"sv, 2, "frame"sv, {}),
                                                                                                                                                                          }),
                                                                                                                                          }),
                                                                  },
        { 0 }, 0, parse_url("https://top.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto nested_update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::current_entry_update(
        SessionHistoryEntryUpdateKind::DocumentStateReloadPending,
        entry(0, "https://frame.example/"sv, 2, "frame"sv)));
    EXPECT(nested_update.accepted);
    EXPECT_EQ(nested_update.dump_reason, "did-update-current-entry"sv);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto top_update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::current_entry_update(
        SessionHistoryEntryUpdateKind::DocumentStateReloadPending,
        entry(0, "https://top.example/"sv, 1, "main"sv, {
                                                            nested_history("frame-1"sv, {
                                                                                            entry(0, "https://frame.example/"sv, 2, "frame"sv),
                                                                                        }),
                                                        })));
    EXPECT(top_update.accepted);
    EXPECT_EQ(top_update.dump_reason, "did-update-current-entry"sv);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto* current_entry = traversable.session_history().current_entry();
    VERIFY(current_entry);
    EXPECT(!current_entry->document_state.reload_pending);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 1);
    EXPECT(!current_entry->document_state.nested_histories[0].entries[0].document_state.reload_pending);
}

TEST_CASE(targeted_reload_pending_update_rejects_other_state_change)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                },
        { 0 }, 0);

    auto updated_entry = entry(0, "https://a.example/"sv, 1, 9, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
    updated_entry.document_state.reload_pending = true;
    EXPECT(!apply_current_entry_update(history, SessionHistoryEntryUpdateKind::DocumentStateReloadPending, move(updated_entry)));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(!current_entry->document_state.reload_pending);
    expect_entry_state(*current_entry, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto);
}

TEST_CASE(targeted_document_state_population_update_updates_same_document_entries)
{
    WebView::TraversableSessionHistory history;

    auto first_same_document_entry = entry_with_unpopulated_document_state(3, "https://same-document.example/"sv, 6, "main"sv);
    first_same_document_entry.document_state.resource = Utf16String::from_utf8("body"sv);
    auto current_same_document_entry = entry_with_unpopulated_document_state(5, "https://same-document.example/current"sv, 6, "main"sv);
    current_same_document_entry.document_state.resource = Utf16String::from_utf8("body"sv);

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://b.example/"sv),
                                                    entry(2, "https://c.example/"sv),
                                                    move(first_same_document_entry),
                                                    move(current_same_document_entry),
                                                },
        { 0, 1, 2, 3, 5 }, 4);

    auto updated_entry = entry_with_unpopulated_document_state(5, "https://same-document.example/current"sv, 6, "main"sv);
    updated_entry.document_state.ever_populated = true;
    updated_entry.document_state.origin = parse_url("https://same-document.example/"sv).origin();
    updated_entry.document_state.request_referrer = parse_url("https://referrer.example/"sv);
    updated_entry.document_state.resource = Empty {};
    EXPECT(apply_current_entry_update(history, SessionHistoryEntryUpdateKind::DocumentStatePopulation, move(updated_entry)));

    auto* first_entry = history.entry_at(3);
    VERIFY(first_entry);
    EXPECT(first_entry->document_state.ever_populated);
    VERIFY(first_entry->document_state.origin.has_value());
    EXPECT(first_entry->document_state.origin->is_same_origin(parse_url("https://same-document.example/"sv).origin()));
    EXPECT(first_entry->document_state.request_referrer.has<URL::URL>());
    EXPECT_EQ(first_entry->document_state.request_referrer.get<URL::URL>(), parse_url("https://referrer.example/"sv));
    expect_entry_resource(*first_entry, "none"sv);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.ever_populated);
    VERIFY(current_entry->document_state.origin.has_value());
    EXPECT(current_entry->document_state.origin->is_same_origin(parse_url("https://same-document.example/"sv).origin()));
    EXPECT(current_entry->document_state.request_referrer.has<URL::URL>());
    EXPECT_EQ(current_entry->document_state.request_referrer.get<URL::URL>(), parse_url("https://referrer.example/"sv));
    expect_entry_resource(*current_entry, "none"sv);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_document_state_population_update_rejects_other_state_change)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry_with_unpopulated_document_state(0, "https://a.example/"sv, 6, "main"sv),
                                                },
        { 0 }, 0);

    auto updated_entry = entry_with_unpopulated_document_state(0, "https://b.example/"sv, 6, "main"sv);
    updated_entry.document_state.ever_populated = true;
    EXPECT(!apply_current_entry_update(history, SessionHistoryEntryUpdateKind::DocumentStatePopulation, move(updated_entry)));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT_EQ(current_entry->url, parse_url("https://a.example/"sv));
    EXPECT(!current_entry->document_state.ever_populated);
}

TEST_CASE(targeted_document_state_target_name_update_updates_same_document_entries)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://b.example/"sv),
                                                    entry(2, "https://c.example/"sv),
                                                    entry(3, "https://same-document.example/"sv, 6, ""sv),
                                                    entry(5, "https://same-document.example/current"sv, 6, ""sv),
                                                },
        { 0, 1, 2, 3, 5 }, 4);

    auto updated_entry = entry(5, "https://same-document.example/current"sv, 6, "named"sv);
    EXPECT(apply_current_entry_update(history, SessionHistoryEntryUpdateKind::DocumentStateNavigableTargetName, move(updated_entry)));

    auto* first_same_document_entry = history.entry_at(3);
    VERIFY(first_same_document_entry);
    expect_entry_document_state(*first_same_document_entry, 6, "named"sv);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_document_state(*current_entry, 6, "named"sv);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_nested_current_entry_update_updates_duplicate_nested_copies)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://b.example/"sv),
                                                    entry(2, "https://c.example/"sv),
                                                    entry(3, "https://nested.example/"sv, 6, "main"sv, {
                                                                                                           nested_history("frame-1"sv, {
                                                                                                                                           entry(3, "https://frame.example/a"sv, 7, ""sv),
                                                                                                                                           entry(4, "https://frame.example/b"sv, 8, ""sv),
                                                                                                                                       }),
                                                                                                       }),
                                                    entry(5, "https://nested.example/same-document"sv, 6, "main"sv, {
                                                                                                                        nested_history("frame-1"sv, {
                                                                                                                                                        entry(3, "https://frame.example/a"sv, 7, ""sv),
                                                                                                                                                        entry(4, "https://frame.example/b"sv, 8, ""sv),
                                                                                                                                                    }),
                                                                                                                    }),
                                                },
        { 0, 1, 2, 3, 4, 5 }, 5);

    auto updated_entry = entry(4, "https://frame.example/b"sv, 8, ""sv);
    updated_entry.navigation_api_state = state_record(9);
    EXPECT(apply_current_entry_update(history, SessionHistoryEntryUpdateKind::NavigationAPIState, move(updated_entry)));

    auto* first_same_document_entry = history.entry_at(3);
    VERIFY(first_same_document_entry);
    EXPECT(first_same_document_entry->document_state.nested_histories[0].entries[1].navigation_api_state == state_record(9));

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.nested_histories[0].entries[1].navigation_api_state == state_record(9));
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_top_level_same_document_navigation_appends_entry_and_clears_forward_history)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, "main"sv),
                                                    entry(1, "https://b.example/"sv, 2, "main"sv),
                                                    entry(2, "https://c.example/"sv, 3, "main"sv),
                                                },
        { 0, 1, 2 }, 1);
    EXPECT(history.web_content_history_is_synchronized());

    auto pushed_entry = entry(3, "https://b.example/#pushed"sv, 2, "main"sv);
    EXPECT(apply_top_level_same_document_navigation(history, move(pushed_entry), {}, 3));

    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/"sv);
    expect_current_entry(history, 3, "https://b.example/#pushed"sv);
    expect_used_step(history, 0, 0);
    expect_used_step(history, 1, 1);
    expect_used_step(history, 2, 3);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_top_level_same_document_navigation_replaces_reused_forward_step)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/#source"sv, 1, "main"sv),
                                                    entry(1, "https://a.example/#old-current"sv, 1, "main"sv),
                                                },
        { 0, 1 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto pushed_entry = entry(1, "https://a.example/#new-current"sv, 1, "main"sv);
    EXPECT(apply_top_level_same_document_navigation(history, move(pushed_entry), {}, 1));

    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    expect_entry(history, 0, 0, "https://a.example/#source"sv);
    expect_current_entry(history, 1, "https://a.example/#new-current"sv);
    expect_used_step(history, 0, 0);
    expect_used_step(history, 1, 1);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_top_level_same_document_navigation_replaces_current_entry)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, "main"sv),
                                                    entry(1, "https://b.example/"sv, 2, "main"sv),
                                                    entry(2, "https://c.example/"sv, 3, "main"sv),
                                                },
        { 0, 1, 2 }, 1);

    auto replacement_entry = entry(1, "https://b.example/#replacement"sv, 2, "main"sv);
    replacement_entry.navigation_api_state = state_record(4);
    EXPECT(apply_top_level_same_document_navigation(history, move(replacement_entry), 1, 1));

    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/#replacement"sv);
    expect_entry(history, 2, 2, "https://c.example/"sv);
    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->navigation_api_state == state_record(4));
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_top_level_same_document_navigation_updates_unsynchronized_state_without_reproving)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv), test_document_state_id(1));
    history.navigate(parse_url("https://b.example/"sv), test_document_state_id(2));

    auto committed_entry = entry(1, "https://b.example/"sv, 2, "main"sv);
    EXPECT(apply_top_level_cross_document_navigation(history, move(committed_entry), 1));
    EXPECT(!history.web_content_history_is_synchronized());

    auto replacement_entry = entry(1, "https://b.example/#replacement"sv, 2, "main"sv);
    EXPECT(apply_top_level_same_document_navigation(history, move(replacement_entry), 1, 1));

    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/#replacement"sv);
    EXPECT(!history.web_content_history_is_synchronized());

    auto pushed_entry = entry(2, "https://b.example/#pushed"sv, 2, "main"sv);
    EXPECT(apply_top_level_same_document_navigation(history, move(pushed_entry), {}, 2));

    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/#replacement"sv);
    expect_current_entry(history, 2, "https://b.example/#pushed"sv);
    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_top_level_same_document_navigation_accepts_unsynchronized_state_without_reproving)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, "main"sv),
                                                    entry(1, "https://b.example/"sv, 2, "main"sv),
                                                },
        { 0, 1 }, 1);

    history.forget_web_content_state();

    auto pushed_entry = entry(2, "https://b.example/#pushed"sv, 2, "main"sv);
    EXPECT(apply_top_level_same_document_navigation(history, move(pushed_entry), {}, 2));
    EXPECT_EQ(history.size(), 3uz);
    expect_current_entry(history, 2, "https://b.example/#pushed"sv);
    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_nested_same_document_navigation_appends_entry)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://top.example/"sv, 1, "main"sv, {
                                                                                                        nested_history("frame-1"sv, {
                                                                                                                                        entry(0, "https://frame.example/"sv, 2, "frame"sv),
                                                                                                                                    }),
                                                                                                    }),
                                                },
        { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto pushed_entry = entry(1, "https://frame.example/#pushed"sv, 2, "frame"sv);
    EXPECT(apply_nested_same_document_navigation(history, test_document_state_id(1), navigable_id("frame-1"sv), move(pushed_entry), {}, 1));

    EXPECT_EQ(history.size(), 1uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    expect_used_step(history, 0, 0);
    expect_used_step(history, 1, 1);
    expect_current_entry(history, 0, "https://top.example/"sv);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 2);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 0, "https://frame.example/"sv);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 1, 1, "https://frame.example/#pushed"sv);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(accepted_nested_same_document_navigation_keeps_web_content_state_synchronized)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://top.example/"sv, 1, "main"sv, {
                                                                                                                          nested_history("frame-1"sv, {
                                                                                                                                                          entry(0, "https://frame.example/"sv, 2, "frame"sv),
                                                                                                                                                      }),
                                                                                                                      }),
                                                                  },
        { 0 }, 0, parse_url("https://top.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto pushed_entry = entry(1, "https://frame.example/#pushed"sv, 2, "frame"sv);
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::nested_same_document_navigation(
        nested_same_document_navigation_details(test_document_state_id(1), navigable_id("frame-1"sv), move(pushed_entry), {}, 1)));
    EXPECT(update.accepted);
    EXPECT_EQ(update.dump_reason, "did-apply-nested-same-document-navigation"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(rejected_nested_same_document_navigation_marks_web_content_state_stale)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://top.example/"sv, 1, "main"sv, {
                                                                                                                          nested_history("frame-1"sv, {
                                                                                                                                                          entry(0, "https://frame.example/"sv, 2, "frame"sv),
                                                                                                                                                      }),
                                                                                                                      }),
                                                                  },
        { 0 }, 0, parse_url("https://top.example/"sv));

    auto pushed_entry = entry(1, "https://frame.example/#pushed"sv, 2, "frame"sv);
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::nested_same_document_navigation(
        nested_same_document_navigation_details(test_document_state_id(99), navigable_id("frame-1"sv), move(pushed_entry), {}, 1)));
    EXPECT(!update.accepted);
    EXPECT_EQ(update.dump_reason, "rejected-nested-same-document-navigation"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(targeted_nested_cross_document_navigation_appends_entry)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://top.example/"sv, 1, "main"sv, {
                                                                                                        nested_history("frame-1"sv, {
                                                                                                                                        entry(0, "https://frame-a.example/"sv, 2, "frame"sv),
                                                                                                                                    }),
                                                                                                    }),
                                                },
        { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto pushed_entry = entry(1, "https://frame-b.example/"sv, 3, "frame"sv);
    EXPECT(apply_nested_cross_document_navigation(history, test_document_state_id(1), navigable_id("frame-1"sv), move(pushed_entry), 1));

    EXPECT_EQ(history.size(), 1uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    expect_used_step(history, 0, 0);
    expect_used_step(history, 1, 1);
    expect_current_entry(history, 0, "https://top.example/"sv);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 2);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 0, "https://frame-a.example/"sv);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 1, 1, "https://frame-b.example/"sv);
    EXPECT_EQ(current_entry->document_state.nested_histories[0].entries[1].document_state.id, test_document_state_id(3));
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_nested_cross_document_navigation_replaces_entry)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://top.example/"sv, 1, "main"sv, {
                                                                                                        nested_history("frame-1"sv, {
                                                                                                                                        entry(0, "https://frame-a.example/"sv, 2, "frame"sv),
                                                                                                                                    }),
                                                                                                    }),
                                                },
        { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto replaced_entry = entry(0, "https://frame-b.example/"sv, 3, "frame"sv);
    EXPECT(apply_nested_cross_document_navigation(history, test_document_state_id(1), navigable_id("frame-1"sv), move(replaced_entry), 0));

    EXPECT_EQ(history.size(), 1uz);
    EXPECT_EQ(history.used_step_count(), 1uz);
    expect_used_step(history, 0, 0);
    expect_current_entry(history, 0, "https://top.example/"sv);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 1);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 0, "https://frame-b.example/"sv);
    EXPECT_EQ(current_entry->document_state.nested_histories[0].entries[0].document_state.id, test_document_state_id(3));
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(accepted_nested_cross_document_navigation_keeps_web_content_state_synchronized)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://top.example/"sv, 1, "main"sv, {
                                                                                                                          nested_history("frame-1"sv, {
                                                                                                                                                          entry(0, "https://frame-a.example/"sv, 2, "frame"sv),
                                                                                                                                                      }),
                                                                                                                      }),
                                                                  },
        { 0 }, 0, parse_url("https://top.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto pushed_entry = entry(1, "https://frame-b.example/"sv, 3, "frame"sv);
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::nested_cross_document_navigation(
        nested_cross_document_navigation_details(test_document_state_id(1), navigable_id("frame-1"sv), move(pushed_entry), 1)));
    EXPECT(update.accepted);
    EXPECT_EQ(update.dump_reason, "did-apply-nested-cross-document-navigation"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(rejected_nested_cross_document_navigation_marks_web_content_state_stale)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://top.example/"sv, 1, "main"sv, {
                                                                                                                          nested_history("frame-1"sv, {
                                                                                                                                                          entry(0, "https://frame-a.example/"sv, 2, "frame"sv),
                                                                                                                                                      }),
                                                                                                                      }),
                                                                  },
        { 0 }, 0, parse_url("https://top.example/"sv));

    auto pushed_entry = entry(1, "https://frame-b.example/"sv, 3, "frame"sv);
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::nested_cross_document_navigation(
        nested_cross_document_navigation_details(test_document_state_id(99), navigable_id("frame-1"sv), move(pushed_entry), 1)));
    EXPECT(!update.accepted);
    EXPECT_EQ(update.dump_reason, "rejected-nested-cross-document-navigation"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(targeted_top_level_cross_document_navigation_updates_predicted_push_without_reproving)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, "main"sv),
                                                },
        { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto document_state_id = allocate_test_ui_process_document_state_id();
    history.navigate(parse_url("https://b.example/"sv), document_state_id);
    EXPECT(!history.web_content_history_is_synchronized());

    auto committed_entry = entry(1, "https://b.example/"sv);
    committed_entry.document_state.id = document_state_id;
    committed_entry.document_state.ever_populated = true;
    committed_entry.document_state.origin = parse_url("https://b.example/"sv).origin();
    committed_entry.navigation_api_key = Utf16String::from_utf8("key-b"sv);
    committed_entry.navigation_api_id = Utf16String::from_utf8("id-b"sv);
    EXPECT(apply_top_level_cross_document_navigation(history, move(committed_entry), 1));

    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_top_level_cross_document_navigation_updates_predicted_push_with_committed_document_state_id_without_reproving)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, "main"sv),
                                                },
        { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto predicted_document_state_id = allocate_test_ui_process_document_state_id();
    history.navigate(parse_url("https://b.example/"sv), predicted_document_state_id);
    EXPECT(!history.web_content_history_is_synchronized());

    auto committed_entry = entry(1, "https://b.example/"sv, 2, "main"sv);
    committed_entry.document_state.ever_populated = true;
    committed_entry.document_state.origin = parse_url("https://b.example/"sv).origin();
    EXPECT(apply_top_level_cross_document_navigation(history, move(committed_entry), 1));

    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_document_state(*current_entry, 2, "main"sv);
    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_top_level_cross_document_navigation_updates_redirected_predicted_push_without_reproving)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, "main"sv),
                                                },
        { 0 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto predicted_document_state_id = allocate_test_ui_process_document_state_id();
    history.navigate(parse_url("https://redirect.example/"sv), predicted_document_state_id);
    EXPECT(!history.web_content_history_is_synchronized());

    auto committed_entry = entry(1, "https://b.example/"sv, 2, "main"sv);
    committed_entry.document_state.ever_populated = true;
    committed_entry.document_state.origin = parse_url("https://b.example/"sv).origin();
    EXPECT(apply_top_level_cross_document_navigation(history, move(committed_entry), 1));

    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_document_state(*current_entry, 2, "main"sv);
    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(targeted_top_level_cross_document_navigation_updates_redirected_forward_replacement_without_reproving)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, "main"sv),
                                                    entry(1, "https://old-b.example/"sv, 2, "main"sv),
                                                },
        { 0, 1 }, 0);
    EXPECT(history.web_content_history_is_synchronized());

    auto predicted_document_state_id = allocate_test_ui_process_document_state_id();
    history.navigate(parse_url("https://redirect.example/"sv), predicted_document_state_id);
    EXPECT(!history.web_content_history_is_synchronized());

    auto committed_entry = entry(1, "https://b.example/"sv, 3, "main"sv);
    committed_entry.document_state.ever_populated = true;
    committed_entry.document_state.origin = parse_url("https://b.example/"sv).origin();
    EXPECT(apply_top_level_cross_document_navigation(history, move(committed_entry), 1));

    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_document_state(*current_entry, 3, "main"sv);
    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(top_level_cross_document_navigation_updates_single_entry_without_reproving)
{
    WebView::TraversableSessionHistory history;

    auto document_state_id = allocate_test_ui_process_document_state_id();
    history.navigate(parse_url("https://a.example/"sv), document_state_id);
    EXPECT(!history.web_content_history_is_synchronized());

    auto committed_entry = entry(0, "https://a.example/"sv);
    committed_entry.document_state.id = document_state_id;
    committed_entry.document_state.ever_populated = true;
    committed_entry.document_state.origin = parse_url("https://a.example/"sv).origin();
    EXPECT(apply_top_level_cross_document_navigation(history, move(committed_entry), 0));

    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(accepted_top_level_cross_document_navigation_keeps_web_content_state_synchronized)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://a.example/"sv, 1, "main"sv),
                                                                  },
        { 0 }, 0, parse_url("https://a.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto document_state_id = allocate_test_ui_process_document_state_id();
    auto start = traversable.did_start_navigation(parse_url("https://b.example/"sv), Empty {}, document_state_id, false, Web::Bindings::NavigationHistoryBehavior::Push, false);
    VERIFY(start.dump_reason.has_value());
    EXPECT_EQ(*start.dump_reason, "did-start-navigation"sv);
    VERIFY(traversable.pending_session_history_navigation().has_value());
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());

    auto committed_entry = entry(1, "https://b.example/"sv);
    committed_entry.document_state.id = document_state_id;
    committed_entry.document_state.ever_populated = true;
    committed_entry.document_state.origin = parse_url("https://b.example/"sv).origin();
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::top_level_cross_document_navigation(
        top_level_cross_document_navigation_details(move(committed_entry), 1)));
    EXPECT(update.accepted);
    EXPECT_EQ(update.dump_reason, "did-apply-top-level-cross-document-navigation"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(!traversable.pending_session_history_navigation().has_value());
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(accepted_top_level_cross_document_navigation_with_committed_document_state_id_keeps_web_content_state_synchronized)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://a.example/"sv, 1, "main"sv),
                                                                  },
        { 0 }, 0, parse_url("https://a.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto predicted_document_state_id = allocate_test_ui_process_document_state_id();
    auto start = traversable.did_start_navigation(parse_url("https://b.example/"sv), Empty {}, predicted_document_state_id, false, Web::Bindings::NavigationHistoryBehavior::Push, false);
    VERIFY(start.dump_reason.has_value());
    EXPECT_EQ(*start.dump_reason, "did-start-navigation"sv);
    VERIFY(traversable.pending_session_history_navigation().has_value());
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());

    auto committed_entry = entry(1, "https://b.example/"sv, 2, "main"sv);
    committed_entry.document_state.ever_populated = true;
    committed_entry.document_state.origin = parse_url("https://b.example/"sv).origin();
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::top_level_cross_document_navigation(
        top_level_cross_document_navigation_details(move(committed_entry), 1)));
    EXPECT(update.accepted);
    EXPECT_EQ(update.dump_reason, "did-apply-top-level-cross-document-navigation"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(!traversable.pending_session_history_navigation().has_value());
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(accepted_top_level_cross_document_navigation_with_redirected_url_keeps_web_content_state_synchronized)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://a.example/"sv, 1, "main"sv),
                                                                  },
        { 0 }, 0, parse_url("https://a.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto predicted_document_state_id = allocate_test_ui_process_document_state_id();
    auto start = traversable.did_start_navigation(parse_url("https://redirect.example/"sv), Empty {}, predicted_document_state_id, false, Web::Bindings::NavigationHistoryBehavior::Push, false);
    VERIFY(start.dump_reason.has_value());
    EXPECT_EQ(*start.dump_reason, "did-start-navigation"sv);
    VERIFY(traversable.pending_session_history_navigation().has_value());
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());

    auto committed_entry = entry(1, "https://b.example/"sv, 2, "main"sv);
    committed_entry.document_state.ever_populated = true;
    committed_entry.document_state.origin = parse_url("https://b.example/"sv).origin();
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::top_level_cross_document_navigation(
        top_level_cross_document_navigation_details(move(committed_entry), 1)));
    EXPECT(update.accepted);
    EXPECT_EQ(update.dump_reason, "did-apply-top-level-cross-document-navigation"sv);
    EXPECT(update.should_update_navigation_action_state);
    VERIFY(update.current_url.has_value());
    EXPECT_EQ(*update.current_url, parse_url("https://b.example/"sv));
    EXPECT(!traversable.pending_session_history_navigation().has_value());
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(rejected_top_level_cross_document_navigation_marks_web_content_state_stale)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://a.example/"sv, 1, "main"sv),
                                                                  },
        { 0 }, 0, parse_url("https://a.example/"sv));

    auto document_state_id = allocate_test_ui_process_document_state_id();
    auto start = traversable.did_start_navigation(parse_url("https://b.example/"sv), Empty {}, document_state_id, false, Web::Bindings::NavigationHistoryBehavior::Push, false);
    VERIFY(start.dump_reason.has_value());
    EXPECT_EQ(*start.dump_reason, "did-start-navigation"sv);

    auto committed_entry = entry(2, "https://c.example/"sv);
    committed_entry.document_state.id = document_state_id;
    committed_entry.document_state.ever_populated = true;
    auto update = traversable.did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation::top_level_cross_document_navigation(
        top_level_cross_document_navigation_details(move(committed_entry), 2)));
    EXPECT(!update.accepted);
    EXPECT_EQ(update.dump_reason, "rejected-top-level-cross-document-navigation"sv);
    EXPECT(update.should_update_navigation_action_state);
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(used_steps_include_nested_history_steps)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, {
                                                                                         nested_history("frame-1"sv, {
                                                                                                                         entry(1, "https://frame.example/a"sv),
                                                                                                                     }),
                                                                                     }),
                                                    entry(2, "https://b.example/"sv),
                                                },
        { 0, 1, 2 }, 2);
    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    EXPECT(!history.has_only_top_level_used_steps());
    EXPECT(history.current_step_is_top_level_entry());
    EXPECT(!history.current_step_to_restore_after_loading_top_level_entry().has_value());
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 2, "https://b.example/"sv);

    auto* first_entry = history.entry_at(0);
    VERIFY(first_entry);
    expect_nested_history(*first_entry, 0, "frame-1"sv, 1);
    expect_nested_entry(first_entry->document_state.nested_histories[0], 0, 1, "https://frame.example/a"sv);

    auto target_step_index = history.target_step_index_for_delta(-1);
    VERIFY(target_step_index.has_value());
    expect_used_step(history, *target_step_index, 1);
    EXPECT_EQ(history.entry_for_step(1), nullptr);
    auto* target_top_level_entry = history.top_level_entry_for_step(1);
    VERIFY(target_top_level_entry);
    EXPECT_EQ(target_top_level_entry->url, parse_url("https://a.example/"sv));

    auto traversal_target = history.traversal_target_for_delta(-1);
    VERIFY(traversal_target.has_value());
    EXPECT_EQ(traversal_target->target_step_index, *target_step_index);
    EXPECT_EQ(traversal_target->target_step, 1);
    EXPECT_EQ(traversal_target->target_top_level_entry, target_top_level_entry);
    EXPECT(!traversal_target->target_step_is_top_level_entry);
    EXPECT(traversal_target->changes_top_level_entry);

    history.traverse_to(*target_step_index);
    expect_current_entry(history, 0, "https://a.example/"sv);
    EXPECT(!history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 1);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
}

TEST_CASE(traversal_target_for_top_level_step)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv), allocate_test_ui_process_document_state_id());
    history.navigate(parse_url("https://b.example/"sv), allocate_test_ui_process_document_state_id());
    history.navigate(parse_url("https://c.example/"sv), allocate_test_ui_process_document_state_id());
    history.traverse_to(1);

    EXPECT(history.current_step_is_top_level_entry());
    EXPECT(!history.current_step_to_restore_after_loading_top_level_entry().has_value());

    auto traversal_target = history.traversal_target_for_delta(1);
    VERIFY(traversal_target.has_value());
    EXPECT_EQ(traversal_target->target_step_index, 2uz);
    EXPECT_EQ(traversal_target->target_step, 2);
    VERIFY(traversal_target->target_top_level_entry);
    EXPECT_EQ(traversal_target->target_top_level_entry->url, parse_url("https://c.example/"sv));
    EXPECT(traversal_target->target_step_is_top_level_entry);
    EXPECT(traversal_target->changes_top_level_entry);
}

TEST_CASE(traversal_target_for_nested_step_in_current_top_level_entry)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, {
                                                                                         nested_history("frame-1"sv, {
                                                                                                                         entry(0, "https://frame.example/a"sv),
                                                                                                                         entry(1, "https://frame.example/b"sv),
                                                                                                                     }),
                                                                                     }),
                                                },
        { 0, 1 }, 0);

    auto traversal_target = history.traversal_target_for_delta(1);
    VERIFY(traversal_target.has_value());
    EXPECT_EQ(traversal_target->target_step_index, 1uz);
    EXPECT_EQ(traversal_target->target_step, 1);
    VERIFY(traversal_target->target_top_level_entry);
    EXPECT_EQ(traversal_target->target_top_level_entry->url, parse_url("https://a.example/"sv));
    EXPECT(!traversal_target->target_step_is_top_level_entry);
    EXPECT(!traversal_target->changes_top_level_entry);

    history.traverse_to(traversal_target->target_step_index);
    EXPECT(!history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 1);
}

TEST_CASE(current_top_level_step_can_need_nested_history_restore)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 7, "main"sv, {
                                                                                                      nested_history("frame-1"sv, {
                                                                                                                                      entry(0, "https://frame.example/a"sv),
                                                                                                                                      entry(1, "https://frame.example/b"sv),
                                                                                                                                  }),
                                                                                                  }),
                                                    entry(2, "https://a.example/same-document"sv, 7, "main"sv, {
                                                                                                                   nested_history("frame-1"sv, {
                                                                                                                                                   entry(0, "https://frame.example/a"sv),
                                                                                                                                                   entry(1, "https://frame.example/b"sv),
                                                                                                                                               }),
                                                                                                               }),
                                                },
        { 0, 1, 2 }, 2);
    EXPECT(history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 2);

    auto traversal_target = history.traversal_target_for_delta(-1);
    VERIFY(traversal_target.has_value());
    EXPECT_EQ(traversal_target->target_step_index, 1uz);
    EXPECT_EQ(traversal_target->target_step, 1);
    VERIFY(traversal_target->target_top_level_entry);
    EXPECT_EQ(traversal_target->target_top_level_entry->url, parse_url("https://a.example/"sv));
    EXPECT(!traversal_target->target_step_is_top_level_entry);
    EXPECT(traversal_target->changes_top_level_entry);

    traversal_target = history.traversal_target_for_step(1);
    VERIFY(traversal_target.has_value());
    EXPECT_EQ(traversal_target->target_step_index, 1uz);
    EXPECT_EQ(traversal_target->target_step, 1);
    VERIFY(traversal_target->target_top_level_entry);
    EXPECT_EQ(traversal_target->target_top_level_entry->url, parse_url("https://a.example/"sv));
    EXPECT(!traversal_target->target_step_is_top_level_entry);
    EXPECT(traversal_target->changes_top_level_entry);

    EXPECT(!history.traversal_target_for_step(42).has_value());
}

TEST_CASE(state_installed_web_content_needs_command_to_restore_current_step)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, {
                                                                                         nested_history("frame-1"sv, {
                                                                                                                         entry(0, "https://frame.example/a"sv),
                                                                                                                         entry(1, "https://frame.example/b"sv),
                                                                                                                     }),
                                                                                     }),
                                                    entry(2, "https://b.example/"sv),
                                                },
        { 0, 1, 2 }, 1);
    EXPECT(!history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 1);

    history.forget_web_content_state();
    record_web_content_state_installed_at_current_top_level_entry(history);
    EXPECT(!history.web_content_history_is_synchronized());

    auto traversal_target = history.traversal_target_for_delta(1);
    VERIFY(traversal_target.has_value());
    EXPECT_EQ(traversal_target->target_step, 2);

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, {
                                                                                         nested_history("frame-1"sv, {
                                                                                                                         entry(0, "https://frame.example/a"sv),
                                                                                                                         entry(1, "https://frame.example/b"sv),
                                                                                                                     }),
                                                                                     }),
                                                    entry(2, "https://b.example/"sv),
                                                },
        { 0, 1, 2 }, 1);

    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(recorded_state_install_does_not_change_nested_ui_current_step)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, {
                                                                                         nested_history("frame-1"sv, {
                                                                                                                         entry(0, "https://frame.example/a"sv),
                                                                                                                         entry(1, "https://frame.example/b"sv),
                                                                                                                     }),
                                                                                     }),
                                                    entry(2, "https://b.example/"sv),
                                                },
        { 0, 1, 2 }, 1);
    EXPECT(!history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 1);

    history.forget_web_content_state();
    history.record_web_content_session_history_state_installed(0);

    EXPECT(!history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 1);
    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(applied_web_content_traversal_updates_current_step)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://b.example/"sv),
                                                    entry(2, "https://c.example/"sv),
                                                },
        { 0, 1, 2 }, 2);
    expect_current_entry(history, 2, "https://c.example/"sv);

    EXPECT(apply_traversal_current_step(history, 1));
    expect_current_entry(history, 1, "https://b.example/"sv);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    EXPECT(history.web_content_history_is_synchronized());

    auto target_a = history.traversal_target_for_delta(-1);
    VERIFY(target_a.has_value());
    EXPECT_EQ(target_a->target_step, 0);

    EXPECT(!apply_traversal_current_step(history, 42));
    expect_current_entry(history, 1, "https://b.example/"sv);
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(applied_web_content_traversal_from_unknown_updates_current_step_without_synchronizing_state)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://b.example/"sv),
                                                    entry(2, "https://c.example/"sv),
                                                },
        { 0, 1, 2 }, 2);

    history.traverse_to(1);
    EXPECT(apply_traversal_current_step(history, 0));
    expect_current_entry(history, 0, "https://a.example/"sv);
    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(recorded_state_install_tracks_preserved_document_state_ids)
{
    WebView::TraversableSessionHistory history;
    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://a.example/replaced"sv, 1, "main"sv),
                                                    entry(2, "https://a.example/pushed"sv, 1, "main"sv),
                                                },
        { 0, 1, 2 }, 0);

    history.forget_web_content_state();
    history.record_web_content_session_history_state_installed(0);

    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(state_install_ack_rejects_unexpected_current_step)
{
    WebView::CanonicalTraversable traversable;
    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://a.example/"sv, 1, "main"sv),
                                                                      entry(1, "https://b.example/"sv, 2, "main"sv),
                                                                  },
        { 0, 1 }, 1, parse_url("https://b.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    traversable.prepare_to_install_web_content_session_history_state();
    auto state_install = traversable.prepare_web_content_session_history_state_install(false);
    VERIFY(state_install.has_value());

    auto current_step = state_install->current_step;
    auto state_install_id = traversable.did_send_web_content_session_history_state_install(0);

    auto ack = traversable.did_receive_web_content_session_history_state_install_ack(state_install_id, true, current_step);
    EXPECT_EQ(ack.dump_reason, "webcontent-session-history-state-install-ack-mismatch"sv);
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(state_install_ack_ignores_stale_state_install_id)
{
    WebView::CanonicalTraversable traversable;
    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://a.example/"sv, 1, "main"sv),
                                                                      entry(1, "https://b.example/"sv, 2, "main"sv),
                                                                  },
        { 0, 1 }, 1, parse_url("https://b.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    traversable.prepare_to_install_web_content_session_history_state();
    auto state_install = traversable.prepare_web_content_session_history_state_install(false);
    VERIFY(state_install.has_value());

    auto state_install_id = traversable.did_send_web_content_session_history_state_install(state_install->current_step);

    auto stale_ack = traversable.did_receive_web_content_session_history_state_install_ack(state_install_id + 1, true, state_install->current_step);
    EXPECT(stale_ack.ignored);
    EXPECT_EQ(stale_ack.dump_reason, "ignored-stale-webcontent-session-history-state-install-ack"sv);
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());

    auto ack = traversable.did_receive_web_content_session_history_state_install_ack(state_install_id, true, state_install->current_step);
    EXPECT(!ack.ignored);
    EXPECT_EQ(ack.dump_reason, "webcontent-session-history-state-install-ack"sv);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(traversal_loads_from_ui_while_webcontent_state_install_is_pending)
{
    WebView::CanonicalTraversable traversable;
    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://example.test/a"sv, 1, "main"sv),
                                                                      entry(1, "https://example.test/b"sv, 2, "main"sv),
                                                                  },
        { 0, 1 }, 0, parse_url("https://example.test/a"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    traversable.prepare_to_install_web_content_session_history_state();
    EXPECT(traversable.pending_web_content_session_history_state_install().should_install_state);

    auto traversal = traversable.traverse_the_history_by_delta(1, WebView::CheckForCancelation::Yes, parse_url("https://example.test/a"sv), {});
    EXPECT_EQ(traversal.action, WebView::HistoryTraversalAction::LoadCurrentEntryFromUIProcess);
    EXPECT(!traversal.outcome.waiting_for_cancelation_check);
    EXPECT(!traversal.command.has_value());
    expect_current_entry(traversable.session_history(), 1, "https://example.test/b"sv);
    EXPECT(traversable.pending_web_content_session_history_state_install().should_install_state);
}

TEST_CASE(post_load_state_install_ignores_webcontent_commit_batch_until_reinstall)
{
    WebView::CanonicalTraversable traversable;
    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://example.test/d"sv, 21, "main"sv),
                                                                      entry(1, "https://example.test/state?replace"sv, 22, "ladybird-history-name"sv),
                                                                      entry(2, "https://example.test/state?push"sv, 22, "ladybird-history-name"sv),
                                                                  },
        { 0, 1, 2 }, 2, parse_url("https://example.test/state?push"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    traversable.did_crash_requiring_web_content_session_history_state_install();
    EXPECT(traversable.prepare_to_restore_current_session_history_entry_from_ui_process());
    EXPECT(traversable.pending_web_content_session_history_state_install().should_install_after_current_history_load);

    auto replacement_entry = entry(2, "https://example.test/state?push"sv, 23, ""sv);
    auto mutation = Web::HTML::WebContentSessionHistoryMutation::top_level_cross_document_navigation(
        top_level_cross_document_navigation_details(move(replacement_entry), 2));
    mutation.epoch = traversable.web_content_session_history_epoch();
    mutation.operation_id = 1;

    Web::HTML::WebContentSessionHistoryMutationBatch batch {
        .epoch = traversable.web_content_session_history_epoch(),
        .operation_id = 1,
        .mutations = { move(mutation) },
        .final_current_step = 2,
    };
    auto result = traversable.did_receive_web_content_session_history_mutation_batch(move(batch));
    EXPECT(!result.accepted);
    EXPECT_EQ(result.dump_reason, "ignored-session-history-mutation-batch-before-post-load-state-install"sv);

    auto* current_entry = traversable.session_history().current_entry();
    VERIFY(current_entry);
    expect_entry_document_state(*current_entry, 22, "ladybird-history-name"sv);
}

TEST_CASE(applied_history_step_result_updates_current_step)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://example.test/a"sv, 1, "main"sv),
                                                                      entry(1, "https://example.test/b"sv, 2, "main"sv),
                                                                  },
        { 0, 1 }, 0, parse_url("https://example.test/a"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto traversal = traversable.traverse_the_history_by_delta(1, WebView::CheckForCancelation::No, parse_url("https://example.test/a"sv), {});
    EXPECT_EQ(traversal.action, WebView::HistoryTraversalAction::ApplySessionHistoryStepInWebContent);
    VERIFY(traversal.command.has_value());
    EXPECT_EQ(traversal.command->target_step, 1);
    VERIFY(traversal.target_step.has_value());
    EXPECT_EQ(*traversal.target_step, 1);
    EXPECT(!traversal.webdriver_pending_navigation_completes_with_session_history_update);

    auto step_result = traversable.did_apply_session_history_step(traversal.command->command_id, true, Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(step_result.dump_reason, "did-apply-session-history-step-command"sv);
    EXPECT(step_result.should_update_navigation_action_state);
    VERIFY(step_result.current_url.has_value());
    EXPECT_EQ(*step_result.current_url, parse_url("https://example.test/b"sv));
    EXPECT(!step_result.should_complete_webdriver_pending_navigation);
    expect_current_entry(traversable.session_history(), 1, "https://example.test/b"sv);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(applied_history_step_result_updates_current_step_without_mutation)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://example.test/a"sv, 1, "main"sv),
                                                                      entry(1, "https://example.test/b"sv, 2, "main"sv),
                                                                  },
        { 0, 1 }, 0, parse_url("https://example.test/a"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto traversal = traversable.traverse_the_history_by_delta(1, WebView::CheckForCancelation::No, parse_url("https://example.test/a"sv), {});
    EXPECT_EQ(traversal.action, WebView::HistoryTraversalAction::ApplySessionHistoryStepInWebContent);
    VERIFY(traversal.command.has_value());
    EXPECT_EQ(traversal.command->target_step, 1);
    VERIFY(traversal.target_step.has_value());
    EXPECT_EQ(*traversal.target_step, 1);

    auto step_result = traversable.did_apply_session_history_step(traversal.command->command_id, true, Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(step_result.dump_reason, "did-apply-session-history-step-command"sv);
    EXPECT(!step_result.should_load_current_session_history_entry_from_ui_process);
    VERIFY(step_result.current_url.has_value());
    EXPECT_EQ(*step_result.current_url, parse_url("https://example.test/b"sv));
    expect_current_entry(traversable.session_history(), 1, "https://example.test/b"sv);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(applied_same_document_traversal_completes_webdriver_from_step_result)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://example.test/state?replace"sv, 7, "main"sv),
                                                                      entry(1, "https://example.test/state?push"sv, 7, "main"sv),
                                                                  },
        { 0, 1 }, 1, parse_url("https://example.test/state?push"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    auto traversal = traversable.traverse_the_history_by_delta(-1, WebView::CheckForCancelation::No, parse_url("https://example.test/state?push"sv), {});
    EXPECT_EQ(traversal.action, WebView::HistoryTraversalAction::ApplySessionHistoryStepInWebContent);
    VERIFY(traversal.command.has_value());
    EXPECT_EQ(traversal.command->target_step, 0);
    VERIFY(traversal.target_step.has_value());
    EXPECT_EQ(*traversal.target_step, 0);
    EXPECT(traversal.webdriver_pending_navigation_completes_with_session_history_update);

    auto step_result = traversable.did_apply_session_history_step(traversal.command->command_id, true, Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(step_result.dump_reason, "did-apply-session-history-step-command"sv);
    EXPECT(step_result.should_update_navigation_action_state);
    VERIFY(step_result.current_url.has_value());
    EXPECT_EQ(*step_result.current_url, parse_url("https://example.test/state?replace"sv));
    EXPECT(step_result.should_complete_webdriver_pending_navigation);
    expect_current_entry(traversable.session_history(), 0, "https://example.test/state?replace"sv);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(applied_history_step_result_restores_state_installed_current_step)
{
    WebView::CanonicalTraversable traversable;

    initialize_canonical_traversable_from_ui_entries(traversable, {
                                                                      entry(0, "https://a.example/"sv, {
                                                                                                           nested_history("frame-1"sv, {
                                                                                                                                           entry(0, "https://frame.example/a"sv),
                                                                                                                                           entry(1, "https://frame.example/b"sv),
                                                                                                                                       }),
                                                                                                       }),
                                                                  },
        { 0, 1 }, 1, parse_url("https://a.example/"sv));
    EXPECT(traversable.current_web_content_session_history_is_synchronized());

    traversable.prepare_to_install_web_content_session_history_state();
    auto state_install = traversable.prepare_web_content_session_history_state_install(false);
    VERIFY(state_install.has_value());
    auto state_install_id = traversable.did_send_web_content_session_history_state_install(state_install->current_step);

    auto current_step = state_install->current_step;

    auto ack = traversable.did_receive_web_content_session_history_state_install_ack(state_install_id, true, current_step);
    EXPECT_EQ(ack.dump_reason, "webcontent-session-history-state-install-ack"sv);
    VERIFY(ack.step_to_traverse.has_value());
    EXPECT_EQ(*ack.step_to_traverse, 1);
    VERIFY(ack.command_to_apply.has_value());
    EXPECT_EQ(ack.command_to_apply->target_step, 1);
    EXPECT(!traversable.current_web_content_session_history_is_synchronized());

    auto step_result = traversable.did_apply_session_history_step(ack.command_to_apply->command_id, true, Web::HTML::HistoryStepResult::Applied);
    EXPECT_EQ(step_result.dump_reason, "did-apply-restore-current-session-history-step-command"sv);
    EXPECT(step_result.should_update_navigation_action_state);
    EXPECT(step_result.should_complete_webdriver_pending_navigation);
    EXPECT(traversable.current_web_content_session_history_is_synchronized());
}

TEST_CASE(explicit_web_content_sync_marks_state_synchronized)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 1, "main"sv),
                                                    entry(1, "https://b.example/"sv, 2, "main"sv),
                                                },
        { 0, 1 }, 1);
    EXPECT(history.web_content_history_is_synchronized());

    history.mark_web_content_history_match_unproven();
    EXPECT(!history.web_content_history_is_synchronized());

    history.record_web_content_history_synchronized();
    EXPECT(history.web_content_history_is_synchronized());
}

TEST_CASE(traversal_target_for_delta_outside_used_steps)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv), allocate_test_ui_process_document_state_id());

    EXPECT(!history.traversal_target_for_delta(-1).has_value());
    EXPECT(!history.traversal_target_for_delta(0).has_value());
    EXPECT(!history.traversal_target_for_delta(1).has_value());
}

TEST_CASE(ui_owned_history_stores_nested_history_descriptors)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, {
                                                                                         nested_history("frame-1"sv, {
                                                                                                                         entry(0, "https://frame.example/a"sv),
                                                                                                                         entry(1, "https://frame.example/b"sv),
                                                                                                                     }),
                                                                                     }),
                                                },
        { 0, 1 }, 1);
    EXPECT_EQ(history.size(), 1uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    EXPECT(!history.has_only_top_level_used_steps());
    EXPECT(!history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 1);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT_EQ(current_entry->step, 0);
    EXPECT_EQ(current_entry->url, parse_url("https://a.example/"sv));
    expect_nested_history(*current_entry, 0, "frame-1"sv, 2);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 0, "https://frame.example/a"sv);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 1, 1, "https://frame.example/b"sv);
}

TEST_CASE(navigate_clears_forward_nested_history_entries)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, {
                                                                                         nested_history("frame-1"sv, {
                                                                                                                         entry(0, "https://frame.example/a"sv),
                                                                                                                         entry(1, "https://frame.example/b"sv),
                                                                                                                         entry(2, "https://frame.example/c"sv),
                                                                                                                     }),
                                                                                     }),
                                                    entry(3, "https://c.example/"sv),
                                                },
        { 0, 1, 2, 3 }, 1);

    history.navigate(parse_url("https://b.example/"sv), allocate_test_ui_process_document_state_id());

    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 2, "https://b.example/"sv);
    expect_used_step(history, 0, 0);
    expect_used_step(history, 1, 1);
    expect_used_step(history, 2, 2);

    auto* first_entry = history.entry_at(0);
    VERIFY(first_entry);
    expect_nested_history(*first_entry, 0, "frame-1"sv, 2);
    expect_nested_entry(first_entry->document_state.nested_histories[0], 0, 0, "https://frame.example/a"sv);
    expect_nested_entry(first_entry->document_state.nested_histories[0], 1, 1, "https://frame.example/b"sv);
}

TEST_CASE(history_log_entries_marks_current_entries_steps_and_reload_pending)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry_with_post_resource(1, "https://b.example/"sv),
                                                    entry_with_reload_pending(2, "https://c.example/"sv, 7, "main"sv, {
                                                                                                                          nested_history("frame"sv, {
                                                                                                                                                        entry(3, "https://frame.example/"sv),
                                                                                                                                                    }),
                                                                                                                      }),
                                                },
        { 0, 1, 2, 3 }, 1);

    EXPECT_EQ(history_log_entries(history), ByteString { "entries=[0:0:https://a.example/, *1:1:https://b.example/ "
                                                         "document_state={id=3:1001, resource=post}, 2:2:https://c.example/ "
                                                         "document_state={id=3:7, reload_pending=true, target_name=main} "
                                                         "nested={1:2=[3:https://frame.example/]}] "
                                                         "used_steps=[0:0, *1:1, 2:2, 3:3]"sv });

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    auto serialized_entry = WebView::history_json_entry(*current_entry, true);
    EXPECT_EQ(serialized_entry.get_string("resource"sv), "post"sv);

    auto* reload_pending_entry = history.entry_at(2);
    VERIFY(reload_pending_entry);
    auto serialized_reload_pending_entry = WebView::history_json_entry(*reload_pending_entry, false);
    EXPECT_EQ(serialized_reload_pending_entry.get_bool("reloadPending"sv), true);
    EXPECT_EQ(serialized_reload_pending_entry.get_string("resource"sv), "none"sv);
}

TEST_CASE(history_log_entries_marks_nested_histories)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://b.example/"sv, 7, "main"sv, {
                                                                                                      nested_history("frame"sv, {
                                                                                                                                    entry(2, "https://frame.example/"sv),
                                                                                                                                }),
                                                                                                  }),
                                                },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(history_log_entries(history), ByteString { "entries=[0:0:https://a.example/, *1:1:https://b.example/ "
                                                         "document_state={id=3:7, target_name=main} "
                                                         "nested={1:2=[2:https://frame.example/]}] "
                                                         "used_steps=[0:0, 1:1, *2:2]"sv });
}

TEST_CASE(clear_current_entry_reload_pending)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry_with_reload_pending(1, "https://b.example/"sv, 7, "main"sv, {}),
                                                },
        { 0, 1 }, 1);

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.reload_pending);

    history.clear_current_entry_reload_pending();

    current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(!current_entry->document_state.reload_pending);
}

TEST_CASE(mark_current_entry_reload_pending)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv),
                                                    entry(1, "https://b.example/"sv),
                                                },
        { 0, 1 }, 1);

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(!current_entry->document_state.reload_pending);
    EXPECT(history.web_content_history_is_synchronized());

    history.mark_current_entry_reload_pending();

    current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.reload_pending);
    EXPECT(!history.web_content_history_is_synchronized());
}

TEST_CASE(navigate_preserves_document_resource)
{
    WebView::TraversableSessionHistory history;

    history.navigate(parse_url("https://a.example/"sv), allocate_test_ui_process_document_state_id());
    history.navigate(parse_url("https://b.example/"sv), allocate_test_ui_process_document_state_id(), Web::HTML::POSTResource {
                                                                                                          .request_body = MUST(ByteBuffer::copy("name=ladybird"sv.bytes())),
                                                                                                          .request_content_type = Web::HTML::POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded,
                                                                                                      });

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_resource(*current_entry, "post"sv);
}

TEST_CASE(replace_current_entry_preserves_document_resource)
{
    WebView::TraversableSessionHistory history;

    history.navigate(parse_url("https://a.example/"sv), allocate_test_ui_process_document_state_id());
    history.replace_current_entry(parse_url("https://b.example/"sv), allocate_test_ui_process_document_state_id(), Web::HTML::POSTResource {
                                                                                                                       .request_body = MUST(ByteBuffer::copy("name=ladybird"sv.bytes())),
                                                                                                                       .request_content_type = Web::HTML::POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded,
                                                                                                                   });

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT_EQ(current_entry->url, parse_url("https://b.example/"sv));
    expect_entry_resource(*current_entry, "post"sv);
}

TEST_CASE(replace_current_entry_discards_replaced_document_state)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 7, "main"sv, {
                                                                                                      nested_history("frame-1"sv, {
                                                                                                                                      entry(0, "https://frame.example/a"sv),
                                                                                                                                      entry(1, "https://frame.example/b"sv),
                                                                                                                                  }),
                                                                                                  }),
                                                    entry(2, "https://c.example/"sv),
                                                },
        { 0, 1, 2 }, 0);

    history.replace_current_entry(parse_url("https://b.example/"sv), allocate_test_ui_process_document_state_id(), Web::HTML::POSTResource {
                                                                                                                       .request_body = MUST(ByteBuffer::copy("name=ladybird"sv.bytes())),
                                                                                                                       .request_content_type = Web::HTML::POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded,
                                                                                                                   });

    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    EXPECT(!history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_current_entry(history, 0, "https://b.example/"sv);
    expect_entry(history, 1, 2, "https://c.example/"sv);
    expect_used_step(history, 0, 0);
    expect_used_step(history, 1, 2);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.id.namespace_id > 0);
    EXPECT(current_entry->document_state.id.local_id > 0);
    EXPECT(current_entry->document_state.navigable_target_name.is_empty());
    EXPECT(current_entry->document_state.nested_histories.is_empty());
    expect_entry_resource(*current_entry, "post"sv);
}

TEST_CASE(replace_current_entry_at_nested_step_keeps_current_step_valid)
{
    WebView::TraversableSessionHistory history;

    initialize_history_from_ui_entries(history, {
                                                    entry(0, "https://a.example/"sv, 7, "main"sv, {
                                                                                                      nested_history("frame-1"sv, {
                                                                                                                                      entry(0, "https://frame.example/a"sv),
                                                                                                                                      entry(1, "https://frame.example/b"sv),
                                                                                                                                  }),
                                                                                                  }),
                                                    entry(2, "https://c.example/"sv),
                                                },
        { 0, 1, 2 }, 1);

    history.replace_current_entry(parse_url("https://b.example/"sv), allocate_test_ui_process_document_state_id(), Empty {});

    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    EXPECT(!history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_current_entry(history, 1, "https://b.example/"sv);
    expect_entry(history, 1, 2, "https://c.example/"sv);
    expect_used_step(history, 0, 1);
    expect_used_step(history, 1, 2);
    EXPECT(history.current_step_is_top_level_entry());
    EXPECT(!history.current_step_to_restore_after_loading_top_level_entry().has_value());

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.nested_histories.is_empty());
}

TEST_CASE(replace_current_entry_url_keeps_document_resource)
{
    WebView::TraversableSessionHistory history;

    history.navigate(parse_url("https://a.example/"sv), allocate_test_ui_process_document_state_id(), Web::HTML::POSTResource {
                                                                                                          .request_body = MUST(ByteBuffer::copy("name=ladybird"sv.bytes())),
                                                                                                          .request_content_type = Web::HTML::POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded,
                                                                                                      });
    history.replace_current_entry_url(parse_url("https://b.example/"sv), allocate_test_ui_process_document_state_id());

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT_EQ(current_entry->url, parse_url("https://b.example/"sv));
    expect_entry_resource(*current_entry, "post"sv);
}
