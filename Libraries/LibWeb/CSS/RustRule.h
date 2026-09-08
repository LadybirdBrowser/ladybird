/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/RustDescriptorBlock.h>
#include <LibWeb/CSS/RustFontFeatureValues.h>
#include <LibWeb/CSS/RustImportRule.h>
#include <LibWeb/CSS/RustMediaList.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

class RustRule {
public:
    using Type = Parser::ValueParserFFI::NativeRuleType;
    using Payload = Parser::ValueParserFFI::FfiRulePayload;

    explicit RustRule(RustDeclarationBlock const& declarations)
        : m_rule(Parser::ValueParserFFI::rust_nested_declarations_create(declarations.handle()))
    {
    }
    explicit RustRule(Parser::ValueParserFFI::NativeRule const* rule)
        : m_rule(Parser::ValueParserFFI::rust_rule_retain(rule))
    {
    }
    RustRule(RustRule const& other)
        : RustRule(other.m_rule)
    {
    }
    RustRule(RustRule&& other)
        : m_rule(exchange(other.m_rule, nullptr))
    {
    }
    ~RustRule() { Parser::ValueParserFFI::rust_rule_release(m_rule); }

    u64 identity() const { return Parser::ValueParserFFI::rust_rule_identity(m_rule); }
    Optional<Utf16String> internal_layer_name() const
    {
        Optional<Utf16String> name;
        Parser::ValueParserFFI::rust_rule_internal_layer_name(m_rule, &name, [](void* context, u16 const* units, size_t length) {
            *static_cast<Optional<Utf16String>*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(units), length });
        });
        return name;
    }
    Type type() const { return Parser::ValueParserFFI::rust_rule_type(m_rule); }
    Payload payload() const { return Parser::ValueParserFFI::rust_rule_payload(m_rule); }
    Optional<RustDeclarationBlock> declarations() const
    {
        if (auto* declarations = Parser::ValueParserFFI::rust_rule_declarations(m_rule))
            return RustDeclarationBlock { declarations };
        return {};
    }
    size_t external_memory_size() const { return Parser::ValueParserFFI::rust_rule_external_memory_size(m_rule); }
    Parser::ValueParserFFI::NativeRule const* handle() const { return m_rule; }

private:
    Parser::ValueParserFFI::NativeRule const* m_rule;
};

class RustCompiledFunction {
public:
    // Adopt immutable compilation output, independent of live CSSOM owners.
    explicit RustCompiledFunction(Parser::ValueParserFFI::CompiledFunction const* function)
        : m_function(function)
    {
    }
    RustCompiledFunction(RustCompiledFunction const& other)
        : m_function(Parser::ValueParserFFI::rust_compiled_function_retain(other.m_function))
    {
    }
    RustCompiledFunction(RustCompiledFunction&& other)
        : m_function(exchange(other.m_function, nullptr))
    {
    }
    ~RustCompiledFunction() { Parser::ValueParserFFI::rust_compiled_function_release(m_function); }
    u64 identity() const { return Parser::ValueParserFFI::rust_compiled_function_identity(m_function); }
    Parser::ValueParserFFI::FunctionSignature const* signature() const { return Parser::ValueParserFFI::rust_compiled_function_signature(m_function); }
    auto* handle() const { return m_function; }

private:
    Parser::ValueParserFFI::CompiledFunction const* m_function;
};

// A callback-scoped view of shared parsed data or an explicitly mutated rule.
class RustRuleView {
public:
    explicit RustRuleView(Parser::ValueParserFFI::NativeRuleView const& view)
        : m_view(view)
    {
    }
    u64 identity() const { return Parser::ValueParserFFI::rust_rule_view_identity(&m_view); }
    RustRule::Type type() const { return Parser::ValueParserFFI::rust_rule_view_type(&m_view); }
    auto* property() const { return Parser::ValueParserFFI::rust_rule_view_property(&m_view); }
    auto* container() const { return Parser::ValueParserFFI::rust_rule_view_container(&m_view); }
    RustDescriptorBlock descriptors() const { return RustDescriptorBlock { Parser::ValueParserFFI::rust_rule_view_descriptors(&m_view) }; }
    RustImportRule import_rule() const { return RustImportRule { Parser::ValueParserFFI::rust_rule_view_import(&m_view) }; }
    RustMediaList import_media() const { return RustMediaList { Parser::ValueParserFFI::rust_rule_view_import_media(&m_view) }; }
    RustFontFeatureValuesSnapshot font_feature_values() const { return RustFontFeatureValuesSnapshot { Parser::ValueParserFFI::rust_rule_view_font_feature_values(&m_view) }; }
    RustCompiledFunction compile_function() const { return RustCompiledFunction { Parser::ValueParserFFI::rust_rule_view_compile_function(&m_view) }; }
    Utf16String name() const
    {
        Utf16String name;
        Parser::ValueParserFFI::rust_rule_view_name(&m_view, &name, [](void* context, u16 const* units, size_t length) {
            *static_cast<Utf16String*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(units), length });
        });
        return name;
    }
    void for_each_keyframe(Function<void(ReadonlySpan<double>, RustDeclarationBlockSnapshot const&)> const& callback) const
    {
        Parser::ValueParserFFI::rust_rule_view_keyframes(&m_view, &callback, [](void const* context, double const* keys, size_t count, Parser::ValueParserFFI::DeclarationBlockData const* declarations) {
            RustDeclarationBlockSnapshot snapshot { Parser::ValueParserFFI::rust_declaration_data_retain(declarations) };
            (*static_cast<Function<void(ReadonlySpan<double>, RustDeclarationBlockSnapshot const&)> const*>(context))({ keys, count }, snapshot);
        });
    }

