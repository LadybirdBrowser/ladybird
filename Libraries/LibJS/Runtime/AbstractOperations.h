/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Forward.h>
#include <LibCrypto/Forward.h>
#include <LibGC/RootVector.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/CanonicalIndex.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/KeyedCollections.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

GC::Ref<DeclarativeEnvironment> new_declarative_environment(Environment&);
JS_API GC::Ref<ObjectEnvironment> new_object_environment(Object&, bool is_with_environment, Environment*);
GC::Ref<FunctionEnvironment> new_function_environment(ECMAScriptFunctionObject&, Object* new_target);
GC::Ref<PrivateEnvironment> new_private_environment(VM& vm, PrivateEnvironment* outer);
GC::Ref<Environment> get_this_environment(VM&);
JS_API bool can_be_held_weakly(Value);
Object* get_super_constructor(VM&);
ThrowCompletionOr<Value> require_object_coercible(VM&, Value);
JS_API ThrowCompletionOr<Value> call_impl(VM&, Value function, Value this_value, ReadonlySpan<Value> arguments = {});
JS_API ThrowCompletionOr<Value> call_impl(VM&, FunctionObject& function, Value this_value, ReadonlySpan<Value> arguments = {});
JS_API ThrowCompletionOr<GC::Ref<Object>> construct_impl(VM&, FunctionObject&, ReadonlySpan<Value> arguments = {}, FunctionObject* new_target = nullptr);
JS_API ThrowCompletionOr<size_t> length_of_array_like(VM&, Object const&);
ThrowCompletionOr<GC::RootVector<Value>> create_list_from_array_like(VM&, Value, Function<ThrowCompletionOr<void>(Value)> = {});
ThrowCompletionOr<FunctionObject*> species_constructor(VM&, Object const&, FunctionObject& default_constructor);
JS_API ThrowCompletionOr<Realm*> get_function_realm(VM&, FunctionObject const&);
bool is_compatible_property_descriptor(bool extensible, PropertyDescriptor&, Optional<PropertyDescriptor> const& current);
bool validate_and_apply_property_descriptor(Object*, PropertyKey const&, bool extensible, PropertyDescriptor&, Optional<PropertyDescriptor> const& current);
JS_API ThrowCompletionOr<Object*> get_prototype_from_constructor(VM&, FunctionObject const& constructor, GC::Ref<Object> (Intrinsics::*intrinsic_default_prototype)());
Object* create_unmapped_arguments_object(VM&, ReadonlySpan<Value> arguments);
Object* create_mapped_arguments_object(VM&, FunctionObject&, NonnullRefPtr<FunctionParameters const> const&, ReadonlySpan<Value> arguments, Environment&);

// 2.1.1 DisposeCapability Records, https://tc39.es/proposal-explicit-resource-management/#sec-disposecapability-records
struct JS_API DisposeCapability {
    void visit_edges(GC::Cell::Visitor&) const;

    Vector<DisposableResource> disposable_resource_stack; // [[DisposableResourceStack]]
};

// 2.1.2 DisposableResource Records, https://tc39.es/proposal-explicit-resource-management/#sec-disposableresource-records
struct DisposableResource {
    void visit_edges(GC::Cell::Visitor&) const;

    GC::Ptr<Object> resource_value;          // [[ResourceValue]]
    Environment::InitializeBindingHint hint; // [[Hint]]
    GC::Ptr<FunctionObject> dispose_method;  // [[DisposeMethod]]
};

JS_API DisposeCapability new_dispose_capability();
JS_API ThrowCompletionOr<void> add_disposable_resource(VM&, DisposeCapability&, Value, Environment::InitializeBindingHint, GC::Ptr<FunctionObject> = {});
ThrowCompletionOr<DisposableResource> create_disposable_resource(VM&, Value, Environment::InitializeBindingHint, GC::Ptr<FunctionObject> = {});
ThrowCompletionOr<GC::Ptr<FunctionObject>> get_dispose_method(VM&, Value, Environment::InitializeBindingHint);
Completion dispose(VM&, Value, Environment::InitializeBindingHint, GC::Ptr<FunctionObject> method);
Completion dispose_resources(VM&, DisposeCapability&, Completion);

bool all_import_attributes_supported(VM& vm, Vector<ImportAttribute> const& attributes);

