/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <AK/Tuple.h>
#include <AK/TypeCasts.h>

namespace IDL {

template<typename FunctionType>
static size_t get_function_shortest_length(FunctionType& function)
{
    size_t length = 0;
    for (auto& parameter : function.parameters) {
        if (!parameter.optional && !parameter.variadic)
            length++;
    }
    return length;
}

enum class SequenceStorageType {
    Vector,     // Used to safely store non-JS values
    RootVector, // Used to safely store JS::Value and anything that inherits JS::Cell, e.g. JS::Object
};

struct CppType {
    ByteString name;
    SequenceStorageType sequence_storage_type;
};

class ParameterizedType;
class UnionType;
class Interface;

class Type : public RefCounted<Type> {
public:
    enum class Kind {
        Plain, // AKA, Type.
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

    Kind kind() const { return m_kind; }

    bool is_plain() const { return m_kind == Kind::Plain; }

    bool is_parameterized() const { return m_kind == Kind::Parameterized; }
    ParameterizedType const& as_parameterized() const;
    ParameterizedType& as_parameterized();

    bool is_union() const { return m_kind == Kind::Union; }
    UnionType const& as_union() const;
    UnionType& as_union();

    ByteString const& name() const { return m_name; }

    bool is_nullable() const { return m_nullable; }
    void set_nullable(bool value) { m_nullable = value; }

    // https://webidl.spec.whatwg.org/#dfn-includes-a-nullable-type
    bool includes_nullable_type() const;

    // -> https://webidl.spec.whatwg.org/#dfn-includes-undefined
    bool includes_undefined() const;

    Type const& innermost_type() const
    {
        // From step 4 of https://webidl.spec.whatwg.org/#dfn-distinguishable
        // "Consider the two "innermost" types derived by taking each type’s inner type if it is an annotated type, and then taking its inner type inner type if the result is a nullable type."
        // FIXME: Annotated types.
        VERIFY(!is_union());
        return *this;
    }

    // https://webidl.spec.whatwg.org/#idl-any
    bool is_any() const { return is_plain() && m_name == "any"; }

    // https://webidl.spec.whatwg.org/#idl-undefined
    bool is_undefined() const { return is_plain() && m_name == "undefined"; }

    // https://webidl.spec.whatwg.org/#idl-boolean
    bool is_boolean() const { return is_plain() && m_name == "boolean"; }

    // https://webidl.spec.whatwg.org/#idl-bigint
    bool is_bigint() const { return is_plain() && m_name == "bigint"; }

    // https://webidl.spec.whatwg.org/#idl-object
    bool is_object() const { return is_plain() && m_name == "object"; }

    // https://webidl.spec.whatwg.org/#idl-symbol
    bool is_symbol() const { return is_plain() && m_name == "symbol"; }

    bool is_string() const { return is_plain() && m_name.is_one_of("ByteString", "CSSOMString", "DOMString", "USVString"); }

    // https://webidl.spec.whatwg.org/#dfn-integer-type
    bool is_integer() const { return is_plain() && m_name.is_one_of("byte", "octet", "short", "unsigned short", "long", "unsigned long", "long long", "unsigned long long"); }

    // https://webidl.spec.whatwg.org/#dfn-numeric-type
    bool is_numeric() const { return is_plain() && (is_integer() || is_floating_point()); }

    // https://webidl.spec.whatwg.org/#dfn-primitive-type
    bool is_primitive() const { return is_plain() && (is_numeric() || is_boolean() || m_name == "bigint"); }

    // https://webidl.spec.whatwg.org/#idl-sequence
    bool is_sequence() const { return is_parameterized() && m_name == "sequence"; }

    // https://webidl.spec.whatwg.org/#dfn-distinguishable
    bool is_distinguishable_from(Interface const&, Type const& other) const;

    bool is_json(Interface const&) const;

    bool is_restricted_floating_point() const { return m_name.is_one_of("float", "double"); }
    bool is_unrestricted_floating_point() const { return m_name.is_one_of("unrestricted float", "unrestricted double"); }
    bool is_floating_point() const { return is_restricted_floating_point() || is_unrestricted_floating_point(); }

private:
    Kind m_kind;
    ByteString m_name;
    bool m_nullable { false };
};

struct Parameter {
    NonnullRefPtr<Type const> type;
    ByteString name;
    bool optional { false };
    Optional<ByteString> optional_default_value;
    HashMap<ByteString, ByteString> extended_attributes;
    bool variadic { false };
};

struct Function {
    NonnullRefPtr<Type const> return_type;
    ByteString name;
    Vector<Parameter> parameters;
    HashMap<ByteString, ByteString> extended_attributes;
    LineTrackingLexer::Position source_position;
    size_t overload_index { 0 };
    bool is_overloaded { false };

