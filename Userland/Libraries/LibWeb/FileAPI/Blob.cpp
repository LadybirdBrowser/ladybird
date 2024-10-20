/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/BlobPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::FileAPI {

JS_DEFINE_ALLOCATOR(Blob);

JS::NonnullGCPtr<Blob> Blob::create(JS::Realm& realm, ByteBuffer byte_buffer, String type)
{
    return realm.heap().allocate<Blob>(realm, realm, move(byte_buffer), move(type));
}

// https://w3c.github.io/FileAPI/#convert-line-endings-to-native
ErrorOr<String> convert_line_endings_to_native(StringView string)
{
    // 1. Let native line ending be be the code point U+000A LF.
    auto native_line_ending = "\n"sv;

    // 2. If the underlying platform’s conventions are to represent newlines as a carriage return and line feed sequence, set native line ending to the code point U+000D CR followed by the code point U+000A LF.
    // NOTE: this step is a no-op since LibWeb does not compile on Windows, which is the only platform we know of that that uses a carriage return and line feed sequence for line endings.

    // 3. Set result to the empty string.
    StringBuilder result;

    // 4. Let position be a position variable for s, initially pointing at the start of s.
    auto lexer = GenericLexer { string };

    // 5. Let token be the result of collecting a sequence of code points that are not equal to U+000A LF or U+000D CR from s given position.
    // 6. Append token to result.
    TRY(result.try_append(lexer.consume_until(is_any_of("\n\r"sv))));

    // 7. While position is not past the end of s:
    while (!lexer.is_eof()) {
        // 1. If the code point at position within s equals U+000D CR:
        if (lexer.peek() == '\r') {
            // 1. Append native line ending to result.
            TRY(result.try_append(native_line_ending));

            // 2. Advance position by 1.
            lexer.ignore(1);

            // 3. If position is not past the end of s and the code point at position within s equals U+000A LF advance position by 1.
            if (!lexer.is_eof() && lexer.peek() == '\n')
                lexer.ignore(1);
        }
        // 2. Otherwise if the code point at position within s equals U+000A LF, advance position by 1 and append native line ending to result.
        else if (lexer.peek() == '\n') {
            lexer.ignore(1);
            TRY(result.try_append(native_line_ending));
        }

        // 3. Let token be the result of collecting a sequence of code points that are not equal to U+000A LF or U+000D CR from s given position.
        // 4. Append token to result.
        TRY(result.try_append(lexer.consume_until(is_any_of("\n\r"sv))));
    }
    // 5. Return result.
    return result.to_string();
}

// https://w3c.github.io/FileAPI/#process-blob-parts
ErrorOr<ByteBuffer> process_blob_parts(Vector<BlobPart> const& blob_parts, Optional<BlobPropertyBag> const& options)
{
    // 1. Let bytes be an empty sequence of bytes.
    ByteBuffer bytes {};

    // 2. For each element in parts:
    for (auto const& blob_part : blob_parts) {
        TRY(blob_part.visit(
            // 1. If element is a USVString, run the following sub-steps:
            [&](String const& string) -> ErrorOr<void> {
                // 1. Let s be element.
                auto s = string;

                // 2. If the endings member of options is "native", set s to the result of converting line endings to native of element.
                if (options.has_value() && options->endings == Bindings::EndingType::Native)
                    s = TRY(convert_line_endings_to_native(s));

                // NOTE: The AK::String is always UTF-8.
                // 3. Append the result of UTF-8 encoding s to bytes.
                return bytes.try_append(s.bytes());
            },
            // 2. If element is a BufferSource, get a copy of the bytes held by the buffer source, and append those bytes to bytes.
            [&](JS::Handle<WebIDL::BufferSource> const& buffer_source) -> ErrorOr<void> {
                auto data_buffer = TRY(WebIDL::get_buffer_source_copy(*buffer_source->raw_object()));
                return bytes.try_append(data_buffer.bytes());
            },
            // 3. If element is a Blob, append the bytes it represents to bytes.
            [&](JS::Handle<Blob> const& blob) -> ErrorOr<void> {
                return bytes.try_append(blob->raw_bytes());
            }));
    }
    // 3. Return bytes.
    return bytes;
}

bool is_basic_latin(StringView view)
{
    for (auto code_point : view) {
        if (code_point < 0x0020 || code_point > 0x007E)
            return false;
    }
    return true;
}

Blob::Blob(JS::Realm& realm)
    : PlatformObject(realm)
{
}

Blob::Blob(JS::Realm& realm, ByteBuffer byte_buffer, String type)
    : PlatformObject(realm)
    , m_byte_buffer(move(byte_buffer))
    , m_type(move(type))
{
}

Blob::Blob(JS::Realm& realm, ByteBuffer byte_buffer)
    : PlatformObject(realm)
    , m_byte_buffer(move(byte_buffer))
{
}

Blob::~Blob() = default;

void Blob::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Blob);
}

