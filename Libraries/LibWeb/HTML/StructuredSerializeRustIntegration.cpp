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
#include <LibGC/RootVector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>

// What the Rust implementation of structured serialization asks of LibWeb's C++ code, declared on the Rust side in
// Libraries/LibWeb/Bindings/Rust/src/html/structured_serialize/cpp.rs. C++ keeps the records that the serializations
// hand their bytes over to and the deserializations read the bytes of, the registry of serializable interfaces, the
// platform objects, and the side tables that SharedArrayBuffers are shared through.

using namespace Web;
using namespace Web::HTML;

// Whose steps serialize and deserialize the objects of a serializable interface.
enum class SerializableSteps : u8 {
    Rust,
    DOMException,
    QuotaExceededError,
    CryptoKey,
    RTCEncodedAudioFrame,
};

extern "C" {

void web_structured_serialize_set_ipc_record(IPCSerializationRecord*, ReadonlyBytes data, JSObject* const* shared_array_buffers, size_t count);
void web_structured_serialize_set_storage_record(StorageSerializationRecord*, ReadonlyBytes data);
bool web_structured_serialize_interface_storage_identifier(u16 interface_name, ReadonlyBytes* identifier);
bool web_structured_serialize_is_ipc_interface(u16 interface_name);
bool web_structured_serialize_interface_name_from_storage_identifier(ReadonlyBytes identifier, u16* interface_name);

void web_structured_serialize_ipc_record_data(IPCSerializationRecord const*, ReadonlyBytes* data);
JSObject* web_structured_serialize_ipc_record_shared_array_buffer(IPCSerializationRecord const*, u32 side_table_index);
JSCompletion web_structured_serialize_data_clone_error_from_serialization_error(void* realm, ReadonlyBytes message);

u64 web_structured_serialize_serializable_payload_version();

bool web_structured_serialize_current_settings_object_agent_cluster(u64* agent_cluster);
bool web_structured_serialize_current_settings_object_cross_origin_isolated_capability();
bool web_structured_serialize_realm_agent_cluster(void* realm, u64* agent_cluster);

bool web_structured_serialize_is_platform_object(JSObject*);
bool web_structured_serialize_serializable_interface_name(JSObject*, u16* interface_name);
JSCompletion web_structured_serialize_serialization_steps(void* realm, JSObject*, void* serialized, bool for_storage);
bool web_structured_serialize_is_exposed(u16 interface_name, void* realm);
u8 web_structured_serialize_serializable_steps(u16 interface_name);
JSCompletion web_structured_serialize_deserialize_rust_platform_object(u16 interface_name, void* realm, void* serialized);

ImageBitmapSharedPixels* web_structured_serialize_gather_image_bitmap_pixels(JSObject* const* transfer_list, size_t count, u32 buffer_index, int* shared_memory, size_t* size);
void web_structured_serialize_image_bitmap_pixels_destroy(ImageBitmapSharedPixels*);

TransferDataEncoder* web_structured_serialize_transfer_data_holder_create();
void web_structured_serialize_transfer_data_holder_append(TransferDataEncoder*, ReadonlyBytes);
void web_structured_serialize_transfer_data_holder_destroy(TransferDataEncoder*);
SerializedTransferRecord* web_structured_serialize_transfer_record_create();
void web_structured_serialize_transfer_record_destroy(SerializedTransferRecord*);
void web_structured_serialize_transfer_record_set_serialized(SerializedTransferRecord*, ReadonlyBytes data, JSObject* const* shared_array_buffers, size_t shared_array_buffer_count, int const* shared_memory, size_t const* shared_memory_sizes, size_t shared_memory_count);
size_t web_structured_serialize_transfer_record_shared_memory_count(SerializedTransferRecord*);
void web_structured_serialize_transfer_record_shared_memory(SerializedTransferRecord*, size_t index, int* shared_memory, size_t* size);
void web_structured_serialize_transfer_record_append_data_holder(SerializedTransferRecord*, TransferDataEncoder*);
size_t web_structured_serialize_transfer_record_data_holder_count(SerializedTransferRecord*);
TransferDataDecoder* web_structured_serialize_transfer_record_take_data_holder(SerializedTransferRecord*, size_t index);
IPCSerializationRecord const* web_structured_serialize_transfer_record_serialized(SerializedTransferRecord*);
void web_structured_serialize_transfer_record_clear_shared_buffers(SerializedTransferRecord*);
void web_structured_serialize_transferred_values_append(Vector<GC::Root<JS::Object>>*, JSObject*);
void web_structured_serialize_transfer_data_decoder_bytes(TransferDataDecoder const*, ReadonlyBytes* bytes);
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

static JS::ArrayBuffer& array_buffer_from_abi(JSObject* array_buffer)
{
    return as<JS::ArrayBuffer>(*JS::HostABI::object_from_abi(array_buffer));
}

static Vector<GC::Root<JS::ArrayBuffer>> shared_array_buffers_from_abi(JSObject* const* shared_array_buffers, size_t count)
{
    Vector<GC::Root<JS::ArrayBuffer>> roots;
    roots.ensure_capacity(count);
    for (size_t i = 0; i < count; ++i)
        roots.unchecked_append(GC::make_root(array_buffer_from_abi(shared_array_buffers[i])));
    return roots;
}

void web_structured_serialize_set_ipc_record(IPCSerializationRecord* record, ReadonlyBytes data, JSObject* const* shared_array_buffers, size_t count)
{
    *record = create_ipc_serialization_record(data, shared_array_buffers_from_abi(shared_array_buffers, count));
}

void web_structured_serialize_set_storage_record(StorageSerializationRecord* record, ReadonlyBytes data)
{
    record->data = MUST(ByteBuffer::copy(data));
}

bool web_structured_serialize_interface_storage_identifier(u16 interface_name, ReadonlyBytes* identifier)
{
    for (auto const& entry : serializable_storage_registry()) {
        if (entry.interface_name == static_cast<Bindings::InterfaceName>(interface_name)) {
            *identifier = entry.identifier.bytes();
            return true;
        }
    }
    return false;
}

bool web_structured_serialize_is_ipc_interface(u16 interface_name)
{
    // Encoded audio frames can be cloned between agents, but cannot be stored.
    if (static_cast<Bindings::InterfaceName>(interface_name) == Bindings::InterfaceName::RTCEncodedAudioFrame)
        return true;
    ReadonlyBytes identifier;
    return web_structured_serialize_interface_storage_identifier(interface_name, &identifier);
}

bool web_structured_serialize_interface_name_from_storage_identifier(ReadonlyBytes identifier, u16* interface_name)
{
    for (auto const& entry : serializable_storage_registry()) {
        if (entry.identifier.bytes() == identifier) {
            *interface_name = to_underlying(entry.interface_name);
            return true;
        }
    }
    return false;
}

void web_structured_serialize_ipc_record_data(IPCSerializationRecord const* record, ReadonlyBytes* data)
{
    *data = record->data.span();
}

JSObject* web_structured_serialize_ipc_record_shared_array_buffer(IPCSerializationRecord const* record, u32 side_table_index)
{
    auto shared_array_buffers = same_process_shared_array_buffers(*record);
    if (side_table_index >= shared_array_buffers.size())
        return nullptr;
    return JS::HostABI::object_to_abi(shared_array_buffers[side_table_index].ptr());
}

JSCompletion web_structured_serialize_data_clone_error_from_serialization_error(void* realm, ReadonlyBytes message)
{
    auto& js_realm = realm_from_abi(realm);
    auto error = AK::Error::from_string_view(StringView { message });
    return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { data_clone_error_from_serialization_error(js_realm, error) });
}

