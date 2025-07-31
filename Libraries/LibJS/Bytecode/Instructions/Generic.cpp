/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../Instructions.h"

#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/CompletionCell.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GeneratorResult.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/MathObject.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Bytecode {

//  bool has_exception_check = IsSame<decltype(declval<OP>().execute_impl(declval<Interpreter&>())), ThrowCompletionOr<void>>
template<typename OP, bool has_exception_check>
ALWAYS_INLINE FLATTEN void Interpreter::handle_generic(u8 const* bytecode, size_t& program_counter)
{
    auto const& instruction = *reinterpret_cast<OP const*>(&bytecode[program_counter]);
    if constexpr (has_exception_check) {
        auto result = instruction.execute_impl(*this);
        if (result.is_error()) [[unlikely]] {
            if (handle_exception(program_counter, result.error_value()) == HandleExceptionResponse::ExitFromExecutable)
                return;
            DISPATCH_NEXT();
        }
    } else {
        instruction.execute_impl(*this);
    }
    increment_program_counter(program_counter, instruction);
    DISPATCH_NEXT();
}

#define HANDLE_INSTRUCTION(name)                                                         \
    FLATTEN void Interpreter::handle_##name(u8 const* bytecode, size_t& program_counter) \
    {                                                                                    \
        [[clang::musttail]] return handle_generic<Op::name>(bytecode, program_counter);  \
    }

HANDLE_INSTRUCTION(Add);
HANDLE_INSTRUCTION(AddPrivateName);
HANDLE_INSTRUCTION(ArrayAppend);
HANDLE_INSTRUCTION(AsyncIteratorClose);
HANDLE_INSTRUCTION(BitwiseAnd);
HANDLE_INSTRUCTION(BitwiseNot);
HANDLE_INSTRUCTION(BitwiseOr);
HANDLE_INSTRUCTION(BitwiseXor);
HANDLE_INSTRUCTION(BlockDeclarationInstantiation);
HANDLE_INSTRUCTION(Call);
HANDLE_INSTRUCTION(CallBuiltin);
HANDLE_INSTRUCTION(CallConstruct);
HANDLE_INSTRUCTION(CallDirectEval);
HANDLE_INSTRUCTION(CallWithArgumentArray);
HANDLE_INSTRUCTION(Catch);
HANDLE_INSTRUCTION(ConcatString);
HANDLE_INSTRUCTION(CopyObjectExcludingProperties);
HANDLE_INSTRUCTION(CreateLexicalEnvironment);
HANDLE_INSTRUCTION(CreateVariableEnvironment);
HANDLE_INSTRUCTION(CreatePrivateEnvironment);
HANDLE_INSTRUCTION(CreateVariable);
HANDLE_INSTRUCTION(CreateRestParams);
HANDLE_INSTRUCTION(CreateArguments);
HANDLE_INSTRUCTION(Decrement);
HANDLE_INSTRUCTION(DeleteById);
HANDLE_INSTRUCTION(DeleteByIdWithThis);
HANDLE_INSTRUCTION(DeleteByValue);
HANDLE_INSTRUCTION(DeleteByValueWithThis);
HANDLE_INSTRUCTION(DeleteVariable);
HANDLE_INSTRUCTION(Div);
HANDLE_INSTRUCTION(Dump);
HANDLE_INSTRUCTION(EnterObjectEnvironment);
HANDLE_INSTRUCTION(Exp);
HANDLE_INSTRUCTION(GetById);
HANDLE_INSTRUCTION(GetByIdWithThis);
HANDLE_INSTRUCTION(GetByValue);
HANDLE_INSTRUCTION(GetByValueWithThis);
HANDLE_INSTRUCTION(GetCalleeAndThisFromEnvironment);
HANDLE_INSTRUCTION(GetCompletionFields);
HANDLE_INSTRUCTION(GetGlobal);
HANDLE_INSTRUCTION(GetImportMeta);
HANDLE_INSTRUCTION(GetIterator);
HANDLE_INSTRUCTION(GetLength);
HANDLE_INSTRUCTION(GetLengthWithThis);
HANDLE_INSTRUCTION(GetMethod);
HANDLE_INSTRUCTION(GetNewTarget);
HANDLE_INSTRUCTION(GetNextMethodFromIteratorRecord);
HANDLE_INSTRUCTION(GetObjectFromIteratorRecord);
HANDLE_INSTRUCTION(GetObjectPropertyIterator);
HANDLE_INSTRUCTION(GetPrivateById);
HANDLE_INSTRUCTION(GetBinding);
HANDLE_INSTRUCTION(GetInitializedBinding);
HANDLE_INSTRUCTION(GreaterThan);
HANDLE_INSTRUCTION(GreaterThanEquals);
HANDLE_INSTRUCTION(HasPrivateId);
HANDLE_INSTRUCTION(ImportCall);
HANDLE_INSTRUCTION(In);
HANDLE_INSTRUCTION(Increment);
HANDLE_INSTRUCTION(InitializeLexicalBinding);
HANDLE_INSTRUCTION(InitializeVariableBinding);
HANDLE_INSTRUCTION(InstanceOf);
HANDLE_INSTRUCTION(IteratorClose);
HANDLE_INSTRUCTION(IteratorNext);
HANDLE_INSTRUCTION(IteratorNextUnpack);
HANDLE_INSTRUCTION(IteratorToArray);
HANDLE_INSTRUCTION(LeaveFinally);
HANDLE_INSTRUCTION(LeaveLexicalEnvironment);
HANDLE_INSTRUCTION(LeavePrivateEnvironment);
HANDLE_INSTRUCTION(LeaveUnwindContext);
HANDLE_INSTRUCTION(LeftShift);
HANDLE_INSTRUCTION(LessThan);
HANDLE_INSTRUCTION(LessThanEquals);
HANDLE_INSTRUCTION(LooselyEquals);
HANDLE_INSTRUCTION(LooselyInequals);
HANDLE_INSTRUCTION(Mod);
HANDLE_INSTRUCTION(Mul);
HANDLE_INSTRUCTION(NewArray);
HANDLE_INSTRUCTION(NewClass);
HANDLE_INSTRUCTION(NewFunction);
HANDLE_INSTRUCTION(NewObject);
HANDLE_INSTRUCTION(NewPrimitiveArray);
HANDLE_INSTRUCTION(NewRegExp);
HANDLE_INSTRUCTION(NewTypeError);
HANDLE_INSTRUCTION(Not);
HANDLE_INSTRUCTION(PostfixDecrement);
HANDLE_INSTRUCTION(PostfixIncrement);
HANDLE_INSTRUCTION(PutById);
HANDLE_INSTRUCTION(PutByIdWithThis);
HANDLE_INSTRUCTION(PutBySpread);
HANDLE_INSTRUCTION(PutByValue);
HANDLE_INSTRUCTION(PutByValueWithThis);
HANDLE_INSTRUCTION(PutPrivateById);
HANDLE_INSTRUCTION(ResolveSuperBase);
HANDLE_INSTRUCTION(ResolveThisBinding);
HANDLE_INSTRUCTION(RestoreScheduledJump);
HANDLE_INSTRUCTION(RightShift);
HANDLE_INSTRUCTION(SetCompletionType);
HANDLE_INSTRUCTION(SetGlobal);
HANDLE_INSTRUCTION(SetLexicalBinding);
HANDLE_INSTRUCTION(SetVariableBinding);
HANDLE_INSTRUCTION(StrictlyEquals);
HANDLE_INSTRUCTION(StrictlyInequals);
HANDLE_INSTRUCTION(Sub);
HANDLE_INSTRUCTION(SuperCallWithArgumentArray);
HANDLE_INSTRUCTION(Throw);
HANDLE_INSTRUCTION(ThrowIfNotObject);
HANDLE_INSTRUCTION(ThrowIfNullish);
HANDLE_INSTRUCTION(ThrowIfTDZ);
HANDLE_INSTRUCTION(Typeof);
HANDLE_INSTRUCTION(TypeofBinding);
HANDLE_INSTRUCTION(UnaryMinus);
HANDLE_INSTRUCTION(UnaryPlus);
HANDLE_INSTRUCTION(UnsignedRightShift);

}

namespace JS {

struct PropertyKeyAndEnumerableFlag {
    JS::PropertyKey key;
    bool enumerable { false };
};

}

namespace AK {

template<>
struct Traits<JS::PropertyKeyAndEnumerableFlag> : public DefaultTraits<JS::PropertyKeyAndEnumerableFlag> {
    static unsigned hash(JS::PropertyKeyAndEnumerableFlag const& entry)
    {
        return Traits<JS::PropertyKey>::hash(entry.key);
    }

    static bool equals(JS::PropertyKeyAndEnumerableFlag const& a, JS::PropertyKeyAndEnumerableFlag const& b)
    {
        return Traits<JS::PropertyKey>::equals(a.key, b.key);
    }
};

}
// FIXME: We can likely inline these directly
// FIXME: Maybe use more files to organize these better

namespace JS::Bytecode {

ALWAYS_INLINE static ThrowCompletionOr<bool> loosely_inequals(VM& vm, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() != src2.encoded();
    }
    return !TRY(is_loosely_equal(vm, src1, src2));
}

ALWAYS_INLINE static ThrowCompletionOr<bool> loosely_equals(VM& vm, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() == src2.encoded();
    }
    return TRY(is_loosely_equal(vm, src1, src2));
}

