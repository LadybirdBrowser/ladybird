/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextDecoderPrototype.h>
#include <LibWeb/Encoding/TextDecoder.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::Encoding {

JS_DEFINE_ALLOCATOR(TextDecoder);

// https://encoding.spec.whatwg.org/#dom-textdecoder
WebIDL::ExceptionOr<JS::NonnullGCPtr<TextDecoder>> TextDecoder::construct_impl(JS::Realm& realm, FlyString label, Optional<TextDecoderOptions> const& options)
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
    auto lowercase_encoding_name = MUST(String::from_byte_string(encoding.value().to_lowercase_string()));

    // 4. If options["fatal"] is true, then set this’s error mode to "fatal".
    auto fatal = options.value_or({}).fatal;

    // 5. Set this’s ignore BOM to options["ignoreBOM"].
    auto ignore_bom = options.value_or({}).ignore_bom;

    // NOTE: This should happen in decode(), but we don't support streaming yet and share decoders across calls.
    auto decoder = TextCodec::decoder_for_exact_name(encoding.value());
    VERIFY(decoder.has_value());

    return realm.heap().allocate<TextDecoder>(realm, realm, *decoder, lowercase_encoding_name, fatal, ignore_bom);
}

// https://encoding.spec.whatwg.org/#dom-textdecoder
TextDecoder::TextDecoder(JS::Realm& realm, TextCodec::Decoder& decoder, FlyString encoding, bool fatal, bool ignore_bom)
    : PlatformObject(realm)
    , m_decoder(decoder)
    , m_encoding(move(encoding))
    , m_fatal(fatal)
    , m_ignore_bom(ignore_bom)
{
}

TextDecoder::~TextDecoder() = default;

void TextDecoder::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextDecoder);
}

// https://encoding.spec.whatwg.org/#dom-textdecoder-decode
WebIDL::ExceptionOr<String> TextDecoder::decode(Optional<JS::Handle<WebIDL::BufferSource>> const& input, Optional<TextDecodeOptions> const&) const
{
    if (!input.has_value())
        return TRY_OR_THROW_OOM(vm(), m_decoder.to_utf8({}));

    // FIXME: Implement the streaming stuff.
    auto data_buffer_or_error = WebIDL::get_buffer_source_copy(*input.value()->raw_object());
    if (data_buffer_or_error.is_error())
        return WebIDL::OperationError::create(realm(), "Failed to copy bytes from ArrayBuffer"_string);
    auto& data_buffer = data_buffer_or_error.value();
    auto result = TRY_OR_THROW_OOM(vm(), m_decoder.to_utf8({ data_buffer.data(), data_buffer.size() }));
    if (this->fatal() && result.contains(0xfffd))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Decoding failed"sv };
    return result;
}

}
