/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Forward.h>
#include <LibCrypto/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/MarkedVector.h>
#include <LibJS/Runtime/CanonicalIndex.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/KeyedCollections.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

NonnullGCPtr<DeclarativeEnvironment> new_declarative_environment(Environment&);
NonnullGCPtr<ObjectEnvironment> new_object_environment(Object&, bool is_with_environment, Environment*);
NonnullGCPtr<FunctionEnvironment> new_function_environment(ECMAScriptFunctionObject&, Object* new_target);
NonnullGCPtr<PrivateEnvironment> new_private_environment(VM& vm, PrivateEnvironment* outer);
NonnullGCPtr<Environment> get_this_environment(VM&);
bool can_be_held_weakly(Value);
Object* get_super_constructor(VM&);
ThrowCompletionOr<Value> require_object_coercible(VM&, Value);
ThrowCompletionOr<Value> call_impl(VM&, Value function, Value this_value, ReadonlySpan<Value> arguments = {});
ThrowCompletionOr<Value> call_impl(VM&, FunctionObject& function, Value this_value, ReadonlySpan<Value> arguments = {});
ThrowCompletionOr<NonnullGCPtr<Object>> construct_impl(VM&, FunctionObject&, ReadonlySpan<Value> arguments = {}, FunctionObject* new_target = nullptr);
ThrowCompletionOr<size_t> length_of_array_like(VM&, Object const&);
ThrowCompletionOr<MarkedVector<Value>> create_list_from_array_like(VM&, Value, Function<ThrowCompletionOr<void>(Value)> = {});
ThrowCompletionOr<FunctionObject*> species_constructor(VM&, Object const&, FunctionObject& default_constructor);
ThrowCompletionOr<Realm*> get_function_realm(VM&, FunctionObject const&);
ThrowCompletionOr<void> initialize_bound_name(VM&, DeprecatedFlyString const&, Value, Environment*);
bool is_compatible_property_descriptor(bool extensible, PropertyDescriptor const&, Optional<PropertyDescriptor> const& current);
bool validate_and_apply_property_descriptor(Object*, PropertyKey const&, bool extensible, PropertyDescriptor const&, Optional<PropertyDescriptor> const& current);
ThrowCompletionOr<Object*> get_prototype_from_constructor(VM&, FunctionObject const& constructor, NonnullGCPtr<Object> (Intrinsics::*intrinsic_default_prototype)());
Object* create_unmapped_arguments_object(VM&, ReadonlySpan<Value> arguments);
Object* create_mapped_arguments_object(VM&, FunctionObject&, Vector<FunctionParameter> const&, ReadonlySpan<Value> arguments, Environment&);

struct DisposableResource {
    Value resource_value;
    NonnullGCPtr<FunctionObject> dispose_method;
};
ThrowCompletionOr<void> add_disposable_resource(VM&, Vector<DisposableResource>& disposable, Value, Environment::InitializeBindingHint, FunctionObject* = nullptr);
ThrowCompletionOr<DisposableResource> create_disposable_resource(VM&, Value, Environment::InitializeBindingHint, FunctionObject* method = nullptr);
ThrowCompletionOr<GCPtr<FunctionObject>> get_dispose_method(VM&, Value, Environment::InitializeBindingHint);
Completion dispose(VM& vm, Value, NonnullGCPtr<FunctionObject> method);
Completion dispose_resources(VM& vm, Vector<DisposableResource> const& disposable, Completion completion);
Completion dispose_resources(VM& vm, GCPtr<DeclarativeEnvironment> disposable, Completion completion);

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
ALWAYS_INLINE ThrowCompletionOr<NonnullGCPtr<Object>> construct(VM& vm, FunctionObject& function, Args&&... args)
{
    constexpr auto argument_count = sizeof...(Args);
    if constexpr (argument_count > 0) {
        AK::Array<Value, argument_count> arguments { forward<Args>(args)... };
        return construct_impl(vm, function, static_cast<ReadonlySpan<Value>>(arguments.span()));
    }

    return construct_impl(vm, function);
}

ALWAYS_INLINE ThrowCompletionOr<NonnullGCPtr<Object>> construct(VM& vm, FunctionObject& function, ReadonlySpan<Value> arguments_list, FunctionObject* new_target = nullptr)
{
    return construct_impl(vm, function, arguments_list, new_target);
}

ALWAYS_INLINE ThrowCompletionOr<NonnullGCPtr<Object>> construct(VM& vm, FunctionObject& function, Span<Value> arguments_list, FunctionObject* new_target = nullptr)
{
    return construct_impl(vm, function, static_cast<ReadonlySpan<Value>>(arguments_list), new_target);
}

// 10.1.13 OrdinaryCreateFromConstructor ( constructor, intrinsicDefaultProto [ , internalSlotsList ] ), https://tc39.es/ecma262/#sec-ordinarycreatefromconstructor
template<typename T, typename... Args>
ThrowCompletionOr<NonnullGCPtr<T>> ordinary_create_from_constructor(VM& vm, FunctionObject const& constructor, NonnullGCPtr<Object> (Intrinsics::*intrinsic_default_prototype)(), Args&&... args)
{
    auto& realm = *vm.current_realm();
    auto* prototype = TRY(get_prototype_from_constructor(vm, constructor, intrinsic_default_prototype));
    return realm.heap().allocate<T>(realm, forward<Args>(args)..., *prototype);
}

// 14.1 MergeLists ( a, b ), https://tc39.es/proposal-temporal/#sec-temporal-mergelists
template<typename T>
Vector<T> merge_lists(Vector<T> const& a, Vector<T> const& b)
{
    // 1. Let merged be a new empty List.
    Vector<T> merged;

    // 2. For each element element of a, do
    for (auto const& element : a) {
        // a. If merged does not contain element, then
        if (!merged.contains_slow(element)) {
            // i. Append element to merged.
            merged.append(element);
        }
    }

    // 3. For each element element of b, do
    for (auto const& element : b) {
        // a. If merged does not contain element, then
        if (!merged.contains_slow(element)) {
            // i. Append element to merged.
            merged.append(element);
        }
    }

    // 4. Return merged.
    return merged;
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
    MarkedVector<Value> new_elements { vm.heap() };
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
        auto key = call(vm, callback_function, js_undefined(), value, Value(k));

        // f. IfAbruptCloseIterator(key, iteratorRecord).
        if (key.is_error())
            return Completion { *TRY(iterator_close(vm, iterator_record, key.release_error())) };

        // g. If keyCoercion is property, then
        if constexpr (IsSame<KeyType, PropertyKey>) {
            // i. Set key to Completion(ToPropertyKey(key)).
            auto property_key = key.value().to_property_key(vm);

            // ii. IfAbruptCloseIterator(key, iteratorRecord).
            if (property_key.is_error())
                return Completion { *TRY(iterator_close(vm, iterator_record, property_key.release_error())) };

            add_value_to_keyed_group(vm, groups, property_key.release_value(), value);
        }
        // h. Else,
        else {
            // i. Assert: keyCoercion is zero.
            static_assert(IsSame<KeyType, void>);

            // ii. Set key to CanonicalizeKeyedCollectionKey(key).
            key = canonicalize_keyed_collection_key(key.value());

            add_value_to_keyed_group(vm, groups, make_handle(key.release_value()), value);
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

}