ALWAYS_INLINE static ThrowCompletionOr<bool> strict_inequals(VM&, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() != src2.encoded();
    }
    return !is_strictly_equal(src1, src2);
}

ALWAYS_INLINE static ThrowCompletionOr<bool> strict_equals(VM&, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() == src2.encoded();
    }
    return is_strictly_equal(src1, src2);
}

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

static Completion throw_null_or_undefined_property_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, IdentifierTableIndex property_identifier, Executable const& executable)
{
    VERIFY(base_value.is_nullish());

    if (base_identifier.has_value())
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithPropertyAndName, executable.get_identifier(property_identifier), base_value, executable.get_identifier(base_identifier.value()));
    return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithProperty, executable.get_identifier(property_identifier), base_value);
}

static Completion throw_null_or_undefined_property_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, Value property, Executable const& executable)
{
    VERIFY(base_value.is_nullish());

    if (base_identifier.has_value())
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithPropertyAndName, property, base_value, executable.get_identifier(base_identifier.value()));
    return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithProperty, property, base_value);
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

ALWAYS_INLINE GC::Ptr<Object> base_object_for_get_impl(VM& vm, Value base_value)
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

    return nullptr;
}

ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> base_object_for_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, IdentifierTableIndex property_identifier, Executable const& executable)
{
    if (auto base_object = base_object_for_get_impl(vm, base_value))
        return GC::Ref { *base_object };

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    return throw_null_or_undefined_property_get(vm, base_value, base_identifier, property_identifier, executable);
}

ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> base_object_for_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, Value property, Executable const& executable)
{
    if (auto base_object = base_object_for_get_impl(vm, base_value))
        return GC::Ref { *base_object };

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    return throw_null_or_undefined_property_get(vm, base_value, base_identifier, property, executable);
}

enum class GetByIdMode {
    Normal,
    Length,
};

template<GetByIdMode mode = GetByIdMode::Normal>
inline ThrowCompletionOr<Value> get_by_id(VM& vm, Optional<IdentifierTableIndex> base_identifier, IdentifierTableIndex property, Value base_value, Value this_value, PropertyLookupCache& cache, Executable const& executable)
{
    if constexpr (mode == GetByIdMode::Length) {
        if (base_value.is_string()) {
            return Value(base_value.as_string().length_in_utf16_code_units());
        }
    }

    auto base_obj = TRY(base_object_for_get(vm, base_value, base_identifier, property, executable));

    if constexpr (mode == GetByIdMode::Length) {
        // OPTIMIZATION: Fast path for the magical "length" property on Array objects.
        if (base_obj->has_magical_length_property()) {
            return Value { base_obj->indexed_properties().array_like_size() };
        }
    }

    auto& shape = base_obj->shape();

    for (auto& cache_entry : cache.entries) {
        if (cache_entry.prototype) {
            // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
            bool can_use_cache = [&]() -> bool {
                if (&shape != cache_entry.shape)
                    return false;
                if (!cache_entry.prototype_chain_validity)
                    return false;
                if (!cache_entry.prototype_chain_validity->is_valid())
                    return false;
                return true;
            }();
            if (can_use_cache) {
                auto value = cache_entry.prototype->get_direct(cache_entry.property_offset.value());
                if (value.is_accessor())
                    return TRY(call(vm, value.as_accessor().getter(), this_value));
                return value;
            }
        } else if (&shape == cache_entry.shape) {
            // OPTIMIZATION: If the shape of the object hasn't changed, we can use the cached property offset.
            auto value = base_obj->get_direct(cache_entry.property_offset.value());
            if (value.is_accessor())
                return TRY(call(vm, value.as_accessor().getter(), this_value));
            return value;
        }
    }

    CacheablePropertyMetadata cacheable_metadata;
    auto value = TRY(base_obj->internal_get(executable.get_identifier(property), this_value, &cacheable_metadata));

    // If internal_get() caused object's shape change, we can no longer be sure
    // that collected metadata is valid, e.g. if getter in prototype chain added
    // property with the same name into the object itself.
    if (&shape == &base_obj->shape()) {
        auto get_cache_slot = [&] -> PropertyLookupCache::Entry& {
            for (size_t i = cache.entries.size() - 1; i >= 1; --i) {
                cache.entries[i] = cache.entries[i - 1];
            }
            cache.entries[0] = {};
            return cache.entries[0];
        };
        if (cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
            auto& entry = get_cache_slot();
            entry.shape = shape;
            entry.property_offset = cacheable_metadata.property_offset.value();
        } else if (cacheable_metadata.type == CacheablePropertyMetadata::Type::InPrototypeChain) {
            auto& entry = get_cache_slot();
            entry.shape = &base_obj->shape();
            entry.property_offset = cacheable_metadata.property_offset.value();
            entry.prototype = *cacheable_metadata.prototype;
            entry.prototype_chain_validity = *cacheable_metadata.prototype->shape().prototype_chain_validity();
        }
    }

    return value;
}

inline ThrowCompletionOr<Value> get_by_value(VM& vm, Optional<IdentifierTableIndex> base_identifier, Value base_value, Value property_key_value, Executable const& executable)
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
                case TypedArrayBase::Kind::Float16Array:
                    return fast_typed_array_get_element<f16>(typed_array, index);
                case TypedArrayBase::Kind::Float32Array:
                    return fast_typed_array_get_element<float>(typed_array, index);
                case TypedArrayBase::Kind::Float64Array:
                    return fast_typed_array_get_element<double>(typed_array, index);
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

    auto object = TRY(base_object_for_get(vm, base_value, base_identifier, property_key_value, executable));

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

    auto& shape = binding_object.shape();
    if (cache.environment_serial_number == declarative_record.environment_serial_number()) {

        // OPTIMIZATION: For global var bindings, if the shape of the global object hasn't changed,
        //               we can use the cached property offset.
        if (&shape == cache.entries[0].shape) {
            auto value = binding_object.get_direct(cache.entries[0].property_offset.value());
            if (value.is_accessor())
                return TRY(call(vm, value.as_accessor().getter(), js_undefined()));
            return value;
        }

        // OPTIMIZATION: For global lexical bindings, if the global declarative environment hasn't changed,
        //               we can use the cached environment binding index.
        if (cache.has_environment_binding_index) {
            if (cache.in_module_environment) {
                auto module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
                return (*module)->environment()->get_binding_value_direct(vm, cache.environment_binding_index);
            }
            return declarative_record.get_binding_value_direct(vm, cache.environment_binding_index);
        }
    }

    cache.environment_serial_number = declarative_record.environment_serial_number();

    auto& identifier = interpreter.current_executable().get_identifier(identifier_index);

    if (auto* module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>()) {
        // NOTE: GetGlobal is used to access variables stored in the module environment and global environment.
        //       The module environment is checked first since it precedes the global environment in the environment chain.
        auto& module_environment = *(*module)->environment();
        Optional<size_t> index;
        if (TRY(module_environment.has_binding(identifier, &index))) {
            if (index.has_value()) {
                cache.environment_binding_index = static_cast<u32>(index.value());
                cache.has_environment_binding_index = true;
                cache.in_module_environment = true;
                return TRY(module_environment.get_binding_value_direct(vm, index.value()));
            }
            return TRY(module_environment.get_binding_value(vm, identifier, vm.in_strict_mode()));
        }
    }

    Optional<size_t> offset;
    if (TRY(declarative_record.has_binding(identifier, &offset))) {
        cache.environment_binding_index = static_cast<u32>(offset.value());
        cache.has_environment_binding_index = true;
        cache.in_module_environment = false;
        return TRY(declarative_record.get_binding_value(vm, identifier, vm.in_strict_mode()));
    }

    if (TRY(binding_object.has_property(identifier))) {
        CacheablePropertyMetadata cacheable_metadata;
        auto value = TRY(binding_object.internal_get(identifier, js_undefined(), &cacheable_metadata));
        if (cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
            cache.entries[0].shape = shape;
            cache.entries[0].property_offset = cacheable_metadata.property_offset.value();
        }
        return value;
    }

    return vm.throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, identifier);
}

