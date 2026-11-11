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
#include <LibIPC/File.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>

// What the Rust implementation of structured serialization asks of LibWeb's C++ code, declared on the Rust side in
// Libraries/LibWeb/Bindings/Rust/src/html/structured_serialize/cpp.rs. C++ keeps the records that the serializations
// hand their bytes over to and the deserializations read the bytes of, C++ platform objects, and the side tables
// that SharedArrayBuffers are shared through.

using namespace Web;
using namespace Web::HTML;

using ReceiveTransferPayload = void (*)(void*, ReadonlyBytes, TransferAttachment const*, size_t);

extern "C" {

void web_structured_serialize_set_ipc_record(IPCSerializationRecord*, ReadonlyBytes data, JSObject* const* shared_array_buffers, size_t count);
void web_structured_serialize_set_storage_record(StorageSerializationRecord*, ReadonlyBytes data);

void web_structured_serialize_ipc_record_data(IPCSerializationRecord const*, ReadonlyBytes* data);
JSObject* web_structured_serialize_ipc_record_shared_array_buffer(IPCSerializationRecord const*, u32 side_table_index);
JSCompletion web_structured_serialize_data_clone_error_from_serialization_error(void* realm, ReadonlyBytes message);

bool web_structured_serialize_current_settings_object_agent_cluster(u64* agent_cluster);
bool web_structured_serialize_current_settings_object_cross_origin_isolated_capability();
bool web_structured_serialize_realm_agent_cluster(void* realm, u64* agent_cluster);

bool web_structured_serialize_cpp_interface_name(JSObject*, u16* interface_name);
bool web_structured_serialize_is_exposed(u16 interface_name, void* realm);

ImageBitmapSharedPixels* web_structured_serialize_gather_image_bitmap_pixels(JSObject* const* transfer_list, size_t count, u32 buffer_index, int* shared_memory, size_t* size);
void web_structured_serialize_image_bitmap_pixels_destroy(ImageBitmapSharedPixels*);

TransferDataEncoder* web_structured_serialize_transfer_data_holder_create(ReadonlyBytes, TransferAttachment const*, size_t);
void web_structured_serialize_transfer_data_holder_take(TransferDataEncoder*, void*, ReceiveTransferPayload);
bool web_structured_serialize_validate_shared_memory(int, size_t);
SerializedTransferRecord* web_structured_serialize_transfer_record_create(ReadonlyBytes, JSObject* const*, size_t, int const*, size_t const*, size_t, int, TransferDataEncoder* const*, size_t);
void web_structured_serialize_transfer_record_destroy(SerializedTransferRecord*);
void web_structured_serialize_transfer_record_take(SerializedTransferRecord*, void*, void (*)(void*, ReadonlyBytes, JSObject* const*, size_t, int const*, size_t const*, size_t, int), void (*)(void*, TransferDataEncoder*));
ReceivedImageBitmapSharedPixels* web_structured_serialize_shared_pixels_create(int const*, size_t const*, size_t);
void web_structured_serialize_shared_pixels_destroy(ReceivedImageBitmapSharedPixels*);
void web_structured_serialize_transferred_values_append(Vector<GC::Root<JS::Object>>*, JSObject*);
}

