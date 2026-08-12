/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WorkletGlobalScope.h>

namespace Web::Bindings {

JS::Realm& main_world_realm(HTML::WorkletGlobalScope const& worklet_global_scope)
{
    auto wrapper = worklet_global_scope.cached_main_world_wrapper();
    VERIFY(wrapper);
    return wrapper->realm();
}

}

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkletGlobalScope);

WorkletGlobalScope::WorkletGlobalScope()
{
}

WorkletGlobalScope::~WorkletGlobalScope() = default;

JS::Realm& WorkletGlobalScope::realm() const
{
    return Bindings::main_world_realm(*this);
}

EnvironmentSettingsObject& WorkletGlobalScope::settings_object() const
{
    VERIFY(m_settings_object);
    return *m_settings_object;
}

void WorkletGlobalScope::set_settings_object(Badge<WorkletEnvironmentSettingsObject>, GC::Ref<EnvironmentSettingsObject> settings_object)
{
    m_settings_object = settings_object;
}

void WorkletGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    UniversalGlobalScopeMixin::visit_edges(visitor);
    visitor.visit(m_settings_object);
}

}
