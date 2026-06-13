/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/SessionHistory.h>

static Web::HTML::SessionHistoryNestedHistoryDescriptor nested_history(StringView id, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries)
{
    return {
        .id = MUST(String::from_utf8(id)),
        .entries = move(entries),
    };
}

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

static Web::HTML::SerializationRecord state_record(u8 byte)
{
    Web::HTML::SerializationRecord record;
    record.append(byte);
    return record;
}

static Web::HTML::SessionHistoryEntryDescriptor create_test_entry(i32 step, URL::URL url)
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
    entry.navigation_api_key = MUST(String::from_utf8(navigation_api_key));
    entry.navigation_api_id = MUST(String::from_utf8(navigation_api_id));
    entry.scroll_restoration_mode = scroll_restoration_mode;
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry_with_scroll_position(i32 step, StringView url, Web::CSSPixelPoint viewport_scroll_position)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.scroll_position_data.viewport_scroll_position = viewport_scroll_position;
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry(i32 step, StringView url, u64 document_state_id, StringView navigable_target_name)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.document_state.id = document_state_id;
    entry.document_state.ever_populated = true;
    entry.document_state.navigable_target_name = MUST(String::from_utf8(navigable_target_name));
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry_with_reload_pending(i32 step, StringView url, u64 document_state_id, StringView navigable_target_name, Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor> nested_histories)
{
    auto entry = create_test_entry(step, parse_url(url));
    entry.document_state.id = document_state_id;
    entry.document_state.reload_pending = true;
    entry.document_state.ever_populated = true;
    entry.document_state.navigable_target_name = MUST(String::from_utf8(navigable_target_name));
    entry.document_state.nested_histories = move(nested_histories);
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
    entry.document_state.id = document_state_id;
    entry.document_state.ever_populated = true;
    entry.document_state.navigable_target_name = MUST(String::from_utf8(navigable_target_name));
    entry.document_state.nested_histories = move(nested_histories);
    return entry;
}

static Web::HTML::SessionHistoryEntryDescriptor entry(i32 step, URL::URL url)
{
    return create_test_entry(step, move(url));
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

static void expect_entry_state(Web::HTML::SessionHistoryEntryDescriptor const& entry, u8 expected_classic_history_api_state, u8 expected_navigation_api_state, StringView expected_navigation_api_key, StringView expected_navigation_api_id, Web::HTML::ScrollRestorationMode expected_scroll_restoration_mode)
{
    EXPECT(entry.classic_history_api_state == state_record(expected_classic_history_api_state));
    EXPECT(entry.navigation_api_state == state_record(expected_navigation_api_state));
    EXPECT_EQ(entry.navigation_api_key, MUST(String::from_utf8(expected_navigation_api_key)));
    EXPECT_EQ(entry.navigation_api_id, MUST(String::from_utf8(expected_navigation_api_id)));
    EXPECT_EQ(entry.scroll_restoration_mode, expected_scroll_restoration_mode);
}

static void expect_entry_document_state(Web::HTML::SessionHistoryEntryDescriptor const& entry, u64 expected_document_state_id, StringView expected_navigable_target_name)
{
    EXPECT_EQ(entry.document_state.id, expected_document_state_id);
    EXPECT_EQ(entry.document_state.navigable_target_name, MUST(String::from_utf8(expected_navigable_target_name)));
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
        EXPECT(entry.document_state.resource.has<String>());
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
    EXPECT_EQ(entry.document_state.nested_histories[index].id, MUST(String::from_utf8(expected_id)));
    EXPECT_EQ(entry.document_state.nested_histories[index].entries.size(), expected_size);
}

static void expect_nested_entry(Web::HTML::SessionHistoryNestedHistoryDescriptor const& nested_history, size_t index, i32 expected_step, StringView expected_url)
{
    VERIFY(index < nested_history.entries.size());
    EXPECT_EQ(nested_history.entries[index].step, expected_step);
    EXPECT_EQ(nested_history.entries[index].url, parse_url(expected_url));
}

TEST_CASE(complete_web_content_update_replaces_mirror)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT_EQ(history.size(), 2uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
}

TEST_CASE(fresh_process_snapshot_does_not_drop_previous_entries)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 2uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
}

TEST_CASE(replace_navigation_snapshot_drops_speculative_entry)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv),
                                                                     entry(1, "https://b.example/"sv),
                                                                 },
        { 0, 1 }, 1);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.navigate(parse_url("https://c.example/"sv));

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://c.example/"sv),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT_EQ(history.size(), 2uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://c.example/"sv);
}

TEST_CASE(replace_navigation_snapshot_drops_speculative_first_entry)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv),
                                                                 },
        { 0 }, 0);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.replace_current_entry_url(parse_url("https://b.example/"sv));

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://b.example/"sv),
                                                         },
        { 0 }, 0);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT_EQ(history.size(), 1uz);
    EXPECT_EQ(history.used_step_count(), 1uz);
    EXPECT(!history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_current_entry(history, 0, "https://b.example/"sv);
}