static JS::Realm& realm_from_abi(void* realm)
{
    return *static_cast<JS::Realm*>(realm);
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

void web_structured_serialize_ipc_record_data(IPCSerializationRecord const* record, ReadonlyBytes* data)
{
    *data = record->data.span();
}

JSObject* web_structured_serialize_ipc_record_shared_array_buffer(IPCSerializationRecord const* record, u32 side_table_index)
{
    auto shared_array_buffers = same_agent_shared_array_buffers(*record);
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

bool web_structured_serialize_cpp_interface_name(JSObject* object, u16* interface_name)
{
    auto* wrappable = Bindings::wrappable_impl_from(JS::HostABI::object_from_abi(object));
    if (!wrappable)
        return false;
    *interface_name = to_underlying(wrappable->interface_name());
    return true;
}

bool web_structured_serialize_is_exposed(u16 interface_name, void* realm)
{
    return Bindings::is_exposed(static_cast<Bindings::InterfaceName>(interface_name), realm_from_abi(realm));
}

// AD-HOC: Gather the pixels of the transferred ImageBitmaps in one buffer of shared memory. Without it, as when there
//         is not enough memory for it, the pixels travel inline.
ImageBitmapSharedPixels* web_structured_serialize_gather_image_bitmap_pixels(JSObject* const* transfer_list, size_t count, u32 buffer_index, int* shared_memory, size_t* size)
{
    Checked<size_t> image_bitmap_pixels_size = 0;
    for (size_t i = 0; i < count; ++i) {
        if (auto* image_bitmap = Bindings::impl_from<ImageBitmap>(JS::HostABI::object_from_abi(transfer_list[i])); image_bitmap && image_bitmap->frame())
            image_bitmap_pixels_size += ImageBitmap::shared_pixels_size(*image_bitmap->frame());
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

TransferDataEncoder* web_structured_serialize_transfer_data_holder_create(ReadonlyBytes bytes, TransferAttachment const* attachments, size_t count)
{
    IPC::MessageDataType data;
    MUST(data.try_append(bytes.data(), bytes.size()));
    return new TransferDataEncoder(IPC::MessageBuffer { move(data), IPC::adopt_raw_attachments({ attachments, count }) });
}

void web_structured_serialize_transfer_data_holder_take(TransferDataEncoder* holder, void* context, ReceiveTransferPayload receive)
{
    auto owner = adopt_own(*holder);
    auto buffer = owner->take_buffer();
    auto attachments = IPC::release_raw_attachments(buffer.take_attachments());
    receive(context, buffer.data().span(), attachments.data(), attachments.size());
}

bool web_structured_serialize_validate_shared_memory(int descriptor, size_t size)
{
    auto file = IPC::File::clone_fd(descriptor);
    if (file.is_error())
        return false;
    auto buffer = Core::AnonymousBuffer::create_from_anon_fd(file.release_value().take_fd(), size);
    if (buffer.is_error())
        return false;
    return !buffer.value().validate_backing_size().is_error();
}

// Takes over the descriptors and the data holders.
SerializedTransferRecord* web_structured_serialize_transfer_record_create(ReadonlyBytes data, JSObject* const* shared_array_buffers, size_t shared_array_buffer_count, int const* shared_memory, size_t const* sizes, size_t shared_memory_count, int shared_store_lifeline, TransferDataEncoder* const* holders, size_t holder_count)
{
    auto* record = new SerializedTransferRecord;
    record->serialized = create_ipc_serialization_record(data, shared_array_buffers_from_abi(shared_array_buffers, shared_array_buffer_count));
    for (size_t i = 0; i < shared_memory_count; ++i)
        record->shared_buffers.append(MUST(Core::AnonymousBuffer::create_from_anon_fd(shared_memory[i], sizes[i])));
    record->shared_store_lifeline = SharedStoreLifeline::adopting_fd(shared_store_lifeline);
    for (size_t i = 0; i < holder_count; ++i) {
        record->transfer_data_holders.append(move(*holders[i]));
        delete holders[i];
    }
    return record;
}

void web_structured_serialize_transfer_record_destroy(SerializedTransferRecord* record)
{
    delete record;
}

// Rust roots the SharedArrayBuffers before this returns.
void web_structured_serialize_transfer_record_take(SerializedTransferRecord* record, void* context, void (*receive)(void*, ReadonlyBytes, JSObject* const*, size_t, int const*, size_t const*, size_t, int), void (*append_holder)(void*, TransferDataEncoder*))
{
    Vector<JSObject*> buffers;
    for (auto const& buffer : same_agent_shared_array_buffers(record->serialized))
        buffers.append(JS::HostABI::object_to_abi(buffer.ptr()));
    Vector<int> descriptors;
    Vector<size_t> sizes;
    for (auto const& buffer : record->shared_buffers) {
        descriptors.append(buffer.fd());
        sizes.append(buffer.size());
    }
    receive(context, record->serialized.data.span(), buffers.data(), buffers.size(), descriptors.data(), sizes.data(), descriptors.size(), record->shared_store_lifeline.fd());
    for (auto& holder : record->transfer_data_holders)
        append_holder(context, new TransferDataEncoder(move(holder)));
    record->shared_buffers.clear();
}

ReceivedImageBitmapSharedPixels* web_structured_serialize_shared_pixels_create(int const* descriptors, size_t const* sizes, size_t count)
{
    Vector<Core::AnonymousBuffer> buffers;
    for (size_t i = 0; i < count; ++i) {
        auto file = IPC::File::clone_fd(descriptors[i]);
        if (file.is_error())
            return nullptr;
        auto buffer = Core::AnonymousBuffer::create_from_anon_fd(file.release_value().take_fd(), sizes[i]);
        if (buffer.is_error())
            return nullptr;
        buffers.append(buffer.release_value());
    }
    auto* received_shared_pixels = new ReceivedImageBitmapSharedPixels { .buffers = move(buffers), .snapshots_of_buffers_without_resize_seals = {} };
    received_shared_pixels->snapshots_of_buffers_without_resize_seals.resize(count);
    return received_shared_pixels;
}

void web_structured_serialize_shared_pixels_destroy(ReceivedImageBitmapSharedPixels* received_shared_pixels)
{
    delete received_shared_pixels;
}

void web_structured_serialize_transferred_values_append(Vector<GC::Root<JS::Object>>* transferred_values, JSObject* value)
{
    transferred_values->append(GC::make_root(*JS::HostABI::object_from_abi(value)));
}