private:
    Parser::ValueParserFFI::NativeRuleView const& m_view;
};

class RustNamespaceContext {
public:
    RustNamespaceContext() = default;
    explicit RustNamespaceContext(Parser::ValueParserFFI::NativeNamespaceContext const* context)
        : m_context(context)
    {
    }
    RustNamespaceContext(RustNamespaceContext const& other)
        : m_context(Parser::ValueParserFFI::rust_namespace_context_retain(other.m_context))
    {
    }
    RustNamespaceContext(RustNamespaceContext&& other)
        : m_context(exchange(other.m_context, nullptr))
    {
    }
    RustNamespaceContext& operator=(RustNamespaceContext other)
    {
        swap(m_context, other.m_context);
        return *this;
    }
    ~RustNamespaceContext() { Parser::ValueParserFFI::rust_namespace_context_release(m_context); }
    auto* handle() const { return m_context; }

private:
    Parser::ValueParserFFI::NativeNamespaceContext const* m_context { nullptr };
};

class RustRuleList {
    AK_MAKE_NONCOPYABLE(RustRuleList);

public:
    enum class Ownership {
        Adopt,
        Borrow,
    };

    RustRuleList()
        : m_list(Parser::ValueParserFFI::rust_rule_list_create())
    {
    }
    // Adopt an owned reference returned by Rust.
    explicit RustRuleList(Parser::ValueParserFFI::NativeRuleList const* list, Ownership ownership = Ownership::Adopt)
        : m_list(list)
        , m_ownership(ownership)
    {
    }
    RustRuleList(RustRuleList&& other)
        : m_list(exchange(other.m_list, nullptr))
        , m_ownership(exchange(other.m_ownership, Ownership::Adopt))
    {
    }
    ~RustRuleList()
    {
        if (m_ownership == Ownership::Adopt)
            Parser::ValueParserFFI::rust_rule_list_release(m_list);
    }
    size_t size() const { return Parser::ValueParserFFI::rust_rule_list_count(m_list); }
    void for_each_rule(Function<bool(RustRuleView const&)> const& callback) const
    {
        Parser::ValueParserFFI::rust_rule_list_visit_rule_data(m_list, &callback, [](void const* context, Parser::ValueParserFFI::NativeRuleView const* rule) {
            return (*static_cast<Function<bool(RustRuleView const&)> const*>(context))(RustRuleView { *rule });
        });
    }
    Vector<size_t> path_to_rule(u64 identity) const
    {
        Vector<size_t> path;
        Parser::ValueParserFFI::rust_rule_list_find_path(m_list, identity, &path, [](void* context, size_t const* indices, size_t count) {
            static_cast<Vector<size_t>*>(context)->append(indices, count);
        });
        return path;
    }
    RustRule at(size_t index) const { return RustRule { Parser::ValueParserFFI::rust_rule_list_at(m_list, index) }; }
    void replace(RustRuleList const& source) { Parser::ValueParserFFI::rust_rule_list_replace(m_list, source.handle()); }
    void remove_imports() { Parser::ValueParserFFI::rust_rule_list_remove_imports(m_list); }
    u64 identity_at(size_t index) const { return Parser::ValueParserFFI::rust_rule_list_identity_at(m_list, index); }
    void insert(size_t index, RustRule const& rule) { Parser::ValueParserFFI::rust_rule_list_insert(m_list, index, rule.handle()); }
    void remove(size_t index) { Parser::ValueParserFFI::rust_rule_list_remove(m_list, index); }
    void clear() { Parser::ValueParserFFI::rust_rule_list_clear(m_list); }
    size_t external_memory_size() const { return Parser::ValueParserFFI::rust_rule_list_external_memory_size(m_list); }
    Parser::ValueParserFFI::NativeRuleList const* handle() const { return m_list; }

private:
    Parser::ValueParserFFI::NativeRuleList const* m_list;
    Ownership m_ownership { Ownership::Adopt };
};

}
