/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibGC/RootVector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/ImageBitmap.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MessagePort.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/ReadableStream.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/TransformStream.h>
#include <LibWeb/Bindings/WritableStream.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>

// What the Rust implementation of structured serialization asks of LibWeb's C++ code, declared on the Rust side in
// Libraries/LibWeb/Bindings/Rust/src/html/structured_serialize/cpp.rs. C++ keeps the records that the serializations
// write and read, which the serialization steps of the interfaces implemented in C++ write and read as well, the
// memories of the serializations, the platform objects, and the side tables that SharedArrayBuffers are shared
// through.

using namespace Web;
using namespace Web::HTML;

extern "C" {

u8 web_structured_serialize_writer_type(StructuredSerializeWriter*);
void web_structured_serialize_writer_encode_u8(StructuredSerializeWriter*, u8);
void web_structured_serialize_writer_encode_bool(StructuredSerializeWriter*, bool);
void web_structured_serialize_writer_encode_i32(StructuredSerializeWriter*, i32);
void web_structured_serialize_writer_encode_u32(StructuredSerializeWriter*, u32);
void web_structured_serialize_writer_encode_u64(StructuredSerializeWriter*, u64);
void web_structured_serialize_writer_encode_f64(StructuredSerializeWriter*, double);
void web_structured_serialize_writer_encode_utf16_string(StructuredSerializeWriter*, FlatPtr);
void web_structured_serialize_writer_encode_optional_utf16_string(StructuredSerializeWriter*, bool has_value, FlatPtr);
void web_structured_serialize_writer_encode_bytes(StructuredSerializeWriter*, ReadonlyBytes);
void web_structured_serialize_writer_encode_interface_name(StructuredSerializeWriter*, u16);
void web_structured_serialize_writer_encode_big_int(StructuredSerializeWriter*, JSValue);
u32 web_structured_serialize_writer_add_shared_array_buffer(StructuredSerializeWriter*, JSObject* array_buffer);
bool web_structured_serialize_writer_can_share_array_buffer_memory(StructuredSerializeWriter*, JSObject* array_buffer);
u32 web_structured_serialize_writer_shared_buffer_count(StructuredSerializeWriter*);
void web_structured_serialize_writer_append_shared_buffer(StructuredSerializeWriter*, JSObject* array_buffer);

u8 web_structured_serialize_reader_type(StructuredSerializeReader*);
bool web_structured_serialize_reader_is_at_end(StructuredSerializeReader*);
JSCompletion web_structured_serialize_reader_decode_u8(StructuredSerializeReader*, void* realm, u8*);
JSCompletion web_structured_serialize_reader_decode_bool(StructuredSerializeReader*, void* realm, bool*);
JSCompletion web_structured_serialize_reader_decode_i32(StructuredSerializeReader*, void* realm, i32*);
JSCompletion web_structured_serialize_reader_decode_u32(StructuredSerializeReader*, void* realm, u32*);
JSCompletion web_structured_serialize_reader_decode_u64(StructuredSerializeReader*, void* realm, u64*);
JSCompletion web_structured_serialize_reader_decode_size(StructuredSerializeReader*, void* realm, size_t*);
JSCompletion web_structured_serialize_reader_decode_f64(StructuredSerializeReader*, void* realm, double*);
JSCompletion web_structured_serialize_reader_decode_utf16_string(StructuredSerializeReader*, void* realm, FlatPtr*);
JSCompletion web_structured_serialize_reader_decode_optional_utf16_string(StructuredSerializeReader*, void* realm, bool* has_value, FlatPtr*);
JSCompletion web_structured_serialize_reader_decode_byte_buffer(StructuredSerializeReader*, void* realm, ByteBuffer** buffer);
size_t web_structured_serialize_byte_buffer_size(ByteBuffer*);
JSObject* web_structured_serialize_array_buffer_from_byte_buffer(void* realm, ByteBuffer*, bool shared);
void web_structured_serialize_byte_buffer_destroy(ByteBuffer*);
JSCompletion web_structured_serialize_reader_decode_interface_name(StructuredSerializeReader*, void* realm, u16*);
JSCompletion web_structured_serialize_reader_decode_big_int(StructuredSerializeReader*, void* realm);
JSCompletion web_structured_serialize_reader_array_buffer_from_shared_memory(StructuredSerializeReader*, void* realm, u32 index, u64 shared_object_id);
JSObject* web_structured_serialize_reader_array_buffer_sharing_side_table_entry(StructuredSerializeReader*, void* realm, u32 side_table_index, size_t byte_length, bool preserve_growability);
JSCompletion web_structured_serialize_data_clone_error_from_serialization_error(void* realm, ReadonlyBytes message);

bool web_structured_serialize_memory_get(SerializationMemory*, JSValue, u32* index);
void web_structured_serialize_memory_set(SerializationMemory*, JSValue, u32 index);
u32 web_structured_serialize_memory_size(SerializationMemory*);
SerializationMemory* web_structured_serialize_memory_create();
void web_structured_serialize_memory_destroy(SerializationMemory*);
u64 web_structured_serialize_serializable_payload_version();
size_t web_structured_deserialize_memory_size(DeserializationMemory*);
void web_structured_deserialize_memory_append(DeserializationMemory*, JSValue);
JSValue web_structured_deserialize_memory_get(DeserializationMemory*, size_t index);
void web_structured_deserialize_memory_set(DeserializationMemory*, size_t index, JSValue);
DeserializationMemory* web_structured_deserialize_memory_create();
void web_structured_deserialize_memory_destroy(DeserializationMemory*);

bool web_structured_serialize_current_settings_object_agent_cluster(u64* agent_cluster);
bool web_structured_serialize_current_settings_object_cross_origin_isolated_capability();
bool web_structured_serialize_realm_agent_cluster(void* realm, u64* agent_cluster);

bool web_structured_serialize_is_platform_object(JSObject*);
bool web_structured_serialize_serializable_interface_name(JSObject*, u16* interface_name);
JSCompletion web_structured_serialize_serialization_steps(void* realm, JSObject*, StructuredSerializeWriter*, bool for_storage, SerializationMemory*);
bool web_structured_serialize_is_exposed(u16 interface_name, void* realm);
bool web_structured_serialize_serializable_interface_is_implemented_in_rust(u16 interface_name);
JSCompletion web_structured_serialize_deserialize_rust_platform_object(u16 interface_name, void* realm, StructuredSerializeReader*, DeserializationMemory*);
JSObject* web_structured_serialize_create_serialized_platform_object(u16 interface_name, void* realm);
JSCompletion web_structured_serialize_deserialization_steps(void* realm, JSObject*, StructuredSerializeReader*, DeserializationMemory*);

bool web_structured_serialize_is_transferable(JSObject*);
bool web_structured_serialize_transferable_is_detached(JSObject*);
void web_structured_serialize_transferable_set_detached(JSObject*);
u8 web_structured_serialize_transferable_primary_interface(JSObject*);
JSCompletion web_structured_serialize_transfer_steps(void* realm, JSObject*, TransferDataEncoder*, ImageBitmapSharedPixels*);
bool web_structured_serialize_is_transferable_interface_exposed(u8 type, void* realm);
JSCompletion web_structured_serialize_create_transferred_platform_object(u8 type, void* realm, TransferDataDecoder*);
ImageBitmapSharedPixels* web_structured_serialize_gather_image_bitmap_pixels(StructuredSerializeWriter*, JSObject* const* transfer_list, size_t count);
void web_structured_serialize_image_bitmap_pixels_destroy(ImageBitmapSharedPixels*);

TransferDataEncoder* web_structured_serialize_transfer_data_holder_create();
void web_structured_serialize_transfer_data_holder_encode_u8(TransferDataEncoder*, u8);
void web_structured_serialize_transfer_data_holder_encode_bytes(TransferDataEncoder*, ReadonlyBytes);
void web_structured_serialize_transfer_data_holder_encode_size(TransferDataEncoder*, size_t);
void web_structured_serialize_transfer_data_holder_destroy(TransferDataEncoder*);
void web_structured_serialize_transfer_record_append_data_holder(SerializedTransferRecord*, TransferDataEncoder*);
size_t web_structured_serialize_transfer_record_data_holder_count(SerializedTransferRecord*);
TransferDataDecoder* web_structured_serialize_transfer_record_take_data_holder(SerializedTransferRecord*, size_t index);
JSCompletion web_structured_serialize_transfer_record_deserialize(SerializedTransferRecord*, void* realm, DeserializationMemory*);
void web_structured_serialize_transfer_record_clear_shared_buffers(SerializedTransferRecord*);
void web_structured_serialize_transferred_values_append(Vector<GC::Root<JS::Object>>*, JSObject*);
JSCompletion web_structured_serialize_transfer_data_decoder_decode_u8(TransferDataDecoder*, void* realm, u8*);
JSCompletion web_structured_serialize_transfer_data_decoder_decode_byte_buffer(TransferDataDecoder*, void* realm, ByteBuffer** buffer);
JSCompletion web_structured_serialize_transfer_data_decoder_decode_size(TransferDataDecoder*, void* realm, size_t*);
void web_structured_serialize_transfer_data_decoder_destroy(TransferDataDecoder*);
}