TEST_CASE(fresh_single_entry_snapshot_does_not_drop_previous_entries)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://b.example/"sv),
                                                         },
        { 0 }, 0);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 2uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);

    auto web_content_known_used_steps = history.web_content_known_used_steps();
    EXPECT_EQ(web_content_known_used_steps.size(), 1uz);
    EXPECT_EQ(web_content_known_used_steps[0], 1);
    auto target_a = history.traversal_target_for_delta(-1);
    VERIFY(target_a.has_value());
    EXPECT(!history.web_content_can_traverse_to(*target_a));
}

TEST_CASE(complete_matching_snapshot_updates_current_index)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.navigate(parse_url("https://c.example/"sv));

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://c.example/"sv),
                                                         },
        { 0, 1, 2 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_current_entry(history, 1, "https://b.example/"sv);
}

TEST_CASE(matching_snapshot_updates_session_history_entry_state)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                                 },
        { 0 }, 0);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, 3, 4, "key-b"sv, "id-b"sv, Web::HTML::ScrollRestorationMode::Manual),
                                                         },
        { 0 }, 0);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT_EQ(history.size(), 1uz);
    EXPECT_EQ(history.used_step_count(), 1uz);
    expect_current_entry(history, 0, "https://a.example/"sv);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_state(*current_entry, 3, 4, "key-b"sv, "id-b"sv, Web::HTML::ScrollRestorationMode::Manual);
}

TEST_CASE(matching_snapshot_updates_session_history_entry_scroll_position)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry_with_scroll_position(0, "https://a.example/"sv, { 0, 100 }),
                                                                 },
        { 0 }, 0);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    auto update_result = history.update_from_web_content({
                                                             entry_with_scroll_position(0, "https://a.example/"sv, { 0, 300 }),
                                                         },
        { 0 }, 0);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT_EQ(history.size(), 1uz);
    EXPECT_EQ(history.used_step_count(), 1uz);
    expect_current_entry(history, 0, "https://a.example/"sv);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_viewport_scroll_position(*current_entry, { 0, 300 });
}

TEST_CASE(matching_snapshot_updates_document_state)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv, 1, "target-a"sv),
                                                                 },
        { 0 }, 0);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, 1, "target-b"sv),
                                                         },
        { 0 }, 0);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT_EQ(history.size(), 1uz);
    EXPECT_EQ(history.used_step_count(), 1uz);
    expect_current_entry(history, 0, "https://a.example/"sv);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_document_state(*current_entry, 1, "target-b"sv);
}

TEST_CASE(partial_snapshot_preserves_forward_history_after_fallback_traversal)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.navigate(parse_url("https://c.example/"sv));
    history.traverse_to(1);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    expect_entry(history, 2, 2, "https://c.example/"sv);
}

TEST_CASE(partial_snapshot_preserves_entries_outside_web_content_known_history)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.navigate(parse_url("https://c.example/"sv));
    history.traverse_to(1);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());

    auto web_content_known_used_steps = history.web_content_known_used_steps();
    EXPECT_EQ(web_content_known_used_steps.size(), 1uz);
    EXPECT_EQ(web_content_known_used_steps[0], 1);

    auto target_a = history.traversal_target_for_delta(-1);
    VERIFY(target_a.has_value());
    EXPECT(!history.web_content_can_traverse_to(*target_a));

    auto target_c = history.traversal_target_for_delta(1);
    VERIFY(target_c.has_value());
    EXPECT(!history.web_content_can_traverse_to(*target_c));
}

TEST_CASE(complete_snapshot_accepts_reassigned_document_state_ids)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv, 1, "top-a"sv),
                                                                     entry(1, "https://nested.example/"sv, 2, "top-nested"sv, {
                                                                                                                                  nested_history("frame-1"sv, {
                                                                                                                                                                  entry(1, "https://frame.example/a"sv, 3, "frame"sv),
                                                                                                                                                                  entry(2, "https://frame.example/b"sv, 4, "frame"sv),
                                                                                                                                                              }),
                                                                                                                              }),
                                                                     entry(3, "https://c.example/"sv, 5, "top-c"sv),
                                                                 },
        { 0, 1, 2, 3 }, 2);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, 10, "top-a"sv),
                                                             entry(1, "https://nested.example/"sv, 20, "top-nested"sv, {
                                                                                                                           nested_history("frame-1"sv, {
                                                                                                                                                           entry(1, "https://frame.example/a"sv, 30, "frame"sv),
                                                                                                                                                           entry(2, "https://frame.example/b"sv, 40, "frame"sv),
                                                                                                                                                       }),
                                                                                                                       }),
                                                             entry(3, "https://c.example/"sv, 50, "top-c"sv),
                                                         },
        { 0, 1, 2, 3 }, 2);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 4uz);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_current_entry(history, 1, "https://nested.example/"sv);
    expect_used_step(history, 2, 2);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_document_state(*current_entry, 2, "top-nested"sv);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 2);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 1, "https://frame.example/a"sv);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 1, 2, "https://frame.example/b"sv);
    expect_entry_document_state(current_entry->document_state.nested_histories[0].entries[0], 3, "frame"sv);
    expect_entry_document_state(current_entry->document_state.nested_histories[0].entries[1], 4, "frame"sv);
}