u64 web_structured_serialize_serializable_payload_version()
{
    return serializable_payload_version;
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

JSCompletion web_structured_serialize_serialization_steps(void* realm, JSObject* object, void* serialized, bool for_storage)
{
    auto serializable = Bindings::serializable_from_object(*JS::HostABI::object_from_abi(object));
    VERIFY(serializable.has_value());
    return void_completion_to_abi(realm_from_abi(realm), serializable->serialization_steps(serialized, for_storage));
}

bool web_structured_serialize_is_exposed(u16 interface_name, void* realm)
{
    return Bindings::is_exposed(static_cast<Bindings::InterfaceName>(interface_name), realm_from_abi(realm));
}

u8 web_structured_serialize_serializable_steps(u16 interface_name)
{
    switch (static_cast<Bindings::InterfaceName>(interface_name)) {
    case Bindings::InterfaceName::DOMException:
        return to_underlying(SerializableSteps::DOMException);
    case Bindings::InterfaceName::QuotaExceededError:
        return to_underlying(SerializableSteps::QuotaExceededError);
    case Bindings::InterfaceName::CryptoKey:
        return to_underlying(SerializableSteps::CryptoKey);
    case Bindings::InterfaceName::RTCEncodedAudioFrame:
        return to_underlying(SerializableSteps::RTCEncodedAudioFrame);
    default:
        VERIFY(Bindings::serializable_interface_is_implemented_in_rust(static_cast<Bindings::InterfaceName>(interface_name)));
        return to_underlying(SerializableSteps::Rust);
    }
}

JSCompletion web_structured_serialize_deserialize_rust_platform_object(u16 interface_name, void* realm, void* serialized)
{
    auto& js_realm = realm_from_abi(realm);
    auto object = Bindings::deserialize_rust_platform_object(static_cast<Bindings::InterfaceName>(interface_name), js_realm, serialized);
    if (object.is_exception())
        return Bindings::completion_to_abi(js_realm, WebIDL::ExceptionOr<JS::Value> { object.release_error() });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { object.release_value() });
}

