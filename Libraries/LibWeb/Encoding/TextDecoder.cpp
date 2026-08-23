/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/FlyString.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Encoding/TextDecoder.h>

namespace Web::Encoding {

GC_DEFINE_ALLOCATOR(TextDecoder);

WebIDL::ExceptionOr<GC::Ref<TextDecoder>> TextDecoder::create(Utf16String const& label, TextDecoderOptions const& options)
{
    return create(Utf16FlyString::from_utf16(label.utf16_view()), options);
}

// https://encoding.spec.whatwg.org/#dom-textdecoder
WebIDL::ExceptionOr<GC::Ref<TextDecoder>> TextDecoder::create(Utf16FlyString label, TextDecoderOptions const& options)
{
    // 1. Let encoding be the result of getting an encoding from label.
    auto encoding = TextCodec::get_standardized_encoding(label.view());

    // 2. If encoding is failure or replacement, then throw a RangeError.
    if (!encoding.has_value() || encoding->equals_ignoring_ascii_case("replacement"sv))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Invalid encoding"_utf16 };

    // 3. Set this’s encoding to encoding.
    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    // The encoding getter steps are to return this’s encoding’s name, ASCII lowercased.
    auto lowercase_encoding_name = encoding.value().to_ascii_lowercase_string();

    // 4. If options["fatal"] is true, then set this’s error mode to "fatal".
    auto error_mode = options.fatal ? TextCodec::ErrorMode::Fatal : TextCodec::ErrorMode::Replacement;

    // 5. Set this’s ignore BOM to options["ignoreBOM"].
    auto ignore_bom = options.ignore_bom;

    // NOTE: This should happen in decode(), but we don't support streaming yet and share decoders across calls.
    return GC::Heap::the().allocate<TextDecoder>(lowercase_encoding_name, error_mode, ignore_bom);
}

// https://encoding.spec.whatwg.org/#dom-textdecoder
TextDecoder::TextDecoder(FlyString encoding, TextCodec::ErrorMode error_mode, bool ignore_bom)
    : TextDecoderCommonMixin(move(encoding), error_mode, ignore_bom)
{
    set_decoder_to_new_instance_of_encoding_decoder();
}

TextDecoder::~TextDecoder() = default;

WebIDL::ExceptionOr<Utf16String> TextDecoder::decode(Optional<WebIDL::BufferSourceVariant> const& input, Optional<TextDecodeOptions> const& options) const
{
    Optional<ByteBuffer> data_buffer;
    if (input.has_value()) {
        auto data_buffer_or_error = WebIDL::get_buffer_source_copy(WebIDL::BufferSource { *input });
        if (data_buffer_or_error.is_error())
            return WebIDL::OperationError::create("Failed to copy bytes from ArrayBuffer"_utf16);
        data_buffer = data_buffer_or_error.release_value();
    }

    return decode(data_buffer.map([](auto const& buffer) { return buffer.bytes(); }), options.has_value() && options->stream);
}

// https://encoding.spec.whatwg.org/#dom-textdecoder-decode
WebIDL::ExceptionOr<Utf16String> TextDecoder::decode(Optional<ReadonlyBytes> input, bool stream) const
{
    auto& vm = JS::VM::the();

    auto decode_bytes = [&](ErrorOr<Utf16String> decoded) -> WebIDL::ExceptionOr<Utf16String> {
        if (decoded.is_error()) {
            // Decoder errors in fatal mode are reported as a TypeError. Only
            // allocation failures should become an internal out-of-memory
            // exception; do not assert that every codec error is ENOMEM.
            if (decoded.error().code() == ENOMEM)
                return vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Decoding failed"_utf16 };
        }
        return decoded.release_value();
    };

    if (stream)
        return decode_bytes(m_decoder->to_utf16(input.has_value() ? input.value() : ReadonlyBytes {}));

    // A non-streaming decode signals end-of-queue to the decoder. This flushes any pending partial sequence.
    // encoding_rs permanently finishes a decoder after that operation, so replace it before returning; the
    // TextDecoder object must be reusable for a subsequent independent decode().
    auto decoded_or_error = m_decoder->to_utf16(input.has_value() ? input.value() : ReadonlyBytes {}, TextCodec::Flush::Yes);
    set_decoder_to_new_instance_of_encoding_decoder();

    return decode_bytes(move(decoded_or_error));
}

}
