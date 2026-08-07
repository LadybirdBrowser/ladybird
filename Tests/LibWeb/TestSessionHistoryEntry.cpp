/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/TextDirective.h>

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
    auto directive_state = Web::HTML::DirectiveState::create(cross_process_id_allocator.allocate(), "text=pending"_string);
    auto entry = Web::HTML::SessionHistoryEntry::create(directive_state);
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
    EXPECT_EQ(descriptor.directive_state_id, directive_state->cross_process_id());
    EXPECT_EQ(descriptor.directive_state_value, "text=pending"sv);
}

TEST_CASE(descriptor_round_trip_preserves_shared_directive_state_identity)
{
    auto vm = JS::VM::create();

    Web::HTML::CrossProcessIdAllocator cross_process_id_allocator { .namespace_id = 3 };
    auto document_state = Web::HTML::DocumentState::create(cross_process_id_allocator.allocate());
    auto directive_state = Web::HTML::DirectiveState::create(cross_process_id_allocator.allocate(), "text=target"_string);

    auto first_entry = Web::HTML::SessionHistoryEntry::create(directive_state);
    first_entry->set_step(0);
    first_entry->set_url(parse_url("https://a.example/first"sv));
    first_entry->set_document_state(document_state);

    auto second_entry = Web::HTML::SessionHistoryEntry::create(directive_state);
    second_entry->set_step(1);
    second_entry->set_url(parse_url("https://a.example/second"sv));
    second_entry->set_document_state(document_state);

    Vector<Web::HTML::SessionHistoryEntryDescriptor> descriptors;
    descriptors.append(Web::HTML::create_session_history_entry_descriptor(*first_entry));
    descriptors.append(Web::HTML::create_session_history_entry_descriptor(*second_entry));

    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(descriptors));

    FixedMemoryStream stream { buffer.data().span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    auto decoded_descriptors = MUST(decoder.decode<Vector<Web::HTML::SessionHistoryEntryDescriptor>>());

    EXPECT_EQ(decoded_descriptors[0].directive_state_id, directive_state->cross_process_id());
    EXPECT_EQ(decoded_descriptors[1].directive_state_id, directive_state->cross_process_id());
    EXPECT_EQ(decoded_descriptors[0].directive_state_value, "text=target"sv);
    EXPECT_EQ(decoded_descriptors[1].directive_state_value, "text=target"sv);
}

TEST_CASE(restored_non_ascii_directive_state_is_ignored)
{
    EXPECT(Web::HTML::parse_the_fragment_directive("text=é"sv).is_empty());
}