WebIDL::ExceptionOr<void> Blob::serialization_steps(HTML::SerializationRecord& record, bool, HTML::SerializationMemory&)
{
    auto& vm = this->vm();

    //  FIXME: 1. Set serialized.[[SnapshotState]] to value’s snapshot state.

    // NON-STANDARD: FileAPI spec doesn't specify that type should be serialized, although
    //               to be conformant with other browsers this needs to be serialized.
    TRY(HTML::serialize_string(vm, record, m_type));

    // 2. Set serialized.[[ByteSequence]] to value’s underlying byte sequence.
    TRY(HTML::serialize_bytes(vm, record, m_byte_buffer.bytes()));

    return {};
}

WebIDL::ExceptionOr<void> Blob::deserialization_steps(ReadonlySpan<u32> const& record, size_t& position, HTML::DeserializationMemory&)
{
    auto& vm = this->vm();

    // FIXME: 1. Set value’s snapshot state to serialized.[[SnapshotState]].

    // NON-STANDARD: FileAPI spec doesn't specify that type should be deserialized, although
    //               to be conformant with other browsers this needs to be deserialized.
    m_type = TRY(HTML::deserialize_string(vm, record, position));

    // 2. Set value’s underlying byte sequence to serialized.[[ByteSequence]].
    m_byte_buffer = TRY(HTML::deserialize_bytes(vm, record, position));

    return {};
}

