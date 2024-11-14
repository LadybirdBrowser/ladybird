/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/AST.h>
#include <LibJS/Lexer.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Script);

// 16.1.5 ParseScript ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parse-script
Result<GC::Ref<Script>, Vector<ParserError>> Script::parse(StringView source_text, Realm& realm, StringView filename, HostDefined* host_defined, size_t line_number_offset)
{
    // 1. Let script be ParseText(sourceText, Script).
    auto parser = Parser(Lexer(source_text, filename, line_number_offset));
    auto script = parser.parse_program();

    // 2. If script is a List of errors, return body.
    if (parser.has_errors())
        return parser.errors();

    // 3. Return Script Record { [[Realm]]: realm, [[ECMAScriptCode]]: script, [[HostDefined]]: hostDefined }.
    return realm.heap().allocate<Script>(realm, filename, move(script), host_defined);
}

Script::Script(Realm& realm, StringView filename, NonnullRefPtr<Program> parse_node, HostDefined* host_defined)
    : m_realm(realm)
    , m_parse_node(move(parse_node))
    , m_filename(filename)
    , m_host_defined(host_defined)
{
}

Script::~Script()
{
}

void Script::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    if (m_host_defined)
        m_host_defined->visit_host_defined_self(visitor);
    for (auto const& loaded_module : m_loaded_modules)
        visitor.visit(loaded_module.module);
}

}
