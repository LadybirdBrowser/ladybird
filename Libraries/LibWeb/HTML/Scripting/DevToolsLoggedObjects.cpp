/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/HTML/Scripting/DevToolsLoggedObjects.h>

namespace Web::HTML {

Vector<JsonValue> DevToolsLoggedObjects::serialize_message_arguments(ReadonlySpan<JS::Value> arguments)
{
    if (m_messages_in_current_table == MESSAGES_PER_TABLE) {
        swap(m_current_table, m_previous_table);
        m_current_table.clear();
        m_messages_in_current_table = 0;
    }
    ++m_messages_in_current_table;

    Vector<JsonValue> serialized_arguments;
    serialized_arguments.ensure_capacity(arguments.size());
    for (auto argument : arguments)
        serialized_arguments.unchecked_append(m_current_table.serialize_value_as_grip(argument));
    return serialized_arguments;
}

bool DevToolsLoggedObjects::contains(u64 object_id) const
{
    return m_current_table.contains(object_id) || m_previous_table.contains(object_id);
}

ErrorOr<WebView::DebuggerObjectProperties> DevToolsLoggedObjects::properties_for_object(u64 object_id)
{
    if (m_previous_table.contains(object_id))
        return m_previous_table.properties_for_object(object_id);
    return m_current_table.properties_for_object(object_id);
}

void DevToolsLoggedObjects::clear()
{
    m_current_table.clear();
    m_previous_table.clear();
    m_messages_in_current_table = 0;
}

void DevToolsLoggedObjects::visit_edges(GC::Cell::Visitor& visitor)
{
    m_current_table.visit_edges(visitor);
    m_previous_table.visit_edges(visitor);
}

}
