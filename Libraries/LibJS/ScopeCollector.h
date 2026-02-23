/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibJS/ScopeRecord.h>

namespace JS {

class Parser;

class ScopeCollector {
public:
    class ScopeHandle {
    public:
        ScopeHandle() = default;

        ~ScopeHandle()
        {
            if (m_collector)
                m_collector->close_scope();
        }

        ScopeHandle(ScopeHandle&& other)
            : m_collector(exchange(other.m_collector, nullptr))
        {
        }

        ScopeHandle& operator=(ScopeHandle&& other)
        {
            if (this != &other) {
                if (m_collector)
                    m_collector->close_scope();
                m_collector = exchange(other.m_collector, nullptr);
            }
            return *this;
        }

        ScopeHandle(ScopeHandle const&) = delete;
        ScopeHandle& operator=(ScopeHandle const&) = delete;

    private:
        friend class ScopeCollector;
        explicit ScopeHandle(ScopeCollector& collector)
            : m_collector(&collector)
        {
        }
        ScopeCollector* m_collector { nullptr };
    };

    explicit ScopeCollector(Parser& parser);

    [[nodiscard]] ScopeHandle open_program_scope(Program& program);
    [[nodiscard]] ScopeHandle open_function_scope(RefPtr<Identifier const> function_name = nullptr);
    [[nodiscard]] ScopeHandle open_block_scope(ScopeNode& node);
    [[nodiscard]] ScopeHandle open_for_loop_scope(ScopeNode& node);
    [[nodiscard]] ScopeHandle open_with_scope(ScopeNode& node);
    [[nodiscard]] ScopeHandle open_catch_scope();
    [[nodiscard]] ScopeHandle open_static_init_scope(ScopeNode& node);
    [[nodiscard]] ScopeHandle open_class_field_scope(ScopeNode& node);
    [[nodiscard]] ScopeHandle open_class_declaration_scope(RefPtr<Identifier const> class_name);

    void add_declaration(NonnullRefPtr<Declaration const> declaration);
    void add_catch_parameter(RefPtr<BindingPattern const> const& pattern, RefPtr<Identifier const> const& parameter);
    void register_identifier(NonnullRefPtr<Identifier> id, Optional<DeclarationKind> declaration_kind = {});
    void set_function_parameters(NonnullRefPtr<FunctionParameters const> parameters);
    void set_scope_node(ScopeNode* node);
    void set_contains_direct_call_to_eval();
    void set_contains_access_to_arguments_object_in_non_strict_mode();
    void set_contains_await_expression();
    void set_uses_this();
    void set_uses_new_target();
    void set_is_arrow_function();
    void set_is_function_declaration();

    bool contains_direct_call_to_eval() const;
    bool uses_this_from_environment() const;
    bool uses_this() const;
    bool contains_await_expression() const;
    bool can_have_using_declaration() const;
    ScopeRecord::ScopeType type() const;
    bool has_declaration(Utf16FlyString const& name) const;
    ScopeRecord const* last_function_scope() const;
    ScopeRecord* parent_scope();
    FunctionParameters const& function_parameters() const;

    bool has_current_scope() const { return m_current != nullptr; }

    bool has_declaration_in_current_function(Utf16FlyString const& name) const;

    struct SavedAncestorFlags {
        ScopeRecord* record;
        bool uses_this;
        bool uses_this_from_environment;
    };

    // Save/restore ancestor function scope flags around speculative parsing.
    // During speculative arrow function parsing, set_uses_this() may propagate
    // flags to ancestor function scopes. If the speculative parse fails, these
    // flags must be restored.
    [[nodiscard]] Vector<SavedAncestorFlags> save_ancestor_flags() const;
    void restore_ancestor_flags(Vector<SavedAncestorFlags> const&);

    void analyze(bool suppress_globals = false);

private:
    void open_scope(ScopeRecord::ScopeType type, ScopeNode* node, ScopeRecord::ScopeLevel level);
    void close_scope();

    void throw_identifier_declared(Utf16FlyString const& name, NonnullRefPtr<Declaration const> const& declaration);

    static void propagate_eval_poisoning(ScopeRecord& scope);
    static void resolve_identifiers(ScopeRecord& scope, bool initiated_by_eval, bool suppress_globals);
    static void hoist_functions(ScopeRecord& scope);
    static void build_function_scope_data(ScopeRecord& scope);
    void analyze_recursive(ScopeRecord& scope, bool suppress_globals);

    Parser& m_parser;
    ScopeRecord* m_current { nullptr };
    OwnPtr<ScopeRecord> m_root;
};

}
