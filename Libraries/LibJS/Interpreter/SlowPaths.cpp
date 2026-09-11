/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/ScopeGuard.h>
#include <AK/Types.h>
#include <LibJS/Bytecode/Builtins.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/PropertyAccess.h>
#include <LibJS/Bytecode/PropertyNameIterator.h>
#include <LibJS/Debugger.h>
#include <LibJS/Interpreter/SlowPathResult.h>
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
#include <LibJS/Runtime/NativeFunction.h>
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
// The caller supplies scalar operands directly, or uses an Op::Values record for
// variable-length and multiple-output operations. Windows always uses a record.
// The first required output is returned in a register where the ABI supports it.
// Input/output operands are also copied back on exceptions if the frame remains active.
// Output-only fields in caller-provided records are uninitialized on entry.
// Control result >= 0: new PC in the low word; bit 32 marks same-frame continuation.
// Control result < 0: exit the interpreter.

using namespace JS;
using namespace JS::Bytecode;

#define DEFINE_SLOW_PATH(name, op) JS_DEFINE_SLOW_PATH_##op(name)
#define DECLARE_SLOW_PATH(name, op) JS_DECLARE_SLOW_PATH_##op(name)

#ifndef AK_OS_WINDOWS
// The System V and AArch64 ABIs return these two words in integer registers.
struct AsmSlowPathResult {
    i64 control;
    u64 value;
};

template<typename Values>
static ALWAYS_INLINE AsmSlowPathResult make_asm_slow_path_result(i64 control, Values const& values)
{
    if constexpr (requires { values.primary_output(); }) {
        return { control, values.primary_output().encoded() };
    }
    return { control, 0 };
}
#endif

#define DEFINE_RECORD_SLOW_PATH(name, OpType) \
    i64 name([[maybe_unused]] VM* vm, [[maybe_unused]] u32 pc, [[maybe_unused]] OpType const* instruction, [[maybe_unused]] OpType::Values& values)

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
    return continue_after_slow_path(pc + sizeof(Op));
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
static i64 asm_get_binding(VM& vm, u32 pc, Value& dst, EnvironmentCoordinate const& cache)
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
    dst = value;
    return static_cast<i64>(pc);
}

template<AsmBindingIsKnownToBeInitialized binding_is_known_to_be_initialized>
static i64 asm_dynamic_get_binding(VM& vm, u32 pc, Value& dst, IdentifierTableIndex identifier_index, Strict strict, EnvironmentCoordinate& cache)
{
    auto const* current_environment = vm.running_execution_context().lexical_environment.ptr();
    if (auto const* cached_environment = asm_get_cached_environment(current_environment, cache)) [[likely]] {
        Value value;
        if constexpr (binding_is_known_to_be_initialized == AsmBindingIsKnownToBeInitialized::No) {
            value = ASM_TRY(vm, pc, static_cast<DeclarativeEnvironment const&>(*cached_environment).get_binding_value_direct(vm, cache.index));
        } else {
            value = static_cast<DeclarativeEnvironment const&>(*cached_environment).get_initialized_binding_value_direct(cache.index);
        }
        dst = value;
        return static_cast<i64>(pc);
    }

    auto& executable = vm.current_executable();
    auto reference = ASM_TRY(vm, pc, vm.resolve_binding(executable.get_identifier(identifier_index), strict));
    asm_update_environment_coordinate_cache(current_environment, reference, cache);

    dst = ASM_TRY(vm, pc, reference.get_value(vm));
    return static_cast<i64>(pc);
}

