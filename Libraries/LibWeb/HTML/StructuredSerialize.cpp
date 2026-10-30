/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Debug.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

// The identifier that a storage record holds for the primary interface of each serializable object.
static constexpr auto s_serializable_storage_registry = to_array<SerializableRegistryEntry>({
    { Bindings::InterfaceName::Blob, "Blob"sv },
    { Bindings::InterfaceName::CryptoKey, "CryptoKey"sv },
    { Bindings::InterfaceName::DOMException, "DOMException"sv },
    { Bindings::InterfaceName::DOMMatrix, "DOMMatrix"sv },
    { Bindings::InterfaceName::DOMMatrixReadOnly, "DOMMatrixReadOnly"sv },
    { Bindings::InterfaceName::DOMPoint, "DOMPoint"sv },
    { Bindings::InterfaceName::DOMPointReadOnly, "DOMPointReadOnly"sv },
    { Bindings::InterfaceName::DOMQuad, "DOMQuad"sv },
    { Bindings::InterfaceName::DOMRect, "DOMRect"sv },
    { Bindings::InterfaceName::DOMRectReadOnly, "DOMRectReadOnly"sv },
    { Bindings::InterfaceName::File, "File"sv },
    { Bindings::InterfaceName::FileList, "FileList"sv },
    { Bindings::InterfaceName::ImageBitmap, "ImageBitmap"sv },
    { Bindings::InterfaceName::ImageData, "ImageData"sv },
    { Bindings::InterfaceName::Module, "Module"sv },
    { Bindings::InterfaceName::QuotaExceededError, "QuotaExceededError"sv },
});

ReadonlySpan<SerializableRegistryEntry> serializable_storage_registry()
{
    return s_serializable_storage_registry.span();
}

// The same-process SharedArrayBuffer aliases of an IPCSerializationRecord. Only this file creates side tables, so a
// record's side table is one of these.
class SharedArrayBufferSideTable final : public SameProcessSerializationSideTable {
public:
    explicit SharedArrayBufferSideTable(Vector<GC::Root<JS::ArrayBuffer>> array_buffers)
        : m_array_buffers(move(array_buffers))
    {
    }

    Vector<GC::Root<JS::ArrayBuffer>> const& array_buffers() const { return m_array_buffers; }

private:
    Vector<GC::Root<JS::ArrayBuffer>> m_array_buffers;
};

IPCSerializationRecord create_ipc_serialization_record(ReadonlyBytes data, Vector<GC::Root<JS::ArrayBuffer>> shared_array_buffers)
{
    IPCSerializationRecord record;
    record.data.append(data.data(), data.size());
    if (!shared_array_buffers.is_empty())
        record.same_process_side_table = adopt_ref(*new SharedArrayBufferSideTable(move(shared_array_buffers)));
    return record;
}

ReadonlySpan<GC::Root<JS::ArrayBuffer>> same_process_shared_array_buffers(IPCSerializationRecord const& record)
{
    if (!record.same_process_side_table)
        return {};
    return static_cast<SharedArrayBufferSideTable const&>(*record.same_process_side_table).array_buffers();
}

WebIDL::Exception data_clone_error_from_serialization_error(JS::Realm& realm, AK::Error const& error)
{
    dbgln_if(STRUCTURED_SERIALIZE_DEBUG, "Rejecting structured serialized data: {}", error);
    return WebIDL::Exception { WebIDL::DataCloneError::create(realm, "Unable to process structured serialized data"_utf16) };
}

// The algorithms of structured serialization, which Libraries/LibWeb/Bindings/Rust/src/html/structured_serialize runs.
extern "C" {
JSCompletion web_rust_structured_serialize(JSVM*, JSValue, u8 allow_shared_array_buffers, Vector<Core::AnonymousBuffer>* shared_buffers, IPCSerializationRecord*);
JSCompletion web_rust_structured_serialize_for_storage(JSVM*, JSValue, StorageSerializationRecord*);
JSCompletion web_rust_structured_deserialize(JSVM*, void* target_realm, IPCSerializationRecord const*, Vector<Core::AnonymousBuffer> const* shared_buffers);
JSCompletion web_rust_structured_deserialize_for_storage(JSVM*, void* target_realm, ReadonlyBytes);
JSCompletion web_rust_structured_serialize_with_transfer(JSVM*, void* realm, JSValue, JSObject* const* transfer_list, size_t count, SerializedTransferRecord** out_record);
JSCompletion web_rust_structured_deserialize_with_transfer(JSVM*, void* target_realm, SerializedTransferRecord*, Vector<GC::Root<JS::Object>>* transferred_values);
JSCompletion web_rust_structured_deserialize_with_transfer_internal(JSVM*, void* target_realm, TransferDataDecoder*);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializewithtransfer
WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::Realm& realm, JS::Value value, ReadonlySpan<GC::Ref<JS::Object>> transfer_list)
{
    Vector<JSObject*> transfer_list_abi;
    transfer_list_abi.ensure_capacity(transfer_list.size());
    for (auto const& transferable : transfer_list)
        transfer_list_abi.unchecked_append(JS::HostABI::object_to_abi(transferable.ptr()));

    // NB: Rust creates the record, and hands it over with the result.
    SerializedTransferRecord* record = nullptr;
    TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_serialize_with_transfer(JS::HostABI::vm_to_abi(realm.vm()), &realm, JS::HostABI::value_to_abi(value), transfer_list_abi.data(), transfer_list_abi.size(), &record)));
    auto owned_record = adopt_own(*record);
    return move(*owned_record);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserializewithtransfer
