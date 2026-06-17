/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/Types.h>
#include <LibJS/Bytecode/Builtins.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/PropertyAccess.h>
#include <LibJS/Bytecode/PropertyNameIterator.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/AsyncFromSyncIteratorPrototype.h>
#include <LibJS/Runtime/AsyncGenerator.h>
#include <LibJS/Runtime/ClassConstruction.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GeneratorObject.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/MathObject.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/StringConstructor.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>

// ===== Slow path functions callable from assembly =====
// All slow path functions follow the same convention:
//   i64 func(VM* vm, u32 pc, Op::Foo const* instruction)
//   Returns >= 0: new program counter to dispatch to
//   Returns < 0: should exit the asm interpreter

using namespace JS;
using namespace JS::Bytecode;

static i64 handle_asm_exception(VM& vm, u32 pc, Value exception)
{
    auto response = vm.handle_exception(pc, exception);
    if (response == VM::HandleExceptionResponse::ExitFromExecutable)
        return -1;
    // ContinueInThisExecutable: new pc is in the execution context
    return static_cast<i64>(vm.running_execution_context().program_counter);
}

#define ASM_TRY(vm, pc, expression)                                                                      \
    ({                                                                                                   \
        auto& asm_try_vm = (vm);                                                                         \
        auto asm_try_pc = (pc);                                                                          \
        auto&& asm_try_result = (expression);                                                            \
        if (asm_try_result.is_error()) [[unlikely]]                                                      \
            return handle_asm_exception(asm_try_vm, asm_try_pc, asm_try_result.release_error().value()); \
        asm_try_result.release_value();                                                                  \
    })

template<typename Op>
static i64 advance_or_continue(u32 pc, i64 next_pc)
{
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op));
}

template<typename EnvironmentPointer>
static EnvironmentPointer asm_get_cacheable_environment(EnvironmentPointer environment, EnvironmentCoordinate const& cache)
{
    VERIFY(cache.is_valid());

    for (size_t i = 0; i < cache.hops; ++i) {
        if (!environment->is_declarative_environment() || environment->is_permanently_screwed_by_eval()) [[unlikely]]
            return nullptr;
        environment = environment->outer_environment();
        if (!environment) [[unlikely]]
            return nullptr;
    }
    if (environment->is_declarative_environment() && !environment->is_permanently_screwed_by_eval()) [[likely]]
        return environment;
    return nullptr;
}

template<typename EnvironmentPointer>
static EnvironmentPointer asm_get_cached_environment(EnvironmentPointer environment, EnvironmentCoordinate& cache)
{
    if (!cache.is_valid()) [[unlikely]]
        return nullptr;

    if (auto* cached_environment = asm_get_cacheable_environment(environment, cache)) [[likely]]
        return cached_environment;

    cache = {};
    return nullptr;
}

template<typename EnvironmentPointer>
static void asm_update_environment_coordinate_cache(EnvironmentPointer environment, Reference const& reference, EnvironmentCoordinate& cache)
{
    if (!reference.environment_coordinate().has_value())
        return;
    auto candidate = reference.environment_coordinate().value();
    if (asm_get_cacheable_environment(environment, candidate))
        cache = candidate;
}

enum class AsmBindingIsKnownToBeInitialized {
    No,
    Yes,
};

template<AsmBindingIsKnownToBeInitialized binding_is_known_to_be_initialized>
static i64 asm_get_binding(VM& vm, u32 pc, Operand dst, EnvironmentCoordinate const& cache)
{
    VERIFY(cache.is_valid());

    auto const* environment = vm.running_execution_context().lexical_environment.ptr();
    for (size_t i = 0; i < cache.hops; ++i)
        environment = environment->outer_environment();

    Value value;
    if constexpr (binding_is_known_to_be_initialized == AsmBindingIsKnownToBeInitialized::No) {
        value = ASM_TRY(vm, pc, static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(vm, cache.index));
    } else {
        value = static_cast<DeclarativeEnvironment const&>(*environment).get_initialized_binding_value_direct(cache.index);
    }
    vm.set(dst, value);
    return static_cast<i64>(pc);
}

template<AsmBindingIsKnownToBeInitialized binding_is_known_to_be_initialized>
static i64 asm_dynamic_get_binding(VM& vm, u32 pc, Operand dst, IdentifierTableIndex identifier_index, Strict strict, EnvironmentCoordinate& cache)
{
    auto const* current_environment = vm.running_execution_context().lexical_environment.ptr();
    if (auto const* cached_environment = asm_get_cached_environment(current_environment, cache)) [[likely]] {
        Value value;
        if constexpr (binding_is_known_to_be_initialized == AsmBindingIsKnownToBeInitialized::No) {
            value = ASM_TRY(vm, pc, static_cast<DeclarativeEnvironment const&>(*cached_environment).get_binding_value_direct(vm, cache.index));
        } else {
            value = static_cast<DeclarativeEnvironment const&>(*cached_environment).get_initialized_binding_value_direct(cache.index);
        }
        vm.set(dst, value);
        return static_cast<i64>(pc);
    }

    auto& executable = vm.current_executable();
    auto reference = ASM_TRY(vm, pc, vm.resolve_binding(executable.get_identifier(identifier_index), strict));
    asm_update_environment_coordinate_cache(current_environment, reference, cache);

    vm.set(dst, ASM_TRY(vm, pc, reference.get_value(vm)));
    return static_cast<i64>(pc);
}

static i64 asm_dynamic_get_callee_and_this_from_environment(VM& vm, u32 pc, Operand callee_dst, Operand this_value_dst, IdentifierTableIndex identifier_index, Strict strict, EnvironmentCoordinate& cache)
{
    auto const* current_environment = vm.running_execution_context().lexical_environment.ptr();
    if (auto const* cached_environment = asm_get_cached_environment(current_environment, cache)) [[likely]] {
        auto callee = ASM_TRY(vm, pc, static_cast<DeclarativeEnvironment const&>(*cached_environment).get_binding_value_direct(vm, cache.index));
        vm.set(callee_dst, callee);
        vm.set(this_value_dst, js_undefined());
        return static_cast<i64>(pc);
    }

    auto reference = ASM_TRY(vm, pc, vm.resolve_binding(vm.get_identifier(identifier_index), strict));
    asm_update_environment_coordinate_cache(current_environment, reference, cache);

    auto callee = ASM_TRY(vm, pc, reference.get_value(vm));

    Value this_value;
    if (reference.is_property_reference()) {
        this_value = reference.get_this_value();
    } else {
        if (reference.is_environment_reference()) {
            if (auto base_object = reference.base_environment().with_base_object()) [[unlikely]]
                this_value = base_object;
        }
    }

    vm.set(callee_dst, callee);
    vm.set(this_value_dst, this_value);
    return static_cast<i64>(pc);
}

template<Op::EnvironmentMode environment_mode, Op::BindingInitializationMode initialization_mode>
static i64 asm_initialize_or_set_binding(VM& vm, u32 pc, Strict strict, Value value, EnvironmentCoordinate const& cache)
{
    VERIFY(cache.is_valid());

    auto* environment = environment_mode == Op::EnvironmentMode::Lexical
        ? vm.running_execution_context().lexical_environment.ptr()
        : vm.running_execution_context().variable_environment.ptr();

    for (size_t i = 0; i < cache.hops; ++i)
        environment = environment->outer_environment();

    if constexpr (initialization_mode == Op::BindingInitializationMode::Initialize) {
        ASM_TRY(vm, pc, static_cast<DeclarativeEnvironment&>(*environment).initialize_binding_direct(vm, cache.index, value, Environment::InitializeBindingHint::Normal));
    } else {
        ASM_TRY(vm, pc, static_cast<DeclarativeEnvironment&>(*environment).set_mutable_binding_direct(vm, cache.index, value, strict == Strict::Yes));
    }
    return static_cast<i64>(pc);
}

template<Op::EnvironmentMode environment_mode, Op::BindingInitializationMode initialization_mode>
static i64 asm_dynamic_initialize_or_set_binding(VM& vm, u32 pc, IdentifierTableIndex identifier_index, Strict strict, Value value, EnvironmentCoordinate& cache)
{
    auto* environment = environment_mode == Op::EnvironmentMode::Lexical
        ? vm.running_execution_context().lexical_environment.ptr()
        : vm.running_execution_context().variable_environment.ptr();

    if (auto* cached_environment = asm_get_cached_environment(environment, cache)) [[likely]] {
        if constexpr (initialization_mode == Op::BindingInitializationMode::Initialize) {
            ASM_TRY(vm, pc, static_cast<DeclarativeEnvironment&>(*cached_environment).initialize_binding_direct(vm, cache.index, value, Environment::InitializeBindingHint::Normal));
        } else if (initialization_mode == Op::BindingInitializationMode::Set) {
            ASM_TRY(vm, pc, static_cast<DeclarativeEnvironment&>(*cached_environment).set_mutable_binding_direct(vm, cache.index, value, strict == Strict::Yes));
        }
        return static_cast<i64>(pc);
    }

    auto reference = ASM_TRY(vm, pc, vm.resolve_binding(vm.get_identifier(identifier_index), strict, environment));
    asm_update_environment_coordinate_cache(environment, reference, cache);
    if constexpr (initialization_mode == Op::BindingInitializationMode::Initialize) {
        ASM_TRY(vm, pc, reference.initialize_referenced_binding(vm, value));
    } else if (initialization_mode == Op::BindingInitializationMode::Set) {
        ASM_TRY(vm, pc, reference.put_value(vm, value));
    }
    return static_cast<i64>(pc);
}