static i64 asm_dynamic_get_callee_and_this_from_environment(VM& vm, u32 pc, Value& callee_dst, Value& this_value_dst, IdentifierTableIndex identifier_index, Strict strict, EnvironmentCoordinate& cache)
{
    auto const* current_environment = vm.running_execution_context().lexical_environment.ptr();
    if (auto const* cached_environment = asm_get_cached_environment(current_environment, cache)) [[likely]] {
        auto callee = ASM_TRY(vm, pc, static_cast<DeclarativeEnvironment const&>(*cached_environment).get_binding_value_direct(vm, cache.index));
        callee_dst = callee;
        this_value_dst = js_undefined();
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

    callee_dst = callee;
    this_value_dst = this_value;
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
            return vm.throw_completion<InternalError>(Utf16String::formatted("Lexical environment already has binding '{}'", name));

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
    if (&shape != cache.shape().ptr())
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

static i64 finish_binary_slow_path_value(VM& vm, u32 pc, Value& destination, Value result)
{
    destination = result;
    auto const* instruction = bit_cast<Instruction const*>(vm.current_executable().bytecode.data() + pc);
    return continue_after_slow_path(pc + instruction->length());
}

template<typename Result>
static i64 finish_binary_slow_path(VM& vm, u32 pc, Value& destination, ThrowCompletionOr<Result> result)
{
    return finish_binary_slow_path_value(vm, pc, destination, Value { ASM_TRY(vm, pc, move(result)) });
}

extern "C" {

i64 asm_slow_path_stack_overflow(VM*, u32);

i64 asm_slow_path_stack_overflow(VM* vm, u32 pc)
{
    return handle_asm_exception(*vm, pc, vm->throw_completion<InternalError>(ErrorType::CallStackSizeExceeded).value());
}

// Forward declarations for all functions called from assembly.
void asm_debugger_check_breakpoint(VM*, u32 pc);
i64 asm_fallback_handler(VM*, u32 pc, u8 const* instruction);
i64 asm_slow_path_jump_less_than_values(VM*, u32 pc, Value, Value, u32, u32);
i64 asm_slow_path_jump_greater_than_values(VM*, u32 pc, Value, Value, u32, u32);
i64 asm_slow_path_jump_less_than_equals_values(VM*, u32 pc, Value, Value, u32, u32);
i64 asm_slow_path_jump_greater_than_equals_values(VM*, u32 pc, Value, Value, u32, u32);
i64 asm_slow_path_jump_loosely_equals_values(VM*, u32 pc, Value, Value, u32, u32);
DECLARE_SLOW_PATH(asm_slow_path_create_private_environment, CreatePrivateEnvironment);
DECLARE_SLOW_PATH(asm_slow_path_throw_const_assignment, ThrowConstAssignment);
DECLARE_SLOW_PATH(asm_slow_path_resolve_this_binding, ResolveThisBinding);
#define DECLARE_CALL_BUILTIN_SLOW_PATH(name, snake_case_name, ...) \
    DECLARE_SLOW_PATH(asm_slow_path_call_builtin_##snake_case_name, CallBuiltin##name);
JS_ENUMERATE_BUILTINS(DECLARE_CALL_BUILTIN_SLOW_PATH)
#undef DECLARE_CALL_BUILTIN_SLOW_PATH
DECLARE_SLOW_PATH(asm_slow_path_add, Add);
DECLARE_SLOW_PATH(asm_slow_path_sub, Sub);
i64 asm_slow_path_add_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_sub_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_mul_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_div_values(VM*, u32 pc, Value&, Value, Value);
DECLARE_SLOW_PATH(asm_slow_path_less_than, LessThan);
DECLARE_SLOW_PATH(asm_slow_path_less_than_equals, LessThanEquals);
DECLARE_SLOW_PATH(asm_slow_path_greater_than, GreaterThan);
DECLARE_SLOW_PATH(asm_slow_path_greater_than_equals, GreaterThanEquals);
i64 asm_slow_path_less_than_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_less_than_equals_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_greater_than_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_greater_than_equals_values(VM*, u32 pc, Value&, Value, Value);
DECLARE_SLOW_PATH(asm_slow_path_increment, Increment);
DECLARE_SLOW_PATH(asm_slow_path_decrement, Decrement);
i64 asm_slow_path_jump_loosely_inequals_values(VM*, u32 pc, Value, Value, u32, u32);
i64 asm_slow_path_jump_strictly_equals_values(VM*, u32 pc, Value, Value, u32, u32);
i64 asm_slow_path_jump_strictly_inequals_values(VM*, u32 pc, Value, Value, u32, u32);
DECLARE_SLOW_PATH(asm_slow_path_get_initialized_binding, GetInitializedBinding);
DECLARE_SLOW_PATH(asm_slow_path_dynamic_get_initialized_binding, DynamicGetInitializedBinding);
DECLARE_SLOW_PATH(asm_slow_path_get_callee_and_this, GetCalleeAndThisFromEnvironment);
DECLARE_SLOW_PATH(asm_slow_path_dynamic_get_callee_and_this, DynamicGetCalleeAndThisFromEnvironment);
DECLARE_SLOW_PATH(asm_slow_path_postfix_increment, PostfixIncrement);
DECLARE_SLOW_PATH(asm_slow_path_get_by_id, GetById);
DECLARE_SLOW_PATH(asm_slow_path_get_by_id_cached_accessor, GetById);
DECLARE_SLOW_PATH(asm_slow_path_get_by_id_with_this, GetByIdWithThis);
DECLARE_SLOW_PATH(asm_slow_path_put_by_id, PutById);
DECLARE_SLOW_PATH(asm_slow_path_put_by_id_with_this, PutByIdWithThis);
DECLARE_SLOW_PATH(asm_slow_path_get_by_value, GetByValue);
DECLARE_SLOW_PATH(asm_slow_path_get_by_value_with_this, GetByValueWithThis);
DECLARE_SLOW_PATH(asm_slow_path_get_length, GetLength);
DECLARE_SLOW_PATH(asm_slow_path_get_length_with_this, GetLengthWithThis);
DECLARE_SLOW_PATH(asm_slow_path_get_method, GetMethod);
DECLARE_SLOW_PATH(asm_slow_path_get_iterator, GetIterator);
DECLARE_SLOW_PATH(asm_slow_path_get_import_meta, GetImportMeta);
DECLARE_SLOW_PATH(asm_slow_path_get_new_target, GetNewTarget);
DECLARE_SLOW_PATH(asm_slow_path_get_super_constructor, GetSuperConstructor);
DECLARE_SLOW_PATH(asm_slow_path_get_global, GetGlobal);
DECLARE_SLOW_PATH(asm_slow_path_set_global, SetGlobal);
DECLARE_SLOW_PATH(asm_slow_path_concat_string, ConcatString);
DECLARE_SLOW_PATH(asm_slow_path_copy_object_excluding_properties, CopyObjectExcludingProperties);
i64 asm_slow_path_exp_values(VM*, u32 pc, Value&, Value, Value);
DECLARE_SLOW_PATH(asm_slow_path_import_call, ImportCall);
DECLARE_SLOW_PATH(asm_slow_path_new_class, NewClass);
DECLARE_SLOW_PATH(asm_slow_path_call, Call);
DECLARE_SLOW_PATH(asm_slow_path_call_direct_eval, CallDirectEval);
DECLARE_SLOW_PATH(asm_slow_path_call_with_argument_array, CallWithArgumentArray);
DECLARE_SLOW_PATH(asm_slow_path_call_direct_eval_with_argument_array, CallDirectEvalWithArgumentArray);
DECLARE_SLOW_PATH(asm_slow_path_get_object_property_iterator, GetObjectPropertyIterator);
DECLARE_SLOW_PATH(asm_slow_path_object_property_iterator_next, ObjectPropertyIteratorNext);
DECLARE_SLOW_PATH(asm_slow_path_iterator_close, IteratorClose);
DECLARE_SLOW_PATH(asm_slow_path_iterator_next, IteratorNext);
DECLARE_SLOW_PATH(asm_slow_path_iterator_next_unpack, IteratorNextUnpack);
DECLARE_SLOW_PATH(asm_slow_path_iterator_to_array, IteratorToArray);
DECLARE_SLOW_PATH(asm_slow_path_call_construct, CallConstruct);
DECLARE_SLOW_PATH(asm_slow_path_call_construct_with_argument_array, CallConstructWithArgumentArray);
DECLARE_SLOW_PATH(asm_slow_path_super_call_with_argument_array, SuperCallWithArgumentArray);
DECLARE_SLOW_PATH(asm_slow_path_new_object, NewObject);
DECLARE_SLOW_PATH(asm_slow_path_new_object_with_no_prototype, NewObjectWithNoPrototype);
DECLARE_SLOW_PATH(asm_slow_path_cache_object_shape, CacheObjectShape);
DECLARE_SLOW_PATH(asm_slow_path_init_object_literal_property, InitObjectLiteralProperty);
DECLARE_SLOW_PATH(asm_slow_path_new_array, NewArray);
DECLARE_SLOW_PATH(asm_slow_path_new_primitive_array, NewPrimitiveArray);
DECLARE_SLOW_PATH(asm_slow_path_new_regexp, NewRegExp);
DECLARE_SLOW_PATH(asm_slow_path_new_reference_error, NewReferenceError);
DECLARE_SLOW_PATH(asm_slow_path_new_type_error, NewTypeError);
DECLARE_SLOW_PATH(asm_slow_path_bitwise_xor, BitwiseXor);
DECLARE_SLOW_PATH(asm_slow_path_bitwise_and, BitwiseAnd);
DECLARE_SLOW_PATH(asm_slow_path_bitwise_or, BitwiseOr);
i64 asm_slow_path_bitwise_xor_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_bitwise_and_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_bitwise_or_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_left_shift_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_right_shift_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_unsigned_right_shift_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_mod_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_strictly_equals_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_strictly_inequals_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_loosely_equals_values(VM*, u32 pc, Value&, Value, Value);
i64 asm_slow_path_loosely_inequals_values(VM*, u32 pc, Value&, Value, Value);
DECLARE_SLOW_PATH(asm_slow_path_unary_minus, UnaryMinus);
DECLARE_SLOW_PATH(asm_slow_path_to_string, ToString);
DECLARE_SLOW_PATH(asm_slow_path_to_primitive_with_string_hint, ToPrimitiveWithStringHint);
DECLARE_SLOW_PATH(asm_slow_path_to_object, ToObject);
DECLARE_SLOW_PATH(asm_slow_path_to_length, ToLength);
DECLARE_SLOW_PATH(asm_slow_path_typeof, Typeof);
DECLARE_SLOW_PATH(asm_slow_path_postfix_decrement, PostfixDecrement);
DECLARE_SLOW_PATH(asm_slow_path_to_int32, ToInt32);
DECLARE_SLOW_PATH(asm_slow_path_put_by_value, PutByValue);
DECLARE_SLOW_PATH(asm_slow_path_put_by_value_with_this, PutByValueWithThis);
DECLARE_SLOW_PATH(asm_slow_path_put_by_spread, PutBySpread);
DECLARE_SLOW_PATH(asm_slow_path_get_binding, GetBinding);
DECLARE_SLOW_PATH(asm_slow_path_dynamic_get_binding, DynamicGetBinding);
DECLARE_SLOW_PATH(asm_slow_path_initialize_lexical_binding, InitializeLexicalBinding);
DECLARE_SLOW_PATH(asm_slow_path_dynamic_initialize_lexical_binding, DynamicInitializeLexicalBinding);
DECLARE_SLOW_PATH(asm_slow_path_initialize_variable_binding, InitializeVariableBinding);
DECLARE_SLOW_PATH(asm_slow_path_dynamic_initialize_variable_binding, DynamicInitializeVariableBinding);
DECLARE_SLOW_PATH(asm_slow_path_set_lexical_binding, SetLexicalBinding);
DECLARE_SLOW_PATH(asm_slow_path_dynamic_set_lexical_binding, DynamicSetLexicalBinding);
DECLARE_SLOW_PATH(asm_slow_path_set_variable_binding, SetVariableBinding);
DECLARE_SLOW_PATH(asm_slow_path_dynamic_set_variable_binding, DynamicSetVariableBinding);
DECLARE_SLOW_PATH(asm_slow_path_resolve_binding, ResolveBinding);
DECLARE_SLOW_PATH(asm_slow_path_resolve_super_base, ResolveSuperBase);
DECLARE_SLOW_PATH(asm_slow_path_set_resolved_binding, SetResolvedBinding);
DECLARE_SLOW_PATH(asm_slow_path_typeof_binding, TypeofBinding);
DECLARE_SLOW_PATH(asm_slow_path_dynamic_typeof_binding, DynamicTypeofBinding);
DECLARE_SLOW_PATH(asm_slow_path_has_private_id, HasPrivateId);
DECLARE_SLOW_PATH(asm_slow_path_set_function_name, SetFunctionName);
DECLARE_SLOW_PATH(asm_slow_path_new_array_with_length, NewArrayWithLength);
DECLARE_SLOW_PATH(asm_slow_path_array_append, ArrayAppend);
DECLARE_SLOW_PATH(asm_slow_path_create_variable, CreateVariable);
DECLARE_SLOW_PATH(asm_slow_path_enter_object_environment, EnterObjectEnvironment);
DECLARE_SLOW_PATH(asm_slow_path_bitwise_not, BitwiseNot);
DECLARE_SLOW_PATH(asm_slow_path_unary_plus, UnaryPlus);
DECLARE_SLOW_PATH(asm_slow_path_is_constructor, IsConstructor);
DECLARE_SLOW_PATH(asm_slow_path_add_private_name, AddPrivateName);
DECLARE_SLOW_PATH(asm_slow_path_create_async_from_sync_iterator, CreateAsyncFromSyncIterator);
DECLARE_SLOW_PATH(asm_slow_path_create_data_property_or_throw, CreateDataPropertyOrThrow);
DECLARE_SLOW_PATH(asm_slow_path_create_immutable_binding, CreateImmutableBinding);
DECLARE_SLOW_PATH(asm_slow_path_create_mutable_binding, CreateMutableBinding);
DECLARE_SLOW_PATH(asm_slow_path_create_rest_params, CreateRestParams);
DECLARE_SLOW_PATH(asm_slow_path_create_arguments, CreateArguments);
DECLARE_SLOW_PATH(asm_slow_path_await, Await);
DECLARE_SLOW_PATH(asm_slow_path_create_lexical_environment, CreateLexicalEnvironment);
DECLARE_SLOW_PATH(asm_slow_path_create_variable_environment, CreateVariableEnvironment);
DECLARE_SLOW_PATH(asm_slow_path_delete_by_id, DeleteById);
DECLARE_SLOW_PATH(asm_slow_path_delete_by_value, DeleteByValue);
DECLARE_SLOW_PATH(asm_slow_path_delete_variable, DeleteVariable);
DECLARE_SLOW_PATH(asm_slow_path_get_completion_fields, GetCompletionFields);
DECLARE_SLOW_PATH(asm_slow_path_set_completion_type, SetCompletionType);
DECLARE_SLOW_PATH(asm_slow_path_get_template_object, GetTemplateObject);
DECLARE_SLOW_PATH(asm_slow_path_new_function, NewFunction);
DECLARE_SLOW_PATH(asm_slow_path_throw, Throw);
DECLARE_SLOW_PATH(asm_slow_path_throw_if_tdz, ThrowIfTDZ);
DECLARE_SLOW_PATH(asm_slow_path_throw_if_not_object, ThrowIfNotObject);
DECLARE_SLOW_PATH(asm_slow_path_throw_if_nullish, ThrowIfNullish);
DECLARE_SLOW_PATH(asm_slow_path_debugger, Debugger);
DECLARE_SLOW_PATH(asm_slow_path_yield, Yield);
DECLARE_SLOW_PATH(asm_slow_path_yield_iterator_result, YieldIteratorResult);
DECLARE_SLOW_PATH(asm_slow_path_instance_of, InstanceOf);
DECLARE_SLOW_PATH(asm_slow_path_in, In);
DECLARE_SLOW_PATH(asm_slow_path_get_private_by_id, GetPrivateById);
DECLARE_SLOW_PATH(asm_slow_path_put_private_by_id, PutPrivateById);

i64 asm_try_get_global_env_binding(VM*, u32 pc, Op::GetGlobal const*, Op::GetGlobal::Values& values);
i64 asm_try_set_global_env_binding(VM*, u32 pc, Op::SetGlobal const*, Op::SetGlobal::Values& values);
i64 asm_try_put_by_value_holey_array(VM*, u32 pc, Op::PutByValue const*, Op::PutByValue::Values& values);
u64 asm_helper_to_boolean(u64 encoded_value);
u64 asm_helper_math_exp(u64 encoded_value);
u64 asm_helper_empty_string(u64);
u64 asm_helper_single_ascii_character_string(u64 encoded_value);
u64 asm_helper_single_utf16_code_unit_string(u64 encoded_value);
i64 asm_helper_handle_raw_native_exception(u64 encoded_exception);
i64 asm_try_inline_call(VM*, u32 pc, Op::Call const*, Op::Call::Values& values);
i64 asm_try_inline_get_by_id_accessor(VM*, u32 pc, Op::GetById const*, Op::GetById::Values& values);
i64 asm_try_put_by_id_cache(VM*, u32 pc, Op::PutById const*, Op::PutById::Values& values);
u64 asm_try_get_by_id_cache(u64, PropertyLookupCache*);

i64 asm_try_get_by_value_typed_array(VM*, u32 pc, Op::GetByValue const*, Op::GetByValue::Values& values);
i64 asm_try_put_by_value_typed_array(VM*, u32 pc, Op::PutByValue const*, Op::PutByValue::Values& values);

// ===== Fallback handler for invalid dispatch table entries =====
// NB: Every bytecode opcode has a DSL handler, so this should never run.
void asm_debugger_check_breakpoint(VM* vm, u32 pc)
{
    // NB: The dispatch table is chosen when entering the interpreter, so we keep getting called
    //     for the rest of the frame even if the host detaches its debugger in the meantime.
    auto* debugger = vm->debugger();
    if (!debugger)
        return;

    // NB: Debugger callbacks must not inspect slots before Enter initializes them.
    if (!vm->running_execution_context().frame_initialized)
        return;

    auto& executable = vm->current_executable();
    debugger->register_executable(executable);
    auto reason = [&]() -> Optional<Debugger::PauseReason> {
        if (debugger->should_pause_on_next_bytecode_execution(executable, pc))
            return Debugger::PauseReason::Entry;
        if (executable.has_debugger_breakpoint_at(pc))
            return Debugger::PauseReason::Breakpoint;
        if (debugger->should_pause_for_step(executable, pc))
            return Debugger::PauseReason::Step;
        return {};
    }();

    bool did_pause = false;
    if (reason.has_value())
        did_pause = debugger->pause_execution(executable, pc, *reason);
    debugger->set_did_pause_before_current_instruction(did_pause);
}

i64 asm_fallback_handler(VM*, u32, u8 const*)
{
    VERIFY_NOT_REACHED();
}

// ===== Specific slow paths for asm-optimized instructions =====
// These are called from asm handlers when the fast path fails.
// The implementation bodies use named values; the generated wrappers adapt these
// to each helper's native calling convention.

i64 asm_slow_path_add_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, add(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_add, Add)
{
    return asm_slow_path_add_values(vm, pc, values.dst, values.lhs, values.rhs);
}

i64 asm_slow_path_sub_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, sub(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_sub, Sub)
{
    return asm_slow_path_sub_values(vm, pc, values.dst, values.lhs, values.rhs);
}

i64 asm_slow_path_mul_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, mul(*vm, lhs, rhs));
}

i64 asm_slow_path_div_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, div(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_less_than, LessThan)
{
    return asm_slow_path_less_than_values(vm, pc, values.dst, values.lhs, values.rhs);
}

i64 asm_slow_path_less_than_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, less_than(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_less_than_equals, LessThanEquals)
{
    return asm_slow_path_less_than_equals_values(vm, pc, values.dst, values.lhs, values.rhs);
}

i64 asm_slow_path_less_than_equals_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, less_than_equals(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_greater_than, GreaterThan)
{
    return asm_slow_path_greater_than_values(vm, pc, values.dst, values.lhs, values.rhs);
}

i64 asm_slow_path_greater_than_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, greater_than(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_greater_than_equals, GreaterThanEquals)
{
    return asm_slow_path_greater_than_equals_values(vm, pc, values.dst, values.lhs, values.rhs);
}

i64 asm_slow_path_greater_than_equals_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, greater_than_equals(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_increment, Increment)
{
    auto old_value = ASM_TRY(*vm, pc, values.dst.to_numeric(*vm));
    if (old_value.is_number())
        values.dst = Value(old_value.as_double() + 1);
    else
        values.dst = BigInt::create(*vm, old_value.as_bigint().big_integer().plus(Crypto::SignedBigInteger { 1 }));
    return continue_after_slow_path(pc + sizeof(Op::Increment));
}

DEFINE_SLOW_PATH(asm_slow_path_decrement, Decrement)
{
    auto old_value = ASM_TRY(*vm, pc, values.dst.to_numeric(*vm));
    if (old_value.is_number())
        values.dst = Value(old_value.as_double() - 1);
    else
        values.dst = BigInt::create(*vm, old_value.as_bigint().big_integer().minus(Crypto::SignedBigInteger { 1 }));
    return continue_after_slow_path(pc + sizeof(Op::Decrement));
}

// Comparison jump slow paths return one of two target PCs.
#define DEFINE_JUMP_COMPARISON_SLOW_PATH(snake_name, compare_call)               \
    i64 asm_slow_path_jump_##snake_name##_values(                                \
        VM* vm, u32 pc, Value lhs, Value rhs, u32 true_target, u32 false_target) \
    {                                                                            \
        if (ASM_TRY(*vm, pc, compare_call))                                      \
            return static_cast<i64>(true_target);                                \
        return static_cast<i64>(false_target);                                   \
    }

DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than, less_than(*vm, lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than, greater_than(*vm, lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than_equals, less_than_equals(*vm, lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than_equals, greater_than_equals(*vm, lhs, rhs))
#undef DEFINE_JUMP_COMPARISON_SLOW_PATH

i64 asm_slow_path_jump_loosely_equals_values(VM* vm, u32 pc, Value lhs, Value rhs, u32 true_target, u32 false_target)
{
    if (ASM_TRY(*vm, pc, is_loosely_equal(*vm, lhs, rhs)))
        return static_cast<i64>(true_target);
    return static_cast<i64>(false_target);
}

i64 asm_slow_path_jump_loosely_inequals_values(VM* vm, u32 pc, Value lhs, Value rhs, u32 true_target, u32 false_target)
{
    if (!ASM_TRY(*vm, pc, is_loosely_equal(*vm, lhs, rhs)))
        return static_cast<i64>(true_target);
    return static_cast<i64>(false_target);
}

i64 asm_slow_path_jump_strictly_equals_values(
    [[maybe_unused]] VM* vm, [[maybe_unused]] u32 pc, Value lhs, Value rhs, u32 true_target, u32 false_target)
{
    if (is_strictly_equal(lhs, rhs))
        return static_cast<i64>(true_target);
    return static_cast<i64>(false_target);
}

i64 asm_slow_path_jump_strictly_inequals_values(
    [[maybe_unused]] VM* vm, [[maybe_unused]] u32 pc, Value lhs, Value rhs, u32 true_target, u32 false_target)
{
    if (!is_strictly_equal(lhs, rhs))
        return static_cast<i64>(true_target);
    return static_cast<i64>(false_target);
}

// ===== Dedicated slow paths for hot instructions =====

DEFINE_SLOW_PATH(asm_slow_path_get_initialized_binding, GetInitializedBinding)
{
    auto next_pc = asm_get_binding<AsmBindingIsKnownToBeInitialized::Yes>(*vm, pc, values.dst, instruction->cache());
    return advance_or_continue<Op::GetInitializedBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_dynamic_get_initialized_binding, DynamicGetInitializedBinding)
{
    auto& cache = vm->current_executable().environment_coordinate_caches[instruction->cache()];
    auto next_pc = asm_dynamic_get_binding<AsmBindingIsKnownToBeInitialized::Yes>(*vm, pc, values.dst, instruction->identifier(), instruction->strict(), cache);
    return advance_or_continue<Op::DynamicGetInitializedBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_get_callee_and_this, GetCalleeAndThisFromEnvironment)
{
    auto const& cache = instruction->cache();
    VERIFY(cache.is_valid());

    auto const* environment = vm->running_execution_context().lexical_environment.ptr();
    for (size_t i = 0; i < cache.hops; ++i)
        environment = environment->outer_environment();

    auto callee = ASM_TRY(*vm, pc, static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(*vm, cache.index));
    values.callee = callee;
    auto this_value = js_undefined();
    if (auto base_object = environment->with_base_object()) [[unlikely]]
        this_value = base_object;
    values.this_value = this_value;
    return continue_after_slow_path(pc + sizeof(Op::GetCalleeAndThisFromEnvironment));
}

DEFINE_SLOW_PATH(asm_slow_path_dynamic_get_callee_and_this, DynamicGetCalleeAndThisFromEnvironment)
{
    auto& cache = vm->current_executable().environment_coordinate_caches[instruction->cache()];
    auto next_pc = asm_dynamic_get_callee_and_this_from_environment(*vm, pc, values.callee, values.this_value, instruction->identifier(), instruction->strict(), cache);
    return advance_or_continue<Op::DynamicGetCalleeAndThisFromEnvironment>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_postfix_increment, PostfixIncrement)
{
    auto old_value = ASM_TRY(*vm, pc, values.src.to_numeric(*vm));
    values.dst = old_value;
    if (old_value.is_number())
        values.src = Value(old_value.as_double() + 1);
    else
        values.src = BigInt::create(*vm, old_value.as_bigint().big_integer().plus(Crypto::SignedBigInteger { 1 }));
    return continue_after_slow_path(pc + sizeof(Op::PostfixIncrement));
}

DEFINE_SLOW_PATH(asm_slow_path_get_by_id, GetById)
{
    auto base_value = values.base;
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Normal>(*vm, [&] { return vm->get_identifier(instruction->base_identifier()); }, [&] -> PropertyKey const& { return vm->get_property_key(instruction->property()); }, base_value, base_value, cache, CachePropertyAbsence::Yes));
    values.dst = value;
    return continue_after_slow_path(pc + sizeof(Op::GetById));
}

DEFINE_SLOW_PATH(asm_slow_path_get_by_id_cached_accessor, GetById)
{
    auto& object = values.base.as_object();
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    auto* entry = cache.first_entry();
    VERIFY(entry);

    auto* holder = entry->prototype ? entry->prototype.ptr() : &object;
    auto value = holder->get_direct(entry->property_offset);
    VERIFY(value.is_accessor());
    auto* getter = value.as_accessor().getter();
    auto result = ASM_TRY(*vm, pc, get_cached_property_value(*vm, value, &object));
    if (getter && is<DirectGetterFunction>(*getter)) {
        if (auto* completed_entry = cache.first_entry(); completed_entry && completed_entry->shape.ptr() == &object.shape()) {
            auto* completed_holder = completed_entry->prototype ? completed_entry->prototype.ptr() : &object;
            auto completed_value = completed_holder->get_direct(completed_entry->property_offset);
            if (completed_value.is_accessor() && completed_value.as_accessor().getter() == getter)
                completed_entry->direct_getter_validated = true;
        }
    }
    values.dst = result;
    return continue_after_slow_path(pc + sizeof(Op::GetById));
}

DEFINE_SLOW_PATH(asm_slow_path_get_by_id_with_this, GetByIdWithThis)
{
    auto base_value = values.base;
    auto this_value = values.this_value;
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Normal>(*vm, [] { return Optional<Utf16FlyString const&> {}; }, [&] -> PropertyKey const& { return vm->get_property_key(instruction->property()); }, base_value, this_value, cache));
    values.dst = value;
    return continue_after_slow_path(pc + sizeof(Op::GetByIdWithThis));
}

DEFINE_SLOW_PATH(asm_slow_path_put_by_id, PutById)
{
    auto value = values.src;
    auto base = values.base;
    Optional<Utf16FlyString const&> base_identifier;
    if (instruction->base_identifier().has_value())
        base_identifier = vm->get_identifier(instruction->base_identifier().value());
    auto const& property_key = vm->get_property_key(instruction->property());
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, base, value, base_identifier, property_key, instruction->kind(), instruction->strict(), &cache));
    return continue_after_slow_path(pc + sizeof(Op::PutById));
}

