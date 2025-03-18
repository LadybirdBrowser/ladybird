/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/OverloadResolution.h>

namespace Web::WebIDL {

// https://webidl.spec.whatwg.org/#dfn-convert-ecmascript-to-idl-value
static JS::Value convert_ecmascript_type_to_idl_value(JS::Value value, IDL::Type const&)
{
    // FIXME: We have this code already in the code generator, in `generate_to_cpp()`, but how do we use it here?
    return value;
}

template<typename Match>
static bool has_overload_with_argument_type_or_subtype_matching(IDL::EffectiveOverloadSet& overloads, size_t argument_index, Match match)
{
    // NOTE: This is to save some repetition.
    //       Almost every sub-step of step 12 of the overload resolution algorithm matches overloads with an argument that is:
    //       - One of several specific types.
    //       - "an annotated type whose inner type is one of the above types"
    //       - "a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types"
    //       So, this function lets you pass in the first check, and handles the others automatically.

    return overloads.has_overload_with_matching_argument_at_index(argument_index,
        [match](IDL::Type const& type, auto) {
            if (match(type))
                return true;

            // FIXME: - an annotated type whose inner type is one of the above types

            if (type.is_union()) {
                auto flattened_members = type.as_union().flattened_member_types();
                for (auto const& member : flattened_members) {
                    if (match(member))
                        return true;

                    // FIXME: - an annotated type whose inner type is one of the above types
                }
                return false;
            }

            return false;
        });
}

// https://webidl.spec.whatwg.org/#es-overloads
JS::ThrowCompletionOr<ResolvedOverload> resolve_overload(JS::VM& vm, IDL::EffectiveOverloadSet& overloads, ReadonlySpan<StringView> dictionary_types)
{
    auto is_dictionary = [&dictionary_types](IDL::Type const& type) {
        return dictionary_types.contains_slow(type.name());
    };

    // 1. Let maxarg be the length of the longest type list of the entries in S.
    // 2. Let n be the size of args.
    // 3. Initialize argcount to be min(maxarg, n).
    // 4. Remove from S all entries whose type list is not of length argcount.
    // NOTE: The IDL-generated callers already only provide an overload set containing overloads with the correct number
    //       of arguments. Therefore, we do not need to remove any entry from that set here. However, we do need to handle
    //       when the number of user-provided arguments exceeds the overload set's argument count.
    int argument_count = min(vm.argument_count(), overloads.is_empty() ? 0 : overloads.items()[0].types.size());

    // 5. If S is empty, then throw a TypeError.
    if (overloads.is_empty())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::OverloadResolutionFailed);

    // 6. Initialize d to −1.
    auto distinguishing_argument_index = -1;

    // 7. Initialize method to undefined.
    GC::Ptr<JS::FunctionObject> method;

    // 8. If there is more than one entry in S, then set d to be the distinguishing argument index for the entries of S.
    if (overloads.size() > 1)
        distinguishing_argument_index = overloads.distinguishing_argument_index();

    // 9. Initialize values to be an empty list, where each entry will be either an IDL value or the special value “missing”.
    Vector<ResolvedOverload::Argument> values;

    // 10. Initialize i to 0.
    auto i = 0;

    // 11. While i < d:
    while (i < distinguishing_argument_index) {
        // 1. Let V be args[i].
        auto const& value = vm.argument(i);

        auto const& item = overloads.items().first();

        // 2. Let type be the type at index i in the type list of any entry in S.
        auto const& type = item.types[i];

        // 3. Let optionality be the value at index i in the list of optionality values of any entry in S.
        auto const& optionality = item.optionality_values[i];

        // 4. If optionality is “optional” and V is undefined, then:
        if (optionality == IDL::Optionality::Optional && value.is_undefined()) {
            // FIXME: 1. If the argument at index i is declared with a default value, then append to values that default value.

            // 2. Otherwise, append to values the special value “missing”.
            values.empend(ResolvedOverload::Missing {});
        }

        // 5. Otherwise, append to values the result of converting V to IDL type type.
        values.empend(convert_ecmascript_type_to_idl_value(value, type));

        // 6. Set i to i + 1.
        ++i;
    }