TEST_CASE(partial_snapshot_updates_nested_history_and_preserves_forward_history)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.navigate(parse_url("https://c.example/"sv));
    history.traverse_to(1);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv, {
                                                                                                  nested_history("frame-1"sv, {
                                                                                                                                  entry(1, "https://frame.example/a"sv),
                                                                                                                              }),
                                                                                              }),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    EXPECT(!history.has_only_top_level_used_steps());
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    expect_entry(history, 2, 2, "https://c.example/"sv);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 1);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 1, "https://frame.example/a"sv);
}

TEST_CASE(fresh_process_snapshot_preserves_forward_history_at_first_entry)
{
    WebView::TraversableSessionHistory history;
    history.navigate(URL::about_blank());
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.traverse_to(0);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                         },
        { 0 }, 0);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT(!history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_current_entry(history, 0, "about:blank"sv);
    expect_entry(history, 1, 1, "https://a.example/"sv);
    expect_entry(history, 2, 2, "https://b.example/"sv);
}

TEST_CASE(partial_snapshot_extends_history_without_dropping_prefix)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://c.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/"sv);
    expect_current_entry(history, 2, "https://c.example/"sv);
}

TEST_CASE(partial_snapshot_accepts_same_document_update_at_current_step)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv, 1, "main"sv),
                                                                     entry(1, "https://b.example/"sv, 2, "main"sv),
                                                                 },
        { 0, 1 }, 1);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    auto update_result = history.update_from_web_content({
                                                             entry(1, "https://b.example/?replaced"sv, 20, "main"sv),
                                                             entry(2, "https://b.example/?pushed"sv, 20, "main"sv),
                                                         },
        { 1, 2 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/?replaced"sv);
    expect_current_entry(history, 2, "https://b.example/?pushed"sv);
    EXPECT(history.web_content_uses_ui_step_coordinates());

    auto web_content_known_used_steps = history.web_content_known_used_steps();
    EXPECT_EQ(web_content_known_used_steps.size(), 2uz);
    EXPECT_EQ(web_content_known_used_steps[0], 1);
    EXPECT_EQ(web_content_known_used_steps[1], 2);

    auto web_content_known_entries = history.web_content_known_entries();
    EXPECT_EQ(web_content_known_entries.size(), 2uz);
    EXPECT_EQ(web_content_known_entries[0].url, parse_url("https://b.example/?replaced"sv));
    EXPECT_EQ(web_content_known_entries[1].url, parse_url("https://b.example/?pushed"sv));
}

TEST_CASE(partial_snapshot_allows_web_content_traversal_within_known_suffix)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://c.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/"sv);
    expect_current_entry(history, 2, "https://c.example/"sv);

    auto web_content_known_used_steps = history.web_content_known_used_steps();
    EXPECT_EQ(web_content_known_used_steps.size(), 2uz);
    EXPECT_EQ(web_content_known_used_steps[0], 1);
    EXPECT_EQ(web_content_known_used_steps[1], 2);
    EXPECT(history.web_content_uses_ui_step_coordinates());

    auto target_b = history.traversal_target_for_delta(-1);
    VERIFY(target_b.has_value());
    EXPECT(history.web_content_can_traverse_to(*target_b));

    auto target_a = history.traversal_target_for_delta(-2);
    VERIFY(target_a.has_value());
    EXPECT(!history.web_content_can_traverse_to(*target_a));
}

TEST_CASE(reseeded_partial_snapshot_preserves_ui_only_history)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv),
                                                                     entry(1, "https://b.example/"sv),
                                                                     entry(2, "https://c.example/"sv),
                                                                     entry(3, "https://d.example/"sv),
                                                                 },
        { 0, 1, 2, 3 }, 3);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.traverse_to(1);
    history.forget_web_content_state();
    auto current_top_level_entry_index = history.current_top_level_entry_index();
    VERIFY(current_top_level_entry_index.has_value());
    history.did_seed_web_content_from_ui_process(*current_top_level_entry_index);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 4uz);
    EXPECT_EQ(history.used_step_count(), 4uz);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    expect_entry(history, 2, 2, "https://c.example/"sv);
    expect_entry(history, 3, 3, "https://d.example/"sv);

    auto web_content_known_used_steps = history.web_content_known_used_steps();
    EXPECT_EQ(web_content_known_used_steps.size(), 1uz);
    EXPECT_EQ(web_content_known_used_steps[0], 1);
    auto web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 1);

    auto target_a = history.traversal_target_for_delta(-1);
    VERIFY(target_a.has_value());
    EXPECT(!history.web_content_can_traverse_to(*target_a));

    auto target_c = history.traversal_target_for_delta(1);
    VERIFY(target_c.has_value());
    EXPECT(!history.web_content_can_traverse_to(*target_c));
}