static ThrowCompletionOr<void> asm_create_variable(VM& vm, Utf16FlyString const& name, Op::EnvironmentMode mode, bool is_global, bool is_immutable, bool is_strict)
{
    if (mode == Op::EnvironmentMode::Lexical) {
        VERIFY(!is_global);

        // Note: This is papering over an issue where "FunctionDeclarationInstantiation" creates these bindings for us.
        //       Instead of crashing in there, we'll just raise an exception here.
        if (TRY(vm.lexical_environment()->has_binding(name))) [[unlikely]]
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

struct FastPropertyNameIteratorData {
    Vector<PropertyKey> properties;
    PropertyNameIterator::FastPath fast_path { PropertyNameIterator::FastPath::None };
    u32 indexed_property_count { 0 };
    bool receiver_has_magical_length_property { false };
    GC::Ptr<Shape> shape;
    GC::Ptr<PrototypeChainValidity> prototype_chain_validity;
};

static bool shape_has_enumerable_string_property(Shape const& shape)
{
    bool has_enumerable_string_property = false;
    shape.for_each_property_in_insertion_order([&](auto const& property_key, auto const& metadata) {
        if (property_key.is_string() && metadata.attributes.is_enumerable()) {
            has_enumerable_string_property = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return has_enumerable_string_property;
}

static bool property_name_iterator_fast_path_is_still_eligible(Object& object, PropertyNameIterator::FastPath fast_path, u32 indexed_property_count)
{
    Object const* object_to_check = &object;
    bool is_receiver = true;

    while (object_to_check) {
        if (!object_to_check->eligible_for_own_property_enumeration_fast_path())
            return false;

        if (is_receiver) {
            if (fast_path == PropertyNameIterator::FastPath::PackedIndexed) {
                if (object_to_check->indexed_storage_kind() != IndexedStorageKind::Packed)
                    return false;
                if (object_to_check->indexed_array_like_size() != indexed_property_count)
                    return false;
            } else if (object_to_check->indexed_array_like_size() != 0) {
                return false;
            }
        } else if (object_to_check->indexed_array_like_size() != 0) {
            return false;
        }

        object_to_check = object_to_check->prototype();
        is_receiver = false;
    }

    return true;
}

static bool object_property_iterator_cache_matches(Object& object, ObjectPropertyIteratorCacheData const& cache)
{
    // A cache entry represents the fully flattened key snapshot for one bytecode
    // site. Reusing it is only valid while the receiver still has the same local
    // state and the prototype chain validity token says nothing above it changed.
    if (object.has_magical_length_property() != cache.receiver_has_magical_length_property())
        return false;

    auto& shape = object.shape();
    if (&shape != cache.shape())
        return false;

    if (shape.is_dictionary() && shape.dictionary_generation() != cache.shape_dictionary_generation())
        return false;

    if (cache.prototype_chain_validity() && !cache.prototype_chain_validity()->is_valid())
        return false;

    return property_name_iterator_fast_path_is_still_eligible(object, cache.fast_path(), cache.indexed_property_count());
}

static ThrowCompletionOr<Optional<FastPropertyNameIteratorData>> asm_try_get_fast_property_name_iterator_data(Object& object)
{
    auto& vm = object.vm();
    FastPropertyNameIteratorData result {};
    result.fast_path = PropertyNameIterator::FastPath::PlainNamed;
    result.receiver_has_magical_length_property = object.has_magical_length_property();
    result.shape = &object.shape();

    GC::RootHashTable<GC::Ref<Object>> seen_objects;
    size_t estimated_properties_count = 0;
    bool prototype_chain_has_enumerable_named_properties = false;
    for (auto object_to_check = GC::Ptr { &object }; object_to_check && !seen_objects.contains(*object_to_check); object_to_check = TRY(object_to_check->internal_get_prototype_of())) {
        seen_objects.set(*object_to_check);
        if (!object_to_check->eligible_for_own_property_enumeration_fast_path())
            return Optional<FastPropertyNameIteratorData> {};
        if (&object == object_to_check.ptr()) {
            if (object_to_check->indexed_array_like_size() != 0) {
                if (object_to_check->indexed_storage_kind() != IndexedStorageKind::Packed)
                    return Optional<FastPropertyNameIteratorData> {};
                result.fast_path = PropertyNameIterator::FastPath::PackedIndexed;
                result.indexed_property_count = object_to_check->indexed_array_like_size();
            } else {
                result.fast_path = PropertyNameIterator::FastPath::PlainNamed;
            }
        } else if (object_to_check->indexed_array_like_size() != 0) {
            // The fast path only knows how to synthesize a packed indexed prefix
            // for the receiver itself. As soon as indexed properties appear in
            // the prototype chain, we fall back to the generic enumeration path.
            return Optional<FastPropertyNameIteratorData> {};
        } else if (!prototype_chain_has_enumerable_named_properties) {
            prototype_chain_has_enumerable_named_properties = shape_has_enumerable_string_property(object_to_check->shape());
        }
        estimated_properties_count += object_to_check->shape().property_count();
    }
    seen_objects.clear_with_capacity();

    if (auto* prototype = object.shape().prototype()) {
        result.prototype_chain_validity = prototype->shape().prototype_chain_validity();
        if (!result.prototype_chain_validity)
            return Optional<FastPropertyNameIteratorData> {};
    }

    if (!prototype_chain_has_enumerable_named_properties) {
        // Common case: only the receiver contributes enumerable string keys, so
        // we can copy them straight from the shape without any shadowing work.
        result.properties.ensure_capacity(object.shape().property_count());
        object.shape().for_each_property_in_insertion_order([&](auto const& property_key, auto const& metadata) {
            if (property_key.is_string() && metadata.attributes.is_enumerable())
                result.properties.append(property_key);
        });
        return result;
    }

    result.properties.ensure_capacity(estimated_properties_count);

    GC::ConservativeHashTable<PropertyKey> seen_non_enumerable_properties;
    Optional<GC::ConservativeHashTable<PropertyKey>> seen_properties;
    auto ensure_seen_properties = [&] {
        if (seen_properties.has_value())
            return;
        // Prototype shadowing ignores enumerability, so once we start looking
        // above the receiver we need an explicit visited set for names we have
        // already decided to expose from lower objects.
        seen_properties.emplace();
        seen_properties->ensure_capacity(result.properties.size());
        for (auto const& property : result.properties)
            seen_properties->set(property);
    };

    bool in_prototype_chain = false;
    for (auto object_to_check = GC::Ptr { &object }; object_to_check && !seen_objects.contains(*object_to_check); object_to_check = TRY(object_to_check->internal_get_prototype_of())) {
        seen_objects.set(*object_to_check);

        // Arrays keep a non-enumerable magical `length` property outside the shape
        // table, but it still shadows enumerable `length` properties higher up the
        // prototype chain during for-in.
        if (object_to_check->has_magical_length_property())
            seen_non_enumerable_properties.set(vm.names.length);

        object_to_check->shape().for_each_property_in_insertion_order([&](auto const& property_key, auto const& metadata) {
            if (!property_key.is_string())
                return;

            bool enumerable = metadata.attributes.is_enumerable();
            if (!enumerable)
                seen_non_enumerable_properties.set(property_key);
            if (in_prototype_chain && enumerable) {
                if (seen_non_enumerable_properties.contains(property_key))
                    return;
                ensure_seen_properties();
                if (seen_properties->contains(property_key))
                    return;
            }
            if (enumerable)
                result.properties.append(property_key);
            if (seen_properties.has_value())
                seen_properties->set(property_key);
        });
        in_prototype_chain = true;
    }

    return result;
}

// 14.7.5.9 EnumerateObjectProperties ( O ), https://tc39.es/ecma262/#sec-enumerate-object-properties
static ThrowCompletionOr<GC::Ref<PropertyNameIterator>> asm_get_object_property_iterator(VM& vm, Value value, ObjectPropertyIteratorCache* cache = nullptr)
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

    if (cache && cache->data) {
        if (object_property_iterator_cache_matches(*object, *cache->data)) {
            if (cache->reusable_property_name_iterator) {
                // We keep one iterator object per bytecode site alive so hot
                // loops can recycle it without allocating a new cell each time.
                auto& iterator = static_cast<PropertyNameIterator&>(*cache->reusable_property_name_iterator);
                cache->reusable_property_name_iterator = nullptr;
                iterator.reset_with_cache_data(object, *cache->data, cache);
                return iterator;
            }

            return PropertyNameIterator::create(vm.realm(), object, *cache->data, cache);
        }
    }

    if (auto fast_iterator_data = TRY(asm_try_get_fast_property_name_iterator_data(*object)); fast_iterator_data.has_value()) {
        VERIFY(fast_iterator_data->shape);
        auto cache_data = vm.heap().allocate<ObjectPropertyIteratorCacheData>(
            vm,
            move(fast_iterator_data->properties),
            fast_iterator_data->fast_path,
            fast_iterator_data->indexed_property_count,
            fast_iterator_data->receiver_has_magical_length_property,
            *fast_iterator_data->shape,
            fast_iterator_data->prototype_chain_validity);
        if (cache)
            cache->data = cache_data;
        if (cache && cache->reusable_property_name_iterator) {
            auto& iterator = static_cast<PropertyNameIterator&>(*cache->reusable_property_name_iterator);
            cache->reusable_property_name_iterator = nullptr;
            iterator.reset_with_cache_data(object, cache_data, cache);
            return iterator;
        }

        return PropertyNameIterator::create(vm.realm(), object, cache_data, cache);
    }

    size_t estimated_properties_count = 0;
    GC::RootHashTable<GC::Ref<Object>> seen_objects;
    for (auto object_to_check = GC::Ptr { object.ptr() }; object_to_check && !seen_objects.contains(*object_to_check); object_to_check = TRY(object_to_check->internal_get_prototype_of())) {
        seen_objects.set(*object_to_check);
        estimated_properties_count += object_to_check->own_properties_count();
    }
    seen_objects.clear_with_capacity();

    GC::ConservativeVector<PropertyKey> properties;
    properties.ensure_capacity(estimated_properties_count);

    GC::ConservativeHashTable<PropertyKey> seen_non_enumerable_properties;
    Optional<GC::ConservativeHashTable<PropertyKey>> seen_properties;
    auto ensure_seen_properties = [&] {
        if (seen_properties.has_value())
            return;
        seen_properties.emplace();
        seen_properties->ensure_capacity(properties.size());
        for (auto const& property : properties)
            seen_properties->set(property);
    };

    // Collect all keys immediately (invariant no. 5)
    bool in_prototype_chain = false;
    for (auto object_to_check = GC::Ptr { object.ptr() }; object_to_check && !seen_objects.contains(*object_to_check); object_to_check = TRY(object_to_check->internal_get_prototype_of())) {
        seen_objects.set(*object_to_check);
        TRY(object_to_check->for_each_own_property_with_enumerability([&](PropertyKey const& property_key, bool enumerable) -> ThrowCompletionOr<void> {
            if (!enumerable)
                seen_non_enumerable_properties.set(property_key);
            if (in_prototype_chain && enumerable) {
                if (seen_non_enumerable_properties.contains(property_key))
                    return {};
                ensure_seen_properties();
                if (seen_properties->contains(property_key))
                    return {};
            }
            if (enumerable)
                properties.append(property_key);
            if (seen_properties.has_value())
                seen_properties->set(property_key);
            return {};
        }));
        in_prototype_chain = true;
    }

    return PropertyNameIterator::create(vm.realm(), object, move(properties));
}

extern "C" {

// Forward declarations for all functions called from assembly.
i64 asm_fallback_handler(VM*, u32 pc, u8 const* instruction);
i64 asm_slow_path_jump_less_than(VM*, u32 pc, Op::JumpLessThan const*);
i64 asm_slow_path_jump_greater_than(VM*, u32 pc, Op::JumpGreaterThan const*);
i64 asm_slow_path_jump_less_than_equals(VM*, u32 pc, Op::JumpLessThanEquals const*);
i64 asm_slow_path_jump_greater_than_equals(VM*, u32 pc, Op::JumpGreaterThanEquals const*);
i64 asm_slow_path_jump_loosely_equals(VM*, u32 pc, Op::JumpLooselyEquals const*);
i64 asm_slow_path_create_private_environment(VM*, u32 pc, Op::CreatePrivateEnvironment const*);
i64 asm_slow_path_throw_const_assignment(VM*, u32 pc, Op::ThrowConstAssignment const*);
i64 asm_slow_path_resolve_this_binding(VM*, u32 pc, Op::ResolveThisBinding const*);
#define DECLARE_CALL_BUILTIN_SLOW_PATH(name, snake_case_name, ...) \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM*, u32 pc, Op::CallBuiltin##name const*);
JS_ENUMERATE_BUILTINS(DECLARE_CALL_BUILTIN_SLOW_PATH)
#undef DECLARE_CALL_BUILTIN_SLOW_PATH
i64 asm_slow_path_add(VM*, u32 pc, Op::Add const*);
i64 asm_slow_path_sub(VM*, u32 pc, Op::Sub const*);
i64 asm_slow_path_mul(VM*, u32 pc, Op::Mul const*);
i64 asm_slow_path_div(VM*, u32 pc, Op::Div const*);
i64 asm_slow_path_less_than(VM*, u32 pc, Op::LessThan const*);
i64 asm_slow_path_less_than_equals(VM*, u32 pc, Op::LessThanEquals const*);
i64 asm_slow_path_greater_than(VM*, u32 pc, Op::GreaterThan const*);
i64 asm_slow_path_greater_than_equals(VM*, u32 pc, Op::GreaterThanEquals const*);
i64 asm_slow_path_increment(VM*, u32 pc, Op::Increment const*);
i64 asm_slow_path_decrement(VM*, u32 pc, Op::Decrement const*);
i64 asm_slow_path_jump_loosely_inequals(VM*, u32 pc, Op::JumpLooselyInequals const*);
i64 asm_slow_path_jump_strictly_equals(VM*, u32 pc, Op::JumpStrictlyEquals const*);
i64 asm_slow_path_jump_strictly_inequals(VM*, u32 pc, Op::JumpStrictlyInequals const*);
i64 asm_slow_path_get_initialized_binding(VM*, u32 pc, Op::GetInitializedBinding const*);
i64 asm_slow_path_dynamic_get_initialized_binding(VM*, u32 pc, Op::DynamicGetInitializedBinding const*);
i64 asm_slow_path_get_callee_and_this(VM*, u32 pc, Op::GetCalleeAndThisFromEnvironment const*);
i64 asm_slow_path_dynamic_get_callee_and_this(VM*, u32 pc, Op::DynamicGetCalleeAndThisFromEnvironment const*);
i64 asm_slow_path_postfix_increment(VM*, u32 pc, Op::PostfixIncrement const*);
i64 asm_slow_path_get_by_id(VM*, u32 pc, Op::GetById const*);
i64 asm_slow_path_get_by_id_with_this(VM*, u32 pc, Op::GetByIdWithThis const*);
i64 asm_slow_path_put_by_id(VM*, u32 pc, Op::PutById const*);
i64 asm_slow_path_put_by_id_with_this(VM*, u32 pc, Op::PutByIdWithThis const*);
i64 asm_slow_path_get_by_value(VM*, u32 pc, Op::GetByValue const*);
i64 asm_slow_path_get_by_value_with_this(VM*, u32 pc, Op::GetByValueWithThis const*);
i64 asm_slow_path_get_length(VM*, u32 pc, Op::GetLength const*);
i64 asm_slow_path_get_length_with_this(VM*, u32 pc, Op::GetLengthWithThis const*);
i64 asm_slow_path_get_method(VM*, u32 pc, Op::GetMethod const*);
i64 asm_slow_path_get_iterator(VM*, u32 pc, Op::GetIterator const*);
i64 asm_slow_path_get_import_meta(VM*, u32 pc, Op::GetImportMeta const*);
i64 asm_slow_path_get_new_target(VM*, u32 pc, Op::GetNewTarget const*);
i64 asm_slow_path_get_super_constructor(VM*, u32 pc, Op::GetSuperConstructor const*);
i64 asm_slow_path_get_global(VM*, u32 pc, Op::GetGlobal const*);
i64 asm_slow_path_set_global(VM*, u32 pc, Op::SetGlobal const*);
i64 asm_slow_path_concat_string(VM*, u32 pc, Op::ConcatString const*);
i64 asm_slow_path_copy_object_excluding_properties(VM*, u32 pc, Op::CopyObjectExcludingProperties const*);
i64 asm_slow_path_exp(VM*, u32 pc, Op::Exp const*);
i64 asm_slow_path_import_call(VM*, u32 pc, Op::ImportCall const*);
i64 asm_slow_path_new_class(VM*, u32 pc, Op::NewClass const*);
i64 asm_slow_path_call(VM*, u32 pc, Op::Call const*);
i64 asm_slow_path_call_direct_eval(VM*, u32 pc, Op::CallDirectEval const*);
i64 asm_slow_path_call_with_argument_array(VM*, u32 pc, Op::CallWithArgumentArray const*);
i64 asm_slow_path_call_direct_eval_with_argument_array(VM*, u32 pc, Op::CallDirectEvalWithArgumentArray const*);
i64 asm_slow_path_get_object_property_iterator(VM*, u32 pc, Op::GetObjectPropertyIterator const*);
i64 asm_slow_path_object_property_iterator_next(VM*, u32 pc, Op::ObjectPropertyIteratorNext const*);
i64 asm_slow_path_iterator_close(VM*, u32 pc, Op::IteratorClose const*);
i64 asm_slow_path_iterator_next(VM*, u32 pc, Op::IteratorNext const*);
i64 asm_slow_path_iterator_next_unpack(VM*, u32 pc, Op::IteratorNextUnpack const*);
i64 asm_slow_path_iterator_to_array(VM*, u32 pc, Op::IteratorToArray const*);
i64 asm_slow_path_call_construct(VM*, u32 pc, Op::CallConstruct const*);
i64 asm_slow_path_call_construct_with_argument_array(VM*, u32 pc, Op::CallConstructWithArgumentArray const*);
i64 asm_slow_path_super_call_with_argument_array(VM*, u32 pc, Op::SuperCallWithArgumentArray const*);
i64 asm_slow_path_new_object(VM*, u32 pc, Op::NewObject const*);
i64 asm_slow_path_new_object_with_no_prototype(VM*, u32 pc, Op::NewObjectWithNoPrototype const*);
i64 asm_slow_path_cache_object_shape(VM*, u32 pc, Op::CacheObjectShape const*);
i64 asm_slow_path_init_object_literal_property(VM*, u32 pc, Op::InitObjectLiteralProperty const*);
i64 asm_slow_path_new_array(VM*, u32 pc, Op::NewArray const*);
i64 asm_slow_path_new_primitive_array(VM*, u32 pc, Op::NewPrimitiveArray const*);
i64 asm_slow_path_new_regexp(VM*, u32 pc, Op::NewRegExp const*);
i64 asm_slow_path_new_reference_error(VM*, u32 pc, Op::NewReferenceError const*);
i64 asm_slow_path_new_type_error(VM*, u32 pc, Op::NewTypeError const*);
i64 asm_slow_path_bitwise_xor(VM*, u32 pc, Op::BitwiseXor const*);
i64 asm_slow_path_bitwise_and(VM*, u32 pc, Op::BitwiseAnd const*);
i64 asm_slow_path_bitwise_or(VM*, u32 pc, Op::BitwiseOr const*);
i64 asm_slow_path_left_shift(VM*, u32 pc, Op::LeftShift const*);
i64 asm_slow_path_right_shift(VM*, u32 pc, Op::RightShift const*);
i64 asm_slow_path_unsigned_right_shift(VM*, u32 pc, Op::UnsignedRightShift const*);
i64 asm_slow_path_mod(VM*, u32 pc, Op::Mod const*);
i64 asm_slow_path_strictly_equals(VM*, u32 pc, Op::StrictlyEquals const*);
i64 asm_slow_path_strictly_inequals(VM*, u32 pc, Op::StrictlyInequals const*);
i64 asm_slow_path_loosely_equals(VM*, u32 pc, Op::LooselyEquals const*);
i64 asm_slow_path_loosely_inequals(VM*, u32 pc, Op::LooselyInequals const*);
i64 asm_slow_path_unary_minus(VM*, u32 pc, Op::UnaryMinus const*);
i64 asm_slow_path_to_string(VM*, u32 pc, Op::ToString const*);
i64 asm_slow_path_to_primitive_with_string_hint(VM*, u32 pc, Op::ToPrimitiveWithStringHint const*);
i64 asm_slow_path_to_object(VM*, u32 pc, Op::ToObject const*);
i64 asm_slow_path_to_length(VM*, u32 pc, Op::ToLength const*);
i64 asm_slow_path_typeof(VM*, u32 pc, Op::Typeof const*);
i64 asm_slow_path_postfix_decrement(VM*, u32 pc, Op::PostfixDecrement const*);
i64 asm_slow_path_to_int32(VM*, u32 pc, Op::ToInt32 const*);
i64 asm_slow_path_put_by_value(VM*, u32 pc, Op::PutByValue const*);
i64 asm_slow_path_put_by_value_with_this(VM*, u32 pc, Op::PutByValueWithThis const*);
i64 asm_slow_path_put_by_spread(VM*, u32 pc, Op::PutBySpread const*);
i64 asm_slow_path_get_binding(VM*, u32 pc, Op::GetBinding const*);
i64 asm_slow_path_dynamic_get_binding(VM*, u32 pc, Op::DynamicGetBinding const*);
i64 asm_slow_path_initialize_lexical_binding(VM*, u32 pc, Op::InitializeLexicalBinding const*);
i64 asm_slow_path_dynamic_initialize_lexical_binding(VM*, u32 pc, Op::DynamicInitializeLexicalBinding const*);
i64 asm_slow_path_initialize_variable_binding(VM*, u32 pc, Op::InitializeVariableBinding const*);
i64 asm_slow_path_dynamic_initialize_variable_binding(VM*, u32 pc, Op::DynamicInitializeVariableBinding const*);
i64 asm_slow_path_set_lexical_binding(VM*, u32 pc, Op::SetLexicalBinding const*);
i64 asm_slow_path_dynamic_set_lexical_binding(VM*, u32 pc, Op::DynamicSetLexicalBinding const*);
i64 asm_slow_path_set_variable_binding(VM*, u32 pc, Op::SetVariableBinding const*);
i64 asm_slow_path_dynamic_set_variable_binding(VM*, u32 pc, Op::DynamicSetVariableBinding const*);
i64 asm_slow_path_resolve_binding(VM*, u32 pc, Op::ResolveBinding const*);
i64 asm_slow_path_resolve_super_base(VM*, u32 pc, Op::ResolveSuperBase const*);
i64 asm_slow_path_set_resolved_binding(VM*, u32 pc, Op::SetResolvedBinding const*);
i64 asm_slow_path_typeof_binding(VM*, u32 pc, Op::TypeofBinding const*);
i64 asm_slow_path_dynamic_typeof_binding(VM*, u32 pc, Op::DynamicTypeofBinding const*);
i64 asm_slow_path_has_private_id(VM*, u32 pc, Op::HasPrivateId const*);
i64 asm_slow_path_set_function_name(VM*, u32 pc, Op::SetFunctionName const*);
i64 asm_slow_path_new_array_with_length(VM*, u32 pc, Op::NewArrayWithLength const*);
i64 asm_slow_path_array_append(VM*, u32 pc, Op::ArrayAppend const*);
i64 asm_slow_path_create_variable(VM*, u32 pc, Op::CreateVariable const*);
i64 asm_slow_path_enter_object_environment(VM*, u32 pc, Op::EnterObjectEnvironment const*);
i64 asm_slow_path_bitwise_not(VM*, u32 pc, Op::BitwiseNot const*);
i64 asm_slow_path_unary_plus(VM*, u32 pc, Op::UnaryPlus const*);
i64 asm_slow_path_is_constructor(VM*, u32 pc, Op::IsConstructor const*);
i64 asm_slow_path_add_private_name(VM*, u32 pc, Op::AddPrivateName const*);
i64 asm_slow_path_create_async_from_sync_iterator(VM*, u32 pc, Op::CreateAsyncFromSyncIterator const*);
i64 asm_slow_path_create_data_property_or_throw(VM*, u32 pc, Op::CreateDataPropertyOrThrow const*);
i64 asm_slow_path_create_immutable_binding(VM*, u32 pc, Op::CreateImmutableBinding const*);
i64 asm_slow_path_create_mutable_binding(VM*, u32 pc, Op::CreateMutableBinding const*);
i64 asm_slow_path_create_rest_params(VM*, u32 pc, Op::CreateRestParams const*);
i64 asm_slow_path_create_arguments(VM*, u32 pc, Op::CreateArguments const*);
i64 asm_slow_path_await(VM*, u32 pc, Op::Await const*);
i64 asm_slow_path_create_lexical_environment(VM*, u32 pc, Op::CreateLexicalEnvironment const*);
i64 asm_slow_path_create_variable_environment(VM*, u32 pc, Op::CreateVariableEnvironment const*);
i64 asm_slow_path_delete_by_id(VM*, u32 pc, Op::DeleteById const*);
i64 asm_slow_path_delete_by_value(VM*, u32 pc, Op::DeleteByValue const*);
i64 asm_slow_path_delete_variable(VM*, u32 pc, Op::DeleteVariable const*);
i64 asm_slow_path_get_completion_fields(VM*, u32 pc, Op::GetCompletionFields const*);
i64 asm_slow_path_set_completion_type(VM*, u32 pc, Op::SetCompletionType const*);
i64 asm_slow_path_get_template_object(VM*, u32 pc, Op::GetTemplateObject const*);
i64 asm_slow_path_new_function(VM*, u32 pc, Op::NewFunction const*);
i64 asm_slow_path_throw(VM*, u32 pc, Op::Throw const*);
i64 asm_slow_path_throw_if_tdz(VM*, u32 pc, Op::ThrowIfTDZ const*);
i64 asm_slow_path_throw_if_not_object(VM*, u32 pc, Op::ThrowIfNotObject const*);
i64 asm_slow_path_throw_if_nullish(VM*, u32 pc, Op::ThrowIfNullish const*);
i64 asm_slow_path_yield(VM*, u32 pc, Op::Yield const*);
i64 asm_slow_path_yield_iterator_result(VM*, u32 pc, Op::YieldIteratorResult const*);
i64 asm_slow_path_instance_of(VM*, u32 pc, Op::InstanceOf const*);
i64 asm_slow_path_in(VM*, u32 pc, Op::In const*);
i64 asm_slow_path_get_private_by_id(VM*, u32 pc, Op::GetPrivateById const*);
i64 asm_slow_path_put_private_by_id(VM*, u32 pc, Op::PutPrivateById const*);

i64 asm_try_get_global_env_binding(VM*, u32 pc, Op::GetGlobal const*);
i64 asm_try_set_global_env_binding(VM*, u32 pc, Op::SetGlobal const*);
i64 asm_try_put_by_value_holey_array(VM*, u32 pc, Op::PutByValue const*);
u64 asm_helper_to_boolean(u64 encoded_value);
u64 asm_helper_math_exp(u64 encoded_value);
u64 asm_helper_empty_string(u64);
u64 asm_helper_single_ascii_character_string(u64 encoded_value);
u64 asm_helper_single_utf16_code_unit_string(u64 encoded_value);
i64 asm_helper_handle_raw_native_exception(u64 encoded_exception);
i64 asm_try_inline_call(VM*, u32 pc, Op::Call const*);
i64 asm_try_put_by_id_cache(VM*, u32 pc, Op::PutById const*);
i64 asm_try_get_by_id_cache(VM*, u32 pc, Op::GetById const*);

i64 asm_try_get_by_value_typed_array(VM*, u32 pc, Op::GetByValue const*);
i64 asm_try_put_by_value_typed_array(VM*, u32 pc, Op::PutByValue const*);

// ===== Fallback handler for invalid dispatch table entries =====
// NB: Every bytecode opcode has a DSL handler, so this should never run.
i64 asm_fallback_handler(VM*, u32, u8 const*)
{
    VERIFY_NOT_REACHED();
}

// ===== Specific slow paths for asm-optimized instructions =====
// These are called from asm handlers when the fast path fails.
// Convention: i64 func(VM*, u32 pc, Op::Foo const* instruction)
//   Returns >= 0: new pc
//   Returns < 0: exit

i64 asm_slow_path_add(VM* vm, u32 pc, Op::Add const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, add(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::Add));
}

i64 asm_slow_path_sub(VM* vm, u32 pc, Op::Sub const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, sub(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::Sub));
}

i64 asm_slow_path_mul(VM* vm, u32 pc, Op::Mul const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, mul(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::Mul));
}

i64 asm_slow_path_div(VM* vm, u32 pc, Op::Div const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, div(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::Div));
}

i64 asm_slow_path_less_than(VM* vm, u32 pc, Op::LessThan const* instruction)
{
    vm->set(instruction->dst(), Value { ASM_TRY(*vm, pc, less_than(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))) });
    return static_cast<i64>(pc + sizeof(Op::LessThan));
}

