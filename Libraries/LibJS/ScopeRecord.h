/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibJS/AST.h>

namespace JS {

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

struct IdentifierGroup {
    bool captured_by_nested_function { false };
    bool used_inside_with_statement { false };
    Vector<NonnullRefPtr<Identifier>> identifiers;
    Optional<DeclarationKind> declaration_kind;
};

struct ScopeRecord {
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

    // NOTE: We really only need ModuleTopLevel and NotModuleTopLevel as the only
    //       difference seems to be in https://tc39.es/ecma262/#sec-static-semantics-varscopeddeclarations
    //       where ModuleItemList only does the VarScopedDeclaration and not the
    //       TopLevelVarScopedDeclarations.
    enum class ScopeLevel {
        NotTopLevel,
        ScriptTopLevel,
        ModuleTopLevel,
        FunctionTopLevel,
        StaticInitTopLevel,
    };

    ScopeType type;
    ScopeLevel level;
    RefPtr<ScopeNode> ast_node;

    HashMap<Utf16FlyString, ScopeVariable> variables;
    HashMap<Utf16FlyString, IdentifierGroup> identifier_groups;
    Vector<NonnullRefPtr<FunctionDeclaration const>> functions_to_hoist;

    RefPtr<FunctionParameters const> function_parameters;

    bool contains_access_to_arguments_object_in_non_strict_mode { false };
    bool contains_direct_call_to_eval { false };
    bool contains_await_expression { false };
    bool screwed_by_eval_in_scope_chain { false };
    bool eval_in_current_function { false };
    bool uses_this_from_environment { false };
    bool uses_this { false };
    bool is_arrow_function { false };
    bool is_function_declaration { false };

    ScopeRecord* parent { nullptr };
    ScopeRecord* top_level { nullptr };
    Vector<NonnullOwnPtr<ScopeRecord>> children;

    bool is_top_level() const { return level != ScopeLevel::NotTopLevel; }

    bool has_variable_with_flags(Utf16FlyString const& name, u16 flags) const
    {
        auto it = variables.find(name);
        return it != variables.end() && (it->value.flags & flags);
    }

    ScopeRecord const* last_function_scope() const
    {
        for (auto const* scope = this; scope; scope = scope->parent) {
            if (scope->type == ScopeType::Function || scope->type == ScopeType::ClassStaticInit)
                return scope;
        }
        return nullptr;
    }
};

}