DEFINE_SLOW_PATH(asm_slow_path_put_by_id_with_this, PutByIdWithThis)
{
    auto value = values.src;
    auto base = values.base;
    auto const& name = vm->get_property_key(instruction->property());
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, values.this_value, value, {}, name, instruction->kind(), instruction->strict(), &cache));
    return continue_after_slow_path(pc + sizeof(Op::PutByIdWithThis));
}

DEFINE_SLOW_PATH(asm_slow_path_get_by_value, GetByValue)
{
    auto base_value = values.base;
    auto property_key_value = values.property;
    auto object = ASM_TRY(*vm, pc, base_object_for_get(*vm, base_value, [&]() -> Optional<Utf16FlyString const&> {
        if (instruction->base_identifier().has_value())
            return vm->get_identifier(instruction->base_identifier().value());
        return {}; }, [&] { return property_key_value; }));
    auto property_key = ASM_TRY(*vm, pc, property_key_value.to_property_key(*vm));
    if (base_value.is_string()) {
        auto string_value = ASM_TRY(*vm, pc, base_value.as_string().get(*vm, property_key));
        if (string_value.has_value()) {
            values.dst = *string_value;
            return continue_after_slow_path(pc + sizeof(Op::GetByValue));
        }
    }
    values.dst = ASM_TRY(*vm, pc, get_by_value_with_keyed_cache(*vm, *object, base_value, property_key));
    return continue_after_slow_path(pc + sizeof(Op::GetByValue));
}

DEFINE_SLOW_PATH(asm_slow_path_get_by_value_with_this, GetByValueWithThis)
{
    auto property_key_value = values.property;
    auto object = ASM_TRY(*vm, pc, values.base.to_object(*vm));
    auto property_key = ASM_TRY(*vm, pc, property_key_value.to_property_key(*vm));
    auto value = ASM_TRY(*vm, pc, get_by_value_with_keyed_cache(*vm, *object, values.this_value, property_key));
    values.dst = value;
    return continue_after_slow_path(pc + sizeof(Op::GetByValueWithThis));
}

DEFINE_SLOW_PATH(asm_slow_path_get_length, GetLength)
{
    auto base_value = values.base;
    auto& executable = vm->current_executable();
    auto& cache = executable.property_lookup_caches[instruction->cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Length>(*vm, [&] { return vm->get_identifier(instruction->base_identifier()); }, [&] -> PropertyKey const& { return executable.get_property_key(*executable.length_identifier); }, base_value, base_value, cache));
    values.dst = value;
    return continue_after_slow_path(pc + sizeof(Op::GetLength));
}

DEFINE_SLOW_PATH(asm_slow_path_get_length_with_this, GetLengthWithThis)
{
    auto base_value = values.base;
    auto this_value = values.this_value;
    auto& executable = vm->current_executable();
    auto& cache = executable.property_lookup_caches[instruction->cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Length>(*vm, [] { return Optional<Utf16FlyString const&> {}; }, [&] -> PropertyKey const& { return executable.get_property_key(*executable.length_identifier); }, base_value, this_value, cache));
    values.dst = value;
    return continue_after_slow_path(pc + sizeof(Op::GetLengthWithThis));
}

DEFINE_SLOW_PATH(asm_slow_path_get_method, GetMethod)
{
    auto const& property_key = vm->get_property_key(instruction->property());
    auto method = ASM_TRY(*vm, pc, values.object.get_method(*vm, property_key));
    values.dst = method ?: js_undefined();
    return continue_after_slow_path(pc + sizeof(Op::GetMethod));
}

DEFINE_SLOW_PATH(asm_slow_path_get_iterator, GetIterator)
{
    auto iterator_record = ASM_TRY(*vm, pc, get_iterator_impl(*vm, values.iterable, instruction->hint()));
    values.dst_iterator_object = iterator_record.iterator;
    values.dst_iterator_next = iterator_record.next_method;
    values.dst_iterator_done = Value(iterator_record.done);
    return continue_after_slow_path(pc + sizeof(Op::GetIterator));
}

DEFINE_SLOW_PATH(asm_slow_path_get_import_meta, GetImportMeta)
{
    values.dst = vm->get_import_meta();
    return continue_after_slow_path(pc + sizeof(Op::GetImportMeta));
}

DEFINE_SLOW_PATH(asm_slow_path_get_new_target, GetNewTarget)
{
    values.dst = vm->get_new_target();
    return continue_after_slow_path(pc + sizeof(Op::GetNewTarget));
}

DEFINE_SLOW_PATH(asm_slow_path_get_super_constructor, GetSuperConstructor)
{
    auto* super_constructor = get_super_constructor(*vm);
    values.dst = super_constructor ? Value(super_constructor) : js_null();
    return continue_after_slow_path(pc + sizeof(Op::GetSuperConstructor));
}

i64 asm_try_get_global_env_binding(VM* vm, u32, Op::GetGlobal const* instruction, Op::GetGlobal::Values& values)
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
    values.dst = result.value();
    return 0;
}

DEFINE_SLOW_PATH(asm_slow_path_get_global, GetGlobal)
{
    auto& binding_object = vm->global_object();
    auto& declarative_record = vm->global_declarative_environment();
    auto& cache = vm->current_executable().global_variable_caches[instruction->cache()];

    auto& shape = binding_object.shape();
    if (cache.environment_serial_number == declarative_record.environment_serial_number()) {
        auto* entry = cache.first_entry();
        if (entry && &shape == entry->shape.ptr() && (!shape.is_dictionary() || shape.dictionary_generation() == entry->shape_dictionary_generation)) {
            auto value = binding_object.get_direct(entry->property_offset);
            values.dst = ASM_TRY(*vm, pc, get_cached_property_value(*vm, value, &binding_object));
            return continue_after_slow_path(pc + sizeof(Op::GetGlobal));
        }

        if (cache.has_environment_binding_index) {
            Value value;
            if (cache.in_module_environment) {
                auto module = vm->running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
                value = ASM_TRY(*vm, pc, (*module)->environment()->get_binding_value_direct(*vm, cache.environment_binding_index));
            } else {
                value = ASM_TRY(*vm, pc, declarative_record.get_binding_value_direct(*vm, cache.environment_binding_index));
            }
            values.dst = value;
            return continue_after_slow_path(pc + sizeof(Op::GetGlobal));
        }
    }

    cache = {};
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
                values.dst = ASM_TRY(*vm, pc, module_environment.get_binding_value_direct(*vm, index.value()));
                return continue_after_slow_path(pc + sizeof(Op::GetGlobal));
            }
            values.dst = ASM_TRY(*vm, pc, module_environment.get_binding_value(*vm, identifier, true));
            return continue_after_slow_path(pc + sizeof(Op::GetGlobal));
        }
    }

    Optional<size_t> offset;
    if (ASM_TRY(*vm, pc, declarative_record.has_binding(identifier, &offset))) {
        cache.environment_binding_index = static_cast<u32>(offset.value());
        cache.has_environment_binding_index = true;
        cache.in_module_environment = false;
        values.dst = ASM_TRY(*vm, pc, declarative_record.get_binding_value(*vm, identifier, instruction->strict() == Strict::Yes));
        return continue_after_slow_path(pc + sizeof(Op::GetGlobal));
    }

    if (ASM_TRY(*vm, pc, binding_object.has_property(identifier))) [[likely]] {
        auto dictionary_generation = shape.dictionary_generation();
        CacheableGetPropertyMetadata cacheable_metadata;
        auto value = ASM_TRY(*vm, pc, binding_object.internal_get(identifier, &binding_object, &cacheable_metadata));
        if (cacheable_metadata.type == CacheableGetPropertyMetadata::Type::GetOwnProperty
            && &shape == &binding_object.shape() && shape.dictionary_generation() == dictionary_generation) {
            cache.update(PropertyLookupCache::Entry::Type::GetOwnProperty, [&](auto& entry) {
                entry.shape = shape;
                entry.property_offset = cacheable_metadata.property_offset.value();

                if (shape.is_dictionary())
                    entry.shape_dictionary_generation = shape.dictionary_generation();
            });
        }
        values.dst = value;
        return continue_after_slow_path(pc + sizeof(Op::GetGlobal));
    }

    auto completion = vm->throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, identifier);
    return handle_asm_exception(*vm, pc, completion.value());
}