static JS::Realm& realm_from_abi(void* realm)
{
    return *static_cast<JS::Realm*>(realm);
}

static JSCompletion void_completion_to_abi(JS::Realm& realm, WebIDL::ExceptionOr<void> result)
{
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, WebIDL::ExceptionOr<JS::Value> { result.release_error() });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { JS::js_undefined() });
}

template<typename T>
static JSCompletion decode_into(JS::Realm& realm, StructuredSerializeReader& reader, T& out_value)
{
    auto value = decode_or_throw_data_clone_error<T>(realm, reader);
    if (value.is_exception())
        return Bindings::completion_to_abi(realm, WebIDL::ExceptionOr<JS::Value> { value.release_error() });
    out_value = value.release_value();
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { JS::js_undefined() });
}

template<typename T>
static JSCompletion decode_into(JS::Realm& realm, TransferDataDecoder& decoder, T& out_value)
{
    auto value = decode_or_throw_data_clone_error<T>(realm, decoder);
    if (value.is_exception())
        return Bindings::completion_to_abi(realm, WebIDL::ExceptionOr<JS::Value> { value.release_error() });
    out_value = value.release_value();
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { JS::js_undefined() });
}

static JS::ArrayBuffer& array_buffer_from_abi(JSObject* array_buffer)
{
    return as<JS::ArrayBuffer>(*JS::HostABI::object_from_abi(array_buffer));
}

