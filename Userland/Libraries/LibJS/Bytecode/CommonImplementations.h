/*
 * Copyright (c) 2021-2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/CommonImplementations.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Bytecode {

// NOTE: This function assumes that the index is valid within the TypedArray,
//       and that the TypedArray is not detached.
template<typename T>
inline Value fast_typed_array_get_element(TypedArrayBase& typed_array, u32 index)
{
    Checked<u32> offset_into_array_buffer = index;
    offset_into_array_buffer *= sizeof(T);
    offset_into_array_buffer += typed_array.byte_offset();

    if (offset_into_array_buffer.has_overflow()) [[unlikely]] {
        return js_undefined();
    }

    auto const& array_buffer = *typed_array.viewed_array_buffer();
    auto const* slot = reinterpret_cast<T const*>(array_buffer.buffer().offset_pointer(offset_into_array_buffer.value()));
    return Value { *slot };
}

// NOTE: This function assumes that the index is valid within the TypedArray,
//       and that the TypedArray is not detached.
template<typename T>
inline void fast_typed_array_set_element(TypedArrayBase& typed_array, u32 index, T value)
{
    Checked<u32> offset_into_array_buffer = index;
    offset_into_array_buffer *= sizeof(T);
    offset_into_array_buffer += typed_array.byte_offset();

    if (offset_into_array_buffer.has_overflow()) [[unlikely]] {
        return;
    }

    auto& array_buffer = *typed_array.viewed_array_buffer();
    auto* slot = reinterpret_cast<T*>(array_buffer.buffer().offset_pointer(offset_into_array_buffer.value()));
    *slot = value;
}

template<typename BaseType, typename PropertyType>
ALWAYS_INLINE Completion throw_null_or_undefined_property_access(VM& vm, Value base_value, BaseType const& base_identifier, PropertyType const& property_identifier)
{
    VERIFY(base_value.is_nullish());

    bool has_base_identifier = true;
    bool has_property_identifier = true;

    if constexpr (requires { base_identifier.has_value(); })
        has_base_identifier = base_identifier.has_value();
    if constexpr (requires { property_identifier.has_value(); })
        has_property_identifier = property_identifier.has_value();

    if (has_base_identifier && has_property_identifier)
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithPropertyAndName, property_identifier, base_value, base_identifier);
    if (has_property_identifier)
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithProperty, property_identifier, base_value);
    if (has_base_identifier)
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithName, base_identifier, base_value);
    return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefined);
}

template<typename BaseType, typename PropertyType>
ALWAYS_INLINE ThrowCompletionOr<NonnullGCPtr<Object>> base_object_for_get(VM& vm, Value base_value, BaseType const& base_identifier, PropertyType property_identifier)
{
    if (base_value.is_object()) [[likely]]
        return base_value.as_object();

    // OPTIMIZATION: For various primitives we can avoid actually creating a new object for them.
    auto& realm = *vm.current_realm();
    if (base_value.is_string())
        return realm.intrinsics().string_prototype();
    if (base_value.is_number())
        return realm.intrinsics().number_prototype();
    if (base_value.is_boolean())
        return realm.intrinsics().boolean_prototype();
    if (base_value.is_bigint())
        return realm.intrinsics().bigint_prototype();
    if (base_value.is_symbol())
        return realm.intrinsics().symbol_prototype();

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    return throw_null_or_undefined_property_access(vm, base_value, base_identifier, property_identifier);
}

enum class GetByIdMode {
    Normal,
    Length,
};

template<GetByIdMode mode = GetByIdMode::Normal>
inline ThrowCompletionOr<Value> get_by_id(VM& vm, Optional<DeprecatedFlyString const&> const& base_identifier, DeprecatedFlyString const& property, Value base_value, Value this_value, PropertyLookupCache& cache)
{
    if constexpr (mode == GetByIdMode::Length) {
        if (base_value.is_string()) {
            return Value(base_value.as_string().utf16_string().length_in_code_units());
        }
    }

    auto base_obj = TRY(base_object_for_get(vm, base_value, base_identifier, property));

    if constexpr (mode == GetByIdMode::Length) {
        // OPTIMIZATION: Fast path for the magical "length" property on Array objects.
        if (base_obj->has_magical_length_property()) {
            return Value { base_obj->indexed_properties().array_like_size() };
        }
    }

    auto& shape = base_obj->shape();

    if (cache.prototype) {
        // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
        bool can_use_cache = [&]() -> bool {
            if (&shape != cache.shape)
                return false;
            if (!cache.prototype_chain_validity)
                return false;
            if (!cache.prototype_chain_validity->is_valid())
                return false;
            return true;
        }();
        if (can_use_cache)
            return cache.prototype->get_direct(cache.property_offset.value());
    } else if (&shape == cache.shape) {
        // OPTIMIZATION: If the shape of the object hasn't changed, we can use the cached property offset.
        return base_obj->get_direct(cache.property_offset.value());
    }

    CacheablePropertyMetadata cacheable_metadata;
    auto value = TRY(base_obj->internal_get(property, this_value, &cacheable_metadata));

    if (cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
        cache = {};
        cache.shape = shape;
        cache.property_offset = cacheable_metadata.property_offset.value();
    } else if (cacheable_metadata.type == CacheablePropertyMetadata::Type::InPrototypeChain) {
        cache = {};
        cache.shape = &base_obj->shape();
        cache.property_offset = cacheable_metadata.property_offset.value();
        cache.prototype = *cacheable_metadata.prototype;
        cache.prototype_chain_validity = *cacheable_metadata.prototype->shape().prototype_chain_validity();
    }

    return value;
}

inline ThrowCompletionOr<Value> get_by_value(VM& vm, Optional<DeprecatedFlyString const&> const& base_identifier, Value base_value, Value property_key_value)
{
    // OPTIMIZATION: Fast path for simple Int32 indexes in array-like objects.
    if (base_value.is_object() && property_key_value.is_int32() && property_key_value.as_i32() >= 0) {
        auto& object = base_value.as_object();
        auto index = static_cast<u32>(property_key_value.as_i32());

        auto const* object_storage = object.indexed_properties().storage();

        // For "non-typed arrays":
        if (!object.may_interfere_with_indexed_property_access()
            && object_storage) {
            auto maybe_value = [&] {
                if (object_storage->is_simple_storage())
                    return static_cast<SimpleIndexedPropertyStorage const*>(object_storage)->inline_get(index);
                else
                    return static_cast<GenericIndexedPropertyStorage const*>(object_storage)->get(index);
            }();
            if (maybe_value.has_value()) {
                auto value = maybe_value->value;
                if (!value.is_accessor())
                    return value;
            }
        }

        // For typed arrays:
        if (object.is_typed_array()) {
            auto& typed_array = static_cast<TypedArrayBase&>(object);
            auto canonical_index = CanonicalIndex { CanonicalIndex::Type::Index, index };

            if (is_valid_integer_index(typed_array, canonical_index)) {
                switch (typed_array.kind()) {
                case TypedArrayBase::Kind::Uint8Array:
                    return fast_typed_array_get_element<u8>(typed_array, index);
                case TypedArrayBase::Kind::Uint16Array:
                    return fast_typed_array_get_element<u16>(typed_array, index);
                case TypedArrayBase::Kind::Uint32Array:
                    return fast_typed_array_get_element<u32>(typed_array, index);
                case TypedArrayBase::Kind::Int8Array:
                    return fast_typed_array_get_element<i8>(typed_array, index);
                case TypedArrayBase::Kind::Int16Array:
                    return fast_typed_array_get_element<i16>(typed_array, index);
                case TypedArrayBase::Kind::Int32Array:
                    return fast_typed_array_get_element<i32>(typed_array, index);
                case TypedArrayBase::Kind::Uint8ClampedArray:
                    return fast_typed_array_get_element<u8>(typed_array, index);
                default:
                    // FIXME: Support more TypedArray kinds.
                    break;
                }
            }

            switch (typed_array.kind()) {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    case TypedArrayBase::Kind::ClassName:                                           \
        return typed_array_get_element<Type>(typed_array, canonical_index);
                JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
            }
        }
    }

    auto object = TRY(base_object_for_get(vm, base_value, base_identifier, property_key_value));

    auto property_key = TRY(property_key_value.to_property_key(vm));

    if (base_value.is_string()) {
        auto string_value = TRY(base_value.as_string().get(vm, property_key));
        if (string_value.has_value())
            return *string_value;
    }

    return TRY(object->internal_get(property_key, base_value));
}

inline ThrowCompletionOr<Value> get_global(Interpreter& interpreter, IdentifierTableIndex identifier_index, GlobalVariableCache& cache)
{
    auto& vm = interpreter.vm();
    auto& binding_object = interpreter.global_object();
    auto& declarative_record = interpreter.global_declarative_environment();

    // OPTIMIZATION: If the shape of the object hasn't changed, we can use the cached property offset.
    auto& shape = binding_object.shape();
    if (cache.environment_serial_number == declarative_record.environment_serial_number()
        && &shape == cache.shape) {
        return binding_object.get_direct(cache.property_offset.value());
    }

    cache.environment_serial_number = declarative_record.environment_serial_number();

    auto& identifier = interpreter.current_executable().get_identifier(identifier_index);

    if (vm.running_execution_context().script_or_module.has<NonnullGCPtr<Module>>()) {
        // NOTE: GetGlobal is used to access variables stored in the module environment and global environment.
        //       The module environment is checked first since it precedes the global environment in the environment chain.
        auto& module_environment = *vm.running_execution_context().script_or_module.get<NonnullGCPtr<Module>>()->environment();
        if (TRY(module_environment.has_binding(identifier))) {
            // TODO: Cache offset of binding value
            return TRY(module_environment.get_binding_value(vm, identifier, vm.in_strict_mode()));
        }
    }

    if (TRY(declarative_record.has_binding(identifier))) {
        // TODO: Cache offset of binding value
        return TRY(declarative_record.get_binding_value(vm, identifier, vm.in_strict_mode()));
    }

    if (TRY(binding_object.has_property(identifier))) {
        CacheablePropertyMetadata cacheable_metadata;
        auto value = TRY(binding_object.internal_get(identifier, js_undefined(), &cacheable_metadata));
        if (cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
            cache.shape = shape;
            cache.property_offset = cacheable_metadata.property_offset.value();
        }
        return value;
    }

    return vm.throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, identifier);
}

inline ThrowCompletionOr<void> put_by_property_key(VM& vm, Value base, Value this_value, Value value, Optional<DeprecatedFlyString const&> const& base_identifier, PropertyKey name, Op::PropertyKind kind, PropertyLookupCache* cache = nullptr)
{
    // Better error message than to_object would give
    if (vm.in_strict_mode() && base.is_nullish())
        return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base.to_string_without_side_effects());

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto maybe_object = base.to_object(vm);
    if (maybe_object.is_error())
        return throw_null_or_undefined_property_access(vm, base, base_identifier, name);
    auto object = maybe_object.release_value();

    if (kind == Op::PropertyKind::Getter || kind == Op::PropertyKind::Setter) {
        // The generator should only pass us functions for getters and setters.
        VERIFY(value.is_function());
    }
    switch (kind) {
    case Op::PropertyKind::Getter: {
        auto& function = value.as_function();
        if (function.name().is_empty() && is<ECMAScriptFunctionObject>(function))
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(ByteString::formatted("get {}", name));
        object->define_direct_accessor(name, &function, nullptr, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case Op::PropertyKind::Setter: {
        auto& function = value.as_function();
        if (function.name().is_empty() && is<ECMAScriptFunctionObject>(function))
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(ByteString::formatted("set {}", name));
        object->define_direct_accessor(name, nullptr, &function, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case Op::PropertyKind::KeyValue: {
        if (cache && cache->shape == &object->shape()) {
            object->put_direct(*cache->property_offset, value);
            return {};
        }

        CacheablePropertyMetadata cacheable_metadata;
        bool succeeded = TRY(object->internal_set(name, value, this_value, &cacheable_metadata));

        if (succeeded && cache && cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
            cache->shape = object->shape();
            cache->property_offset = cacheable_metadata.property_offset.value();
        }

        if (!succeeded && vm.in_strict_mode()) {
            if (base.is_object())
                return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base.to_string_without_side_effects());
            return vm.throw_completion<TypeError>(ErrorType::ReferencePrimitiveSetProperty, name, base.typeof(), base.to_string_without_side_effects());
        }
        break;
    }
    case Op::PropertyKind::DirectKeyValue:
        object->define_direct_property(name, value, Attribute::Enumerable | Attribute::Writable | Attribute::Configurable);
        break;
    case Op::PropertyKind::Spread:
        TRY(object->copy_data_properties(vm, value, {}));
        break;
    case Op::PropertyKind::ProtoSetter:
        if (value.is_object() || value.is_null())
            MUST(object->internal_set_prototype_of(value.is_object() ? &value.as_object() : nullptr));
        break;
    }

    return {};
}

inline ThrowCompletionOr<Value> perform_call(Interpreter& interpreter, Value this_value, Op::CallType call_type, Value callee, ReadonlySpan<Value> argument_values)
{
    auto& vm = interpreter.vm();
    auto& function = callee.as_function();
    Value return_value;
    if (call_type == Op::CallType::DirectEval) {
        if (callee == interpreter.realm().intrinsics().eval_function())
            return_value = TRY(perform_eval(vm, !argument_values.is_empty() ? argument_values[0].value_or(JS::js_undefined()) : js_undefined(), vm.in_strict_mode() ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
        else
            return_value = TRY(JS::call(vm, function, this_value, argument_values));
    } else if (call_type == Op::CallType::Call)
        return_value = TRY(JS::call(vm, function, this_value, argument_values));
    else
        return_value = TRY(construct(vm, function, argument_values));

    return return_value;
}

static inline Completion throw_type_error_for_callee(Bytecode::Interpreter& interpreter, Value callee, StringView callee_type, Optional<StringTableIndex> const& expression_string)
{
    auto& vm = interpreter.vm();

    if (expression_string.has_value())
        return vm.throw_completion<TypeError>(ErrorType::IsNotAEvaluatedFrom, callee.to_string_without_side_effects(), callee_type, interpreter.current_executable().get_string(expression_string->value()));

    return vm.throw_completion<TypeError>(ErrorType::IsNotA, callee.to_string_without_side_effects(), callee_type);
}

inline ThrowCompletionOr<void> throw_if_needed_for_call(Interpreter& interpreter, Value callee, Op::CallType call_type, Optional<StringTableIndex> const& expression_string)
{
    if ((call_type == Op::CallType::Call || call_type == Op::CallType::DirectEval)
        && !callee.is_function())
        return throw_type_error_for_callee(interpreter, callee, "function"sv, expression_string);
    if (call_type == Op::CallType::Construct && !callee.is_constructor())
        return throw_type_error_for_callee(interpreter, callee, "constructor"sv, expression_string);
    return {};
}

inline Value new_function(VM& vm, FunctionNode const& function_node, Optional<IdentifierTableIndex> const& lhs_name, Optional<Operand> const& home_object)
{
    Value value;

    if (!function_node.has_name()) {
        DeprecatedFlyString name = {};
        if (lhs_name.has_value())
            name = vm.bytecode_interpreter().current_executable().get_identifier(lhs_name.value());
        value = function_node.instantiate_ordinary_function_expression(vm, name);
    } else {
        value = ECMAScriptFunctionObject::create(*vm.current_realm(), function_node.name(), function_node.source_text(), function_node.body(), function_node.parameters(), function_node.function_length(), function_node.local_variables_names(), vm.lexical_environment(), vm.running_execution_context().private_environment, function_node.kind(), function_node.is_strict_mode(),
            function_node.parsing_insights(), function_node.is_arrow_function());
    }

    if (home_object.has_value()) {
        auto home_object_value = vm.bytecode_interpreter().get(home_object.value());
        static_cast<ECMAScriptFunctionObject&>(value.as_function()).set_home_object(&home_object_value.as_object());
    }

    return value;
}

inline ThrowCompletionOr<void> put_by_value(VM& vm, Value base, Optional<DeprecatedFlyString const&> const& base_identifier, Value property_key_value, Value value, Op::PropertyKind kind)
{
    // OPTIMIZATION: Fast path for simple Int32 indexes in array-like objects.
    if ((kind == Op::PropertyKind::KeyValue || kind == Op::PropertyKind::DirectKeyValue)
        && base.is_object() && property_key_value.is_int32() && property_key_value.as_i32() >= 0) {
        auto& object = base.as_object();
        auto* storage = object.indexed_properties().storage();
        auto index = static_cast<u32>(property_key_value.as_i32());

        // For "non-typed arrays":
        if (storage
            && storage->is_simple_storage()
            && !object.may_interfere_with_indexed_property_access()) {
            auto maybe_value = storage->get(index);
            if (maybe_value.has_value()) {
                auto existing_value = maybe_value->value;
                if (!existing_value.is_accessor()) {
                    storage->put(index, value);
                    return {};
                }
            }
        }

        // For typed arrays:
        if (object.is_typed_array()) {
            auto& typed_array = static_cast<TypedArrayBase&>(object);
            auto canonical_index = CanonicalIndex { CanonicalIndex::Type::Index, index };

            if (value.is_int32() && is_valid_integer_index(typed_array, canonical_index)) {
                switch (typed_array.kind()) {
                case TypedArrayBase::Kind::Uint8Array:
                    fast_typed_array_set_element<u8>(typed_array, index, static_cast<u8>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Uint16Array:
                    fast_typed_array_set_element<u16>(typed_array, index, static_cast<u16>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Uint32Array:
                    fast_typed_array_set_element<u32>(typed_array, index, static_cast<u32>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Int8Array:
                    fast_typed_array_set_element<i8>(typed_array, index, static_cast<i8>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Int16Array:
                    fast_typed_array_set_element<i16>(typed_array, index, static_cast<i16>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Int32Array:
                    fast_typed_array_set_element<i32>(typed_array, index, value.as_i32());
                    return {};
                case TypedArrayBase::Kind::Uint8ClampedArray:
                    fast_typed_array_set_element<u8>(typed_array, index, clamp(value.as_i32(), 0, 255));
                    return {};
                default:
                    // FIXME: Support more TypedArray kinds.
                    break;
                }
            }

            if (typed_array.kind() == TypedArrayBase::Kind::Uint32Array && value.is_integral_number()) {
                auto integer = value.as_double();

                if (AK::is_within_range<u32>(integer) && is_valid_integer_index(typed_array, canonical_index)) {
                    fast_typed_array_set_element<u32>(typed_array, index, static_cast<u32>(integer));
                    return {};
                }
            }

            switch (typed_array.kind()) {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    case TypedArrayBase::Kind::ClassName:                                           \
        return typed_array_set_element<Type>(typed_array, canonical_index, value);
                JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
            }
            return {};
        }
    }

    auto property_key = kind != Op::PropertyKind::Spread ? TRY(property_key_value.to_property_key(vm)) : PropertyKey {};
    TRY(put_by_property_key(vm, base, base, value, base_identifier, property_key, kind));
    return {};
}

struct CalleeAndThis {
    Value callee;
    Value this_value;
};

inline ThrowCompletionOr<CalleeAndThis> get_callee_and_this_from_environment(Bytecode::Interpreter& interpreter, DeprecatedFlyString const& name, EnvironmentCoordinate& cache)
{
    auto& vm = interpreter.vm();

    Value callee = js_undefined();
    Value this_value = js_undefined();

    if (cache.is_valid()) {
        auto const* environment = interpreter.running_execution_context().lexical_environment.ptr();
        for (size_t i = 0; i < cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) {
            callee = TRY(static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(vm, cache.index));
            this_value = js_undefined();
            if (auto base_object = environment->with_base_object())
                this_value = base_object;
            return CalleeAndThis {
                .callee = callee,
                .this_value = this_value,
            };
        }
        cache = {};
    }

    auto reference = TRY(vm.resolve_binding(name));
    if (reference.environment_coordinate().has_value())
        cache = reference.environment_coordinate().value();

    callee = TRY(reference.get_value(vm));

    if (reference.is_property_reference()) {
        this_value = reference.get_this_value();
    } else {
        if (reference.is_environment_reference()) {
            if (auto base_object = reference.base_environment().with_base_object(); base_object != nullptr)
                this_value = base_object;
        }
    }

    return CalleeAndThis {
        .callee = callee,
        .this_value = this_value,
    };
}

// 13.2.7.3 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-regular-expression-literals-runtime-semantics-evaluation
inline Value new_regexp(VM& vm, ParsedRegex const& parsed_regex, ByteString const& pattern, ByteString const& flags)
{
    // 1. Let pattern be CodePointsToString(BodyText of RegularExpressionLiteral).
    // 2. Let flags be CodePointsToString(FlagText of RegularExpressionLiteral).

    // 3. Return ! RegExpCreate(pattern, flags).
    auto& realm = *vm.current_realm();
    Regex<ECMA262> regex(parsed_regex.regex, parsed_regex.pattern, parsed_regex.flags);
    // NOTE: We bypass RegExpCreate and subsequently RegExpAlloc as an optimization to use the already parsed values.
    auto regexp_object = RegExpObject::create(realm, move(regex), pattern, flags);
    // RegExpAlloc has these two steps from the 'Legacy RegExp features' proposal.
    regexp_object->set_realm(realm);
    // We don't need to check 'If SameValue(newTarget, thisRealm.[[Intrinsics]].[[%RegExp%]]) is true'
    // here as we know RegExpCreate calls RegExpAlloc with %RegExp% for newTarget.
    regexp_object->set_legacy_features_enabled(true);
    return regexp_object;
}

// 13.3.8.1 https://tc39.es/ecma262/#sec-runtime-semantics-argumentlistevaluation
inline MarkedVector<Value> argument_list_evaluation(VM& vm, Value arguments)
{
    // Note: Any spreading and actual evaluation is handled in preceding opcodes
    // Note: The spec uses the concept of a list, while we create a temporary array
    //       in the preceding opcodes, so we have to convert in a manner that is not
    //       visible to the user
    MarkedVector<Value> argument_values { vm.heap() };

    auto& argument_array = arguments.as_array();
    auto array_length = argument_array.indexed_properties().array_like_size();

    argument_values.ensure_capacity(array_length);

    for (size_t i = 0; i < array_length; ++i) {
        if (auto maybe_value = argument_array.indexed_properties().get(i); maybe_value.has_value())
            argument_values.append(maybe_value.release_value().value);
        else
            argument_values.append(js_undefined());
    }

    return argument_values;
}

inline ThrowCompletionOr<void> create_variable(VM& vm, DeprecatedFlyString const& name, Op::EnvironmentMode mode, bool is_global, bool is_immutable, bool is_strict)
{
    if (mode == Op::EnvironmentMode::Lexical) {
        VERIFY(!is_global);

        // Note: This is papering over an issue where "FunctionDeclarationInstantiation" creates these bindings for us.
        //       Instead of crashing in there, we'll just raise an exception here.
        if (TRY(vm.lexical_environment()->has_binding(name)))
            return vm.throw_completion<InternalError>(TRY_OR_THROW_OOM(vm, String::formatted("Lexical environment already has binding '{}'", name)));

        if (is_immutable)
            return vm.lexical_environment()->create_immutable_binding(vm, name, is_strict);
        return vm.lexical_environment()->create_mutable_binding(vm, name, is_strict);
    }

    if (!is_global) {
        if (is_immutable)
            return vm.variable_environment()->create_immutable_binding(vm, name, is_strict);
        return vm.variable_environment()->create_mutable_binding(vm, name, is_strict);
    }

    // NOTE: CreateVariable with m_is_global set to true is expected to only be used in GlobalDeclarationInstantiation currently, which only uses "false" for "can_be_deleted".
    //       The only area that sets "can_be_deleted" to true is EvalDeclarationInstantiation, which is currently fully implemented in C++ and not in Bytecode.
    return verify_cast<GlobalEnvironment>(vm.variable_environment())->create_global_var_binding(name, false);
}

inline ThrowCompletionOr<ECMAScriptFunctionObject*> new_class(VM& vm, Value super_class, ClassExpression const& class_expression, Optional<IdentifierTableIndex> const& lhs_name, ReadonlySpan<Value> element_keys)
{
    auto& interpreter = vm.bytecode_interpreter();
    auto name = class_expression.name();

    // NOTE: NewClass expects classEnv to be active lexical environment
    auto* class_environment = vm.lexical_environment();
    vm.running_execution_context().lexical_environment = vm.running_execution_context().saved_lexical_environments.take_last();

    Optional<DeprecatedFlyString> binding_name;
    DeprecatedFlyString class_name;
    if (!class_expression.has_name() && lhs_name.has_value()) {
        class_name = interpreter.current_executable().get_identifier(lhs_name.value());
    } else {
        binding_name = name;
        class_name = name.is_null() ? ""sv : name;
    }

    return TRY(class_expression.create_class_constructor(vm, class_environment, vm.lexical_environment(), super_class, element_keys, binding_name, class_name));
}

// 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
inline ThrowCompletionOr<NonnullGCPtr<Object>> super_call_with_argument_array(VM& vm, Value argument_array, bool is_synthetic)
{
    // 1. Let newTarget be GetNewTarget().
    auto new_target = vm.get_new_target();

    // 2. Assert: Type(newTarget) is Object.
    VERIFY(new_target.is_object());

    // 3. Let func be GetSuperConstructor().
    auto* func = get_super_constructor(vm);

    // 4. Let argList be ? ArgumentListEvaluation of Arguments.
    MarkedVector<Value> arg_list { vm.heap() };
    if (is_synthetic) {
        VERIFY(argument_array.is_object() && is<Array>(argument_array.as_object()));
        auto const& array_value = static_cast<Array const&>(argument_array.as_object());
        auto length = MUST(length_of_array_like(vm, array_value));
        for (size_t i = 0; i < length; ++i)
            arg_list.append(array_value.get_without_side_effects(PropertyKey { i }));
    } else {
        arg_list = argument_list_evaluation(vm, argument_array);
    }

    // 5. If IsConstructor(func) is false, throw a TypeError exception.
    if (!Value(func).is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, "Super constructor");

    // 6. Let result be ? Construct(func, argList, newTarget).
    auto result = TRY(construct(vm, static_cast<FunctionObject&>(*func), arg_list.span(), &new_target.as_function()));

    // 7. Let thisER be GetThisEnvironment().
    auto& this_environment = verify_cast<FunctionEnvironment>(*get_this_environment(vm));

    // 8. Perform ? thisER.BindThisValue(result).
    TRY(this_environment.bind_this_value(vm, result));

    // 9. Let F be thisER.[[FunctionObject]].
    auto& f = this_environment.function_object();

    // 10. Assert: F is an ECMAScript function object.
    // NOTE: This is implied by the strong C++ type.

    // 11. Perform ? InitializeInstanceElements(result, F).
    TRY(result->initialize_instance_elements(f));

    // 12. Return result.
    return result;
}

inline ThrowCompletionOr<NonnullGCPtr<Array>> iterator_to_array(VM& vm, Value iterator)
{
    auto& iterator_record = verify_cast<IteratorRecord>(iterator.as_object());

    auto array = MUST(Array::create(*vm.current_realm(), 0));
    size_t index = 0;

    while (true) {
        auto value = TRY(iterator_step_value(vm, iterator_record));
        if (!value.has_value())
            return array;

        MUST(array->create_data_property_or_throw(index, value.release_value()));
        index++;
    }
}

inline ThrowCompletionOr<void> append(VM& vm, Value lhs, Value rhs, bool is_spread)
{
    // Note: This OpCode is used to construct array literals and argument arrays for calls,
    //       containing at least one spread element,
    //       Iterating over such a spread element to unpack it has to be visible by
    //       the user courtesy of
    //       (1) https://tc39.es/ecma262/#sec-runtime-semantics-arrayaccumulation
    //          SpreadElement : ... AssignmentExpression
    //              1. Let spreadRef be ? Evaluation of AssignmentExpression.
    //              2. Let spreadObj be ? GetValue(spreadRef).
    //              3. Let iteratorRecord be ? GetIterator(spreadObj).
    //              4. Repeat,
    //                  a. Let next be ? IteratorStep(iteratorRecord).
    //                  b. If next is false, return nextIndex.
    //                  c. Let nextValue be ? IteratorValue(next).
    //                  d. Perform ! CreateDataPropertyOrThrow(array, ! ToString(𝔽(nextIndex)), nextValue).
    //                  e. Set nextIndex to nextIndex + 1.
    //       (2) https://tc39.es/ecma262/#sec-runtime-semantics-argumentlistevaluation
    //          ArgumentList : ... AssignmentExpression
    //              1. Let list be a new empty List.
    //              2. Let spreadRef be ? Evaluation of AssignmentExpression.
    //              3. Let spreadObj be ? GetValue(spreadRef).
    //              4. Let iteratorRecord be ? GetIterator(spreadObj).
    //              5. Repeat,
    //                  a. Let next be ? IteratorStep(iteratorRecord).
    //                  b. If next is false, return list.
    //                  c. Let nextArg be ? IteratorValue(next).
    //                  d. Append nextArg to list.
    //          ArgumentList : ArgumentList , ... AssignmentExpression
    //             1. Let precedingArgs be ? ArgumentListEvaluation of ArgumentList.
    //             2. Let spreadRef be ? Evaluation of AssignmentExpression.
    //             3. Let iteratorRecord be ? GetIterator(? GetValue(spreadRef)).
    //             4. Repeat,
    //                 a. Let next be ? IteratorStep(iteratorRecord).
    //                 b. If next is false, return precedingArgs.
    //                 c. Let nextArg be ? IteratorValue(next).
    //                 d. Append nextArg to precedingArgs.

    // Note: We know from codegen, that lhs is a plain array with only indexed properties
    auto& lhs_array = lhs.as_array();
    auto lhs_size = lhs_array.indexed_properties().array_like_size();

    if (is_spread) {
        // ...rhs
        size_t i = lhs_size;
        TRY(get_iterator_values(vm, rhs, [&i, &lhs_array](Value iterator_value) -> Optional<Completion> {
            lhs_array.indexed_properties().put(i, iterator_value, default_attributes);
            ++i;
            return {};
        }));
    } else {
        lhs_array.indexed_properties().put(lhs_size, rhs, default_attributes);
    }

    return {};
}

inline ThrowCompletionOr<Value> delete_by_id(Bytecode::Interpreter& interpreter, Value base, IdentifierTableIndex property)
{
    auto& vm = interpreter.vm();

    auto const& identifier = interpreter.current_executable().get_identifier(property);
    bool strict = vm.in_strict_mode();
    auto reference = Reference { base, identifier, {}, strict };

    return TRY(reference.delete_(vm));
}

inline ThrowCompletionOr<Value> delete_by_value(Bytecode::Interpreter& interpreter, Value base, Value property_key_value)
{
    auto& vm = interpreter.vm();

    auto property_key = TRY(property_key_value.to_property_key(vm));
    bool strict = vm.in_strict_mode();
    auto reference = Reference { base, property_key, {}, strict };

    return Value(TRY(reference.delete_(vm)));
}

inline ThrowCompletionOr<Value> delete_by_value_with_this(Bytecode::Interpreter& interpreter, Value base, Value property_key_value, Value this_value)
{
    auto& vm = interpreter.vm();

    auto property_key = TRY(property_key_value.to_property_key(vm));
    bool strict = vm.in_strict_mode();
    auto reference = Reference { base, property_key, this_value, strict };

    return Value(TRY(reference.delete_(vm)));
}

// 14.7.5.9 EnumerateObjectProperties ( O ), https://tc39.es/ecma262/#sec-enumerate-object-properties
inline ThrowCompletionOr<Object*> get_object_property_iterator(VM& vm, Value value)
{
    // While the spec does provide an algorithm, it allows us to implement it ourselves so long as we meet the following invariants:
    //    1- Returned property keys do not include keys that are Symbols
    //    2- Properties of the target object may be deleted during enumeration. A property that is deleted before it is processed by the iterator's next method is ignored
    //    3- If new properties are added to the target object during enumeration, the newly added properties are not guaranteed to be processed in the active enumeration
    //    4- A property name will be returned by the iterator's next method at most once in any enumeration.
    //    5- Enumerating the properties of the target object includes enumerating properties of its prototype, and the prototype of the prototype, and so on, recursively;
    //       but a property of a prototype is not processed if it has the same name as a property that has already been processed by the iterator's next method.
    //    6- The values of [[Enumerable]] attributes are not considered when determining if a property of a prototype object has already been processed.
    //    7- The enumerable property names of prototype objects must be obtained by invoking EnumerateObjectProperties passing the prototype object as the argument.
    //    8- EnumerateObjectProperties must obtain the own property keys of the target object by calling its [[OwnPropertyKeys]] internal method.
    //    9- Property attributes of the target object must be obtained by calling its [[GetOwnProperty]] internal method

    // Invariant 3 effectively allows the implementation to ignore newly added keys, and we do so (similar to other implementations).
    auto object = TRY(value.to_object(vm));
    // Note: While the spec doesn't explicitly require these to be ordered, it says that the values should be retrieved via OwnPropertyKeys,
    //       so we just keep the order consistent anyway.
    OrderedHashTable<PropertyKey> properties;
    OrderedHashTable<PropertyKey> non_enumerable_properties;
    HashTable<NonnullGCPtr<Object>> seen_objects;
    // Collect all keys immediately (invariant no. 5)
    for (auto object_to_check = GCPtr { object.ptr() }; object_to_check && !seen_objects.contains(*object_to_check); object_to_check = TRY(object_to_check->internal_get_prototype_of())) {
        seen_objects.set(*object_to_check);
        for (auto& key : TRY(object_to_check->internal_own_property_keys())) {
            if (key.is_symbol())
                continue;
            auto property_key = TRY(PropertyKey::from_value(vm, key));

            // If there is a non-enumerable property higher up the prototype chain with the same key,
            // we mustn't include this property even if it's enumerable (invariant no. 5 and 6)
            if (non_enumerable_properties.contains(property_key))
                continue;
            if (properties.contains(property_key))
                continue;

            auto descriptor = TRY(object_to_check->internal_get_own_property(property_key));
            if (!*descriptor->enumerable)
                non_enumerable_properties.set(move(property_key));
            else
                properties.set(move(property_key));
        }
    }
    auto& realm = *vm.current_realm();
    auto callback = NativeFunction::create(
        *vm.current_realm(), [items = move(properties)](VM& vm) mutable -> ThrowCompletionOr<Value> {
            auto& realm = *vm.current_realm();
            auto iterated_object_value = vm.this_value();
            if (!iterated_object_value.is_object())
                return vm.throw_completion<InternalError>("Invalid state for GetObjectPropertyIterator.next"sv);

            auto& iterated_object = iterated_object_value.as_object();
            auto result_object = Object::create(realm, nullptr);
            while (true) {
                if (items.is_empty()) {
                    result_object->define_direct_property(vm.names.done, JS::Value(true), default_attributes);
                    return result_object;
                }

                auto key = items.take_first();

                // If the property is deleted, don't include it (invariant no. 2)
                if (!TRY(iterated_object.has_property(key)))
                    continue;

                result_object->define_direct_property(vm.names.done, JS::Value(false), default_attributes);

                if (key.is_number())
                    result_object->define_direct_property(vm.names.value, PrimitiveString::create(vm, TRY_OR_THROW_OOM(vm, String::number(key.as_number()))), default_attributes);
                else if (key.is_string())
                    result_object->define_direct_property(vm.names.value, PrimitiveString::create(vm, key.as_string()), default_attributes);
                else
                    VERIFY_NOT_REACHED(); // We should not have non-string/number keys.

                return result_object;
            }
        },
        1, vm.names.next);
    return vm.heap().allocate<IteratorRecord>(realm, realm, object, callback, false).ptr();
}

}
