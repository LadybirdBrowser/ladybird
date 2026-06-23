/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/FlyString.h>
#include <AK/StringBuilder.h>
#include <AK/Variant.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextDecoder.h>
#include <LibWeb/Encoding/TextDecoder.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::Encoding {

GC_DEFINE_ALLOCATOR(TextDecoder);

// https://encoding.spec.whatwg.org/#dom-textdecoder
WebIDL::ExceptionOr<GC::Ref<TextDecoder>> TextDecoder::construct_impl(JS::Realm& realm, StringView label, Bindings::TextDecoderOptions const& options)
{
    auto& vm = realm.vm();

    // 1. Let encoding be the result of getting an encoding from label.
    auto encoding = TextCodec::get_standardized_encoding(label);

    // 2. If encoding is failure or replacement, then throw a RangeError.
    if (!encoding.has_value() || encoding->equals_ignoring_ascii_case("replacement"sv))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, TRY_OR_THROW_OOM(vm, String::formatted("Invalid encoding {}", label)) };

    // 3. Set this’s encoding to encoding.
    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    // The encoding getter steps are to return this’s encoding’s name, ASCII lowercased.
    auto lowercase_encoding_name = encoding.value().to_ascii_lowercase_string();

    // 4. If options["fatal"] is true, then set this’s error mode to "fatal".
    auto error_mode = options.fatal ? TextCodec::ErrorMode::Fatal : TextCodec::ErrorMode::Replacement;

    // 5. Set this’s ignore BOM to options["ignoreBOM"].
    auto ignore_bom = options.ignore_bom;

    return realm.create<TextDecoder>(realm, lowercase_encoding_name, error_mode, ignore_bom);
}

// https://encoding.spec.whatwg.org/#dom-textdecoder
TextDecoder::TextDecoder(JS::Realm& realm, FlyString encoding, TextCodec::ErrorMode error_mode, bool ignore_bom)
    : PlatformObject(realm)
    , TextDecoderCommonMixin(move(encoding), error_mode, ignore_bom)
{
}

TextDecoder::~TextDecoder() = default;

void TextDecoder::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextDecoder);
    Base::initialize(realm);
}

// https://encoding.spec.whatwg.org/#dom-textdecoder-decode
WebIDL::ExceptionOr<String> TextDecoder::decode(Optional<WebIDL::BufferSourceVariant> input, Bindings::TextDecodeOptions const& options)
{
    // 1. If this’s do not flush is false, then set this’s decoder to a new instance of this’s encoding’s decoder, this’s
    //    I/O queue to the I/O queue of bytes « end-of-queue », and this’s BOM seen to false.
    if (!m_do_not_flush)
        set_decoder_to_new_instance_of_encoding_decoder();
    VERIFY(m_decoder);

    // 2. Set this’s do not flush to options["stream"].
    m_do_not_flush = options.stream;

    Optional<ByteBuffer> input_copy;

    // 3. If input is given, then push a copy of input to this’s I/O queue.
    // NOTE: Implementations are strongly encouraged to use an implementation strategy that avoids this copy. When doing
    //       so they will have to make sure that changes to input do not affect future calls to decode().
    // WARNING: The memory exposed by SharedArrayBuffer objects does not adhere to data race freedom properties required
    //          by the memory model of programming languages typically used for implementations. When implementing, take
    //          care to use the appropriate facilities when accessing memory exposed by SharedArrayBuffer objects.
    // FIXME: Safely avoid this copy.
    if (input.has_value()) {
        auto data_buffer_or_error = WebIDL::get_buffer_source_copy(*input);
        if (data_buffer_or_error.is_error())
            return WebIDL::OperationError::create(realm(), "Failed to copy bytes from ArrayBuffer"_utf16);
        input_copy = data_buffer_or_error.release_value();
    }

    // 4. Let output be the I/O queue of scalar values « end-of-queue ».
    TextDecoderOutputQueue output;

    // 5. While true:
    bool did_read_input = false;
    auto read_item = [&]() -> Variant<ReadonlyBytes, EndOfQueue> {
        if (did_read_input || !input_copy.has_value())
            return EndOfQueue {};
        did_read_input = true;
        return ReadonlyBytes { input_copy->data(), input_copy->size() };
    };

    while (true) {
        // 1. Let item be the result of reading from this’s I/O queue.
        auto item = read_item();

        // 2. If item is end-of-queue and this’s do not flush is true, then return the result of running serialize I/O
        //    queue with this and output.
        if (item.has<EndOfQueue>() && m_do_not_flush) {
            return serialize_io_queue(vm(), output);
        }
        // 3. Otherwise:
        else {
            //  1. Let result be the result of processing an item with item, this’s decoder, this’s I/O queue, output,
            //      and this’s error mode.
            auto result = item.visit([&](auto item) {
                return process_an_item(vm(), item, output);
            });

            // 2. If result is finished, then return the result of running serialize I/O queue with this and output.
            if (!result.is_error() && item.has<EndOfQueue>())
                return serialize_io_queue(vm(), output);

            // 3. If result is error, throw a TypeError.
            if (result.is_error())
                return result.release_error();
        }
    }
}

}