u8 web_structured_serialize_writer_type(StructuredSerializeWriter* writer)
{
    return to_underlying(writer->type());
}

void web_structured_serialize_writer_encode_u8(StructuredSerializeWriter* writer, u8 value)
{
    writer->encode(value);
}

void web_structured_serialize_writer_encode_bool(StructuredSerializeWriter* writer, bool value)
{
    writer->encode(value);
}

void web_structured_serialize_writer_encode_i32(StructuredSerializeWriter* writer, i32 value)
{
    writer->encode(static_cast<int>(value));
}

void web_structured_serialize_writer_encode_u32(StructuredSerializeWriter* writer, u32 value)
{
    writer->encode(value);
}

void web_structured_serialize_writer_encode_u64(StructuredSerializeWriter* writer, u64 value)
{
    writer->encode(static_cast<unsigned long long>(value));
}

void web_structured_serialize_writer_encode_f64(StructuredSerializeWriter* writer, double value)
{
    writer->encode(value);
}

void web_structured_serialize_writer_encode_utf16_string(StructuredSerializeWriter* writer, FlatPtr string)
{
    writer->encode(Utf16String::from_raw(string));
}

void web_structured_serialize_writer_encode_optional_utf16_string(StructuredSerializeWriter* writer, bool has_value, FlatPtr string)
{
    writer->encode(has_value ? Optional<Utf16String> { Utf16String::from_raw(string) } : Optional<Utf16String> {});
}

void web_structured_serialize_writer_encode_bytes(StructuredSerializeWriter* writer, ReadonlyBytes bytes)
{
    writer->encode(bytes);
}

void web_structured_serialize_writer_encode_interface_name(StructuredSerializeWriter* writer, u16 interface_name)
{
    writer->encode(static_cast<Bindings::InterfaceName>(interface_name));
}

void web_structured_serialize_writer_encode_big_int(StructuredSerializeWriter* writer, JSValue big_int)
{
    encode_signed_big_integer(*writer, JS::HostABI::value_from_abi(big_int).as_bigint().big_integer());
}

u32 web_structured_serialize_writer_add_shared_array_buffer(StructuredSerializeWriter* writer, JSObject* array_buffer)
{
    return writer->add_shared_array_buffer(array_buffer_from_abi(array_buffer));
}

bool web_structured_serialize_writer_can_share_array_buffer_memory(StructuredSerializeWriter* writer, JSObject* array_buffer)
{
    return array_buffer_from_abi(array_buffer).shared_buffer().has_value() && writer->supports_shared_buffers();
}

