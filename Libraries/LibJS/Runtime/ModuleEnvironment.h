/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Module.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/Environment.h>

namespace JS {

// 9.1.1.5 Module Environment Records, https://tc39.es/ecma262/#sec-module-environment-records
class JS_API ModuleEnvironment final : public DeclarativeEnvironment {
    JS_ENVIRONMENT(ModuleEnvironment, DeclarativeEnvironment);
    GC_DECLARE_ALLOCATOR(ModuleEnvironment);

public:
    // Note: Module Environment Records support all of the declarative Environment Record methods listed
    //       in Table 18 and share the same specifications for all of those methods except for
    //       GetBindingValue, DeleteBinding, HasThisBinding and GetThisBinding.
    //       In addition, module Environment Records support the methods listed in Table 24.
    virtual ThrowCompletionOr<Value> get_binding_value(VM&, Utf16FlyString const& name, bool strict) override;
    virtual ThrowCompletionOr<bool> delete_binding(VM&, Utf16FlyString const& name) override;
    virtual bool has_this_binding() const final { return true; }
    virtual ThrowCompletionOr<Value> get_this_binding(VM&) const final;
    ThrowCompletionOr<void> create_import_binding(Utf16FlyString name, Module* module, Utf16FlyString binding_name);

private:
    explicit ModuleEnvironment(Environment* outer_environment);

    virtual void visit_edges(Visitor&) override;

    struct IndirectBinding {
        Utf16FlyString name;
        GC::Ptr<Module> module;
        Utf16FlyString binding_name;
    };
    IndirectBinding const* get_indirect_binding(Utf16FlyString const& name) const;

    virtual Optional<BindingAndIndex> find_binding_and_index(Utf16FlyString const& name) const override;

    // FIXME: Since we always access this via the name this could be a map.
    Vector<IndirectBinding> m_indirect_bindings;
};

}
