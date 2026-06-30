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

static Web::HTML::CrossProcessId navigable_id(StringView id)
{
    if (id == "frame-1"sv)
        return { 1, 1 };
    if (id == "frame"sv)
        return { 1, 2 };
    if (id == "child"sv)
        return { 1, 3 };
    if (id == "grandchild"sv)
        return { 1, 4 };
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

static Web::HTML::PendingSessionHistoryEntryDescriptor pending_entry(Web::HTML::SessionHistoryEntryDescriptor entry)
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
    };
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

static Web::HTML::PendingSessionHistoryEntryDescriptor pending_entry(StringView url, u64 document_state_id)
{
    auto descriptor = entry(0, url, document_state_id, ""sv);
    return {
        .url = move(descriptor.url),
        .document_state = move(descriptor.document_state),
        .classic_history_api_state = move(descriptor.classic_history_api_state),
        .navigation_api_state = move(descriptor.navigation_api_state),
        .navigation_api_key = move(descriptor.navigation_api_key),
        .navigation_api_id = move(descriptor.navigation_api_id),
        .scroll_restoration_mode = descriptor.scroll_restoration_mode,
        .scroll_position_data = move(descriptor.scroll_position_data),
    };
}

static Web::HTML::SameDocumentNavigationEntry same_document_entry(Web::HTML::SessionHistoryEntryDescriptor entry)
{
    return {
        .url = move(entry.url),
        .document_state_id = entry.document_state.id,
        .classic_history_api_state = move(entry.classic_history_api_state),
        .navigation_api_state = move(entry.navigation_api_state),
        .navigation_api_key = move(entry.navigation_api_key),
        .navigation_api_id = move(entry.navigation_api_id),
        .scroll_restoration_mode = entry.scroll_restoration_mode,
        .scroll_position_data = move(entry.scroll_position_data),
    };
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

TEST_CASE(targeted_entry_updates_find_nested_history_entries_by_navigation_api_key)
{
    WebView::TraversableSessionHistory history;

    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries {
        entry(0, "https://top.example/0"sv, 1, 1, "top-0"sv, "top-id-0"sv, Web::HTML::ScrollRestorationMode::Auto),
        entry(1, "https://top.example/1"sv, 2, 2, "top-1"sv, "top-id-1"sv, Web::HTML::ScrollRestorationMode::Auto),
    };

    auto entries0 = {
        entry(0, "https://child.example/0"sv, 3, 3, "child-0"sv, "child-id-0"sv, Web::HTML::ScrollRestorationMode::Auto),
        entry(2, "https://child.example/1"sv, 4, 4, "child-1"sv, "child-id-1"sv, Web::HTML::ScrollRestorationMode::Auto),
    };
    entries[0].document_state.nested_histories.append(nested_history("frame-1"sv, entries0));
    auto entries1 = {
        entry(0, "https://child.example/0"sv, 3, 3, "child-0"sv, "child-id-0"sv, Web::HTML::ScrollRestorationMode::Auto),
        entry(2, "https://child.example/1"sv, 4, 4, "child-1"sv, "child-id-1"sv, Web::HTML::ScrollRestorationMode::Auto),
    };
    entries[1].document_state.nested_histories.append(nested_history("frame-1"sv, entries1));

    auto update_result = history.initialize_for_testing(move(entries), { 0, 1, 2 }, 2);
    EXPECT_EQ(update_result, true);

    EXPECT(history.update_entry(navigable_id("frame-1"sv), Utf16String::from_utf8("child-1"sv), [&](auto& entry) {
        entry.navigation_api_state = state_record(9);
    }));
    EXPECT(history.update_entry(navigable_id("frame-1"sv), Utf16String::from_utf8("child-1"sv), [&](auto& entry) {
        entry.scroll_restoration_mode = Web::HTML::ScrollRestorationMode::Manual;
    }));

    auto expect_copied_nested_histories_were_updated = [](Vector<Web::HTML::SessionHistoryEntryDescriptor> const& copied_entries) {
        VERIFY(copied_entries.size() == 2);
        for (auto const& top_level_entry : copied_entries) {
            VERIFY(top_level_entry.document_state.nested_histories.size() == 1);
            auto const& nested_entries = top_level_entry.document_state.nested_histories[0].entries;
            VERIFY(nested_entries.size() == 2);
            expect_entry_state(nested_entries[0], 3, 3, "child-0"sv, "child-id-0"sv, Web::HTML::ScrollRestorationMode::Auto);
            expect_entry_state(nested_entries[1], 4, 9, "child-1"sv, "child-id-1"sv, Web::HTML::ScrollRestorationMode::Manual);
        }
    };

    expect_copied_nested_histories_were_updated(history.entries());
    expect_copied_nested_histories_were_updated(history.entries());
}

TEST_CASE(persisted_state_updates_require_entry_and_document_state_identity)
{
    WebView::TraversableSessionHistory history;

    auto current_entry = entry(0, "https://example.com/"sv, 10, "main"sv);
    current_entry.navigation_api_key = Utf16String::from_utf8("current"sv);
    auto update_result = history.initialize_for_testing({ move(current_entry) }, { 0 }, 0);
    EXPECT_EQ(update_result, true);

    auto persisted_state = Web::HTML::SessionHistoryEntryPersistedState {
        .document_state_id = test_document_state_id(11),
        .navigation_api_key = Utf16String::from_utf8("current"sv),
        .scroll_position_data = { .viewport_scroll_position = Web::CSSPixelPoint { 0, 100 } },
    };
    EXPECT(!history.update_entry_persisted_state({}, persisted_state));

    persisted_state.document_state_id = test_document_state_id(10);
    EXPECT(history.update_entry_persisted_state({}, persisted_state));

    auto entries = history.entries();
    VERIFY(entries.size() == 1);
    expect_entry_viewport_scroll_position(entries[0], { 0, 100 });
}

TEST_CASE(child_history_mutations_use_the_reported_parent_document_state)
{
    WebView::CanonicalTraversable traversable;
    traversable.set_id({ 9, 1 });

    WebView::TraversableSessionHistory history;
    auto earlier_parent_entry = entry(0, "https://parent.example/earlier"sv, 10, "main"sv);
    auto current_parent_entry = entry(1, "https://parent.example/current"sv, 11, "main"sv);
    current_parent_entry.document_state.nested_histories.append(nested_history("frame"sv, {
                                                                                              entry(1, "https://child.example/current"sv),
                                                                                          }));
    auto update_result = history.initialize_for_testing({ move(earlier_parent_entry), move(current_parent_entry) }, { 0, 1 }, 1);
    EXPECT_EQ(update_result, true);

    auto assigned_step = history.append_nested_history(traversable, test_document_state_id(10), navigable_id("frame"sv), pending_entry("https://child.example/earlier"sv, 12));
    VERIFY(assigned_step.has_value());
    EXPECT_EQ(*assigned_step, 0);
    EXPECT_EQ(history.current_step(), 1);

    auto* earlier_entry = history.entry_at(0);
    VERIFY(earlier_entry);
    expect_nested_history(*earlier_entry, 0, "frame"sv, 1);
    expect_nested_entry(earlier_entry->document_state.nested_histories[0], 0, 0, "https://child.example/earlier"sv);

    auto* current_entry = history.entry_at(1);
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame"sv, 1);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 1, "https://child.example/current"sv);

    EXPECT(history.remove_nested_history(traversable, test_document_state_id(10), navigable_id("frame"sv)));
    earlier_entry = history.entry_at(0);
    VERIFY(earlier_entry);
    EXPECT(earlier_entry->document_state.nested_histories.is_empty());
    current_entry = history.entry_at(1);
    VERIFY(current_entry);
    expect_nested_history(*current_entry, 0, "frame"sv, 1);
    expect_nested_entry(current_entry->document_state.nested_histories[0], 0, 1, "https://child.example/current"sv);
}

TEST_CASE(used_steps_include_nested_history_steps)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.initialize_for_testing({
                                                            entry(0, "https://a.example/"sv, {
                                                                                                 nested_history("frame-1"sv, {
                                                                                                                                 entry(1, "https://frame.example/a"sv),
                                                                                                                             }),
                                                                                             }),
                                                            entry(2, "https://b.example/"sv),
                                                        },
        { 0, 1, 2 }, 2);

    EXPECT_EQ(update_result, true);
    EXPECT_EQ(history.size(), 2uz);
    EXPECT_EQ(history.used_step_count(), 3uz);
    EXPECT(!history.has_only_top_level_used_steps());
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
    EXPECT(history.can_go_back());
    EXPECT(history.can_go_forward());
}