    // 12. If i = d, then:
    if (i == distinguishing_argument_index) {
        // 1. Let V be args[i].
        auto const& value = vm.argument(i);

        // 2. If V is undefined, and there is an entry in S whose list of optionality values has “optional” at index i, then remove from S all other entries.
        if (value.is_undefined()
            && overloads.has_overload_with_matching_argument_at_index(i, [](auto&, IDL::Optionality const& optionality) { return optionality == IDL::Optionality::Optional; })) {
            overloads.remove_all_other_entries();
        }

        // 3. Otherwise: if V is null or undefined, and there is an entry in S that has one of the following types at position i of its type list,
        //    - a nullable type
        //    - a dictionary type
        //    - an annotated type whose inner type is one of the above types
        //    - a union type or annotated union type that includes a nullable type or that has a dictionary type in its flattened members
        //    then remove from S all other entries.
        // NOTE: This is the one case we can't use `has_overload_with_argument_type_or_subtype_matching()` because we also need to look
        //       for dictionary types in the flattened members.
        else if ((value.is_undefined() || value.is_null())
            && overloads.has_overload_with_matching_argument_at_index(i, [&is_dictionary](IDL::Type const& type, auto) {
                   if (type.is_nullable())
                       return true;
                   if (is_dictionary(type))
                       return true;

                   // FIXME: - an annotated type whose inner type is one of the above types
                   if (type.is_union()) {
                       auto flattened_members = type.as_union().flattened_member_types();
                       for (auto const& member : flattened_members) {
                           if (member->is_nullable())
                               return true;
                           if (is_dictionary(type))
                               return true;
                           // FIXME: - an annotated type whose inner type is one of the above types
                       }
                       return false;
                   }
                   return false;
               })) {
            overloads.remove_all_other_entries();
        }

        // 4. Otherwise: if V is a platform object, and there is an entry in S that has one of the following types at position i of its type list,
        //    - an interface type that V implements
        //    - object
        //    - a nullable version of any of the above types
        //    - an annotated type whose inner type is one of the above types
        //    - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //    then remove from S all other entries.
        else if (value.is_object() && is<Bindings::PlatformObject>(value.as_object())
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [value](IDL::Type const& type) {
                   // - an interface type that V implements
                   if (static_cast<Bindings::PlatformObject const&>(value.as_object()).implements_interface(MUST(String::from_byte_string(type.name()))))
                       return true;

                   // - object
                   if (type.is_object())
                       return true;

                   return false;
               })) {
            overloads.remove_all_other_entries();
        }