ThrowCompletionOr<Value> perform_import_call(VM&, Value specifier, Value options_value);

enum class CanonicalIndexMode {
    DetectNumericRoundtrip,
    IgnoreNumericRoundtrip,
};
[[nodiscard]] CanonicalIndex canonical_numeric_index_string(PropertyKey const&, CanonicalIndexMode needs_numeric);
ThrowCompletionOr<String> get_substitution(VM&, Utf16View const& matched, Utf16View const& str, size_t position, Span<Value> captures, Value named_captures, Value replacement);

enum class CallerMode {
    Strict,
    NonStrict
};

ThrowCompletionOr<Value> perform_eval(VM&, Value, CallerMode, EvalMode);

ThrowCompletionOr<void> eval_declaration_instantiation(VM& vm, Program const& program, Environment* variable_environment, Environment* lexical_environment, PrivateEnvironment* private_environment, bool strict);

// 7.3.14 Call ( F, V [ , argumentsList ] ), https://tc39.es/ecma262/#sec-call
ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, Value function, Value this_value, ReadonlySpan<Value> arguments_list)
{
    return call_impl(vm, function, this_value, arguments_list);
}

ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, Value function, Value this_value, Span<Value> arguments_list)
{
    return call_impl(vm, function, this_value, static_cast<ReadonlySpan<Value>>(arguments_list));
}

template<typename... Args>
ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, Value function, Value this_value, Args&&... args)
{
    constexpr auto argument_count = sizeof...(Args);
    if constexpr (argument_count > 0) {
        AK::Array<Value, argument_count> arguments { forward<Args>(args)... };
        return call_impl(vm, function, this_value, static_cast<ReadonlySpan<Value>>(arguments.span()));
    }

    return call_impl(vm, function, this_value);
}

ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, FunctionObject& function, Value this_value, ReadonlySpan<Value> arguments_list)
{
    return call_impl(vm, function, this_value, arguments_list);
}

ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, FunctionObject& function, Value this_value, Span<Value> arguments_list)
{
    return call_impl(vm, function, this_value, static_cast<ReadonlySpan<Value>>(arguments_list));
}

template<typename... Args>
ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, FunctionObject& function, Value this_value, Args&&... args)
{
    constexpr auto argument_count = sizeof...(Args);
    if constexpr (argument_count > 0) {
        AK::Array<Value, argument_count> arguments { forward<Args>(args)... };
        return call_impl(vm, function, this_value, static_cast<ReadonlySpan<Value>>(arguments.span()));
    }

    return call_impl(vm, function, this_value);
}

// 7.3.15 Construct ( F [ , argumentsList [ , newTarget ] ] ), https://tc39.es/ecma262/#sec-construct
template<typename... Args>
ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> construct(VM& vm, FunctionObject& function, Args&&... args)
{
    constexpr auto argument_count = sizeof...(Args);
    if constexpr (argument_count > 0) {
        AK::Array<Value, argument_count> arguments { forward<Args>(args)... };
        return construct_impl(vm, function, static_cast<ReadonlySpan<Value>>(arguments.span()));
    }

    return construct_impl(vm, function);
}

ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> construct(VM& vm, FunctionObject& function, ReadonlySpan<Value> arguments_list, FunctionObject* new_target = nullptr)
{
    return construct_impl(vm, function, arguments_list, new_target);
}

ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> construct(VM& vm, FunctionObject& function, Span<Value> arguments_list, FunctionObject* new_target = nullptr)
{
    return construct_impl(vm, function, static_cast<ReadonlySpan<Value>>(arguments_list), new_target);
}

// 10.1.13 OrdinaryCreateFromConstructor ( constructor, intrinsicDefaultProto [ , internalSlotsList ] ), https://tc39.es/ecma262/#sec-ordinarycreatefromconstructor
template<typename T, typename... Args>
ALWAYS_INLINE ThrowCompletionOr<GC::Ref<T>> ordinary_create_from_constructor(VM& vm, Realm& realm, FunctionObject const& constructor, GC::Ref<Object> (Intrinsics::*intrinsic_default_prototype)(), Args&&... args)
{
    auto* prototype = TRY(get_prototype_from_constructor(vm, constructor, intrinsic_default_prototype));
    return realm.create<T>(forward<Args>(args)..., *prototype);
}