TEST_CASE(partial_snapshot_replaces_forward_history_after_new_navigation)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.navigate(parse_url("https://c.example/"sv));
    history.traverse_to(1);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://d.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/"sv);
    expect_current_entry(history, 2, "https://d.example/"sv);
}

TEST_CASE(partial_snapshot_anchors_to_current_duplicate_url_entry)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://c.example/"sv));
    history.traverse_to(2);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://a.example/"sv),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 4uz);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/"sv);
    expect_current_entry(history, 2, "https://a.example/"sv);
    expect_entry(history, 3, 3, "https://c.example/"sv);
}

TEST_CASE(partial_snapshot_prefers_duplicate_url_entry_with_matching_document_state)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv, 1, "main"sv),
                                                                     entry(1, "https://b.example/"sv, 2, "main"sv),
                                                                     entry(2, "https://b.example/"sv, 3, "main"sv),
                                                                     entry(3, "https://c.example/"sv, 4, "main"sv),
                                                                 },
        { 0, 1, 2, 3 }, 3);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.traverse_to(2);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv, 2, "main"sv),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 4uz);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    expect_entry(history, 2, 2, "https://b.example/"sv);
    expect_entry(history, 3, 3, "https://c.example/"sv);
}

TEST_CASE(partial_snapshot_translates_fresh_process_steps_to_ui_steps)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(4, "https://a.example/"sv),
                                                                     entry(5, "https://b.example/"sv),
                                                                     entry(6, "https://c.example/"sv),
                                                                 },
        { 4, 5, 6 }, 2);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.traverse_to(1);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://d.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 4, "https://a.example/"sv);
    expect_entry(history, 1, 5, "https://b.example/"sv);
    expect_current_entry(history, 6, "https://d.example/"sv);

    auto target_b = history.traversal_target_for_delta(-1);
    VERIFY(target_b.has_value());
    EXPECT(!history.web_content_uses_ui_step_coordinates());
    EXPECT(!history.web_content_can_traverse_to(*target_b));
}

TEST_CASE(used_steps_include_nested_history_steps)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, {
                                                                                                  nested_history("frame-1"sv, {
                                                                                                                                  entry(1, "https://frame.example/a"sv),
                                                                                                                              }),
                                                                                              }),
                                                             entry(2, "https://b.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
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
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.navigate(parse_url("https://c.example/"sv));
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

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, {
                                                                                                  nested_history("frame-1"sv, {
                                                                                                                                  entry(0, "https://frame.example/a"sv),
                                                                                                                                  entry(1, "https://frame.example/b"sv),
                                                                                                                              }),
                                                                                              }),
                                                         },
        { 0, 1 }, 0);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

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

    auto update_result = history.update_from_web_content({
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

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
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

TEST_CASE(seeded_web_content_must_restore_nested_current_step_before_traversing)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, {
                                                                                                  nested_history("frame-1"sv, {
                                                                                                                                  entry(0, "https://frame.example/a"sv),
                                                                                                                                  entry(1, "https://frame.example/b"sv),
                                                                                                                              }),
                                                                                              }),
                                                             entry(2, "https://b.example/"sv),
                                                         },
        { 0, 1, 2 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT(!history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 1);

    history.forget_web_content_state();
    auto current_top_level_entry_index = history.current_top_level_entry_index();
    VERIFY(current_top_level_entry_index.has_value());
    history.did_seed_web_content_from_ui_process(*current_top_level_entry_index);

    auto web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 0);

    auto traversal_target = history.traversal_target_for_delta(1);
    VERIFY(traversal_target.has_value());
    EXPECT(!history.web_content_can_traverse_to(*traversal_target));

    update_result = history.update_from_web_content({
                                                        entry(0, "https://a.example/"sv, {
                                                                                             nested_history("frame-1"sv, {
                                                                                                                             entry(0, "https://frame.example/a"sv),
                                                                                                                             entry(1, "https://frame.example/b"sv),
                                                                                                                         }),
                                                                                         }),
                                                        entry(2, "https://b.example/"sv),
                                                    },
        { 0, 1, 2 }, 1);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 1);
    EXPECT(history.web_content_can_traverse_to(*traversal_target));
}