        // 5. Otherwise: if Type(V) is Object, V has an [[ArrayBufferData]] internal slot, and there is an entry in S that has one of the following types at position i of its type list,
        //    - ArrayBuffer
        //    - object
        //    - a nullable version of either of the above types
        //    - an annotated type whose inner type is one of the above types
        //    - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //    then remove from S all other entries.
        else if (value.is_object() && is<JS::ArrayBuffer>(value.as_object())
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) {
                   if (type.is_plain() && (type.name() == "ArrayBuffer" || type.name() == "BufferSource"))
                       return true;
                   if (type.is_object())
                       return true;
                   return false;
               })) {
            overloads.remove_all_other_entries();
        }

        // 6. Otherwise: if Type(V) is Object, V has a [[DataView]] internal slot, and there is an entry in S that has one of the following types at position i of its type list,
        //    - DataView
        //    - object
        //    - a nullable version of either of the above types
        //    - an annotated type whose inner type is one of the above types
        //    - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //    then remove from S all other entries.
        else if (value.is_object() && is<JS::DataView>(value.as_object())
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) {
                   if (type.is_plain() && (type.name() == "DataView" || type.name() == "BufferSource" || type.name() == "ArrayBufferView"))
                       return true;
                   if (type.is_object())
                       return true;
                   return false;
               })) {
            overloads.remove_all_other_entries();
        }

        // 7. Otherwise: if Type(V) is Object, V has a [[TypedArrayName]] internal slot, and there is an entry in S that has one of the following types at position i of its type list,
        //    - a typed array type whose name is equal to the value of V’s [[TypedArrayName]] internal slot
        //    - object
        //    - a nullable version of either of the above types
        //    - an annotated type whose inner type is one of the above types
        //    - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //    then remove from S all other entries.
        else if (value.is_object() && value.as_object().is_typed_array()
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [&](IDL::Type const& type) {
                   if (type.is_plain() && (type.name() == static_cast<JS::TypedArrayBase const&>(value.as_object()).element_name().bytes_as_string_view() || type.name() == "BufferSource" || type.name() == "ArrayBufferView"))
                       return true;
                   if (type.is_object())
                       return true;
                   return false;
               })) {
            overloads.remove_all_other_entries();
        }

        // 8. Otherwise: if IsCallable(V) is true, and there is an entry in S that has one of the following types at position i of its type list,
        //    - a callback function type
        //    - object
        //    - a nullable version of any of the above types
        //    - an annotated type whose inner type is one of the above types
        //    - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //    then remove from S all other entries.
        else if (value.is_function()
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) {
                   // FIXME: - a callback function type
                   if (type.is_object())
                       return true;
                   return false;
               })) {
            overloads.remove_all_other_entries();
        }

        // 9. Otherwise: if Type(V) is Object and there is an entry in S that has one of the following types at position i of its type list,
        //    - a sequence type
        //    - a frozen array type
        //    - a nullable version of any of the above types
        //    - an annotated type whose inner type is one of the above types
        //    - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //    and after performing the following steps,
        //    {
        //        1. Let method be ? GetMethod(V, @@iterator).
        //    }
        //    method is not undefined, then remove from S all other entries.
        else if (value.is_object()
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [&vm, &method, &value](IDL::Type const& type) {
                   // - a sequence type
                   // FIXME: - a frozen array type
                   // - a nullable version of any of the above types
                   if (type.name() != "sequence")
                       return false;

                   // and after performing the following steps,
                   // {
                   //     1. Let method be ? GetMethod(V, @@iterator).
                   // }
                   // method is not undefined, then remove from S all other entries.
                   auto maybe_method = value.get_method(vm, vm.well_known_symbol_iterator());
                   if (maybe_method.is_error() || maybe_method.value() == nullptr)
                       return false;

                   method = *maybe_method.release_value();
                   return true;
               })) {
            overloads.remove_all_other_entries();
        }
        // 10. Otherwise: if Type(V) is Object and there is an entry in S that has one of the following types at position i of its type list,
        //     - a callback interface type
        //     - a dictionary type
        //     - a record type
        //     - object
        //     - a nullable version of any of the above types
        //     - an annotated type whose inner type is one of the above types
        //     - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //     then remove from S all other entries.
        else if (value.is_object()
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [&is_dictionary](IDL::Type const& type) {
                   if (is_dictionary(type))
                       return true;
                   // FIXME: a callback interface type
                   // FIXME: a record type
                   return type.is_object();
               })) {
            overloads.remove_all_other_entries();
        }

        // 11. Otherwise: if Type(V) is Boolean and there is an entry in S that has one of the following types at position i of its type list,
        //     - boolean
        //     - a nullable boolean
        //     - an annotated type whose inner type is one of the above types
        //     - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //     then remove from S all other entries.
        else if (value.is_boolean()
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) { return type.is_boolean(); })) {
            overloads.remove_all_other_entries();
        }

        // 12. Otherwise: if Type(V) is Number and there is an entry in S that has one of the following types at position i of its type list,
        //     - a numeric type
        //     - a nullable numeric type
        //     - an annotated type whose inner type is one of the above types
        //     - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //     then remove from S all other entries.
        else if (value.is_number()
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) { return type.is_numeric(); })) {
            overloads.remove_all_other_entries();
        }

        // 13. Otherwise: if Type(V) is BigInt and there is an entry in S that has one of the following types at position i of its type list,
        //     - bigint
        //     - a nullable bigint
        //     - an annotated type whose inner type is one of the above types
        //     - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //     then remove from S all other entries.
        else if (value.is_bigint()
            && has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) { return type.is_bigint(); })) {
            overloads.remove_all_other_entries();
        }

        // 14. Otherwise: if there is an entry in S that has one of the following types at position i of its type list,
        //     - a string type
        //     - a nullable version of any of the above types
        //     - an annotated type whose inner type is one of the above types
        //     - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //     then remove from S all other entries.
        else if (has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) { return type.is_string(); })) {
            overloads.remove_all_other_entries();
        }

        // 15. Otherwise: if there is an entry in S that has one of the following types at position i of its type list,
        //     - a numeric type
        //     - a nullable numeric type
        //     - an annotated type whose inner type is one of the above types
        //     - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //     then remove from S all other entries.
        else if (has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) { return type.is_numeric(); })) {
            overloads.remove_all_other_entries();
        }

        // 16. Otherwise: if there is an entry in S that has one of the following types at position i of its type list,
        //     - boolean
        //     - a nullable boolean
        //     - an annotated type whose inner type is one of the above types
        //     - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //     then remove from S all other entries.
        else if (has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) { return type.is_boolean(); })) {
            overloads.remove_all_other_entries();
        }

        // 17. Otherwise: if there is an entry in S that has one of the following types at position i of its type list,
        //     - bigint
        //     - a nullable bigint
        //     - an annotated type whose inner type is one of the above types
        //     - a union type, nullable union type, or annotated union type that has one of the above types in its flattened member types
        //     then remove from S all other entries.
        else if (has_overload_with_argument_type_or_subtype_matching(overloads, i, [](IDL::Type const& type) { return type.is_bigint(); })) {
            overloads.remove_all_other_entries();
        }

        // 18. Otherwise: if there is an entry in S that has any at position i of its type list, then remove from S all other entries.
        else if (overloads.has_overload_with_matching_argument_at_index(i, [](auto const& type, auto) { return type->is_any(); })) {
            overloads.remove_all_other_entries();
        }

        // 19. Otherwise: throw a TypeError.
        else {
            // FIXME: Remove this message once all the above sub-steps are implemented.
            dbgln("Failed to determine IDL overload. (Probably because of unimplemented steps.)");
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::OverloadResolutionFailed);
        }
    }

    // 13. Let callable be the operation or extended attribute of the single entry in S.
    auto const& callable = overloads.only_item();

    // 14. If i = d and method is not undefined, then
    if (i == distinguishing_argument_index && method) {
        // 1. Let V be args[i].
        auto const& value = vm.argument(i);

        // 2. Let T be the type at index i in the type list of the remaining entry in S.
        auto const& type = overloads.only_item().types[i];

        (void)value;
        (void)type;
        // FIXME: 3. If T is a sequence type, then append to values the result of creating a sequence of type T from V and method.

        // FIXME: 4. Otherwise, T is a frozen array type. Append to values the result of creating a frozen array of type T from V and method.

        // 5. Set i to i + 1.
        ++i;
    }

    // 15. While i < argcount:
    while (i < argument_count) {
        // 1. Let V be args[i].
        auto const& value = vm.argument(i);

        // 2. Let type be the type at index i in the type list of the remaining entry in S.
        auto const& entry = overloads.only_item();
        auto const& type = entry.types[i];

        // 3. Let optionality be the value at index i in the list of optionality values of the remaining entry in S.
        auto const& optionality = entry.optionality_values[i];

        // 4. If optionality is “optional” and V is undefined, then:
        if (optionality == IDL::Optionality::Optional && value.is_undefined()) {
            // FIXME: 1. If the argument at index i is declared with a default value, then append to values that default value.

            // 2. Otherwise, append to values the special value “missing”.
            values.empend(ResolvedOverload::Missing {});
        }

        // 5. Otherwise, append to values the result of converting V to IDL type type.
        else {
            values.append(convert_ecmascript_type_to_idl_value(value, type));
        }

        // 6. Set i to i + 1.
        ++i;
    }

    // 16. While i is less than the number of arguments callable is declared to take:
    while (i < static_cast<int>(callable.types.size())) {
        // FIXME: 1. If callable’s argument at index i is declared with a default value, then append to values that default value.
        if (false) {
        }

        // 2. Otherwise, if callable’s argument at index i is not variadic, then append to values the special value “missing”.
        else if (callable.optionality_values[i] != IDL::Optionality::Variadic) {
            values.empend(ResolvedOverload::Missing {});
        }

        // 3. Set i to i + 1.
        ++i;
    }

    // 17. Return the pair <callable, values>.
    return ResolvedOverload { callable.callable_id, move(values) };
}

}
