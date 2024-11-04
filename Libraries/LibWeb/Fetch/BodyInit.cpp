/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/DOMURL/URLSearchParams.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/FormControlInfrastructure.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::Fetch {

// https://fetch.spec.whatwg.org/#bodyinit-safely-extract
Infrastructure::BodyWithType safely_extract_body(JS::Realm& realm, BodyInitOrReadableBytes const& object)
{
    // 1. If object is a ReadableStream object, then:
    if (auto const* stream = object.get_pointer<GC::Root<Streams::ReadableStream>>()) {
        // 1. Assert: object is neither disturbed nor locked.
        VERIFY(!((*stream)->is_disturbed() || (*stream)->is_locked()));
    }

    // 2. Return the result of extracting object.
    return MUST(extract_body(realm, object));
}

// https://fetch.spec.whatwg.org/#concept-bodyinit-extract
WebIDL::ExceptionOr<Infrastructure::BodyWithType> extract_body(JS::Realm& realm, BodyInitOrReadableBytes const& object, bool keepalive)
{
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    auto& vm = realm.vm();

    // 1. Let stream be null.
    GC::Ptr<Streams::ReadableStream> stream;

    // 2. If object is a ReadableStream object, then set stream to object.
    if (auto const* stream_handle = object.get_pointer<GC::Root<Streams::ReadableStream>>()) {
        stream = const_cast<Streams::ReadableStream*>(stream_handle->cell());
    }
    // 3. Otherwise, if object is a Blob object, set stream to the result of running object’s get stream.
    else if (auto const* blob_handle = object.get_pointer<GC::Root<FileAPI::Blob>>()) {
        stream = blob_handle->cell()->get_stream();
    }
    // 4. Otherwise, set stream to a new ReadableStream object, and set up stream with byte reading support.
    else {
        stream = realm.create<Streams::ReadableStream>(realm);
        Streams::set_up_readable_stream_controller_with_byte_reading_support(*stream);
    }

    // 5. Assert: stream is a ReadableStream object.
    VERIFY(stream);

    // 6. Let action be null.
    Function<ByteBuffer()> action;

    // 7. Let source be null.
    Infrastructure::Body::SourceType source {};

    // 8. Let length be null.
    Optional<u64> length {};

    // 9. Let type be null.
    Optional<ByteBuffer> type {};

    // 10. Switch on object.
    TRY(object.visit(
        [&](GC::Root<FileAPI::Blob> const& blob) -> WebIDL::ExceptionOr<void> {
            // Set source to object.
            source = blob;
            // Set length to object’s size.
            length = blob->size();
            // If object’s type attribute is not the empty byte sequence, set type to its value.
            if (!blob->type().is_empty())
                type = MUST(ByteBuffer::copy(blob->type().bytes()));
            return {};
        },
        [&](ReadonlyBytes bytes) -> WebIDL::ExceptionOr<void> {
            // Set source to object.
            source = MUST(ByteBuffer::copy(bytes));
            return {};
        },
        [&](GC::Root<WebIDL::BufferSource> const& buffer_source) -> WebIDL::ExceptionOr<void> {
            // Set source to a copy of the bytes held by object.
            source = MUST(WebIDL::get_buffer_source_copy(*buffer_source->raw_object()));
            return {};
        },
        [&](GC::Root<XHR::FormData> const& form_data) -> WebIDL::ExceptionOr<void> {
            // Set action to this step: run the multipart/form-data encoding algorithm, with object’s entry list and UTF-8.
            auto serialized_form_data = MUST(HTML::serialize_to_multipart_form_data(form_data->entry_list()));
            // Set source to object.
            source = serialized_form_data.serialized_data;
            // FIXME: Set length to unclear, see html/6424 for improving this.
            // Set type to `multipart/form-data; boundary=`, followed by the multipart/form-data boundary string generated by the multipart/form-data encoding algorithm.
            type = MUST(ByteBuffer::copy(MUST(String::formatted("multipart/form-data; boundary={}"sv, serialized_form_data.boundary)).bytes()));
            return {};
        },
        [&](GC::Root<DOMURL::URLSearchParams> const& url_search_params) -> WebIDL::ExceptionOr<void> {
            // Set source to the result of running the application/x-www-form-urlencoded serializer with object’s list.
            auto search_params_string = url_search_params->to_string();
            source = MUST(ByteBuffer::copy(search_params_string.bytes()));
            // Set type to `application/x-www-form-urlencoded;charset=UTF-8`.
            type = MUST(ByteBuffer::copy("application/x-www-form-urlencoded;charset=UTF-8"sv.bytes()));
            return {};
        },
        [&](String const& scalar_value_string) -> WebIDL::ExceptionOr<void> {
            // NOTE: AK::String is always UTF-8.
            // Set source to the UTF-8 encoding of object.
            source = MUST(ByteBuffer::copy(scalar_value_string.bytes()));
            // Set type to `text/plain;charset=UTF-8`.
            type = MUST(ByteBuffer::copy("text/plain;charset=UTF-8"sv.bytes()));
            return {};
        },
        [&](GC::Root<Streams::ReadableStream> const& stream) -> WebIDL::ExceptionOr<void> {
            // If keepalive is true, then throw a TypeError.
            if (keepalive)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot extract body from stream when keepalive is set"sv };

            // If object is disturbed or locked, then throw a TypeError.
            if (stream->is_disturbed() || stream->is_locked())
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot extract body from disturbed or locked stream"sv };

            return {};
        }));

    // 11. If source is a byte sequence, then set action to a step that returns source and length to source’s length.
    if (source.has<ByteBuffer>()) {
        action = [source = MUST(ByteBuffer::copy(source.get<ByteBuffer>()))]() mutable {
            return move(source);
        };
        length = source.get<ByteBuffer>().size();
    }

    // 12. If action is non-null, then run these steps in parallel:
    if (action) {
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, stream, action = move(action)] {
            HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. Run action.
            auto bytes = action();

            // Whenever one or more bytes are available and stream is not errored, enqueue the result of creating a
            // Uint8Array from the available bytes into stream.
            if (!bytes.is_empty() && !stream->is_errored()) {
                auto array_buffer = JS::ArrayBuffer::create(stream->realm(), move(bytes));
                auto chunk = JS::Uint8Array::create(stream->realm(), array_buffer->byte_length(), *array_buffer);

                Streams::readable_stream_enqueue(*stream->controller(), chunk).release_value_but_fixme_should_propagate_errors();
            }

            // When running action is done, close stream.
            stream->close();
        }));
    }

    // 13. Let body be a body whose stream is stream, source is source, and length is length.
    auto body = Infrastructure::Body::create(vm, *stream, move(source), move(length));

    // 14. Return (body, type).
    return Infrastructure::BodyWithType { .body = move(body), .type = move(type) };
}

}