TEST_CASE(seed_ack_snapshot_preserves_nested_ui_current_step)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, {
                                                                                                  nested_history("frame-1"sv, {
                                                                                                                                  entry(0, "https://frame.example/a"sv),
                                                                                                                                  entry(1, "https://frame.example/b"sv),
                                                                                                                              }),
                                                                                              }),
                                                             entry(2, "https://b.example/"sv),
                                                         },
        { 0, 1, 2 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    EXPECT(!history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 1);

    history.forget_web_content_state();
    auto accepted = history.did_seed_web_content_from_ui_process({
                                                                     entry(0, "https://a.example/"sv, {
                                                                                                          nested_history("frame-1"sv, {
                                                                                                                                          entry(0, "https://frame.example/a"sv),
                                                                                                                                          entry(1, "https://frame.example/b"sv),
                                                                                                                                      }),
                                                                                                      }),
                                                                     entry(2, "https://b.example/"sv),
                                                                 },
        { 0, 1, 2 }, 0);

    EXPECT(accepted);
    EXPECT(!history.current_step_is_top_level_entry());
    expect_step_to_restore(history.current_step_to_restore_after_loading_top_level_entry(), 1);

    auto web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 0);
}

TEST_CASE(seed_ack_snapshot_rejects_mismatched_reconstructed_history)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1 }, 1);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.forget_web_content_state();
    auto accepted = history.did_seed_web_content_from_ui_process({
                                                                     entry(0, "https://a.example/"sv),
                                                                     entry(1, "https://c.example/"sv),
                                                                 },
        { 0, 1 }, 1);

    EXPECT(!accepted);
    EXPECT(!history.web_content_current_step().has_value());
    EXPECT(!history.web_content_uses_ui_step_coordinates());
    expect_current_entry(history, 1, "https://b.example/"sv);
}

TEST_CASE(seeded_web_content_restore_updates_current_step)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, {
                                                                                                  nested_history("frame-1"sv, {
                                                                                                                                  entry(0, "https://frame.example/a"sv),
                                                                                                                                  entry(1, "https://frame.example/b"sv),
                                                                                                                              }),
                                                                                              }),
                                                             entry(2, "https://b.example/"sv),
                                                         },
        { 0, 1, 2 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.forget_web_content_state();
    auto current_top_level_entry_index = history.current_top_level_entry_index();
    VERIFY(current_top_level_entry_index.has_value());
    history.did_seed_web_content_from_ui_process(*current_top_level_entry_index);

    auto web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 0);

    EXPECT(history.did_restore_web_content_to_current_step(1));
    web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 1);

    EXPECT(!history.did_restore_web_content_to_current_step(3));
    web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 1);
}

TEST_CASE(restored_web_content_step_must_match_current_ui_step)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://c.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.traverse_to(1);
    history.forget_web_content_state();
    auto current_top_level_entry_index = history.current_top_level_entry_index();
    VERIFY(current_top_level_entry_index.has_value());
    history.did_seed_web_content_from_ui_process(*current_top_level_entry_index);

    EXPECT(!history.did_restore_web_content_to_current_step(2));
    auto web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 1);
}

TEST_CASE(applied_web_content_traversal_updates_current_step)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://c.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
    expect_current_entry(history, 2, "https://c.example/"sv);

    EXPECT(history.did_apply_web_content_traversal_to_step(1));
    expect_current_entry(history, 1, "https://b.example/"sv);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());

    auto web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 1);

    auto target_a = history.traversal_target_for_delta(-1);
    VERIFY(target_a.has_value());
    EXPECT(history.web_content_can_traverse_to(*target_a));

    EXPECT(!history.did_apply_web_content_traversal_to_step(42));
    expect_current_entry(history, 1, "https://b.example/"sv);
    web_content_current_step = history.web_content_current_step();
    VERIFY(web_content_current_step.has_value());
    EXPECT_EQ(*web_content_current_step, 1);
}

TEST_CASE(applied_web_content_traversal_rejects_unknown_web_content_state)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://c.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.traverse_to(1);
    EXPECT(!history.did_apply_web_content_traversal_to_step(0));
    expect_current_entry(history, 1, "https://b.example/"sv);
    EXPECT(!history.web_content_current_step().has_value());
}