inline ThrowCompletionOr<void> put_by_property_key(VM& vm, Value base, Value this_value, Value value, Optional<FlyString const&> const& base_identifier, PropertyKey name, Op::PropertyKind kind, PropertyLookupCache* caches = nullptr)
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
        if (is<ECMAScriptFunctionObject>(function) && static_cast<ECMAScriptFunctionObject const&>(function).name().is_empty())
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(MUST(String::formatted("get {}", name)));
        object->define_direct_accessor(name, &function, nullptr, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case Op::PropertyKind::Setter: {
        auto& function = value.as_function();
        if (is<ECMAScriptFunctionObject>(function) && static_cast<ECMAScriptFunctionObject const&>(function).name().is_empty())
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(MUST(String::formatted("set {}", name)));
        object->define_direct_accessor(name, nullptr, &function, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case Op::PropertyKind::KeyValue: {
        auto& shape = object->shape();
        if (caches) {
            for (auto& cache : caches->entries) {
                if (cache.prototype) {
                    // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
                    bool can_use_cache = [&]() -> bool {
                        if (&object->shape() != cache.shape)
                            return false;
                        if (!cache.prototype_chain_validity)
                            return false;
                        if (!cache.prototype_chain_validity->is_valid())
                            return false;
                        return true;
                    }();
                    if (can_use_cache) {
                        auto value_in_prototype = cache.prototype->get_direct(cache.property_offset.value());
                        if (value_in_prototype.is_accessor()) {
                            TRY(call(vm, value_in_prototype.as_accessor().setter(), this_value, value));
                            return {};
                        }
                    }
                } else if (cache.shape == &object->shape()) {
                    auto value_in_object = object->get_direct(cache.property_offset.value());
                    if (value_in_object.is_accessor()) {
                        TRY(call(vm, value_in_object.as_accessor().setter(), this_value, value));
                    } else {
                        object->put_direct(*cache.property_offset, value);
                    }
                    return {};
                }
            }
        }

        CacheablePropertyMetadata cacheable_metadata;
        bool succeeded = TRY(object->internal_set(name, value, this_value, &cacheable_metadata));

        // If internal_set() caused object's shape change, we can no longer be sure
        // that collected metadata is valid, e.g. if setter in prototype chain added
        // property with the same name into the object itself.
        if (succeeded && caches && &shape == &object->shape()) {
            auto get_cache_slot = [&] -> PropertyLookupCache::Entry& {
                for (size_t i = caches->entries.size() - 1; i >= 1; --i) {
                    caches->entries[i] = caches->entries[i - 1];
                }
                caches->entries[0] = {};
                return caches->entries[0];
            };
            auto& cache = get_cache_slot();
            if (cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
                cache.shape = object->shape();
                cache.property_offset = cacheable_metadata.property_offset.value();
            } else if (cacheable_metadata.type == CacheablePropertyMetadata::Type::InPrototypeChain) {
                cache.shape = object->shape();
                cache.property_offset = cacheable_metadata.property_offset.value();
                cache.prototype = *cacheable_metadata.prototype;
                cache.prototype_chain_validity = *cacheable_metadata.prototype->shape().prototype_chain_validity();
            }
        }

        if (!succeeded && vm.in_strict_mode()) [[unlikely]] {
            if (base.is_object())
                return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base.to_string_without_side_effects());
            return vm.throw_completion<TypeError>(ErrorType::ReferencePrimitiveSetProperty, name, base.typeof_(vm)->utf8_string(), base.to_string_without_side_effects());
        }
        break;
    }
    case Op::PropertyKind::DirectKeyValue:
        object->define_direct_property(name, value, Attribute::Enumerable | Attribute::Writable | Attribute::Configurable);
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
            return_value = TRY(perform_eval(vm, !argument_values.is_empty() ? argument_values[0] : js_undefined(), vm.in_strict_mode() ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
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
        return vm.throw_completion<TypeError>(ErrorType::IsNotAEvaluatedFrom, callee.to_string_without_side_effects(), callee_type, interpreter.current_executable().get_string(*expression_string));

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
        FlyString name;
        if (lhs_name.has_value())
            name = vm.bytecode_interpreter().current_executable().get_identifier(lhs_name.value());
        value = function_node.instantiate_ordinary_function_expression(vm, name);
    } else {
        value = ECMAScriptFunctionObject::create_from_function_node(
            function_node,
            function_node.name(),
            *vm.current_realm(),
            vm.lexical_environment(),
            vm.running_execution_context().private_environment);
    }

    if (home_object.has_value()) {
        auto home_object_value = vm.bytecode_interpreter().get(home_object.value());
        static_cast<ECMAScriptFunctionObject&>(value.as_function()).set_home_object(&home_object_value.as_object());
    }

    return value;
}

inline ThrowCompletionOr<void> put_by_value(VM& vm, Value base, Optional<FlyString const&> const& base_identifier, Value property_key_value, Value value, Op::PropertyKind kind)
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

            if (is_valid_integer_index(typed_array, canonical_index)) {
                if (value.is_int32()) {
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
                        break;
                    }
                } else if (value.is_double()) {
                    switch (typed_array.kind()) {
                    case TypedArrayBase::Kind::Float16Array:
                        fast_typed_array_set_element<f16>(typed_array, index, static_cast<f16>(value.as_double()));
                        return {};
                    case TypedArrayBase::Kind::Float32Array:
                        fast_typed_array_set_element<float>(typed_array, index, static_cast<float>(value.as_double()));
                        return {};
                    case TypedArrayBase::Kind::Float64Array:
                        fast_typed_array_set_element<double>(typed_array, index, value.as_double());
                        return {};
                    case TypedArrayBase::Kind::Int8Array:
                        fast_typed_array_set_element<i8>(typed_array, index, MUST(value.to_i8(vm)));
                        return {};
                    case TypedArrayBase::Kind::Int16Array:
                        fast_typed_array_set_element<i16>(typed_array, index, MUST(value.to_i16(vm)));
                        return {};
                    case TypedArrayBase::Kind::Int32Array:
                        fast_typed_array_set_element<i32>(typed_array, index, MUST(value.to_i32(vm)));
                        return {};
                    case TypedArrayBase::Kind::Uint8Array:
                        fast_typed_array_set_element<u8>(typed_array, index, MUST(value.to_u8(vm)));
                        return {};
                    case TypedArrayBase::Kind::Uint16Array:
                        fast_typed_array_set_element<u16>(typed_array, index, MUST(value.to_u16(vm)));
                        return {};
                    case TypedArrayBase::Kind::Uint32Array:
                        fast_typed_array_set_element<u32>(typed_array, index, MUST(value.to_u32(vm)));
                        return {};
                    default:
                        break;
                    }
                }
                // FIXME: Support more TypedArray kinds.
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

    auto property_key = TRY(property_key_value.to_property_key(vm));
    TRY(put_by_property_key(vm, base, base, value, base_identifier, property_key, kind));
    return {};
}

struct CalleeAndThis {
    Value callee;
    Value this_value;
};

inline ThrowCompletionOr<CalleeAndThis> get_callee_and_this_from_environment(Bytecode::Interpreter& interpreter, FlyString const& name, EnvironmentCoordinate& cache)
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
inline Value new_regexp(VM& vm, ParsedRegex const& parsed_regex, String const& pattern, String const& flags)
{
    // 1. Let pattern be CodePointsToString(BodyText of RegularExpressionLiteral).
    // 2. Let flags be CodePointsToString(FlagText of RegularExpressionLiteral).

    // 3. Return ! RegExpCreate(pattern, flags).
    auto& realm = *vm.current_realm();
    Regex<ECMA262> regex(parsed_regex.regex, parsed_regex.pattern.to_byte_string(), parsed_regex.flags);
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
inline Span<Value> argument_list_evaluation(Interpreter& interpreter, Value arguments)
{
    // Note: Any spreading and actual evaluation is handled in preceding opcodes
    // Note: The spec uses the concept of a list, while we create a temporary array
    //       in the preceding opcodes, so we have to convert in a manner that is not
    //       visible to the user

    auto& argument_array = arguments.as_array();
    auto array_length = argument_array.indexed_properties().array_like_size();

    auto argument_values = interpreter.allocate_argument_values(array_length);

    for (size_t i = 0; i < array_length; ++i) {
        if (auto maybe_value = argument_array.indexed_properties().get(i); maybe_value.has_value())
            argument_values[i] = maybe_value.release_value().value;
        else
            argument_values[i] = js_undefined();
    }

    return argument_values;
}

inline ThrowCompletionOr<void> create_variable(VM& vm, FlyString const& name, Op::EnvironmentMode mode, bool is_global, bool is_immutable, bool is_strict)
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
    return as<GlobalEnvironment>(vm.variable_environment())->create_global_var_binding(name, false);
}

inline ThrowCompletionOr<ECMAScriptFunctionObject*> new_class(VM& vm, Value super_class, ClassExpression const& class_expression, Optional<IdentifierTableIndex> const& lhs_name, ReadonlySpan<Value> element_keys)
{
    auto& interpreter = vm.bytecode_interpreter();
    auto name = class_expression.name();

    // NOTE: NewClass expects classEnv to be active lexical environment
    auto* class_environment = vm.lexical_environment();
    vm.running_execution_context().lexical_environment = vm.running_execution_context().saved_lexical_environments.take_last();

    Optional<FlyString> binding_name;
    FlyString class_name;
    if (!class_expression.has_name() && lhs_name.has_value()) {
        class_name = interpreter.current_executable().get_identifier(lhs_name.value());
    } else {
        binding_name = name;
        class_name = name;
    }

    return TRY(class_expression.create_class_constructor(vm, class_environment, vm.lexical_environment(), super_class, element_keys, binding_name, class_name));
}

// 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
inline ThrowCompletionOr<GC::Ref<Object>> super_call_with_argument_array(Interpreter& interpreter, Value argument_array, bool is_synthetic)
{
    auto& vm = interpreter.vm();

    // 1. Let newTarget be GetNewTarget().
    auto new_target = vm.get_new_target();

    // 2. Assert: Type(newTarget) is Object.
    VERIFY(new_target.is_object());

    // 3. Let func be GetSuperConstructor().
    auto* func = get_super_constructor(vm);

    // 4. Let argList be ? ArgumentListEvaluation of Arguments.
    Span<Value> arg_list;
    if (is_synthetic) {
        VERIFY(argument_array.is_object() && is<Array>(argument_array.as_object()));
        auto const& array_value = static_cast<Array const&>(argument_array.as_object());
        auto length = MUST(length_of_array_like(vm, array_value));
        arg_list = interpreter.allocate_argument_values(length);
        for (size_t i = 0; i < length; ++i)
            arg_list[i] = array_value.get_without_side_effects(PropertyKey { i });
    } else {
        arg_list = argument_list_evaluation(interpreter, argument_array);
    }

    // 5. If IsConstructor(func) is false, throw a TypeError exception.
    if (!Value(func).is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, "Super constructor");

    // 6. Let result be ? Construct(func, argList, newTarget).
    auto result = TRY(construct(vm, static_cast<FunctionObject&>(*func), arg_list, &new_target.as_function()));

    // 7. Let thisER be GetThisEnvironment().
    auto& this_environment = as<FunctionEnvironment>(*get_this_environment(vm));

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

inline ThrowCompletionOr<GC::Ref<Array>> iterator_to_array(VM& vm, Value iterator)
{
    auto& iterator_record = static_cast<IteratorRecord&>(iterator.as_cell());

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

class JS_API PropertyNameIterator final
    : public Object
    , public BuiltinIterator {
    JS_OBJECT(PropertyNameIterator, Object);
    GC_DECLARE_ALLOCATOR(PropertyNameIterator);

public:
    virtual ~PropertyNameIterator() override = default;

    BuiltinIterator* as_builtin_iterator_if_next_is_not_redefined(IteratorRecord const&) override { return this; }
    ThrowCompletionOr<void> next(VM&, bool& done, Value& value) override
    {
        while (true) {
            if (m_iterator == m_properties.end()) {
                done = true;
                return {};
            }

            auto const& entry = *m_iterator;
            ScopeGuard remove_first = [&] { ++m_iterator; };

            // If the property is deleted, don't include it (invariant no. 2)
            if (!TRY(m_object->has_property(entry.key.key)))
                continue;

            done = false;
            value = entry.value;
            return {};
        }
    }

private:
    PropertyNameIterator(JS::Realm& realm, GC::Ref<Object> object, OrderedHashMap<PropertyKeyAndEnumerableFlag, Value> properties)
        : Object(realm, nullptr)
        , m_object(object)
        , m_properties(move(properties))
        , m_iterator(m_properties.begin())
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_object);
        visitor.visit(m_properties);
    }

    GC::Ref<Object> m_object;
    OrderedHashMap<PropertyKeyAndEnumerableFlag, Value> m_properties;
    decltype(m_properties.begin()) m_iterator;
};

GC_DEFINE_ALLOCATOR(PropertyNameIterator);

// 14.7.5.9 EnumerateObjectProperties ( O ), https://tc39.es/ecma262/#sec-enumerate-object-properties
inline ThrowCompletionOr<Value> get_object_property_iterator(Interpreter& interpreter, Value value)
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

    auto& vm = interpreter.vm();

    // Invariant 3 effectively allows the implementation to ignore newly added keys, and we do so (similar to other implementations).
    auto object = TRY(value.to_object(vm));
    // Note: While the spec doesn't explicitly require these to be ordered, it says that the values should be retrieved via OwnPropertyKeys,
    //       so we just keep the order consistent anyway.

    GC::OrderedRootHashMap<PropertyKeyAndEnumerableFlag, Value> properties(vm.heap());
    HashTable<GC::Ref<Object>> seen_objects;
    // Collect all keys immediately (invariant no. 5)
    for (auto object_to_check = GC::Ptr { object.ptr() }; object_to_check && !seen_objects.contains(*object_to_check); object_to_check = TRY(object_to_check->internal_get_prototype_of())) {
        seen_objects.set(*object_to_check);
        auto keys = TRY(object_to_check->internal_own_property_keys());
        properties.ensure_capacity(properties.size() + keys.size());
        for (auto& key : keys) {
            if (key.is_symbol())
                continue;

            // NOTE: If there is a non-enumerable property higher up the prototype chain with the same key,
            //       we mustn't include this property even if it's enumerable (invariant no. 5 and 6)
            //       This is achieved with the PropertyKeyAndEnumerableFlag struct, which doesn't consider
            //       the enumerable flag when comparing keys.
            PropertyKeyAndEnumerableFlag new_entry {
                .key = TRY(PropertyKey::from_value(vm, key)),
                .enumerable = false,
            };

            if (properties.contains(new_entry))
                continue;

            auto descriptor = TRY(object_to_check->internal_get_own_property(new_entry.key));
            if (!descriptor.has_value())
                continue;

            new_entry.enumerable = *descriptor->enumerable;
            properties.set(move(new_entry), key, AK::HashSetExistingEntryBehavior::Keep);
        }
    }

    properties.remove_all_matching([&](auto& key, auto&) { return !key.enumerable; });

    auto iterator = interpreter.realm().create<PropertyNameIterator>(interpreter.realm(), object, move(properties));

    return vm.heap().allocate<IteratorRecord>(iterator, js_undefined(), false);
}

}

