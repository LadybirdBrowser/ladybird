/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibGC/Root.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/StructuredSerialize.h>

// NOTE: These tests use the persistent main-thread VM, so they must not share a
// process with tests that create their own JS::VM.

static JS::Realm& source_realm()
{
    static auto& realm = *new GC::Root<JS::Realm>;
    if (!realm)
        realm = Web::Bindings::create_a_simple_javascript_realm();
    return *realm;
}

static JS::Realm& target_realm()
{
    static auto& realm = *new GC::Root<JS::Realm>;
    if (!realm)
        realm = Web::Bindings::create_a_simple_javascript_realm();
    return *realm;
}

static GC::Ref<JS::ArrayBuffer> make_shared_array_buffer(JS::Realm& realm, ReadonlyBytes contents)
{
    auto buffer = MUST(JS::ArrayBuffer::create(realm, contents.size(), JS::DataBlock::Shared::Yes));
    buffer->overwrite(0, contents.data(), contents.size());
    return buffer;
}

static Web::HTML::IPCSerializationRecord serialize_same_agent(JS::Value value)
{
    auto& vm = Web::Bindings::main_thread_vm();
    return MUST(Web::HTML::structured_serialize(vm, value, Web::HTML::AllowSharedArrayBuffers::SameAgentAlways));
}

static JS::Value deserialize(Web::HTML::IPCSerializationRecord const& record, JS::Realm& realm)
{
    auto& vm = Web::Bindings::main_thread_vm();
    return MUST(Web::HTML::structured_deserialize(vm, record, realm));
}

static JS::ArrayBuffer& as_array_buffer(JS::Value value)
{
    VERIFY(value.is_object());
    VERIFY(is<JS::ArrayBuffer>(value.as_object()));
    return static_cast<JS::ArrayBuffer&>(value.as_object());
}

static ByteBuffer contents_of(JS::ArrayBuffer const& buffer)
{
    return MUST(buffer.copy_to_byte_buffer());
}

static Web::HTML::IPCSerializationRecord ipc_round_trip(Web::HTML::IPCSerializationRecord const& record)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(record));

    FixedMemoryStream stream { buffer.data().span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return MUST(decoder.decode<Web::HTML::IPCSerializationRecord>());
}

TEST_CASE(same_process_clone_aliases_the_backing_store)
{
    auto source = make_shared_array_buffer(source_realm(), "shared array buffer"sv.bytes());

    auto record = serialize_same_agent(JS::Value { source.ptr() });
    EXPECT_EQ(record.shared_array_buffers.size(), 1uz);
    EXPECT_EQ(record.shared_array_buffers[0].ptr(), source.ptr());

    auto& clone = as_array_buffer(deserialize(record, target_realm()));
    EXPECT(clone.is_shared_array_buffer());
    EXPECT_EQ(clone.byte_length(), source->byte_length());

    EXPECT(&clone != source.ptr());
    EXPECT_EQ(clone.data_at(0), source->data_at(0));
    EXPECT(clone.shares_storage_with(*source));

    auto const& external = clone.data_block().byte_buffer.get<JS::DataBlock::ExternalPrimitiveStorage>();
    EXPECT(external.owner.ptr() == static_cast<GC::Cell*>(source.ptr()));

    source->overwrite(0, "S", 1);
    auto clone_contents = contents_of(clone);
    EXPECT_EQ(clone_contents[0], static_cast<u8>('S'));
    clone.overwrite(1, "C", 1);
    auto source_contents = contents_of(*source);
    EXPECT_EQ(source_contents[1], static_cast<u8>('C'));
}

TEST_CASE(alias_chains_are_flattened)
{
    auto source = make_shared_array_buffer(source_realm(), "flatten me"sv.bytes());

    auto first_record = serialize_same_agent(JS::Value { source.ptr() });
    auto& first_clone = as_array_buffer(deserialize(first_record, target_realm()));

    // Cloning the clone back into the source realm must reference the original
    // storage-owning buffer, not chain through the intermediate clone.
    auto second_record = serialize_same_agent(JS::Value { &first_clone });
    auto& second_clone = as_array_buffer(deserialize(second_record, source_realm()));

    EXPECT(second_clone.shares_storage_with(*source));
    auto const& external = second_clone.data_block().byte_buffer.get<JS::DataBlock::ExternalPrimitiveStorage>();
    EXPECT(external.owner.ptr() == static_cast<GC::Cell*>(source.ptr()));
}