u32 web_structured_serialize_writer_shared_buffer_count(StructuredSerializeWriter* writer)
{
    return static_cast<u32>(writer->shared_buffers().size());
}

void web_structured_serialize_writer_append_shared_buffer(StructuredSerializeWriter* writer, JSObject* array_buffer)
{
    writer->shared_buffers().append(array_buffer_from_abi(array_buffer).shared_buffer().release_value());
}

u8 web_structured_serialize_reader_type(StructuredSerializeReader* reader)
{
    return to_underlying(reader->type());
}

bool web_structured_serialize_reader_is_at_end(StructuredSerializeReader* reader)
{
    return reader->is_at_end();
}

JSCompletion web_structured_serialize_reader_decode_u8(StructuredSerializeReader* reader, void* realm, u8* value)
{
    return decode_into(realm_from_abi(realm), *reader, *value);
}

JSCompletion web_structured_serialize_reader_decode_bool(StructuredSerializeReader* reader, void* realm, bool* value)
{
    return decode_into(realm_from_abi(realm), *reader, *value);
}

JSCompletion web_structured_serialize_reader_decode_i32(StructuredSerializeReader* reader, void* realm, i32* value)
{
    int decoded = 0;
    auto completion = decode_into(realm_from_abi(realm), *reader, decoded);
    *value = decoded;
    return completion;
}

JSCompletion web_structured_serialize_reader_decode_u32(StructuredSerializeReader* reader, void* realm, u32* value)
{
    return decode_into(realm_from_abi(realm), *reader, *value);
}

JSCompletion web_structured_serialize_reader_decode_u64(StructuredSerializeReader* reader, void* realm, u64* value)
{
    unsigned long long decoded = 0;
    auto completion = decode_into(realm_from_abi(realm), *reader, decoded);
    *value = decoded;
    return completion;
}

JSCompletion web_structured_serialize_reader_decode_size(StructuredSerializeReader* reader, void* realm, size_t* value)
{
    return decode_into(realm_from_abi(realm), *reader, *value);
}

JSCompletion web_structured_serialize_reader_decode_f64(StructuredSerializeReader* reader, void* realm, double* value)
{
    return decode_into(realm_from_abi(realm), *reader, *value);
}

JSCompletion web_structured_serialize_reader_decode_utf16_string(StructuredSerializeReader* reader, void* realm, FlatPtr* string)
{
    Utf16String decoded;
    auto completion = decode_into(realm_from_abi(realm), *reader, decoded);
    *string = move(decoded).into_raw();
    return completion;
}

JSCompletion web_structured_serialize_reader_decode_optional_utf16_string(StructuredSerializeReader* reader, void* realm, bool* has_value, FlatPtr* string)
{
    Optional<Utf16String> decoded;
    auto completion = decode_into(realm_from_abi(realm), *reader, decoded);
    *has_value = decoded.has_value();
    *string = decoded.has_value() ? decoded.release_value().into_raw() : 0;
    return completion;
}

// The bytes of a record, decoded into a buffer of their own, which the ArrayBuffer made from them takes over.
JSCompletion web_structured_serialize_reader_decode_byte_buffer(StructuredSerializeReader* reader, void* realm, ByteBuffer** buffer)
{
    auto& js_realm = realm_from_abi(realm);
    auto decoded = decode_or_throw_data_clone_error<ByteBuffer>(js_realm, *reader);
    if (decoded.is_exception())
        return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { decoded.release_error() });
    *buffer = new ByteBuffer(decoded.release_value());
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { JS::js_undefined() });
}

size_t web_structured_serialize_byte_buffer_size(ByteBuffer* buffer)
{
    return buffer->size();
}

JSObject* web_structured_serialize_array_buffer_from_byte_buffer(void* realm, ByteBuffer* buffer, bool shared)
{
    auto array_buffer = JS::ArrayBuffer::create(realm_from_abi(realm), move(*buffer), shared ? JS::DataBlock::Shared::Yes : JS::DataBlock::Shared::No);
    delete buffer;
    return JS::HostABI::object_to_abi(array_buffer.ptr());
}

void web_structured_serialize_byte_buffer_destroy(ByteBuffer* buffer)
{
    delete buffer;
}