TEST_CASE(same_document_reseed_snapshot_accepts_reassigned_document_state_ids)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
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
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.forget_web_content_state();
    auto current_top_level_entry_index = history.current_top_level_entry_index();
    VERIFY(current_top_level_entry_index.has_value());
    history.did_seed_web_content_from_ui_process(*current_top_level_entry_index);

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://c.example/"sv),
                                                             entry(3, "https://nested.example/"sv, 8, "main"sv, {
                                                                                                                    nested_history("1"sv, {
                                                                                                                                              entry(3, "https://frame.example/a"sv, 6, ""sv),
                                                                                                                                              entry(4, "https://frame.example/b"sv, 7, ""sv),
                                                                                                                                          }),
                                                                                                                }),
                                                             entry(5, "https://nested.example/same-document"sv, 8, "main"sv, {
                                                                                                                                 nested_history("1"sv, {
                                                                                                                                                           entry(3, "https://frame.example/a"sv, 6, ""sv),
                                                                                                                                                           entry(4, "https://frame.example/b"sv, 7, ""sv),
                                                                                                                                                       }),
                                                                                                                             }),
                                                         },
        { 0, 1, 2, 3, 4, 5 }, 5);

    EXPECT_NE(update_result, WebView::TraversableSessionHistory::UpdateResult::InvalidSnapshot);
    EXPECT_EQ(history.current_used_step_index(), 5uz);
    expect_current_entry(history, 5, "https://nested.example/same-document"sv);
    EXPECT(history.web_content_can_traverse_to(*history.traversal_target_for_delta(-1)));
}

TEST_CASE(seed_ack_accepts_reconstructed_document_state_for_unknown_current_entry)
{
    WebView::TraversableSessionHistory history;
    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://a.example/replaced"sv, 1, "main"sv),
                                                             entry(2, "https://a.example/pushed"sv, 1, "main"sv),
                                                         },
        { 0, 1, 2 }, 0);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.forget_web_content_state();

    EXPECT(history.did_seed_web_content_from_ui_process({
                                                            entry(0, "https://a.example/"sv, 1, "main"sv),
                                                            entry(1, "https://a.example/replaced"sv, 2, "main"sv),
                                                            entry(2, "https://a.example/pushed"sv, 2, "main"sv),
                                                        },
        { 0, 1, 2 }, 0));
    EXPECT(history.web_content_history_matches_mirror());
    EXPECT_EQ(history.web_content_known_entries().size(), 3uz);
    EXPECT_EQ(history.web_content_current_step().value(), 0);
}

TEST_CASE(seed_ack_rejects_reconstructed_history_with_mismatched_state)
{
    WebView::TraversableSessionHistory history;
    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, 1, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Manual),
                                                             entry(1, "https://a.example/replaced"sv, 3, 4, "key-b"sv, "id-b"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                         },
        { 0, 1 }, 0);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.forget_web_content_state();

    EXPECT(!history.did_seed_web_content_from_ui_process({
                                                             entry(0, "https://a.example/"sv, 9, 2, "key-a"sv, "id-a"sv, Web::HTML::ScrollRestorationMode::Manual),
                                                             entry(1, "https://a.example/replaced"sv, 3, 4, "key-b"sv, "id-b"sv, Web::HTML::ScrollRestorationMode::Auto),
                                                         },
        { 0, 1 }, 0));
    EXPECT(!history.web_content_history_matches_mirror());
    EXPECT(history.web_content_known_entries().is_empty());
}

TEST_CASE(traversal_target_for_delta_outside_used_steps)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));

    EXPECT(!history.traversal_target_for_delta(-1).has_value());
    EXPECT(!history.traversal_target_for_delta(0).has_value());
    EXPECT(!history.traversal_target_for_delta(1).has_value());
}

TEST_CASE(complete_web_content_update_stores_nested_history_descriptors)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, {
                                                                                                  nested_history("frame-1"sv, {
                                                                                                                                  entry(0, "https://frame.example/a"sv),
                                                                                                                                  entry(1, "https://frame.example/b"sv),
                                                                                                                              }),
                                                                                              }),
                                                         },
        { 0, 1 }, 1);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);
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

    auto update_result = history.update_from_web_content({
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
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.navigate(parse_url("https://b.example/"sv));

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

TEST_CASE(partial_snapshot_missing_nested_history_descriptor_is_invalid)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.navigate(parse_url("https://c.example/"sv));
    history.traverse_to(1);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::InvalidSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/"sv);
    expect_entry(history, 2, 2, "https://c.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    expect_used_step(history, 2, 2);
}

TEST_CASE(invalid_snapshot_forgets_known_web_content_state)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv),
                                                                     entry(1, "https://b.example/"sv),
                                                                     entry(2, "https://c.example/"sv),
                                                                 },
        { 0, 1, 2 }, 2);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::InvalidSnapshot);

    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    expect_current_entry(history, 2, "https://c.example/"sv);

    auto web_content_known_used_steps = history.web_content_known_used_steps();
    EXPECT_EQ(web_content_known_used_steps.size(), 0uz);
    EXPECT(!history.web_content_current_step().has_value());

    auto target_b = history.traversal_target_for_delta(-1);
    VERIFY(target_b.has_value());
    EXPECT(!history.web_content_can_traverse_to(*target_b));
}