i64 asm_try_set_global_env_binding(VM* vm, u32, Op::SetGlobal const* instruction, Op::SetGlobal::Values& values)
{
    auto& cache = vm->current_executable().global_variable_caches[instruction->cache()];

    if (!cache.has_environment_binding_index) [[unlikely]]
        return 1;

    auto& current_vm = *vm;
    auto src = values.src;
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

DEFINE_SLOW_PATH(asm_slow_path_set_global, SetGlobal)
{
    auto& binding_object = vm->global_object();
    auto& declarative_record = vm->global_declarative_environment();
    auto& cache = vm->current_executable().global_variable_caches[instruction->cache()];
    auto& shape = binding_object.shape();
    auto src = values.src;

    if (cache.environment_serial_number == declarative_record.environment_serial_number()) {
        auto* entry = cache.first_entry();
        if (entry && &shape == entry->shape.ptr() && (!shape.is_dictionary() || shape.dictionary_generation() == entry->shape_dictionary_generation)) {
            auto value = binding_object.get_direct(entry->property_offset);
            if (value.is_accessor())
                ASM_TRY(*vm, pc, call(*vm, value.as_accessor().setter(), &binding_object, src));
            else
                binding_object.put_direct(entry->property_offset, src);
            return continue_after_slow_path(pc + sizeof(Op::SetGlobal));
        }

        if (cache.has_environment_binding_index) {
            if (cache.in_module_environment) {
                auto module = vm->running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
                ASM_TRY(*vm, pc, (*module)->environment()->set_mutable_binding_direct(*vm, cache.environment_binding_index, src, instruction->strict() == Strict::Yes));
            } else {
                ASM_TRY(*vm, pc, declarative_record.set_mutable_binding_direct(*vm, cache.environment_binding_index, src, instruction->strict() == Strict::Yes));
            }
            return continue_after_slow_path(pc + sizeof(Op::SetGlobal));
        }
    }

    cache = {};
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
                return continue_after_slow_path(pc + sizeof(Op::SetGlobal));
            }
            ASM_TRY(*vm, pc, module_environment.set_mutable_binding(*vm, identifier, src, instruction->strict() == Strict::Yes));
            return continue_after_slow_path(pc + sizeof(Op::SetGlobal));
        }
    }

    Optional<size_t> offset;
    if (ASM_TRY(*vm, pc, declarative_record.has_binding(identifier, &offset))) {
        cache.environment_binding_index = static_cast<u32>(offset.value());
        cache.has_environment_binding_index = true;
        cache.in_module_environment = false;
        ASM_TRY(*vm, pc, declarative_record.set_mutable_binding(*vm, identifier, src, instruction->strict() == Strict::Yes));
        return continue_after_slow_path(pc + sizeof(Op::SetGlobal));
    }

    if (ASM_TRY(*vm, pc, binding_object.has_property(identifier))) {
        auto dictionary_generation = shape.dictionary_generation();
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
        if (cacheable_metadata.type == CacheableSetPropertyMetadata::Type::ChangeOwnProperty
            && &shape == &binding_object.shape() && shape.dictionary_generation() == dictionary_generation) {
            cache.update(PropertyLookupCache::Entry::Type::ChangeOwnProperty, [&](auto& entry) {
                entry.shape = shape;
                entry.property_offset = cacheable_metadata.property_offset.value();

                if (shape.is_dictionary())
                    entry.shape_dictionary_generation = shape.dictionary_generation();
            });
        }
        return continue_after_slow_path(pc + sizeof(Op::SetGlobal));
    }

    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(identifier, instruction->strict(), &declarative_record));
    ASM_TRY(*vm, pc, reference.put_value(*vm, src));
    return continue_after_slow_path(pc + sizeof(Op::SetGlobal));
}

DEFINE_SLOW_PATH(asm_slow_path_concat_string, ConcatString)
{
    auto string = ASM_TRY(*vm, pc, values.src.to_primitive_string(*vm));
    values.dst = ASM_TRY(*vm, pc, PrimitiveString::create(*vm, values.dst.as_string(), string));
    return continue_after_slow_path(pc + sizeof(Op::ConcatString));
}

DEFINE_SLOW_PATH(asm_slow_path_copy_object_excluding_properties, CopyObjectExcludingProperties)
{
    auto& realm = *vm->current_realm();
    auto from_object = values.from_object;
    auto to_object = Object::create(realm, realm.intrinsics().object_prototype().ptr());

    GC::ConservativeHashTable<PropertyKey> excluded_names;
    for (size_t i = 0; i < instruction->excluded_names_count(); ++i)
        excluded_names.set(ASM_TRY(*vm, pc, values.excluded_names[i].to_property_key(*vm)));

    ASM_TRY(*vm, pc, to_object->copy_data_properties(*vm, from_object, excluded_names));
    values.dst = to_object;
    return continue_after_slow_path(pc + instruction->length());
}

i64 asm_slow_path_exp_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, exp(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_import_call, ImportCall)
{
    auto specifier = values.specifier;
    auto options_value = values.options;
    values.dst = ASM_TRY(*vm, pc, perform_import_call(*vm, specifier, options_value));
    return continue_after_slow_path(pc + sizeof(Op::ImportCall));
}

DEFINE_SLOW_PATH(asm_slow_path_new_class, NewClass)
{
    Value super_class;
    if (instruction->super_class().has_value())
        super_class = values.super_class;
    GC::RootVector<Value> element_keys;
    element_keys.ensure_capacity(instruction->element_keys_count());
    for (size_t i = 0; i < instruction->element_keys_count(); ++i) {
        Value element_key;
        if (instruction->element_keys()[i].has_value())
            element_key = values.element_keys[i];
        element_keys.unchecked_append(element_key);
    }

    auto& running_execution_context = vm->running_execution_context();
    auto* class_environment = &as<Environment>(values.class_environment.as_cell());
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

    auto retval = ASM_TRY(*vm, pc, construct_class(*vm, blueprint, vm->current_executable(), class_environment, outer_environment, super_class, element_keys, binding_name, class_name));
    values.dst = retval;
    return continue_after_slow_path(pc + instruction->length());
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
    ReadonlySpan<Value> arguments,
    Value& dst,
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
        callee_context_argument_values[i] = arguments[i];
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
    dst = retval;
    return {};
}

DEFINE_SLOW_PATH(asm_slow_path_call, Call)
{
    ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, values.callee, values.this_value, ReadonlySpan<Value> { values.arguments, instruction->argument_count() }, values.dst, instruction->expression_string(), instruction->strict()));
    return continue_after_slow_path(pc + instruction->length());
}

