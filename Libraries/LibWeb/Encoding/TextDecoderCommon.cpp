/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Encoding/TextDecoderCommon.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::Encoding {

void TextDecoderOutputQueue::push(Utf16String output)
{
    if (output.is_empty())
        return;

    if (m_has_builder) {
        m_builder.append(output);
        return;
    }

    if (!m_single_output.has_value()) {
        m_single_output = move(output);
        return;
    }

    m_has_builder = true;
    m_builder.append(m_single_output.release_value());
    m_builder.append(output);
}

Utf16String TextDecoderOutputQueue::serialize()
{
    if (m_has_builder)
        return m_builder.to_string();
    if (m_single_output.has_value())
        return m_single_output.release_value();
    return {};
}

TextDecoderCommonMixin::TextDecoderCommonMixin(FlyString encoding, TextCodec::ErrorMode error_mode, bool ignore_bom)
    : m_encoding(move(encoding))
    , m_encoding_for_bindings(Utf16FlyString::from_fly_string(m_encoding))
    , m_error_mode(error_mode)
    , m_ignore_bom(ignore_bom)
{
}

void TextDecoderCommonMixin::set_decoder_to_new_instance_of_encoding_decoder() const
{
    m_decoder = make<TextCodec::StreamingDecoder>(m_encoding, m_ignore_bom ? TextCodec::IgnoreBOM::Yes : TextCodec::IgnoreBOM::No, m_error_mode);
}

static WebIDL::ExceptionOr<void> append_decoded_output(TextDecoderOutputQueue& output, ErrorOr<Utf16String> decoded_or_error)
{
    if (decoded_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Decoding failed"_utf16 };

    output.push(decoded_or_error.release_value());
    return {};
}

WebIDL::ExceptionOr<void> TextDecoderCommonMixin::process_an_item(ReadonlyBytes item, TextDecoderOutputQueue& output)
{
    VERIFY(m_decoder);
    return append_decoded_output(output, m_decoder->decode(item));
}

WebIDL::ExceptionOr<void> TextDecoderCommonMixin::process_an_item(EndOfQueue, TextDecoderOutputQueue& output)
{
    VERIFY(m_decoder);
    return append_decoded_output(output, m_decoder->decode({}, TextCodec::Flush::Yes));
}

Utf16String TextDecoderCommonMixin::serialize_io_queue(TextDecoderOutputQueue& output)
{
    return output.serialize();
}

}