i64 asm_slow_path_less_than_equals(VM* vm, u32 pc, Op::LessThanEquals const* instruction)
{
    vm->set(instruction->dst(), Value { ASM_TRY(*vm, pc, less_than_equals(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))) });
    return static_cast<i64>(pc + sizeof(Op::LessThanEquals));
}

i64 asm_slow_path_greater_than(VM* vm, u32 pc, Op::GreaterThan const* instruction)
{
    vm->set(instruction->dst(), Value { ASM_TRY(*vm, pc, greater_than(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))) });
    return static_cast<i64>(pc + sizeof(Op::GreaterThan));
}

i64 asm_slow_path_greater_than_equals(VM* vm, u32 pc, Op::GreaterThanEquals const* instruction)
{
    vm->set(instruction->dst(), Value { ASM_TRY(*vm, pc, greater_than_equals(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))) });
    return static_cast<i64>(pc + sizeof(Op::GreaterThanEquals));
}

i64 asm_slow_path_increment(VM* vm, u32 pc, Op::Increment const* instruction)
{
    auto old_value = ASM_TRY(*vm, pc, vm->get(instruction->dst()).to_numeric(*vm));
    if (old_value.is_number())
        vm->set(instruction->dst(), Value(old_value.as_double() + 1));
    else
        vm->set(instruction->dst(), BigInt::create(*vm, old_value.as_bigint().big_integer().plus(Crypto::SignedBigInteger { 1 })));
    return static_cast<i64>(pc + sizeof(Op::Increment));
}

i64 asm_slow_path_decrement(VM* vm, u32 pc, Op::Decrement const* instruction)
{
    auto old_value = ASM_TRY(*vm, pc, vm->get(instruction->dst()).to_numeric(*vm));
    if (old_value.is_number())
        vm->set(instruction->dst(), Value(old_value.as_double() - 1));
    else
        vm->set(instruction->dst(), BigInt::create(*vm, old_value.as_bigint().big_integer().minus(Crypto::SignedBigInteger { 1 })));
    return static_cast<i64>(pc + sizeof(Op::Decrement));
}

// Comparison jump slow paths return one of two target PCs.
#define DEFINE_JUMP_COMPARISON_SLOW_PATH(snake_name, op_name, compare_call)                   \
    i64 asm_slow_path_jump_##snake_name(VM* vm, u32 pc, Op::Jump##op_name const* instruction) \
    {                                                                                         \
        auto lhs = vm->get(instruction->lhs());                                               \
        auto rhs = vm->get(instruction->rhs());                                               \
        if (ASM_TRY(*vm, pc, compare_call))                                                   \
            return static_cast<i64>(instruction->true_target().address());                    \
        return static_cast<i64>(instruction->false_target().address());                       \
    }

DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than, LessThan, less_than(*vm, lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than, GreaterThan, greater_than(*vm, lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than_equals, LessThanEquals, less_than_equals(*vm, lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than_equals, GreaterThanEquals, greater_than_equals(*vm, lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(loosely_equals, LooselyEquals, is_loosely_equal(*vm, lhs, rhs))
#undef DEFINE_JUMP_COMPARISON_SLOW_PATH

i64 asm_slow_path_jump_loosely_inequals(VM* vm, u32 pc, Op::JumpLooselyInequals const* instruction)
{
    auto lhs = vm->get(instruction->lhs());
    auto rhs = vm->get(instruction->rhs());
    if (!ASM_TRY(*vm, pc, is_loosely_equal(*vm, lhs, rhs)))
        return static_cast<i64>(instruction->true_target().address());
    return static_cast<i64>(instruction->false_target().address());
}

i64 asm_slow_path_jump_strictly_equals(VM* vm, [[maybe_unused]] u32 pc, Op::JumpStrictlyEquals const* instruction)
{
    auto lhs = vm->get(instruction->lhs());
    auto rhs = vm->get(instruction->rhs());
    if (is_strictly_equal(lhs, rhs))
        return static_cast<i64>(instruction->true_target().address());
    return static_cast<i64>(instruction->false_target().address());
}

i64 asm_slow_path_jump_strictly_inequals(VM* vm, [[maybe_unused]] u32 pc, Op::JumpStrictlyInequals const* instruction)
{
    auto lhs = vm->get(instruction->lhs());
    auto rhs = vm->get(instruction->rhs());
    if (!is_strictly_equal(lhs, rhs))
        return static_cast<i64>(instruction->true_target().address());
    return static_cast<i64>(instruction->false_target().address());
}

// ===== Dedicated slow paths for hot instructions =====

i64 asm_slow_path_get_initialized_binding(VM* vm, u32 pc, Op::GetInitializedBinding const* instruction)
{
    auto next_pc = asm_get_binding<AsmBindingIsKnownToBeInitialized::Yes>(*vm, pc, instruction->dst(), instruction->cache());
    return advance_or_continue<Op::GetInitializedBinding>(pc, next_pc);
}

i64 asm_slow_path_dynamic_get_initialized_binding(VM* vm, u32 pc, Op::DynamicGetInitializedBinding const* instruction)
{
    auto& cache = vm->current_executable().environment_coordinate_caches[instruction->cache()];
    auto next_pc = asm_dynamic_get_binding<AsmBindingIsKnownToBeInitialized::Yes>(*vm, pc, instruction->dst(), instruction->identifier(), instruction->strict(), cache);
    return advance_or_continue<Op::DynamicGetInitializedBinding>(pc, next_pc);
}

i64 asm_slow_path_get_callee_and_this(VM* vm, u32 pc, Op::GetCalleeAndThisFromEnvironment const* instruction)
{
    auto const& cache = instruction->cache();
    VERIFY(cache.is_valid());

    auto const* environment = vm->running_execution_context().lexical_environment.ptr();
    for (size_t i = 0; i < cache.hops; ++i)
        environment = environment->outer_environment();

    auto callee = ASM_TRY(*vm, pc, static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(*vm, cache.index));
    vm->set(instruction->callee(), callee);
    auto this_value = js_undefined();
    if (auto base_object = environment->with_base_object()) [[unlikely]]
        this_value = base_object;
    vm->set(instruction->this_value(), this_value);
    return static_cast<i64>(pc + sizeof(Op::GetCalleeAndThisFromEnvironment));
}

i64 asm_slow_path_dynamic_get_callee_and_this(VM* vm, u32 pc, Op::DynamicGetCalleeAndThisFromEnvironment const* instruction)
{
    auto& cache = vm->current_executable().environment_coordinate_caches[instruction->cache()];
    auto next_pc = asm_dynamic_get_callee_and_this_from_environment(*vm, pc, instruction->callee(), instruction->this_value(), instruction->identifier(), instruction->strict(), cache);
    return advance_or_continue<Op::DynamicGetCalleeAndThisFromEnvironment>(pc, next_pc);
}

i64 asm_slow_path_postfix_increment(VM* vm, u32 pc, Op::PostfixIncrement const* instruction)
{
    auto old_value = ASM_TRY(*vm, pc, vm->get(instruction->src()).to_numeric(*vm));
    vm->set(instruction->dst(), old_value);
    if (old_value.is_number())
        vm->set(instruction->src(), Value(old_value.as_double() + 1));
    else
        vm->set(instruction->src(), BigInt::create(*vm, old_value.as_bigint().big_integer().plus(Crypto::SignedBigInteger { 1 })));
    return static_cast<i64>(pc + sizeof(Op::PostfixIncrement));
}

i64 asm_slow_path_get_by_id(VM* vm, u32 pc, Op::GetById const* instruction)
{
    auto base_value = vm->get(instruction->base());
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Normal>(*vm, [&] { return vm->get_identifier(instruction->base_identifier()); }, [&] -> PropertyKey const& { return vm->get_property_key(instruction->property()); }, base_value, base_value, cache));
    vm->set(instruction->dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetById));
}

i64 asm_slow_path_get_by_id_with_this(VM* vm, u32 pc, Op::GetByIdWithThis const* instruction)
{
    auto base_value = vm->get(instruction->base());
    auto this_value = vm->get(instruction->this_value());
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Normal>(*vm, [] { return Optional<Utf16FlyString const&> {}; }, [&] -> PropertyKey const& { return vm->get_property_key(instruction->property()); }, base_value, this_value, cache));
    vm->set(instruction->dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetByIdWithThis));
}

i64 asm_slow_path_put_by_id(VM* vm, u32 pc, Op::PutById const* instruction)
{
    auto value = vm->get(instruction->src());
    auto base = vm->get(instruction->base());
    Optional<Utf16FlyString const&> base_identifier;
    if (instruction->base_identifier().has_value())
        base_identifier = vm->get_identifier(instruction->base_identifier().value());
    auto const& property_key = vm->get_property_key(instruction->property());
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, base, value, base_identifier, property_key, instruction->kind(), instruction->strict(), &cache));
    return static_cast<i64>(pc + sizeof(Op::PutById));
}

i64 asm_slow_path_put_by_id_with_this(VM* vm, u32 pc, Op::PutByIdWithThis const* instruction)
{
    auto value = vm->get(instruction->src());
    auto base = vm->get(instruction->base());
    auto const& name = vm->get_property_key(instruction->property());
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, vm->get(instruction->this_value()), value, {}, name, instruction->kind(), instruction->strict(), &cache));
    return static_cast<i64>(pc + sizeof(Op::PutByIdWithThis));
}

i64 asm_slow_path_get_by_value(VM* vm, u32 pc, Op::GetByValue const* instruction)
{
    auto base_value = vm->get(instruction->base());
    auto property_key_value = vm->get(instruction->property());
    auto object = ASM_TRY(*vm, pc, base_object_for_get(*vm, base_value, [&]() -> Optional<Utf16FlyString const&> {
        if (instruction->base_identifier().has_value())
            return vm->get_identifier(instruction->base_identifier().value());
        return {}; }, [&] { return property_key_value; }));
    auto property_key = ASM_TRY(*vm, pc, property_key_value.to_property_key(*vm));
    if (base_value.is_string()) {
        auto string_value = ASM_TRY(*vm, pc, base_value.as_string().get(*vm, property_key));
        if (string_value.has_value()) {
            vm->set(instruction->dst(), *string_value);
            return static_cast<i64>(pc + sizeof(Op::GetByValue));
        }
    }
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, object->internal_get(property_key, base_value)));
    return static_cast<i64>(pc + sizeof(Op::GetByValue));
}

i64 asm_slow_path_get_by_value_with_this(VM* vm, u32 pc, Op::GetByValueWithThis const* instruction)
{
    auto property_key_value = vm->get(instruction->property());
    auto object = ASM_TRY(*vm, pc, vm->get(instruction->base()).to_object(*vm));
    auto property_key = ASM_TRY(*vm, pc, property_key_value.to_property_key(*vm));
    auto value = ASM_TRY(*vm, pc, object->internal_get(property_key, vm->get(instruction->this_value())));
    vm->set(instruction->dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetByValueWithThis));
}

i64 asm_slow_path_get_length(VM* vm, u32 pc, Op::GetLength const* instruction)
{
    auto base_value = vm->get(instruction->base());
    auto& executable = vm->current_executable();
    auto& cache = executable.property_lookup_caches[instruction->cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Length>(*vm, [&] { return vm->get_identifier(instruction->base_identifier()); }, [&] -> PropertyKey const& { return executable.get_property_key(*executable.length_identifier); }, base_value, base_value, cache));
    vm->set(instruction->dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetLength));
}

i64 asm_slow_path_get_length_with_this(VM* vm, u32 pc, Op::GetLengthWithThis const* instruction)
{
    auto base_value = vm->get(instruction->base());
    auto this_value = vm->get(instruction->this_value());
    auto& executable = vm->current_executable();
    auto& cache = executable.property_lookup_caches[instruction->cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Length>(*vm, [] { return Optional<Utf16FlyString const&> {}; }, [&] -> PropertyKey const& { return executable.get_property_key(*executable.length_identifier); }, base_value, this_value, cache));
    vm->set(instruction->dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetLengthWithThis));
}

i64 asm_slow_path_get_method(VM* vm, u32 pc, Op::GetMethod const* instruction)
{
    auto const& property_key = vm->get_property_key(instruction->property());
    auto method = ASM_TRY(*vm, pc, vm->get(instruction->object()).get_method(*vm, property_key));
    vm->set(instruction->dst(), method ?: js_undefined());
    return static_cast<i64>(pc + sizeof(Op::GetMethod));
}

i64 asm_slow_path_get_iterator(VM* vm, u32 pc, Op::GetIterator const* instruction)
{
    auto iterator_record = ASM_TRY(*vm, pc, get_iterator_impl(*vm, vm->get(instruction->iterable()), instruction->hint()));
    vm->set(instruction->dst_iterator_object(), iterator_record.iterator);
    vm->set(instruction->dst_iterator_next(), iterator_record.next_method);
    vm->set(instruction->dst_iterator_done(), Value(iterator_record.done));
    return static_cast<i64>(pc + sizeof(Op::GetIterator));
}

i64 asm_slow_path_get_import_meta(VM* vm, u32 pc, Op::GetImportMeta const* instruction)
{
    vm->set(instruction->dst(), vm->get_import_meta());
    return static_cast<i64>(pc + sizeof(Op::GetImportMeta));
}

i64 asm_slow_path_get_new_target(VM* vm, u32 pc, Op::GetNewTarget const* instruction)
{
    vm->set(instruction->dst(), vm->get_new_target());
    return static_cast<i64>(pc + sizeof(Op::GetNewTarget));
}

i64 asm_slow_path_get_super_constructor(VM* vm, u32 pc, Op::GetSuperConstructor const* instruction)
{
    auto* super_constructor = get_super_constructor(*vm);
    vm->set(instruction->dst(), super_constructor ? Value(super_constructor) : js_null());
    return static_cast<i64>(pc + sizeof(Op::GetSuperConstructor));
}

i64 asm_try_get_global_env_binding(VM* vm, u32, Op::GetGlobal const* instruction)
{
    auto& cache = vm->current_executable().global_variable_caches[instruction->cache()];

    if (!cache.has_environment_binding_index) [[unlikely]]
        return 1;

    auto& current_vm = *vm;
    ThrowCompletionOr<Value> result = js_undefined();
    if (cache.in_module_environment) {
        auto module = current_vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
        if (!module) [[unlikely]]
            return 1;
        result = (*module)->environment()->get_binding_value_direct(current_vm, cache.environment_binding_index);
    } else {
        result = vm->global_declarative_environment().get_binding_value_direct(current_vm, cache.environment_binding_index);
    }
    if (result.is_error()) [[unlikely]]
        return 1;
    vm->set(instruction->dst(), result.value());
    return 0;
}

i64 asm_slow_path_get_global(VM* vm, u32 pc, Op::GetGlobal const* instruction)
{

    auto& binding_object = vm->global_object();
    auto& declarative_record = vm->global_declarative_environment();
    auto& cache = vm->current_executable().global_variable_caches[instruction->cache()];

    auto& shape = binding_object.shape();
    if (cache.environment_serial_number == declarative_record.environment_serial_number()) {
        auto* entry = cache.first_entry();
        if (entry && &shape == entry->shape && (!shape.is_dictionary() || shape.dictionary_generation() == entry->shape_dictionary_generation)) {
            auto value = binding_object.get_direct(entry->property_offset);
            vm->set(instruction->dst(), ASM_TRY(*vm, pc, get_cached_property_value(*vm, value, &binding_object)));
            return static_cast<i64>(pc + sizeof(Op::GetGlobal));
        }

        if (cache.has_environment_binding_index) {
            Value value;
            if (cache.in_module_environment) {
                auto module = vm->running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
                value = ASM_TRY(*vm, pc, (*module)->environment()->get_binding_value_direct(*vm, cache.environment_binding_index));
            } else {
                value = ASM_TRY(*vm, pc, declarative_record.get_binding_value_direct(*vm, cache.environment_binding_index));
            }
            vm->set(instruction->dst(), value);
            return static_cast<i64>(pc + sizeof(Op::GetGlobal));
        }
    }

    cache.environment_serial_number = declarative_record.environment_serial_number();

    auto& identifier = vm->get_identifier(instruction->identifier());

    if (auto* module = vm->running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>()) {
        auto& module_environment = *(*module)->environment();
        Optional<size_t> index;
        if (ASM_TRY(*vm, pc, module_environment.has_binding(identifier, &index))) {
            if (index.has_value()) {
                cache.environment_binding_index = static_cast<u32>(index.value());
                cache.has_environment_binding_index = true;
                cache.in_module_environment = true;
                vm->set(instruction->dst(), ASM_TRY(*vm, pc, module_environment.get_binding_value_direct(*vm, index.value())));
                return static_cast<i64>(pc + sizeof(Op::GetGlobal));
            }
            vm->set(instruction->dst(), ASM_TRY(*vm, pc, module_environment.get_binding_value(*vm, identifier, true)));
            return static_cast<i64>(pc + sizeof(Op::GetGlobal));
        }
    }

    Optional<size_t> offset;
    if (ASM_TRY(*vm, pc, declarative_record.has_binding(identifier, &offset))) {
        cache.environment_binding_index = static_cast<u32>(offset.value());
        cache.has_environment_binding_index = true;
        cache.in_module_environment = false;
        vm->set(instruction->dst(), ASM_TRY(*vm, pc, declarative_record.get_binding_value(*vm, identifier, instruction->strict() == Strict::Yes)));
        return static_cast<i64>(pc + sizeof(Op::GetGlobal));
    }

    if (ASM_TRY(*vm, pc, binding_object.has_property(identifier))) [[likely]] {
        CacheableGetPropertyMetadata cacheable_metadata;
        auto value = ASM_TRY(*vm, pc, binding_object.internal_get(identifier, &binding_object, &cacheable_metadata));
        if (cacheable_metadata.type == CacheableGetPropertyMetadata::Type::GetOwnProperty) {
            cache.update(PropertyLookupCache::Entry::Type::GetOwnProperty, [&](auto& entry) {
                entry.shape = shape;
                entry.property_offset = cacheable_metadata.property_offset.value();

                if (shape.is_dictionary())
                    entry.shape_dictionary_generation = shape.dictionary_generation();
            });
        }
        vm->set(instruction->dst(), value);
        return static_cast<i64>(pc + sizeof(Op::GetGlobal));
    }

    auto completion = vm->throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, identifier);
    return handle_asm_exception(*vm, pc, completion.value());
}