namespace JS::Bytecode::Op {

static void dump_object(Object& o, HashTable<Object const*>& seen, int indent = 0)
{
    if (seen.contains(&o))
        return;
    seen.set(&o);
    for (auto& it : o.shape().property_table()) {
        auto value = o.get_direct(it.value.offset);
        dbgln("{}  {} -> {}", String::repeated(' ', indent).release_value(), it.key.to_string(), value);
        if (value.is_object()) {
            dump_object(value.as_object(), seen, indent + 2);
        }
    }
}

void Dump::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto value = interpreter.get(m_value);
    dbgln("(DUMP) {}: {}", m_text, value);
    if (value.is_object()) {
        HashTable<Object const*> seen;
        dump_object(value.as_object(), seen);
    }
}

#define JS_DEFINE_EXECUTE_FOR_COMMON_BINARY_OP(OpTitleCase, op_snake_case)                      \
    ThrowCompletionOr<void> OpTitleCase::execute_impl(Bytecode::Interpreter& interpreter) const \
    {                                                                                           \
        auto& vm = interpreter.vm();                                                            \
        auto lhs = interpreter.get(m_lhs);                                                      \
        auto rhs = interpreter.get(m_rhs);                                                      \
        interpreter.set(m_dst, Value { TRY(op_snake_case(vm, lhs, rhs)) });                     \
        return {};                                                                              \
    }

JS_ENUMERATE_COMMON_BINARY_OPS_WITHOUT_FAST_PATH(JS_DEFINE_EXECUTE_FOR_COMMON_BINARY_OP)

