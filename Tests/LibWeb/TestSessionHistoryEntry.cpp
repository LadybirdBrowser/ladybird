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

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

TEST_CASE(concretizing_a_pending_entry_preserves_its_identity)
{
    auto vm = JS::VM::create();

    Web::HTML::CrossProcessIdAllocator cross_process_id_allocator { .namespace_id = 3 };
    auto entry = Web::HTML::SessionHistoryEntry::create();
    entry->set_url(parse_url("https://example.com/pending"sv));
    entry->set_document_state(Web::HTML::DocumentState::create(
        cross_process_id_allocator.allocate()));

    auto navigation_api_key = entry->navigation_api_key();
    auto navigation_api_id = entry->navigation_api_id();
    auto descriptor = Web::HTML::create_session_history_entry_descriptor(
        Web::HTML::create_pending_session_history_entry_descriptor(*entry), 7);

    EXPECT_EQ(descriptor.step, 7);
    EXPECT_EQ(descriptor.navigation_api_key, navigation_api_key);
    EXPECT_EQ(descriptor.navigation_api_id, navigation_api_id);
}
