/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOMURL/URLSearchParams.h>
#include <LibWeb/Fetch/Body.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Infra/JSON.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::Fetch {

BodyMixin::~BodyMixin() = default;

// https://fetch.spec.whatwg.org/#body-unusable
bool BodyMixin::is_unusable() const
{
    // An object including the Body interface mixin is said to be unusable if its body is non-null and its body’s stream is disturbed or locked.
    auto const& body = body_impl();
    return body && (body->stream()->is_disturbed() || body->stream()->is_locked());
}

// https://fetch.spec.whatwg.org/#dom-body-body
GC::Ptr<Streams::ReadableStream> BodyMixin::body() const
{
    // The body getter steps are to return null if this’s body is null; otherwise this’s body’s stream.
    auto const& body = body_impl();
    return body ? body->stream().ptr() : nullptr;
}

// https://fetch.spec.whatwg.org/#dom-body-bodyused
bool BodyMixin::body_used() const
{
    // The bodyUsed getter steps are to return true if this’s body is non-null and this’s body’s stream is disturbed; otherwise false.
    auto const& body = body_impl();
    return body && body->stream()->is_disturbed();
}

// https://fetch.spec.whatwg.org/#dom-body-arraybuffer
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> BodyMixin::array_buffer() const
{
    auto& vm = Bindings::main_thread_vm();
    auto& realm = *vm.current_realm();

    // The arrayBuffer() method steps are to return the result of running consume body with this and
    // the following step given a byte sequence bytes:
    return consume_body(realm, *this, GC::create_function(realm.heap(), [&realm](ByteBuffer bytes) -> WebIDL::ExceptionOr<JS::Value> {
        // return the result of creating an ArrayBuffer from bytes in this’s relevant realm.
        // NOTE: The above method can reject with a RangeError.
        return JS::ArrayBuffer::create(realm, move(bytes));
    }));
}

// https://fetch.spec.whatwg.org/#dom-body-blob
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> BodyMixin::blob() const
{
    auto& vm = Bindings::main_thread_vm();
    auto& realm = *vm.current_realm();

    // The blob() method steps are to return the result of running consume body with this and
    // the following step given a byte sequence bytes:
    return consume_body(realm, *this, GC::create_function(realm.heap(), [this, &realm](ByteBuffer bytes) -> WebIDL::ExceptionOr<JS::Value> {
        // return a Blob whose contents are bytes and whose type attribute is the result of get the MIME type with this.
        // NOTE: If extracting the mime type returns failure, other browsers set it to an empty string - not sure if that's spec'd.
        auto mime_type = this->mime_type_impl();
        auto mime_type_string = mime_type.has_value() ? mime_type->serialized() : String {};
        return FileAPI::Blob::create(realm, move(bytes), move(mime_type_string));
    }));
}

// https://fetch.spec.whatwg.org/#dom-body-bytes
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> BodyMixin::bytes() const
{
    auto& vm = Bindings::main_thread_vm();
    auto& realm = *vm.current_realm();

    // The bytes() method steps are to return the result of running consume body with this and
    // the following step given a byte sequence bytes:
    return consume_body(realm, *this, GC::create_function(realm.heap(), [&realm](ByteBuffer bytes) -> WebIDL::ExceptionOr<JS::Value> {
        // return the result of creating a Uint8Array from bytes in this’s relevant realm.
        // NOTE: The above method can reject with a RangeError.
        auto bytes_length = bytes.size();
        auto array_buffer = JS::ArrayBuffer::create(realm, move(bytes));
        return JS::Uint8Array::create(realm, bytes_length, *array_buffer);
    }));
}