// AD-HOC: Gather the pixels of the transferred ImageBitmaps in one buffer of shared memory. Without it, as when there
//         is not enough memory for it, the pixels travel inline.
ImageBitmapSharedPixels* web_structured_serialize_gather_image_bitmap_pixels(JSObject* const* transfer_list, size_t count, u32 buffer_index, int* shared_memory, size_t* size)
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
    auto* image_bitmap_pixels = new ImageBitmapSharedPixels { .buffer = buffer.release_value(), .buffer_index = buffer_index };
    *shared_memory = image_bitmap_pixels->buffer.fd();
    *size = image_bitmap_pixels->buffer.size();
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

void web_structured_serialize_transfer_data_holder_append(TransferDataEncoder* data_holder, ReadonlyBytes bytes)
{
    MUST(data_holder->append(bytes));
}

void web_structured_serialize_transfer_data_holder_destroy(TransferDataEncoder* data_holder)
{
    delete data_holder;
}

SerializedTransferRecord* web_structured_serialize_transfer_record_create()
{
    return new SerializedTransferRecord;
}

void web_structured_serialize_transfer_record_destroy(SerializedTransferRecord* record)
{
    delete record;
}

// The record's [[Serialized]], with the shared memory of the transfer, whose descriptors it takes over.
void web_structured_serialize_transfer_record_set_serialized(SerializedTransferRecord* record, ReadonlyBytes data, JSObject* const* shared_array_buffers, size_t shared_array_buffer_count, int const* shared_memory, size_t const* shared_memory_sizes, size_t shared_memory_count)
{
    record->serialized = create_ipc_serialization_record(data, shared_array_buffers_from_abi(shared_array_buffers, shared_array_buffer_count));
    record->shared_buffers.ensure_capacity(shared_memory_count);
    for (size_t i = 0; i < shared_memory_count; ++i)
        record->shared_buffers.unchecked_append(MUST(Core::AnonymousBuffer::create_from_anon_fd(shared_memory[i], shared_memory_sizes[i])));
}

size_t web_structured_serialize_transfer_record_shared_memory_count(SerializedTransferRecord* record)
{
    return record->shared_buffers.size();
}

void web_structured_serialize_transfer_record_shared_memory(SerializedTransferRecord* record, size_t index, int* shared_memory, size_t* size)
{
    *shared_memory = record->shared_buffers[index].fd();
    *size = record->shared_buffers[index].size();
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

IPCSerializationRecord const* web_structured_serialize_transfer_record_serialized(SerializedTransferRecord* record)
{
    return &record->serialized;
}

void web_structured_serialize_transfer_record_clear_shared_buffers(SerializedTransferRecord* record)
{
    record->shared_buffers.clear();
}

void web_structured_serialize_transferred_values_append(Vector<GC::Root<JS::Object>>* transferred_values, JSObject* value)
{
    transferred_values->append(GC::make_root(*JS::HostABI::object_from_abi(value)));
}

void web_structured_serialize_transfer_data_decoder_bytes(TransferDataDecoder const* decoder, ReadonlyBytes* bytes)
{
    *bytes = decoder->bytes();
}

void web_structured_serialize_transfer_data_decoder_destroy(TransferDataDecoder* decoder)
{
    delete decoder;
}
