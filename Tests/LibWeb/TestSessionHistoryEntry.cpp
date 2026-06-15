/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

TEST_CASE(post_load_seed_match_allows_ui_owned_nested_histories)
{
    Web::HTML::SessionHistoryEntryDescriptor live_descriptor;
    live_descriptor.step = 0;
    live_descriptor.url = parse_url("https://a.example/"sv);
    live_descriptor.document_state.id = 1;
    live_descriptor.document_state.ever_populated = true;
    live_descriptor.document_state.navigable_target_name = MUST(String::from_utf8("main"sv));

    auto seed_descriptor = live_descriptor;

    Web::HTML::SessionHistoryEntryDescriptor nested_entry;
    nested_entry.step = 1;
    nested_entry.url = parse_url("https://frame.example/"sv);
    seed_descriptor.document_state.nested_histories.append({
        .id = MUST(String::from_utf8("frame-1"sv)),
        .entries = { move(nested_entry) },
    });

    EXPECT(!Web::HTML::session_history_entry_descriptors_match_ignoring_document_state_id(live_descriptor, seed_descriptor));
    EXPECT(Web::HTML::session_history_entry_descriptors_match_ignoring_document_state_id(live_descriptor, seed_descriptor, Web::HTML::MatchNestedHistories::No));
}

TEST_CASE(descriptor_creation_preserves_nested_history_with_only_pending_entries)
{
    auto vm = JS::VM::create();

    auto top_level_document_state = Web::HTML::DocumentState::create();

    auto pending_child_entry = Web::HTML::SessionHistoryEntry::create();
    pending_child_entry->set_url(parse_url("https://frame.example/pending"sv));
    pending_child_entry->set_document_state(Web::HTML::DocumentState::create());

    top_level_document_state->nested_histories().append({
        .id = MUST(String::from_utf8("frame-1"sv)),
        .entries = { pending_child_entry },
    });

    auto top_level_entry = Web::HTML::SessionHistoryEntry::create();
    top_level_entry->set_step(0);
    top_level_entry->set_url(parse_url("https://a.example/"sv));
    top_level_entry->set_document_state(top_level_document_state);

    Web::HTML::SessionHistoryEntryDescriptorCreationState creation_state;
    auto descriptor = Web::HTML::create_session_history_entry_descriptor(top_level_entry, creation_state);

    EXPECT_EQ(descriptor.document_state.nested_histories.size(), 1u);
    EXPECT_EQ(descriptor.document_state.nested_histories[0].id, "frame-1"sv);
    EXPECT(descriptor.document_state.nested_histories[0].entries.is_empty());
}