// https://w3c.github.io/FileAPI/#ref-for-dom-blob-blob
JS::NonnullGCPtr<Blob> Blob::create(JS::Realm& realm, Optional<Vector<BlobPart>> const& blob_parts, Optional<BlobPropertyBag> const& options)
{
    // 1. If invoked with zero parameters, return a new Blob object consisting of 0 bytes, with size set to 0, and with type set to the empty string.
    if (!blob_parts.has_value() && !options.has_value())
        return realm.heap().allocate<Blob>(realm, realm);

    ByteBuffer byte_buffer {};
    // 2. Let bytes be the result of processing blob parts given blobParts and options.
    if (blob_parts.has_value()) {
        byte_buffer = MUST(process_blob_parts(blob_parts.value(), options));
    }

    auto type = String {};
    // 3. If the type member of the options argument is not the empty string, run the following sub-steps:
    if (options.has_value() && !options->type.is_empty()) {
        // FIXME: 1. If the type member is provided and is not the empty string, let t be set to the type dictionary member.
        //    If t contains any characters outside the range U+0020 to U+007E, then set t to the empty string and return from these substeps.
        // FIXME: 2. Convert every character in t to ASCII lowercase.

        // NOTE: The spec is out of date, and we are supposed to call into the MimeType parser here.
        if (!options->type.is_empty()) {
            auto maybe_parsed_type = Web::MimeSniff::MimeType::parse(options->type);

            if (maybe_parsed_type.has_value())
                type = maybe_parsed_type->serialized();
        }
    }

    // 4. Return a Blob object referring to bytes as its associated byte sequence, with its size set to the length of bytes, and its type set to the value of t from the substeps above.
    return realm.heap().allocate<Blob>(realm, realm, move(byte_buffer), move(type));
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<Blob>> Blob::construct_impl(JS::Realm& realm, Optional<Vector<BlobPart>> const& blob_parts, Optional<BlobPropertyBag> const& options)
{
    return Blob::create(realm, blob_parts, options);
}

// https://w3c.github.io/FileAPI/#dfn-slice
WebIDL::ExceptionOr<JS::NonnullGCPtr<Blob>> Blob::slice(Optional<i64> start, Optional<i64> end, Optional<String> const& content_type)
{
    // 1. Let sliceStart, sliceEnd, and sliceContentType be null.
    // 2. If start is given, set sliceStart to start.
    // 3. If end is given, set sliceEnd to end.
    // 3. If contentType is given, set sliceContentType to contentType.
    // 4. Return the result of slice blob given this, sliceStart, sliceEnd, and sliceContentType.
    return slice_blob(start, end, content_type);
}

// https://w3c.github.io/FileAPI/#slice-blob
WebIDL::ExceptionOr<JS::NonnullGCPtr<Blob>> Blob::slice_blob(Optional<i64> start, Optional<i64> end, Optional<String> const& content_type)
{
    auto& vm = realm().vm();

    // 1. Let originalSize be blob’s size.
    auto original_size = size();

    // 2. The start parameter, if non-null, is a value for the start point of a slice blob call, and must be treated as a byte-order position,
    //    with the zeroth position representing the first byte. User agents must normalize start according to the following:
    i64 relative_start;
    if (!start.has_value()) {
        // a. If start is null, let relativeStart be 0.
        relative_start = 0;
    } else {
        auto start_value = start.value();

        // b. If start is negative, let relativeStart be max((originalSize + start), 0).
        if (start_value < 0) {
            relative_start = max((static_cast<i64>(original_size) + start_value), 0);
        }
        // c. Otherwise, let relativeStart be min(start, originalSize).
        else {
            relative_start = min(start_value, original_size);
        }
    }

    // 3. The end parameter, if non-null. is a value for the end point of a slice blob call. User agents must normalize end according to the following:
    i64 relative_end;
    if (!end.has_value()) {
        // a. If end is null, let relativeEnd be originalSize.
        relative_end = original_size;
    } else {
        auto end_value = end.value();

        // b. If end is negative, let relativeEnd be max((originalSize + end), 0).
        if (end_value < 0) {
            relative_end = max((static_cast<i64>(original_size) + end_value), 0);
        }
        // c. Otherwise, let relativeEnd be min(end, originalSize).
        else {
            relative_end = min(end_value, original_size);
        }
    }

    // 4. The contentType parameter, if non-null, is used to set the ASCII-encoded string in lower case representing the media type of the Blob.
    //    User agents must normalize contentType according to the following:
    String relative_content_type;
    if (!content_type.has_value()) {
        // a. If contentType is null, let relativeContentType be set to the empty string.
        relative_content_type = {};
    } else {
        // b. Otherwise, let relativeContentType be set to contentType and run the substeps below:

        // 1. If relativeContentType contains any characters outside the range of U+0020 to U+007E, then set relativeContentType to the empty string
        //    and return from these substeps:
        if (!is_basic_latin(content_type.value())) {
            relative_content_type = {};
        }
        // 2. Convert every character in relativeContentType to ASCII lowercase.
        else {
            relative_content_type = content_type.value().to_ascii_lowercase();
        }
    }

    // 5. Let span be max((relativeEnd - relativeStart), 0).
    auto span = max((relative_end - relative_start), 0);

    // 6. Return a new Blob object S with the following characteristics:
    // a. S refers to span consecutive bytes from blob’s associated byte sequence, beginning with the byte at byte-order position relativeStart.
    // b. S.size = span.
    // c. S.type = relativeContentType.
    auto byte_buffer = TRY_OR_THROW_OOM(vm, m_byte_buffer.slice(relative_start, span));
    return heap().allocate<Blob>(realm(), realm(), move(byte_buffer), move(relative_content_type));
}

// https://w3c.github.io/FileAPI/#dom-blob-stream
JS::NonnullGCPtr<Streams::ReadableStream> Blob::stream()
{
    // The stream() method, when invoked, must return the result of calling get stream on this.
    return get_stream();
}

// https://w3c.github.io/FileAPI/#blob-get-stream
JS::NonnullGCPtr<Streams::ReadableStream> Blob::get_stream()
{
    auto& realm = this->realm();

    // 1. Let stream be a new ReadableStream created in blob’s relevant Realm.
    auto stream = realm.heap().allocate<Streams::ReadableStream>(realm, realm);

    // 2. Set up stream with byte reading support.
    set_up_readable_stream_controller_with_byte_reading_support(stream);

    // FIXME: 3. Run the following steps in parallel:
    {
        // 1. While not all bytes of blob have been read:
        //    NOTE: for simplicity the chunk is the entire buffer for now.
        {
            // 1. Let bytes be the byte sequence that results from reading a chunk from blob, or failure if a chunk cannot be read.
            auto bytes = m_byte_buffer;

            // 2. Queue a global task on the file reading task source given blob’s relevant global object to perform the following steps:
            HTML::queue_global_task(HTML::Task::Source::FileReading, realm.global_object(), JS::create_heap_function(heap(), [stream, bytes = move(bytes)]() {
                // NOTE: Using an TemporaryExecutionContext here results in a crash in the method HTML::incumbent_settings_object()
                //       since we end up in a state where we have no execution context + an event loop with an empty incumbent
                //       settings object stack. We still need an execution context therefore we push the realm's execution context
                //       onto the realm's VM, and we need an incumbent settings object which is pushed onto the incumbent settings
                //       object stack by EnvironmentSettings::prepare_to_run_callback().
                auto& realm = stream->realm();
                auto& environment_settings = Bindings::host_defined_environment_settings_object(realm);
                realm.vm().push_execution_context(environment_settings.realm_execution_context());
                environment_settings.prepare_to_run_callback();
                ScopeGuard const guard = [&environment_settings, &realm] {
                    environment_settings.clean_up_after_running_callback();
                    realm.vm().pop_execution_context();
                };

                // 1. If bytes is failure, then error stream with a failure reason and abort these steps.
                // 2. Let chunk be a new Uint8Array wrapping an ArrayBuffer containing bytes. If creating the ArrayBuffer throws an exception, then error stream with that exception and abort these steps.
                auto array_buffer = JS::ArrayBuffer::create(stream->realm(), bytes);
                auto chunk = JS::Uint8Array::create(stream->realm(), bytes.size(), *array_buffer);

                // 3. Enqueue chunk in stream.
                auto maybe_error = Bindings::throw_dom_exception_if_needed(stream->realm().vm(), [&]() {
                    return readable_stream_enqueue(*stream->controller(), chunk);
                });

                if (maybe_error.is_error()) {
                    readable_stream_error(*stream, maybe_error.release_error().value().value());
                    return;
                }

                // FIXME: Close the stream now that we have finished enqueuing all chunks to the stream. Without this, ReadableStream.read will never resolve the second time around with 'done' set.
                //        Nowhere in the spec seems to mention this - but testing against other implementations the stream does appear to be closed after reading all data (closed callback is fired).
                //        Probably there is a better way of doing this.
                readable_stream_close(*stream);
            }));
        }
    }

    // 4. Return stream.
    return stream;
}

// https://w3c.github.io/FileAPI/#dom-blob-text
JS::NonnullGCPtr<JS::Promise> Blob::text()
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Let stream be the result of calling get stream on this.
    auto stream = get_stream();

    // 2. Let reader be the result of getting a reader from stream. If that threw an exception, return a new promise rejected with that exception.
    auto reader_or_exception = acquire_readable_stream_default_reader(*stream);
    if (reader_or_exception.is_exception())
        return WebIDL::create_rejected_promise_from_exception(realm, reader_or_exception.release_error());
    auto reader = reader_or_exception.release_value();

    // 3. Let promise be the result of reading all bytes from stream with reader
    auto promise = reader->read_all_bytes_deprecated();

    // 4. Return the result of transforming promise by a fulfillment handler that returns the result of running UTF-8 decode on its first argument.
    return WebIDL::upon_fulfillment(*promise, JS::create_heap_function(heap(), [&vm](JS::Value first_argument) -> WebIDL::ExceptionOr<JS::Value> {
        auto const& object = first_argument.as_object();
        VERIFY(is<JS::ArrayBuffer>(object));
        auto const& buffer = static_cast<const JS::ArrayBuffer&>(object).buffer();

        auto decoder = TextCodec::decoder_for("UTF-8"sv);
        auto utf8_text = TRY_OR_THROW_OOM(vm, TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, buffer));
        return JS::PrimitiveString::create(vm, move(utf8_text));
    }));
}

