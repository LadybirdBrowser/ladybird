/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Base64.h>
#include <AK/ByteBuffer.h>
#include <AK/Time.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/FileReaderPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/FileReader.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::FileAPI {

JS_DEFINE_ALLOCATOR(FileReader);

FileReader::~FileReader() = default;

FileReader::FileReader(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void FileReader::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FileReader);
}

void FileReader::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_error);
}

JS::NonnullGCPtr<FileReader> FileReader::create(JS::Realm& realm)
{
    return realm.heap().allocate<FileReader>(realm, realm);
}

JS::NonnullGCPtr<FileReader> FileReader::construct_impl(JS::Realm& realm)
{
    return FileReader::create(realm);
}

// https://w3c.github.io/FileAPI/#blob-package-data
WebIDL::ExceptionOr<FileReader::Result> FileReader::blob_package_data(JS::Realm& realm, ByteBuffer bytes, Type type, Optional<String> const& mime_type, Optional<String> const& encoding_name)
{
    // A Blob has an associated package data algorithm, given bytes, a type, a optional mimeType, and a optional encodingName, which switches on type and runs the associated steps:
    switch (type) {
    case Type::DataURL:
        // Return bytes as a DataURL [RFC2397] subject to the considerations below:
        // Use mimeType as part of the Data URL if it is available in keeping with the Data URL specification [RFC2397].
        // If mimeType is not available return a Data URL without a media-type. [RFC2397].
        return MUST(URL::create_with_data(mime_type.value_or(String {}), MUST(encode_base64(bytes)), true).to_string());
    case Type::Text: {
        // 1. Let encoding be failure.
        Optional<StringView> encoding;

        // 2. If the encodingName is present, set encoding to the result of getting an encoding from encodingName.
        if (encoding_name.has_value())
            encoding = TextCodec::get_standardized_encoding(encoding_name.value());

        // 3. If encoding is failure, and mimeType is present:
        if (!encoding.has_value() && mime_type.has_value()) {
            // 1. Let type be the result of parse a MIME type given mimeType.
            auto maybe_type = MimeSniff::MimeType::parse(mime_type.value());

            // 2. If type is not failure, set encoding to the result of getting an encoding from type’s parameters["charset"].
            if (maybe_type.has_value()) {
                auto const& type = maybe_type.value();
                auto it = type.parameters().find("charset"sv);
                if (it != type.parameters().end())
                    encoding = TextCodec::get_standardized_encoding(it->value);
            }
        }

        // 4. If encoding is failure, then set encoding to UTF-8.
        // 5. Decode bytes using fallback encoding encoding, and return the result.
        auto decoder = TextCodec::decoder_for(encoding.value_or("UTF-8"sv));
        VERIFY(decoder.has_value());
        return TRY_OR_THROW_OOM(realm.vm(), convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(decoder.value(), bytes));
    }
    case Type::ArrayBuffer:
        // Return a new ArrayBuffer whose contents are bytes.
        return JS::ArrayBuffer::create(realm, move(bytes));
    case Type::BinaryString:
        // FIXME: Return bytes as a binary string, in which every byte is represented by a code unit of equal value [0..255].
        return WebIDL::NotSupportedError::create(realm, "BinaryString not supported yet"_string);
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/FileAPI/#readOperation
WebIDL::ExceptionOr<void> FileReader::read_operation(Blob& blob, Type type, Optional<String> const& encoding_name)
{
    auto& realm = this->realm();
    auto const blobs_type = blob.type();

    // 1. If fr’s state is "loading", throw an InvalidStateError DOMException.
    if (m_state == State::Loading)
        return WebIDL::InvalidStateError::create(realm, "Read already in progress"_string);

    // 2. Set fr’s state to "loading".
    m_state = State::Loading;

    // 3. Set fr’s result to null.
    m_result = {};

    // 4. Set fr’s error to null.
    m_error = {};

    // 5. Let stream be the result of calling get stream on blob.
    auto stream = blob.get_stream();

    // 6. Let reader be the result of getting a reader from stream.
    auto reader = TRY(acquire_readable_stream_default_reader(*stream));

    // 7. Let bytes be an empty byte sequence.
    ByteBuffer bytes;

    // 8. Let chunkPromise be the result of reading a chunk from stream with reader.
    auto chunk_promise = reader->read();

    // 9. Let isFirstChunk be true.
    bool is_first_chunk = true;

    // 10. In parallel, while true:
    Platform::EventLoopPlugin::the().deferred_invoke([this, chunk_promise, reader, bytes, is_first_chunk, &realm, type, encoding_name, blobs_type]() mutable {
        HTML::TemporaryExecutionContext execution_context { Bindings::host_defined_environment_settings_object(realm) };
        Optional<MonotonicTime> progress_timer;

        while (true) {
            auto& vm = realm.vm();

            // 1. Wait for chunkPromise to be fulfilled or rejected.
            Platform::EventLoopPlugin::the().spin_until([&]() {
                return chunk_promise->state() == JS::Promise::State::Fulfilled || chunk_promise->state() == JS::Promise::State::Rejected;
            });

            // 2. If chunkPromise is fulfilled, and isFirstChunk is true, queue a task to fire a progress event called loadstart at fr.
            // NOTE: ISSUE 2 We might change loadstart to be dispatched synchronously, to align with XMLHttpRequest behavior. [Issue #119]
            if (chunk_promise->state() == JS::Promise::State::Fulfilled && is_first_chunk) {
                HTML::queue_global_task(HTML::Task::Source::FileReading, realm.global_object(), JS::create_heap_function(heap(), [this, &realm]() {
                    dispatch_event(DOM::Event::create(realm, HTML::EventNames::loadstart));
                }));
            }

            // 3. Set isFirstChunk to false.
            is_first_chunk = false;

            VERIFY(chunk_promise->result().is_object());
            auto& result = chunk_promise->result().as_object();

            auto value = MUST(result.get(vm.names.value));
            auto done = MUST(result.get(vm.names.done));

            // 4. If chunkPromise is fulfilled with an object whose done property is false and whose value property is a Uint8Array object, run these steps:
            if (chunk_promise->state() == JS::Promise::State::Fulfilled && !done.as_bool() && is<JS::Uint8Array>(value.as_object())) {
                // 1. Let bs be the byte sequence represented by the Uint8Array object.
                auto const& byte_sequence = verify_cast<JS::Uint8Array>(value.as_object());

                // 2. Append bs to bytes.
                bytes.append(byte_sequence.data());

                // 3. If roughly 50ms have passed since these steps were last invoked, queue a task to fire a progress event called progress at fr.
                auto now = MonotonicTime::now();
                bool enough_time_passed = !progress_timer.has_value() || (now - progress_timer.value() >= AK::Duration::from_milliseconds(50));
                // WPT tests for this and expects no progress event to fire when there isn't any data.
                // See http://wpt.live/FileAPI/reading-data-section/filereader_events.any.html
                bool contained_data = byte_sequence.array_length().length() > 0;
                if (enough_time_passed && contained_data) {
                    HTML::queue_global_task(HTML::Task::Source::FileReading, realm.global_object(), JS::create_heap_function(heap(), [this, &realm]() {
                        dispatch_event(DOM::Event::create(realm, HTML::EventNames::progress));
                    }));
                    progress_timer = now;
                }

                // 4. Set chunkPromise to the result of reading a chunk from stream with reader.
                chunk_promise = reader->read();
            }
            // 5. Otherwise, if chunkPromise is fulfilled with an object whose done property is true, queue a task to run the following steps and abort this algorithm:
            else if (chunk_promise->state() == JS::Promise::State::Fulfilled && done.as_bool()) {
                HTML::queue_global_task(HTML::Task::Source::FileReading, realm.global_object(), JS::create_heap_function(heap(), [this, bytes, type, &realm, encoding_name, blobs_type]() {
                    // 1. Set fr’s state to "done".
                    m_state = State::Done;

                    // 2. Let result be the result of package data given bytes, type, blob’s type, and encodingName.
                    auto result = blob_package_data(realm, bytes, type, blobs_type, encoding_name);

                    // 3. If package data threw an exception error:
                    if (result.is_error()) {
                        // FIXME: 1. Set fr’s error to error.

                        // 2. Fire a progress event called error at fr.
                        dispatch_event(DOM::Event::create(realm, HTML::EventNames::error));
                    }
                    // 4. Else:
                    else {
                        // 1. Set fr’s result to result.
                        m_result = result.release_value();

                        // 2. Fire a progress event called load at the fr.
                        dispatch_event(DOM::Event::create(realm, HTML::EventNames::load));
                    }

                    // 5. If fr’s state is not "loading", fire a progress event called loadend at the fr.
                    if (m_state != State::Loading)
                        dispatch_event(DOM::Event::create(realm, HTML::EventNames::loadend));

                    // NOTE: Event handler for the load or error events could have started another load, if that happens the loadend event for this load is not fired.
                }));

                return;
            }
            // 6. Otherwise, if chunkPromise is rejected with an error error, queue a task to run the following steps and abort this algorithm:
            else if (chunk_promise->state() == JS::Promise::State::Rejected) {
                HTML::queue_global_task(HTML::Task::Source::FileReading, realm.global_object(), JS::create_heap_function(heap(), [this, &realm]() {
                    // 1. Set fr’s state to "done".
                    m_state = State::Done;

                    // FIXME: 2. Set fr’s error to error.

                    // 5. Fire a progress event called error at fr.
                    dispatch_event(DOM::Event::create(realm, HTML::EventNames::loadend));

                    // 4. If fr’s state is not "loading", fire a progress event called loadend at fr.
                    if (m_state != State::Loading)
                        dispatch_event(DOM::Event::create(realm, HTML::EventNames::loadend));

                    // 5. Note: Event handler for the error event could have started another load, if that happens the loadend event for this load is not fired.
                }));
            }
        }
    });

    return {};
}

// https://w3c.github.io/FileAPI/#dfn-readAsDataURL
WebIDL::ExceptionOr<void> FileReader::read_as_data_url(Blob& blob)
{
    // The readAsDataURL(blob) method, when invoked, must initiate a read operation for blob with DataURL.
    return read_operation(blob, Type::DataURL);
}

// https://w3c.github.io/FileAPI/#dfn-readAsText
WebIDL::ExceptionOr<void> FileReader::read_as_text(Blob& blob, Optional<String> const& encoding)
{
    // The readAsText(blob, encoding) method, when invoked, must initiate a read operation for blob with Text and encoding.
    return read_operation(blob, Type::Text, encoding);
}

// https://w3c.github.io/FileAPI/#dfn-readAsArrayBuffer
WebIDL::ExceptionOr<void> FileReader::read_as_array_buffer(Blob& blob)
{
    // The readAsArrayBuffer(blob) method, when invoked, must initiate a read operation for blob with ArrayBuffer.
    return read_operation(blob, Type::ArrayBuffer);
}

// https://w3c.github.io/FileAPI/#dfn-readAsBinaryString
WebIDL::ExceptionOr<void> FileReader::read_as_binary_string(Blob& blob)
{
    // The readAsBinaryString(blob) method, when invoked, must initiate a read operation for blob with BinaryString.
    // NOTE: The use of readAsArrayBuffer() is preferred over readAsBinaryString(), which is provided for backwards compatibility.
    return read_operation(blob, Type::BinaryString);
}

// https://w3c.github.io/FileAPI/#dfn-abort
void FileReader::abort()
{
    auto& realm = this->realm();

    // 1. If this's state is "empty" or if this's state is "done" set this's result to null and terminate this algorithm.
    if (m_state == State::Empty || m_state == State::Done) {
        m_result = {};
        return;
    }

    // 2. If this's state is "loading" set this's state to "done" and set this's result to null.
    if (m_state == State::Loading) {
        m_state = State::Done;
        m_result = {};
    }

    // FIXME: 3. If there are any tasks from this on the file reading task source in an affiliated task queue, then remove those tasks from that task queue.

    // FIXME: 4. Terminate the algorithm for the read method being processed.

    // 5. Fire a progress event called abort at this.
    dispatch_event(DOM::Event::create(realm, HTML::EventNames::abort));

    // 6. If this's state is not "loading", fire a progress event called loadend at this.
    if (m_state != State::Loading)
        dispatch_event(DOM::Event::create(realm, HTML::EventNames::loadend));
}

void FileReader::set_onloadstart(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::loadstart, value);
}

WebIDL::CallbackType* FileReader::onloadstart()
{
    return event_handler_attribute(HTML::EventNames::loadstart);
}

void FileReader::set_onprogress(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::progress, value);
}

WebIDL::CallbackType* FileReader::onprogress()
{
    return event_handler_attribute(HTML::EventNames::progress);
}

void FileReader::set_onload(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::load, value);
}

WebIDL::CallbackType* FileReader::onload()
{
    return event_handler_attribute(HTML::EventNames::load);
}

void FileReader::set_onabort(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::abort, value);
}

WebIDL::CallbackType* FileReader::onabort()
{
    return event_handler_attribute(HTML::EventNames::abort);
}

void FileReader::set_onerror(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::error, value);
}

WebIDL::CallbackType* FileReader::onerror()
{
    return event_handler_attribute(HTML::EventNames::error);
}

void FileReader::set_onloadend(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::loadend, value);
}

WebIDL::CallbackType* FileReader::onloadend()
{
    return event_handler_attribute(HTML::EventNames::loadend);
}

}