static ThrowCompletionOr<void> call_direct_eval(
    VM& vm,
    Value callee,
    Value this_value,
    ReadonlySpan<Value> arguments,
    Value& dst,
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
        callee_context_argument_values[i] = arguments[i];
    for (size_t i = insn_argument_count; i < callee_context_argument_count; ++i)
        callee_context_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    Value retval;
    if (callee == vm.realm().intrinsics().eval_function()) {
        retval = TRY(perform_eval(vm, callee_context->argument_count > 0 ? callee_context->arguments_data()[0] : js_undefined(), strict == Strict::Yes ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
    } else {
        retval = TRY(function.internal_call(*callee_context, this_value));
    }
    dst = retval;
    return {};
}

DEFINE_SLOW_PATH(asm_slow_path_call_direct_eval, CallDirectEval)
{
    ASM_TRY(*vm, pc, call_direct_eval(*vm, values.callee, values.this_value, ReadonlySpan<Value> { values.arguments, instruction->argument_count() }, values.dst, instruction->expression_string(), instruction->strict()));
    return continue_after_slow_path(pc + instruction->length());
}

static ThrowCompletionOr<void> call_with_argument_array(
    Op::CallType call_type,
    VM& vm,
    Value callee,
    Value this_value,
    Value arguments,
    Value& dst,
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

    dst = retval;
    return {};
}

DEFINE_SLOW_PATH(asm_slow_path_call_with_argument_array, CallWithArgumentArray)
{
    ASM_TRY(*vm, pc, call_with_argument_array(Op::CallType::Call, *vm, values.callee, values.this_value, values.arguments, values.dst, instruction->expression_string(), instruction->strict()));
    return continue_after_slow_path(pc + sizeof(Op::CallWithArgumentArray));
}

DEFINE_SLOW_PATH(asm_slow_path_call_direct_eval_with_argument_array, CallDirectEvalWithArgumentArray)
{
    ASM_TRY(*vm, pc, call_with_argument_array(Op::CallType::DirectEval, *vm, values.callee, values.this_value, values.arguments, values.dst, instruction->expression_string(), instruction->strict()));
    return continue_after_slow_path(pc + sizeof(Op::CallDirectEvalWithArgumentArray));
}

DEFINE_SLOW_PATH(asm_slow_path_get_object_property_iterator, GetObjectPropertyIterator)
{
    auto* cache = &vm->current_executable().object_property_iterator_caches[instruction->cache()];
    values.dst_iterator = ASM_TRY(*vm, pc, asm_get_object_property_iterator(*vm, values.object, cache));
    return continue_after_slow_path(pc + sizeof(Op::GetObjectPropertyIterator));
}

DEFINE_SLOW_PATH(asm_slow_path_object_property_iterator_next, ObjectPropertyIteratorNext)
{
    auto& iterator = static_cast<PropertyNameIterator&>(values.iterator_object.as_object());
    Value value;
    bool done = false;
    ASM_TRY(*vm, pc, iterator.next(*vm, done, value));
    values.dst_done = Value(done);
    values.dst_value = value;
    return continue_after_slow_path(pc + sizeof(Op::ObjectPropertyIteratorNext));
}

DEFINE_SLOW_PATH(asm_slow_path_iterator_close, IteratorClose)
{
    auto& iterator_object = values.iterator_object.as_object();
    auto iterator_next_method = values.iterator_next;
    auto iterator_done_property = values.iterator_done.as_bool();
    IteratorRecordImpl iterator_record { .done = iterator_done_property, .iterator = iterator_object, .next_method = iterator_next_method };

    ASM_TRY(*vm, pc, iterator_close(*vm, iterator_record, Completion { instruction->completion_type(), values.completion_value }));
    return continue_after_slow_path(pc + sizeof(Op::IteratorClose));
}

DEFINE_SLOW_PATH(asm_slow_path_iterator_next, IteratorNext)
{
    auto& iterator_object = values.iterator_object.as_object();
    auto iterator_next_method = values.iterator_next;
    auto iterator_done_property = values.iterator_done.as_bool();
    IteratorRecordImpl iterator_record { .done = iterator_done_property, .iterator = iterator_object, .next_method = iterator_next_method };
    auto result = iterator_next(*vm, iterator_record);
    if (iterator_record.done)
        values.iterator_done = Value(true);
    values.dst = ASM_TRY(*vm, pc, result);
    return continue_after_slow_path(pc + sizeof(Op::IteratorNext));
}

DEFINE_SLOW_PATH(asm_slow_path_iterator_next_unpack, IteratorNextUnpack)
{
    auto& iterator_object = values.iterator_object.as_object();
    auto iterator_next_method = values.iterator_next;
    auto iterator_done_property = values.iterator_done.as_bool();
    IteratorRecordImpl iterator_record { .done = iterator_done_property, .iterator = iterator_object, .next_method = iterator_next_method };
    auto iteration_result_or_done_or_error = iterator_step(*vm, iterator_record);
    if (iterator_record.done)
        values.iterator_done = Value(true);
    auto iteration_result_or_done = ASM_TRY(*vm, pc, iteration_result_or_done_or_error);
    if (iteration_result_or_done.has<IterationDone>()) {
        values.dst_value = js_undefined();
        values.dst_done = Value(true);
        return continue_after_slow_path(pc + sizeof(Op::IteratorNextUnpack));
    }
    auto& iteration_result = iteration_result_or_done.get<IterationResult>();
    values.dst_done = ASM_TRY(*vm, pc, iteration_result.done);
    auto value = move(iteration_result.value);
    if (value.is_throw_completion())
        values.iterator_done = Value(true);
    values.dst_value = ASM_TRY(*vm, pc, value);
    return continue_after_slow_path(pc + sizeof(Op::IteratorNextUnpack));
}

DEFINE_SLOW_PATH(asm_slow_path_iterator_to_array, IteratorToArray)
{
    IteratorRecordImpl iterator_record {
        .done = values.iterator_done_property.as_bool(),
        .iterator = values.iterator_object.as_object(),
        .next_method = values.iterator_next_method
    };

    auto array = MUST(JS::Array::create(*vm->current_realm(), 0));
    size_t index = 0;
    while (true) {
        auto value_or_error = iterator_step_value(*vm, iterator_record);
        if (iterator_record.done)
            values.iterator_done_property = Value(true);
        auto value = ASM_TRY(*vm, pc, value_or_error);
        if (!value.has_value()) {
            values.dst = array;
            return continue_after_slow_path(pc + sizeof(Op::IteratorToArray));
        }

        MUST(array->create_data_property_or_throw(index, value.release_value()));
        ++index;
    }
}

#define JS_DEFINE_UNARY_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, implementation)                                                                                           \
    DEFINE_SLOW_PATH(asm_slow_path_call_builtin_##snake_case_name, CallBuiltin##name)                                                                                           \
    {                                                                                                                                                                           \
        Value arguments[] { values.argument };                                                                                                                                  \
        auto callee = values.callee;                                                                                                                                            \
        if (callee.is_function() && callee.as_function().builtin() == Builtin::name) {                                                                                          \
            values.dst = ASM_TRY(*vm, pc, implementation(*vm, values.argument));                                                                                                \
            return continue_after_slow_path(pc + sizeof(Op::CallBuiltin##name));                                                                                                \
        }                                                                                                                                                                       \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, callee, values.this_value, arguments, values.dst, instruction->expression_string(), instruction->strict())); \
        return continue_after_slow_path(pc + sizeof(Op::CallBuiltin##name));                                                                                                    \
    }

#define JS_DEFINE_BINARY_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, implementation)                                                                                          \
    DEFINE_SLOW_PATH(asm_slow_path_call_builtin_##snake_case_name, CallBuiltin##name)                                                                                           \
    {                                                                                                                                                                           \
        Value arguments[] { values.argument0, values.argument1 };                                                                                                               \
        auto callee = values.callee;                                                                                                                                            \
        if (callee.is_function() && callee.as_function().builtin() == Builtin::name) {                                                                                          \
            values.dst = ASM_TRY(*vm, pc, implementation(*vm, values.argument0, values.argument1));                                                                             \
            return continue_after_slow_path(pc + sizeof(Op::CallBuiltin##name));                                                                                                \
        }                                                                                                                                                                       \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, callee, values.this_value, arguments, values.dst, instruction->expression_string(), instruction->strict())); \
        return continue_after_slow_path(pc + sizeof(Op::CallBuiltin##name));                                                                                                    \
    }

#define JS_DEFINE_NULLARY_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, implementation)                                                                                  \
    DEFINE_SLOW_PATH(asm_slow_path_call_builtin_##snake_case_name, CallBuiltin##name)                                                                                    \
    {                                                                                                                                                                    \
        auto callee = values.callee;                                                                                                                                     \
        if (callee.is_function() && callee.as_function().builtin() == Builtin::name) {                                                                                   \
            values.dst = implementation();                                                                                                                               \
            return continue_after_slow_path(pc + sizeof(Op::CallBuiltin##name));                                                                                         \
        }                                                                                                                                                                \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, callee, values.this_value, {}, values.dst, instruction->expression_string(), instruction->strict())); \
        return continue_after_slow_path(pc + sizeof(Op::CallBuiltin##name));                                                                                             \
    }

#define JS_DEFINE_GENERIC_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, ...)                                                                                                    \
    DEFINE_SLOW_PATH(asm_slow_path_call_builtin_##snake_case_name, CallBuiltin##name)                                                                                           \
    {                                                                                                                                                                           \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, values.callee, values.this_value, {}, values.dst, instruction->expression_string(), instruction->strict())); \
        return continue_after_slow_path(pc + sizeof(Op::CallBuiltin##name));                                                                                                    \
    }

#define JS_DEFINE_UNARY_GENERIC_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, ...)                                                                                                     \
    DEFINE_SLOW_PATH(asm_slow_path_call_builtin_##snake_case_name, CallBuiltin##name)                                                                                                  \
    {                                                                                                                                                                                  \
        Value arguments[] { values.argument };                                                                                                                                         \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, values.callee, values.this_value, arguments, values.dst, instruction->expression_string(), instruction->strict())); \
        return continue_after_slow_path(pc + sizeof(Op::CallBuiltin##name));                                                                                                           \
    }

#define JS_DEFINE_BINARY_GENERIC_BUILTIN_CALL_SLOW_PATH(name, snake_case_name, ...)                                                                                                    \
    DEFINE_SLOW_PATH(asm_slow_path_call_builtin_##snake_case_name, CallBuiltin##name)                                                                                                  \
    {                                                                                                                                                                                  \
        Value arguments[] { values.argument0, values.argument1 };                                                                                                                      \
        ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Call, *vm, values.callee, values.this_value, arguments, values.dst, instruction->expression_string(), instruction->strict())); \
        return continue_after_slow_path(pc + sizeof(Op::CallBuiltin##name));                                                                                                           \
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

DEFINE_SLOW_PATH(asm_slow_path_call_construct, CallConstruct)
{
    auto callee = values.callee;
    if (callee.is_object() && is<ECMAScriptFunctionObject>(callee.as_object())) {
        auto& function = static_cast<ECMAScriptFunctionObject&>(callee.as_object());
        if (function.can_inline_call() && callee.is_constructor() && function.constructor_kind() == ConstructorKind::Base && !function.has_class_data()) {
            auto* prototype = ASM_TRY(*vm, pc, get_prototype_from_constructor(*vm, function, &Intrinsics::object_prototype));
            auto this_object = Object::create(*function.realm(), prototype);
            auto* context = vm->push_inline_frame(function, function.inline_call_executable(), ReadonlySpan<Value> { values.arguments, instruction->argument_count() }, pc + instruction->length(), instruction->dst().raw(), this_object, &function, true);
            if (!context) [[unlikely]] {
                ASM_TRY(*vm, pc, vm->throw_completion<InternalError>(ErrorType::CallStackSizeExceeded));
                VERIFY_NOT_REACHED();
            }
            // Constructors retain their receiver even when the body never reads this.
            context->this_value = this_object;
            return 0;
        }
    }
    ASM_TRY(*vm, pc, execute_asm_call(Op::CallType::Construct, *vm, values.callee, js_undefined(), ReadonlySpan<Value> { values.arguments, instruction->argument_count() }, values.dst, instruction->expression_string(), instruction->strict()));
    return continue_after_slow_path(pc + instruction->length());
}

DEFINE_SLOW_PATH(asm_slow_path_call_construct_with_argument_array, CallConstructWithArgumentArray)
{
    ASM_TRY(*vm, pc, call_with_argument_array(Op::CallType::Construct, *vm, values.callee, js_undefined(), values.arguments, values.dst, instruction->expression_string(), instruction->strict()));
    return continue_after_slow_path(pc + sizeof(Op::CallConstructWithArgumentArray));
}

DEFINE_SLOW_PATH(asm_slow_path_super_call_with_argument_array, SuperCallWithArgumentArray)
{
    auto new_target = vm->get_new_target();
    VERIFY(new_target.is_object());

    auto super_constructor = values.super_constructor;
    if (!super_constructor.is_constructor()) [[unlikely]] {
        vm->running_execution_context().program_counter = pc;
        auto completion = vm->throw_completion<TypeError>(ErrorType::NotAConstructor, "Super constructor");
        return handle_asm_exception(*vm, pc, completion.value());
    }

    auto& function = super_constructor.as_function();

    auto& argument_array = values.arguments.as_array_exotic_object();
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

    values.dst = result;
    return continue_after_slow_path(pc + sizeof(Op::SuperCallWithArgumentArray));
}

DEFINE_SLOW_PATH(asm_slow_path_new_object, NewObject)
{
    auto& realm = *vm->current_realm();

    if (instruction->cache() != NumericLimits<u32>::max()) {
        auto& cache = vm->current_executable().object_shape_caches[instruction->cache()];
        auto cached_shape = cache.shape.ptr();
        if (cached_shape) {
            values.dst = Object::create_with_premade_shape(*cached_shape);
            return continue_after_slow_path(pc + sizeof(Op::NewObject));
        }
    }

    values.dst = Object::create(realm, realm.intrinsics().object_prototype().ptr());
    return continue_after_slow_path(pc + sizeof(Op::NewObject));
}

DEFINE_SLOW_PATH(asm_slow_path_new_object_with_no_prototype, NewObjectWithNoPrototype)
{
    auto& realm = *vm->current_realm();
    values.dst = Object::create(realm, nullptr);
    return continue_after_slow_path(pc + sizeof(Op::NewObjectWithNoPrototype));
}

DEFINE_SLOW_PATH(asm_slow_path_cache_object_shape, CacheObjectShape)
{
    auto& cache = vm->current_executable().object_shape_caches[instruction->cache()];
    if (!cache.shape) {
        auto& object = values.object.as_object();
        if (!object.shape().is_dictionary())
            cache.shape = &object.shape();
    }
    return continue_after_slow_path(pc + sizeof(Op::CacheObjectShape));
}

DEFINE_SLOW_PATH(asm_slow_path_init_object_literal_property, InitObjectLiteralProperty)
{
    auto& object = values.object.as_object();
    auto value = values.src;
    auto& cache = vm->current_executable().object_shape_caches[instruction->shape_cache_index()];

    auto cached_shape = cache.shape.ptr();
    if (cached_shape && &object.shape() == cached_shape && instruction->property_slot() < cache.property_offsets.size()) {
        object.put_direct(cache.property_offsets[instruction->property_slot()], value);
        return continue_after_slow_path(pc + sizeof(Op::InitObjectLiteralProperty));
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

    return continue_after_slow_path(pc + sizeof(Op::InitObjectLiteralProperty));
}

DEFINE_SLOW_PATH(asm_slow_path_new_array, NewArray)
{
    auto array = MUST(JS::Array::create(vm->realm(), instruction->element_count()));
    for (size_t i = 0; i < instruction->element_count(); ++i)
        array->indexed_put(i, values.elements[i]);
    values.dst = array;
    return continue_after_slow_path(pc + instruction->length());
}

DEFINE_SLOW_PATH(asm_slow_path_new_primitive_array, NewPrimitiveArray)
{
    auto array = MUST(JS::Array::create(vm->realm(), instruction->element_count()));
    for (size_t i = 0; i < instruction->element_count(); ++i)
        array->indexed_put(i, instruction->elements()[i]);
    values.dst = array;
    return continue_after_slow_path(pc + instruction->length());
}

DEFINE_SLOW_PATH(asm_slow_path_new_regexp, NewRegExp)
{
    auto& realm = *vm->current_realm();
    auto regexp_object = RegExpObject::create(
        realm,
        vm->current_executable().get_string(instruction->source_index()),
        vm->current_executable().get_string(instruction->flags_index()));
    regexp_object->set_realm(realm);
    regexp_object->set_legacy_features_enabled(true);
    values.dst = regexp_object;
    return continue_after_slow_path(pc + sizeof(Op::NewRegExp));
}

DEFINE_SLOW_PATH(asm_slow_path_new_reference_error, NewReferenceError)
{
    auto& realm = *vm->current_realm();
    values.dst = ReferenceError::create(realm, vm->current_executable().get_string(instruction->error_string()));
    return continue_after_slow_path(pc + sizeof(Op::NewReferenceError));
}

DEFINE_SLOW_PATH(asm_slow_path_new_type_error, NewTypeError)
{
    auto& realm = *vm->current_realm();
    values.dst = TypeError::create(realm, vm->current_executable().get_string(instruction->error_string()));
    return continue_after_slow_path(pc + sizeof(Op::NewTypeError));
}

DEFINE_SLOW_PATH(asm_slow_path_bitwise_xor, BitwiseXor)
{
    return asm_slow_path_bitwise_xor_values(vm, pc, values.dst, values.lhs, values.rhs);
}

i64 asm_slow_path_bitwise_xor_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, bitwise_xor(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_bitwise_and, BitwiseAnd)
{
    return asm_slow_path_bitwise_and_values(vm, pc, values.dst, values.lhs, values.rhs);
}

i64 asm_slow_path_bitwise_and_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, bitwise_and(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_bitwise_or, BitwiseOr)
{
    return asm_slow_path_bitwise_or_values(vm, pc, values.dst, values.lhs, values.rhs);
}

i64 asm_slow_path_bitwise_or_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, bitwise_or(*vm, lhs, rhs));
}

i64 asm_slow_path_left_shift_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, left_shift(*vm, lhs, rhs));
}

i64 asm_slow_path_right_shift_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, right_shift(*vm, lhs, rhs));
}

i64 asm_slow_path_unsigned_right_shift_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, unsigned_right_shift(*vm, lhs, rhs));
}

i64 asm_slow_path_mod_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, mod(*vm, lhs, rhs));
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

i64 asm_slow_path_strictly_equals_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path_value(*vm, pc, destination, Value { strictly_equals(lhs, rhs) });
}

i64 asm_slow_path_strictly_inequals_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path_value(*vm, pc, destination, Value { !strictly_equals(lhs, rhs) });
}

i64 asm_slow_path_loosely_equals_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, loosely_equals(*vm, lhs, rhs));
}

i64 asm_slow_path_loosely_inequals_values(VM* vm, u32 pc, Value& destination, Value lhs, Value rhs)
{
    return finish_binary_slow_path(*vm, pc, destination, loosely_inequals(*vm, lhs, rhs));
}

DEFINE_SLOW_PATH(asm_slow_path_unary_minus, UnaryMinus)
{
    values.dst = ASM_TRY(*vm, pc, unary_minus(*vm, values.src));
    return continue_after_slow_path(pc + sizeof(Op::UnaryMinus));
}

DEFINE_SLOW_PATH(asm_slow_path_to_string, ToString)
{
    auto result = ASM_TRY(*vm, pc, values.value.to_primitive_string(*vm));
    values.dst = Value { result };
    return continue_after_slow_path(pc + sizeof(Op::ToString));
}

DEFINE_SLOW_PATH(asm_slow_path_to_primitive_with_string_hint, ToPrimitiveWithStringHint)
{
    auto result = ASM_TRY(*vm, pc, values.value.to_primitive(*vm, Value::PreferredType::String));
    values.dst = result;
    return continue_after_slow_path(pc + sizeof(Op::ToPrimitiveWithStringHint));
}

DEFINE_SLOW_PATH(asm_slow_path_to_object, ToObject)
{
    auto result = ASM_TRY(*vm, pc, values.value.to_object(*vm));
    values.dst = result;
    return continue_after_slow_path(pc + sizeof(Op::ToObject));
}

DEFINE_SLOW_PATH(asm_slow_path_to_length, ToLength)
{
    auto result = ASM_TRY(*vm, pc, values.value.to_length(*vm));
    values.dst = Value { result };
    return continue_after_slow_path(pc + sizeof(Op::ToLength));
}

DEFINE_SLOW_PATH(asm_slow_path_typeof, Typeof)
{
    values.dst = values.src.typeof_(*vm);
    return continue_after_slow_path(pc + sizeof(Op::Typeof));
}

DEFINE_SLOW_PATH(asm_slow_path_postfix_decrement, PostfixDecrement)
{
    auto old_value = ASM_TRY(*vm, pc, values.src.to_numeric(*vm));
    values.dst = old_value;
    if (old_value.is_number())
        values.src = Value(old_value.as_double() - 1);
    else
        values.src = BigInt::create(*vm, old_value.as_bigint().big_integer().minus(Crypto::SignedBigInteger { 1 }));
    return continue_after_slow_path(pc + sizeof(Op::PostfixDecrement));
}

DEFINE_SLOW_PATH(asm_slow_path_to_int32, ToInt32)
{
    values.dst = Value(ASM_TRY(*vm, pc, values.value.to_i32(*vm)));
    return continue_after_slow_path(pc + sizeof(Op::ToInt32));
}

DEFINE_SLOW_PATH(asm_slow_path_put_by_value, PutByValue)
{
    auto value = values.src;
    auto base = values.base;
    Optional<Utf16FlyString const&> base_identifier;
    if (instruction->base_identifier().has_value())
        base_identifier = vm->get_identifier(instruction->base_identifier().value());
    auto property = values.property;
    auto property_key = ASM_TRY(*vm, pc, property.to_property_key(*vm));
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, base, value, base_identifier, property_key, instruction->kind(), instruction->strict()));
    return continue_after_slow_path(pc + sizeof(Op::PutByValue));
}

DEFINE_SLOW_PATH(asm_slow_path_put_by_value_with_this, PutByValueWithThis)
{
    auto value = values.src;
    auto base = values.base;
    auto this_value = values.this_value;
    auto property_key = ASM_TRY(*vm, pc, values.property.to_property_key(*vm));
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, this_value, value, {}, property_key, instruction->kind(), instruction->strict()));
    return continue_after_slow_path(pc + sizeof(Op::PutByValueWithThis));
}

