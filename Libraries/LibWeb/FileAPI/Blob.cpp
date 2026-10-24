/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::FileAPI {

// The bytes that C++ hands a Blob, which the Blob owns from then on.
struct BlobBytes {
    AK_ALLOC_WITH_KMALLOC;

    Variant<ByteBuffer, Core::AnonymousBuffer> bytes;
};

// The bytes as Bindings/Rust/src/file_api/blob.rs takes them over.
struct BlobBytesABI {
    BlobBytes* owner { nullptr };
    u8 const* data { nullptr };
    size_t length { 0 };
};

static BlobBytesABI hand_over(Variant<ByteBuffer, Core::AnonymousBuffer> bytes)
{
    auto* owner = new BlobBytes { move(bytes) };
    auto data = owner->bytes.visit([](auto const& bytes) { return bytes.bytes(); });
    return { .owner = owner, .data = data.data(), .length = data.size() };
}

extern "C" {

// Defined in Bindings/Rust/src/file_api/exports.rs.
void* web_blob_create(JSVM*, BlobBytesABI bytes, FlatPtr type);
u8 const* web_blob_byte_sequence(void* blob, size_t* out_length);
BlobBytes* web_blob_foreign_bytes_owner(void* blob);
void web_blob_set_foreign_bytes(void* blob, BlobBytesABI bytes);
void* web_blob_slice(JSVM*, void* blob, bool has_start, i64 start, bool has_end, i64 end, bool has_content_type, FlatPtr content_type);
FlatPtr web_blob_type(void* blob);
bool web_blob_is_file(void* blob);

// Called by Bindings/Rust/src/file_api/blob.rs.
void web_blob_bytes_destroy(BlobBytes*);
JSObject* web_blob_get_stream(void* realm, ReadonlyBytes);
WebIDL::Promise* web_blob_text(void* realm, ReadonlyBytes);
WebIDL::Promise* web_blob_array_buffer(void* realm, ReadonlyBytes);
WebIDL::Promise* web_blob_bytes(void* realm, ReadonlyBytes);
JSCompletion web_blob_deserialize_byte_sequence(Bindings::RustSerializationReader*, BlobBytesABI* out_bytes);
}

GC::Ref<Blob> Blob::create(ByteBuffer bytes, Utf16String type)
{
    return GC::Ref { *static_cast<Blob*>(web_blob_create(JS::HostABI::vm_to_abi(JS::VM::the()), hand_over(move(bytes)), move(type).into_raw())) };
}

GC::Ref<Blob> Blob::create(Core::AnonymousBuffer bytes, Utf16String type)
{
    return GC::Ref { *static_cast<Blob*>(web_blob_create(JS::HostABI::vm_to_abi(JS::VM::the()), hand_over(move(bytes)), move(type).into_raw())) };
}

Utf16String Blob::type() const
{
    return Utf16String::from_raw(web_blob_type(const_cast<Blob*>(this)));
}

ReadonlyBytes Blob::raw_bytes() const
{
    size_t length = 0;
    auto const* data = web_blob_byte_sequence(const_cast<Blob*>(this), &length);
    return { data, length };
}

ErrorOr<Core::AnonymousBuffer> Blob::shared_bytes()
{
    if (auto* owner = web_blob_foreign_bytes_owner(this); owner && owner->bytes.has<Core::AnonymousBuffer>())
        return owner->bytes.get<Core::AnonymousBuffer>();

    auto bytes = raw_bytes();
    Core::AnonymousBuffer buffer;
    if (!bytes.is_empty()) {
        buffer = TRY(Core::AnonymousBuffer::create_with_size(bytes.size()));
        bytes.copy_to({ buffer.data<u8>(), buffer.size() });
    }
    web_blob_set_foreign_bytes(this, hand_over(buffer));
    return buffer;
}

GC::Ref<Blob> Blob::slice_blob(Optional<i64> start, Optional<i64> end, Optional<Utf16String> content_type) const
{
    auto has_content_type = content_type.has_value();
    auto raw_content_type = has_content_type ? content_type.release_value().into_raw() : 0;
    auto* sliced_blob = web_blob_slice(JS::HostABI::vm_to_abi(JS::VM::the()), const_cast<Blob*>(this), start.has_value(), start.value_or(0), end.has_value(), end.value_or(0), has_content_type, raw_content_type);
    return GC::Ref { *static_cast<Blob*>(sliced_blob) };
}

bool Blob::is_file() const
{
    return web_blob_is_file(const_cast<Blob*>(this));
}

// https://w3c.github.io/FileAPI/#blob-get-stream
static GC::Ref<Streams::ReadableStream> get_stream(JS::Realm& realm, ReadonlyBytes blob_bytes)
{
    // 1. Let stream be a new ReadableStream created in blob’s relevant Realm.
    auto stream = GC::Heap::the().allocate<Streams::ReadableStream>();

    // 2. Set up stream with byte reading support.
    stream->set_up_with_byte_reading_support(realm);

    // FIXME: 3. Run the following steps in parallel:
    {
        // 1. While not all bytes of blob have been read:
        //    NOTE: for simplicity the chunk is the entire buffer for now.
        {
            // 1. Let bytes be the byte sequence that results from reading a chunk from blob, or failure if a chunk cannot be read.
            auto bytes = MUST(ByteBuffer::copy(blob_bytes));

            // 2. Queue a global task on the file reading task source given blob’s relevant global object to perform the following steps:
            HTML::queue_global_task(HTML::Task::Source::FileReading, realm.global_object(), GC::create_function(GC::Heap::the(), [&realm, stream, bytes = move(bytes)]() {
                HTML::TemporaryExecutionContext const execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. If bytes is failure, then error stream with a failure reason and abort these steps.
                // 2. Let chunk be a new Uint8Array wrapping an ArrayBuffer containing bytes. If creating the ArrayBuffer throws an exception, then error stream with that exception and abort these steps.
                auto array_buffer = JS::ArrayBuffer::create(realm, bytes);
                auto chunk = JS::Uint8Array::create(realm, bytes.size(), *array_buffer);

                // 3. Enqueue chunk in stream.
                auto maybe_error = WebIDL::throw_dom_exception_if_needed(realm.vm(), realm, [&]() {
                    return stream->enqueue(chunk);
                });

                if (maybe_error.is_error()) {
                    Streams::readable_stream_error(*stream, maybe_error.release_error().value());
                    return;
                }

                // FIXME: Spec bug: https://github.com/w3c/FileAPI/issues/206
                //
                // We need to close the stream so that the stream will finish reading.
                stream->close();
            }));
        }
    }

    // 4. Return stream.
    return stream;
}

GC::Ref<Streams::ReadableStream> Blob::get_stream(JS::Realm& realm) const
{
    return FileAPI::get_stream(realm, raw_bytes());
}

static GC::Ref<WebIDL::Promise> read_all_blob_bytes(JS::Realm& realm, ReadonlyBytes blob_bytes)
{
    auto stream = get_stream(realm, blob_bytes);
    auto reader_or_exception = stream->get_a_reader();
    if (reader_or_exception.is_exception())
        return WebIDL::create_rejected_promise_from_exception(realm, reader_or_exception.release_error());
    return reader_or_exception.release_value()->read_all_bytes_deprecated();
}

// https://w3c.github.io/FileAPI/#dom-blob-stream
JSObject* web_blob_get_stream(void* realm, ReadonlyBytes blob_bytes)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);

    // The stream() method, when invoked, must return the result of calling get stream on this.
    auto stream = get_stream(js_realm, blob_bytes);
    return JS::HostABI::object_to_abi(Bindings::wrap(Bindings::host_defined_wrapper_world(js_realm), js_realm, stream).ptr());
}