JSCompletion web_structured_serialize_reader_decode_interface_name(StructuredSerializeReader* reader, void* realm, u16* interface_name)
{
    Bindings::InterfaceName decoded {};
    auto completion = decode_into(realm_from_abi(realm), *reader, decoded);
    *interface_name = to_underlying(decoded);
    return completion;
}

JSCompletion web_structured_serialize_reader_decode_big_int(StructuredSerializeReader* reader, void* realm)
{
    auto& js_realm = realm_from_abi(realm);
    auto big_integer = decode_signed_big_integer(*reader, js_realm);
    if (big_integer.is_exception())
        return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { big_integer.release_error() });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { JS::BigInt::create(js_realm.vm(), big_integer.release_value()) });
}

// AD-HOC: When the backing was transferred as cross-process shared memory, map it from the side list by index — so
//         both agents reference the same bytes.
JSCompletion web_structured_serialize_reader_array_buffer_from_shared_memory(StructuredSerializeReader* reader, void* realm, u32 index, u64 shared_object_id)
{
    auto& js_realm = realm_from_abi(realm);
    auto const* shared_buffers = reader->shared_buffers();
    if (!shared_buffers || index >= shared_buffers->size())
        return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { data_clone_error_from_serialization_error(js_realm, AK::Error::from_string_literal("SharedArrayBuffer side-list index out of range")) });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { JS::ArrayBuffer::create(js_realm, shared_buffers->at(index), shared_object_id) });
}

// Returns no value when the source storage cannot be aliased, allowing the caller to copy it.
static Optional<JS::DataBlock> data_block_sharing_storage_with(JS::ArrayBuffer& source_buffer, size_t byte_length, bool preserve_growability = false)
{
    VERIFY(source_buffer.is_shared_array_buffer());
    auto const& source_block = source_buffer.data_block();

    // Flatten alias chains to the actual owner. Keep this buffer's byte length because the owner's
    // storage may be larger (for example, a fixed-length view of growable WebAssembly.Memory).
    if (auto const* external = source_block.byte_buffer.get_pointer<JS::DataBlock::ExternalPrimitiveStorage>()) {
        if (preserve_growability)
            return JS::DataBlock { JS::DataBlock::ExternalPrimitiveStorage { external->owner, external->handle }, JS::DataBlock::Shared::Yes };
        return JS::DataBlock { JS::DataBlock::ExternalPrimitiveStorage { external->owner, external->handle, byte_length }, JS::DataBlock::Shared::Yes };
    }

    // AD-HOC: A fixed-length shared block backed by cross-process shared memory is aliased by referencing the same
    //         shared object — so the clone shares its bytes, and can itself be shared with an agent in another process.
    if (auto const* shared = source_block.byte_buffer.get_pointer<JS::DataBlock::SharedBackingStore>())
        return JS::DataBlock { JS::DataBlock::SharedBackingStore { shared->buffer, shared->object_id }, JS::DataBlock::Shared::Yes };

    if (auto const* owned = source_block.byte_buffer.get_pointer<JS::DataBlock::OwnedBackingStore>()) {
        // Zero-length buffers have no backing storage to share.
        if (!owned->handle().is_valid())
            return {};
        if (preserve_growability)
            return JS::DataBlock { JS::DataBlock::ExternalPrimitiveStorage { GC::Ref<GC::Cell> { source_buffer }, owned->handle() }, JS::DataBlock::Shared::Yes };
        return JS::DataBlock { JS::DataBlock::ExternalPrimitiveStorage { GC::Ref<GC::Cell> { source_buffer }, owned->handle(), byte_length }, JS::DataBlock::Shared::Yes };
    }

    // A detached block has no storage to share.
    return {};
}

// A new SharedArrayBuffer that shares the storage of the buffer at the index of the record's same-process side table,
// or null if there is no such buffer, or its storage cannot be shared.
JSObject* web_structured_serialize_reader_array_buffer_sharing_side_table_entry(StructuredSerializeReader* reader, void* realm, u32 side_table_index, size_t byte_length, bool preserve_growability)
{
    auto shared_array_buffers = reader->shared_array_buffers();
    if (side_table_index >= shared_array_buffers.size())
        return nullptr;
    auto data_block = data_block_sharing_storage_with(*shared_array_buffers[side_table_index], byte_length, preserve_growability);
    if (!data_block.has_value())
        return nullptr;
    return JS::HostABI::object_to_abi(JS::ArrayBuffer::create(realm_from_abi(realm), data_block.release_value()).ptr());
}

