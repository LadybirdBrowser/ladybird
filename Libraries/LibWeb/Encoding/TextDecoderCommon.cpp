/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Encoding/TextDecoderCommon.h>

namespace Web::Encoding {

ErrorOr<void> TextDecoderOutputQueue::push(String output)
{
    if (output.is_empty())
        return {};

    if (m_has_builder)
        return m_builder.try_append(output.bytes_as_string_view());

    if (!m_single_output.has_value()) {
        m_single_output = move(output);
        return {};
    }

    m_has_builder = true;
    TRY(m_builder.try_append(m_single_output->bytes_as_string_view()));
    m_single_output.clear();
    return m_builder.try_append(output.bytes_as_string_view());
}

ErrorOr<String> TextDecoderOutputQueue::serialize()
{
    if (m_has_builder)
        return m_builder.to_string();
    if (m_single_output.has_value())
        return m_single_output.release_value();
    return String {};
}

TextDecoderCommonMixin::TextDecoderCommonMixin(FlyString encoding, TextCodec::ErrorMode error_mode, bool ignore_bom)
    : m_encoding(move(encoding))
    , m_error_mode(error_mode)
    , m_ignore_bom(ignore_bom)
{
}

void TextDecoderCommonMixin::set_decoder_to_new_instance_of_encoding_decoder()
{
    m_decoder = make<TextCodec::StreamingDecoder>(m_encoding, m_ignore_bom ? TextCodec::IgnoreBOM::Yes : TextCodec::IgnoreBOM::No, m_error_mode);
}

static WebIDL::ExceptionOr<void> append_decoded_output(JS::VM& vm, TextDecoderOutputQueue& output, ErrorOr<String> decoded_or_error)
{
    if (decoded_or_error.is_error() && decoded_or_error.error().code() != ENOMEM)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Decoding failed"sv };

    auto decoded = TRY_OR_THROW_OOM(vm, move(decoded_or_error));
    TRY_OR_THROW_OOM(vm, output.push(move(decoded)));
    return {};
}

WebIDL::ExceptionOr<void> TextDecoderCommonMixin::process_an_item(JS::VM& vm, ReadonlyBytes item, TextDecoderOutputQueue& output)
{
    VERIFY(m_decoder);
    return append_decoded_output(vm, output, m_decoder->to_utf8(item));
}

WebIDL::ExceptionOr<void> TextDecoderCommonMixin::process_an_item(JS::VM& vm, EndOfQueue, TextDecoderOutputQueue& output)
{
    VERIFY(m_decoder);
    return append_decoded_output(vm, output, m_decoder->finish());
}

WebIDL::ExceptionOr<String> TextDecoderCommonMixin::serialize_io_queue(JS::VM& vm, TextDecoderOutputQueue& output)
{
    return TRY_OR_THROW_OOM(vm, output.serialize());
}

}
