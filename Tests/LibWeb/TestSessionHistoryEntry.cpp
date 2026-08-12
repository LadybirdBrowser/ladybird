/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>

static Web::HTML::CrossProcessId frame_1_id()
{
    return { 1, 1 };
}

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

TEST_CASE(descriptor_creation_preserves_nested_history_with_only_pending_entries)
{
    auto vm = JS::VM::create();

    Web::HTML::CrossProcessIdAllocator cross_process_id_allocator { .namespace_id = 3 };
    auto top_level_document_state = Web::HTML::DocumentState::create(cross_process_id_allocator.allocate());

    auto pending_child_entry = Web::HTML::SessionHistoryEntry::create();
    pending_child_entry->set_url(parse_url("https://frame.example/pending"sv));
    pending_child_entry->set_document_state(Web::HTML::DocumentState::create(cross_process_id_allocator.allocate()));

    top_level_document_state->nested_histories().append({
        .id = frame_1_id(),
        .entries = { pending_child_entry },
    });

    auto top_level_entry = Web::HTML::SessionHistoryEntry::create();
    top_level_entry->set_step(0);
    top_level_entry->set_url(parse_url("https://a.example/"sv));
    top_level_entry->set_document_state(top_level_document_state);

    auto descriptor = Web::HTML::create_session_history_entry_descriptor(top_level_entry);

    EXPECT_EQ(descriptor.document_state.nested_histories.size(), 1u);
    EXPECT_EQ(descriptor.document_state.nested_histories[0].id, frame_1_id());
    EXPECT(descriptor.document_state.nested_histories[0].entries.is_empty());
}
