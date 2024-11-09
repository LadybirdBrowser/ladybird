/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>

namespace JS {

class FunctionEnvironment final : public DeclarativeEnvironment {
    JS_ENVIRONMENT(FunctionEnvironment, DeclarativeEnvironment);
    JS_DECLARE_ALLOCATOR(FunctionEnvironment);

public:
    enum class ThisBindingStatus : u8 {
        Lexical,
        Initialized,
        Uninitialized,
    };

    virtual ~FunctionEnvironment() override = default;

    ThisBindingStatus this_binding_status() const { return m_this_binding_status; }
    void set_this_binding_status(ThisBindingStatus status) { m_this_binding_status = status; }

    ECMAScriptFunctionObject& function_object() { return *m_function_object; }
    ECMAScriptFunctionObject const& function_object() const { return *m_function_object; }
    void set_function_object(ECMAScriptFunctionObject& function) { m_function_object = &function; }

    Value new_target() const { return m_new_target; }
    void set_new_target(Value new_target)
    {
        VERIFY(!new_target.is_empty());
        m_new_target = new_target;
    }

    // Abstract operations
    ThrowCompletionOr<Value> get_super_base() const;
    bool has_super_binding() const;
    virtual bool has_this_binding() const override;
    virtual ThrowCompletionOr<Value> get_this_binding(VM&) const override;
    ThrowCompletionOr<Value> bind_this_value(VM&, Value);

private:
    explicit FunctionEnvironment(Environment* parent_environment);

    virtual bool is_function_environment() const override { return true; }
    virtual void visit_edges(Visitor&) override;

    Value m_this_value;                                                           // [[ThisValue]]
    ThisBindingStatus m_this_binding_status { ThisBindingStatus::Uninitialized }; // [[ThisBindingStatus]]
    GCPtr<ECMAScriptFunctionObject> m_function_object;                            // [[FunctionObject]]
    Value m_new_target { js_undefined() };                                        // [[NewTarget]]
};

template<>
inline bool Environment::fast_is<FunctionEnvironment>() const { return is_function_environment(); }

}