i64 asm_try_set_global_env_binding(VM* vm, u32, Op::SetGlobal const* instruction)
{
    auto& cache = vm->current_executable().global_variable_caches[instruction->cache()];

    if (!cache.has_environment_binding_index) [[unlikely]]
        return 1;

    auto& current_vm = *vm;
    auto src = vm->get(instruction->src());
    ThrowCompletionOr<void> result;
    if (cache.in_module_environment) {
        auto module = current_vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
        if (!module) [[unlikely]]
            return 1;
        result = (*module)->environment()->set_mutable_binding_direct(current_vm, cache.environment_binding_index, src, instruction->strict() == Strict::Yes);
    } else {
        result = vm->global_declarative_environment().set_mutable_binding_direct(current_vm, cache.environment_binding_index, src, instruction->strict() == Strict::Yes);
    }
    if (result.is_error()) [[unlikely]]
        return 1;
    return 0;
}

i64 asm_slow_path_set_global(VM* vm, u32 pc, Op::SetGlobal const* instruction)
{

    auto& binding_object = vm->global_object();
    auto& declarative_record = vm->global_declarative_environment();
    auto& cache = vm->current_executable().global_variable_caches[instruction->cache()];
    auto& shape = binding_object.shape();
    auto src = vm->get(instruction->src());

    if (cache.environment_serial_number == declarative_record.environment_serial_number()) {
        auto* entry = cache.first_entry();
        if (entry && &shape == entry->shape && (!shape.is_dictionary() || shape.dictionary_generation() == entry->shape_dictionary_generation)) {
            auto value = binding_object.get_direct(entry->property_offset);
            if (value.is_accessor())
                ASM_TRY(*vm, pc, call(*vm, value.as_accessor().setter(), &binding_object, src));
            else
                binding_object.put_direct(entry->property_offset, src);
            return static_cast<i64>(pc + sizeof(Op::SetGlobal));
        }

        if (cache.has_environment_binding_index) {
            if (cache.in_module_environment) {
                auto module = vm->running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
                ASM_TRY(*vm, pc, (*module)->environment()->set_mutable_binding_direct(*vm, cache.environment_binding_index, src, instruction->strict() == Strict::Yes));
            } else {
                ASM_TRY(*vm, pc, declarative_record.set_mutable_binding_direct(*vm, cache.environment_binding_index, src, instruction->strict() == Strict::Yes));
            }
            return static_cast<i64>(pc + sizeof(Op::SetGlobal));
        }
    }

    cache.environment_serial_number = declarative_record.environment_serial_number();

    auto& identifier = vm->get_identifier(instruction->identifier());

    if (auto* module = vm->running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>()) {
        auto& module_environment = *(*module)->environment();
        Optional<size_t> index;
        if (ASM_TRY(*vm, pc, module_environment.has_binding(identifier, &index))) {
            if (index.has_value()) {
                cache.environment_binding_index = static_cast<u32>(index.value());
                cache.has_environment_binding_index = true;
                cache.in_module_environment = true;
                ASM_TRY(*vm, pc, module_environment.set_mutable_binding_direct(*vm, index.value(), src, instruction->strict() == Strict::Yes));
                return static_cast<i64>(pc + sizeof(Op::SetGlobal));
            }
            ASM_TRY(*vm, pc, module_environment.set_mutable_binding(*vm, identifier, src, instruction->strict() == Strict::Yes));
            return static_cast<i64>(pc + sizeof(Op::SetGlobal));
        }
    }

    Optional<size_t> offset;
    if (ASM_TRY(*vm, pc, declarative_record.has_binding(identifier, &offset))) {
        cache.environment_binding_index = static_cast<u32>(offset.value());
        cache.has_environment_binding_index = true;
        cache.in_module_environment = false;
        ASM_TRY(*vm, pc, declarative_record.set_mutable_binding(*vm, identifier, src, instruction->strict() == Strict::Yes));
        return static_cast<i64>(pc + sizeof(Op::SetGlobal));
    }

    if (ASM_TRY(*vm, pc, binding_object.has_property(identifier))) {
        CacheableSetPropertyMetadata cacheable_metadata;
        auto success = ASM_TRY(*vm, pc, binding_object.internal_set(identifier, src, &binding_object, &cacheable_metadata));
        if (!success && instruction->strict() == Strict::Yes) [[unlikely]] {
            auto property_or_error = binding_object.internal_get_own_property(identifier);
            if (!property_or_error.is_error()) {
                auto property = property_or_error.release_value();
                if (property.has_value() && !property->writable.value_or(true)) {
                    auto completion = vm->throw_completion<TypeError>(ErrorType::DescWriteNonWritable, identifier);
                    return handle_asm_exception(*vm, pc, completion.value());
                }
            }
            auto completion = vm->throw_completion<TypeError>(ErrorType::ObjectSetReturnedFalse);
            return handle_asm_exception(*vm, pc, completion.value());
        }
        if (cacheable_metadata.type == CacheableSetPropertyMetadata::Type::ChangeOwnProperty) {
            cache.update(PropertyLookupCache::Entry::Type::ChangeOwnProperty, [&](auto& entry) {
                entry.shape = shape;
                entry.property_offset = cacheable_metadata.property_offset.value();

                if (shape.is_dictionary())
                    entry.shape_dictionary_generation = shape.dictionary_generation();
            });
        }
        return static_cast<i64>(pc + sizeof(Op::SetGlobal));
    }

    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(identifier, instruction->strict(), &declarative_record));
    ASM_TRY(*vm, pc, reference.put_value(*vm, src));
    return static_cast<i64>(pc + sizeof(Op::SetGlobal));
}

i64 asm_slow_path_concat_string(VM* vm, u32 pc, Op::ConcatString const* instruction)
{
    auto string = ASM_TRY(*vm, pc, vm->get(instruction->src()).to_primitive_string(*vm));
    vm->set(instruction->dst(), PrimitiveString::create(*vm, vm->get(instruction->dst()).as_string(), string));
    return static_cast<i64>(pc + sizeof(Op::ConcatString));
}

i64 asm_slow_path_copy_object_excluding_properties(VM* vm, u32 pc, Op::CopyObjectExcludingProperties const* instruction)
{
    auto& realm = *vm->current_realm();
    auto from_object = vm->get(instruction->from_object());
    auto to_object = Object::create(realm, realm.intrinsics().object_prototype());

    GC::ConservativeHashTable<PropertyKey> excluded_names;
    auto excluded_names_operands = instruction->excluded_names();
    for (size_t i = 0; i < instruction->excluded_names_count(); ++i)
        excluded_names.set(ASM_TRY(*vm, pc, vm->get(excluded_names_operands[i]).to_property_key(*vm)));

    ASM_TRY(*vm, pc, to_object->copy_data_properties(*vm, from_object, excluded_names));
    vm->set(instruction->dst(), to_object);
    return static_cast<i64>(pc + instruction->length());
}

i64 asm_slow_path_exp(VM* vm, u32 pc, Op::Exp const* instruction)
{
    auto result = ASM_TRY(*vm, pc, exp(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs())));
    vm->set(instruction->dst(), result);
    return static_cast<i64>(pc + sizeof(Op::Exp));
}

i64 asm_slow_path_import_call(VM* vm, u32 pc, Op::ImportCall const* instruction)
{
    auto specifier = vm->get(instruction->specifier());
    auto options_value = vm->get(instruction->options());
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, perform_import_call(*vm, specifier, options_value)));
    return static_cast<i64>(pc + sizeof(Op::ImportCall));
}

i64 asm_slow_path_new_class(VM* vm, u32 pc, Op::NewClass const* instruction)
{

    Value super_class;
    if (instruction->super_class().has_value())
        super_class = vm->get(instruction->super_class().value());
    GC::RootVector<Value> element_keys;
    element_keys.ensure_capacity(instruction->element_keys_count());
    for (size_t i = 0; i < instruction->element_keys_count(); ++i) {
        Value element_key;
        if (instruction->element_keys()[i].has_value())
            element_key = vm->get(instruction->element_keys()[i].value());
        element_keys.unchecked_append(element_key);
    }

    auto& running_execution_context = vm->running_execution_context();
    auto* class_environment = &as<Environment>(vm->get(instruction->class_environment()).as_cell());
    auto& outer_environment = running_execution_context.lexical_environment;

    auto const& blueprint = vm->current_executable().class_blueprints[instruction->class_blueprint_index()];

    Optional<Utf16FlyString> binding_name;
    Utf16FlyString class_name;
    if (!blueprint.has_name && instruction->lhs_name().has_value()) {
        class_name = vm->get_identifier(instruction->lhs_name().value());
    } else {
        class_name = blueprint.name;
        binding_name = class_name;
    }

    auto* retval = ASM_TRY(*vm, pc, construct_class(*vm, blueprint, vm->current_executable(), class_environment, outer_environment, super_class, element_keys, binding_name, class_name));
    vm->set(instruction->dst(), retval);
    return static_cast<i64>(pc + instruction->length());
}

static COLD Completion throw_type_error_for_asm_callee(VM& vm, Value callee, StringView callee_type, Optional<StringTableIndex> const expression_string)
{
    if (expression_string.has_value())
        return vm.throw_completion<TypeError>(ErrorType::IsNotAEvaluatedFrom, callee, callee_type, vm.current_executable().get_string(*expression_string));

    return vm.throw_completion<TypeError>(ErrorType::IsNotA, callee, callee_type);
}

static ThrowCompletionOr<void> throw_if_needed_for_asm_call(VM& vm, Value callee, Op::CallType call_type, Optional<StringTableIndex> const expression_string)
{
    if ((call_type == Op::CallType::Call || call_type == Op::CallType::DirectEval)
        && !callee.is_function()) [[unlikely]]
        return throw_type_error_for_asm_callee(vm, callee, "function"sv, expression_string);
    if (call_type == Op::CallType::Construct && !callee.is_constructor()) [[unlikely]]
        return throw_type_error_for_asm_callee(vm, callee, "constructor"sv, expression_string);
    return {};
}

NEVER_INLINE static ThrowCompletionOr<void> execute_asm_call(
    Op::CallType call_type,
    VM& vm,
    Value callee,
    Value this_value,
    ReadonlySpan<Operand> arguments,
    Operand dst,
    Optional<StringTableIndex> const expression_string,
    Strict strict)
{
    TRY(throw_if_needed_for_asm_call(vm, callee, call_type, expression_string));

    auto& function = callee.as_function();

    size_t registers_and_locals_count = 0;
    ReadonlySpan<Value> constants;
    size_t argument_count = arguments.size();
    function.get_stack_frame_info(registers_and_locals_count, constants, argument_count);

    auto& stack = vm.interpreter_stack();
    auto* stack_mark = stack.top();
    auto* callee_context = stack.allocate(registers_and_locals_count, constants, max(arguments.size(), argument_count));
    if (!callee_context) [[unlikely]]
        return vm.throw_completion<InternalError>(ErrorType::CallStackSizeExceeded);
    ScopeGuard deallocate_guard = [&stack, stack_mark] {
        if (stack.top() > stack_mark)
            stack.deallocate(stack_mark);
    };

    auto* callee_context_argument_values = callee_context->arguments_data();
    auto const callee_context_argument_count = callee_context->argument_count;
    auto const insn_argument_count = arguments.size();

    for (size_t i = 0; i < insn_argument_count; ++i)
        callee_context_argument_values[i] = vm.get(arguments.data()[i]);
    for (size_t i = insn_argument_count; i < callee_context_argument_count; ++i)
        callee_context_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    Value retval;
    if (call_type == Op::CallType::DirectEval) {
        if (callee == vm.realm().intrinsics().eval_function()) {
            retval = TRY(perform_eval(vm, callee_context->argument_count > 0 ? callee_context->arguments_data()[0] : js_undefined(), strict == Strict::Yes ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
        } else {
            retval = TRY(function.internal_call(*callee_context, this_value));
        }
    } else if (call_type == Op::CallType::Construct) {
        retval = TRY(function.internal_construct(*callee_context, function));
    } else {
        retval = TRY(function.internal_call(*callee_context, this_value));
    }
    vm.set(dst, retval);
    return {};
}

i64 asm_slow_path_call(VM* vm, u32 pc, Op::Call const* instruction)
{
    ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, vm->get(instruction->callee()), vm->get(instruction->this_value()), instruction->arguments(), instruction->dst(), instruction->expression_string(), instruction->strict()));
    return static_cast<i64>(pc + instruction->length());
}

