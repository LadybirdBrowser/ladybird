/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ContainerQuery.h"
#include <AK/NonnullRefPtr.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

NonnullRefPtr<ContainerConditions> ContainerConditions::create(Parser::ValueParserFFI::ContainerConditionsData const* data)
{
    return adopt_ref(*new ContainerConditions(data));
}

ContainerConditions::ContainerConditions(Parser::ValueParserFFI::ContainerConditionsData const* data)
    : m_data(Parser::ValueParserFFI::rust_container_conditions_retain(data))
{
    VERIFY(m_data);
}

ContainerConditions::~ContainerConditions()
{
    Parser::ValueParserFFI::rust_container_conditions_release(m_data);
}

Vector<ContainerConditions::Condition> const& ContainerConditions::entries() const
{
    if (!m_entries.has_value()) {
        Vector<Condition> entries;
        auto count = Parser::ValueParserFFI::rust_container_conditions_count(m_data);
        entries.ensure_capacity(count);
        for (size_t index = 0; index < count; ++index) {
            auto name = Parser::ValueParserFFI::rust_container_conditions_name(m_data, index);
            auto const* query = Parser::ValueParserFFI::rust_container_conditions_query(m_data, index);
            Condition condition;
            // A container name is a nonempty custom identifier; an empty view means it is absent.
            if (name.length)
                condition.container_name = Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(name.utf16), name.length });
            if (query)
                condition.container_query = ContainerQuery::create(RustQueryHandle::retained(query));
            entries.unchecked_append(move(condition));
        }
        m_entries = move(entries);
    }
    return *m_entries;
}

NonnullRefPtr<ContainerQuery> ContainerQuery::create(RustQueryHandle handle)
{
    return adopt_ref(*new ContainerQuery(move(handle)));
}

ContainerQuery::ContainerQuery(RustQueryHandle handle)
    : m_rust_query_handle(move(handle))
{
}

Utf16String ContainerQuery::to_string() const
{
    Utf16String serialized;
    auto set_serialized_query = [](void* context, u16 const* code_units, size_t length) {
        *static_cast<Utf16String*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(code_units), length });
    };
    VERIFY(Parser::ValueParserFFI::css_query_serialize_condition(m_rust_query_handle.data(), &serialized, set_serialized_query));
    return serialized;
}

}