JSCompletion web_structured_serialize_data_clone_error_from_serialization_error(void* realm, ReadonlyBytes message)
{
    auto& js_realm = realm_from_abi(realm);
    auto error = AK::Error::from_string_view(StringView { message });
    return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { data_clone_error_from_serialization_error(js_realm, error) });
}

bool web_structured_serialize_memory_get(SerializationMemory* memory, JSValue value, u32* index)
{
    auto found = memory->get(JS::HostABI::value_from_abi(value));
    if (!found.has_value())
        return false;
    *index = found.value();
    return true;
}

void web_structured_serialize_memory_set(SerializationMemory* memory, JSValue value, u32 index)
{
    memory->set(GC::make_root(JS::HostABI::value_from_abi(value)), index);
}

u32 web_structured_serialize_memory_size(SerializationMemory* memory)
{
    return memory->size();
}

SerializationMemory* web_structured_serialize_memory_create()
{
    return new SerializationMemory;
}

void web_structured_serialize_memory_destroy(SerializationMemory* memory)
{
    delete memory;
}

u64 web_structured_serialize_serializable_payload_version()
{
    return serializable_payload_version;
}

size_t web_structured_deserialize_memory_size(DeserializationMemory* memory)
{
    return memory->size();
}

void web_structured_deserialize_memory_append(DeserializationMemory* memory, JSValue value)
{
    memory->append(JS::HostABI::value_from_abi(value));
}

JSValue web_structured_deserialize_memory_get(DeserializationMemory* memory, size_t index)
{
    return JS::HostABI::value_to_abi((*memory)[index]);
}

void web_structured_deserialize_memory_set(DeserializationMemory* memory, size_t index, JSValue value)
{
    (*memory)[index] = JS::HostABI::value_from_abi(value);
}

DeserializationMemory* web_structured_deserialize_memory_create()
{
    return new DeserializationMemory;
}

void web_structured_deserialize_memory_destroy(DeserializationMemory* memory)
{
    delete memory;
}

bool web_structured_serialize_current_settings_object_agent_cluster(u64* agent_cluster)
{
    auto id = current_settings_object().agent_cluster_id();
    if (!id.has_value())
        return false;
    *agent_cluster = *id;
    return true;
}

bool web_structured_serialize_current_settings_object_cross_origin_isolated_capability()
{
    return current_settings_object().cross_origin_isolated_capability() == CanUseCrossOriginIsolatedAPIs::Yes;
}

bool web_structured_serialize_realm_agent_cluster(void* realm, u64* agent_cluster)
{
    auto id = Bindings::principal_host_defined_environment_settings_object(realm_from_abi(realm)).agent_cluster_id();
    if (!id.has_value())
        return false;
    *agent_cluster = *id;
    return true;
}

bool web_structured_serialize_is_platform_object(JSObject* object)
{
    return Bindings::is_platform_object(*JS::HostABI::object_from_abi(object));
}

bool web_structured_serialize_serializable_interface_name(JSObject* object, u16* interface_name)
{
    auto serializable = Bindings::serializable_from_object(*JS::HostABI::object_from_abi(object));
    if (!serializable.has_value())
        return false;
    *interface_name = to_underlying(serializable->interface_name);
    return true;
}

JSCompletion web_structured_serialize_serialization_steps(void* realm, JSObject* object, StructuredSerializeWriter* writer, bool for_storage, SerializationMemory* memory)
{
    auto serializable = Bindings::serializable_from_object(*JS::HostABI::object_from_abi(object));
    VERIFY(serializable.has_value());
    return void_completion_to_abi(realm_from_abi(realm), serializable->serialization_steps(*writer, for_storage, *memory));
}

bool web_structured_serialize_is_exposed(u16 interface_name, void* realm)
{
    return Bindings::is_exposed(static_cast<Bindings::InterfaceName>(interface_name), realm_from_abi(realm));
}

bool web_structured_serialize_serializable_interface_is_implemented_in_rust(u16 interface_name)
{
    return Bindings::serializable_interface_is_implemented_in_rust(static_cast<Bindings::InterfaceName>(interface_name));
}