DEFINE_SLOW_PATH(asm_slow_path_put_by_spread, PutBySpread)
{
    auto value = values.src;
    auto base = values.base;

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto object = ASM_TRY(*vm, pc, base.to_object(*vm));

    ASM_TRY(*vm, pc, object->copy_data_properties(*vm, value, {}));
    return continue_after_slow_path(pc + sizeof(Op::PutBySpread));
}

i64 asm_try_put_by_value_holey_array(VM*, u32, Op::PutByValue const*, Op::PutByValue::Values& values)
{
    auto base = values.base;
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = values.property;
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

    array.indexed_put(index, values.src);
    return 0;
}

// Try to inline a JS-to-JS call by building the callee frame through the
// shared VM::push_inline_frame() helper. Returns 0 on success (callee frame
// pushed) and 1 on failure (caller should keep handling the Call itself).
i64 asm_try_inline_call(VM* vm, u32 pc, Op::Call const* instruction, Op::Call::Values& values)
{
    auto callee = values.callee;
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
        ReadonlySpan<Value> { values.arguments, instruction->argument_count() },
        pc + instruction->length(),
        instruction->dst().raw(),
        values.this_value,
        nullptr,
        false);

    return callee_context ? 0 : 1;
}

i64 asm_try_inline_get_by_id_accessor(VM* vm, u32 pc, Op::GetById const* instruction, Op::GetById::Values& values)
{
    auto& object = values.base.as_object();
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];
    auto* entry = cache.first_entry();
    VERIFY(entry);

    auto* holder = entry->prototype ? entry->prototype.ptr() : &object;
    auto value = holder->get_direct(entry->property_offset);
    VERIFY(value.is_accessor());

    auto* getter = value.as_accessor().getter();
    if (!getter || !is<ECMAScriptFunctionObject>(*getter)) [[unlikely]]
        return 1;

    auto& getter_function = static_cast<ECMAScriptFunctionObject&>(*getter);
    if (!getter_function.can_inline_call()) [[unlikely]]
        return 1;

    auto* callee_context = vm->push_inline_frame(
        getter_function,
        getter_function.inline_call_executable(),
        {},
        pc + instruction->length(),
        instruction->dst().raw(),
        &object,
        nullptr,
        false);

    return callee_context ? 0 : 1;
}