TEST_CASE(partial_snapshot_translates_nested_history_descriptors)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(4, "https://a.example/"sv),
                                                                     entry(5, "https://b.example/"sv),
                                                                     entry(6, "https://c.example/"sv),
                                                                 },
        { 4, 5, 6 }, 2);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.traverse_to(1);

    auto update_result = history.update_from_web_content({
                                                             entry(0, URL::about_blank()),
                                                             entry(1, "https://b.example/"sv, {
                                                                                                  nested_history("frame-1"sv, {
                                                                                                                                  entry(1, "https://frame.example/a"sv),
                                                                                                                                  entry(2, "https://frame.example/b"sv),
                                                                                                                              }),
                                                                                              }),
                                                         },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 4, "https://a.example/"sv);
    expect_current_entry(history, 5, "https://b.example/"sv);
    expect_used_step(history, 2, 6);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 2);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 5, "https://frame.example/a"sv);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 1, 6, "https://frame.example/b"sv);
    EXPECT_EQ(history.entry_for_step(6), nullptr);
}

TEST_CASE(partial_snapshot_translates_seeded_nested_history_initial_step)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv),
                                                                     entry(1, "https://b.example/"sv),
                                                                     entry(2, "https://c.example/"sv),
                                                                     entry(3, "https://nested.example/"sv, {
                                                                                                               nested_history("frame-1"sv, {
                                                                                                                                               entry(3, "https://frame.example/a"sv),
                                                                                                                                           }),
                                                                                                           }),
                                                                 },
        { 0, 1, 2, 3 }, 3);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                             entry(2, "https://c.example/"sv),
                                                             entry(3, "https://nested.example/"sv, {
                                                                                                       nested_history("frame-1"sv, {
                                                                                                                                       entry(0, "https://frame.example/a"sv),
                                                                                                                                       entry(4, "https://frame.example/b"sv),
                                                                                                                                   }),
                                                                                                   }),
                                                         },
        { 0, 1, 2, 3, 4 }, 4);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);
    EXPECT_EQ(history.size(), 4uz);
    EXPECT_EQ(history.used_step_count(), 5uz);
    EXPECT(!history.has_only_top_level_used_steps());
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/"sv);
    expect_entry(history, 2, 2, "https://c.example/"sv);
    expect_current_entry(history, 3, "https://nested.example/"sv);
    expect_used_step(history, 4, 4);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 2);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 3, "https://frame.example/a"sv);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 1, 4, "https://frame.example/b"sv);
    EXPECT_EQ(history.entry_for_step(4), nullptr);
}

TEST_CASE(partial_snapshot_missing_preserved_nested_history_descriptor_is_invalid)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv),
                                                                     entry(1, "https://b.example/"sv),
                                                                     entry(2, "https://c.example/"sv),
                                                                     entry(3, "https://nested.example/"sv, {
                                                                                                               nested_history("frame-1"sv, {
                                                                                                                                               entry(3, "https://frame.example/a"sv),
                                                                                                                                               entry(4, "https://frame.example/b"sv),
                                                                                                                                           }),
                                                                                                           }),
                                                                 },
        { 0, 1, 2, 3, 4 }, 4);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://nested.example/"sv, {
                                                                                                       nested_history("frame-1"sv, {
                                                                                                                                       entry(0, "https://frame.example/a"sv),
                                                                                                                                   }),
                                                                                                   }),
                                                         },
        { 0 }, 0);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::InvalidSnapshot);
    EXPECT_EQ(history.size(), 4uz);
    EXPECT_EQ(history.used_step_count(), 5uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/"sv);
    expect_entry(history, 2, 2, "https://c.example/"sv);
    expect_current_entry(history, 3, "https://nested.example/"sv);
    expect_used_step(history, 4, 4);

    auto* current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame-1"sv, 2);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 3, "https://frame.example/a"sv);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 1, 4, "https://frame.example/b"sv);
    EXPECT_EQ(history.entry_for_step(4), nullptr);
}