JSCompletion web_structured_serialize_deserialize_rust_platform_object(u16 interface_name, void* realm, StructuredSerializeReader* reader, DeserializationMemory* memory)
{
    auto& js_realm = realm_from_abi(realm);
    auto object = Bindings::deserialize_rust_platform_object(static_cast<Bindings::InterfaceName>(interface_name), js_realm, *reader, *memory);
    if (object.is_exception())
        return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { object.release_error() });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { object.release_value() });
}

JSObject* web_structured_serialize_create_serialized_platform_object(u16 interface_name, void* realm)
{
    return JS::HostABI::object_to_abi(Bindings::create_serialized_platform_object(static_cast<Bindings::InterfaceName>(interface_name), realm_from_abi(realm)).ptr());
}

JSCompletion web_structured_serialize_deserialization_steps(void* realm, JSObject* object, StructuredSerializeReader* reader, DeserializationMemory* memory)
{
    auto& js_realm = realm_from_abi(realm);
    auto serializable = Bindings::serializable_from_object(*JS::HostABI::object_from_abi(object));
    VERIFY(serializable.has_value() && serializable->serializable);
    return void_completion_to_abi(js_realm, serializable->serializable->deserialization_steps(js_realm, *reader, *memory));
}

bool web_structured_serialize_is_transferable(JSObject* object)
{
    return Bindings::transferable_from_object(*JS::HostABI::object_from_abi(object)).has_value();
}

bool web_structured_serialize_transferable_is_detached(JSObject* object)
{
    return Bindings::transferable_from_object(*JS::HostABI::object_from_abi(object))->is_detached();
}

void web_structured_serialize_transferable_set_detached(JSObject* object)
{
    Bindings::transferable_from_object(*JS::HostABI::object_from_abi(object))->set_detached();
}

u8 web_structured_serialize_transferable_primary_interface(JSObject* object)
{
    return to_underlying(Bindings::transferable_from_object(*JS::HostABI::object_from_abi(object))->primary_interface);
}

JSCompletion web_structured_serialize_transfer_steps(void* realm, JSObject* object, TransferDataEncoder* data_holder, ImageBitmapSharedPixels* image_bitmap_pixels)
{
    auto& js_realm = realm_from_abi(realm);
    auto transferable = Bindings::transferable_from_object(*JS::HostABI::object_from_abi(object));
    return void_completion_to_abi(js_realm, transferable->transfer_steps(js_realm, *data_holder, image_bitmap_pixels));
}

bool web_structured_serialize_is_transferable_interface_exposed(u8 type, void* realm)
{
    auto& js_realm = realm_from_abi(realm);
    auto name = static_cast<TransferType>(type);
    switch (name) {
    case TransferType::MessagePort:
        return is_exposed(Bindings::InterfaceName::MessagePort, js_realm);
    case TransferType::ReadableStream:
        return is_exposed(Bindings::InterfaceName::ReadableStream, js_realm);
    case TransferType::WritableStream:
        return is_exposed(Bindings::InterfaceName::WritableStream, js_realm);
    case TransferType::TransformStream:
        return is_exposed(Bindings::InterfaceName::TransformStream, js_realm);
    case TransferType::ImageBitmap:
        return is_exposed(Bindings::InterfaceName::ImageBitmap, js_realm);
    case TransferType::OffscreenCanvas:
        return is_exposed(Bindings::InterfaceName::OffscreenCanvas, js_realm);
    case TransferType::Unknown:
        dbgln("Unknown interface type for transfer: {}", to_underlying(name));
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    return false;
}

JSCompletion web_structured_serialize_create_transferred_platform_object(u8 type, void* realm, TransferDataDecoder* decoder)
{
    auto& js_realm = realm_from_abi(realm);
    auto object = Bindings::create_transferred_platform_object(static_cast<TransferType>(type), js_realm, *decoder);
    if (object.is_exception())
        return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { object.release_error() });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { object.release_value() });
}