static ThrowCompletionOr<void> call_direct_eval(
    VM& vm,
    Value callee,
    Value this_value,
    ReadonlySpan<Operand> arguments,
    Operand dst,
    Optional<StringTableIndex> const expression_string,
    Strict strict)
{
    TRY(throw_if_needed_for_asm_call(vm, callee, Op::CallType::DirectEval, expression_string));

    auto& function = callee.as_function();

    size_t registers_and_locals_count = 0;
    ReadonlySpan<Value> constants;
    size_t argument_count = arguments.size();
    function.get_stack_frame_info(registers_and_locals_count, constants, argument_count);

    auto& stack = vm.interpreter_stack();
    auto* stack_mark = stack.top();
    auto* callee_context = stack.allocate(registers_and_locals_count, constants, max(arguments.size(), argument_count));
    if (!callee_context) [[unlikely]]
        return vm.throw_completion<InternalError>(ErrorType::CallStackSizeExceeded);
    ScopeGuard deallocate_guard = [&stack, stack_mark] { stack.deallocate(stack_mark); };

    auto* callee_context_argument_values = callee_context->arguments_data();
    auto const callee_context_argument_count = callee_context->argument_count;
    auto const insn_argument_count = arguments.size();

    for (size_t i = 0; i < insn_argument_count; ++i)
        callee_context_argument_values[i] = vm.get(arguments.data()[i]);
    for (size_t i = insn_argument_count; i < callee_context_argument_count; ++i)
        callee_context_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    Value retval;
    if (callee == vm.realm().intrinsics().eval_function()) {
        retval = TRY(perform_eval(vm, callee_context->argument_count > 0 ? callee_context->arguments_data()[0] : js_undefined(), strict == Strict::Yes ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
    } else {
        retval = TRY(function.internal_call(*callee_context, this_value));
    }
    vm.set(dst, retval);
    return {};
}

i64 asm_slow_path_call_direct_eval(VM* vm, u32 pc, Op::CallDirectEval const* instruction)
{
    ASM_TRY(*vm, pc, call_direct_eval(*vm, vm->get(instruction->callee()), vm->get(instruction->this_value()), instruction->arguments(), instruction->dst(), instruction->expression_string(), instruction->strict()));
    return static_cast<i64>(pc + instruction->length());
}

static ThrowCompletionOr<void> call_with_argument_array(
    Op::CallType call_type,
    VM& vm,
    Value callee,
    Value this_value,
    Value arguments,
    Operand dst,
    Optional<StringTableIndex> const expression_string,
    Strict strict)
{
    TRY(throw_if_needed_for_asm_call(vm, callee, call_type, expression_string));

    auto& function = callee.as_function();

    auto& argument_array = arguments.as_array_exotic_object();
    auto argument_array_length = argument_array.indexed_array_like_size();

    size_t argument_count = argument_array_length;
    size_t registers_and_locals_count = 0;
    ReadonlySpan<Value> constants;
    function.get_stack_frame_info(registers_and_locals_count, constants, argument_count);

    auto& stack = vm.interpreter_stack();
    auto* stack_mark = stack.top();
    auto* callee_context = stack.allocate(registers_and_locals_count, constants, max(argument_array_length, argument_count));
    if (!callee_context) [[unlikely]]
        return vm.throw_completion<InternalError>(ErrorType::CallStackSizeExceeded);
    ScopeGuard deallocate_guard = [&stack, stack_mark] {
        if (stack.top() > stack_mark)
            stack.deallocate(stack_mark);
    };

    auto* callee_context_argument_values = callee_context->arguments_data();
    auto const callee_context_argument_count = callee_context->argument_count;
    auto const insn_argument_count = argument_array_length;

    for (size_t i = 0; i < insn_argument_count; ++i) {
        if (auto maybe_value = argument_array.indexed_get(i); maybe_value.has_value())
            callee_context_argument_values[i] = maybe_value.release_value().value;
        else
            callee_context_argument_values[i] = js_undefined();
    }
    for (size_t i = insn_argument_count; i < callee_context_argument_count; ++i)
        callee_context_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    Value retval;
    if (call_type == Op::CallType::DirectEval && callee == vm.realm().intrinsics().eval_function()) {
        retval = TRY(perform_eval(vm, callee_context->argument_count > 0 ? callee_context->arguments_data()[0] : js_undefined(), strict == Strict::Yes ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
    } else if (call_type == Op::CallType::Construct) {
        retval = TRY(function.internal_construct(*callee_context, function));
    } else {
        retval = TRY(function.internal_call(*callee_context, this_value));
    }

    vm.set(dst, retval);
    return {};
}

i64 asm_slow_path_call_with_argument_array(VM* vm, u32 pc, Op::CallWithArgumentArray const* instruction)
{
    ASM_TRY(*vm, pc, call_with_argument_array(Op::CallType::Call, *vm, vm->get(instruction->callee()), vm->get(instruction->this_value()), vm->get(instruction->arguments()), instruction->dst(), instruction->expression_string(), instruction->strict()));
    return static_cast<i64>(pc + sizeof(Op::CallWithArgumentArray));
}

i64 asm_slow_path_call_direct_eval_with_argument_array(VM* vm, u32 pc, Op::CallDirectEvalWithArgumentArray const* instruction)
{
    ASM_TRY(*vm, pc, call_with_argument_array(Op::CallType::DirectEval, *vm, vm->get(instruction->callee()), vm->get(instruction->this_value()), vm->get(instruction->arguments()), instruction->dst(), instruction->expression_string(), instruction->strict()));
    return static_cast<i64>(pc + sizeof(Op::CallDirectEvalWithArgumentArray));
}

i64 asm_slow_path_get_object_property_iterator(VM* vm, u32 pc, Op::GetObjectPropertyIterator const* instruction)
{
    auto* cache = &vm->current_executable().object_property_iterator_caches[instruction->cache()];
    vm->set(instruction->dst_iterator(), ASM_TRY(*vm, pc, asm_get_object_property_iterator(*vm, vm->get(instruction->object()), cache)));
    return static_cast<i64>(pc + sizeof(Op::GetObjectPropertyIterator));
}

i64 asm_slow_path_object_property_iterator_next(VM* vm, u32 pc, Op::ObjectPropertyIteratorNext const* instruction)
{
    auto& iterator = static_cast<PropertyNameIterator&>(vm->get(instruction->iterator_object()).as_object());
    Value value;
    bool done = false;
    ASM_TRY(*vm, pc, iterator.next(*vm, done, value));
    vm->set(instruction->dst_done(), Value(done));
    if (!done)
        vm->set(instruction->dst_value(), value);
    return static_cast<i64>(pc + sizeof(Op::ObjectPropertyIteratorNext));
}

i64 asm_slow_path_iterator_close(VM* vm, u32 pc, Op::IteratorClose const* instruction)
{
    auto& iterator_object = vm->get(instruction->iterator_object()).as_object();
    auto iterator_next_method = vm->get(instruction->iterator_next());
    auto iterator_done_property = vm->get(instruction->iterator_done()).as_bool();
    IteratorRecordImpl iterator_record { .done = iterator_done_property, .iterator = iterator_object, .next_method = iterator_next_method };

    ASM_TRY(*vm, pc, iterator_close(*vm, iterator_record, Completion { instruction->completion_type(), vm->get(instruction->completion_value()) }));
    return static_cast<i64>(pc + sizeof(Op::IteratorClose));
}

i64 asm_slow_path_iterator_next(VM* vm, u32 pc, Op::IteratorNext const* instruction)
{
    auto& iterator_object = vm->get(instruction->iterator_object()).as_object();
    auto iterator_next_method = vm->get(instruction->iterator_next());
    auto iterator_done_property = vm->get(instruction->iterator_done()).as_bool();
    IteratorRecordImpl iterator_record { .done = iterator_done_property, .iterator = iterator_object, .next_method = iterator_next_method };
    auto result = iterator_next(*vm, iterator_record);
    if (iterator_record.done)
        vm->set(instruction->iterator_done(), Value(true));
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, result));
    return static_cast<i64>(pc + sizeof(Op::IteratorNext));
}

i64 asm_slow_path_iterator_next_unpack(VM* vm, u32 pc, Op::IteratorNextUnpack const* instruction)
{
    auto& iterator_object = vm->get(instruction->iterator_object()).as_object();
    auto iterator_next_method = vm->get(instruction->iterator_next());
    auto iterator_done_property = vm->get(instruction->iterator_done()).as_bool();
    IteratorRecordImpl iterator_record { .done = iterator_done_property, .iterator = iterator_object, .next_method = iterator_next_method };
    auto iteration_result_or_done_or_error = iterator_step(*vm, iterator_record);
    if (iterator_record.done)
        vm->set(instruction->iterator_done(), Value(true));
    auto iteration_result_or_done = ASM_TRY(*vm, pc, iteration_result_or_done_or_error);
    if (iteration_result_or_done.has<IterationDone>()) {
        vm->set(instruction->dst_done(), Value(true));
        return static_cast<i64>(pc + sizeof(Op::IteratorNextUnpack));
    }
    auto& iteration_result = iteration_result_or_done.get<IterationResult>();
    vm->set(instruction->dst_done(), ASM_TRY(*vm, pc, iteration_result.done));
    auto value = move(iteration_result.value);
    if (value.is_throw_completion())
        vm->set(instruction->iterator_done(), Value(true));
    vm->set(instruction->dst_value(), ASM_TRY(*vm, pc, value));
    return static_cast<i64>(pc + sizeof(Op::IteratorNextUnpack));
}

i64 asm_slow_path_iterator_to_array(VM* vm, u32 pc, Op::IteratorToArray const* instruction)
{
    IteratorRecordImpl iterator_record {
        .done = vm->get(instruction->iterator_done_property()).as_bool(),
        .iterator = vm->get(instruction->iterator_object()).as_object(),
        .next_method = vm->get(instruction->iterator_next_method())
    };

    auto array = MUST(JS::Array::create(*vm->current_realm(), 0));
    size_t index = 0;
    while (true) {
        auto value_or_error = iterator_step_value(*vm, iterator_record);
        if (iterator_record.done)
            vm->set(instruction->iterator_done_property(), Value(true));
        auto value = ASM_TRY(*vm, pc, value_or_error);
        if (!value.has_value()) {
            vm->set(instruction->dst(), array);
            return static_cast<i64>(pc + sizeof(Op::IteratorToArray));
        }

        MUST(array->create_data_property_or_throw(index, value.release_value()));
        ++index;
    }
}

#define JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, implementation)                                                                                                                    \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM* vm, u32 pc, Op::CallBuiltin##name const* instruction)                                                                                           \
    {                                                                                                                                                                                                    \
        Operand arguments[] { instruction->argument() };                                                                                                                                                 \
        auto callee = vm->get(instruction->callee());                                                                                                                                                    \
        if (callee.is_function() && callee.as_function().builtin() == Builtin::name) {                                                                                                                   \
            vm->set(instruction->dst(), ASM_TRY(*vm, pc, implementation(*vm, vm->get(instruction->argument()))));                                                                                        \
            return static_cast<i64>(pc + sizeof(Op::CallBuiltin##name));                                                                                                                                 \
        }                                                                                                                                                                                                \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, callee, vm->get(instruction->this_value()), arguments, instruction->dst(), instruction->expression_string(), instruction->strict())); \
        return static_cast<i64>(pc + sizeof(Op::CallBuiltin##name));                                                                                                                                     \
    }

#define JS_DEFINE_BINARY_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, implementation)                                                                                                                   \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM* vm, u32 pc, Op::CallBuiltin##name const* instruction)                                                                                           \
    {                                                                                                                                                                                                    \
        Operand arguments[] { instruction->argument0(), instruction->argument1() };                                                                                                                      \
        auto callee = vm->get(instruction->callee());                                                                                                                                                    \
        if (callee.is_function() && callee.as_function().builtin() == Builtin::name) {                                                                                                                   \
            vm->set(instruction->dst(), ASM_TRY(*vm, pc, implementation(*vm, vm->get(instruction->argument0()), vm->get(instruction->argument1()))));                                                    \
            return static_cast<i64>(pc + sizeof(Op::CallBuiltin##name));                                                                                                                                 \
        }                                                                                                                                                                                                \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, callee, vm->get(instruction->this_value()), arguments, instruction->dst(), instruction->expression_string(), instruction->strict())); \
        return static_cast<i64>(pc + sizeof(Op::CallBuiltin##name));                                                                                                                                     \
    }

#define JS_DEFINE_NULLARY_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, implementation)                                                                                                           \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM* vm, u32 pc, Op::CallBuiltin##name const* instruction)                                                                                    \
    {                                                                                                                                                                                             \
        auto callee = vm->get(instruction->callee());                                                                                                                                             \
        if (callee.is_function() && callee.as_function().builtin() == Builtin::name) {                                                                                                            \
            vm->set(instruction->dst(), implementation());                                                                                                                                        \
            return static_cast<i64>(pc + sizeof(Op::CallBuiltin##name));                                                                                                                          \
        }                                                                                                                                                                                         \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, callee, vm->get(instruction->this_value()), {}, instruction->dst(), instruction->expression_string(), instruction->strict())); \
        return static_cast<i64>(pc + sizeof(Op::CallBuiltin##name));                                                                                                                              \
    }

#define JS_DEFINE_GENERIC_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, ...)                                                                                                                                              \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM* vm, u32 pc, Op::CallBuiltin##name const* instruction)                                                                                                            \
    {                                                                                                                                                                                                                     \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, vm->get(instruction->callee()), vm->get(instruction->this_value()), {}, instruction->dst(), instruction->expression_string(), instruction->strict())); \
        return static_cast<i64>(pc + sizeof(Op::CallBuiltin##name));                                                                                                                                                      \
    }

#define JS_DEFINE_UNARY_GENERIC_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, ...)                                                                                                                                               \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM* vm, u32 pc, Op::CallBuiltin##name const* instruction)                                                                                                                   \
    {                                                                                                                                                                                                                            \
        Operand arguments[] { instruction->argument() };                                                                                                                                                                         \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, vm->get(instruction->callee()), vm->get(instruction->this_value()), arguments, instruction->dst(), instruction->expression_string(), instruction->strict())); \
        return static_cast<i64>(pc + sizeof(Op::CallBuiltin##name));                                                                                                                                                             \
    }

#define JS_DEFINE_BINARY_GENERIC_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, ...)                                                                                                                                              \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM* vm, u32 pc, Op::CallBuiltin##name const* instruction)                                                                                                                   \
    {                                                                                                                                                                                                                            \
        Operand arguments[] { instruction->argument0(), instruction->argument1() };                                                                                                                                              \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, vm->get(instruction->callee()), vm->get(instruction->this_value()), arguments, instruction->dst(), instruction->expression_string(), instruction->strict())); \
        return static_cast<i64>(pc + sizeof(Op::CallBuiltin##name));                                                                                                                                                             \
    }

JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathAbs, math_abs, MathObject::abs_impl)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathLog, math_log, MathObject::log_impl)
JS_DEFINE_BINARY_BUILTIN_CALL_SLOW_PATH(MathPow, math_pow, MathObject::pow_impl)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathExp, math_exp, MathObject::exp_impl)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathCeil, math_ceil, MathObject::ceil_impl)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathFloor, math_floor, MathObject::floor_impl)
JS_DEFINE_BINARY_BUILTIN_CALL_SLOW_PATH(MathImul, math_imul, MathObject::imul_impl)
JS_DEFINE_NULLARY_BUILTIN_CALL_SLOW_PATH(MathRandom, math_random, MathObject::random_impl)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathRound, math_round, MathObject::round_impl)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathSqrt, math_sqrt, MathObject::sqrt_impl)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathSin, math_sin, MathObject::sin_impl)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathCos, math_cos, MathObject::cos_impl)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(MathTan, math_tan, MathObject::tan_impl)
JS_DEFINE_UNARY_GENERIC_BUILTIN_CALL_SLOW_PATH(RegExpPrototypeExec, regexp_prototype_exec)
JS_DEFINE_BINARY_GENERIC_BUILTIN_CALL_SLOW_PATH(RegExpPrototypeReplace, regexp_prototype_replace)
JS_DEFINE_BINARY_GENERIC_BUILTIN_CALL_SLOW_PATH(RegExpPrototypeSplit, regexp_prototype_split)
JS_DEFINE_UNARY_GENERIC_BUILTIN_CALL_SLOW_PATH(OrdinaryHasInstance, ordinary_has_instance)
JS_DEFINE_GENERIC_BUILTIN_CALL_SLOW_PATH(ArrayIteratorPrototypeNext, array_iterator_prototype_next)
JS_DEFINE_GENERIC_BUILTIN_CALL_SLOW_PATH(MapIteratorPrototypeNext, map_iterator_prototype_next)
JS_DEFINE_GENERIC_BUILTIN_CALL_SLOW_PATH(SetIteratorPrototypeNext, set_iterator_prototype_next)
JS_DEFINE_GENERIC_BUILTIN_CALL_SLOW_PATH(StringIteratorPrototypeNext, string_iterator_prototype_next)
JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(StringFromCharCode, string_from_char_code, StringConstructor::from_char_code_impl)
JS_DEFINE_UNARY_GENERIC_BUILTIN_CALL_SLOW_PATH(StringPrototypeCharCodeAt, string_prototype_char_code_at)
JS_DEFINE_UNARY_GENERIC_BUILTIN_CALL_SLOW_PATH(StringPrototypeCharAt, string_prototype_char_at)

#undef JS_DEFINE_BINARY_GENERIC_BUILTIN_CALL_SLOW_PATH
#undef JS_DEFINE_UNARY_GENERIC_BUILTIN_CALL_SLOW_PATH
#undef JS_DEFINE_GENERIC_BUILTIN_CALL_SLOW_PATH
#undef JS_DEFINE_NULLARY_BUILTIN_CALL_SLOW_PATH
#undef JS_DEFINE_BINARY_BUILTIN_CALL_SLOW_PATH
#undef JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH

i64 asm_slow_path_call_construct(VM* vm, u32 pc, Op::CallConstruct const* instruction)
{
    ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Construct, *vm, vm->get(instruction->callee()), js_undefined(), instruction->arguments(), instruction->dst(), instruction->expression_string(), instruction->strict()));
    return static_cast<i64>(pc + instruction->length());
}

i64 asm_slow_path_call_construct_with_argument_array(VM* vm, u32 pc, Op::CallConstructWithArgumentArray const* instruction)
{
    ASM_TRY(*vm, pc, call_with_argument_array(Op::CallType::Construct, *vm, vm->get(instruction->callee()), js_undefined(), vm->get(instruction->arguments()), instruction->dst(), instruction->expression_string(), instruction->strict()));
    return static_cast<i64>(pc + sizeof(Op::CallConstructWithArgumentArray));
}