WebIDL::ExceptionOr<DeserializedTransferRecord> structured_deserialize_with_transfer(SerializedTransferRecord& serialize_with_transfer_result, JS::Realm& target_realm)
{
    Vector<GC::Root<JS::Object>> transferred_values;
    auto deserialized = TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_deserialize_with_transfer(JS::HostABI::vm_to_abi(target_realm.vm()), &target_realm, &serialize_with_transfer_result, &transferred_values)));
    return DeserializedTransferRecord { .deserialized = deserialized, .transferred_values = move(transferred_values) };
}

// AD-HOC: This non-standard overload is meant to extract just one transferrable value from a serialized transfer record.
//         It's primarily useful for an object's transfer receiving steps to deserialize a nested value.
WebIDL::ExceptionOr<JS::Value> structured_deserialize_with_transfer_internal(TransferDataDecoder& decoder, JS::Realm& target_realm)
{
    return TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_deserialize_with_transfer_internal(JS::HostABI::vm_to_abi(target_realm.vm()), &target_realm, &decoder)));
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserialize
WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM& vm, JS::Value value)
{
    return structured_serialize(vm, value, AllowSharedArrayBuffers::CrossOriginIsolatedOnly);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserialize
WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM& vm, JS::Value value, AllowSharedArrayBuffers allow_shared_array_buffers)
{
    // 1. Return ? StructuredSerializeInternal(value, false).
    IPCSerializationRecord serialized;
    TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_serialize(JS::HostABI::vm_to_abi(vm), JS::HostABI::value_to_abi(value), to_underlying(allow_shared_array_buffers), nullptr, &serialized)));
    return serialized;
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserialize
// AD-HOC: For a record that reaches agents in other processes. The record itself is bytes-only, so a shared-memory-
//         backed SharedArrayBuffer is handed over beside it, in shared_buffers, by file descriptor — and every
//         receiver references the same [[ArrayBufferData]], rather than rebuilding the buffer from a byte copy.
WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM& vm, JS::Value value, Vector<Core::AnonymousBuffer>& shared_buffers)
{
    // 1. Return ? StructuredSerializeInternal(value, false).
    IPCSerializationRecord serialized;
    Vector<Core::AnonymousBuffer> serialized_shared_buffers;
    TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_serialize(JS::HostABI::vm_to_abi(vm), JS::HostABI::value_to_abi(value), to_underlying(AllowSharedArrayBuffers::CrossOriginIsolatedOnly), &serialized_shared_buffers, &serialized)));
    shared_buffers = move(serialized_shared_buffers);
    return serialized;
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeforstorage
WebIDL::ExceptionOr<StorageSerializationRecord> structured_serialize_for_storage(JS::VM& vm, JS::Value value)
{
    // 1. Return ? StructuredSerializeInternal(value, true).
    StorageSerializationRecord serialized;
    TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_serialize_for_storage(JS::HostABI::vm_to_abi(vm), JS::HostABI::value_to_abi(value), &serialized)));
    return serialized;
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeforstorage
// NB: StructuredSerializeInternal returns undefined and null as primitives at step 4, which needs no VM.
StorageSerializationRecord structured_serialize_undefined_or_null_for_storage(JS::Value value)
{
    VERIFY(value.is_undefined() || value.is_null());
    return value.is_undefined() ? storage_serialization_record_for_undefined() : storage_serialization_record_for_null();
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM& vm, IPCSerializationRecord const& serialized, JS::Realm& target_realm, Vector<Core::AnonymousBuffer> const* shared_buffers)
{
    TemporaryExecutionContext execution_context { target_realm };
    return TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_deserialize(JS::HostABI::vm_to_abi(vm), &target_realm, &serialized, shared_buffers)));
}

WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM& vm, StorageSerializationRecord const& serialized, JS::Realm& target_realm)
{
    TemporaryExecutionContext execution_context { target_realm };
    return TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_deserialize_for_storage(JS::HostABI::vm_to_abi(vm), &target_realm, serialized.data.bytes())));
}

TransferDataDecoder::TransferDataDecoder(TransferDataEncoder&& data_holder)
    : m_buffer(data_holder.take_buffer())
    , m_stream(m_buffer.data().span())
    , m_decoder(m_stream, m_attachments)
{
    for (auto& attachment : m_buffer.take_attachments())
        m_attachments.enqueue(move(attachment));
}

WebIDL::ExceptionOr<ByteBuffer> TransferDataDecoder::decode_buffer()
{
    auto buffer = m_decoder.decode<ByteBuffer>();

    if (buffer.is_error()) {
        VERIFY(buffer.error().code() == ENOMEM);
        return WebIDL::DataCloneError::create("Unable to allocate memory for transferred buffer"_utf16);
    }
    return buffer.release_value();
}

WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> TransferDataDecoder::decode_unsigned_big_integer()
{
    auto buffer = TRY(decode_buffer());
    return ::Crypto::UnsignedBigInteger::import_data(buffer);
}

}