// https://w3c.github.io/FileAPI/#dom-blob-arraybuffer
JS::NonnullGCPtr<JS::Promise> Blob::array_buffer()
{
    auto& realm = this->realm();

    // 1. Let stream be the result of calling get stream on this.
    auto stream = get_stream();

    // 2. Let reader be the result of getting a reader from stream. If that threw an exception, return a new promise rejected with that exception.
    auto reader_or_exception = acquire_readable_stream_default_reader(*stream);
    if (reader_or_exception.is_exception())
        return WebIDL::create_rejected_promise_from_exception(realm, reader_or_exception.release_error());
    auto reader = reader_or_exception.release_value();

    // 3. Let promise be the result of reading all bytes from stream with reader.
    auto promise = reader->read_all_bytes_deprecated();

    // 4. Return the result of transforming promise by a fulfillment handler that returns a new ArrayBuffer whose contents are its first argument.
    return WebIDL::upon_fulfillment(*promise, JS::create_heap_function(heap(), [&realm](JS::Value first_argument) -> WebIDL::ExceptionOr<JS::Value> {
        auto const& object = first_argument.as_object();
        VERIFY(is<JS::ArrayBuffer>(object));
        auto const& buffer = static_cast<const JS::ArrayBuffer&>(object).buffer();

        return JS::ArrayBuffer::create(realm, buffer);
    }));
}

