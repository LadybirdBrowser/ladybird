/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/HTML/Scripting/DevToolsLoggedObjects.h>

namespace Web::HTML {

void DevToolsLoggedObjects::start_message()
{
    if (m_messages_in_current_table == MESSAGES_PER_TABLE) {
        swap(m_current_table, m_previous_table);
        m_current_table.clear();
        m_messages_in_current_table = 0;
    }
    ++m_messages_in_current_table;
}

Vector<JsonValue> DevToolsLoggedObjects::serialize_message_arguments(ReadonlySpan<JS::Value> arguments)
{
    start_message();

    Vector<JsonValue> serialized_arguments;
    serialized_arguments.ensure_capacity(arguments.size());
    for (auto argument : arguments)
        serialized_arguments.unchecked_append(m_current_table.serialize_value_as_grip(argument));
    return serialized_arguments;
}

// The value of an own data property, read without running any script.
static Optional<JS::Value> own_data_property(JS::Object& object, JS::PropertyKey const& key)
{
    auto descriptor = object.internal_get_own_property(key);
    if (descriptor.is_error() || !descriptor.value().has_value() || !descriptor.value()->is_data_descriptor())
        return {};
    return descriptor.value()->value.value_or(JS::js_undefined());
}

// The arguments of a console.table() message, in the shape a client builds its table from: The console's rows go in an
// ownProperties field of the first argument's grip, keyed by each row's index, with each row's own cells in an
// ownProperties field of its grip.
// https://console.spec.whatwg.org/#table
Vector<JsonValue> DevToolsLoggedObjects::serialize_console_table_arguments(JS::VM& vm, JS::Object& table_data)
{
    start_message();

    auto rows_value = own_data_property(table_data, JS::PropertyKey { "rows"_utf16_fly_string });
    if (!rows_value.has_value() || !rows_value->is_object())
        return {};
    auto& rows = rows_value->as_object();

    JsonObject table_rows;
    for (size_t row_index = 0; row_index < MAX_CONSOLE_TABLE_ROWS; ++row_index) {
        auto row_value = own_data_property(rows, JS::PropertyKey { row_index });
        if (!row_value.has_value() || !row_value->is_object())
            break;
        auto& row = row_value->as_object();

        auto keys = row.internal_own_property_keys();
        if (keys.is_error())
            continue;

        JsonObject cells;
        String index;
        for (auto key_value : keys.value()) {
            auto key = JS::PropertyKey::from_value(vm, key_value);
            if (key.is_error())
                continue;
            auto cell = own_data_property(row, key.value());
            if (!cell.has_value())
                continue;

            auto name = key_value.to_utf16_string_without_side_effects().to_utf8();
            if (name == "(index)"sv) {
                index = cell->to_utf16_string_without_side_effects().to_utf8();
                continue;
            }

            JsonObject descriptor;
            descriptor.set("value"sv, m_current_table.serialize_value_as_grip(*cell));
            cells.set(name, move(descriptor));
        }

        // A row's preview would repeat its cells, along with its index.
        auto row_grip = m_current_table.serialize_value_as_grip(&row);
        row_grip.as_object().remove("preview"sv);
        row_grip.as_object().set("ownPropertyLength"sv, cells.size());
        row_grip.as_object().set("ownProperties"sv, move(cells));

        JsonObject descriptor;
        descriptor.set("value"sv, move(row_grip));
        table_rows.set(index, move(descriptor));
    }

    auto table_grip = m_current_table.serialize_value_as_grip(&rows);
    table_grip.as_object().set("ownProperties"sv, move(table_rows));
    table_grip.as_object().set("preview"sv, JsonValue {});

    Vector<JsonValue> arguments;
    arguments.append(move(table_grip));
    return arguments;
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