i64 asm_slow_path_super_call_with_argument_array(VM* vm, u32 pc, Op::SuperCallWithArgumentArray const* instruction)
{

    auto new_target = vm->get_new_target();
    VERIFY(new_target.is_object());

    auto super_constructor = vm->get(instruction->super_constructor());
    if (!super_constructor.is_constructor()) [[unlikely]] {
        vm->running_execution_context().program_counter = pc;
        auto completion = vm->throw_completion<TypeError>(ErrorType::NotAConstructor, "Super constructor");
        return handle_asm_exception(*vm, pc, completion.value());
    }

    auto& function = super_constructor.as_function();

    auto& argument_array = vm->get(instruction->arguments()).as_array_exotic_object();
    size_t argument_array_length = 0;

    if (instruction->is_synthetic()) {
        argument_array_length = MUST(length_of_array_like(*vm, argument_array));
    } else {
        argument_array_length = argument_array.indexed_array_like_size();
    }

    size_t argument_count = argument_array_length;
    size_t registers_and_locals_count = 0;
    ReadonlySpan<Value> constants;
    function.get_stack_frame_info(registers_and_locals_count, constants, argument_count);

    auto& stack = vm->interpreter_stack();
    auto* stack_mark = stack.top();
    auto* callee_context = stack.allocate(registers_and_locals_count, constants, max(argument_array_length, argument_count));
    if (!callee_context) [[unlikely]] {
        vm->running_execution_context().program_counter = pc;
        auto completion = vm->throw_completion<InternalError>(ErrorType::CallStackSizeExceeded);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    ScopeGuard deallocate_guard = [&stack, stack_mark] {
        if (stack.top() > stack_mark)
            stack.deallocate(stack_mark);
    };

    auto* callee_context_argument_values = callee_context->arguments_data();
    auto const callee_context_argument_count = callee_context->argument_count;
    auto const insn_argument_count = argument_array_length;

    if (instruction->is_synthetic()) {
        for (size_t i = 0; i < insn_argument_count; ++i)
            callee_context_argument_values[i] = argument_array.get_without_side_effects(PropertyKey { i });
    } else {
        for (size_t i = 0; i < insn_argument_count; ++i) {
            if (auto maybe_value = argument_array.indexed_get(i); maybe_value.has_value())
                callee_context_argument_values[i] = maybe_value.release_value().value;
            else
                callee_context_argument_values[i] = js_undefined();
        }
    }
    for (size_t i = insn_argument_count; i < callee_context_argument_count; ++i)
        callee_context_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    auto result = ASM_TRY(*vm, pc, function.internal_construct(*callee_context, new_target.as_function()));

    auto& this_environment = as<FunctionEnvironment>(*get_this_environment(*vm));
    ASM_TRY(*vm, pc, this_environment.bind_this_value(*vm, result));

    auto& f = as<ECMAScriptFunctionObject>(this_environment.function_object());
    ASM_TRY(*vm, pc, result->initialize_instance_elements(f));

    vm->set(instruction->dst(), result);
    return static_cast<i64>(pc + sizeof(Op::SuperCallWithArgumentArray));
}

i64 asm_slow_path_new_object(VM* vm, u32 pc, Op::NewObject const* instruction)
{
    auto& realm = *vm->current_realm();

    if (instruction->cache() != NumericLimits<u32>::max()) {
        auto& cache = vm->current_executable().object_shape_caches[instruction->cache()];
        auto cached_shape = cache.shape.ptr();
        if (cached_shape) {
            vm->set(instruction->dst(), Object::create_with_premade_shape(*cached_shape));
            return static_cast<i64>(pc + sizeof(Op::NewObject));
        }
    }

    vm->set(instruction->dst(), Object::create(realm, realm.intrinsics().object_prototype()));
    return static_cast<i64>(pc + sizeof(Op::NewObject));
}

i64 asm_slow_path_new_object_with_no_prototype(VM* vm, u32 pc, Op::NewObjectWithNoPrototype const* instruction)
{
    auto& realm = *vm->current_realm();
    vm->set(instruction->dst(), Object::create(realm, nullptr));
    return static_cast<i64>(pc + sizeof(Op::NewObjectWithNoPrototype));
}

i64 asm_slow_path_cache_object_shape(VM* vm, u32 pc, Op::CacheObjectShape const* instruction)
{
    auto& cache = vm->current_executable().object_shape_caches[instruction->cache()];
    if (!cache.shape) {
        auto& object = vm->get(instruction->object()).as_object();
        if (!object.shape().is_dictionary())
            cache.shape = &object.shape();
    }
    return static_cast<i64>(pc + sizeof(Op::CacheObjectShape));
}

i64 asm_slow_path_init_object_literal_property(VM* vm, u32 pc, Op::InitObjectLiteralProperty const* instruction)
{
    auto& object = vm->get(instruction->object()).as_object();
    auto value = vm->get(instruction->src());
    auto& cache = vm->current_executable().object_shape_caches[instruction->shape_cache_index()];

    auto cached_shape = cache.shape.ptr();
    if (cached_shape && &object.shape() == cached_shape && instruction->property_slot() < cache.property_offsets.size()) {
        object.put_direct(cache.property_offsets[instruction->property_slot()], value);
        return static_cast<i64>(pc + sizeof(Op::InitObjectLiteralProperty));
    }

    auto const& property_key = vm->current_executable().get_property_key(instruction->property());
    object.define_direct_property(property_key, value, JS::Attribute::Enumerable | JS::Attribute::Writable | JS::Attribute::Configurable);

    if (!object.shape().is_dictionary()) {
        auto metadata = object.shape().lookup(property_key);
        if (metadata.has_value()) {
            if (instruction->property_slot() >= cache.property_offsets.size())
                cache.property_offsets.resize(instruction->property_slot() + 1);
            cache.property_offsets[instruction->property_slot()] = metadata->offset;
        }
    }

    return static_cast<i64>(pc + sizeof(Op::InitObjectLiteralProperty));
}

i64 asm_slow_path_new_array(VM* vm, u32 pc, Op::NewArray const* instruction)
{
    auto array = MUST(JS::Array::create(vm->realm(), instruction->element_count()));
    for (size_t i = 0; i < instruction->element_count(); ++i)
        array->indexed_put(i, vm->get(instruction->elements()[i]));
    vm->set(instruction->dst(), array);
    return static_cast<i64>(pc + instruction->length());
}

i64 asm_slow_path_new_primitive_array(VM* vm, u32 pc, Op::NewPrimitiveArray const* instruction)
{
    auto array = MUST(JS::Array::create(vm->realm(), instruction->element_count()));
    for (size_t i = 0; i < instruction->element_count(); ++i)
        array->indexed_put(i, instruction->elements()[i]);
    vm->set(instruction->dst(), array);
    return static_cast<i64>(pc + instruction->length());
}

i64 asm_slow_path_new_regexp(VM* vm, u32 pc, Op::NewRegExp const* instruction)
{
    auto& realm = *vm->current_realm();
    auto regexp_object = RegExpObject::create(
        realm,
        vm->current_executable().get_string(instruction->source_index()),
        vm->current_executable().get_string(instruction->flags_index()));
    regexp_object->set_realm(realm);
    regexp_object->set_legacy_features_enabled(true);
    vm->set(instruction->dst(), regexp_object);
    return static_cast<i64>(pc + sizeof(Op::NewRegExp));
}

i64 asm_slow_path_new_reference_error(VM* vm, u32 pc, Op::NewReferenceError const* instruction)
{
    auto& realm = *vm->current_realm();
    vm->set(instruction->dst(), ReferenceError::create(realm, vm->current_executable().get_string(instruction->error_string())));
    return static_cast<i64>(pc + sizeof(Op::NewReferenceError));
}

i64 asm_slow_path_new_type_error(VM* vm, u32 pc, Op::NewTypeError const* instruction)
{
    auto& realm = *vm->current_realm();
    vm->set(instruction->dst(), TypeError::create(realm, vm->current_executable().get_string(instruction->error_string())));
    return static_cast<i64>(pc + sizeof(Op::NewTypeError));
}

i64 asm_slow_path_bitwise_xor(VM* vm, u32 pc, Op::BitwiseXor const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, bitwise_xor(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::BitwiseXor));
}

i64 asm_slow_path_bitwise_and(VM* vm, u32 pc, Op::BitwiseAnd const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, bitwise_and(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::BitwiseAnd));
}

i64 asm_slow_path_bitwise_or(VM* vm, u32 pc, Op::BitwiseOr const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, bitwise_or(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::BitwiseOr));
}

i64 asm_slow_path_left_shift(VM* vm, u32 pc, Op::LeftShift const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, left_shift(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::LeftShift));
}

i64 asm_slow_path_right_shift(VM* vm, u32 pc, Op::RightShift const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, right_shift(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::RightShift));
}

i64 asm_slow_path_unsigned_right_shift(VM* vm, u32 pc, Op::UnsignedRightShift const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, unsigned_right_shift(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::UnsignedRightShift));
}

i64 asm_slow_path_mod(VM* vm, u32 pc, Op::Mod const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, mod(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))));
    return static_cast<i64>(pc + sizeof(Op::Mod));
}

static ThrowCompletionOr<bool> loosely_equals(VM& vm, Value lhs, Value rhs)
{
    if (lhs.tag() == rhs.tag()) {
        if (lhs.is_int32() || lhs.is_object() || lhs.is_boolean() || lhs.is_nullish())
            return lhs.encoded() == rhs.encoded();
    }
    return TRY(is_loosely_equal(vm, lhs, rhs));
}

static ThrowCompletionOr<bool> loosely_inequals(VM& vm, Value lhs, Value rhs)
{
    return !TRY(loosely_equals(vm, lhs, rhs));
}

static bool strictly_equals(Value lhs, Value rhs)
{
    if (lhs.tag() == rhs.tag()) {
        if (lhs.is_int32() || lhs.is_object() || lhs.is_boolean() || lhs.is_nullish())
            return lhs.encoded() == rhs.encoded();
    }
    return is_strictly_equal(lhs, rhs);
}

i64 asm_slow_path_strictly_equals(VM* vm, u32 pc, Op::StrictlyEquals const* instruction)
{
    vm->set(instruction->dst(), Value { strictly_equals(vm->get(instruction->lhs()), vm->get(instruction->rhs())) });
    return static_cast<i64>(pc + sizeof(Op::StrictlyEquals));
}

i64 asm_slow_path_strictly_inequals(VM* vm, u32 pc, Op::StrictlyInequals const* instruction)
{
    vm->set(instruction->dst(), Value { !strictly_equals(vm->get(instruction->lhs()), vm->get(instruction->rhs())) });
    return static_cast<i64>(pc + sizeof(Op::StrictlyInequals));
}

i64 asm_slow_path_loosely_equals(VM* vm, u32 pc, Op::LooselyEquals const* instruction)
{
    vm->set(instruction->dst(), Value { ASM_TRY(*vm, pc, loosely_equals(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))) });
    return static_cast<i64>(pc + sizeof(Op::LooselyEquals));
}

i64 asm_slow_path_loosely_inequals(VM* vm, u32 pc, Op::LooselyInequals const* instruction)
{
    vm->set(instruction->dst(), Value { ASM_TRY(*vm, pc, loosely_inequals(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs()))) });
    return static_cast<i64>(pc + sizeof(Op::LooselyInequals));
}

i64 asm_slow_path_unary_minus(VM* vm, u32 pc, Op::UnaryMinus const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, unary_minus(*vm, vm->get(instruction->src()))));
    return static_cast<i64>(pc + sizeof(Op::UnaryMinus));
}

i64 asm_slow_path_to_string(VM* vm, u32 pc, Op::ToString const* instruction)
{
    auto result = ASM_TRY(*vm, pc, vm->get(instruction->value()).to_primitive_string(*vm));
    vm->set(instruction->dst(), Value { result });
    return static_cast<i64>(pc + sizeof(Op::ToString));
}

i64 asm_slow_path_to_primitive_with_string_hint(VM* vm, u32 pc, Op::ToPrimitiveWithStringHint const* instruction)
{
    auto result = ASM_TRY(*vm, pc, vm->get(instruction->value()).to_primitive(*vm, Value::PreferredType::String));
    vm->set(instruction->dst(), result);
    return static_cast<i64>(pc + sizeof(Op::ToPrimitiveWithStringHint));
}

i64 asm_slow_path_to_object(VM* vm, u32 pc, Op::ToObject const* instruction)
{
    auto result = ASM_TRY(*vm, pc, vm->get(instruction->value()).to_object(*vm));
    vm->set(instruction->dst(), result);
    return static_cast<i64>(pc + sizeof(Op::ToObject));
}

i64 asm_slow_path_to_length(VM* vm, u32 pc, Op::ToLength const* instruction)
{
    auto result = ASM_TRY(*vm, pc, vm->get(instruction->value()).to_length(*vm));
    vm->set(instruction->dst(), Value { result });
    return static_cast<i64>(pc + sizeof(Op::ToLength));
}

i64 asm_slow_path_typeof(VM* vm, u32 pc, Op::Typeof const* instruction)
{
    vm->set(instruction->dst(), vm->get(instruction->src()).typeof_(*vm));
    return static_cast<i64>(pc + sizeof(Op::Typeof));
}

i64 asm_slow_path_postfix_decrement(VM* vm, u32 pc, Op::PostfixDecrement const* instruction)
{
    auto old_value = ASM_TRY(*vm, pc, vm->get(instruction->src()).to_numeric(*vm));
    vm->set(instruction->dst(), old_value);
    if (old_value.is_number())
        vm->set(instruction->src(), Value(old_value.as_double() - 1));
    else
        vm->set(instruction->src(), BigInt::create(*vm, old_value.as_bigint().big_integer().minus(Crypto::SignedBigInteger { 1 })));
    return static_cast<i64>(pc + sizeof(Op::PostfixDecrement));
}

i64 asm_slow_path_to_int32(VM* vm, u32 pc, Op::ToInt32 const* instruction)
{
    vm->set(instruction->dst(), Value(ASM_TRY(*vm, pc, vm->get(instruction->value()).to_i32(*vm))));
    return static_cast<i64>(pc + sizeof(Op::ToInt32));
}

i64 asm_slow_path_put_by_value(VM* vm, u32 pc, Op::PutByValue const* instruction)
{
    auto value = vm->get(instruction->src());
    auto base = vm->get(instruction->base());
    Optional<Utf16FlyString const&> base_identifier;
    if (instruction->base_identifier().has_value())
        base_identifier = vm->get_identifier(instruction->base_identifier().value());
    auto property = vm->get(instruction->property());
    auto property_key = ASM_TRY(*vm, pc, property.to_property_key(*vm));
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, base, value, base_identifier, property_key, instruction->kind(), instruction->strict()));
    return static_cast<i64>(pc + sizeof(Op::PutByValue));
}

i64 asm_slow_path_put_by_value_with_this(VM* vm, u32 pc, Op::PutByValueWithThis const* instruction)
{
    auto value = vm->get(instruction->src());
    auto base = vm->get(instruction->base());
    auto this_value = vm->get(instruction->this_value());
    auto property_key = ASM_TRY(*vm, pc, vm->get(instruction->property()).to_property_key(*vm));
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, this_value, value, {}, property_key, instruction->kind(), instruction->strict()));
    return static_cast<i64>(pc + sizeof(Op::PutByValueWithThis));
}

i64 asm_slow_path_put_by_spread(VM* vm, u32 pc, Op::PutBySpread const* instruction)
{
    auto value = vm->get(instruction->src());
    auto base = vm->get(instruction->base());

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto object = ASM_TRY(*vm, pc, base.to_object(*vm));

    ASM_TRY(*vm, pc, object->copy_data_properties(*vm, value, {}));
    return static_cast<i64>(pc + sizeof(Op::PutBySpread));
}