TEST_CASE(traversal_target_for_top_level_step)
{
    WebView::TraversableSessionHistory history;
    auto update_result = history.initialize_for_testing({
                                                            entry(0, "https://a.example/"sv),
                                                            entry(1, "https://b.example/"sv),
                                                            entry(2, "https://c.example/"sv),
                                                        },
        { 0, 1, 2 }, 2);
    EXPECT_EQ(update_result, true);
    history.traverse_to(1);

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

    auto update_result = history.initialize_for_testing({
                                                            entry(0, "https://a.example/"sv, {
                                                                                                 nested_history("frame-1"sv, {
                                                                                                                                 entry(0, "https://frame.example/a"sv),
                                                                                                                                 entry(1, "https://frame.example/b"sv),
                                                                                                                             }),
                                                                                             }),
                                                        },
        { 0, 1 }, 0);

    EXPECT_EQ(update_result, true);

    auto traversal_target = history.traversal_target_for_delta(1);
    VERIFY(traversal_target.has_value());
    EXPECT_EQ(traversal_target->target_step_index, 1uz);
    EXPECT_EQ(traversal_target->target_step, 1);
    VERIFY(traversal_target->target_top_level_entry);
    EXPECT_EQ(traversal_target->target_top_level_entry->url, parse_url("https://a.example/"sv));
    EXPECT(!traversal_target->target_step_is_top_level_entry);
    EXPECT(!traversal_target->changes_top_level_entry);

    history.traverse_to(traversal_target->target_step_index);
}