// https://w3c.github.io/FileAPI/#dom-blob-bytes
JS::NonnullGCPtr<JS::Promise> Blob::bytes()
{
    auto& realm = this->realm();

    // 1. Let stream be the result of calling get stream on this.
    auto stream = get_stream();

    // 2. Let reader be the result of getting a reader from stream. If that threw an exception, return a new promise rejected with that exception.
    auto reader_or_exception = acquire_readable_stream_default_reader(*stream);
    if (reader_or_exception.is_exception())
        return WebIDL::create_rejected_promise_from_exception(realm, reader_or_exception.release_error());
    auto reader = reader_or_exception.release_value();

    // 3. Let promise be the result of reading all bytes from stream with reader.
    auto promise = reader->read_all_bytes_deprecated();

    // 4. Return the result of transforming promise by a fulfillment handler that returns a new Uint8Array wrapping an ArrayBuffer containing its first argument.
    return WebIDL::upon_fulfillment(*promise, JS::create_heap_function(heap(), [&realm](JS::Value first_argument) -> WebIDL::ExceptionOr<JS::Value> {
        auto& object = first_argument.as_object();
        VERIFY(is<JS::ArrayBuffer>(object));
        auto& array_buffer = static_cast<JS::ArrayBuffer&>(object);
        return JS::Uint8Array::create(realm, array_buffer.byte_length(), array_buffer);
    }));
}

}