// https://fetch.spec.whatwg.org/#dom-body-formdata
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> BodyMixin::form_data() const
{
    auto& vm = Bindings::main_thread_vm();
    auto& realm = *vm.current_realm();

    // The formData() method steps are to return the result of running consume body with this and
    // the following steps given a byte sequence bytes:
    return consume_body(realm, *this, GC::create_function(realm.heap(), [this, &realm](ByteBuffer bytes) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Let mimeType be the result of get the MIME type with this.
        auto mime_type = this->mime_type_impl();

        // 2. If mimeType is non-null, then switch on mimeType’s essence and run the corresponding steps:
        if (mime_type.has_value()) {
            // -> "multipart/form-data"
            if (mime_type->essence() == "multipart/form-data"sv) {
                // 1. Parse bytes, using the value of the `boundary` parameter from mimeType, per the rules set forth in Returning Values from Forms: multipart/form-data. [RFC7578]
                auto error_or_entry_list = parse_multipart_form_data(realm, bytes, mime_type.value());

                // 2. If that fails for some reason, then throw a TypeError.
                if (error_or_entry_list.is_error())
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Failed to parse multipart form data: {}", error_or_entry_list.release_error().message)) };

                // 3. Return a new FormData object, appending each entry, resulting from the parsing operation, to its entry list.
                return TRY(XHR::FormData::create(realm, error_or_entry_list.release_value()));
            }
            // -> "application/x-www-form-urlencoded"
            if (mime_type->essence() == "application/x-www-form-urlencoded"sv) {
                // 1. Let entries be the result of parsing bytes.
                auto entries = DOMURL::url_decode(StringView { bytes });

                // 2. Return a new FormData object whose entry list is entries.
                return TRY(XHR::FormData::create(realm, entries));
            }
        }

        // 3. Throw a TypeError.
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Mime type must be 'multipart/form-data' or 'application/x-www-form-urlencoded'"sv };
    }));
}

// https://fetch.spec.whatwg.org/#dom-body-json
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> BodyMixin::json() const
{
    auto& vm = Bindings::main_thread_vm();
    auto& realm = *vm.current_realm();

    // The json() method steps are to return the result of running consume body with this and parse JSON from bytes.
    // NOTE: The above method can reject with a SyntaxError.
    return consume_body(realm, *this, GC::create_function(realm.heap(), [&realm](ByteBuffer bytes) -> WebIDL::ExceptionOr<JS::Value> {
        return Infra::parse_json_bytes_to_javascript_value(realm, bytes);
    }));
}

// https://fetch.spec.whatwg.org/#dom-body-text
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> BodyMixin::text() const
{
    auto& vm = Bindings::main_thread_vm();
    auto& realm = *vm.current_realm();

    // The text() method steps are to return the result of running consume body with this and UTF-8 decode.
    return consume_body(realm, *this, GC::create_function(realm.heap(), [&vm](ByteBuffer bytes) -> WebIDL::ExceptionOr<JS::Value> {
        auto decoder = TextCodec::decoder_for("UTF-8"sv);
        VERIFY(decoder.has_value());

        auto utf8_text = MUST(TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, bytes));
        return JS::PrimitiveString::create(vm, move(utf8_text));
    }));
}

// https://fetch.spec.whatwg.org/#concept-body-consume-body
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> consume_body(JS::Realm& realm, BodyMixin const& object, ConvertBytesToJSValueCallback convert_bytes_to_js_value)
{
    // 1. If object is unusable, then return a promise rejected with a TypeError.
    if (object.is_unusable()) {
        WebIDL::SimpleException exception { WebIDL::SimpleExceptionType::TypeError, "Body is unusable"sv };
        return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
    }

    // 2. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 3. Let errorSteps given error be to reject promise with error.
    // NOTE: `promise` and `realm` is protected by GC::HeapFunction.
    auto error_steps = GC::create_function(realm.heap(), [promise, &realm](JS::Value error) {
        // AD-HOC: An execution context is required for Promise's reject function.
        HTML::TemporaryExecutionContext execution_context { realm };
        WebIDL::reject_promise(realm, promise, error);
    });

    // 4. Let successSteps given a byte sequence data be to resolve promise with the result of running convertBytesToJSValue
    //    with data. If that threw an exception, then run errorSteps with that exception.
    // NOTE: `promise`, `realm` and `object` is protected by GC::HeapFunction.
    auto success_steps = GC::create_function(realm.heap(), [promise, &realm, convert_bytes_to_js_value](ByteBuffer data) {
        auto& vm = realm.vm();

        // AD-HOC: An execution context is required for Promise's reject function and JSON.parse.
        HTML::TemporaryExecutionContext execution_context { realm };

        auto value_or_error = Bindings::throw_dom_exception_if_needed(vm, [&]() -> WebIDL::ExceptionOr<JS::Value> {
            return convert_bytes_to_js_value->function()(data);
        });

        if (value_or_error.is_error()) {
            // We can't call error_steps here without moving it into success_steps, causing a double move when we pause error_steps
            // to fully_read, so just reject the promise like error_steps does.
            WebIDL::reject_promise(realm, promise, value_or_error.release_error().value().value());
            return;
        }

        WebIDL::resolve_promise(realm, promise, value_or_error.release_value());
    });

    // 5. If object’s body is null, then run successSteps with an empty byte sequence.
    auto const& body = object.body_impl();
    if (!body) {
        success_steps->function()(ByteBuffer {});
    }
    // 6. Otherwise, fully read object’s body given successSteps, errorSteps, and object’s relevant global object.
    else {
        body->fully_read(realm, success_steps, error_steps, GC::Ref { HTML::relevant_global_object(object.as_platform_object()) });
    }

    // 7. Return promise.
    return promise;
}