TEST_CASE(traversal_target_for_nested_step_in_earlier_top_level_entry)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.initialize_for_testing({
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

    EXPECT_EQ(update_result, true);

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

TEST_CASE(traversal_target_for_delta_outside_used_steps)
{
    WebView::TraversableSessionHistory history;
    auto update_result = history.initialize_for_testing(
        { entry(0, "https://a.example/"sv) }, { 0 }, 0);
    EXPECT_EQ(update_result, true);

    EXPECT(!history.traversal_target_for_delta(-1).has_value());
    EXPECT(!history.traversal_target_for_delta(0).has_value());
    EXPECT(!history.traversal_target_for_delta(1).has_value());
}

TEST_CASE(cross_document_push_clears_forward_history_at_finalization)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.initialize_for_testing({
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
    EXPECT_EQ(update_result, true);

    auto target_step = history.finalize_cross_document_navigation(
        {}, pending_entry(entry(2, "https://b.example/"sv)), {});
    EXPECT(target_step.has_value());
    EXPECT_EQ(*target_step, 2);
    EXPECT_EQ(history.current_step(), 1);
    history.set_current_session_history_step(2);

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

    auto update_result = history.initialize_for_testing({
                                                            entry(0, "https://a.example/"sv),
                                                            entry_with_post_resource(1, "https://b.example/"sv),
                                                            entry_with_reload_pending(2, "https://c.example/"sv, 7, "main"sv, {
                                                                                                                                  nested_history("frame"sv, {
                                                                                                                                                                entry(3, "https://frame.example/"sv),
                                                                                                                                                            }),
                                                                                                                              }),
                                                        },
        { 0, 1, 2, 3 }, 1);
    EXPECT_EQ(update_result, true);

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

    auto update_result = history.initialize_for_testing({
                                                            entry(0, "https://a.example/"sv),
                                                            entry(1, "https://b.example/"sv, 7, "main"sv, {
                                                                                                              nested_history("frame"sv, {
                                                                                                                                            entry(2, "https://frame.example/"sv),
                                                                                                                                        }),
                                                                                                          }),
                                                        },
        { 0, 1, 2 }, 2);
    EXPECT_EQ(update_result, true);

    EXPECT_EQ(history_log_entries(history), ByteString { "entries=[0:0:https://a.example/, *1:1:https://b.example/ "
                                                         "document_state={id=3:7, target_name=main} "
                                                         "nested={1:2=[2:https://frame.example/]}] "
                                                         "used_steps=[0:0, 1:1, *2:2]"sv });
}

TEST_CASE(mark_current_entry_reload_pending)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.initialize_for_testing({
                                                            entry(0, "https://a.example/"sv),
                                                            entry(1, "https://b.example/"sv),
                                                        },
        { 0, 1 }, 1);
    EXPECT_EQ(update_result, true);

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(!current_entry->document_state.reload_pending);

    history.mark_current_entry_reload_pending();

    current_entry = history.current_entry();
    VERIFY(current_entry);
    EXPECT(current_entry->document_state.reload_pending);
}

TEST_CASE(cross_document_push_preserves_document_resource)
{
    WebView::TraversableSessionHistory history;

    auto update_result = history.initialize_for_testing(
        { entry(0, "https://a.example/"sv) }, { 0 }, 0);
    EXPECT_EQ(update_result, true);

    auto target_step = history.finalize_cross_document_navigation(
        {}, pending_entry(entry_with_post_resource(1, "https://b.example/"sv)), {});
    EXPECT(target_step.has_value());
    EXPECT_EQ(*target_step, 1);
    history.set_current_session_history_step(1);

    auto current_entry = history.current_entry();
    VERIFY(current_entry);
    expect_entry_resource(*current_entry, "post"sv);
}

TEST_CASE(cross_document_replacement_preserves_forward_history)
{
    WebView::TraversableSessionHistory history;

    auto entry_to_replace = entry(0, "https://a.example/"sv, 7, "main"sv, {
                                                                              nested_history("frame-1"sv, {
                                                                                                              entry(0, "https://frame.example/a"sv),
                                                                                                              entry(1, "https://frame.example/b"sv),
                                                                                                          }),
                                                                          });
    entry_to_replace.navigation_api_key = Utf16String::from_utf8("current"sv);
    auto update_result = history.initialize_for_testing(
        { move(entry_to_replace), entry(2, "https://c.example/"sv) },
        { 0, 1, 2 }, 0);
    EXPECT_EQ(update_result, true);

    auto target_step = history.finalize_cross_document_navigation(
        {}, pending_entry(entry_with_post_resource(0, "https://b.example/"sv)),
        Utf16String::from_utf8("current"sv));
    EXPECT(target_step.has_value());
    EXPECT_EQ(*target_step, 0);

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

TEST_CASE(nested_cross_document_push_updates_copied_session_histories)
{
    WebView::TraversableSessionHistory history;

    auto child_entry = entry(0, "https://frame.example/first"sv, 20, ""sv);
    child_entry.navigation_api_key = Utf16String::from_utf8("child-current"sv);
    auto first_parent_entry = entry(0, "https://parent.example/first"sv, 10, "main"sv, {
                                                                                           nested_history("frame"sv, { move(child_entry) }),
                                                                                       });
    auto second_parent_entry = first_parent_entry;
    second_parent_entry.step = 1;
    second_parent_entry.url = parse_url("https://parent.example/pushed"sv);
    auto update_result = history.initialize_for_testing({ move(first_parent_entry), move(second_parent_entry) }, { 0, 1 }, 1);
    EXPECT_EQ(update_result, true);

    auto committed_entry = entry(0, "https://frame.example/second"sv, 21, ""sv);
    committed_entry.navigation_api_key = Utf16String::from_utf8("child-new"sv);
    auto target_step = history.finalize_cross_document_navigation(navigable_id("frame"sv), pending_entry(move(committed_entry)), {});
    VERIFY(target_step.has_value());
    EXPECT_EQ(*target_step, 2);

    auto entries = history.entries();
    EXPECT_EQ(entries.size(), 2uz);
    for (auto const& parent_entry : entries) {
        expect_nested_history(parent_entry, 0, "frame"sv, 2);
        auto const& nested_history = parent_entry.document_state.nested_histories.first();
        expect_nested_entry(nested_history, 0, 0, "https://frame.example/first"sv);
        expect_nested_entry(nested_history, 1, 2, "https://frame.example/second"sv);
    }
}

TEST_CASE(nested_cross_document_replacement_updates_copied_session_histories)
{
    WebView::TraversableSessionHistory history;

    auto child_entry = entry(0, "https://frame.example/first"sv, 20, ""sv);
    child_entry.navigation_api_key = Utf16String::from_utf8("child-current"sv);
    auto first_parent_entry = entry(0, "https://parent.example/first"sv, 10, "main"sv, {
                                                                                           nested_history("frame"sv, { move(child_entry) }),
                                                                                       });
    auto second_parent_entry = first_parent_entry;
    second_parent_entry.step = 1;
    second_parent_entry.url = parse_url("https://parent.example/pushed"sv);
    auto update_result = history.initialize_for_testing({ move(first_parent_entry), move(second_parent_entry) }, { 0, 1 }, 1);
    EXPECT_EQ(update_result, true);

    auto committed_entry = entry(0, "https://frame.example/replaced"sv, 21, ""sv);
    committed_entry.navigation_api_key = Utf16String::from_utf8("child-new"sv);
    auto target_step = history.finalize_cross_document_navigation(
        navigable_id("frame"sv), pending_entry(move(committed_entry)), Utf16String::from_utf8("child-current"sv));
    VERIFY(target_step.has_value());
    EXPECT_EQ(*target_step, 1);

    auto entries = history.entries();
    EXPECT_EQ(entries.size(), 2uz);
    for (auto const& parent_entry : entries) {
        expect_nested_history(parent_entry, 0, "frame"sv, 1);
        auto const& nested_history = parent_entry.document_state.nested_histories.first();
        expect_nested_entry(nested_history, 0, 0, "https://frame.example/replaced"sv);
        EXPECT_EQ(nested_history.entries.first().navigation_api_key, Utf16String::from_utf8("child-new"sv));
    }
}

TEST_CASE(same_document_push_clears_forward_history_at_queue_position)
{
    WebView::CanonicalTraversable traversable;
    WebView::TraversableSessionHistory history;

    auto current_entry = entry(0, "https://example.com/current"sv, 10, "main"sv);
    current_entry.navigation_api_key = Utf16String::from_utf8("current"sv);
    auto update_result = history.initialize_for_testing(
        { move(current_entry), entry(2, "https://example.com/forward"sv) }, { 0, 2 }, 0);
    EXPECT_EQ(update_result, true);

    auto target_entry = entry(0, "https://example.com/pushed"sv, 10, "main"sv);
    target_entry.navigation_api_key = Utf16String::from_utf8("pushed"sv);
    target_entry.navigation_api_id = Utf16String::from_utf8("pushed-id"sv);
    traversable.set_active_session_history_entry(target_entry);
    auto target_step = history.finalize_same_document_navigation(
        traversable, same_document_entry(move(target_entry)), {});

    VERIFY(target_step.has_value());
    EXPECT_EQ(*target_step, 1);
    EXPECT_EQ(history.current_step(), 0);
    EXPECT_EQ(history.size(), 2uz);
    expect_current_entry(history, 0, "https://example.com/current"sv);
    expect_entry(history, 1, 1, "https://example.com/pushed"sv);
}

TEST_CASE(same_document_replacement_preserves_forward_history)
{
    WebView::CanonicalTraversable traversable;
    WebView::TraversableSessionHistory history;

    auto current_entry = entry(0, "https://example.com/current"sv, 10, "main"sv);
    current_entry.navigation_api_key = Utf16String::from_utf8("current"sv);
    auto update_result = history.initialize_for_testing(
        { move(current_entry), entry(2, "https://example.com/forward"sv) }, { 0, 2 }, 0);
    EXPECT_EQ(update_result, true);

    auto target_entry = entry(0, "https://example.com/replaced"sv, 10, "main"sv);
    target_entry.navigation_api_key = Utf16String::from_utf8("current"sv);
    target_entry.navigation_api_id = Utf16String::from_utf8("replacement-id"sv);
    traversable.set_active_session_history_entry(target_entry);
    auto target_step = history.finalize_same_document_navigation(
        traversable,
        same_document_entry(move(target_entry)),
        Utf16String::from_utf8("current"sv));

    VERIFY(target_step.has_value());
    EXPECT_EQ(*target_step, 0);
    EXPECT_EQ(history.current_step(), 0);
    EXPECT_EQ(history.size(), 2uz);
    expect_current_entry(history, 0, "https://example.com/replaced"sv);
    expect_entry(history, 1, 2, "https://example.com/forward"sv);
}

TEST_CASE(first_cross_document_finalization_initializes_history)
{
    WebView::TraversableSessionHistory history;

    auto target_step = history.finalize_cross_document_navigation(
        {}, pending_entry(entry(0, "https://a.example/"sv)), {});
    EXPECT(target_step.has_value());
    EXPECT_EQ(*target_step, 0);
    EXPECT_EQ(history.size(), 1uz);
    EXPECT_EQ(history.used_step_count(), 1uz);
    expect_current_entry(history, 0, "https://a.example/"sv);
}

TEST_CASE(nested_finalization_replaces_initial_entry_after_its_key_changes)
{
    WebView::TraversableSessionHistory history;
    auto initial_entry = entry(0, "about:blank"sv);
    initial_entry.navigation_api_key = Utf16String::from_utf8("canonical-initial"sv);
    auto update_result = history.initialize_for_testing({ entry(0, "https://top.example/"sv, {
                                                                                                 nested_history("frame"sv, { move(initial_entry) }),
                                                                                             }) },
        { 0 }, 0);
    EXPECT_EQ(update_result, true);

    auto committed_entry = entry(0, "https://frame.example/"sv, 2, ""sv);
    committed_entry.navigation_api_key = Utf16String::from_utf8("live-initial"sv);
    auto target_step = history.finalize_cross_document_navigation(navigable_id("frame"sv), pending_entry(move(committed_entry)), Utf16String::from_utf8("live-initial"sv));
    EXPECT(target_step.has_value());
    EXPECT_EQ(*target_step, 0);

    auto entries = history.entries();
    auto const& nested_history = entries.first().document_state.nested_histories.first();
    auto const& nested_entries = nested_history.entries;
    EXPECT_EQ(nested_entries.size(), 1uz);
    expect_nested_entry(nested_history, 0, 0, "https://frame.example/"sv);
    EXPECT_EQ(nested_entries.first().navigation_api_key, Utf16String::from_utf8("live-initial"sv));
}

TEST_CASE(nested_finalization_rejects_wrong_key_for_populated_entry)
{
    WebView::TraversableSessionHistory history;
    auto populated_entry = entry(0, "https://frame.example/first"sv, 2, ""sv);
    populated_entry.navigation_api_key = Utf16String::from_utf8("canonical"sv);
    auto update_result = history.initialize_for_testing({ entry(0, "https://top.example/"sv, {
                                                                                                 nested_history("frame"sv, { move(populated_entry) }),
                                                                                             }) },
        { 0 }, 0);
    EXPECT_EQ(update_result, true);

    auto committed_entry = entry(0, "https://frame.example/second"sv, 3, ""sv);
    committed_entry.navigation_api_key = Utf16String::from_utf8("stale"sv);
    EXPECT(!history.finalize_cross_document_navigation(navigable_id("frame"sv), pending_entry(move(committed_entry)), Utf16String::from_utf8("stale"sv)).has_value());

    auto entries = history.entries();
    auto const& nested_history = entries.first().document_state.nested_histories.first();
    EXPECT_EQ(nested_history.entries.size(), 1uz);
    expect_nested_entry(nested_history, 0, 0, "https://frame.example/first"sv);
}

static constexpr auto restore_error_structurally_invalid = "Session history snapshot is structurally invalid"sv;
static constexpr auto restore_error_step_not_reachable = "Session history snapshot has a used step that is not reachable"sv;
static constexpr auto restore_error_no_document_state = "Session history snapshot's current entry has no document state"sv;

TEST_CASE(restore_from_ui_snapshot_installs_a_valid_snapshot)
{
    WebView::TraversableSessionHistory history;

    auto result = history.restore_from_ui_snapshot({
                                                       entry(0, "https://a.example/"sv, 1, "main"sv),
                                                       entry(1, "https://b.example/"sv, 2, "main"sv),
                                                   },
        { 0, 1 }, 1, allocate_test_ui_process_document_state_id);

    EXPECT(!result.is_error());
    EXPECT_EQ(history.size(), 2uz);
    EXPECT(history.can_go_back());
    EXPECT(!history.can_go_forward());
    expect_entry(history, 0, 0, "https://a.example/"sv);
    expect_current_entry(history, 1, "https://b.example/"sv);
    EXPECT_EQ(history.entry_at(0)->document_state.id.namespace_id, 4u);
    EXPECT(history.entry_at(0)->document_state.id != test_document_state_id(1));
    EXPECT_EQ(history.entry_at(1)->document_state.id.namespace_id, 4u);
    EXPECT(history.entry_at(1)->document_state.id != test_document_state_id(2));
}

TEST_CASE(restore_from_ui_snapshot_installs_a_valid_nested_snapshot)
{
    WebView::TraversableSessionHistory history;

    // Nested step 1 lives under the step-0 entry, which is the correct top-level entry for step 1.
    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries;
    entries.append(entry_with_reload_pending(0, "https://a.example/"sv, 1, "main"sv,
        { nested_history("frame"sv, { entry(1, "https://nested.example/"sv) }) }));
    entries.append(entry(2, "https://b.example/"sv, 2, "main"sv));

    auto result = history.restore_from_ui_snapshot(move(entries), { 0, 1, 2 }, 2, allocate_test_ui_process_document_state_id);

    EXPECT(!result.is_error());
    EXPECT_EQ(history.size(), 2uz);
    expect_current_entry(history, 2, "https://b.example/"sv);
    auto const& restored_nested_history = history.entry_at(0)->document_state.nested_histories[0];
    EXPECT_EQ(restored_nested_history.id.namespace_id, 4u);
    EXPECT(restored_nested_history.id != navigable_id("frame"sv));
}

TEST_CASE(restore_from_ui_snapshot_preserves_shared_document_state_identity)
{
    WebView::TraversableSessionHistory history;
    auto original_id = test_document_state_id(1);

    auto result = history.restore_from_ui_snapshot({
                                                       entry(0, "https://a.example/"sv, 1, "main"sv),
                                                       entry(1, "https://a.example/next"sv, 1, "main"sv),
                                                   },
        { 0, 1 }, 1, allocate_test_ui_process_document_state_id);

    EXPECT(!result.is_error());
    auto first_id = history.entry_at(0)->document_state.id;
    auto second_id = history.entry_at(1)->document_state.id;
    EXPECT_EQ(first_id.namespace_id, 4u);
    EXPECT(first_id != original_id);
    EXPECT_EQ(first_id, second_id);
}

TEST_CASE(restore_from_ui_snapshot_rejects_structurally_invalid_snapshots)
{
    auto restore = [](Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index) {
        WebView::TraversableSessionHistory history;
        auto result = history.restore_from_ui_snapshot(move(entries), move(used_steps), current_used_step_index, allocate_test_ui_process_document_state_id);
        EXPECT(result.is_error());
        EXPECT_EQ(result.error().string_literal(), restore_error_structurally_invalid);
        EXPECT_EQ(history.size(), 0uz);
    };

    restore({}, { 0 }, 0);
    restore({ entry(0, "https://a.example/"sv, 1, "main"sv) }, {}, 0);
    restore({ entry(0, "https://a.example/"sv, 1, "main"sv) }, { 0 }, 5);
    restore({ entry(0, "https://a.example/"sv, 1, "main"sv), entry(1, "https://b.example/"sv, 2, "main"sv) }, { 0, 5 }, 1);
}

TEST_CASE(restore_from_ui_snapshot_rejects_excessive_nesting_before_restoration)
{
    auto nested_entry = entry(0, "https://nested.example/"sv, 1, "frame"sv);
    for (size_t depth = 0; depth <= WebView::MAX_NESTED_HISTORY_DEPTH; ++depth) {
        Web::HTML::SessionHistoryNestedHistoryDescriptor nested_history {
            .id = { 1, depth + 1 },
            .entries = { move(nested_entry) },
        };
        nested_entry = entry_with_reload_pending(0, "https://nested.example/"sv, depth + 2, "frame"sv, { move(nested_history) });
    }

    WebView::TraversableSessionHistory history;
    auto result = history.restore_from_ui_snapshot({ move(nested_entry) }, { 0 }, 0, allocate_test_ui_process_document_state_id);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), restore_error_structurally_invalid);
    EXPECT_EQ(history.size(), 0uz);
}

TEST_CASE(restore_from_ui_snapshot_rejects_current_entry_without_document_state)
{
    WebView::TraversableSessionHistory history;

    auto result = history.restore_from_ui_snapshot({
                                                       entry(0, "https://a.example/"sv, 1, "main"sv),
                                                       entry(1, "https://b.example/"sv),
                                                   },
        { 0, 1 }, 1, allocate_test_ui_process_document_state_id);

    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), restore_error_no_document_state);
    EXPECT_EQ(history.size(), 0uz);
}

