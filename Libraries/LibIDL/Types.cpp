/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibIDL/Types.h>

namespace IDL {

ParameterizedType const& Type::as_parameterized() const
{
    return verify_cast<ParameterizedType const>(*this);
}

ParameterizedType& Type::as_parameterized()
{
    return verify_cast<ParameterizedType>(*this);
}

UnionType const& Type::as_union() const
{
    return verify_cast<UnionType const>(*this);
}

UnionType& Type::as_union()
{
    return verify_cast<UnionType>(*this);
}

// https://webidl.spec.whatwg.org/#dfn-includes-a-nullable-type
bool Type::includes_nullable_type() const
{
    // A type includes a nullable type if:
    // - the type is a nullable type, or
    if (is_nullable())
        return true;

    // FIXME: - the type is an annotated type and its inner type is a nullable type, or

    // - the type is a union type and its number of nullable member types is 1.
    if (is_union() && as_union().number_of_nullable_member_types() == 1)
        return true;

    return false;
}

// https://webidl.spec.whatwg.org/#dfn-includes-undefined
bool Type::includes_undefined() const
{
    // A type includes undefined if:
    // - the type is undefined, or
    if (is_undefined())
        return true;

    // - the type is a nullable type and its inner type includes undefined, or
    //   NOTE: We don't treat nullable as its own type, so this is handled by the other cases.

    // FIXME: - the type is an annotated type and its inner type includes undefined, or

    // - the type is a union type and one of its member types includes undefined.
    if (is_union()) {
        for (auto& type : as_union().member_types()) {
            if (type->includes_undefined())
                return true;
        }
    }

    return false;
}

// https://webidl.spec.whatwg.org/#dfn-distinguishable
bool Type::is_distinguishable_from(IDL::Interface const& interface, IDL::Type const& other) const
{
    // 1. If one type includes a nullable type and the other type either includes a nullable type,
    //    is a union type with flattened member types including a dictionary type, or is a dictionary type,
    //    return false.
    if (includes_nullable_type() && (other.includes_nullable_type() || (other.is_union() && any_of(other.as_union().flattened_member_types(), [&interface](auto const& type) { return interface.dictionaries.contains(type->name()); })) || interface.dictionaries.contains(other.name())))
        return false;

    // 2. If both types are either a union type or nullable union type, return true if each member type
    //    of the one is distinguishable with each member type of the other, or false otherwise.
    if (is_union() && other.is_union()) {
        auto const& this_union = as_union();
        auto const& other_union = other.as_union();

        for (auto& this_member_type : this_union.member_types()) {
            for (auto& other_member_type : other_union.member_types()) {
                if (!this_member_type->is_distinguishable_from(interface, other_member_type))
                    return false;
            }
        }
        return true;
    }

    // 3. If one type is a union type or nullable union type, return true if each member type of the union
    //    type is distinguishable with the non-union type, or false otherwise.
    if (is_union() || other.is_union()) {
        auto const& the_union = is_union() ? as_union() : other.as_union();
        auto const& non_union = is_union() ? other : *this;

        for (auto& member_type : the_union.member_types()) {
            if (!non_union.is_distinguishable_from(interface, member_type))
                return false;
        }
        return true;
    }

    // 4. Consider the two "innermost" types derived by taking each type’s inner type if it is an annotated type,
    //    and then taking its inner type inner type if the result is a nullable type. If these two innermost types
    //    appear or are in categories appearing in the following table and there is a “●” mark in the corresponding
    //    entry or there is a letter in the corresponding entry and the designated additional requirement below the
    //    table is satisfied, then return true. Otherwise return false.
    auto const& this_innermost_type = innermost_type();
    auto const& other_innermost_type = other.innermost_type();

    enum class DistinguishabilityCategory {
        Undefined,
        Boolean,
        Numeric,
        BigInt,
        String,
        Object,
        Symbol,
        InterfaceLike,
        CallbackFunction,
        DictionaryLike,
        SequenceLike,
        __Count
    };

    // See https://webidl.spec.whatwg.org/#distinguishable-table
    // clang-format off
    static constexpr bool table[to_underlying(DistinguishabilityCategory::__Count)][to_underlying(DistinguishabilityCategory::__Count)] {
        {false,  true,  true,  true,  true,  true,  true,  true,  true, false,  true},
        { true, false,  true,  true,  true,  true,  true,  true,  true,  true,  true},
        { true,  true, false,  true,  true,  true,  true,  true,  true,  true,  true},
        { true,  true,  true, false,  true,  true,  true,  true,  true,  true,  true},
        { true,  true,  true,  true, false,  true,  true,  true,  true,  true,  true},
        { true,  true,  true,  true,  true, false,  true, false, false, false, false},
        { true,  true,  true,  true,  true,  true, false,  true,  true,  true,  true},
        { true,  true,  true,  true,  true, false,  true, false,  true,  true,  true},
        { true,  true,  true,  true,  true, false,  true,  true, false, false,  true},
        {false,  true,  true,  true,  true, false,  true,  true, false, false,  true},
        { true,  true,  true,  true,  true, false,  true,  true,  true,  true, false},
    };
    // clang-format on

    auto determine_category = [&interface](Type const& type) -> DistinguishabilityCategory {
        if (type.is_undefined())
            return DistinguishabilityCategory::Undefined;
        if (type.is_boolean())
            return DistinguishabilityCategory::Boolean;
        if (type.is_numeric())
            return DistinguishabilityCategory::Numeric;
        if (type.is_bigint())
            return DistinguishabilityCategory::BigInt;
        if (type.is_string())
            return DistinguishabilityCategory::String;
        if (type.is_object())
            return DistinguishabilityCategory::Object;
        if (type.is_symbol())
            return DistinguishabilityCategory::Symbol;
        // FIXME: InterfaceLike - see below
        // FIXME: CallbackFunction
        // DictionaryLike
        // * Dictionary Types
        // * Record Types
        // FIXME: * Callback Interface Types
        if (interface.dictionaries.contains(type.name()) || (type.is_parameterized() && type.name() == "record"sv))
            return DistinguishabilityCategory::DictionaryLike;
        // FIXME: Frozen array types are included in "sequence-like"
        if (type.is_sequence())
            return DistinguishabilityCategory::SequenceLike;

        // FIXME: For lack of a better way of determining if something is an interface type, this just assumes anything we don't recognise is one.
        dbgln_if(IDL_DEBUG, "Unable to determine category for type named '{}', assuming it's an interface type.", type.name());
        return DistinguishabilityCategory::InterfaceLike;
    };

    auto this_distinguishability = determine_category(this_innermost_type);
    auto other_distinguishability = determine_category(other_innermost_type);

    if (this_distinguishability == DistinguishabilityCategory::InterfaceLike && other_distinguishability == DistinguishabilityCategory::InterfaceLike) {
        // The two identified interface-like types are not the same, and
        // FIXME: no single platform object implements both interface-like types.
        return this_innermost_type.name() != other_innermost_type.name();
    }

    return table[to_underlying(this_distinguishability)][to_underlying(other_distinguishability)];
}

// https://webidl.spec.whatwg.org/#dfn-json-types
bool Type::is_json(Interface const& interface) const
{
    // The JSON types are:
    // - numeric types,
    if (is_numeric())
        return true;

    // - boolean,
    if (is_boolean())
        return true;

    // - string types,
    if (is_string() || interface.enumerations.find(m_name) != interface.enumerations.end())
        return true;

    // - object,
    if (is_object())
        return true;

    // - nullable types whose inner type is a JSON type,
    // - annotated types whose inner type is a JSON type,
    // NOTE: We don't separate nullable and annotated into separate types.

    // - union types whose member types are JSON types,
    if (is_union()) {
        auto const& union_type = as_union();

        for (auto const& type : union_type.member_types()) {
            if (!type->is_json(interface))
                return false;
        }

        return true;
    }

    // - typedefs whose type being given a new name is a JSON type,
    auto typedef_iterator = interface.typedefs.find(m_name);
    if (typedef_iterator != interface.typedefs.end())
        return typedef_iterator->value.type->is_json(interface);

    // - sequence types whose parameterized type is a JSON type,
    // - frozen array types whose parameterized type is a JSON type,
    // - records where all of their values are JSON types,
    if (is_parameterized() && m_name.is_one_of("sequence", "FrozenArray", "record")) {
        auto const& parameterized_type = as_parameterized();

        for (auto const& parameter : parameterized_type.parameters()) {
            if (!parameter->is_json(interface))
                return false;
        }

        return true;
    }

    // - dictionary types where the types of all members declared on the dictionary and all its inherited dictionaries are JSON types,
    auto dictionary_iterator = interface.dictionaries.find(m_name);
    if (dictionary_iterator != interface.dictionaries.end()) {
        auto const& dictionary = dictionary_iterator->value;
        for (auto const& member : dictionary.members) {
            if (!member.type->is_json(interface))
                return false;
        }

        return true;
    }

    // - interface types that have a toJSON operation declared on themselves or one of their inherited interfaces.
    Optional<Interface const&> current_interface_for_to_json;
    if (m_name == interface.name) {
        current_interface_for_to_json = interface;
    } else {
        // NOTE: Interface types must have the IDL file of their interface imported.
        //       Though the type name may not refer to an interface, so we don't assert this here.
        auto imported_interface_iterator = interface.imported_modules.find_if([this](IDL::Interface const& imported_interface) {
            return imported_interface.name == m_name;
        });

        if (imported_interface_iterator != interface.imported_modules.end())
            current_interface_for_to_json = *imported_interface_iterator;
    }

    while (current_interface_for_to_json.has_value()) {
        auto to_json_iterator = current_interface_for_to_json->functions.find_if([](IDL::Function const& function) {
            return function.name == "toJSON"sv;
        });

        if (to_json_iterator != current_interface_for_to_json->functions.end())
            return true;

        if (current_interface_for_to_json->parent_name.is_empty())
            break;

        auto imported_interface_iterator = current_interface_for_to_json->imported_modules.find_if([&current_interface_for_to_json](IDL::Interface const& imported_interface) {
            return imported_interface.name == current_interface_for_to_json->parent_name;
        });

        // Inherited interfaces must have their IDL files imported.
        VERIFY(imported_interface_iterator != interface.imported_modules.end());

        current_interface_for_to_json = *imported_interface_iterator;
    }

    return false;
}

void EffectiveOverloadSet::remove_all_other_entries()
{
    Vector<Item> new_items;
    new_items.append(m_items[*m_last_matching_item_index]);
    m_items = move(new_items);
}

}
