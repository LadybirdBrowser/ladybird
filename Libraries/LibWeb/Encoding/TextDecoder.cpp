/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Encoding/TextDecoder.h>

namespace Web::Encoding {

GC_DEFINE_ALLOCATOR(TextDecoder);

WebIDL::ExceptionOr<GC::Ref<TextDecoder>> TextDecoder::create(String const& label, TextDecoderOptions const& options)
{
    return create(FlyString { label }, options);
}

// https://encoding.spec.whatwg.org/#dom-textdecoder
WebIDL::ExceptionOr<GC::Ref<TextDecoder>> TextDecoder::create(FlyString label, TextDecoderOptions const& options)
{
    // 1. Let encoding be the result of getting an encoding from label.
    auto encoding = TextCodec::get_standardized_encoding(label);

    // 2. If encoding is failure or replacement, then throw a RangeError.
    if (!encoding.has_value() || encoding->equals_ignoring_ascii_case("replacement"sv))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Invalid encoding"sv };

    // 3. Set this’s encoding to encoding.
    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    // The encoding getter steps are to return this’s encoding’s name, ASCII lowercased.
    auto lowercase_encoding_name = encoding.value().to_ascii_lowercase_string();

    // 4. If options["fatal"] is true, then set this’s error mode to "fatal".
    auto error_mode = options.fatal ? ErrorMode::Fatal : ErrorMode::Replacement;

    // 5. Set this’s ignore BOM to options["ignoreBOM"].
    auto ignore_bom = options.ignore_bom;

    // NOTE: This should happen in decode(), but we don't support streaming yet and share decoders across calls.
    auto decoder = TextCodec::decoder_for_exact_name(encoding.value());
    VERIFY(decoder.has_value());

    return GC::Heap::the().allocate<TextDecoder>(*decoder, lowercase_encoding_name, error_mode, ignore_bom);
}

// https://encoding.spec.whatwg.org/#dom-textdecoder
TextDecoder::TextDecoder(TextCodec::Decoder& decoder, FlyString encoding, ErrorMode error_mode, bool ignore_bom)
    : TextDecoderCommonMixin(decoder, move(encoding), error_mode, ignore_bom)
{
}

TextDecoder::~TextDecoder() = default;

WebIDL::ExceptionOr<String> TextDecoder::decode(Optional<WebIDL::BufferSourceVariant> const& input, Optional<TextDecodeOptions> const&) const
{
    Optional<ByteBuffer> data_buffer;
    if (input.has_value()) {
        auto data_buffer_or_error = WebIDL::get_buffer_source_copy(WebIDL::BufferSource { *input });
        if (data_buffer_or_error.is_error())
            return WebIDL::OperationError::create("Failed to copy bytes from ArrayBuffer"_utf16);
        data_buffer = data_buffer_or_error.release_value();
    }

    return decode(data_buffer.map([](auto const& buffer) { return buffer.bytes(); }));
}

// https://encoding.spec.whatwg.org/#dom-textdecoder-decode
WebIDL::ExceptionOr<String> TextDecoder::decode(Optional<ReadonlyBytes> input) const
{
    auto& vm = JS::VM::the();

    if (!input.has_value())
        return TRY_OR_THROW_OOM(vm, m_decoder.to_utf8({}));

    // FIXME: Implement the streaming stuff.
    auto result = TRY_OR_THROW_OOM(vm, m_decoder.to_utf8(input.value()));
    if (this->fatal() && result.contains(0xfffd))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Decoding failed"sv };
    return result;
}

}