TEST_CASE(restore_from_ui_snapshot_rejects_used_step_without_top_level_entry)
{
    WebView::TraversableSessionHistory history;

    // The only top-level entry is at step 5, but used step 1 (a nested step) has no top-level entry,
    // so a later back traversal to step 1 would crash. The step set is internally consistent.
    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries;
    entries.append(entry_with_reload_pending(5, "https://a.example/"sv, 1, "main"sv,
        { nested_history("frame"sv, { entry(1, "https://nested.example/"sv) }) }));

    auto result = history.restore_from_ui_snapshot(move(entries), { 1, 5 }, 1, allocate_test_ui_process_document_state_id);

    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), restore_error_step_not_reachable);
    EXPECT_EQ(history.size(), 0uz);
}

TEST_CASE(restore_from_ui_snapshot_rejects_used_step_under_shadowed_top_level_entry)
{
    WebView::TraversableSessionHistory history;

    // Nested step 4 lives under the step-0 entry, but the step-2 top-level entry shadows it, so
    // top_level_entry_for_step(4) resolves to page 2, which does not contain step 4.
    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries;
    entries.append(entry_with_reload_pending(0, "https://a.example/"sv, 1, "main"sv,
        { nested_history("frame"sv, { entry(4, "https://nested.example/"sv) }) }));
    entries.append(entry(2, "https://b.example/"sv, 2, "main"sv));

    auto result = history.restore_from_ui_snapshot(move(entries), { 0, 2, 4 }, 1, allocate_test_ui_process_document_state_id);

    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), restore_error_step_not_reachable);
    EXPECT_EQ(history.size(), 0uz);
}