// Fast cache-only PutById. Tries all cache entries for ChangeOwnProperty and
// AddOwnProperty. Returns 0 on cache hit, 1 on miss (caller should use full slow path).
i64 asm_try_put_by_id_cache(VM* vm, u32, Op::PutById const* instruction, Op::PutById::Values& values)
{
    auto base = values.base;
    if (!base.is_object()) [[unlikely]]
        return 1;
    auto& object = base.as_object();
    auto value = values.src;
    auto& cache = vm->current_executable().property_lookup_caches[instruction->cache()];

    for (auto& entry : cache.entries_for_shape(object.shape())) {
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
            if (entry.from_shape.ptr() != &object.shape()) [[unlikely]]
                continue;
            if (object.requires_slow_add_own_property()) [[unlikely]]
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
// chain lookups. Returns the cached value on hit, or Empty on miss.
u64 asm_try_get_by_id_cache(u64 encoded_base, PropertyLookupCache* cache)
{
    auto base = bit_cast<Value>(encoded_base);
    if (!base.is_object()) [[unlikely]]
        return js_special_empty_value().encoded();
    auto& object = base.as_object();
    auto& shape = object.shape();

    for (auto& entry : cache->entries_for_shape(shape)) {
        if (entry.type == PropertyLookupCache::Entry::Type::GetMissingProperty) {
            if (!object.is_cacheable_for_property_absence()) [[unlikely]]
                continue;
            if (&shape != entry.shape.ptr()) [[unlikely]]
                continue;
            if (shape.is_dictionary()
                && shape.dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            if (shape.prototype()) {
                auto* prototype_chain_validity = entry.prototype_chain_validity.ptr();
                if (!prototype_chain_validity || !prototype_chain_validity->is_valid()) [[unlikely]]
                    continue;
            }
            return js_undefined().encoded();
        }

        if (entry.type != PropertyLookupCache::Entry::Type::GetOwnProperty
            && entry.type != PropertyLookupCache::Entry::Type::GetPropertyInPrototypeChain) {
            continue;
        }

        auto cached_prototype = entry.prototype.ptr();
        if (cached_prototype) {
            if (&shape != entry.shape.ptr()) [[unlikely]]
                continue;
            if (shape.is_dictionary()
                && shape.dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto pcv = entry.prototype_chain_validity.ptr();
            if (!pcv || !pcv->is_valid()) [[unlikely]]
                continue;
            auto value = cached_prototype->get_direct(entry.property_offset);
            if (value.is_accessor()) [[unlikely]]
                return js_special_empty_value().encoded();
            return value.encoded();
        } else if (&shape == entry.shape.ptr()) {
            if (shape.is_dictionary()
                && shape.dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto value = object.get_direct(entry.property_offset);
            if (value.is_accessor()) [[unlikely]]
                return js_special_empty_value().encoded();
            return value.encoded();
        }
    }
    return js_special_empty_value().encoded();
}

DEFINE_SLOW_PATH(asm_slow_path_get_binding, GetBinding)
{
    auto next_pc = asm_get_binding<AsmBindingIsKnownToBeInitialized::No>(*vm, pc, values.dst, instruction->cache());
    return advance_or_continue<Op::GetBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_dynamic_get_binding, DynamicGetBinding)
{
    auto& cache = vm->current_executable().environment_coordinate_caches[instruction->cache()];
    auto next_pc = asm_dynamic_get_binding<AsmBindingIsKnownToBeInitialized::No>(*vm, pc, values.dst, instruction->identifier(), instruction->strict(), cache);
    return advance_or_continue<Op::DynamicGetBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_initialize_lexical_binding, InitializeLexicalBinding)
{
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Initialize>(*vm, pc, instruction->strict(), values.src, instruction->cache());
    return advance_or_continue<Op::InitializeLexicalBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_dynamic_initialize_lexical_binding, DynamicInitializeLexicalBinding)
{
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Initialize>(*vm, pc, instruction->identifier(), instruction->strict(), values.src, vm->current_executable().environment_coordinate_caches[instruction->cache()]);
    return advance_or_continue<Op::DynamicInitializeLexicalBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_initialize_variable_binding, InitializeVariableBinding)
{
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Initialize>(*vm, pc, instruction->strict(), values.src, instruction->cache());
    return advance_or_continue<Op::InitializeVariableBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_dynamic_initialize_variable_binding, DynamicInitializeVariableBinding)
{
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Initialize>(*vm, pc, instruction->identifier(), instruction->strict(), values.src, vm->current_executable().environment_coordinate_caches[instruction->cache()]);
    return advance_or_continue<Op::DynamicInitializeVariableBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_set_lexical_binding, SetLexicalBinding)
{
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Set>(*vm, pc, instruction->strict(), values.src, instruction->cache());
    return advance_or_continue<Op::SetLexicalBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_dynamic_set_lexical_binding, DynamicSetLexicalBinding)
{
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Set>(*vm, pc, instruction->identifier(), instruction->strict(), values.src, vm->current_executable().environment_coordinate_caches[instruction->cache()]);
    return advance_or_continue<Op::DynamicSetLexicalBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_set_variable_binding, SetVariableBinding)
{
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Set>(*vm, pc, instruction->strict(), values.src, instruction->cache());
    return advance_or_continue<Op::SetVariableBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_dynamic_set_variable_binding, DynamicSetVariableBinding)
{
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Set>(*vm, pc, instruction->identifier(), instruction->strict(), values.src, vm->current_executable().environment_coordinate_caches[instruction->cache()]);
    return advance_or_continue<Op::DynamicSetVariableBinding>(pc, next_pc);
}

DEFINE_SLOW_PATH(asm_slow_path_resolve_binding, ResolveBinding)
{
    auto const& identifier = vm->get_identifier(instruction->identifier());
    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(identifier, instruction->strict()));
    if (reference.is_unresolvable()) {
        values.dst = js_null();
        return continue_after_slow_path(pc + sizeof(Op::ResolveBinding));
    }

    VERIFY(reference.is_environment_reference());
    values.dst = &reference.base_environment();
    return continue_after_slow_path(pc + sizeof(Op::ResolveBinding));
}

DEFINE_SLOW_PATH(asm_slow_path_resolve_super_base, ResolveSuperBase)
{
    auto& environment = as<FunctionEnvironment>(*get_this_environment(*vm));
    VERIFY(environment.has_super_binding());
    auto base_value = ASM_TRY(*vm, pc, environment.get_super_base());
    values.dst = base_value;
    return continue_after_slow_path(pc + sizeof(Op::ResolveSuperBase));
}

DEFINE_SLOW_PATH(asm_slow_path_set_resolved_binding, SetResolvedBinding)
{
    auto const& identifier = vm->get_identifier(instruction->identifier());
    auto environment = values.environment;
    auto reference = environment.is_null()
        ? Reference { Reference::BaseType::Unresolvable, PropertyKey { identifier }, instruction->strict() }
        : Reference { as<Environment>(environment.as_cell()), identifier, instruction->strict() };
    ASM_TRY(*vm, pc, reference.put_value(*vm, values.src));
    return continue_after_slow_path(pc + sizeof(Op::SetResolvedBinding));
}

DEFINE_SLOW_PATH(asm_slow_path_typeof_binding, TypeofBinding)
{
    VERIFY(instruction->cache().is_valid());

    auto const* environment = vm->running_execution_context().lexical_environment.ptr();
    for (size_t i = 0; i < instruction->cache().hops; ++i)
        environment = environment->outer_environment();

    auto value = ASM_TRY(*vm, pc, static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(*vm, instruction->cache().index));
    values.dst = value.typeof_(*vm);
    return continue_after_slow_path(pc + sizeof(Op::TypeofBinding));
}

DEFINE_SLOW_PATH(asm_slow_path_dynamic_typeof_binding, DynamicTypeofBinding)
{
    auto& cache = vm->current_executable().environment_coordinate_caches[instruction->cache()];
    auto const* current_environment = vm->running_execution_context().lexical_environment.ptr();
    if (auto const* environment = asm_get_cached_environment(current_environment, cache)) [[likely]] {
        auto value = ASM_TRY(*vm, pc, static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(*vm, cache.index));
        values.dst = value.typeof_(*vm);
        return continue_after_slow_path(pc + sizeof(Op::DynamicTypeofBinding));
    }

    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(vm->get_identifier(instruction->identifier()), instruction->strict()));
    if (reference.is_unresolvable()) {
        values.dst = PrimitiveString::create(*vm, "undefined"_utf16_fly_string);
        return continue_after_slow_path(pc + sizeof(Op::DynamicTypeofBinding));
    }

    asm_update_environment_coordinate_cache(current_environment, reference, cache);
    auto value = ASM_TRY(*vm, pc, reference.get_value(*vm));
    values.dst = value.typeof_(*vm);
    return continue_after_slow_path(pc + sizeof(Op::DynamicTypeofBinding));
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

DEFINE_SLOW_PATH(asm_slow_path_has_private_id, HasPrivateId)
{
    auto base = values.base;
    if (!base.is_object()) [[unlikely]] {
        auto completion = vm->throw_completion<TypeError>(ErrorType::InOperatorWithObject);
        return handle_asm_exception(*vm, pc, completion.value());
    }

    auto private_environment = vm->running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(vm->get_identifier(instruction->property()));
    values.dst = Value(base.as_object().private_element_find(private_name) != nullptr);
    return continue_after_slow_path(pc + sizeof(Op::HasPrivateId));
}

DEFINE_SLOW_PATH(asm_slow_path_set_function_name, SetFunctionName)
{
    auto function = values.function.as_if<ECMAScriptFunctionObject>();
    if (!function || !function->name().is_empty())
        return continue_after_slow_path(pc + sizeof(Op::SetFunctionName));

    auto property_key = ASM_TRY(*vm, pc, values.name.to_property_key(*vm));
    function->set_inferred_name(Variant<PropertyKey, PrivateName> { move(property_key) }, asm_function_name_prefix_to_string(instruction->prefix()));
    return continue_after_slow_path(pc + sizeof(Op::SetFunctionName));
}

DEFINE_SLOW_PATH(asm_slow_path_new_array_with_length, NewArrayWithLength)
{
    auto length = static_cast<u64>(values.array_length.as_double());
    auto array = ASM_TRY(*vm, pc, JS::Array::create(vm->realm(), length));
    values.dst = array;
    return continue_after_slow_path(pc + sizeof(Op::NewArrayWithLength));
}

DEFINE_SLOW_PATH(asm_slow_path_array_append, ArrayAppend)
{
    auto rhs = values.src;
    auto& lhs_array = values.dst.as_array_exotic_object();
    auto lhs_size = lhs_array.indexed_array_like_size();

    if (instruction->is_spread()) {
        auto* rhs_array = rhs.is_object() ? as_if<JS::Array>(rhs.as_object()) : nullptr;
        Optional<IteratorRecordImpl> iterator_record;

        if (rhs_array && lhs_array.indexed_storage_kind() <= IndexedStorageKind::Packed) {
            static auto& iterator_method_cache = *new StaticPropertyLookupCache;
            auto iterator_method = ASM_TRY(*vm, pc, rhs.get_method(*vm, vm->well_known_symbol_iterator(), iterator_method_cache));
            if (!iterator_method) {
                auto completion = vm->throw_completion<TypeError>(ErrorType::NotIterable, rhs);
                return handle_asm_exception(*vm, pc, completion.value());
            }

            // OPTIMIZATION: The original array iterator has no observable side effects, so a packed
            //               array can be appended in bulk if its next method is also unchanged.
            auto original_iterator_method = vm->current_realm()->intrinsics().array_prototype_values_function();
            if (iterator_method == original_iterator_method && rhs_array->is_simple_packed_array()) {
                auto iterator_prototype = vm->current_realm()->intrinsics().array_iterator_prototype();

                // NB: Inspect the intrinsic prototype's own property without invoking it. Using get()
                //     here would call an accessor with the prototype as its receiver, whereas the
                //     iterator protocol calls it with the newly created iterator as its receiver.
                //     Accessors and replacement methods therefore take the generic path below.
                static auto& next_method_cache = *new StaticPropertyLookupCache;
                auto next_method = get_own_property_without_side_effects(*iterator_prototype, vm->names.next, next_method_cache);
                if (next_method.is_function() && next_method.as_function().is_native_function()
                    && static_cast<NativeFunction const&>(next_method.as_function()).is_array_prototype_next_builtin()) {
                    auto elements = rhs_array->indexed_packed_elements_span();
                    if (elements.size() <= NumericLimits<u32>::max() - lhs_size) {
                        lhs_array.indexed_append(elements);
                        return continue_after_slow_path(pc + sizeof(Op::ArrayAppend));
                    }
                }
            }

            iterator_record = ASM_TRY(*vm, pc, get_iterator_from_method_impl(*vm, rhs, *iterator_method));
        }

        size_t i = lhs_size;
        if (iterator_record.has_value()) {
            while (true) {
                auto iterator_value = ASM_TRY(*vm, pc, iterator_step_value(*vm, *iterator_record));
                if (!iterator_value.has_value())
                    break;
                lhs_array.indexed_put(i++, iterator_value.release_value());
            }
            return continue_after_slow_path(pc + sizeof(Op::ArrayAppend));
        }

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

    return continue_after_slow_path(pc + sizeof(Op::ArrayAppend));
}

DEFINE_SLOW_PATH(asm_slow_path_create_variable, CreateVariable)
{
    auto const& name = vm->get_identifier(instruction->identifier());
    ASM_TRY(*vm, pc, asm_create_variable(*vm, name, instruction->mode(), instruction->is_global(), instruction->is_immutable(), instruction->is_strict()));
    return continue_after_slow_path(pc + sizeof(Op::CreateVariable));
}

DEFINE_SLOW_PATH(asm_slow_path_enter_object_environment, EnterObjectEnvironment)
{
    auto object = ASM_TRY(*vm, pc, values.object.to_object(*vm));
    auto& old_environment = vm->running_execution_context().lexical_environment;
    auto new_environment = new_object_environment(*object, true, old_environment.ptr());
    values.dst = new_environment;
    vm->running_execution_context().lexical_environment = new_environment;
    return continue_after_slow_path(pc + sizeof(Op::EnterObjectEnvironment));
}

DEFINE_SLOW_PATH(asm_slow_path_bitwise_not, BitwiseNot)
{
    values.dst = ASM_TRY(*vm, pc, bitwise_not(*vm, values.src));
    return continue_after_slow_path(pc + sizeof(Op::BitwiseNot));
}

DEFINE_SLOW_PATH(asm_slow_path_unary_plus, UnaryPlus)
{
    values.dst = ASM_TRY(*vm, pc, unary_plus(*vm, values.src));
    return continue_after_slow_path(pc + sizeof(Op::UnaryPlus));
}

DEFINE_SLOW_PATH(asm_slow_path_is_constructor, IsConstructor)
{
    values.dst = Value(values.value.is_constructor());
    return continue_after_slow_path(pc + sizeof(Op::IsConstructor));
}

DEFINE_SLOW_PATH(asm_slow_path_add_private_name, AddPrivateName)
{
    auto const& name = vm->get_identifier(instruction->name());
    vm->running_execution_context().private_environment->add_private_name(name);
    return continue_after_slow_path(pc + sizeof(Op::AddPrivateName));
}

DEFINE_SLOW_PATH(asm_slow_path_create_async_from_sync_iterator, CreateAsyncFromSyncIterator)
{
    auto& realm = vm->realm();

    auto& iterator = values.iterator.as_object();
    auto next_method = values.next_method;
    auto done = values.done.as_bool();

    auto iterator_record = realm.create<IteratorRecord>(iterator, next_method, done);
    auto async_from_sync_iterator = create_async_from_sync_iterator(*vm, iterator_record);

    auto iterator_object = Object::create(realm, nullptr);
    iterator_object->define_direct_property(vm->names.iterator, async_from_sync_iterator.iterator, default_attributes);
    iterator_object->define_direct_property(vm->names.nextMethod, async_from_sync_iterator.next_method, default_attributes);
    iterator_object->define_direct_property(vm->names.done, Value { async_from_sync_iterator.done }, default_attributes);

    values.dst = iterator_object;
    return continue_after_slow_path(pc + sizeof(Op::CreateAsyncFromSyncIterator));
}

DEFINE_SLOW_PATH(asm_slow_path_create_data_property_or_throw, CreateDataPropertyOrThrow)
{
    auto& object = values.object.as_object();
    auto property = ASM_TRY(*vm, pc, values.property.to_property_key(*vm));
    auto value = values.value;
    ASM_TRY(*vm, pc, object.create_data_property_or_throw(property, value));
    return continue_after_slow_path(pc + sizeof(Op::CreateDataPropertyOrThrow));
}

DEFINE_SLOW_PATH(asm_slow_path_create_immutable_binding, CreateImmutableBinding)
{
    auto& environment = as<Environment>(values.environment.as_cell());
    ASM_TRY(*vm, pc, environment.create_immutable_binding(*vm, vm->get_identifier(instruction->identifier()), instruction->strict_binding()));
    return continue_after_slow_path(pc + sizeof(Op::CreateImmutableBinding));
}

DEFINE_SLOW_PATH(asm_slow_path_create_mutable_binding, CreateMutableBinding)
{
    auto& environment = as<Environment>(values.environment.as_cell());
    ASM_TRY(*vm, pc, environment.create_mutable_binding(*vm, vm->get_identifier(instruction->identifier()), instruction->can_be_deleted()));
    return continue_after_slow_path(pc + sizeof(Op::CreateMutableBinding));
}

DEFINE_SLOW_PATH(asm_slow_path_create_rest_params, CreateRestParams)
{
    auto const arguments = vm->running_execution_context().arguments_span();
    auto arguments_count = vm->running_execution_context().passed_argument_count;
    auto array = MUST(JS::Array::create(vm->realm(), 0));
    for (size_t rest_index = instruction->rest_index(); rest_index < arguments_count; ++rest_index)
        array->indexed_append(arguments[rest_index]);
    values.dst = array;
    return continue_after_slow_path(pc + sizeof(Op::CreateRestParams));
}

DEFINE_SLOW_PATH(asm_slow_path_create_arguments, CreateArguments)
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
        values.dst = arguments_object;
        return continue_after_slow_path(pc + sizeof(Op::CreateArguments));
    }

    if (instruction->is_immutable()) {
        MUST(environment->create_immutable_binding(*vm, vm->names.arguments.as_string(), false));
    } else {
        MUST(environment->create_mutable_binding(*vm, vm->names.arguments.as_string(), false));
    }
    MUST(environment->initialize_binding(*vm, vm->names.arguments.as_string(), arguments_object, Environment::InitializeBindingHint::Normal));
    return continue_after_slow_path(pc + sizeof(Op::CreateArguments));
}

DEFINE_SLOW_PATH(asm_slow_path_await, Await)
{
    auto yielded_value = values.argument.is_special_empty_value() ? js_undefined() : values.argument;
    auto& context = vm->running_execution_context();
    context.yield_continuation = instruction->continuation_label().address();
    context.yield_is_await = true;
    context.yield_value_is_iterator_result = false;
    vm->do_return(yielded_value);
    return -1;
}

DEFINE_SLOW_PATH(asm_slow_path_create_lexical_environment, CreateLexicalEnvironment)
{
    auto& parent = as<Environment>(values.parent.as_cell());
    auto environment = new_declarative_environment(parent);
    environment->ensure_capacity(instruction->capacity());
    environment->set_is_catch_environment(instruction->is_catch_environment());
    values.dst = environment;
    vm->running_execution_context().lexical_environment = environment;
    return continue_after_slow_path(pc + sizeof(Op::CreateLexicalEnvironment));
}

DEFINE_SLOW_PATH(asm_slow_path_create_private_environment, CreatePrivateEnvironment)
{
    auto& running_execution_context = vm->running_execution_context();
    auto outer_private_environment = running_execution_context.private_environment;
    running_execution_context.private_environment = new_private_environment(*vm, outer_private_environment.ptr());
    return continue_after_slow_path(pc + sizeof(Op::CreatePrivateEnvironment));
}

DEFINE_SLOW_PATH(asm_slow_path_create_variable_environment, CreateVariableEnvironment)
{
    auto& running_execution_context = vm->running_execution_context();
    auto var_environment = new_declarative_environment(*running_execution_context.lexical_environment);
    if (auto* shared_data = vm->active_shared_function_data(); shared_data && instruction->capacity() == shared_data->m_var_environment_bindings_count)
        var_environment->set_environment_shape_cache(shared_data->m_var_environment_shape, instruction->capacity());
    var_environment->ensure_capacity(instruction->capacity());
    running_execution_context.variable_environment = var_environment;
    running_execution_context.lexical_environment = var_environment;
    return continue_after_slow_path(pc + sizeof(Op::CreateVariableEnvironment));
}

DEFINE_SLOW_PATH(asm_slow_path_delete_by_id, DeleteById)
{
    auto const& property_key = vm->get_property_key(instruction->property());
    auto reference = Reference { values.base, property_key, {}, instruction->strict() };
    auto result = ASM_TRY(*vm, pc, reference.delete_(*vm));
    values.dst = Value(result);
    return continue_after_slow_path(pc + sizeof(Op::DeleteById));
}

DEFINE_SLOW_PATH(asm_slow_path_delete_by_value, DeleteByValue)
{
    auto property_key = ASM_TRY(*vm, pc, values.property.to_property_key(*vm));
    auto reference = Reference { values.base, property_key, {}, instruction->strict() };
    auto result = ASM_TRY(*vm, pc, reference.delete_(*vm));
    values.dst = Value(result);
    return continue_after_slow_path(pc + sizeof(Op::DeleteByValue));
}

DEFINE_SLOW_PATH(asm_slow_path_delete_variable, DeleteVariable)
{
    auto const& string = vm->get_identifier(instruction->identifier());
    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(string, instruction->strict()));
    auto result = ASM_TRY(*vm, pc, reference.delete_(*vm));
    values.dst = Value(result);
    return continue_after_slow_path(pc + sizeof(Op::DeleteVariable));
}

DEFINE_SLOW_PATH(asm_slow_path_get_completion_fields, GetCompletionFields)
{
    auto& completion_source = values.completion.as_object();
    if (is<GeneratorObject>(completion_source)) {
        auto const& generator = as<GeneratorObject>(completion_source);
        values.value_dst = generator.pending_completion_value();
        values.type_dst = Value(to_underlying(generator.pending_completion_type()));
        return continue_after_slow_path(pc + sizeof(Op::GetCompletionFields));
    }

    auto const& async_generator = as<AsyncGenerator>(completion_source);
    values.value_dst = async_generator.pending_completion_value();
    values.type_dst = Value(to_underlying(async_generator.pending_completion_type()));
    return continue_after_slow_path(pc + sizeof(Op::GetCompletionFields));
}

DEFINE_SLOW_PATH(asm_slow_path_set_completion_type, SetCompletionType)
{
    auto& completion_source = values.completion.as_object();
    if (is<GeneratorObject>(completion_source)) {
        as<GeneratorObject>(completion_source).set_pending_completion_type(instruction->completion_type());
        return continue_after_slow_path(pc + sizeof(Op::SetCompletionType));
    }

    as<AsyncGenerator>(completion_source).set_pending_completion_type(instruction->completion_type());
    return continue_after_slow_path(pc + sizeof(Op::SetCompletionType));
}

DEFINE_SLOW_PATH(asm_slow_path_get_template_object, GetTemplateObject)
{
    auto& cache = *vm->current_executable().template_object_caches[instruction->cache()];

    if (cache.cached_template_object) {
        values.dst = cache.cached_template_object;
        return continue_after_slow_path(pc + instruction->length());
    }

    auto& realm = *vm->current_realm();
    u32 count = instruction->strings_count() / 2;
    auto template_object = MUST(JS::Array::create(realm, count));
    auto raw_object = MUST(JS::Array::create(realm, count));

    for (size_t index = 0; index < count; ++index) {
        template_object->indexed_put(index, values.strings[index], Attribute::Enumerable);
        raw_object->indexed_put(index, values.strings[count + index], Attribute::Enumerable);
    }

    MUST(raw_object->set_integrity_level(Object::IntegrityLevel::Frozen));
    template_object->define_direct_property(vm->names.raw, raw_object, PropertyAttributes {});
    MUST(template_object->set_integrity_level(Object::IntegrityLevel::Frozen));

    cache.cached_template_object = template_object;
    values.dst = template_object;
    return continue_after_slow_path(pc + instruction->length());
}

DEFINE_SLOW_PATH(asm_slow_path_new_function, NewFunction)
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
        auto home_object_value = values.home_object;
        function->make_method(home_object_value.as_object());
    }

    values.dst = function;
    return continue_after_slow_path(pc + sizeof(Op::NewFunction));
}