// 10.1.13 OrdinaryCreateFromConstructor ( constructor, intrinsicDefaultProto [ , internalSlotsList ] ), https://tc39.es/ecma262/#sec-ordinarycreatefromconstructor
template<typename T, typename... Args>
ALWAYS_INLINE ThrowCompletionOr<GC::Ref<T>> ordinary_create_from_constructor(VM& vm, FunctionObject const& constructor, GC::Ref<Object> (Intrinsics::*intrinsic_default_prototype)(), Args&&... args)
{
    return ordinary_create_from_constructor<T>(vm, *vm.current_realm(), constructor, intrinsic_default_prototype, forward<Args>(args)...);
}

// 7.3.35 AddValueToKeyedGroup ( groups, key, value ), https://tc39.es/ecma262/#sec-add-value-to-keyed-group
template<typename GroupsType, typename KeyType>
void add_value_to_keyed_group(VM& vm, GroupsType& groups, KeyType key, Value value)
{
    // 1. For each Record { [[Key]], [[Elements]] } g of groups, do
    //      a. If SameValue(g.[[Key]], key) is true, then
    //      NOTE: This is performed in KeyedGroupTraits::equals for groupToMap and Traits<JS::PropertyKey>::equals for group.
    auto existing_elements_iterator = groups.find(key);
    if (existing_elements_iterator != groups.end()) {
        // i. Assert: exactly one element of groups meets this criteria.
        // NOTE: This is done on insertion into the hash map, as only `set` tells us if we overrode an entry.

        // ii. Append value as the last element of g.[[Elements]].
        existing_elements_iterator->value.append(value);

        // iii. Return unused.
        return;
    }

    // 2. Let group be the Record { [[Key]]: key, [[Elements]]: « value » }.
    GC::RootVector<Value> new_elements { vm.heap() };
    new_elements.append(value);

    // 3. Append group as the last element of groups.
    auto result = groups.set(key, move(new_elements));
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);

    // 4. Return unused.
}

// 7.3.36 GroupBy ( items, callbackfn, keyCoercion ), https://tc39.es/ecma262/#sec-groupby
template<typename GroupsType, typename KeyType>
ThrowCompletionOr<GroupsType> group_by(VM& vm, Value items, Value callback_function)
{
    // 1. Perform ? RequireObjectCoercible(items).
    TRY(require_object_coercible(vm, items));

    // 2. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callback_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback_function.to_string_without_side_effects());

    // 3. Let groups be a new empty List.
    GroupsType groups;

    // 4. Let iteratorRecord be ? GetIterator(items, sync).
    auto iterator_record = TRY(get_iterator(vm, items, IteratorHint::Sync));

    // 5. Let k be 0.
    u64 k = 0;

    // 6. Repeat,
    while (true) {
        // a. If k ≥ 2^53 - 1, then
        if (k >= MAX_ARRAY_LIKE_INDEX) {
            // i. Let error be ThrowCompletion(a newly created TypeError object).
            auto error = vm.throw_completion<TypeError>(ErrorType::ArrayMaxSize);

            // ii. Return ? IteratorClose(iteratorRecord, error).
            return iterator_close(vm, iterator_record, move(error));
        }

        // b. Let next be ? IteratorStepValue(iteratorRecord).
        auto next = TRY(iterator_step_value(vm, iterator_record));

        // c. If next is DONE, then
        if (!next.has_value()) {
            // i. Return groups.
            return ThrowCompletionOr<GroupsType> { move(groups) };
        }

        // d. Let value be next.
        auto value = next.release_value();

        // e. Let key be Completion(Call(callbackfn, undefined, « value, 𝔽(k) »)).
        // f. IfAbruptCloseIterator(key, iteratorRecord).
        auto key = TRY_OR_CLOSE_ITERATOR(vm, iterator_record, call(vm, callback_function, js_undefined(), value, Value(k)));

        // g. If keyCoercion is property, then
        if constexpr (IsSame<KeyType, PropertyKey>) {
            // i. Set key to Completion(ToPropertyKey(key)).
            // ii. IfAbruptCloseIterator(key, iteratorRecord).
            auto property_key = TRY_OR_CLOSE_ITERATOR(vm, iterator_record, key.to_property_key(vm));

            add_value_to_keyed_group(vm, groups, move(property_key), value);
        }
        // h. Else,
        else {
            // i. Assert: keyCoercion is zero.
            static_assert(IsSame<KeyType, void>);

            // ii. Set key to CanonicalizeKeyedCollectionKey(key).
            key = canonicalize_keyed_collection_key(key);

            add_value_to_keyed_group(vm, groups, make_root(key), value);
        }

        // i. Perform AddValueToKeyedGroup(groups, key, value).
        // NOTE: This is dependent on the `key_coercion` template parameter and thus done separately in the branches above.

        // j. Set k to k + 1.
        ++k;
    }
}