i64 asm_try_put_by_value_holey_array(VM* vm, u32, Op::PutByValue const* instruction)
{

    auto base = vm->get(instruction->base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = vm->get(instruction->property());
    if (!property.is_non_negative_int32()) [[unlikely]]
        return 1;

    auto& object = base.as_object();
    if (!is<JS::Array>(object)) [[unlikely]]
        return 1;

    auto& array = static_cast<JS::Array&>(object);
    if (array.is_proxy_target()
        || !array.default_prototype_chain_intact()
        || !array.extensible()
        || array.may_interfere_with_indexed_property_access()
        || array.indexed_storage_kind() != IndexedStorageKind::Holey) [[unlikely]]
        return 1;

    auto index = static_cast<u32>(property.as_i32());
    if (index >= array.indexed_array_like_size()) [[unlikely]]
        return 1;

    array.indexed_put(index, vm->get(instruction->src()));
    return 0;
}

// Try to inline a JS-to-JS call by building the callee frame through the
// shared VM::push_inline_frame() helper. Returns 0 on success (callee frame
// pushed) and 1 on failure (caller should keep handling the Call itself).
i64 asm_try_inline_call(VM* vm, u32 pc, Op::Call const* instruction)
{

    auto callee = vm->get(instruction->callee());
    if (!callee.is_object()) [[unlikely]]
        return 1;

    auto& callee_object = callee.as_object();
    if (!is<ECMAScriptFunctionObject>(callee_object)) [[unlikely]]
        return 1;

    auto& callee_function = static_cast<ECMAScriptFunctionObject&>(callee_object);
    if (!callee_function.can_inline_call()) [[unlikely]]
        return 1;

    auto* callee_context = vm->push_inline_frame(
        callee_function,
        callee_function.inline_call_executable(),
        instruction->arguments(),
        pc + instruction->length(),
        instruction->dst().raw(),
        vm->get(instruction->this_value()),
        nullptr,
        false);

    return callee_context ? 0 : 1;
}

// Fast cache-only PutById. Tries all cache entries for ChangeOwnProperty and
// AddOwnProperty. Returns 0 on cache hit, 1 on miss (caller should use full slow path).
i64 asm_try_put_by_id_cache(VM* vm, u32, Op::PutById const* instruction)
{
    auto base = vm->get(instruction->base());
    if (!base.is_object()) [[unlikely]]
        return 1;
    auto& object = base.as_object();
    auto value = vm->get(instruction->src());
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];

    for (auto& entry : cache.entries()) {
        switch (entry.type) {
        case PropertyLookupCache::Entry::Type::ChangeOwnProperty: {
            auto cached_shape = entry.shape.ptr();
            if (cached_shape != &object.shape()) [[unlikely]]
                continue;
            if (cached_shape->is_dictionary()
                && cached_shape->dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto current = object.get_direct(entry.property_offset);
            if (current.is_accessor()) [[unlikely]]
                return 1;
            object.put_direct(entry.property_offset, value);
            return 0;
        }
        case PropertyLookupCache::Entry::Type::AddOwnProperty: {
            if (entry.from_shape != &object.shape()) [[unlikely]]
                continue;
            auto cached_shape = entry.shape.ptr();
            if (!cached_shape) [[unlikely]]
                continue;
            if (!object.extensible()) [[unlikely]]
                continue;
            if (cached_shape->is_dictionary()
                && object.shape().dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto pcv = entry.prototype_chain_validity.ptr();
            if (pcv && !pcv->is_valid()) [[unlikely]]
                continue;
            object.unsafe_set_shape(*cached_shape);
            object.put_direct(entry.property_offset, value);
            return 0;
        }
        default:
            continue;
        }
    }
    return 1;
}

// Fast cache-only GetById. Tries all cache entries for own-property and prototype
// chain lookups. On cache hit, writes the result to the dst operand and returns 0.
// On miss, returns 1 (caller should use full slow path).
i64 asm_try_get_by_id_cache(VM* vm, u32, Op::GetById const* instruction)
{
    auto base = vm->get(instruction->base());
    if (!base.is_object()) [[unlikely]]
        return 1;
    auto& object = base.as_object();
    auto& shape = object.shape();
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];

    for (auto& entry : cache.entries()) {
        if (entry.type != PropertyLookupCache::Entry::Type::GetOwnProperty
            && entry.type != PropertyLookupCache::Entry::Type::GetPropertyInPrototypeChain) {
            continue;
        }

        auto cached_prototype = entry.prototype.ptr();
        if (cached_prototype) {
            if (&shape != entry.shape) [[unlikely]]
                continue;
            if (shape.is_dictionary()
                && shape.dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto pcv = entry.prototype_chain_validity.ptr();
            if (!pcv || !pcv->is_valid()) [[unlikely]]
                continue;
            auto value = cached_prototype->get_direct(entry.property_offset);
            if (value.is_accessor()) [[unlikely]]
                return 1;
            vm->set(instruction->dst(), value);
            return 0;
        } else if (&shape == entry.shape) {
            if (shape.is_dictionary()
                && shape.dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto value = object.get_direct(entry.property_offset);
            if (value.is_accessor()) [[unlikely]]
                return 1;
            vm->set(instruction->dst(), value);
            return 0;
        }
    }
    return 1;
}

i64 asm_slow_path_get_binding(VM* vm, u32 pc, Op::GetBinding const* instruction)
{
    auto next_pc = asm_get_binding<AsmBindingIsKnownToBeInitialized::No>(*vm, pc, instruction->dst(), instruction->cache());
    return advance_or_continue<Op::GetBinding>(pc, next_pc);
}

i64 asm_slow_path_dynamic_get_binding(VM* vm, u32 pc, Op::DynamicGetBinding const* instruction)
{
    auto& cache = vm->current_executable().environment_coordinate_caches[instruction->cache()];
    auto next_pc = asm_dynamic_get_binding<AsmBindingIsKnownToBeInitialized::No>(*vm, pc, instruction->dst(), instruction->identifier(), instruction->strict(), cache);
    return advance_or_continue<Op::DynamicGetBinding>(pc, next_pc);
}

i64 asm_slow_path_initialize_lexical_binding(VM* vm, u32 pc, Op::InitializeLexicalBinding const* instruction)
{
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Initialize>(*vm, pc, instruction->strict(), vm->get(instruction->src()), instruction->cache());
    return advance_or_continue<Op::InitializeLexicalBinding>(pc, next_pc);
}

i64 asm_slow_path_dynamic_initialize_lexical_binding(VM* vm, u32 pc, Op::DynamicInitializeLexicalBinding const* instruction)
{
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Initialize>(*vm, pc, instruction->identifier(), instruction->strict(), vm->get(instruction->src()), vm->current_executable().environment_coordinate_caches[instruction->cache()]);
    return advance_or_continue<Op::DynamicInitializeLexicalBinding>(pc, next_pc);
}

i64 asm_slow_path_initialize_variable_binding(VM* vm, u32 pc, Op::InitializeVariableBinding const* instruction)
{
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Initialize>(*vm, pc, instruction->strict(), vm->get(instruction->src()), instruction->cache());
    return advance_or_continue<Op::InitializeVariableBinding>(pc, next_pc);
}

i64 asm_slow_path_dynamic_initialize_variable_binding(VM* vm, u32 pc, Op::DynamicInitializeVariableBinding const* instruction)
{
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Initialize>(*vm, pc, instruction->identifier(), instruction->strict(), vm->get(instruction->src()), vm->current_executable().environment_coordinate_caches[instruction->cache()]);
    return advance_or_continue<Op::DynamicInitializeVariableBinding>(pc, next_pc);
}

i64 asm_slow_path_set_lexical_binding(VM* vm, u32 pc, Op::SetLexicalBinding const* instruction)
{
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Set>(*vm, pc, instruction->strict(), vm->get(instruction->src()), instruction->cache());
    return advance_or_continue<Op::SetLexicalBinding>(pc, next_pc);
}

i64 asm_slow_path_dynamic_set_lexical_binding(VM* vm, u32 pc, Op::DynamicSetLexicalBinding const* instruction)
{
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Set>(*vm, pc, instruction->identifier(), instruction->strict(), vm->get(instruction->src()), vm->current_executable().environment_coordinate_caches[instruction->cache()]);
    return advance_or_continue<Op::DynamicSetLexicalBinding>(pc, next_pc);
}

i64 asm_slow_path_set_variable_binding(VM* vm, u32 pc, Op::SetVariableBinding const* instruction)
{
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Set>(*vm, pc, instruction->strict(), vm->get(instruction->src()), instruction->cache());
    return advance_or_continue<Op::SetVariableBinding>(pc, next_pc);
}

i64 asm_slow_path_dynamic_set_variable_binding(VM* vm, u32 pc, Op::DynamicSetVariableBinding const* instruction)
{
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Set>(*vm, pc, instruction->identifier(), instruction->strict(), vm->get(instruction->src()), vm->current_executable().environment_coordinate_caches[instruction->cache()]);
    return advance_or_continue<Op::DynamicSetVariableBinding>(pc, next_pc);
}

i64 asm_slow_path_resolve_binding(VM* vm, u32 pc, Op::ResolveBinding const* instruction)
{
    auto const& identifier = vm->get_identifier(instruction->identifier());
    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(identifier, instruction->strict()));
    if (reference.is_unresolvable()) {
        vm->set(instruction->dst(), js_null());
        return static_cast<i64>(pc + sizeof(Op::ResolveBinding));
    }

    VERIFY(reference.is_environment_reference());
    vm->set(instruction->dst(), &reference.base_environment());
    return static_cast<i64>(pc + sizeof(Op::ResolveBinding));
}

i64 asm_slow_path_resolve_super_base(VM* vm, u32 pc, Op::ResolveSuperBase const* instruction)
{

    auto& environment = as<FunctionEnvironment>(*get_this_environment(*vm));
    VERIFY(environment.has_super_binding());
    auto base_value = ASM_TRY(*vm, pc, environment.get_super_base());
    vm->set(instruction->dst(), base_value);
    return static_cast<i64>(pc + sizeof(Op::ResolveSuperBase));
}

i64 asm_slow_path_set_resolved_binding(VM* vm, u32 pc, Op::SetResolvedBinding const* instruction)
{
    auto const& identifier = vm->get_identifier(instruction->identifier());
    auto environment = vm->get(instruction->environment());
    auto reference = environment.is_null()
        ? Reference { Reference::BaseType::Unresolvable, PropertyKey { identifier }, instruction->strict() }
        : Reference { as<Environment>(environment.as_cell()), identifier, instruction->strict() };
    ASM_TRY(*vm, pc, reference.put_value(*vm, vm->get(instruction->src())));
    return static_cast<i64>(pc + sizeof(Op::SetResolvedBinding));
}

i64 asm_slow_path_typeof_binding(VM* vm, u32 pc, Op::TypeofBinding const* instruction)
{
    VERIFY(instruction->cache().is_valid());

    auto const* environment = vm->running_execution_context().lexical_environment.ptr();
    for (size_t i = 0; i < instruction->cache().hops; ++i)
        environment = environment->outer_environment();

    auto value = ASM_TRY(*vm, pc, static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(*vm, instruction->cache().index));
    vm->set(instruction->dst(), value.typeof_(*vm));
    return static_cast<i64>(pc + sizeof(Op::TypeofBinding));
}

i64 asm_slow_path_dynamic_typeof_binding(VM* vm, u32 pc, Op::DynamicTypeofBinding const* instruction)
{
    auto& cache = vm->current_executable().environment_coordinate_caches[instruction->cache()];
    auto const* current_environment = vm->running_execution_context().lexical_environment.ptr();
    if (auto const* environment = asm_get_cached_environment(current_environment, cache)) [[likely]] {
        auto value = ASM_TRY(*vm, pc, static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(*vm, cache.index));
        vm->set(instruction->dst(), value.typeof_(*vm));
        return static_cast<i64>(pc + sizeof(Op::DynamicTypeofBinding));
    }

    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(vm->get_identifier(instruction->identifier()), instruction->strict()));
    if (reference.is_unresolvable()) {
        vm->set(instruction->dst(), PrimitiveString::create(*vm, "undefined"_string));
        return static_cast<i64>(pc + sizeof(Op::DynamicTypeofBinding));
    }

    asm_update_environment_coordinate_cache(current_environment, reference, cache);
    auto value = ASM_TRY(*vm, pc, reference.get_value(*vm));
    vm->set(instruction->dst(), value.typeof_(*vm));
    return static_cast<i64>(pc + sizeof(Op::DynamicTypeofBinding));
}

static Optional<StringView> asm_function_name_prefix_to_string(Op::FunctionNamePrefix prefix)
{
    switch (prefix) {
    case Op::FunctionNamePrefix::None:
        return {};
    case Op::FunctionNamePrefix::Get:
        return "get"sv;
    case Op::FunctionNamePrefix::Set:
        return "set"sv;
    }
    VERIFY_NOT_REACHED();
}

i64 asm_slow_path_has_private_id(VM* vm, u32 pc, Op::HasPrivateId const* instruction)
{
    auto base = vm->get(instruction->base());
    if (!base.is_object()) [[unlikely]] {
        auto completion = vm->throw_completion<TypeError>(ErrorType::InOperatorWithObject);
        return handle_asm_exception(*vm, pc, completion.value());
    }

    auto private_environment = vm->running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(vm->get_identifier(instruction->property()));
    vm->set(instruction->dst(), Value(base.as_object().private_element_find(private_name) != nullptr));
    return static_cast<i64>(pc + sizeof(Op::HasPrivateId));
}

i64 asm_slow_path_set_function_name(VM* vm, u32 pc, Op::SetFunctionName const* instruction)
{
    auto function = vm->get(instruction->function()).as_if<ECMAScriptFunctionObject>();
    if (!function || !function->name().is_empty())
        return static_cast<i64>(pc + sizeof(Op::SetFunctionName));

    auto property_key = ASM_TRY(*vm, pc, vm->get(instruction->name()).to_property_key(*vm));
    function->set_inferred_name(Variant<PropertyKey, PrivateName> { move(property_key) }, asm_function_name_prefix_to_string(instruction->prefix()));
    return static_cast<i64>(pc + sizeof(Op::SetFunctionName));
}

i64 asm_slow_path_new_array_with_length(VM* vm, u32 pc, Op::NewArrayWithLength const* instruction)
{
    auto length = static_cast<u64>(vm->get(instruction->array_length()).as_double());
    auto array = ASM_TRY(*vm, pc, JS::Array::create(vm->realm(), length));
    vm->set(instruction->dst(), array);
    return static_cast<i64>(pc + sizeof(Op::NewArrayWithLength));
}

i64 asm_slow_path_array_append(VM* vm, u32 pc, Op::ArrayAppend const* instruction)
{
    auto rhs = vm->get(instruction->src());
    auto& lhs_array = vm->get(instruction->dst()).as_array_exotic_object();
    auto lhs_size = lhs_array.indexed_array_like_size();

    if (instruction->is_spread()) {
        size_t i = lhs_size;
        auto result = get_iterator_values(*vm, rhs, [&i, &lhs_array](Value iterator_value) -> Optional<Completion> {
            lhs_array.indexed_put(i, iterator_value);
            ++i;
            return {};
        });
        if (result.is_error()) [[unlikely]]
            return handle_asm_exception(*vm, pc, result.value());
    } else {
        lhs_array.indexed_put(lhs_size, rhs);
    }

    return static_cast<i64>(pc + sizeof(Op::ArrayAppend));
}

i64 asm_slow_path_create_variable(VM* vm, u32 pc, Op::CreateVariable const* instruction)
{
    auto const& name = vm->get_identifier(instruction->identifier());
    ASM_TRY(*vm, pc, asm_create_variable(*vm, name, instruction->mode(), instruction->is_global(), instruction->is_immutable(), instruction->is_strict()));
    return static_cast<i64>(pc + sizeof(Op::CreateVariable));
}

i64 asm_slow_path_enter_object_environment(VM* vm, u32 pc, Op::EnterObjectEnvironment const* instruction)
{
    auto object = ASM_TRY(*vm, pc, vm->get(instruction->object()).to_object(*vm));
    auto& old_environment = vm->running_execution_context().lexical_environment;
    auto new_environment = new_object_environment(*object, true, old_environment);
    vm->set(instruction->dst(), new_environment);
    vm->running_execution_context().lexical_environment = new_environment;
    return static_cast<i64>(pc + sizeof(Op::EnterObjectEnvironment));
}

i64 asm_slow_path_bitwise_not(VM* vm, u32 pc, Op::BitwiseNot const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, bitwise_not(*vm, vm->get(instruction->src()))));
    return static_cast<i64>(pc + sizeof(Op::BitwiseNot));
}

i64 asm_slow_path_unary_plus(VM* vm, u32 pc, Op::UnaryPlus const* instruction)
{
    vm->set(instruction->dst(), ASM_TRY(*vm, pc, unary_plus(*vm, vm->get(instruction->src()))));
    return static_cast<i64>(pc + sizeof(Op::UnaryPlus));
}

i64 asm_slow_path_is_constructor(VM* vm, u32 pc, Op::IsConstructor const* instruction)
{
    vm->set(instruction->dst(), Value(vm->get(instruction->value()).is_constructor()));
    return static_cast<i64>(pc + sizeof(Op::IsConstructor));
}

i64 asm_slow_path_add_private_name(VM* vm, u32 pc, Op::AddPrivateName const* instruction)
{
    auto const& name = vm->get_identifier(instruction->name());
    vm->running_execution_context().private_environment->add_private_name(name);
    return static_cast<i64>(pc + sizeof(Op::AddPrivateName));
}

i64 asm_slow_path_create_async_from_sync_iterator(VM* vm, u32 pc, Op::CreateAsyncFromSyncIterator const* instruction)
{
    auto& realm = vm->realm();

    auto& iterator = vm->get(instruction->iterator()).as_object();
    auto next_method = vm->get(instruction->next_method());
    auto done = vm->get(instruction->done()).as_bool();

    auto iterator_record = realm.create<IteratorRecord>(iterator, next_method, done);
    auto async_from_sync_iterator = create_async_from_sync_iterator(*vm, iterator_record);

    auto iterator_object = Object::create(realm, nullptr);
    iterator_object->define_direct_property(vm->names.iterator, async_from_sync_iterator.iterator, default_attributes);
    iterator_object->define_direct_property(vm->names.nextMethod, async_from_sync_iterator.next_method, default_attributes);
    iterator_object->define_direct_property(vm->names.done, Value { async_from_sync_iterator.done }, default_attributes);

    vm->set(instruction->dst(), iterator_object);
    return static_cast<i64>(pc + sizeof(Op::CreateAsyncFromSyncIterator));
}

i64 asm_slow_path_create_data_property_or_throw(VM* vm, u32 pc, Op::CreateDataPropertyOrThrow const* instruction)
{
    auto& object = vm->get(instruction->object()).as_object();
    auto property = ASM_TRY(*vm, pc, vm->get(instruction->property()).to_property_key(*vm));
    auto value = vm->get(instruction->value());
    ASM_TRY(*vm, pc, object.create_data_property_or_throw(property, value));
    return static_cast<i64>(pc + sizeof(Op::CreateDataPropertyOrThrow));
}

i64 asm_slow_path_create_immutable_binding(VM* vm, u32 pc, Op::CreateImmutableBinding const* instruction)
{
    auto& environment = as<Environment>(vm->get(instruction->environment()).as_cell());
    ASM_TRY(*vm, pc, environment.create_immutable_binding(*vm, vm->get_identifier(instruction->identifier()), instruction->strict_binding()));
    return static_cast<i64>(pc + sizeof(Op::CreateImmutableBinding));
}

i64 asm_slow_path_create_mutable_binding(VM* vm, u32 pc, Op::CreateMutableBinding const* instruction)
{
    auto& environment = as<Environment>(vm->get(instruction->environment()).as_cell());
    ASM_TRY(*vm, pc, environment.create_mutable_binding(*vm, vm->get_identifier(instruction->identifier()), instruction->can_be_deleted()));
    return static_cast<i64>(pc + sizeof(Op::CreateMutableBinding));
}

i64 asm_slow_path_create_rest_params(VM* vm, u32 pc, Op::CreateRestParams const* instruction)
{
    auto const arguments = vm->running_execution_context().arguments_span();
    auto arguments_count = vm->running_execution_context().passed_argument_count;
    auto array = MUST(JS::Array::create(vm->realm(), 0));
    for (size_t rest_index = instruction->rest_index(); rest_index < arguments_count; ++rest_index)
        array->indexed_append(arguments[rest_index]);
    vm->set(instruction->dst(), array);
    return static_cast<i64>(pc + sizeof(Op::CreateRestParams));
}

i64 asm_slow_path_create_arguments(VM* vm, u32 pc, Op::CreateArguments const* instruction)
{
    auto const& function = vm->running_execution_context().function;
    auto const arguments = vm->running_execution_context().arguments_span();
    auto const& environment = vm->running_execution_context().lexical_environment;

    auto passed_arguments = ReadonlySpan<Value> { arguments.data(), vm->running_execution_context().passed_argument_count };
    Object* arguments_object;
    if (instruction->kind() == Op::ArgumentsKind::Mapped) {
        auto const& ecma_function = static_cast<ECMAScriptFunctionObject const&>(*function);
        arguments_object = create_mapped_arguments_object(*vm, *function, ecma_function.parameter_names_for_mapped_arguments(), passed_arguments, *environment);
    } else {
        arguments_object = create_unmapped_arguments_object(*vm, passed_arguments);
    }

    if (instruction->dst().has_value()) {
        vm->set(*instruction->dst(), arguments_object);
        return static_cast<i64>(pc + sizeof(Op::CreateArguments));
    }

    if (instruction->is_immutable()) {
        MUST(environment->create_immutable_binding(*vm, vm->names.arguments.as_string(), false));
    } else {
        MUST(environment->create_mutable_binding(*vm, vm->names.arguments.as_string(), false));
    }
    MUST(environment->initialize_binding(*vm, vm->names.arguments.as_string(), arguments_object, Environment::InitializeBindingHint::Normal));
    return static_cast<i64>(pc + sizeof(Op::CreateArguments));
}

i64 asm_slow_path_await(VM* vm, [[maybe_unused]] u32 pc, Op::Await const* instruction)
{
    auto yielded_value = vm->get(instruction->argument()).is_special_empty_value() ? js_undefined() : vm->get(instruction->argument());
    auto& context = vm->running_execution_context();
    context.yield_continuation = instruction->continuation_label().address();
    context.yield_is_await = true;
    context.yield_value_is_iterator_result = false;
    vm->do_return(yielded_value);
    return -1;
}

i64 asm_slow_path_create_lexical_environment(VM* vm, u32 pc, Op::CreateLexicalEnvironment const* instruction)
{
    auto& parent = as<Environment>(vm->get(instruction->parent()).as_cell());
    auto environment = new_declarative_environment(parent);
    environment->ensure_capacity(instruction->capacity());
    environment->set_is_catch_environment(instruction->is_catch_environment());
    vm->set(instruction->dst(), environment);
    vm->running_execution_context().lexical_environment = environment;
    return static_cast<i64>(pc + sizeof(Op::CreateLexicalEnvironment));
}

i64 asm_slow_path_create_private_environment(VM* vm, u32 pc, Op::CreatePrivateEnvironment const*)
{
    auto& running_execution_context = vm->running_execution_context();
    auto outer_private_environment = running_execution_context.private_environment;
    running_execution_context.private_environment = new_private_environment(*vm, outer_private_environment);
    return static_cast<i64>(pc + sizeof(Op::CreatePrivateEnvironment));
}

i64 asm_slow_path_create_variable_environment(VM* vm, u32 pc, Op::CreateVariableEnvironment const* instruction)
{
    auto& running_execution_context = vm->running_execution_context();
    auto var_environment = new_declarative_environment(*running_execution_context.lexical_environment);
    if (auto* shared_data = vm->active_shared_function_data(); shared_data && instruction->capacity() == shared_data->m_var_environment_bindings_count)
        var_environment->set_environment_shape_cache(shared_data->m_var_environment_shape, instruction->capacity());
    var_environment->ensure_capacity(instruction->capacity());
    running_execution_context.variable_environment = var_environment;
    running_execution_context.lexical_environment = var_environment;
    return static_cast<i64>(pc + sizeof(Op::CreateVariableEnvironment));
}

i64 asm_slow_path_delete_by_id(VM* vm, u32 pc, Op::DeleteById const* instruction)
{
    auto const& property_key = vm->get_property_key(instruction->property());
    auto reference = Reference { vm->get(instruction->base()), property_key, {}, instruction->strict() };
    auto result = ASM_TRY(*vm, pc, reference.delete_(*vm));
    vm->set(instruction->dst(), Value(result));
    return static_cast<i64>(pc + sizeof(Op::DeleteById));
}