TEST_CASE(shared_array_buffer_reached_through_a_view_is_aliased)
{
    auto source = make_shared_array_buffer(source_realm(), "viewed buffer"sv.bytes());
    auto view = JS::Uint8Array::create(source_realm(), source->byte_length(), *source);

    auto record = serialize_same_agent(JS::Value { view.ptr() });
    auto value = deserialize(record, target_realm());

    VERIFY(value.is_object());
    VERIFY(is<JS::Uint8Array>(value.as_object()));
    auto& cloned_view = static_cast<JS::Uint8Array&>(value.as_object());
    auto& cloned_buffer = *cloned_view.viewed_array_buffer();

    EXPECT(cloned_buffer.is_shared_array_buffer());
    EXPECT(cloned_buffer.shares_storage_with(*source));
}

TEST_CASE(ipc_round_trip_degrades_to_a_copy)
{
    auto source = make_shared_array_buffer(source_realm(), "copied across processes"sv.bytes());

    auto record = ipc_round_trip(serialize_same_agent(JS::Value { source.ptr() }));
    EXPECT(record.shared_array_buffers.is_empty());

    auto& clone = as_array_buffer(deserialize(record, target_realm()));
    EXPECT(clone.is_shared_array_buffer());
    EXPECT_EQ(clone.byte_length(), source->byte_length());

    EXPECT_EQ(contents_of(clone), contents_of(*source));
    EXPECT(!clone.shares_storage_with(*source));

    source->overwrite(0, "X", 1);
    auto clone_contents = contents_of(clone);
    EXPECT_NE(clone_contents[0], static_cast<u8>('X'));
}

TEST_CASE(growable_shared_array_buffer_aliases_the_backing_store)
{
    auto source = make_shared_array_buffer(source_realm(), "growable"sv.bytes());
    source->set_max_byte_length(128);

    auto record = serialize_same_agent(JS::Value { source.ptr() });
    EXPECT_EQ(record.shared_array_buffers.size(), 1uz);
    EXPECT_EQ(record.shared_array_buffers[0].ptr(), source.ptr());

    auto& clone = as_array_buffer(deserialize(record, target_realm()));
    EXPECT(clone.is_shared_array_buffer());
    EXPECT(!clone.is_fixed_length());
    EXPECT_EQ(clone.max_byte_length(), 128uz);
    EXPECT_EQ(contents_of(clone), contents_of(*source));
    EXPECT(clone.shares_storage_with(*source));
}

TEST_CASE(zero_length_shared_array_buffer_round_trips)
{
    auto source = make_shared_array_buffer(source_realm(), {});

    auto& clone = as_array_buffer(deserialize(serialize_same_agent(JS::Value { source.ptr() }), target_realm()));
    EXPECT(clone.is_shared_array_buffer());
    EXPECT_EQ(clone.byte_length(), 0uz);
}

TEST_CASE(cross_origin_isolation_gate_still_applies_by_default)
{
    auto& vm = Web::Bindings::main_thread_vm();
    auto source = make_shared_array_buffer(source_realm(), "gated"sv.bytes());

    // The default serialization mode keeps the cross-origin isolated capability check, and
    // the capability is never granted today, so web-visible SharedArrayBuffer clones throw.
    auto result = Web::HTML::structured_serialize(vm, JS::Value { source.ptr() });
    EXPECT(result.is_error());
}

TEST_CASE(storage_serialization_rejects_shared_array_buffers_even_when_same_agent)
{
    auto& vm = Web::Bindings::main_thread_vm();
    auto source = make_shared_array_buffer(source_realm(), "not for storage"sv.bytes());

    auto writer = Web::HTML::StructuredSerializeWriter::create_storage();
    Web::HTML::SerializationMemory memory;
    auto result = Web::HTML::structured_serialize_internal(vm, writer, JS::Value { source.ptr() }, true, memory, Web::HTML::AllowSharedArrayBuffers::SameAgentAlways);
    EXPECT(result.is_error());
}

TEST_CASE(appending_an_ipc_record_rejects_a_shared_array_buffer_side_table)
{
    auto record = serialize_same_agent(JS::Value { make_shared_array_buffer(source_realm(), "side table"sv.bytes()).ptr() });
    EXPECT(!record.shared_array_buffers.is_empty());

    auto writer = Web::HTML::StructuredSerializeWriter::create_ipc();
    EXPECT_DEATH("Appending an IPC record with a SharedArrayBuffer side table", [&] {
        writer.append(move(record));
    }());
}