TEST_CASE(matching_snapshot_missing_nested_history_descriptor_is_invalid)
{
    WebView::TraversableSessionHistory history;
    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv));
    history.navigate(parse_url("https://c.example/"sv));
    history.traverse_to(1);

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::InvalidSnapshot);
    EXPECT_EQ(history.size(), 3uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_entry(history, 1, 1, "https://b.example/"sv);
    expect_entry(history, 2, 2, "https://c.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    expect_used_step(history, 2, 2);
}

TEST_CASE(history_log_entries_marks_current_entries_steps_and_reload_pending)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry_with_post_resource(1, "https://b.example/"sv),
                                                             entry_with_reload_pending(2, "https://c.example/"sv, 7, "main"sv, {
                                                                                                                                   nested_history("frame"sv, {
                                                                                                                                                                 entry(3, "https://frame.example/"sv),
                                                                                                                                                             }),
                                                                                                                               }),
                                                         },
        { 0, 1, 2, 3 }, 1);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    EXPECT_EQ(history_log_entries(history), ByteString { "entries=[0:0:https://a.example/, *1:1:https://b.example/ "
                                                         "document_state={id=0, resource=post}, 2:2:https://c.example/ "
                                                         "document_state={id=1, reload_pending=true, target_name=main} "
                                                         "nested={frame=[3:https://frame.example/]}] "
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

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv, 7, "main"sv, {
                                                                                                               nested_history("frame"sv, {
                                                                                                                                             entry(2, "https://frame.example/"sv),
                                                                                                                                         }),
                                                                                                           }),
                                                         },
        { 0, 1, 2 }, 2);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    EXPECT_EQ(history_log_entries(history), ByteString { "entries=[0:0:https://a.example/, *1:1:https://b.example/ "
                                                         "document_state={id=1, target_name=main} "
                                                         "nested={frame=[2:https://frame.example/]}] "
                                                         "used_steps=[0:0, 1:1, *2:2]"sv });
}

TEST_CASE(web_content_known_entries_describe_current_web_content_view)
{
    WebView::TraversableSessionHistory history;

    auto initial_update_result = history.update_from_web_content({
                                                                     entry(0, "https://a.example/"sv),
                                                                     entry(1, "https://b.example/"sv),
                                                                 },
        { 0, 1 }, 1);
    EXPECT_EQ(initial_update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.traverse_to(0);

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                         },
        { 0 }, 0);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::MergedPartialSnapshot);

    auto web_content_known_entries = history.web_content_known_entries();
    EXPECT_EQ(history_log_entries(history), ByteString { "entries=[*0:0:https://a.example/, 1:1:https://b.example/] used_steps=[*0:0, 1:1]"sv });
    EXPECT_EQ(WebView::history_log_entries(web_content_known_entries, 0), ByteString { "[*0:0:https://a.example/]"sv });
}

TEST_CASE(clear_current_entry_reload_pending)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry_with_reload_pending(1, "https://b.example/"sv, 7, "main"sv, {}),
                                                         },
        { 0, 1 }, 1);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

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

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv),
                                                             entry(1, "https://b.example/"sv),
                                                         },
        { 0, 1 }, 1);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(!current_entry->document_state.reload_pending);

    history.mark_current_entry_reload_pending();

    current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.reload_pending);
}

TEST_CASE(navigate_preserves_document_resource)
{
    WebView::TraversableSessionHistory history;

    history.navigate(parse_url("https://a.example/"sv));
    history.navigate(parse_url("https://b.example/"sv), Web::HTML::POSTResource {
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

    history.navigate(parse_url("https://a.example/"sv));
    history.replace_current_entry(parse_url("https://b.example/"sv), Web::HTML::POSTResource {
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

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, 7, "main"sv, {
                                                                                                               nested_history("frame-1"sv, {
                                                                                                                                               entry(0, "https://frame.example/a"sv),
                                                                                                                                               entry(1, "https://frame.example/b"sv),
                                                                                                                                           }),
                                                                                                           }),
                                                             entry(2, "https://c.example/"sv),
                                                         },
        { 0, 1, 2 }, 0);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.replace_current_entry(parse_url("https://b.example/"sv), Web::HTML::POSTResource {
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
    EXPECT_EQ(current_entry->document_state.id, 0u);
    EXPECT(current_entry->document_state.navigable_target_name.is_empty());
    EXPECT(current_entry->document_state.nested_histories.is_empty());
    expect_entry_resource(*current_entry, "post"sv);
}

TEST_CASE(replace_current_entry_at_nested_step_keeps_current_step_valid)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.update_from_web_content({
                                                             entry(0, "https://a.example/"sv, 7, "main"sv, {
                                                                                                               nested_history("frame-1"sv, {
                                                                                                                                               entry(0, "https://frame.example/a"sv),
                                                                                                                                               entry(1, "https://frame.example/b"sv),
                                                                                                                                           }),
                                                                                                           }),
                                                             entry(2, "https://c.example/"sv),
                                                         },
        { 0, 1, 2 }, 1);
    EXPECT_EQ(update_result, WebView::TraversableSessionHistory::UpdateResult::CompleteSnapshot);

    history.replace_current_entry(parse_url("https://b.example/"sv), Empty {});

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

    history.navigate(parse_url("https://a.example/"sv), Web::HTML::POSTResource {
                                                            .request_body = MUST(ByteBuffer::copy("name=ladybird"sv.bytes())),
                                                            .request_content_type = Web::HTML::POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded,
                                                        });
    history.replace_current_entry_url(parse_url("https://b.example/"sv));

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT_EQ(current_entry->url, parse_url("https://b.example/"sv));
    expect_entry_resource(*current_entry, "post"sv);
}
