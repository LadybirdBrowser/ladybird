/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>

namespace Web::WebIDL {

class ParameterizedType;
class UnionType;

class Type : public RefCounted<Type> {
public:
    enum class Kind {
        Plain,
        Parameterized,
        Union,
    };

    Type(ByteString name, bool nullable)
        : m_kind(Kind::Plain)
        , m_name(move(name))
        , m_nullable(nullable)
    {
    }

    Type(Kind kind, ByteString name, bool nullable)
        : m_kind(kind)
        , m_name(move(name))
        , m_nullable(nullable)
    {
    }

    virtual ~Type() = default;

    bool is_plain() const { return m_kind == Kind::Plain; }

    bool is_parameterized() const { return m_kind == Kind::Parameterized; }
    ParameterizedType const& as_parameterized() const;
    ParameterizedType& as_parameterized();

    bool is_union() const { return m_kind == Kind::Union; }
    UnionType const& as_union() const;
    UnionType& as_union();

    ByteString const& name() const { return m_name; }

    bool is_nullable() const { return m_nullable; }

    bool includes_nullable_type() const;
    bool includes_undefined() const;

    bool is_any() const { return is_plain() && m_name == "any"; }
    bool is_undefined() const { return is_plain() && m_name == "undefined"; }
    bool is_boolean() const { return is_plain() && m_name == "boolean"; }
    bool is_bigint() const { return is_plain() && m_name == "bigint"; }
    bool is_object() const { return is_plain() && m_name == "object"; }
    bool is_symbol() const { return is_plain() && m_name == "symbol"; }
    bool is_string() const { return is_plain() && m_name.is_one_of("ByteString", "DOMString", "Utf16DOMString", "USVString", "Utf16USVString"); }
    bool is_integer() const { return is_plain() && m_name.is_one_of("byte", "octet", "short", "unsigned short", "long", "unsigned long", "long long", "unsigned long long"); }
    bool is_numeric() const { return is_plain() && (is_integer() || is_floating_point()); }
    bool is_sequence() const { return is_parameterized() && m_name == "sequence"; }

    bool is_buffer() const;
    bool is_typed_array() const;
    bool is_buffer_view() const;
    bool is_buffer_source() const;

    bool is_restricted_floating_point() const { return m_name.is_one_of("float", "double"); }
    bool is_unrestricted_floating_point() const { return m_name.is_one_of("unrestricted float", "unrestricted double"); }
    bool is_floating_point() const { return is_restricted_floating_point() || is_unrestricted_floating_point(); }

private:
    Kind m_kind;
    ByteString m_name;
    bool m_nullable { false };
};

class ParameterizedType : public Type {
public:
    ParameterizedType(ByteString name, bool nullable, Vector<NonnullRefPtr<Type const>> parameters)
        : Type(Kind::Parameterized, move(name), nullable)
        , m_parameters(move(parameters))
    {
    }

    virtual ~ParameterizedType() override = default;

    Vector<NonnullRefPtr<Type const>> const& parameters() const { return m_parameters; }

private:
    Vector<NonnullRefPtr<Type const>> m_parameters;
};

class UnionType : public Type {
public:
    UnionType(ByteString name, bool nullable, Vector<NonnullRefPtr<Type const>> member_types)
        : Type(Kind::Union, move(name), nullable)
        , m_member_types(move(member_types))
    {
    }

    virtual ~UnionType() override = default;

    Vector<NonnullRefPtr<Type const>> const& member_types() const { return m_member_types; }

    Vector<NonnullRefPtr<Type const>> flattened_member_types() const;
    size_t number_of_nullable_member_types() const;

private:
    Vector<NonnullRefPtr<Type const>> m_member_types;
};

enum class Optionality {
    Required,
    Optional,
    Variadic,
};

class EffectiveOverloadSet {
public:
    struct Item {
        int callable_id;
        Vector<NonnullRefPtr<Type const>> types;
        Vector<Optionality> optionality_values;
    };

    EffectiveOverloadSet(Vector<Item> items, size_t distinguishing_argument_index)
        : m_items(move(items))
        , m_distinguishing_argument_index(distinguishing_argument_index)
    {
    }

    Vector<Item>& items() { return m_items; }
    Vector<Item> const& items() const { return m_items; }

    Item const& only_item() const
    {
        VERIFY(m_items.size() == 1);
        return m_items[0];
    }

    bool is_empty() const { return m_items.is_empty(); }
    size_t size() const { return m_items.size(); }

    size_t distinguishing_argument_index() const { return m_distinguishing_argument_index; }

    template<typename Matches>
    bool has_overload_with_matching_argument_at_index(size_t index, Matches matches)
    {
        for (size_t i = 0; i < m_items.size(); ++i) {
            auto const& item = m_items[i];
            if (matches(item.types[index], item.optionality_values[index])) {
                m_last_matching_item_index = i;
                return true;
            }
        }
        m_last_matching_item_index = {};
        return false;
    }

    void remove_all_other_entries();

private:
    Vector<Item> m_items;
    size_t m_distinguishing_argument_index { 0 };
    Optional<size_t> m_last_matching_item_index;
};

}