// AD-HOC: Gather the pixels of the transferred ImageBitmaps in one buffer of shared memory. Without it, as when there
//         is not enough memory for it, the pixels travel inline.
ImageBitmapSharedPixels* web_structured_serialize_gather_image_bitmap_pixels(StructuredSerializeWriter* writer, JSObject* const* transfer_list, size_t count)
{
    Checked<size_t> image_bitmap_pixels_size = 0;
    for (size_t i = 0; i < count; ++i) {
        if (auto* image_bitmap = Bindings::impl_from<ImageBitmap>(JS::HostABI::object_from_abi(transfer_list[i])); image_bitmap && image_bitmap->bitmap())
            image_bitmap_pixels_size += ImageBitmap::shared_pixels_size(*image_bitmap->bitmap());
    }
    if (image_bitmap_pixels_size.has_overflow() || image_bitmap_pixels_size.value() == 0)
        return nullptr;
    auto buffer = Core::AnonymousBuffer::create_with_size(image_bitmap_pixels_size.value());
    if (buffer.is_error())
        return nullptr;
    auto* image_bitmap_pixels = new ImageBitmapSharedPixels { .buffer = buffer.release_value(), .buffer_index = static_cast<u32>(writer->shared_buffers().size()) };
    writer->shared_buffers().append(image_bitmap_pixels->buffer);
    return image_bitmap_pixels;
}

void web_structured_serialize_image_bitmap_pixels_destroy(ImageBitmapSharedPixels* image_bitmap_pixels)
{
    delete image_bitmap_pixels;
}

TransferDataEncoder* web_structured_serialize_transfer_data_holder_create()
{
    return new TransferDataEncoder;
}

void web_structured_serialize_transfer_data_holder_encode_u8(TransferDataEncoder* data_holder, u8 value)
{
    MUST(data_holder->encode(value));
}

void web_structured_serialize_transfer_data_holder_encode_bytes(TransferDataEncoder* data_holder, ReadonlyBytes bytes)
{
    MUST(data_holder->encode(bytes));
}

void web_structured_serialize_transfer_data_holder_encode_size(TransferDataEncoder* data_holder, size_t value)
{
    MUST(data_holder->encode(value));
}

void web_structured_serialize_transfer_data_holder_destroy(TransferDataEncoder* data_holder)
{
    delete data_holder;
}

void web_structured_serialize_transfer_record_append_data_holder(SerializedTransferRecord* record, TransferDataEncoder* data_holder)
{
    record->transfer_data_holders.append(move(*data_holder));
    delete data_holder;
}

size_t web_structured_serialize_transfer_record_data_holder_count(SerializedTransferRecord* record)
{
    return record->transfer_data_holders.size();
}

// A decoder of the data holder at the index, which it takes from the record, or null if the data holder is empty.
TransferDataDecoder* web_structured_serialize_transfer_record_take_data_holder(SerializedTransferRecord* record, size_t index)
{
    auto& transfer_data_holder = record->transfer_data_holders[index];
    if (transfer_data_holder.buffer().data().is_empty())
        return nullptr;
    auto* decoder = new TransferDataDecoder { move(transfer_data_holder) };
    decoder->set_shared_buffers(record->shared_buffers);
    return decoder;
}

JSCompletion web_structured_serialize_transfer_record_deserialize(SerializedTransferRecord* record, void* realm, DeserializationMemory* memory)
{
    auto& js_realm = realm_from_abi(realm);
    auto deserialized = structured_deserialize(js_realm.vm(), record->serialized, js_realm, *memory, &record->shared_buffers);
    if (deserialized.is_exception())
        return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { deserialized.release_error() });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { deserialized.release_value() });
}

void web_structured_serialize_transfer_record_clear_shared_buffers(SerializedTransferRecord* record)
{
    record->shared_buffers.clear();
}

void web_structured_serialize_transferred_values_append(Vector<GC::Root<JS::Object>>* transferred_values, JSObject* value)
{
    transferred_values->append(GC::make_root(*JS::HostABI::object_from_abi(value)));
}

JSCompletion web_structured_serialize_transfer_data_decoder_decode_u8(TransferDataDecoder* decoder, void* realm, u8* value)
{
    return decode_into(realm_from_abi(realm), *decoder, *value);
}

JSCompletion web_structured_serialize_transfer_data_decoder_decode_byte_buffer(TransferDataDecoder* decoder, void* realm, ByteBuffer** buffer)
{
    auto& js_realm = realm_from_abi(realm);
    auto decoded = decoder->decode_buffer();
    if (decoded.is_exception())
        return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { decoded.release_error() });
    *buffer = new ByteBuffer(decoded.release_value());
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { JS::js_undefined() });
}

JSCompletion web_structured_serialize_transfer_data_decoder_decode_size(TransferDataDecoder* decoder, void* realm, size_t* value)
{
    return decode_into(realm_from_abi(realm), *decoder, *value);
}

void web_structured_serialize_transfer_data_decoder_destroy(TransferDataDecoder* decoder)
{
    delete decoder;
}