ThrowCompletionOr<void> Add::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);

    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            if (!Checked<i32>::addition_would_overflow(lhs.as_i32(), rhs.as_i32())) {
                interpreter.set(m_dst, Value(lhs.as_i32() + rhs.as_i32()));
                return {};
            }
        }
        interpreter.set(m_dst, Value(lhs.as_double() + rhs.as_double()));
        return {};
    }

    interpreter.set(m_dst, TRY(add(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> Mul::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);

    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            if (!Checked<i32>::multiplication_would_overflow(lhs.as_i32(), rhs.as_i32())) {
                interpreter.set(m_dst, Value(lhs.as_i32() * rhs.as_i32()));
                return {};
            }
        }
        interpreter.set(m_dst, Value(lhs.as_double() * rhs.as_double()));
        return {};
    }

    interpreter.set(m_dst, TRY(mul(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> Sub::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);

    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            if (!Checked<i32>::subtraction_would_overflow(lhs.as_i32(), rhs.as_i32())) {
                interpreter.set(m_dst, Value(lhs.as_i32() - rhs.as_i32()));
                return {};
            }
        }
        interpreter.set(m_dst, Value(lhs.as_double() - rhs.as_double()));
        return {};
    }

    interpreter.set(m_dst, TRY(sub(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> BitwiseXor::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        interpreter.set(m_dst, Value(lhs.as_i32() ^ rhs.as_i32()));
        return {};
    }
    interpreter.set(m_dst, TRY(bitwise_xor(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> BitwiseAnd::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        interpreter.set(m_dst, Value(lhs.as_i32() & rhs.as_i32()));
        return {};
    }
    interpreter.set(m_dst, TRY(bitwise_and(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> BitwiseOr::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        interpreter.set(m_dst, Value(lhs.as_i32() | rhs.as_i32()));
        return {};
    }
    interpreter.set(m_dst, TRY(bitwise_or(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> UnsignedRightShift::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        auto const shift_count = static_cast<u32>(rhs.as_i32()) % 32;
        interpreter.set(m_dst, Value(static_cast<u32>(lhs.as_i32()) >> shift_count));
        return {};
    }
    interpreter.set(m_dst, TRY(unsigned_right_shift(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> RightShift::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        auto const shift_count = static_cast<u32>(rhs.as_i32()) % 32;
        interpreter.set(m_dst, Value(lhs.as_i32() >> shift_count));
        return {};
    }
    interpreter.set(m_dst, TRY(right_shift(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> LeftShift::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        auto const shift_count = static_cast<u32>(rhs.as_i32()) % 32;
        interpreter.set(m_dst, Value(lhs.as_i32() << shift_count));
        return {};
    }
    interpreter.set(m_dst, TRY(left_shift(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> LessThan::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            interpreter.set(m_dst, Value(lhs.as_i32() < rhs.as_i32()));
            return {};
        }
        interpreter.set(m_dst, Value(lhs.as_double() < rhs.as_double()));
        return {};
    }
    interpreter.set(m_dst, Value { TRY(less_than(vm, lhs, rhs)) });
    return {};
}

ThrowCompletionOr<void> LessThanEquals::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            interpreter.set(m_dst, Value(lhs.as_i32() <= rhs.as_i32()));
            return {};
        }
        interpreter.set(m_dst, Value(lhs.as_double() <= rhs.as_double()));
        return {};
    }
    interpreter.set(m_dst, Value { TRY(less_than_equals(vm, lhs, rhs)) });
    return {};
}

ThrowCompletionOr<void> GreaterThan::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            interpreter.set(m_dst, Value(lhs.as_i32() > rhs.as_i32()));
            return {};
        }
        interpreter.set(m_dst, Value(lhs.as_double() > rhs.as_double()));
        return {};
    }
    interpreter.set(m_dst, Value { TRY(greater_than(vm, lhs, rhs)) });
    return {};
}

ThrowCompletionOr<void> GreaterThanEquals::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            interpreter.set(m_dst, Value(lhs.as_i32() >= rhs.as_i32()));
            return {};
        }
        interpreter.set(m_dst, Value(lhs.as_double() >= rhs.as_double()));
        return {};
    }
    interpreter.set(m_dst, Value { TRY(greater_than_equals(vm, lhs, rhs)) });
    return {};
}

static ThrowCompletionOr<Value> not_(VM&, Value value)
{
    return Value(!value.to_boolean());
}

static ThrowCompletionOr<Value> typeof_(VM& vm, Value value)
{
    return value.typeof_(vm);
}

#define JS_DEFINE_COMMON_UNARY_OP(OpTitleCase, op_snake_case)                                   \
    ThrowCompletionOr<void> OpTitleCase::execute_impl(Bytecode::Interpreter& interpreter) const \
    {                                                                                           \
        auto& vm = interpreter.vm();                                                            \
        interpreter.set(dst(), TRY(op_snake_case(vm, interpreter.get(src()))));                 \
        return {};                                                                              \
    }

JS_ENUMERATE_COMMON_UNARY_OPS(JS_DEFINE_COMMON_UNARY_OP)

void NewArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto array = MUST(Array::create(interpreter.realm(), 0));
    for (size_t i = 0; i < m_element_count; i++) {
        array->indexed_properties().put(i, interpreter.get(m_elements[i]), default_attributes);
    }
    interpreter.set(dst(), array);
}

void NewPrimitiveArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto array = MUST(Array::create(interpreter.realm(), 0));
    for (size_t i = 0; i < m_element_count; i++)
        array->indexed_properties().put(i, m_elements[i], default_attributes);
    interpreter.set(dst(), array);
}

void AddPrivateName::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& name = interpreter.current_executable().get_identifier(m_name);
    interpreter.vm().running_execution_context().private_environment->add_private_name(name);
}

ThrowCompletionOr<void> ArrayAppend::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return append(interpreter.vm(), interpreter.get(dst()), interpreter.get(src()), m_is_spread);
}

ThrowCompletionOr<void> ImportCall::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto specifier = interpreter.get(m_specifier);
    auto options_value = interpreter.get(m_options);
    interpreter.set(dst(), TRY(perform_import_call(vm, specifier, options_value)));
    return {};
}

ThrowCompletionOr<void> IteratorToArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), TRY(iterator_to_array(interpreter.vm(), interpreter.get(iterator()))));
    return {};
}

void NewObject::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& realm = *vm.current_realm();
    interpreter.set(dst(), Object::create(realm, realm.intrinsics().object_prototype()));
}

void NewRegExp::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(),
        new_regexp(
            interpreter.vm(),
            interpreter.current_executable().regex_table->get(m_regex_index),
            interpreter.current_executable().get_string(m_source_index),
            interpreter.current_executable().get_string(m_flags_index)));
}

#define JS_DEFINE_NEW_BUILTIN_ERROR_OP(ErrorName)                                                                      \
    void New##ErrorName::execute_impl(Bytecode::Interpreter& interpreter) const                                        \
    {                                                                                                                  \
        auto& vm = interpreter.vm();                                                                                   \
        auto& realm = *vm.current_realm();                                                                             \
        interpreter.set(dst(), ErrorName::create(realm, interpreter.current_executable().get_string(m_error_string))); \
    }

JS_ENUMERATE_NEW_BUILTIN_ERROR_OPS(JS_DEFINE_NEW_BUILTIN_ERROR_OP)

ThrowCompletionOr<void> CopyObjectExcludingProperties::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& realm = *vm.current_realm();

    auto from_object = interpreter.get(m_from_object);

    auto to_object = Object::create(realm, realm.intrinsics().object_prototype());

    HashTable<PropertyKey> excluded_names;
    for (size_t i = 0; i < m_excluded_names_count; ++i) {
        excluded_names.set(TRY(interpreter.get(m_excluded_names[i]).to_property_key(vm)));
    }

    TRY(to_object->copy_data_properties(vm, from_object, excluded_names));

    interpreter.set(dst(), to_object);
    return {};
}

ThrowCompletionOr<void> ConcatString::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto string = TRY(interpreter.get(src()).to_primitive_string(vm));
    interpreter.set(dst(), PrimitiveString::create(vm, interpreter.get(dst()).as_string(), string));
    return {};
}

enum class BindingIsKnownToBeInitialized {
    No,
    Yes,
};

template<BindingIsKnownToBeInitialized binding_is_known_to_be_initialized>
static ThrowCompletionOr<void> get_binding(Interpreter& interpreter, Operand dst, IdentifierTableIndex identifier, EnvironmentCoordinate& cache)
{
    auto& vm = interpreter.vm();
    auto& executable = interpreter.current_executable();

    if (cache.is_valid()) {
        auto const* environment = interpreter.running_execution_context().lexical_environment.ptr();
        for (size_t i = 0; i < cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) {
            Value value;
            if constexpr (binding_is_known_to_be_initialized == BindingIsKnownToBeInitialized::No) {
                value = TRY(static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(vm, cache.index));
            } else {
                value = static_cast<DeclarativeEnvironment const&>(*environment).get_initialized_binding_value_direct(cache.index);
            }
            interpreter.set(dst, value);
            return {};
        }
        cache = {};
    }

    auto reference = TRY(vm.resolve_binding(executable.get_identifier(identifier)));
    if (reference.environment_coordinate().has_value())
        cache = reference.environment_coordinate().value();
    interpreter.set(dst, TRY(reference.get_value(vm)));
    return {};
}

ThrowCompletionOr<void> GetBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return get_binding<BindingIsKnownToBeInitialized::No>(interpreter, m_dst, m_identifier, m_cache);
}

ThrowCompletionOr<void> GetInitializedBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return get_binding<BindingIsKnownToBeInitialized::Yes>(interpreter, m_dst, m_identifier, m_cache);
}

ThrowCompletionOr<void> GetCalleeAndThisFromEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee_and_this = TRY(get_callee_and_this_from_environment(
        interpreter,
        interpreter.current_executable().get_identifier(m_identifier),
        m_cache));
    interpreter.set(m_callee, callee_and_this.callee);
    interpreter.set(m_this_value, callee_and_this.this_value);
    return {};
}

ThrowCompletionOr<void> GetGlobal::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), TRY(get_global(interpreter, m_identifier, interpreter.current_executable().global_variable_caches[m_cache_index])));
    return {};
}

