/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibJS/SourceCode.h>
#include <LibURL/Parser.h>
#include <LibWeb/HTML/Scripting/ScriptRegistry.h>

namespace Web::HTML {

static ScriptRegistry::Content source_content(ScriptRegistry::Script const& script)
{
    return script.content.visit(
        [&](ScriptRegistry::JavaScriptSource const& js_source) -> ScriptRegistry::Content {
            return {
                .content_type = script.description.content_type,
                .text = js_source.source_code->code().to_utf8(),
            };
        });
}

ScriptRegistry::Script const& ScriptRegistry::register_javascript_source(NonnullRefPtr<JS::SourceCode const> source_code, ByteString const& filename, String introduction_type, IsInlineSource is_inline_source, size_t source_line_number, size_t source_length)
{
    // FIXME: Support WebAssembly sources once WebAssembly modules retain their original binary module bytes.
    // FIXME: Register worker sources when DevTools can target workers.
    auto parsed_url = URL::Parser::basic_parse(filename);
    auto id = m_next_script_id++;
    auto is_inline = is_inline_source == IsInlineSource::Yes;
    m_scripts.set(id, {
                          .description = {
                              .id = { .document_id = {}, .script_id = id },
                              .url = move(parsed_url),
                              .display_url = String::from_utf8_with_replacement_character(filename),
                              .introduction_type = move(introduction_type),
                              .content_type = is_inline ? "text/html"_string : "text/javascript"_string,
                              .is_inline_source = is_inline,
                              .source_start_line = static_cast<u32>(source_line_number),
                              .source_start_column = 0,
                              .source_length = source_length,
                          },
                          .content = ContentHandle { JavaScriptSource { move(source_code) } },
                      });

    return m_scripts.find(id)->value;
}

Optional<ScriptRegistry::Content> ScriptRegistry::script_content(u64 script_id, String const& document_source) const
{
    auto script = m_scripts.find(script_id);
    if (script == m_scripts.end())
        return {};

    if (script->value.description.is_inline_source && !document_source.is_empty()) {
        return Content { .content_type = script->value.description.content_type, .text = document_source };
    }

    return source_content(script->value);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ScriptRegistry::Identifier const& identifier)
{
    TRY(encoder.encode(identifier.document_id));
    TRY(encoder.encode(identifier.script_id));
    return {};
}

template<>
ErrorOr<Web::HTML::ScriptRegistry::Identifier> decode(Decoder& decoder)
{
    auto document_id = TRY(decoder.decode<Web::UniqueNodeID>());
    auto script_id = TRY(decoder.decode<u64>());

    return Web::HTML::ScriptRegistry::Identifier {
        .document_id = document_id,
        .script_id = script_id,
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ScriptRegistry::Description const& source)
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
ErrorOr<Web::HTML::ScriptRegistry::Description> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Web::HTML::ScriptRegistry::Identifier>());
    auto url = TRY(decoder.decode<Optional<URL::URL>>());
    auto display_url = TRY(decoder.decode<String>());
    auto introduction_type = TRY(decoder.decode<String>());
    auto content_type = TRY(decoder.decode<String>());
    auto is_inline_source = TRY(decoder.decode<bool>());
    auto source_start_line = TRY(decoder.decode<u32>());
    auto source_start_column = TRY(decoder.decode<u32>());
    auto source_length = TRY(decoder.decode<size_t>());

    return Web::HTML::ScriptRegistry::Description {
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
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ScriptRegistry::Content const& source_content)
{
    TRY(encoder.encode(source_content.content_type));
    TRY(encoder.encode(source_content.text));
    return {};
}

template<>
ErrorOr<Web::HTML::ScriptRegistry::Content> decode(Decoder& decoder)
{
    auto content_type = TRY(decoder.decode<String>());
    auto text = TRY(decoder.decode<String>());

    return Web::HTML::ScriptRegistry::Content {
        .content_type = move(content_type),
        .text = move(text),
    };
}

}
