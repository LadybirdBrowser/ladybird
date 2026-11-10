/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/MemoryStream.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGC/RootVector.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibIPC/RawAttachment.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace JS {

class HostObject;

}

namespace Web::HTML {

using TransferAttachment = IPC::RawAttachment;

struct StructuredSerializeOptions {
    GC::RootVector<GC::Ref<JS::Object>> transfer;
};

struct DeserializedTransferRecord {
    JS::Value deserialized;
    Vector<GC::Root<JS::Object>> transferred_values;
};

// AD-HOC: Serializing a SharedArrayBuffer is normally gated on the current settings object's
//         cross-origin isolated capability (a Spectre mitigation for data that may reach another
//         agent). SameAgentAlways bypasses that gate for internal clones that never leave the
//         surrounding agent, where the gate's threat model does not apply. Web-visible entry points
//         must use CrossOriginIsolatedOnly.
enum class AllowSharedArrayBuffers : u8 {
    CrossOriginIsolatedOnly,
    SameAgentAlways,
};

// The SharedArrayBuffers that a record produced in this process aliases instead of copying.
WEB_API ReadonlySpan<GC::Root<JS::ArrayBuffer>> same_agent_shared_array_buffers(IPCSerializationRecord const&);
IPCSerializationRecord create_ipc_serialization_record(ReadonlyBytes data, Vector<GC::Root<JS::ArrayBuffer>> shared_array_buffers);

WEB_API WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM&, JS::Value);
WEB_API WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM&, JS::Value, AllowSharedArrayBuffers);
WEB_API WebIDL::ExceptionOr<StorageSerializationRecord> structured_serialize_for_storage(JS::VM&, JS::Value);
WEB_API StorageSerializationRecord structured_serialize_undefined_or_null_for_storage(JS::Value);

WEB_API WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM&, IPCSerializationRecord const&, JS::Realm&);
WEB_API WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM&, StorageSerializationRecord const&, JS::Realm&);

WEB_API WebIDL::Exception data_clone_error_from_serialization_error(JS::Realm&, AK::Error const&);

// Pairs each serializable interface with its storage identifier.
struct SerializableRegistryEntry {
    Bindings::InterfaceName interface_name;
    StringView identifier;
};

WEB_API Vector<SerializableRegistryEntry> serializable_storage_registry();

WEB_API WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::Realm&, JS::Value, ReadonlySpan<GC::Ref<JS::Object>> transfer_list);
WEB_API WebIDL::ExceptionOr<DeserializedTransferRecord> structured_deserialize_with_transfer(SerializedTransferRecord&, JS::Realm&);

}