// https://andreubotella.github.io/multipart-form-data/#parse-a-multipart-form-data-name
static MultipartParsingErrorOr<String> parse_multipart_form_data_name(GenericLexer& lexer)
{
    // 1. Assert: The byte at (position - 1) is 0x22 (").
    VERIFY(lexer.peek(-1) == '"');

    // 2. Let name be the result of collecting a sequence of bytes that are not 0x0A (LF), 0x0D (CR) or 0x22 ("), given position.
    auto name = lexer.consume_until(is_any_of("\n\r\""sv));

    // 3. If the byte at position is not 0x22 ("), return failure. Otherwise, advance position by 1.
    if (!lexer.consume_specific('"'))
        return MultipartParsingError { MUST(String::formatted("Expected \" at position {}", lexer.tell())) };

    // 4. Replace any occurrence of the following subsequences in name with the given byte:
    //    - "%0A" with 0x0A (LF)
    //    - "%0D" with 0x0D (CR)
    //    - "%22" with 0x22 (")
    StringBuilder builder;
    for (size_t i = 0; i < name.length(); ++i) {
        // Check for subsequences starting with '%'
        if (name[i] == '%' && i + 2 < name.length()) {
            auto subsequence = name.substring_view(i, 3);
            if (subsequence == "%0A"sv) {
                builder.append(0x0A); // Append LF
                i += 2;               // Skip the next two characters
                continue;
            }
            if (subsequence == "%0D"sv) {
                builder.append(0x0D); // Append CR
                i += 2;               // Skip the next two characters
                continue;
            }
            if (subsequence == "%22"sv) {
                builder.append(0x22); // Append "
                i += 2;               // Skip the next two characters
                continue;
            }
        }

        // Append the current character if no substitution was made
        builder.append(name[i]);
    }

    return builder.to_string_without_validation();
}

