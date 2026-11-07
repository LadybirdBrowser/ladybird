/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

// The same-agent SharedArrayBuffer aliases of an IPCSerializationRecord. Only this file creates side tables, so a
// record's side table is one of these.
class SharedArrayBufferSideTable final : public SameAgentSerializationSideTable {
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
        record.same_agent_side_table = adopt_ref(*new SharedArrayBufferSideTable(move(shared_array_buffers)));
    return record;
}

ReadonlySpan<GC::Root<JS::ArrayBuffer>> same_agent_shared_array_buffers(IPCSerializationRecord const& record)
{
    if (!record.same_agent_side_table)
        return {};
    return static_cast<SharedArrayBufferSideTable const&>(*record.same_agent_side_table).array_buffers();
}

WebIDL::Exception data_clone_error_from_serialization_error(JS::Realm& realm, AK::Error const& error)
{
    dbgln_if(STRUCTURED_SERIALIZE_DEBUG, "Rejecting structured serialized data: {}", error);
    return WebIDL::Exception { WebIDL::DataCloneError::create(realm, "Unable to process structured serialized data"_utf16) };
}

// The algorithms of structured serialization, which Libraries/LibWeb/Bindings/Rust/src/html/structured_serialize runs.
extern "C" {
JSCompletion web_rust_structured_serialize(JSVM*, JSValue, u8 allow_shared_array_buffers, IPCSerializationRecord*);
JSCompletion web_rust_structured_serialize_for_storage(JSVM*, JSValue, StorageSerializationRecord*);
JSCompletion web_rust_structured_deserialize(JSVM*, void* target_realm, IPCSerializationRecord const*);
JSCompletion web_rust_structured_deserialize_for_storage(JSVM*, void* target_realm, ReadonlyBytes);
JSCompletion web_rust_structured_serialize_with_transfer(JSVM*, void* realm, JSValue, JSObject* const* transfer_list, size_t count, SerializedTransferRecord** out_record);
JSCompletion web_rust_structured_deserialize_with_transfer(JSVM*, void* target_realm, SerializedTransferRecord*, Vector<GC::Root<JS::Object>>* transferred_values);
void web_rust_serializable_storage_registry(void* context, void (*append)(void*, u16 interface_name, ReadonlyBytes identifier));
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
    TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_serialize(JS::HostABI::vm_to_abi(vm), JS::HostABI::value_to_abi(value), to_underlying(allow_shared_array_buffers), &serialized)));
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
WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM& vm, IPCSerializationRecord const& serialized, JS::Realm& target_realm)
{
    TemporaryExecutionContext execution_context { target_realm };
    return TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_deserialize(JS::HostABI::vm_to_abi(vm), &target_realm, &serialized)));
}

WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM& vm, StorageSerializationRecord const& serialized, JS::Realm& target_realm)
{
    TemporaryExecutionContext execution_context { target_realm };
    return TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_deserialize_for_storage(JS::HostABI::vm_to_abi(vm), &target_realm, serialized.data.bytes())));
}

Vector<SerializableRegistryEntry> serializable_storage_registry()
{
    Vector<SerializableRegistryEntry> entries;
    web_rust_serializable_storage_registry(&entries, [](void* context, u16 interface_name, ReadonlyBytes identifier) {
        static_cast<Vector<SerializableRegistryEntry>*>(context)->append({ static_cast<Bindings::InterfaceName>(interface_name), StringView { identifier } });
    });
    return entries;
}

}