    size_t shortest_length() const { return get_function_shortest_length(*this); }
};

struct Constructor {
    ByteString name;
    Vector<Parameter> parameters;
    HashMap<ByteString, ByteString> extended_attributes;
    size_t overload_index { 0 };
    bool is_overloaded { false };

    size_t shortest_length() const { return get_function_shortest_length(*this); }
};

struct Constant {
    NonnullRefPtr<Type const> type;
    ByteString name;
    ByteString value;
};

struct Attribute {
    bool inherit { false };
    bool readonly { false };
    NonnullRefPtr<Type const> type;
    ByteString name;
    HashMap<ByteString, ByteString> extended_attributes;

    // Added for convenience after parsing
    ByteString getter_callback_name;
    ByteString setter_callback_name;
};

struct DictionaryMember {
    bool required { false };
    NonnullRefPtr<Type const> type;
    ByteString name;
    HashMap<ByteString, ByteString> extended_attributes;
    Optional<ByteString> default_value;
};

struct Dictionary {
    ByteString parent_name;
    Vector<DictionaryMember> members;
};

struct Typedef {
    HashMap<ByteString, ByteString> extended_attributes;
    NonnullRefPtr<Type const> type;
};

struct Enumeration {
    OrderedHashTable<ByteString> values;
    OrderedHashMap<ByteString, ByteString> translated_cpp_names;
    HashMap<ByteString, ByteString> extended_attributes;
    ByteString first_member;
    bool is_original_definition { true };
};

struct CallbackFunction {
    NonnullRefPtr<Type const> return_type;
    Vector<Parameter> parameters;
    bool is_legacy_treat_non_object_as_null { false };
};

class ParameterizedType : public Type {
public:
    ParameterizedType(ByteString name, bool nullable, Vector<NonnullRefPtr<Type const>> parameters)
        : Type(Kind::Parameterized, move(name), nullable)
        , m_parameters(move(parameters))
    {
    }

    virtual ~ParameterizedType() override = default;

    void generate_sequence_from_iterable(SourceGenerator& generator, ByteString const& cpp_name, ByteString const& iterable_cpp_name, ByteString const& iterator_method_cpp_name, IDL::Interface const&, size_t recursion_depth) const;

    Vector<NonnullRefPtr<Type const>> const& parameters() const { return m_parameters; }
    Vector<NonnullRefPtr<Type const>>& parameters() { return m_parameters; }

private:
    Vector<NonnullRefPtr<Type const>> m_parameters;
};

static inline size_t get_shortest_function_length(Vector<Function&> const& overload_set)
{
    size_t shortest_length = SIZE_MAX;
    for (auto const& function : overload_set)
        shortest_length = min(function.shortest_length(), shortest_length);
    return shortest_length;
}

class Interface {
    AK_MAKE_NONCOPYABLE(Interface);
    AK_MAKE_NONMOVABLE(Interface);

public:
    explicit Interface() = default;

    ByteString name;
    ByteString parent_name;
    ByteString namespaced_name;
    ByteString implemented_name;

    bool is_namespace { false };
    bool is_mixin { false };

    HashMap<ByteString, ByteString> extended_attributes;

    Vector<Attribute> attributes;
    Vector<Attribute> static_attributes;
    Vector<Constant> constants;
    Vector<Constructor> constructors;
    Vector<Function> functions;
    Vector<Function> static_functions;
    bool has_stringifier { false };
    Optional<Attribute> stringifier_attribute;
    bool has_unscopable_member { false };

    Optional<NonnullRefPtr<Type const>> value_iterator_type;
    Optional<Tuple<NonnullRefPtr<Type const>, NonnullRefPtr<Type const>>> pair_iterator_types;
    Optional<NonnullRefPtr<Type const>> set_entry_type;
    bool is_set_readonly { false };

    Optional<Function> named_property_getter;
    Optional<Function> named_property_setter;

