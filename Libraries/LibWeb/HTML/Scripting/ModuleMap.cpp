/*
 * Copyright (c) 2022-2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/ModuleMap.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ModuleMap);

void ModuleMap::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& it : m_values)
        it.value.visit(
            [&](GC::Ref<ModuleScript> module_script) { visitor.visit(module_script); },
            [&](CallbackList const& callbacks) { visitor.visit(callbacks); });
}

Optional<ModuleMap::Entry const&> ModuleMap::get(URL::URL const& url, Utf16View type) const
{
    return m_values.get({ url, type });
}

void ModuleMap::set(URL::URL const& url, Utf16View type, CallbackList callbacks)
{
    VERIFY(!m_values.contains({ url, type }));
    m_values.set({ url, type }, move(callbacks));
}

void ModuleMap::append(URL::URL const& url, Utf16View type, CallbackFunction callback)
{
    auto entry = m_values.find({ url, type });
    VERIFY(entry != m_values.end());
    entry->value.get<CallbackList>().append(move(callback));
}

void ModuleMap::complete_fetch(URL::URL const& url, Utf16View type, GC::Ptr<ModuleScript> module_script)
{
    auto entry = m_values.take({ url, type });
    VERIFY(entry.has_value());
    auto value = entry.release_value();
    auto callbacks_to_invoke = move(value.get<CallbackList>());

    Vector<GC::Root<GC::Function<void(GC::Ptr<ModuleScript>)>>> callbacks;
    callbacks.ensure_capacity(callbacks_to_invoke.size());
    for (auto const& callback : callbacks_to_invoke)
        callbacks.unchecked_append(GC::make_root(callback));

    if (module_script)
        m_values.set({ url, type }, GC::Ref { *module_script });

    for (auto const& callback : callbacks)
        callback->function()(module_script);
}

}
