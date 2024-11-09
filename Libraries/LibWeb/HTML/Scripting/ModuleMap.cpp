/*
 * Copyright (c) 2022-2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/ModuleMap.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ModuleMap);

void ModuleMap::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& it : m_values)
        visitor.visit(it.value.module_script);

    for (auto const& it : m_callbacks)
        visitor.visit(it.value);
}

bool ModuleMap::is_fetching(URL::URL const& url, ByteString const& type) const
{
    return is(url, type, EntryType::Fetching);
}

bool ModuleMap::is_failed(URL::URL const& url, ByteString const& type) const
{
    return is(url, type, EntryType::Failed);
}

bool ModuleMap::is(URL::URL const& url, ByteString const& type, EntryType entry_type) const
{
    auto value = m_values.get({ url, type });
    if (!value.has_value())
        return false;

    return value->type == entry_type;
}

Optional<ModuleMap::Entry> ModuleMap::get(URL::URL const& url, ByteString const& type) const
{
    return m_values.get({ url, type });
}

AK::HashSetResult ModuleMap::set(URL::URL const& url, ByteString const& type, Entry entry)
{
    // NOTE: Re-entering this function while firing wait_for_change callbacks is not allowed.
    VERIFY(!m_firing_callbacks);

    auto value = m_values.set({ url, type }, entry);

    auto callbacks = m_callbacks.get({ url, type });
    if (callbacks.has_value()) {
        m_firing_callbacks = true;
        for (auto const& callback : *callbacks)
            callback->function()(entry);
        m_firing_callbacks = false;
    }

    return value;
}

void ModuleMap::wait_for_change(JS::Heap& heap, URL::URL const& url, ByteString const& type, Function<void(Entry)> callback)
{
    m_callbacks.ensure({ url, type }).append(JS::create_heap_function(heap, move(callback)));
}

}
