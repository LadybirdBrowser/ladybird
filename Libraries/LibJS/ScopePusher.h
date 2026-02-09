/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibJS/AST.h>

namespace JS {

class Parser;

struct ScopeVariable {
    enum Flag : u16 {
        None = 0,
        IsVar = 1 << 0,
        IsLexical = 1 << 1,
        IsFunction = 1 << 2,
        IsCatchParameter = 1 << 3,
        IsForbiddenLexical = 1 << 4,
        IsForbiddenVar = 1 << 5,
        IsBound = 1 << 6,
        IsParameterCandidate = 1 << 7,
    };

    u16 flags { 0 };
    Identifier const* var_identifier { nullptr };
    RefPtr<FunctionDeclaration const> function_declaration;

    bool has_flag(u16 flag) const { return flags & flag; }
};

class ScopePusher {

    // NOTE: We really only need ModuleTopLevel and NotModuleTopLevel as the only
    //       difference seems to be in https://tc39.es/ecma262/#sec-static-semantics-varscopeddeclarations
    //       where ModuleItemList only does the VarScopedDeclaration and not the
    //       TopLevelVarScopedDeclarations.
    enum class ScopeLevel {
        NotTopLevel,
        ScriptTopLevel,
        ModuleTopLevel,
        FunctionTopLevel,
        StaticInitTopLevel
    };

public:
    enum class ScopeType {
        Function,
        Program,
        Block,
        ForLoop,
        With,
        Catch,
        ClassStaticInit,
        ClassField,
        ClassDeclaration,
    };

    static ScopePusher function_scope(Parser& parser, RefPtr<Identifier const> function_name = nullptr);
    static ScopePusher program_scope(Parser& parser, Program& program);
    static ScopePusher block_scope(Parser& parser, ScopeNode& node);
    static ScopePusher for_loop_scope(Parser& parser, ScopeNode& node);
    static ScopePusher with_scope(Parser& parser, ScopeNode& node);
    static ScopePusher catch_scope(Parser& parser);
    static ScopePusher static_init_block_scope(Parser& parser, ScopeNode& node);
    static ScopePusher class_field_scope(Parser& parser, ScopeNode& node);
    static ScopePusher class_declaration_scope(Parser& parser, RefPtr<Identifier const> class_name);

    ~ScopePusher();

    ScopeType type() const { return m_type; }

    void add_declaration(NonnullRefPtr<Declaration const> declaration);
    void add_catch_parameter(RefPtr<BindingPattern const> const& pattern, RefPtr<Identifier const> const& parameter);

    ScopePusher const* last_function_scope() const;

    auto const& function_parameters() const { return *m_function_parameters; }

    ScopePusher* parent_scope() { return m_parent_scope; }
    ScopePusher const* parent_scope() const { return m_parent_scope; }

    [[nodiscard]] bool has_declaration(Utf16FlyString const& name) const;

    bool contains_direct_call_to_eval() const { return m_contains_direct_call_to_eval; }
    void set_contains_direct_call_to_eval();
    void set_contains_access_to_arguments_object_in_non_strict_mode() { m_contains_access_to_arguments_object_in_non_strict_mode = true; }
    void set_scope_node(ScopeNode* node) { m_node = node; }
    void set_function_parameters(NonnullRefPtr<FunctionParameters const> parameters);

    void set_contains_await_expression() { m_contains_await_expression = true; }
    bool contains_await_expression() const { return m_contains_await_expression; }

    bool can_have_using_declaration() const { return m_scope_level != ScopeLevel::ScriptTopLevel; }

    void register_identifier(NonnullRefPtr<Identifier> id, Optional<DeclarationKind> declaration_kind = {});

    bool uses_this() const { return m_uses_this; }
    bool uses_this_from_environment() const { return m_uses_this_from_environment; }

    void set_uses_this();
    void set_uses_new_target();
    void set_is_arrow_function() { m_is_arrow_function = true; }
    void set_is_function_declaration() { m_is_function_declaration = true; }

private:
    ScopePusher(Parser& parser, ScopeNode* node, ScopeLevel scope_level, ScopeType type);

    bool is_top_level() { return m_scope_level != ScopeLevel::NotTopLevel; }

    void propagate_flags_to_parent();
    void propagate_eval_poisoning();
    void resolve_identifiers();
    void hoist_functions();
    void build_function_scope_data();

    void throw_identifier_declared(Utf16FlyString const& name, NonnullRefPtr<Declaration const> const& declaration);

    bool has_variable_with_flags(Utf16FlyString const& name, u16 flags) const
    {
        auto it = m_variables.find(name);
        return it != m_variables.end() && (it->value.flags & flags);
    }

    Parser& m_parser;
    ScopeNode* m_node { nullptr };
    ScopeLevel m_scope_level { ScopeLevel::NotTopLevel };
    ScopeType m_type;

    ScopePusher* m_parent_scope { nullptr };
    ScopePusher* m_top_level_scope { nullptr };

    HashMap<Utf16FlyString, ScopeVariable> m_variables;
    Vector<NonnullRefPtr<FunctionDeclaration const>> m_functions_to_hoist;

    struct IdentifierGroup {
        bool captured_by_nested_function { false };
        bool used_inside_with_statement { false };
        Vector<NonnullRefPtr<Identifier>> identifiers;
        Optional<DeclarationKind> declaration_kind;
    };
    HashMap<Utf16FlyString, IdentifierGroup> m_identifier_groups;

    RefPtr<FunctionParameters const> m_function_parameters;

    bool m_contains_access_to_arguments_object_in_non_strict_mode { false };
    bool m_contains_direct_call_to_eval { false };
    bool m_contains_await_expression { false };
    bool m_screwed_by_eval_in_scope_chain { false };

    // Tracks eval within the current function (propagates through block scopes but not across function boundaries).
    // Used for global identifier marking - eval can't inject vars into parent function scopes.
    bool m_eval_in_current_function { false };

    // Function uses this binding from function environment if:
    // 1. It's an arrow function or establish parent scope for an arrow function
    // 2. Uses new.target
    bool m_uses_this_from_environment { false };
    bool m_uses_this { false };
    bool m_is_arrow_function { false };

    bool m_is_function_declaration { false };
};

}
