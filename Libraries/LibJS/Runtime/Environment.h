/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

struct Variable {
    Value value;
    DeclarationKind declaration_kind;
};

#define JS_ENVIRONMENT(class_, base_class) GC_CELL(class_, base_class)

class Environment : public Cell {
    GC_CELL(Environment, Cell);

public:
    enum class InitializeBindingHint {
        Normal,
        SyncDispose,
        AsyncDispose,
    };

    virtual bool has_this_binding() const { return false; }
    virtual ThrowCompletionOr<Value> get_this_binding(VM&) const { return Value {}; }

    virtual Object* with_base_object() const { return nullptr; }

    virtual ThrowCompletionOr<bool> has_binding([[maybe_unused]] FlyString const& name, [[maybe_unused]] Optional<size_t>* out_index = nullptr) const { return false; }
    virtual ThrowCompletionOr<void> create_mutable_binding(VM&, [[maybe_unused]] FlyString const& name, [[maybe_unused]] bool can_be_deleted) { return {}; }
    virtual ThrowCompletionOr<void> create_immutable_binding(VM&, [[maybe_unused]] FlyString const& name, [[maybe_unused]] bool strict) { return {}; }
    virtual ThrowCompletionOr<void> initialize_binding(VM&, [[maybe_unused]] FlyString const& name, Value, InitializeBindingHint) { return {}; }
    virtual ThrowCompletionOr<void> set_mutable_binding(VM&, [[maybe_unused]] FlyString const& name, Value, [[maybe_unused]] bool strict) { return {}; }
    virtual ThrowCompletionOr<Value> get_binding_value(VM&, [[maybe_unused]] FlyString const& name, [[maybe_unused]] bool strict) { return Value {}; }
    virtual ThrowCompletionOr<bool> delete_binding(VM&, [[maybe_unused]] FlyString const& name) { return false; }

    // [[OuterEnv]]
    Environment* outer_environment() { return m_outer_environment; }
    Environment const* outer_environment() const { return m_outer_environment; }

    [[nodiscard]] bool is_declarative_environment() const { return m_declarative; }
    virtual bool is_global_environment() const { return false; }
    virtual bool is_function_environment() const { return false; }

    template<typename T>
    bool fast_is() const = delete;

    // This flag is set on the entire variable environment chain when direct eval() is performed.
    // It is used to disable non-local variable access caching.
    bool is_permanently_screwed_by_eval() const { return m_permanently_screwed_by_eval; }
    void set_permanently_screwed_by_eval();

protected:
    enum class IsDeclarative {
        No,
        Yes,
    };
    explicit Environment(Environment* parent, IsDeclarative = IsDeclarative::No);

    virtual void visit_edges(Visitor&) override;

private:
    bool m_permanently_screwed_by_eval { false };
    bool m_declarative { false };

    GC::Ptr<Environment> m_outer_environment;
};

}