// https://andreubotella.github.io/multipart-form-data/#parse-multipart-form-data-headers
static MultipartParsingErrorOr<MultiPartFormDataHeader> parse_multipart_form_data_header(GenericLexer& lexer)
{
    // 1. Let name, filename and contentType be null.
    MultiPartFormDataHeader header;

    // 2. While true:
    while (true) {
        // 1. If position points to a sequence of bytes starting with 0x0D 0x0A (CR LF):
        if (lexer.next_is("\r\n"sv)) {
            // 1. If name is null, return failure.
            if (!header.name.has_value())
                return MultipartParsingError { "Missing name parameter in Content-Disposition header"_string };

            // 2. Return name, filename and contentType.
            return header;
        }

        // 2. Let header name be the result of collecting a sequence of bytes that are not 0x0A (LF), 0x0D (CR) or 0x3A (:), given position.
        auto header_name = lexer.consume_until(is_any_of("\n\r:"sv));

        // 3. Remove any HTTP tab or space bytes from the start or end of header name.
        header_name = header_name.trim(Infrastructure::HTTP_TAB_OR_SPACE, TrimMode::Both);

        // 4. If header name does not match the field-name token production, return failure.
        if (!Infrastructure::is_header_name(header_name.bytes()))
            return MultipartParsingError { MUST(String::formatted("Invalid header name {}", header_name)) };

        // 5. If the byte at position is not 0x3A (:), return failure.
        // 6. Advance position by 1.
        if (!lexer.consume_specific(':'))
            return MultipartParsingError { MUST(String::formatted("Expected : at position {}", lexer.tell())) };

        // 7. Collect a sequence of bytes that are HTTP tab or space bytes given position. (Do nothing with those bytes.)
        lexer.ignore_while(Infrastructure::is_http_tab_or_space);

        // 8. Byte-lowercase header name and switch on the result:
        // -> `content-disposition`
        if (header_name.equals_ignoring_ascii_case("content-disposition"sv)) {
            // 1. Set name and filename to null.
            header.name.clear();
            header.filename.clear();

            // 2. If position does not point to a sequence of bytes starting with `form-data; name="`, return failure.
            // 3. Advance position so it points at the byte after the next 0x22 (") byte (the one in the sequence of bytes matched above).
            if (!lexer.consume_specific("form-data; name=\""sv))
                return MultipartParsingError { MUST(String::formatted("Expected `form-data; name=\"` at position {}", lexer.tell())) };

            // 4. Set name to the result of parsing a multipart/form-data name given input and position, if the result is not failure. Otherwise, return failure.
            auto maybe_name = parse_multipart_form_data_name(lexer);
            if (maybe_name.is_error())
                return maybe_name.release_error();
            header.name = maybe_name.release_value();

            // 5. If position points to a sequence of bytes starting with `; filename="`:
            //     1. Advance position so it points at the byte after the next 0x22 (") byte (the one in the sequence of bytes matched above).
            if (lexer.consume_specific("; filename=\""sv)) {
                // 2. Set filename to the result of parsing a multipart/form-data name given input and position, if the result is not failure. Otherwise, return failure.
                auto maybe_filename = parse_multipart_form_data_name(lexer);
                if (maybe_filename.is_error())
                    return maybe_filename.release_error();
                header.filename = maybe_filename.release_value();
            }
        }
        // -> `content-type`
        else if (header_name.equals_ignoring_ascii_case("content-type"sv)) {
            // 1. Let header value be the result of collecting a sequence of bytes that are not 0x0A (LF) or 0x0D (CR), given position.
            auto header_value = lexer.consume_until(Infrastructure::is_http_newline);

            // 2. Remove any HTTP tab or space bytes from the end of header value.
            header_value = header_value.trim(Infrastructure::HTTP_TAB_OR_SPACE, TrimMode::Right);

            // 3. Set contentType to the isomorphic decoding of header value.
            header.content_type = Infra::isomorphic_decode(header_value.bytes());
        }
        // -> Otherwise
        else {
            // 1. Collect a sequence of bytes that are not 0x0A (LF) or 0x0D (CR), given position. (Do nothing with those bytes.)
            lexer.ignore_until(Infrastructure::is_http_newline);
        }

        // 9. If position does not point to a sequence of bytes starting with 0x0D 0x0A (CR LF), return failure. Otherwise, advance position by 2 (past the newline).
        if (!lexer.consume_specific("\r\n"sv))
            return MultipartParsingError { MUST(String::formatted("Expected CRLF at position {}", lexer.tell())) };
    }
    return header;
}