ThrowCompletionOr<void> SetGlobal::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& binding_object = interpreter.global_object();
    auto& declarative_record = interpreter.global_declarative_environment();

    auto& cache = interpreter.current_executable().global_variable_caches[m_cache_index];
    auto& shape = binding_object.shape();
    auto src = interpreter.get(m_src);

    if (cache.environment_serial_number == declarative_record.environment_serial_number()) {
        // OPTIMIZATION: For global var bindings, if the shape of the global object hasn't changed,
        //               we can use the cached property offset.
        if (&shape == cache.entries[0].shape) {
            auto value = binding_object.get_direct(cache.entries[0].property_offset.value());
            if (value.is_accessor())
                TRY(call(vm, value.as_accessor().setter(), &binding_object, src));
            else
                binding_object.put_direct(cache.entries[0].property_offset.value(), src);
            return {};
        }

        // OPTIMIZATION: For global lexical bindings, if the global declarative environment hasn't changed,
        //               we can use the cached environment binding index.
        if (cache.has_environment_binding_index) {
            if (cache.in_module_environment) {
                auto module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
                TRY((*module)->environment()->set_mutable_binding_direct(vm, cache.environment_binding_index, src, vm.in_strict_mode()));
            } else {
                TRY(declarative_record.set_mutable_binding_direct(vm, cache.environment_binding_index, src, vm.in_strict_mode()));
            }
            return {};
        }
    }

    cache.environment_serial_number = declarative_record.environment_serial_number();

    auto& identifier = interpreter.current_executable().get_identifier(m_identifier);

    if (auto* module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>()) {
        // NOTE: GetGlobal is used to access variables stored in the module environment and global environment.
        //       The module environment is checked first since it precedes the global environment in the environment chain.
        auto& module_environment = *(*module)->environment();
        Optional<size_t> index;
        if (TRY(module_environment.has_binding(identifier, &index))) {
            if (index.has_value()) {
                cache.environment_binding_index = static_cast<u32>(index.value());
                cache.has_environment_binding_index = true;
                cache.in_module_environment = true;
                return TRY(module_environment.set_mutable_binding_direct(vm, index.value(), src, vm.in_strict_mode()));
            }
            return TRY(module_environment.set_mutable_binding(vm, identifier, src, vm.in_strict_mode()));
        }
    }

    Optional<size_t> offset;
    if (TRY(declarative_record.has_binding(identifier, &offset))) {
        cache.environment_binding_index = static_cast<u32>(offset.value());
        cache.has_environment_binding_index = true;
        cache.in_module_environment = false;
        TRY(declarative_record.set_mutable_binding(vm, identifier, src, vm.in_strict_mode()));
        return {};
    }

    if (TRY(binding_object.has_property(identifier))) {
        CacheablePropertyMetadata cacheable_metadata;
        auto success = TRY(binding_object.internal_set(identifier, src, &binding_object, &cacheable_metadata));
        if (!success && vm.in_strict_mode()) {
            // Note: Nothing like this in the spec, this is here to produce nicer errors instead of the generic one thrown by Object::set().

            auto property_or_error = binding_object.internal_get_own_property(identifier);
            if (!property_or_error.is_error()) {
                auto property = property_or_error.release_value();
                if (property.has_value() && !property->writable.value_or(true)) {
                    return vm.throw_completion<TypeError>(ErrorType::DescWriteNonWritable, identifier);
                }
            }
            return vm.throw_completion<TypeError>(ErrorType::ObjectSetReturnedFalse);
        }
        if (cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
            cache.entries[0].shape = shape;
            cache.entries[0].property_offset = cacheable_metadata.property_offset.value();
        }
        return {};
    }

    auto reference = TRY(vm.resolve_binding(identifier, &declarative_record));
    TRY(reference.put_value(vm, src));

    return {};
}

ThrowCompletionOr<void> DeleteVariable::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const& string = interpreter.current_executable().get_identifier(m_identifier);
    auto reference = TRY(vm.resolve_binding(string));
    interpreter.set(dst(), Value(TRY(reference.delete_(vm))));
    return {};
}

void CreateLexicalEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto make_and_swap_envs = [&](auto& old_environment) {
        auto declarative_environment = new_declarative_environment(*old_environment).ptr();
        declarative_environment->ensure_capacity(m_capacity);
        GC::Ptr<Environment> environment = declarative_environment;
        swap(old_environment, environment);
        return environment;
    };
    auto& running_execution_context = interpreter.running_execution_context();
    running_execution_context.saved_lexical_environments.append(make_and_swap_envs(running_execution_context.lexical_environment));
}

void CreatePrivateEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& running_execution_context = interpreter.vm().running_execution_context();
    auto outer_private_environment = running_execution_context.private_environment;
    running_execution_context.private_environment = new_private_environment(interpreter.vm(), outer_private_environment);
}

void CreateVariableEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& running_execution_context = interpreter.running_execution_context();
    auto var_environment = new_declarative_environment(*running_execution_context.lexical_environment);
    var_environment->ensure_capacity(m_capacity);
    running_execution_context.variable_environment = var_environment;
    running_execution_context.lexical_environment = var_environment;
}

ThrowCompletionOr<void> EnterObjectEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto object = TRY(interpreter.get(m_object).to_object(interpreter.vm()));
    interpreter.enter_object_environment(*object);
    return {};
}

void Catch::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.catch_exception(dst());
}

void LeaveFinally::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.leave_finally();
}

void RestoreScheduledJump::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.restore_scheduled_jump();
}

ThrowCompletionOr<void> CreateVariable::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& name = interpreter.current_executable().get_identifier(m_identifier);
    return create_variable(interpreter.vm(), name, m_mode, m_is_global, m_is_immutable, m_is_strict);
}

void CreateRestParams::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const arguments = interpreter.running_execution_context().arguments;
    auto arguments_count = interpreter.running_execution_context().passed_argument_count;
    auto array = MUST(Array::create(interpreter.realm(), 0));
    for (size_t rest_index = m_rest_index; rest_index < arguments_count; ++rest_index)
        array->indexed_properties().append(arguments[rest_index]);
    interpreter.set(m_dst, array);
}

void CreateArguments::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& function = interpreter.running_execution_context().function;
    auto const arguments = interpreter.running_execution_context().arguments;
    auto const& environment = interpreter.running_execution_context().lexical_environment;

    auto passed_arguments = ReadonlySpan<Value> { arguments.data(), interpreter.running_execution_context().passed_argument_count };
    Object* arguments_object;
    if (m_kind == Kind::Mapped) {
        arguments_object = create_mapped_arguments_object(interpreter.vm(), *function, function->formal_parameters(), passed_arguments, *environment);
    } else {
        arguments_object = create_unmapped_arguments_object(interpreter.vm(), passed_arguments);
    }

    if (m_dst.has_value()) {
        interpreter.set(*m_dst, arguments_object);
        return;
    }

    if (m_is_immutable) {
        MUST(environment->create_immutable_binding(interpreter.vm(), interpreter.vm().names.arguments.as_string(), false));
    } else {
        MUST(environment->create_mutable_binding(interpreter.vm(), interpreter.vm().names.arguments.as_string(), false));
    }
    MUST(environment->initialize_binding(interpreter.vm(), interpreter.vm().names.arguments.as_string(), arguments_object, Environment::InitializeBindingHint::Normal));
}

template<EnvironmentMode environment_mode, BindingInitializationMode initialization_mode>
static ThrowCompletionOr<void> initialize_or_set_binding(Interpreter& interpreter, IdentifierTableIndex identifier_index, Value value, EnvironmentCoordinate& cache)
{
    auto& vm = interpreter.vm();

    auto* environment = environment_mode == EnvironmentMode::Lexical
        ? interpreter.running_execution_context().lexical_environment.ptr()
        : interpreter.running_execution_context().variable_environment.ptr();

    if (cache.is_valid()) {
        for (size_t i = 0; i < cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) {
            if constexpr (initialization_mode == BindingInitializationMode::Initialize) {
                TRY(static_cast<DeclarativeEnvironment&>(*environment).initialize_binding_direct(vm, cache.index, value, Environment::InitializeBindingHint::Normal));
            } else {
                TRY(static_cast<DeclarativeEnvironment&>(*environment).set_mutable_binding_direct(vm, cache.index, value, vm.in_strict_mode()));
            }
            return {};
        }
        cache = {};
    }

    auto reference = TRY(vm.resolve_binding(interpreter.current_executable().get_identifier(identifier_index), environment));
    if (reference.environment_coordinate().has_value())
        cache = reference.environment_coordinate().value();
    if constexpr (initialization_mode == BindingInitializationMode::Initialize) {
        TRY(reference.initialize_referenced_binding(vm, value));
    } else if (initialization_mode == BindingInitializationMode::Set) {
        TRY(reference.put_value(vm, value));
    }
    return {};
}

ThrowCompletionOr<void> InitializeLexicalBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Lexical, BindingInitializationMode::Initialize>(interpreter, m_identifier, interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> InitializeVariableBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Var, BindingInitializationMode::Initialize>(interpreter, m_identifier, interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> SetLexicalBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Lexical, BindingInitializationMode::Set>(interpreter, m_identifier, interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> SetVariableBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Var, BindingInitializationMode::Set>(interpreter, m_identifier, interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> GetById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(base());
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];

    interpreter.set(dst(), TRY(get_by_id(interpreter.vm(), m_base_identifier, m_property, base_value, base_value, cache, interpreter.current_executable())));
    return {};
}

ThrowCompletionOr<void> GetByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];
    interpreter.set(dst(), TRY(get_by_id(interpreter.vm(), {}, m_property, base_value, this_value, cache, interpreter.current_executable())));
    return {};
}

ThrowCompletionOr<void> GetLength::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(base());
    auto& executable = interpreter.current_executable();
    auto& cache = executable.property_lookup_caches[m_cache_index];

    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Length>(interpreter.vm(), m_base_identifier, *executable.length_identifier, base_value, base_value, cache, executable)));
    return {};
}

ThrowCompletionOr<void> GetLengthWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    auto& executable = interpreter.current_executable();
    auto& cache = executable.property_lookup_caches[m_cache_index];
    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Length>(interpreter.vm(), {}, *executable.length_identifier, base_value, this_value, cache, executable)));
    return {};
}

ThrowCompletionOr<void> GetPrivateById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const& name = interpreter.current_executable().get_identifier(m_property);
    auto base_value = interpreter.get(m_base);
    auto private_reference = make_private_reference(vm, base_value, name);
    interpreter.set(dst(), TRY(private_reference.get_value(vm)));
    return {};
}

ThrowCompletionOr<void> HasPrivateId::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();

    auto base = interpreter.get(m_base);
    if (!base.is_object())
        return vm.throw_completion<TypeError>(ErrorType::InOperatorWithObject);

    auto private_environment = interpreter.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(interpreter.current_executable().get_identifier(m_property));
    interpreter.set(dst(), Value(base.as_object().private_element_find(private_name) != nullptr));
    return {};
}

ThrowCompletionOr<void> PutBySpread::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base = interpreter.get(m_base);

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto object = TRY(base.to_object(vm));

    TRY(object->copy_data_properties(vm, value, {}));

    return {};
}