    Optional<Function> indexed_property_getter;
    Optional<Function> indexed_property_setter;

    Optional<Function> named_property_deleter;

    HashMap<ByteString, Dictionary> dictionaries;
    HashMap<ByteString, Vector<Dictionary>> partial_dictionaries;
    HashMap<ByteString, Enumeration> enumerations;
    HashMap<ByteString, Typedef> typedefs;
    HashMap<ByteString, Interface*> mixins;
    HashMap<ByteString, CallbackFunction> callback_functions;

    // Added for convenience after parsing
    ByteString fully_qualified_name;
    ByteString constructor_class;
    ByteString prototype_class;
    ByteString prototype_base_class;
    ByteString namespace_class;
    ByteString global_mixin_class;
    HashMap<ByteString, HashTable<ByteString>> included_mixins;

    ByteString module_own_path;
    Vector<Interface&> imported_modules;

    HashMap<ByteString, Vector<Function&>> overload_sets;
    HashMap<ByteString, Vector<Function&>> static_overload_sets;
    HashMap<ByteString, Vector<Constructor&>> constructor_overload_sets;

    // https://webidl.spec.whatwg.org/#dfn-support-indexed-properties
    bool supports_indexed_properties() const { return indexed_property_getter.has_value(); }

    // https://webidl.spec.whatwg.org/#dfn-support-named-properties
    bool supports_named_properties() const { return named_property_getter.has_value(); }

    // https://webidl.spec.whatwg.org/#dfn-legacy-platform-object
    bool is_legacy_platform_object() const { return !extended_attributes.contains("Global") && (supports_indexed_properties() || supports_named_properties()); }

    bool will_generate_code() const
    {
        return !name.is_empty() || any_of(enumerations, [](auto& entry) { return entry.value.is_original_definition; });
    }
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
    Vector<NonnullRefPtr<Type const>>& member_types() { return m_member_types; }

    // https://webidl.spec.whatwg.org/#dfn-flattened-union-member-types
    Vector<NonnullRefPtr<Type const>> flattened_member_types() const
    {
        // 1. Let T be the union type.

        // 2. Initialize S to ∅.
        Vector<NonnullRefPtr<Type const>> types;

        // 3. For each member type U of T:
        for (auto& type : m_member_types) {
            // FIXME: 1. If U is an annotated type, then set U to be the inner type of U.

            // 2. If U is a nullable type, then set U to be the inner type of U. (NOTE: Not necessary as nullable is stored with Type and not as a separate struct)

            // 3. If U is a union type, then add to S the flattened member types of U.
            if (type->is_union()) {
                auto& union_member_type = type->as_union();
                types.extend(union_member_type.flattened_member_types());
            } else {
                // 4. Otherwise, U is not a union type. Add U to S.
                types.append(type);
            }
        }

        // 4. Return S.
        return types;
    }

    // https://webidl.spec.whatwg.org/#dfn-number-of-nullable-member-types
    size_t number_of_nullable_member_types() const
    {
        // 1. Let T be the union type.

        // 2. Initialize n to 0.
        size_t num_nullable_member_types = 0;

        // 3. For each member type U of T:
        for (auto& type : m_member_types) {
            // 1. If U is a nullable type, then:
            if (type->is_nullable()) {
                // 1. Set n to n + 1.
                ++num_nullable_member_types;

                // 2. Set U to be the inner type of U. (NOTE: Not necessary as nullable is stored with Type and not as a separate struct)
            }

            // 2. If U is a union type, then:
            if (type->is_union()) {
                auto& union_member_type = type->as_union();

                // 1. Let m be the number of nullable member types of U.
                // 2. Set n to n + m.
                num_nullable_member_types += union_member_type.number_of_nullable_member_types();
            }
        }

        // 4. Return n.
        return num_nullable_member_types;
    }

private:
    Vector<NonnullRefPtr<Type const>> m_member_types;
};

// https://webidl.spec.whatwg.org/#dfn-optionality-value
enum class Optionality {
    Required,
    Optional,
    Variadic,
};

// https://webidl.spec.whatwg.org/#dfn-effective-overload-set
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
    // FIXME: This should be an "ordered set".
    Vector<Item> m_items;
    size_t m_distinguishing_argument_index { 0 };

    Optional<size_t> m_last_matching_item_index;
};

}