// https://andreubotella.github.io/multipart-form-data/#multipart-form-data-parser
MultipartParsingErrorOr<Vector<XHR::FormDataEntry>> parse_multipart_form_data(JS::Realm& realm, StringView input, MimeSniff::MimeType const& mime_type)
{
    // 1. Assert: mimeType’s essence is "multipart/form-data".
    VERIFY(mime_type.essence() == "multipart/form-data"sv);

    // 2. If mimeType’s parameters["boundary"] does not exist, return failure. Otherwise, let boundary be the result of UTF-8 decoding mimeType’s parameters["boundary"].
    auto maybe_boundary = mime_type.parameters().get("boundary"sv);
    if (!maybe_boundary.has_value())
        return MultipartParsingError { "Missing boundary parameter in Content-Type header"_string };
    auto boundary = maybe_boundary.release_value();

    // 3. Let entry list be an empty entry list.
    Vector<XHR::FormDataEntry> entry_list;

    // 4. Let position be a pointer to a byte in input, initially pointing at the first byte.
    GenericLexer lexer(input);

    auto boundary_with_dashes = MUST(String::formatted("--{}", boundary));

    // 5. While true:
    while (true) {
        // 1. If position points to a sequence of bytes starting with 0x2D 0x2D (`--`) followed by boundary, advance position by 2 + the length of boundary. Otherwise, return failure.
        if (!lexer.consume_specific(boundary_with_dashes))
            return MultipartParsingError { MUST(String::formatted("Expected `--` followed by boundary at position {}", lexer.tell())) };

        // 2. If position points to the sequence of bytes 0x2D 0x2D 0x0D 0x0A (`--` followed by CR LF) followed by the end of input, return entry list.
        if (lexer.next_is("--\r\n"sv))
            return entry_list;

        // 3. If position does not point to a sequence of bytes starting with 0x0D 0x0A (CR LF), return failure.
        // 4. Advance position by 2. (This skips past the newline.)
        if (!lexer.consume_specific("\r\n"sv))
            return MultipartParsingError { MUST(String::formatted("Expected CRLF at position {}", lexer.tell())) };

        // 5. Let name, filename and contentType be the result of parsing multipart/form-data headers on input and position, if the result is not failure. Otherwise, return failure.
        auto header = TRY(parse_multipart_form_data_header(lexer));

        // 6. Advance position by 2. (This skips past the empty line that marks the end of the headers.)
        lexer.ignore(2);

        // 7. Let body be the empty byte sequence.
        // 8. Body loop: While position is not past the end of input:
        //      1. Append the code point at position to body.
        //      2. If body ends with boundary:
        //          1. Remove the last 4 + (length of boundary) bytes from body.
        //          2. Decrease position by 4 + (length of boundary).
        //          3. Break out of body loop.
        auto body = lexer.consume_until(boundary_with_dashes.bytes_as_string_view());
        if (lexer.next_is(boundary_with_dashes.bytes_as_string_view())) {
            constexpr size_t trailing_crlf_length = 2;
            if (body.length() >= trailing_crlf_length) {
                body = body.substring_view(0, body.length() - trailing_crlf_length);
                lexer.retreat(trailing_crlf_length);
            }
        }

        // 9. If position does not point to a sequence of bytes starting with 0x0D 0x0A (CR LF), return failure. Otherwise, advance position by 2.
        if (!lexer.consume_specific("\r\n"sv))
            return MultipartParsingError { MUST(String::formatted("Expected CRLF at position {}", lexer.tell())) };

        // 10. If filename is not null:
        Optional<XHR::FormDataEntryValue> value;
        if (header.filename.has_value()) {
            // 1. If contentType is null, set contentType to "text/plain".
            if (!header.content_type.has_value())
                header.content_type = "text/plain"_string;

            // 2. If contentType is not an ASCII string, set contentType to the empty string.
            if (!all_of(header.content_type->code_points(), is_ascii)) {
                header.content_type = ""_string;
            }

            // 3. Let value be a new File object with name filename, type contentType, and body body.
            auto blob = FileAPI::Blob::create(realm, MUST(ByteBuffer::copy(body.bytes())), header.content_type.release_value());
            FileAPI::FilePropertyBag options {};
            options.type = blob->type();
            auto file = MUST(FileAPI::File::create(realm, { GC::make_root(blob) }, header.filename.release_value(), move(options)));
            value = GC::make_root(file);
        }
        // 11. Otherwise:
        else {
            // 1. Let value be the UTF-8 decoding without BOM of body.
            value = String::from_utf8_with_replacement_character(body, String::WithBOMHandling::No);
        }

        // 12. Assert: name is a scalar value string and value is either a scalar value string or a File object.
        VERIFY(header.name.has_value() && value.has_value());

        // 13. Create an entry with name and value, and append it to entry list.
        entry_list.empend(header.name.release_value(), value.release_value());
    }
}

}