ThrowCompletionOr<void> PutById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base = interpreter.get(m_base);
    auto base_identifier = interpreter.current_executable().get_identifier(m_base_identifier);
    PropertyKey name = interpreter.current_executable().get_identifier(m_property);
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];
    TRY(put_by_property_key(vm, base, base, value, base_identifier, name, m_kind, &cache));
    return {};
}

ThrowCompletionOr<void> PutByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base = interpreter.get(m_base);
    PropertyKey name = interpreter.current_executable().get_identifier(m_property);
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];
    TRY(put_by_property_key(vm, base, interpreter.get(m_this_value), value, {}, name, m_kind, &cache));
    return {};
}

ThrowCompletionOr<void> PutPrivateById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto object = TRY(interpreter.get(m_base).to_object(vm));
    auto name = interpreter.current_executable().get_identifier(m_property);
    auto private_reference = make_private_reference(vm, object, name);
    TRY(private_reference.put_value(vm, value));
    return {};
}

ThrowCompletionOr<void> DeleteById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    interpreter.set(dst(), TRY(Bytecode::delete_by_id(interpreter, base_value, m_property)));
    return {};
}

ThrowCompletionOr<void> DeleteByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto base_value = interpreter.get(m_base);
    auto const& identifier = interpreter.current_executable().get_identifier(m_property);
    bool strict = vm.in_strict_mode();
    auto reference = Reference { base_value, identifier, interpreter.get(m_this_value), strict };
    interpreter.set(dst(), Value(TRY(reference.delete_(vm))));
    return {};
}

ThrowCompletionOr<void> ResolveThisBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& cached_this_value = interpreter.reg(Register::this_value());
    if (!cached_this_value.is_special_empty_value())
        return {};
    // OPTIMIZATION: Because the value of 'this' cannot be reassigned during a function execution, it's
    //               resolved once and then saved for subsequent use.
    auto& running_execution_context = interpreter.running_execution_context();
    if (auto function = running_execution_context.function; function && is<ECMAScriptFunctionObject>(*function) && !static_cast<ECMAScriptFunctionObject&>(*function).allocates_function_environment()) {
        cached_this_value = running_execution_context.this_value.value();
    } else {
        auto& vm = interpreter.vm();
        cached_this_value = TRY(vm.resolve_this_binding());
    }
    return {};
}

// https://tc39.es/ecma262/#sec-makesuperpropertyreference
ThrowCompletionOr<void> ResolveSuperBase::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();

    // 1. Let env be GetThisEnvironment().
    auto& env = as<FunctionEnvironment>(*get_this_environment(vm));

    // 2. Assert: env.HasSuperBinding() is true.
    VERIFY(env.has_super_binding());

    // 3. Let baseValue be ? env.GetSuperBase().
    interpreter.set(dst(), TRY(env.get_super_base()));

    return {};
}

void GetNewTarget::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), interpreter.vm().get_new_target());
}

void GetImportMeta::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), interpreter.vm().get_import_meta());
}

static ThrowCompletionOr<Value> dispatch_builtin_call(Bytecode::Interpreter& interpreter, Bytecode::Builtin builtin, ReadonlySpan<Operand> arguments)
{
    switch (builtin) {
    case Builtin::MathAbs:
        return TRY(MathObject::abs_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathLog:
        return TRY(MathObject::log_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathPow:
        return TRY(MathObject::pow_impl(interpreter.vm(), interpreter.get(arguments[0]), interpreter.get(arguments[1])));
    case Builtin::MathExp:
        return TRY(MathObject::exp_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathCeil:
        return TRY(MathObject::ceil_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathFloor:
        return TRY(MathObject::floor_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathImul:
        return TRY(MathObject::imul_impl(interpreter.vm(), interpreter.get(arguments[0]), interpreter.get(arguments[1])));
    case Builtin::MathRandom:
        return MathObject::random_impl();
    case Builtin::MathRound:
        return TRY(MathObject::round_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathSqrt:
        return TRY(MathObject::sqrt_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathSin:
        return TRY(MathObject::sin_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathCos:
        return TRY(MathObject::cos_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathTan:
        return TRY(MathObject::tan_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::ArrayIteratorPrototypeNext:
    case Builtin::MapIteratorPrototypeNext:
    case Builtin::SetIteratorPrototypeNext:
    case Builtin::StringIteratorPrototypeNext:
        VERIFY_NOT_REACHED();
    case Bytecode::Builtin::__Count:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

ThrowCompletionOr<void> Call::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee = interpreter.get(m_callee);

    if (!callee.is_function()) [[unlikely]] {
        return throw_type_error_for_callee(interpreter, callee, "function"sv, m_expression_string);
    }

    auto& function = callee.as_function();

    ExecutionContext* callee_context = nullptr;
    size_t registers_and_constants_and_locals_count = 0;
    size_t argument_count = m_argument_count;
    TRY(function.get_stack_frame_size(registers_and_constants_and_locals_count, argument_count));
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK_WITHOUT_CLEARING_ARGS(callee_context, registers_and_constants_and_locals_count, max(m_argument_count, argument_count));

    auto* callee_context_argument_values = callee_context->arguments.data();
    auto const callee_context_argument_count = callee_context->arguments.size();
    auto const insn_argument_count = m_argument_count;

    for (size_t i = 0; i < insn_argument_count; ++i)
        callee_context_argument_values[i] = interpreter.get(m_arguments[i]);
    for (size_t i = insn_argument_count; i < callee_context_argument_count; ++i)
        callee_context_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    auto retval = TRY(function.internal_call(*callee_context, interpreter.get(m_this_value)));
    interpreter.set(m_dst, retval);
    return {};
}

ThrowCompletionOr<void> CallConstruct::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee = interpreter.get(m_callee);

    TRY(throw_if_needed_for_call(interpreter, callee, CallType::Construct, expression_string()));

    auto argument_values = interpreter.allocate_argument_values(m_argument_count);
    for (size_t i = 0; i < m_argument_count; ++i)
        argument_values[i] = interpreter.get(m_arguments[i]);
    interpreter.set(dst(), TRY(perform_call(interpreter, Value(), CallType::Construct, callee, argument_values)));
    return {};
}

ThrowCompletionOr<void> CallDirectEval::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee = interpreter.get(m_callee);

    TRY(throw_if_needed_for_call(interpreter, callee, CallType::DirectEval, expression_string()));

    auto argument_values = interpreter.allocate_argument_values(m_argument_count);
    for (size_t i = 0; i < m_argument_count; ++i)
        argument_values[i] = interpreter.get(m_arguments[i]);
    interpreter.set(dst(), TRY(perform_call(interpreter, interpreter.get(m_this_value), CallType::DirectEval, callee, argument_values)));
    return {};
}

ThrowCompletionOr<void> CallBuiltin::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee = interpreter.get(m_callee);

    TRY(throw_if_needed_for_call(interpreter, callee, CallType::Call, expression_string()));

    if (m_argument_count == Bytecode::builtin_argument_count(m_builtin) && callee.is_object() && interpreter.realm().get_builtin_value(m_builtin) == &callee.as_object()) {
        interpreter.set(dst(), TRY(dispatch_builtin_call(interpreter, m_builtin, { m_arguments, m_argument_count })));

        return {};
    }

    auto argument_values = interpreter.allocate_argument_values(m_argument_count);
    for (size_t i = 0; i < m_argument_count; ++i)
        argument_values[i] = interpreter.get(m_arguments[i]);
    interpreter.set(dst(), TRY(perform_call(interpreter, interpreter.get(m_this_value), CallType::Call, callee, argument_values)));
    return {};
}

ThrowCompletionOr<void> CallWithArgumentArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee = interpreter.get(m_callee);
    TRY(throw_if_needed_for_call(interpreter, callee, call_type(), expression_string()));
    auto argument_values = argument_list_evaluation(interpreter, interpreter.get(arguments()));
    interpreter.set(dst(), TRY(perform_call(interpreter, interpreter.get(m_this_value), call_type(), callee, move(argument_values))));
    return {};
}

// 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
ThrowCompletionOr<void> SuperCallWithArgumentArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), TRY(super_call_with_argument_array(interpreter, interpreter.get(arguments()), m_is_synthetic)));
    return {};
}

void NewFunction::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    interpreter.set(dst(), new_function(vm, m_function_node, m_lhs_name, m_home_object));
}

ThrowCompletionOr<void> Increment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_value = interpreter.get(dst());

    // OPTIMIZATION: Fast path for Int32 values.
    if (old_value.is_int32()) {
        auto integer_value = old_value.as_i32();
        if (integer_value != NumericLimits<i32>::max()) [[likely]] {
            interpreter.set(dst(), Value { integer_value + 1 });
            return {};
        }
    }

    old_value = TRY(old_value.to_numeric(vm));

    if (old_value.is_number())
        interpreter.set(dst(), Value(old_value.as_double() + 1));
    else
        interpreter.set(dst(), BigInt::create(vm, old_value.as_bigint().big_integer().plus(Crypto::SignedBigInteger { 1 })));
    return {};
}

ThrowCompletionOr<void> PostfixIncrement::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_value = interpreter.get(m_src);

    // OPTIMIZATION: Fast path for Int32 values.
    if (old_value.is_int32()) {
        auto integer_value = old_value.as_i32();
        if (integer_value != NumericLimits<i32>::max()) [[likely]] {
            interpreter.set(m_dst, old_value);
            interpreter.set(m_src, Value { integer_value + 1 });
            return {};
        }
    }

    old_value = TRY(old_value.to_numeric(vm));
    interpreter.set(m_dst, old_value);

    if (old_value.is_number())
        interpreter.set(m_src, Value(old_value.as_double() + 1));
    else
        interpreter.set(m_src, BigInt::create(vm, old_value.as_bigint().big_integer().plus(Crypto::SignedBigInteger { 1 })));
    return {};
}

ThrowCompletionOr<void> Decrement::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_value = interpreter.get(dst());

    old_value = TRY(old_value.to_numeric(vm));

    if (old_value.is_number())
        interpreter.set(dst(), Value(old_value.as_double() - 1));
    else
        interpreter.set(dst(), BigInt::create(vm, old_value.as_bigint().big_integer().minus(Crypto::SignedBigInteger { 1 })));
    return {};
}