TEST_CASE(restore_from_ui_snapshot_rejects_step_under_inactive_nested_sibling)
{
    WebView::TraversableSessionHistory history;

    // The child history's active entry for step 4 is the sibling at step 2, but the step-4 grandchild
    // lives under the inactive child at step 0, so step 4 is unreachable on the active path.
    auto child_entry_0 = entry_with_reload_pending(0, "https://child0.example/"sv, 2, "frame"sv,
        { nested_history("grandchild"sv, { entry(4, "https://grandchild.example/"sv) }) });
    auto child_entry_2 = entry(2, "https://child2.example/"sv, 3, "frame"sv);

    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries;
    entries.append(entry_with_reload_pending(0, "https://a.example/"sv, 1, "main"sv,
        { nested_history("child"sv, { move(child_entry_0), move(child_entry_2) }) }));

    auto result = history.restore_from_ui_snapshot(move(entries), { 0, 2, 4 }, 1, allocate_test_ui_process_document_state_id);

    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), restore_error_step_not_reachable);
    EXPECT_EQ(history.size(), 0uz);
}

TEST_CASE(restore_from_ui_snapshot_leaves_history_untouched_on_failure)
{
    WebView::TraversableSessionHistory history;
    MUST(history.restore_from_ui_snapshot({ entry(0, "https://a.example/"sv, 1, "main"sv) }, { 0 }, 0, allocate_test_ui_process_document_state_id));
    EXPECT_EQ(history.size(), 1uz);

    auto result = history.restore_from_ui_snapshot({}, { 0 }, 0, allocate_test_ui_process_document_state_id);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), restore_error_structurally_invalid);
    EXPECT_EQ(history.size(), 1uz);
    expect_current_entry(history, 0, "https://a.example/"sv);
}
