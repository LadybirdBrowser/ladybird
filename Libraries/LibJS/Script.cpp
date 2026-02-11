/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/AST.h>
#include <LibJS/Lexer.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Script);

// 16.1.5 ParseScript ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parse-script
Result<GC::Ref<Script>, Vector<ParserError>> Script::parse(StringView source_text, Realm& realm, StringView filename, HostDefined* host_defined, size_t line_number_offset)
{
    // 1. Let script be ParseText(sourceText, Script).
    auto parser = Parser(Lexer(SourceCode::create(String::from_utf8(filename).release_value_but_fixme_should_propagate_errors(), Utf16String::from_utf8(source_text)), line_number_offset));
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
    auto& vm = realm.vm();
    auto& program = *m_parse_node;

    m_is_strict_mode = program.is_strict_mode();

    // Pre-compute lexically declared names (GDI step 3).
    MUST(program.for_each_lexically_declared_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
        m_lexical_names.append(identifier.string());
        return {};
    }));

    // Pre-compute var declared names (GDI step 4).
    MUST(program.for_each_var_declared_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
        m_var_names.append(identifier.string());
        return {};
    }));

    // Pre-compute functions to initialize and declared function names (GDI steps 7-8).
    MUST(program.for_each_var_function_declaration_in_reverse_order([&](FunctionDeclaration const& function) -> ThrowCompletionOr<void> {
        auto function_name = function.name();
        if (m_declared_function_names.set(function_name) != AK::HashSetResult::InsertedNewEntry)
            return {};
        m_functions_to_initialize.append({ function.ensure_shared_data(vm), function_name });
        return {};
    }));

    // Pre-compute var scoped variable names (GDI step 10).
    MUST(program.for_each_var_scoped_variable_declaration([&](VariableDeclaration const& declaration) {
        return declaration.for_each_bound_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
            m_var_scoped_names.append(identifier.string());
            return {};
        });
    }));

    // Pre-compute AnnexB candidates (GDI step 13).
    if (!m_is_strict_mode) {
        MUST(program.for_each_function_hoistable_with_annexB_extension([&](FunctionDeclaration& function_declaration) -> ThrowCompletionOr<void> {
            m_annex_b_candidates.append(function_declaration);
            return {};
        }));
    }

    // Pre-compute lexical bindings (GDI step 15).
    MUST(program.for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
        return declaration.for_each_bound_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
            m_lexical_bindings.append({ identifier.string(), declaration.is_constant_declaration() });
            return {};
        });
    }));
}

Script::~Script()
{
}

void Script::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_executable);
    for (auto const& function : m_functions_to_initialize)
        visitor.visit(function.shared_data);
    if (m_host_defined)
        m_host_defined->visit_host_defined_self(visitor);
    for (auto const& loaded_module : m_loaded_modules)
        visitor.visit(loaded_module.module);
}

}