ThrowCompletionOr<void> PostfixDecrement::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_value = interpreter.get(m_src);

    old_value = TRY(old_value.to_numeric(vm));
    interpreter.set(m_dst, old_value);

    if (old_value.is_number())
        interpreter.set(m_src, Value(old_value.as_double() - 1));
    else
        interpreter.set(m_src, BigInt::create(vm, old_value.as_bigint().big_integer().minus(Crypto::SignedBigInteger { 1 })));
    return {};
}

ThrowCompletionOr<void> Throw::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return throw_completion(interpreter.get(src()));
}

ThrowCompletionOr<void> ThrowIfNotObject::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto src = interpreter.get(m_src);
    if (!src.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, src.to_string_without_side_effects());
    return {};
}

ThrowCompletionOr<void> ThrowIfNullish::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    if (value.is_nullish())
        return vm.throw_completion<TypeError>(ErrorType::NotObjectCoercible, value.to_string_without_side_effects());
    return {};
}

ThrowCompletionOr<void> ThrowIfTDZ::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    if (value.is_special_empty_value())
        return vm.throw_completion<ReferenceError>(ErrorType::BindingNotInitialized, value.to_string_without_side_effects());
    return {};
}

void LeaveLexicalEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& running_execution_context = interpreter.running_execution_context();
    running_execution_context.lexical_environment = running_execution_context.saved_lexical_environments.take_last();
}

void LeavePrivateEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& running_execution_context = interpreter.vm().running_execution_context();
    running_execution_context.private_environment = running_execution_context.private_environment->outer_environment();
}

void LeaveUnwindContext::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.leave_unwind_context();
}

ThrowCompletionOr<void> GetByValue::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), TRY(get_by_value(interpreter.vm(), m_base_identifier, interpreter.get(m_base), interpreter.get(m_property), interpreter.current_executable())));
    return {};
}

ThrowCompletionOr<void> GetByValueWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto property_key_value = interpreter.get(m_property);
    auto object = TRY(interpreter.get(m_base).to_object(vm));
    auto property_key = TRY(property_key_value.to_property_key(vm));
    interpreter.set(dst(), TRY(object->internal_get(property_key, interpreter.get(m_this_value))));
    return {};
}

ThrowCompletionOr<void> PutByValue::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base_identifier = interpreter.current_executable().get_identifier(m_base_identifier);
    TRY(put_by_value(vm, interpreter.get(m_base), base_identifier, interpreter.get(m_property), value, m_kind));
    return {};
}

ThrowCompletionOr<void> PutByValueWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base = interpreter.get(m_base);
    auto property_key = TRY(interpreter.get(m_property).to_property_key(vm));
    TRY(put_by_property_key(vm, base, interpreter.get(m_this_value), value, {}, property_key, m_kind));
    return {};
}

ThrowCompletionOr<void> DeleteByValue::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto property_key_value = interpreter.get(m_property);
    interpreter.set(dst(), TRY(delete_by_value(interpreter, base_value, property_key_value)));

    return {};
}

ThrowCompletionOr<void> DeleteByValueWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto property_key_value = interpreter.get(m_property);
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    interpreter.set(dst(), TRY(delete_by_value_with_this(interpreter, base_value, property_key_value, this_value)));

    return {};
}

ThrowCompletionOr<void> GetIterator::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    interpreter.set(dst(), TRY(get_iterator(vm, interpreter.get(iterable()), m_hint)));
    return {};
}

void GetObjectFromIteratorRecord::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& iterator_record = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());
    interpreter.set(m_object, iterator_record.iterator);
}

void GetNextMethodFromIteratorRecord::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& iterator_record = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());
    interpreter.set(m_next_method, iterator_record.next_method);
}

ThrowCompletionOr<void> GetMethod::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto identifier = interpreter.current_executable().get_identifier(m_property);
    auto method = TRY(interpreter.get(m_object).get_method(vm, identifier));
    interpreter.set(dst(), method ?: js_undefined());
    return {};
}

ThrowCompletionOr<void> GetObjectPropertyIterator::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto iterator_record = TRY(get_object_property_iterator(interpreter, interpreter.get(object())));
    interpreter.set(dst(), iterator_record);
    return {};
}

ThrowCompletionOr<void> IteratorClose::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());

    // FIXME: Return the value of the resulting completion. (Note that m_completion_value can be empty!)
    TRY(iterator_close(vm, iterator, Completion { m_completion_type, m_completion_value.value_or(js_undefined()) }));
    return {};
}

ThrowCompletionOr<void> AsyncIteratorClose::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());

    // FIXME: Return the value of the resulting completion. (Note that m_completion_value can be empty!)
    TRY(async_iterator_close(vm, iterator, Completion { m_completion_type, m_completion_value.value_or(js_undefined()) }));
    return {};
}

ThrowCompletionOr<void> IteratorNext::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator_record = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());
    interpreter.set(dst(), TRY(iterator_next(vm, iterator_record)));
    return {};
}

ThrowCompletionOr<void> IteratorNextUnpack::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator_record = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());
    auto iteration_result_or_done = TRY(iterator_step(vm, iterator_record));
    if (iteration_result_or_done.has<IterationDone>()) {
        interpreter.set(dst_done(), Value(true));
        return {};
    }
    auto& iteration_result = iteration_result_or_done.get<IterationResult>();
    interpreter.set(dst_done(), TRY(iteration_result.done));
    interpreter.set(dst_value(), TRY(iteration_result.value));
    return {};
}

ThrowCompletionOr<void> NewClass::execute_impl(Bytecode::Interpreter& interpreter) const
{
    Value super_class;
    if (m_super_class.has_value())
        super_class = interpreter.get(m_super_class.value());
    Vector<Value> element_keys;
    for (size_t i = 0; i < m_element_keys_count; ++i) {
        Value element_key;
        if (m_element_keys[i].has_value())
            element_key = interpreter.get(m_element_keys[i].value());
        element_keys.append(element_key);
    }
    interpreter.set(dst(), TRY(new_class(interpreter.vm(), super_class, m_class_expression, m_lhs_name, element_keys)));
    return {};
}

// 13.5.3.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-typeof-operator-runtime-semantics-evaluation
ThrowCompletionOr<void> TypeofBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();

    if (m_cache.is_valid()) {
        auto const* environment = interpreter.running_execution_context().lexical_environment.ptr();
        for (size_t i = 0; i < m_cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) {
            auto value = TRY(static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(vm, m_cache.index));
            interpreter.set(dst(), value.typeof_(vm));
            return {};
        }
        m_cache = {};
    }

    // 1. Let val be the result of evaluating UnaryExpression.
    auto reference = TRY(vm.resolve_binding(interpreter.current_executable().get_identifier(m_identifier)));

    // 2. If val is a Reference Record, then
    //    a. If IsUnresolvableReference(val) is true, return "undefined".
    if (reference.is_unresolvable()) {
        interpreter.set(dst(), PrimitiveString::create(vm, "undefined"_string));
        return {};
    }

    // 3. Set val to ? GetValue(val).
    auto value = TRY(reference.get_value(vm));

    if (reference.environment_coordinate().has_value())
        m_cache = reference.environment_coordinate().value();

    // 4. NOTE: This step is replaced in section B.3.6.3.
    // 5. Return a String according to Table 41.
    interpreter.set(dst(), value.typeof_(vm));
    return {};
}

void BlockDeclarationInstantiation::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_environment = interpreter.running_execution_context().lexical_environment;
    auto& running_execution_context = interpreter.running_execution_context();
    running_execution_context.saved_lexical_environments.append(old_environment);
    running_execution_context.lexical_environment = new_declarative_environment(*old_environment);
    m_scope_node.block_declaration_instantiation(vm, running_execution_context.lexical_environment);
}

void GetCompletionFields::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& completion_cell = static_cast<CompletionCell const&>(interpreter.get(m_completion).as_cell());
    interpreter.set(m_value_dst, completion_cell.completion().value());
    interpreter.set(m_type_dst, Value(to_underlying(completion_cell.completion().type())));
}

void SetCompletionType::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& completion_cell = static_cast<CompletionCell&>(interpreter.get(m_completion).as_cell());
    auto completion = completion_cell.completion();
    completion_cell.set_completion(Completion { m_type, completion.value() });
}

}