DEFINE_SLOW_PATH(asm_slow_path_throw, Throw)
{
    return handle_asm_exception(*vm, pc, values.src);
}

DEFINE_SLOW_PATH(asm_slow_path_throw_if_tdz, ThrowIfTDZ)
{
    auto value = values.src;
    if (value.is_special_empty_value()) [[unlikely]] {
        auto completion = vm->throw_completion<ReferenceError>(ErrorType::BindingNotInitialized, value);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    return continue_after_slow_path(pc + sizeof(Op::ThrowIfTDZ));
}

DEFINE_SLOW_PATH(asm_slow_path_throw_if_not_object, ThrowIfNotObject)
{
    auto src = values.src;
    if (!src.is_object()) [[unlikely]] {
        auto completion = vm->throw_completion<TypeError>(ErrorType::NotAnObject, src);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    return continue_after_slow_path(pc + sizeof(Op::ThrowIfNotObject));
}

DEFINE_SLOW_PATH(asm_slow_path_throw_if_nullish, ThrowIfNullish)
{
    auto value = values.src;
    if (value.is_nullish()) [[unlikely]] {
        auto completion = vm->throw_completion<TypeError>(ErrorType::NotObjectCoercible, value);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    return continue_after_slow_path(pc + sizeof(Op::ThrowIfNullish));
}

DEFINE_SLOW_PATH(asm_slow_path_throw_const_assignment, ThrowConstAssignment)
{
    auto completion = vm->throw_completion<TypeError>(ErrorType::InvalidAssignToConst);
    return handle_asm_exception(*vm, pc, completion.value());
}

DEFINE_SLOW_PATH(asm_slow_path_debugger, Debugger)
{
    // NB: Don't pause twice if the debugger trampoline already paused before this instruction.
    if (auto* debugger = vm->debugger(); debugger && !debugger->did_pause_before_current_instruction())
        debugger->pause_execution(vm->current_executable(), pc, Debugger::PauseReason::DebuggerStatement);
    return continue_after_slow_path(pc + sizeof(Op::Debugger));
}

DEFINE_SLOW_PATH(asm_slow_path_yield, Yield)
{
    auto yielded_value = values.value.is_special_empty_value() ? js_undefined() : values.value;
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

DEFINE_SLOW_PATH(asm_slow_path_yield_iterator_result, YieldIteratorResult)
{
    auto yielded_value = values.value.is_special_empty_value() ? js_undefined() : values.value;
    auto& context = vm->running_execution_context();
    context.yield_continuation = instruction->continuation_label().address();
    context.yield_is_await = false;
    context.yield_value_is_iterator_result = true;
    vm->do_return(yielded_value);
    return -1;
}

// Fast path for GetByValue on typed arrays.
// Returns 0 on success (result stored in dst), 1 on miss (fall to slow path).
i64 asm_try_get_by_value_typed_array(VM*, u32, Op::GetByValue const*, Op::GetByValue::Values& values)
{
    auto base = values.base;
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = values.property;
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
        values.dst = js_undefined();
        return 0;
    }

    if (!is_valid_integer_index(typed_array, CanonicalIndex { CanonicalIndex::Type::Index, index })) [[unlikely]] {
        values.dst = js_undefined();
        return 0;
    }

    auto* buffer = typed_array.viewed_array_buffer();
    Checked<size_t> byte_index = index;
    byte_index *= typed_array.element_size();
    byte_index += typed_array.byte_offset();
    if (byte_index.has_overflow()) [[unlikely]]
        return 1;

    Value result;
    switch (typed_array.kind()) {
    case TypedArrayBase::Kind::Uint8Array:
    case TypedArrayBase::Kind::Uint8ClampedArray:
        result = buffer->get_value<u8>(byte_index.value(), true, ArrayBuffer::Order::Unordered);
        break;
    case TypedArrayBase::Kind::Int8Array:
        result = buffer->get_value<i8>(byte_index.value(), true, ArrayBuffer::Order::Unordered);
        break;
    case TypedArrayBase::Kind::Uint16Array:
        result = buffer->get_value<u16>(byte_index.value(), true, ArrayBuffer::Order::Unordered);
        break;
    case TypedArrayBase::Kind::Int16Array:
        result = buffer->get_value<i16>(byte_index.value(), true, ArrayBuffer::Order::Unordered);
        break;
    case TypedArrayBase::Kind::Uint32Array:
        result = buffer->get_value<u32>(byte_index.value(), true, ArrayBuffer::Order::Unordered);
        break;
    case TypedArrayBase::Kind::Int32Array:
        result = buffer->get_value<i32>(byte_index.value(), true, ArrayBuffer::Order::Unordered);
        break;
    case TypedArrayBase::Kind::Float32Array:
        result = buffer->get_value<float>(byte_index.value(), true, ArrayBuffer::Order::Unordered);
        break;
    case TypedArrayBase::Kind::Float64Array:
        result = buffer->get_value<double>(byte_index.value(), true, ArrayBuffer::Order::Unordered);
        break;
    default:
        return 1;
    }

    values.dst = result;
    return 0;
}

// Fast path for PutByValue on typed arrays.
// Returns 0 on success, 1 on miss (fall to slow path).
i64 asm_try_put_by_value_typed_array(VM*, u32, Op::PutByValue const*, Op::PutByValue::Values& values)
{
    auto base = values.base;
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = values.property;
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
    Checked<size_t> byte_index = index;
    byte_index *= typed_array.element_size();
    byte_index += typed_array.byte_offset();
    if (byte_index.has_overflow()) [[unlikely]]
        return 1;
    auto value = values.src;

    if (value.is_int32()) {
        auto int_val = value.as_i32();
        switch (typed_array.kind()) {
        case TypedArrayBase::Kind::Uint8Array:
            buffer->set_value<u8>(byte_index.value(), value, true, ArrayBuffer::Order::Unordered);
            return 0;
        case TypedArrayBase::Kind::Uint8ClampedArray:
            buffer->set_value<ClampedU8>(byte_index.value(), Value { clamp(int_val, 0, 255) }, true, ArrayBuffer::Order::Unordered);
            return 0;
        case TypedArrayBase::Kind::Int8Array:
            buffer->set_value<i8>(byte_index.value(), value, true, ArrayBuffer::Order::Unordered);
            return 0;
        case TypedArrayBase::Kind::Uint16Array:
            buffer->set_value<u16>(byte_index.value(), value, true, ArrayBuffer::Order::Unordered);
            return 0;
        case TypedArrayBase::Kind::Int16Array:
            buffer->set_value<i16>(byte_index.value(), value, true, ArrayBuffer::Order::Unordered);
            return 0;
        case TypedArrayBase::Kind::Uint32Array:
            buffer->set_value<u32>(byte_index.value(), value, true, ArrayBuffer::Order::Unordered);
            return 0;
        case TypedArrayBase::Kind::Int32Array:
            buffer->set_value<i32>(byte_index.value(), value, true, ArrayBuffer::Order::Unordered);
            return 0;
        default:
            break;
        }
    } else if (value.is_double()) {
        auto dbl_val = value.as_double();
        switch (typed_array.kind()) {
        case TypedArrayBase::Kind::Float32Array:
            buffer->set_value<float>(byte_index.value(), Value { dbl_val }, true, ArrayBuffer::Order::Unordered);
            return 0;
        case TypedArrayBase::Kind::Float64Array:
            buffer->set_value<double>(byte_index.value(), Value { dbl_val }, true, ArrayBuffer::Order::Unordered);
            return 0;
        default:
            break;
        }
    }

    return 1;
}

DEFINE_SLOW_PATH(asm_slow_path_instance_of, InstanceOf)
{
    auto result = ASM_TRY(*vm, pc, instance_of(*vm, values.lhs, values.rhs));
    values.dst = result;
    return continue_after_slow_path(pc + sizeof(Op::InstanceOf));
}

DEFINE_SLOW_PATH(asm_slow_path_in, In)
{
    auto result = ASM_TRY(*vm, pc, in(*vm, values.lhs, values.rhs));
    values.dst = result;
    return continue_after_slow_path(pc + sizeof(Op::In));
}

DEFINE_SLOW_PATH(asm_slow_path_resolve_this_binding, ResolveThisBinding)
{
    auto& cached_this_value = vm->reg(Register::this_value());
    if (!cached_this_value.is_special_empty_value())
        return continue_after_slow_path(pc + sizeof(Op::ResolveThisBinding));

    auto& running_execution_context = vm->running_execution_context();
    if (auto function = running_execution_context.function; function && is<ECMAScriptFunctionObject>(*function)) {
        auto& ecmascript_function = static_cast<ECMAScriptFunctionObject&>(*function);
        if (!ecmascript_function.allocates_function_environment() && !ecmascript_function.this_value_needs_environment_resolution()) {
            cached_this_value = running_execution_context.this_value.value();
            return continue_after_slow_path(pc + sizeof(Op::ResolveThisBinding));
        }
    }
    cached_this_value = ASM_TRY(*vm, pc, vm->resolve_this_binding());
    return continue_after_slow_path(pc + sizeof(Op::ResolveThisBinding));
}

// Direct handler for GetPrivateById: bypasses Reference indirection.
DEFINE_SLOW_PATH(asm_slow_path_get_private_by_id, GetPrivateById)
{
    auto base_value = values.base;
    auto& current_vm = *vm;

    if (!base_value.is_object()) [[unlikely]] {
        ASM_TRY(*vm, pc, base_value.to_object(current_vm));
        auto const& name = current_vm.get_identifier(instruction->property());
        auto private_name = make_private_reference(current_vm, base_value, name);
        auto result = ASM_TRY(*vm, pc, private_name.get_value(current_vm));
        values.dst = result;
        return continue_after_slow_path(pc + sizeof(Op::GetPrivateById));
    }

    auto const& name = current_vm.get_identifier(instruction->property());
    auto private_environment = current_vm.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(name);
    auto result = ASM_TRY(*vm, pc, base_value.as_object().private_get(private_name));
    values.dst = result;
    return continue_after_slow_path(pc + sizeof(Op::GetPrivateById));
}

// Direct handler for PutPrivateById: bypasses Reference indirection.
DEFINE_SLOW_PATH(asm_slow_path_put_private_by_id, PutPrivateById)
{
    auto base_value = values.base;
    auto& current_vm = *vm;
    auto value = values.src;

    if (!base_value.is_object()) [[unlikely]] {
        auto object = ASM_TRY(*vm, pc, base_value.to_object(current_vm));
        auto const& name = current_vm.get_identifier(instruction->property());
        auto private_reference = make_private_reference(current_vm, object, name);
        ASM_TRY(*vm, pc, private_reference.put_value(current_vm, value));
        return continue_after_slow_path(pc + sizeof(Op::PutPrivateById));
    }

    auto const& name = current_vm.get_identifier(instruction->property());
    auto private_environment = current_vm.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(name);
    ASM_TRY(*vm, pc, base_value.as_object().private_set(private_name, value));
    return continue_after_slow_path(pc + sizeof(Op::PutPrivateById));
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
