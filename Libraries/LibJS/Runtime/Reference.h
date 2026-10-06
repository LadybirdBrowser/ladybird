/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// 6.2.5 The Reference Record Specification Type, https://tc39.es/ecma262/#sec-reference-record-specification-type
// The Reference Records that VM::resolve_binding() resolves an identifier to: unresolvable, or with an Environment
// Record as their base.
class JS_API Reference {
public:
    enum class BaseType : u8 {
        Unresolvable,
        Environment,
    };

    Reference(BaseType type, PropertyKey name, Strict strict)
        : m_name(move(name))
        , m_base_type(type)
        , m_strict(strict)
    {
        VERIFY(type == BaseType::Unresolvable);
    }

    Reference(Environment& base, Utf16FlyString referenced_name, Strict strict)
        : m_name(move(referenced_name))
        , m_base_environment(&base)
        , m_base_type(BaseType::Environment)
        , m_strict(strict)
    {
    }

    Environment& base_environment() const
    {
        VERIFY(m_base_type == BaseType::Environment);
        return *m_base_environment;
    }

    PropertyKey const& name() const { return m_name; }
    bool is_strict() const { return m_strict == Strict::Yes; }

    // 6.2.4.2 IsUnresolvableReference ( V ), https://tc39.es/ecma262/#sec-isunresolvablereference
    bool is_unresolvable() const { return m_base_type == BaseType::Unresolvable; }

    // 6.2.4.1 IsPropertyReference ( V ), https://tc39.es/ecma262/#sec-ispropertyreference
    bool is_property_reference() const { return false; }

    // Note: Non-standard helper.
    bool is_environment_reference() const
    {
        return m_base_type == BaseType::Environment;
    }

    ThrowCompletionOr<void> put_value(VM&, Value);
    ThrowCompletionOr<Value> get_value(VM&) const;
    ThrowCompletionOr<bool> delete_(VM&);

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        visitor.visit(m_base_environment);
        m_name.visit_edges(visitor);
    }

private:
    Completion throw_reference_error(VM&) const;

    PropertyKey m_name;
    GC::Ptr<Environment> m_base_environment;
    BaseType m_base_type { BaseType::Unresolvable };
    Strict m_strict { Strict::No };
};

}