// https://w3c.github.io/FileAPI/#dom-blob-text
WebIDL::Promise* web_blob_text(void* realm, ReadonlyBytes blob_bytes)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto& vm = js_realm.vm();

    auto promise = read_all_blob_bytes(js_realm, blob_bytes);
    promise = WebIDL::upon_fulfillment(*promise, GC::create_function(GC::Heap::the(), [&vm](JS::Value first_argument) -> WebIDL::ExceptionOr<JS::Value> {
        auto const& object = first_argument.as_object();
        VERIFY(is<JS::ArrayBuffer>(object));
        auto const& array_buffer = static_cast<JS::ArrayBuffer const&>(object);

        auto text = TRY_OR_THROW_OOM(vm, TextCodec::utf8_decode_to_utf16(ReadonlyBytes { array_buffer.data_at(0), array_buffer.byte_length() }));
        return JS::PrimitiveString::create(vm, move(text));
    }));
    return promise.ptr();
}

// https://w3c.github.io/FileAPI/#dom-blob-arraybuffer
WebIDL::Promise* web_blob_array_buffer(void* realm, ReadonlyBytes blob_bytes)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);

    auto promise = read_all_blob_bytes(js_realm, blob_bytes);
    promise = WebIDL::upon_fulfillment(*promise, GC::create_function(GC::Heap::the(), [&js_realm](JS::Value first_argument) -> WebIDL::ExceptionOr<JS::Value> {
        auto const& object = first_argument.as_object();
        VERIFY(is<JS::ArrayBuffer>(object));
        auto const& array_buffer = static_cast<JS::ArrayBuffer const&>(object);

        return JS::ArrayBuffer::create(js_realm, MUST(ByteBuffer::copy(ReadonlyBytes { array_buffer.data_at(0), array_buffer.byte_length() })));
    }));
    return promise.ptr();
}

// https://w3c.github.io/FileAPI/#dom-blob-bytes
WebIDL::Promise* web_blob_bytes(void* realm, ReadonlyBytes blob_bytes)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);

    auto promise = read_all_blob_bytes(js_realm, blob_bytes);
    promise = WebIDL::upon_fulfillment(*promise, GC::create_function(GC::Heap::the(), [&js_realm](JS::Value first_argument) -> WebIDL::ExceptionOr<JS::Value> {
        auto& object = first_argument.as_object();
        VERIFY(is<JS::ArrayBuffer>(object));
        auto& array_buffer = static_cast<JS::ArrayBuffer&>(object);
        return JS::Uint8Array::create(js_realm, array_buffer.byte_length(), array_buffer);
    }));
    return promise.ptr();
}

void web_blob_bytes_destroy(BlobBytes* bytes)
{
    delete bytes;
}

JSCompletion web_blob_deserialize_byte_sequence(Bindings::RustSerializationReader* reader, BlobBytesABI* out_bytes)
{
    auto bytes = HTML::decode_or_throw_data_clone_error<ByteBuffer>(reader->realm, reader->serialized);
    if (bytes.is_exception())
        return Bindings::completion_to_abi(reader->realm, bytes.exception());
    *out_bytes = hand_over(bytes.release_value());
    return Bindings::completion_to_abi(reader->realm, JS::js_undefined());
}

}
