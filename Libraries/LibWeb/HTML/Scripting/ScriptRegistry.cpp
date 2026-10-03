/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ScriptCompilation.h>
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
                .text = js_source.source_code->code(),
            };
        });
}

ScriptRegistry::Script const& ScriptRegistry::register_javascript_source(NonnullRefPtr<JS::SourceCode const> source_code, JavaScriptSource::Type type, ByteString const& filename, Utf16String display_url, Utf16String introduction_type, IsInlineSource is_inline_source, size_t source_line_number, size_t source_length)
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
                              .display_url = move(display_url),
                              .introduction_type = move(introduction_type),
                              .content_type = is_inline ? "text/html"_utf16 : "text/javascript"_utf16,
                              .is_inline_source = is_inline,
                              .source_start_line = static_cast<u32>(source_line_number),
                              .source_start_column = 0,
                              .source_length = source_length,
                          },
                          .content = ContentHandle { JavaScriptSource {
                              .source_code = move(source_code),
                              .type = type,
                              .line_number_offset = source_line_number,
                              .breakpoint_positions = {},
                          } },
                      });

    return m_scripts.find(id)->value;
}

Optional<ScriptRegistry::Script const&> ScriptRegistry::script_for_source_code(JS::SourceCode const& source_code) const
{
    for (auto const& script : m_scripts) {
        bool has_matching_source_code = false;
        script.value.content.visit([&](JavaScriptSource const& javascript_source) {
            has_matching_source_code = javascript_source.source_code.ptr() == &source_code;
        });
        if (has_matching_source_code)
            return script.value;
    }
    return {};
}

Optional<NonnullRefPtr<JS::SourceCode const>> ScriptRegistry::source_code(u64 script_id) const
{
    auto script = m_scripts.find(script_id);
    if (script == m_scripts.end())
        return {};
    return script->value.content.visit(
        [](JavaScriptSource const& javascript_source) -> Optional<NonnullRefPtr<JS::SourceCode const>> {
            return javascript_source.source_code;
        });
}

Optional<ScriptRegistry::Content> ScriptRegistry::script_content(u64 script_id, Utf16View document_source) const
{
    auto script = m_scripts.find(script_id);
    if (script == m_scripts.end())
        return {};

    if (script->value.description.is_inline_source && !document_source.is_empty()) {
        return Content { .content_type = script->value.description.content_type, .text = Utf16String::from_utf16(document_source) };
    }

    return source_content(script->value);
}

ReadonlySpan<JS::Position> ScriptRegistry::breakpoint_positions(u64 script_id) const
{
    auto script = m_scripts.find(script_id);
    if (script == m_scripts.end())
        return {};

    return script->value.content.visit(
        [](JavaScriptSource const& source) -> ReadonlySpan<JS::Position> {
            if (!source.breakpoint_positions.has_value()) {
                // Function bodies are compiled lazily, so the live Executables do not necessarily contain every breakpoint
                // position. Recompile a GC-free copy once when DevTools first requests the complete set.
                auto program_type = source.type == JavaScriptSource::Type::Script
                    ? JS::ProgramType::Script
                    : JS::ProgramType::Module;
                source.breakpoint_positions = JS::breakpoint_positions_for_source(*source.source_code, program_type, source.line_number_offset);
            }
            return source.breakpoint_positions->span();
        });
}

}
