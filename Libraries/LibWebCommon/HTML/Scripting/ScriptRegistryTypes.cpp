/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/Scripting/ScriptRegistryTypes.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ScriptRegistryIdentifier const& identifier)
{
    TRY(encoder.encode(identifier.document_id));
    TRY(encoder.encode(identifier.script_id));
    return {};
}

template<>
ErrorOr<Web::HTML::ScriptRegistryIdentifier> decode(Decoder& decoder)
{
    auto document_id = TRY(decoder.decode<Web::UniqueNodeID>());
    auto script_id = TRY(decoder.decode<u64>());

    return Web::HTML::ScriptRegistryIdentifier {
        .document_id = document_id,
        .script_id = script_id,
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ScriptRegistryDescription const& source)
{
    TRY(encoder.encode(source.id));
    TRY(encoder.encode(source.url));
    TRY(encoder.encode(source.display_url));
    TRY(encoder.encode(source.introduction_type));
    TRY(encoder.encode(source.content_type));
    TRY(encoder.encode(source.is_inline_source));
    TRY(encoder.encode(source.source_start_line));
    TRY(encoder.encode(source.source_start_column));
    TRY(encoder.encode(source.source_length));
    return {};
}

template<>
ErrorOr<Web::HTML::ScriptRegistryDescription> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Web::HTML::ScriptRegistryIdentifier>());
    auto url = TRY(decoder.decode<Optional<URL::URL>>());
    auto display_url = TRY(decoder.decode<Utf16String>());
    auto introduction_type = TRY(decoder.decode<Utf16String>());
    auto content_type = TRY(decoder.decode<Utf16String>());
    auto is_inline_source = TRY(decoder.decode<bool>());
    auto source_start_line = TRY(decoder.decode<u32>());
    auto source_start_column = TRY(decoder.decode<u32>());
    auto source_length = TRY(decoder.decode<size_t>());

    return Web::HTML::ScriptRegistryDescription {
        .id = id,
        .url = move(url),
        .display_url = move(display_url),
        .introduction_type = move(introduction_type),
        .content_type = move(content_type),
        .is_inline_source = is_inline_source,
        .source_start_line = source_start_line,
        .source_start_column = source_start_column,
        .source_length = source_length,
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ScriptRegistryContent const& source_content)
{
    TRY(encoder.encode(source_content.content_type));
    TRY(encoder.encode(source_content.text));
    return {};
}

template<>
ErrorOr<Web::HTML::ScriptRegistryContent> decode(Decoder& decoder)
{
    auto content_type = TRY(decoder.decode<Utf16String>());
    auto text = TRY(decoder.decode<Utf16String>());

    return Web::HTML::ScriptRegistryContent {
        .content_type = move(content_type),
        .text = move(text),
    };
}

}