// x modulo y, https://tc39.es/ecma262/#eqn-modulo
template<Arithmetic T, Arithmetic U>
auto modulo(T x, U y)
{
    // The notation “x modulo y” (y must be finite and non-zero) computes a value k of the same sign as y (or zero) such that abs(k) < abs(y) and x - k = q × y for some integer q.
    VERIFY(y != 0);
    if constexpr (IsFloatingPoint<T> || IsFloatingPoint<U>) {
        if constexpr (IsFloatingPoint<U>)
            VERIFY(isfinite(y));
        auto r = fmod(x, y);
        return r < 0 ? r + y : r;
    } else {
        return ((x % y) + y) % y;
    }
}

auto modulo(Crypto::BigInteger auto const& x, Crypto::BigInteger auto const& y)
{
    VERIFY(!y.is_zero());
    auto result = x.divided_by(y).remainder;
    if (result.is_negative())
        result = result.plus(y);
    return result;
}

// remainder(x, y), https://tc39.es/proposal-temporal/#eqn-remainder
template<Arithmetic T, Arithmetic U>
auto remainder(T x, U y)
{
    // The mathematical function remainder(x, y) produces the mathematical value whose sign is the sign of x and whose magnitude is abs(x) modulo y.
    VERIFY(y != 0);
    if constexpr (IsFloatingPoint<T> || IsFloatingPoint<U>) {
        if constexpr (IsFloatingPoint<U>)
            VERIFY(isfinite(y));
        return fmod(x, y);
    } else {
        return x % y;
    }
}

auto remainder(Crypto::BigInteger auto const& x, Crypto::BigInteger auto const& y)
{
    VERIFY(!y.is_zero());
    return x.divided_by(y).remainder;
}

// 14.3 The Year-Week Record Specification Type, https://tc39.es/proposal-temporal/#sec-year-week-record-specification-type
struct YearWeek {
    Optional<u8> week;
    Optional<i32> year;
};

// 14.5.1.1 ToIntegerIfIntegral ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerifintegral
template<typename... Args>
ThrowCompletionOr<double> to_integer_if_integral(VM& vm, Value argument, ErrorType const& error_type, Args&&... args)
{
    // 1. Let number be ? ToNumber(argument).
    auto number = TRY(argument.to_number(vm));

    // 2. If number is not an integral Number, throw a RangeError exception.
    if (!number.is_integral_number())
        return vm.throw_completion<RangeError>(error_type, forward<Args>(args)...);

    // 3. Return ℝ(number).
    return number.as_double();
}

enum class OptionType {
    Boolean,
    String,
};

struct Required { };
using OptionDefault = Variant<Required, Empty, bool, StringView, double>;

ThrowCompletionOr<GC::Ref<Object>> get_options_object(VM&, Value options);
ThrowCompletionOr<Value> get_option(VM&, Object const& options, PropertyKey const& property, OptionType type, ReadonlySpan<StringView> values, OptionDefault const&);

template<size_t Size>
ThrowCompletionOr<Value> get_option(VM& vm, Object const& options, PropertyKey const& property, OptionType type, StringView const (&values)[Size], OptionDefault const& default_)
{
    return get_option(vm, options, property, type, ReadonlySpan<StringView> { values }, default_);
}

// https://tc39.es/proposal-temporal/#table-temporal-rounding-modes
enum class RoundingMode {
    Ceil,
    Floor,
    Expand,
    Trunc,
    HalfCeil,
    HalfFloor,
    HalfExpand,
    HalfTrunc,
    HalfEven,
};

ThrowCompletionOr<RoundingMode> get_rounding_mode_option(VM&, Object const& options, RoundingMode fallback);
ThrowCompletionOr<u64> get_rounding_increment_option(VM&, Object const& options);

Crypto::SignedBigInteger big_floor(Crypto::SignedBigInteger const& numerator, Crypto::UnsignedBigInteger const& denominator);

}