i64 asm_slow_path_delete_by_value(VM* vm, u32 pc, Op::DeleteByValue const* instruction)
{
    auto property_key = ASM_TRY(*vm, pc, vm->get(instruction->property()).to_property_key(*vm));
    auto reference = Reference { vm->get(instruction->base()), property_key, {}, instruction->strict() };
    auto result = ASM_TRY(*vm, pc, reference.delete_(*vm));
    vm->set(instruction->dst(), Value(result));
    return static_cast<i64>(pc + sizeof(Op::DeleteByValue));
}

i64 asm_slow_path_delete_variable(VM* vm, u32 pc, Op::DeleteVariable const* instruction)
{
    auto const& string = vm->get_identifier(instruction->identifier());
    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(string, instruction->strict()));
    auto result = ASM_TRY(*vm, pc, reference.delete_(*vm));
    vm->set(instruction->dst(), Value(result));
    return static_cast<i64>(pc + sizeof(Op::DeleteVariable));
}

i64 asm_slow_path_get_completion_fields(VM* vm, u32 pc, Op::GetCompletionFields const* instruction)
{
    auto& completion_source = vm->get(instruction->completion()).as_object();
    if (is<GeneratorObject>(completion_source)) {
        auto const& generator = as<GeneratorObject>(completion_source);
        vm->set(instruction->value_dst(), generator.pending_completion_value());
        vm->set(instruction->type_dst(), Value(to_underlying(generator.pending_completion_type())));
        return static_cast<i64>(pc + sizeof(Op::GetCompletionFields));
    }

    auto const& async_generator = as<AsyncGenerator>(completion_source);
    vm->set(instruction->value_dst(), async_generator.pending_completion_value());
    vm->set(instruction->type_dst(), Value(to_underlying(async_generator.pending_completion_type())));
    return static_cast<i64>(pc + sizeof(Op::GetCompletionFields));
}

i64 asm_slow_path_set_completion_type(VM* vm, u32 pc, Op::SetCompletionType const* instruction)
{
    auto& completion_source = vm->get(instruction->completion()).as_object();
    if (is<GeneratorObject>(completion_source)) {
        as<GeneratorObject>(completion_source).set_pending_completion_type(instruction->completion_type());
        return static_cast<i64>(pc + sizeof(Op::SetCompletionType));
    }

    as<AsyncGenerator>(completion_source).set_pending_completion_type(instruction->completion_type());
    return static_cast<i64>(pc + sizeof(Op::SetCompletionType));
}

i64 asm_slow_path_get_template_object(VM* vm, u32 pc, Op::GetTemplateObject const* instruction)
{
    auto& cache = *vm->current_executable().template_object_caches[instruction->cache()];

    if (cache.cached_template_object) {
        vm->set(instruction->dst(), cache.cached_template_object);
        return static_cast<i64>(pc + instruction->length());
    }

    auto& realm = *vm->current_realm();
    auto strings = instruction->strings();
    u32 count = instruction->strings_count() / 2;
    auto template_object = MUST(JS::Array::create(realm, count));
    auto raw_object = MUST(JS::Array::create(realm, count));

    for (size_t index = 0; index < count; ++index) {
        template_object->indexed_put(index, vm->get(strings[index]), Attribute::Enumerable);
        raw_object->indexed_put(index, vm->get(strings[count + index]), Attribute::Enumerable);
    }

    MUST(raw_object->set_integrity_level(Object::IntegrityLevel::Frozen));
    template_object->define_direct_property(vm->names.raw, raw_object, PropertyAttributes {});
    MUST(template_object->set_integrity_level(Object::IntegrityLevel::Frozen));

    cache.cached_template_object = template_object;
    vm->set(instruction->dst(), template_object);
    return static_cast<i64>(pc + instruction->length());
}

i64 asm_slow_path_new_function(VM* vm, u32 pc, Op::NewFunction const* instruction)
{
    auto& shared_data = *vm->current_executable().shared_function_data[instruction->shared_function_data_index()];
    auto& realm = *vm->current_realm();

    GC::Ref<Object> prototype = [&]() -> GC::Ref<Object> {
        switch (shared_data.m_kind) {
        case FunctionKind::Normal:
            return realm.intrinsics().function_prototype();
        case FunctionKind::Generator:
            return realm.intrinsics().generator_function_prototype();
        case FunctionKind::Async:
            return realm.intrinsics().async_function_prototype();
        case FunctionKind::AsyncGenerator:
            return realm.intrinsics().async_generator_function_prototype();
        }
        VERIFY_NOT_REACHED();
    }();

    auto function = ECMAScriptFunctionObject::create_from_function_data(
        realm,
        shared_data,
        vm->lexical_environment(),
        vm->running_execution_context().private_environment,
        *prototype);

    if (instruction->home_object().has_value()) {
        auto home_object_value = vm->get(instruction->home_object().value());
        function->make_method(home_object_value.as_object());
    }

    vm->set(instruction->dst(), function);
    return static_cast<i64>(pc + sizeof(Op::NewFunction));
}

i64 asm_slow_path_throw(VM* vm, u32 pc, Op::Throw const* instruction)
{
    return handle_asm_exception(*vm, pc, vm->get(instruction->src()));
}

i64 asm_slow_path_throw_if_tdz(VM* vm, u32 pc, Op::ThrowIfTDZ const* instruction)
{
    auto value = vm->get(instruction->src());
    if (value.is_special_empty_value()) [[unlikely]] {
        auto completion = vm->throw_completion<ReferenceError>(ErrorType::BindingNotInitialized, value);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    return static_cast<i64>(pc + sizeof(Op::ThrowIfTDZ));
}

i64 asm_slow_path_throw_if_not_object(VM* vm, u32 pc, Op::ThrowIfNotObject const* instruction)
{
    auto src = vm->get(instruction->src());
    if (!src.is_object()) [[unlikely]] {
        auto completion = vm->throw_completion<TypeError>(ErrorType::NotAnObject, src);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    return static_cast<i64>(pc + sizeof(Op::ThrowIfNotObject));
}

i64 asm_slow_path_throw_if_nullish(VM* vm, u32 pc, Op::ThrowIfNullish const* instruction)
{
    auto value = vm->get(instruction->src());
    if (value.is_nullish()) [[unlikely]] {
        auto completion = vm->throw_completion<TypeError>(ErrorType::NotObjectCoercible, value);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    return static_cast<i64>(pc + sizeof(Op::ThrowIfNullish));
}

i64 asm_slow_path_throw_const_assignment(VM* vm, u32 pc, Op::ThrowConstAssignment const*)
{
    auto completion = vm->throw_completion<TypeError>(ErrorType::InvalidAssignToConst);
    return handle_asm_exception(*vm, pc, completion.value());
}

i64 asm_slow_path_yield(VM* vm, [[maybe_unused]] u32 pc, Op::Yield const* instruction)
{
    auto yielded_value = vm->get(instruction->value()).is_special_empty_value() ? js_undefined() : vm->get(instruction->value());
    auto& context = vm->running_execution_context();
    if (instruction->continuation_label().has_value())
        context.yield_continuation = instruction->continuation_label()->address();
    else
        context.yield_continuation = ExecutionContext::no_yield_continuation;
    context.yield_is_await = false;
    context.yield_value_is_iterator_result = false;
    vm->do_return(yielded_value);
    return -1;
}

i64 asm_slow_path_yield_iterator_result(VM* vm, [[maybe_unused]] u32 pc, Op::YieldIteratorResult const* instruction)
{
    auto yielded_value = vm->get(instruction->value()).is_special_empty_value() ? js_undefined() : vm->get(instruction->value());
    auto& context = vm->running_execution_context();
    context.yield_continuation = instruction->continuation_label().address();
    context.yield_is_await = false;
    context.yield_value_is_iterator_result = true;
    vm->do_return(yielded_value);
    return -1;
}

// Fast path for GetByValue on typed arrays.
// Returns 0 on success (result stored in dst), 1 on miss (fall to slow path).
i64 asm_try_get_by_value_typed_array(VM* vm, u32, Op::GetByValue const* instruction)
{

    auto base = vm->get(instruction->base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = vm->get(instruction->property());
    if (!property.is_non_negative_int32()) [[unlikely]]
        return 1;

    auto& object = base.as_object();
    if (!object.is_typed_array()) [[unlikely]]
        return 1;

    auto& typed_array = static_cast<TypedArrayBase&>(object);
    auto index = static_cast<u32>(property.as_i32());

    // Fast path: fixed-length typed array with cached data pointer
    auto const& array_length = typed_array.array_length();
    if (array_length.is_auto()) [[unlikely]]
        return 1;

    auto length = array_length.length();
    if (index >= length) [[unlikely]] {
        vm->set(instruction->dst(), js_undefined());
        return 0;
    }

    if (!is_valid_integer_index(typed_array, CanonicalIndex { CanonicalIndex::Type::Index, index })) [[unlikely]] {
        vm->set(instruction->dst(), js_undefined());
        return 0;
    }

    auto* buffer = typed_array.viewed_array_buffer();
    auto const* data = buffer->data() + typed_array.byte_offset();

    Value result;
    switch (typed_array.kind()) {
    case TypedArrayBase::Kind::Uint8Array:
    case TypedArrayBase::Kind::Uint8ClampedArray:
        result = Value(static_cast<i32>(data[index]));
        break;
    case TypedArrayBase::Kind::Int8Array:
        result = Value(static_cast<i32>(reinterpret_cast<i8 const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Uint16Array:
        result = Value(static_cast<i32>(reinterpret_cast<u16 const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Int16Array:
        result = Value(static_cast<i32>(reinterpret_cast<i16 const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Uint32Array:
        result = Value(static_cast<double>(reinterpret_cast<u32 const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Int32Array:
        result = Value(reinterpret_cast<i32 const*>(data)[index]);
        break;
    case TypedArrayBase::Kind::Float32Array:
        result = Value(static_cast<double>(reinterpret_cast<float const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Float64Array:
        result = Value(reinterpret_cast<double const*>(data)[index]);
        break;
    default:
        return 1;
    }

    vm->set(instruction->dst(), result);
    return 0;
}

// Fast path for PutByValue on typed arrays.
// Returns 0 on success, 1 on miss (fall to slow path).
i64 asm_try_put_by_value_typed_array(VM* vm, u32, Op::PutByValue const* instruction)
{

    auto base = vm->get(instruction->base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = vm->get(instruction->property());
    if (!property.is_non_negative_int32()) [[unlikely]]
        return 1;

    auto& object = base.as_object();
    if (!object.is_typed_array()) [[unlikely]]
        return 1;

    auto& typed_array = static_cast<TypedArrayBase&>(object);
    auto index = static_cast<u32>(property.as_i32());

    auto const& array_length = typed_array.array_length();
    if (array_length.is_auto()) [[unlikely]]
        return 1;

    // NB: An out-of-bounds write is not simply a no-op: TypedArraySetElement still
    //     evaluates ToNumber(value) for its side effects before discarding the store.
    //     Fall back to the slow path so those side effects happen.
    if (index >= array_length.length()) [[unlikely]]
        return 1;

    if (!is_valid_integer_index(typed_array, CanonicalIndex { CanonicalIndex::Type::Index, index })) [[unlikely]]
        return 1;

    auto* buffer = typed_array.viewed_array_buffer();
    auto* data = buffer->data() + typed_array.byte_offset();
    auto value = vm->get(instruction->src());

    if (value.is_int32()) {
        auto int_val = value.as_i32();
        switch (typed_array.kind()) {
        case TypedArrayBase::Kind::Uint8Array:
            data[index] = static_cast<u8>(int_val);
            return 0;
        case TypedArrayBase::Kind::Uint8ClampedArray:
            data[index] = static_cast<u8>(clamp(int_val, 0, 255));
            return 0;
        case TypedArrayBase::Kind::Int8Array:
            reinterpret_cast<i8*>(data)[index] = static_cast<i8>(int_val);
            return 0;
        case TypedArrayBase::Kind::Uint16Array:
            reinterpret_cast<u16*>(data)[index] = static_cast<u16>(int_val);
            return 0;
        case TypedArrayBase::Kind::Int16Array:
            reinterpret_cast<i16*>(data)[index] = static_cast<i16>(int_val);
            return 0;
        case TypedArrayBase::Kind::Uint32Array:
            reinterpret_cast<u32*>(data)[index] = static_cast<u32>(int_val);
            return 0;
        case TypedArrayBase::Kind::Int32Array:
            reinterpret_cast<i32*>(data)[index] = int_val;
            return 0;
        default:
            break;
        }
    } else if (value.is_double()) {
        auto dbl_val = value.as_double();
        switch (typed_array.kind()) {
        case TypedArrayBase::Kind::Float32Array:
            reinterpret_cast<float*>(data)[index] = static_cast<float>(dbl_val);
            return 0;
        case TypedArrayBase::Kind::Float64Array:
            reinterpret_cast<double*>(data)[index] = dbl_val;
            return 0;
        default:
            break;
        }
    }

    return 1;
}

i64 asm_slow_path_instance_of(VM* vm, u32 pc, Op::InstanceOf const* instruction)
{
    auto result = ASM_TRY(*vm, pc, instance_of(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs())));
    vm->set(instruction->dst(), result);
    return static_cast<i64>(pc + sizeof(Op::InstanceOf));
}

i64 asm_slow_path_in(VM* vm, u32 pc, Op::In const* instruction)
{
    auto result = ASM_TRY(*vm, pc, in(*vm, vm->get(instruction->lhs()), vm->get(instruction->rhs())));
    vm->set(instruction->dst(), result);
    return static_cast<i64>(pc + sizeof(Op::In));
}

i64 asm_slow_path_resolve_this_binding(VM* vm, u32 pc, Op::ResolveThisBinding const*)
{
    auto& cached_this_value = vm->reg(Register::this_value());
    if (!cached_this_value.is_special_empty_value())
        return static_cast<i64>(pc + sizeof(Op::ResolveThisBinding));

    auto& running_execution_context = vm->running_execution_context();
    if (auto function = running_execution_context.function; function && is<ECMAScriptFunctionObject>(*function)) {
        auto& ecmascript_function = static_cast<ECMAScriptFunctionObject&>(*function);
        if (!ecmascript_function.allocates_function_environment() && !ecmascript_function.this_value_needs_environment_resolution()) {
            cached_this_value = running_execution_context.this_value.value();
            return static_cast<i64>(pc + sizeof(Op::ResolveThisBinding));
        }
    }
    cached_this_value = ASM_TRY(*vm, pc, vm->resolve_this_binding());
    return static_cast<i64>(pc + sizeof(Op::ResolveThisBinding));
}

// Direct handler for GetPrivateById: bypasses Reference indirection.
i64 asm_slow_path_get_private_by_id(VM* vm, u32 pc, Op::GetPrivateById const* instruction)
{
    auto base_value = vm->get(instruction->base());
    auto& current_vm = *vm;

    if (!base_value.is_object()) [[unlikely]] {
        ASM_TRY(*vm, pc, base_value.to_object(current_vm));
        auto const& name = current_vm.get_identifier(instruction->property());
        auto private_name = make_private_reference(current_vm, base_value, name);
        auto result = ASM_TRY(*vm, pc, private_name.get_value(current_vm));
        vm->set(instruction->dst(), result);
        return static_cast<i64>(pc + sizeof(Op::GetPrivateById));
    }

    auto const& name = current_vm.get_identifier(instruction->property());
    auto private_environment = current_vm.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(name);
    auto result = ASM_TRY(*vm, pc, base_value.as_object().private_get(private_name));
    vm->set(instruction->dst(), result);
    return static_cast<i64>(pc + sizeof(Op::GetPrivateById));
}

// Direct handler for PutPrivateById: bypasses Reference indirection.
i64 asm_slow_path_put_private_by_id(VM* vm, u32 pc, Op::PutPrivateById const* instruction)
{
    auto base_value = vm->get(instruction->base());
    auto& current_vm = *vm;
    auto value = vm->get(instruction->src());

    if (!base_value.is_object()) [[unlikely]] {
        auto object = ASM_TRY(*vm, pc, base_value.to_object(current_vm));
        auto const& name = current_vm.get_identifier(instruction->property());
        auto private_reference = make_private_reference(current_vm, object, name);
        ASM_TRY(*vm, pc, private_reference.put_value(current_vm, value));
        return static_cast<i64>(pc + sizeof(Op::PutPrivateById));
    }

    auto const& name = current_vm.get_identifier(instruction->property());
    auto private_environment = current_vm.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(name);
    ASM_TRY(*vm, pc, base_value.as_object().private_set(private_name, value));
    return static_cast<i64>(pc + sizeof(Op::PutPrivateById));
}

// Helper: convert value to boolean (called from asm jump handlers)
// Returns 0 (false) or 1 (true). Never throws.
u64 asm_helper_to_boolean(u64 encoded_value)
{
    auto value = bit_cast<Value>(encoded_value);
    return value.to_boolean() ? 1 : 0;
}

u64 asm_helper_math_exp(u64 encoded_value)
{
    auto value = bit_cast<Value>(encoded_value);
    return bit_cast<u64>(Value(::exp(value.as_double())));
}

u64 asm_helper_empty_string(u64)
{
    return bit_cast<u64>(Value(&VM::the().empty_string()));
}

i64 asm_helper_handle_raw_native_exception(u64 encoded_exception)
{
    auto& vm = VM::the();
    auto& callee_frame = vm.running_execution_context();
    VERIFY(callee_frame.caller_frame);

    // Raw-native asm calls keep their callee frame off the VM execution
    // context stack, so we have to unwind it manually before exception
    // dispatch. Match VM::handle_exception()'s inline-frame semantics by
    // probing the caller with a PC inside the Call instruction.
    auto caller_pc = callee_frame.caller_return_pc;
    vm.unwind_inline_frame_for_exception();
    return handle_asm_exception(vm, caller_pc - 1, bit_cast<Value>(encoded_exception));
}

u64 asm_helper_single_ascii_character_string(u64 encoded_value)
{
    return bit_cast<u64>(Value(&VM::the().single_ascii_character_string(static_cast<u8>(encoded_value))));
}

u64 asm_helper_single_utf16_code_unit_string(u64 encoded_value)
{
    char16_t code_unit = static_cast<char16_t>(encoded_value);
    return bit_cast<u64>(Value(PrimitiveString::create(VM::the(), Utf16View(&code_unit, 1))));
}

} // extern "C"
