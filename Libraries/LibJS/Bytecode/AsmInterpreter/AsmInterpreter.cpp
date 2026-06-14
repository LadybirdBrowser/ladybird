/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibJS/Bytecode/AsmInterpreter/AsmInterpreter.h>
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
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>
#include <stdlib.h>

// ===== Slow path hit counters (for profiling) =====
// Define JS_ASMINT_SLOW_PATH_COUNTERS to enable per-opcode slow path
// counters. They are printed on exit when the asm interpreter is active.
#ifdef JS_ASMINT_SLOW_PATH_COUNTERS
static struct AsmSlowPathStats {
    u64 fallback_by_type[256] {};
    u64 slow_path_by_type[256] {};
    bool registered {};
} s_stats;

static void print_asm_slow_path_stats()
{
    fprintf(stderr, "\n=== AsmInterpreter slow path stats ===\n");

    static char const* const s_type_names[] = {
#    define __BYTECODE_OP(op) #op,
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
#    undef __BYTECODE_OP
    };

    struct Entry {
        char const* name;
        u64 count;
    };
    Entry entries[512];
    size_t num_entries = 0;

    for (size_t i = 0; i < 256; ++i) {
        if (s_stats.fallback_by_type[i] > 0)
            entries[num_entries++] = { s_type_names[i], s_stats.fallback_by_type[i] };
    }

    for (size_t i = 0; i < 256; ++i) {
        if (s_stats.slow_path_by_type[i] > 0)
            entries[num_entries++] = { s_type_names[i], s_stats.slow_path_by_type[i] };
    }

    // Bubble sort by count descending (small array, no need for qsort)
    for (size_t i = 0; i < num_entries; ++i)
        for (size_t j = i + 1; j < num_entries; ++j)
            if (entries[j].count > entries[i].count) {
                auto tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }

    for (size_t i = 0; i < num_entries; ++i)
        fprintf(stderr, "  %12llu  %s\n", static_cast<unsigned long long>(entries[i].count), entries[i].name);

    fprintf(stderr, "===\n\n");
}
#endif

namespace JS::Bytecode {

// Defined in generated assembly (asmint_x86_64.S or asmint_aarch64.S)
extern "C" void asm_interpreter_entry(u8 const* bytecode, u32 entry_point, Value* values, VM* vm);

void AsmInterpreter::run(VM& vm, size_t entry_point)
{
#ifdef JS_ASMINT_SLOW_PATH_COUNTERS
    if (!s_stats.registered) {
        s_stats.registered = true;
        atexit(print_asm_slow_path_stats);
    }
#endif

    auto& context = vm.running_execution_context();
    auto* bytecode = context.executable->bytecode.data();
    auto* values = context.registers_and_constants_and_locals_and_arguments_span().data();

    asm_interpreter_entry(bytecode, static_cast<u32>(entry_point), values, &vm);
}

}

// ===== Slow path functions callable from assembly =====
// All slow path functions follow the same convention:
//   i64 func(VM* vm, u32 pc)
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
        asm_try_vm.running_execution_context().program_counter = asm_try_pc;                             \
        auto&& asm_try_result = (expression);                                                            \
        if (asm_try_result.is_error()) [[unlikely]]                                                      \
            return handle_asm_exception(asm_try_vm, asm_try_pc, asm_try_result.release_error().value()); \
        asm_try_result.release_value();                                                                  \
    })

// Helper: execute a throwing instruction and handle errors
template<typename InsnType>
static i64 execute_throwing(VM& vm, u32 pc)
{
    vm.running_execution_context().program_counter = pc;
    auto* bytecode = vm.current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<InsnType const*>(&bytecode[pc]);
    ASM_TRY(vm, pc, insn.execute_impl(vm));
    if constexpr (InsnType::IsVariableLength)
        return static_cast<i64>(pc + insn.length());
    else
        return static_cast<i64>(pc + sizeof(InsnType));
}

// Slow path wrappers: optionally bump per-opcode counter, then delegate.
#ifdef JS_ASMINT_SLOW_PATH_COUNTERS
template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_throwing(VM& vm, u32 pc)
{
    ++s_stats.slow_path_by_type[static_cast<u8>(Instruction::Type { vm.current_executable().bytecode[pc] })];
    return execute_throwing<InsnType>(vm, pc);
}

ALWAYS_INLINE static void bump_slow_path(VM& vm, u32 pc)
{
    ++s_stats.slow_path_by_type[static_cast<u8>(Instruction::Type { vm.current_executable().bytecode[pc] })];
}
#else
template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_throwing(VM& vm, u32 pc)
{
    return execute_throwing<InsnType>(vm, pc);
}

ALWAYS_INLINE static void bump_slow_path(VM&, u32) { }
#endif

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
i64 asm_fallback_handler(VM*, u32 pc);
i64 asm_slow_path_add(VM*, u32 pc);
i64 asm_slow_path_sub(VM*, u32 pc);
i64 asm_slow_path_mul(VM*, u32 pc);
i64 asm_slow_path_div(VM*, u32 pc);
i64 asm_slow_path_increment(VM*, u32 pc);
i64 asm_slow_path_decrement(VM*, u32 pc);
i64 asm_slow_path_less_than(VM*, u32 pc);
i64 asm_slow_path_less_than_equals(VM*, u32 pc);
i64 asm_slow_path_greater_than(VM*, u32 pc);
i64 asm_slow_path_greater_than_equals(VM*, u32 pc);
i64 asm_slow_path_jump_less_than(VM*, u32 pc);
i64 asm_slow_path_jump_greater_than(VM*, u32 pc);
i64 asm_slow_path_jump_less_than_equals(VM*, u32 pc);
i64 asm_slow_path_jump_greater_than_equals(VM*, u32 pc);
i64 asm_slow_path_jump_loosely_equals(VM*, u32 pc);
i64 asm_slow_path_jump_loosely_inequals(VM*, u32 pc);
i64 asm_slow_path_jump_strictly_equals(VM*, u32 pc);
i64 asm_slow_path_jump_strictly_inequals(VM*, u32 pc);
i64 asm_slow_path_set_lexical_environment(VM*, u32 pc);
i64 asm_slow_path_postfix_increment(VM*, u32 pc);
i64 asm_slow_path_get_by_id(VM*, u32 pc);
i64 asm_slow_path_get_by_id_with_this(VM*, u32 pc);
i64 asm_slow_path_put_by_id(VM*, u32 pc);
i64 asm_slow_path_put_by_id_with_this(VM*, u32 pc);
i64 asm_slow_path_get_by_value(VM*, u32 pc);
i64 asm_slow_path_get_by_value_with_this(VM*, u32 pc);
i64 asm_slow_path_get_length(VM*, u32 pc);
i64 asm_slow_path_get_length_with_this(VM*, u32 pc);
i64 asm_slow_path_get_method(VM*, u32 pc);
i64 asm_slow_path_get_iterator(VM*, u32 pc);
i64 asm_slow_path_get_import_meta(VM*, u32 pc);
i64 asm_slow_path_get_new_target(VM*, u32 pc);
i64 asm_slow_path_get_super_constructor(VM*, u32 pc);
i64 asm_try_get_global_env_binding(VM*, u32 pc);
i64 asm_try_set_global_env_binding(VM*, u32 pc);
i64 asm_slow_path_get_global(VM*, u32 pc);
i64 asm_slow_path_set_global(VM*, u32 pc);
i64 asm_slow_path_concat_string(VM*, u32 pc);
i64 asm_slow_path_copy_object_excluding_properties(VM*, u32 pc);
i64 asm_slow_path_exp(VM*, u32 pc);
i64 asm_slow_path_import_call(VM*, u32 pc);
i64 asm_slow_path_new_class(VM*, u32 pc);
i64 asm_slow_path_call(VM*, u32 pc);
i64 asm_slow_path_call_direct_eval(VM*, u32 pc);
i64 asm_slow_path_call_with_argument_array(VM*, u32 pc);
i64 asm_slow_path_call_direct_eval_with_argument_array(VM*, u32 pc);
#define DECLARE_CALL_BUILTIN_SLOW_PATH(name, snake_case_name, ...) \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM*, u32 pc);
JS_ENUMERATE_BUILTINS(DECLARE_CALL_BUILTIN_SLOW_PATH)
#undef DECLARE_CALL_BUILTIN_SLOW_PATH
i64 asm_slow_path_get_object_property_iterator(VM*, u32 pc);
i64 asm_slow_path_object_property_iterator_next(VM*, u32 pc);
i64 asm_slow_path_iterator_close(VM*, u32 pc);
i64 asm_slow_path_iterator_next(VM*, u32 pc);
i64 asm_slow_path_iterator_next_unpack(VM*, u32 pc);
i64 asm_slow_path_iterator_to_array(VM*, u32 pc);
i64 asm_slow_path_call_construct(VM*, u32 pc);
i64 asm_slow_path_call_construct_with_argument_array(VM*, u32 pc);
i64 asm_slow_path_super_call_with_argument_array(VM*, u32 pc);
i64 asm_slow_path_new_object(VM*, u32 pc);
i64 asm_slow_path_new_object_with_no_prototype(VM*, u32 pc);
i64 asm_slow_path_cache_object_shape(VM*, u32 pc);
i64 asm_slow_path_init_object_literal_property(VM*, u32 pc);
i64 asm_slow_path_new_array(VM*, u32 pc);
i64 asm_slow_path_new_primitive_array(VM*, u32 pc);
i64 asm_slow_path_new_regexp(VM*, u32 pc);
i64 asm_slow_path_new_reference_error(VM*, u32 pc);
i64 asm_slow_path_new_type_error(VM*, u32 pc);
i64 asm_slow_path_bitwise_xor(VM*, u32 pc);
i64 asm_slow_path_bitwise_and(VM*, u32 pc);
i64 asm_slow_path_bitwise_or(VM*, u32 pc);
i64 asm_slow_path_left_shift(VM*, u32 pc);
i64 asm_slow_path_right_shift(VM*, u32 pc);
i64 asm_slow_path_unsigned_right_shift(VM*, u32 pc);
i64 asm_slow_path_mod(VM*, u32 pc);
i64 asm_slow_path_strictly_equals(VM*, u32 pc);
i64 asm_slow_path_strictly_inequals(VM*, u32 pc);
i64 asm_slow_path_unary_minus(VM*, u32 pc);
i64 asm_slow_path_typeof(VM*, u32 pc);
i64 asm_slow_path_to_string(VM*, u32 pc);
i64 asm_slow_path_to_primitive_with_string_hint(VM*, u32 pc);
i64 asm_slow_path_to_object(VM*, u32 pc);
i64 asm_slow_path_to_length(VM*, u32 pc);
i64 asm_slow_path_postfix_decrement(VM*, u32 pc);
i64 asm_slow_path_to_int32(VM*, u32 pc);
i64 asm_slow_path_put_by_value(VM*, u32 pc);
i64 asm_slow_path_put_by_value_with_this(VM*, u32 pc);
i64 asm_slow_path_put_by_spread(VM*, u32 pc);
i64 asm_try_put_by_value_holey_array(VM*, u32 pc);
u64 asm_helper_to_boolean(u64 encoded_value);
u64 asm_helper_math_exp(u64 encoded_value);
u64 asm_helper_empty_string(u64);
u64 asm_helper_single_ascii_character_string(u64 encoded_value);
u64 asm_helper_single_utf16_code_unit_string(u64 encoded_value);
i64 asm_helper_handle_raw_native_exception(u64 encoded_exception);
i64 asm_try_inline_call(VM*, u32 pc);
i64 asm_try_put_by_id_cache(VM*, u32 pc);
i64 asm_try_get_by_id_cache(VM*, u32 pc);
i64 asm_slow_path_initialize_lexical_binding(VM*, u32 pc);
i64 asm_slow_path_dynamic_initialize_lexical_binding(VM*, u32 pc);
i64 asm_slow_path_initialize_variable_binding(VM*, u32 pc);
i64 asm_slow_path_dynamic_initialize_variable_binding(VM*, u32 pc);

i64 asm_try_get_by_value_typed_array(VM*, u32 pc);
i64 asm_slow_path_get_initialized_binding(VM*, u32 pc);
i64 asm_slow_path_dynamic_get_initialized_binding(VM*, u32 pc);
i64 asm_slow_path_get_binding(VM*, u32 pc);
i64 asm_slow_path_dynamic_get_binding(VM*, u32 pc);
i64 asm_slow_path_set_lexical_binding(VM*, u32 pc);
i64 asm_slow_path_dynamic_set_lexical_binding(VM*, u32 pc);
i64 asm_slow_path_set_variable_binding(VM*, u32 pc);
i64 asm_slow_path_dynamic_set_variable_binding(VM*, u32 pc);
i64 asm_slow_path_resolve_binding(VM*, u32 pc);
i64 asm_slow_path_resolve_super_base(VM*, u32 pc);
i64 asm_slow_path_set_resolved_binding(VM*, u32 pc);
i64 asm_slow_path_typeof_binding(VM*, u32 pc);
i64 asm_slow_path_dynamic_typeof_binding(VM*, u32 pc);
i64 asm_slow_path_has_private_id(VM*, u32 pc);
i64 asm_slow_path_set_function_name(VM*, u32 pc);
i64 asm_slow_path_new_array_with_length(VM*, u32 pc);
i64 asm_slow_path_array_append(VM*, u32 pc);
i64 asm_slow_path_create_variable(VM*, u32 pc);
i64 asm_slow_path_enter_object_environment(VM*, u32 pc);
i64 asm_slow_path_bitwise_not(VM*, u32 pc);
i64 asm_slow_path_unary_plus(VM*, u32 pc);
i64 asm_slow_path_is_callable(VM*, u32 pc);
i64 asm_slow_path_is_constructor(VM*, u32 pc);
i64 asm_slow_path_add_private_name(VM*, u32 pc);
i64 asm_slow_path_create_async_from_sync_iterator(VM*, u32 pc);
i64 asm_slow_path_create_data_property_or_throw(VM*, u32 pc);
i64 asm_slow_path_create_immutable_binding(VM*, u32 pc);
i64 asm_slow_path_create_mutable_binding(VM*, u32 pc);
i64 asm_slow_path_create_rest_params(VM*, u32 pc);
i64 asm_slow_path_create_arguments(VM*, u32 pc);
i64 asm_slow_path_await(VM*, u32 pc);
i64 asm_slow_path_create_lexical_environment(VM*, u32 pc);
i64 asm_slow_path_create_private_environment(VM*, u32 pc);
i64 asm_slow_path_create_variable_environment(VM*, u32 pc);
i64 asm_slow_path_delete_by_id(VM*, u32 pc);
i64 asm_slow_path_delete_by_value(VM*, u32 pc);
i64 asm_slow_path_delete_variable(VM*, u32 pc);
i64 asm_slow_path_get_completion_fields(VM*, u32 pc);
i64 asm_slow_path_set_completion_type(VM*, u32 pc);
i64 asm_slow_path_get_template_object(VM*, u32 pc);
i64 asm_slow_path_new_function(VM*, u32 pc);
i64 asm_slow_path_leave_private_environment(VM*, u32 pc);
i64 asm_slow_path_throw(VM*, u32 pc);
i64 asm_slow_path_throw_if_tdz(VM*, u32 pc);
i64 asm_slow_path_throw_if_not_object(VM*, u32 pc);
i64 asm_slow_path_throw_if_nullish(VM*, u32 pc);
i64 asm_slow_path_throw_const_assignment(VM*, u32 pc);
i64 asm_slow_path_yield(VM*, u32 pc);
i64 asm_slow_path_yield_iterator_result(VM*, u32 pc);
i64 asm_slow_path_loosely_equals(VM*, u32 pc);
i64 asm_slow_path_loosely_inequals(VM*, u32 pc);
i64 asm_slow_path_get_callee_and_this(VM*, u32 pc);
i64 asm_slow_path_dynamic_get_callee_and_this(VM*, u32 pc);
i64 asm_try_put_by_value_typed_array(VM*, u32 pc);
i64 asm_slow_path_get_private_by_id(VM*, u32 pc);
i64 asm_slow_path_put_private_by_id(VM*, u32 pc);
i64 asm_slow_path_instance_of(VM*, u32 pc);
i64 asm_slow_path_in(VM*, u32 pc);
i64 asm_slow_path_resolve_this_binding(VM*, u32 pc);

// ===== Fallback handler for opcodes without DSL handlers =====
// NB: Opcodes with DSL handlers are dispatched directly and never reach here.
i64 asm_fallback_handler(VM* vm, u32 pc)
{
    auto& ctx = vm->running_execution_context();
    ctx.program_counter = pc;
    auto* bytecode = ctx.executable->bytecode.data();
    auto& insn = *reinterpret_cast<Instruction const*>(&bytecode[pc]);
#ifdef JS_ASMINT_SLOW_PATH_COUNTERS
    ++s_stats.fallback_by_type[to_underlying(insn.type())];
#endif

    switch (insn.type()) {
    // Throwing instructions
    default:
        VERIFY_NOT_REACHED();
    }
}

// ===== Specific slow paths for asm-optimized instructions =====
// These are called from asm handlers when the fast path fails.
// Convention: i64 func(VM*, u32 pc)
//   Returns >= 0: new pc
//   Returns < 0: exit

i64 asm_slow_path_add(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Add>(*vm, pc);
}

i64 asm_slow_path_sub(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Sub>(*vm, pc);
}

i64 asm_slow_path_mul(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Mul>(*vm, pc);
}

i64 asm_slow_path_div(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Div>(*vm, pc);
}

i64 asm_slow_path_less_than(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LessThan>(*vm, pc);
}

i64 asm_slow_path_less_than_equals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LessThanEquals>(*vm, pc);
}

i64 asm_slow_path_greater_than(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GreaterThan>(*vm, pc);
}

i64 asm_slow_path_greater_than_equals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GreaterThanEquals>(*vm, pc);
}

i64 asm_slow_path_increment(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Increment>(*vm, pc);
}

i64 asm_slow_path_decrement(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Decrement>(*vm, pc);
}

// Comparison jump slow paths - these are terminators (execute_impl returns void),
// so they need custom handling instead of the generic slow_path_throwing template.
#define DEFINE_JUMP_COMPARISON_SLOW_PATH(snake_name, op_name, compare_call)      \
    i64 asm_slow_path_jump_##snake_name(VM* vm, u32 pc)                          \
    {                                                                            \
        bump_slow_path(*vm, pc);                                                 \
        auto* bytecode = vm->current_executable().bytecode.data();               \
        auto& insn = *reinterpret_cast<Op::Jump##op_name const*>(&bytecode[pc]); \
        auto lhs = vm->get(insn.lhs());                                          \
        auto rhs = vm->get(insn.rhs());                                          \
        if (ASM_TRY(*vm, pc, compare_call))                                      \
            return static_cast<i64>(insn.true_target().address());               \
        return static_cast<i64>(insn.false_target().address());                  \
    }

DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than, LessThan, less_than(VM::the(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than, GreaterThan, greater_than(VM::the(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than_equals, LessThanEquals, less_than_equals(VM::the(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than_equals, GreaterThanEquals, greater_than_equals(VM::the(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(loosely_equals, LooselyEquals, is_loosely_equal(VM::the(), lhs, rhs))
#undef DEFINE_JUMP_COMPARISON_SLOW_PATH

i64 asm_slow_path_jump_loosely_inequals(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::JumpLooselyInequals const*>(&bytecode[pc]);
    auto lhs = vm->get(insn.lhs());
    auto rhs = vm->get(insn.rhs());
    if (!ASM_TRY(*vm, pc, is_loosely_equal(VM::the(), lhs, rhs)))
        return static_cast<i64>(insn.true_target().address());
    return static_cast<i64>(insn.false_target().address());
}

i64 asm_slow_path_jump_strictly_equals(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::JumpStrictlyEquals const*>(&bytecode[pc]);
    auto lhs = vm->get(insn.lhs());
    auto rhs = vm->get(insn.rhs());
    if (is_strictly_equal(lhs, rhs))
        return static_cast<i64>(insn.true_target().address());
    return static_cast<i64>(insn.false_target().address());
}

i64 asm_slow_path_jump_strictly_inequals(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::JumpStrictlyInequals const*>(&bytecode[pc]);
    auto lhs = vm->get(insn.lhs());
    auto rhs = vm->get(insn.rhs());
    if (!is_strictly_equal(lhs, rhs))
        return static_cast<i64>(insn.true_target().address());
    return static_cast<i64>(insn.false_target().address());
}

// ===== Dedicated slow paths for hot instructions =====

i64 asm_slow_path_set_lexical_environment(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SetLexicalEnvironment const*>(&bytecode[pc]);
    vm->running_execution_context().lexical_environment = &as<Environment>(vm->get(insn.environment()).as_cell());
    return static_cast<i64>(pc + sizeof(Op::SetLexicalEnvironment));
}

i64 asm_slow_path_get_initialized_binding(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetInitializedBinding>(*vm, pc);
}

i64 asm_slow_path_dynamic_get_initialized_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DynamicGetInitializedBinding const*>(&bytecode[pc]);
    auto& cache = vm->current_executable().environment_coordinate_caches[insn.cache()];
    auto next_pc = asm_dynamic_get_binding<AsmBindingIsKnownToBeInitialized::Yes>(*vm, pc, insn.dst(), insn.identifier(), insn.strict(), cache);
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::DynamicGetInitializedBinding));
}

i64 asm_slow_path_loosely_equals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LooselyEquals>(*vm, pc);
}

i64 asm_slow_path_loosely_inequals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LooselyInequals>(*vm, pc);
}

i64 asm_slow_path_get_callee_and_this(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetCalleeAndThisFromEnvironment>(*vm, pc);
}

i64 asm_slow_path_dynamic_get_callee_and_this(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DynamicGetCalleeAndThisFromEnvironment const*>(&bytecode[pc]);
    auto& cache = vm->current_executable().environment_coordinate_caches[insn.cache()];
    auto next_pc = asm_dynamic_get_callee_and_this_from_environment(*vm, pc, insn.callee(), insn.this_value(), insn.identifier(), insn.strict(), cache);
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::DynamicGetCalleeAndThisFromEnvironment));
}

i64 asm_slow_path_postfix_increment(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::PostfixIncrement>(*vm, pc);
}

i64 asm_slow_path_get_by_id(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetById const*>(&bytecode[pc]);
    auto base_value = vm->get(insn.base());
    auto& cache = vm->current_executable().property_lookup_caches[insn.cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Normal>(*vm, [&] { return vm->get_identifier(insn.base_identifier()); }, [&] -> PropertyKey const& { return vm->get_property_key(insn.property()); }, base_value, base_value, cache));
    vm->set(insn.dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetById));
}

i64 asm_slow_path_get_by_id_with_this(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetByIdWithThis const*>(&bytecode[pc]);
    auto base_value = vm->get(insn.base());
    auto this_value = vm->get(insn.this_value());
    auto& cache = vm->current_executable().property_lookup_caches[insn.cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Normal>(*vm, [] { return Optional<Utf16FlyString const&> {}; }, [&] -> PropertyKey const& { return vm->get_property_key(insn.property()); }, base_value, this_value, cache));
    vm->set(insn.dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetByIdWithThis));
}

i64 asm_slow_path_put_by_id(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::PutById>(*vm, pc);
}

i64 asm_slow_path_put_by_id_with_this(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutByIdWithThis const*>(&bytecode[pc]);
    auto value = vm->get(insn.src());
    auto base = vm->get(insn.base());
    auto const& name = vm->get_property_key(insn.property());
    auto& cache = vm->current_executable().property_lookup_caches[insn.cache()];
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, vm->get(insn.this_value()), value, {}, name, insn.kind(), insn.strict(), &cache));
    return static_cast<i64>(pc + sizeof(Op::PutByIdWithThis));
}

i64 asm_slow_path_get_by_value(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetByValue>(*vm, pc);
}

i64 asm_slow_path_get_by_value_with_this(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetByValueWithThis const*>(&bytecode[pc]);
    auto property_key_value = vm->get(insn.property());
    auto object = ASM_TRY(*vm, pc, vm->get(insn.base()).to_object(*vm));
    auto property_key = ASM_TRY(*vm, pc, property_key_value.to_property_key(*vm));
    auto value = ASM_TRY(*vm, pc, object->internal_get(property_key, vm->get(insn.this_value())));
    vm->set(insn.dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetByValueWithThis));
}

i64 asm_slow_path_get_length(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetLength const*>(&bytecode[pc]);
    auto base_value = vm->get(insn.base());
    auto& executable = vm->current_executable();
    auto& cache = executable.property_lookup_caches[insn.cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Length>(*vm, [&] { return vm->get_identifier(insn.base_identifier()); }, [&] -> PropertyKey const& { return executable.get_property_key(*executable.length_identifier); }, base_value, base_value, cache));
    vm->set(insn.dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetLength));
}

i64 asm_slow_path_get_length_with_this(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetLengthWithThis const*>(&bytecode[pc]);
    auto base_value = vm->get(insn.base());
    auto this_value = vm->get(insn.this_value());
    auto& executable = vm->current_executable();
    auto& cache = executable.property_lookup_caches[insn.cache()];
    auto value = ASM_TRY(*vm, pc, get_by_id<GetByIdMode::Length>(*vm, [] { return Optional<Utf16FlyString const&> {}; }, [&] -> PropertyKey const& { return executable.get_property_key(*executable.length_identifier); }, base_value, this_value, cache));
    vm->set(insn.dst(), value);
    return static_cast<i64>(pc + sizeof(Op::GetLengthWithThis));
}

i64 asm_slow_path_get_method(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetMethod const*>(&bytecode[pc]);
    auto const& property_key = vm->get_property_key(insn.property());
    auto method = ASM_TRY(*vm, pc, vm->get(insn.object()).get_method(*vm, property_key));
    vm->set(insn.dst(), method ?: js_undefined());
    return static_cast<i64>(pc + sizeof(Op::GetMethod));
}

i64 asm_slow_path_get_iterator(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetIterator const*>(&bytecode[pc]);
    auto iterator_record = ASM_TRY(*vm, pc, get_iterator_impl(*vm, vm->get(insn.iterable()), insn.hint()));
    vm->set(insn.dst_iterator_object(), iterator_record.iterator);
    vm->set(insn.dst_iterator_next(), iterator_record.next_method);
    vm->set(insn.dst_iterator_done(), Value(iterator_record.done));
    return static_cast<i64>(pc + sizeof(Op::GetIterator));
}

i64 asm_slow_path_get_import_meta(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetImportMeta const*>(&bytecode[pc]);
    vm->set(insn.dst(), vm->get_import_meta());
    return static_cast<i64>(pc + sizeof(Op::GetImportMeta));
}

i64 asm_slow_path_get_new_target(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetNewTarget const*>(&bytecode[pc]);
    vm->set(insn.dst(), vm->get_new_target());
    return static_cast<i64>(pc + sizeof(Op::GetNewTarget));
}

i64 asm_slow_path_get_super_constructor(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetSuperConstructor const*>(&bytecode[pc]);
    auto* super_constructor = get_super_constructor(*vm);
    vm->set(insn.dst(), super_constructor ? Value(super_constructor) : js_null());
    return static_cast<i64>(pc + sizeof(Op::GetSuperConstructor));
}

i64 asm_try_get_global_env_binding(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetGlobal const*>(&bytecode[pc]);
    auto& cache = vm->current_executable().global_variable_caches[insn.cache()];

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
    vm->set(insn.dst(), result.value());
    return 0;
}

i64 asm_slow_path_get_global(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetGlobal>(*vm, pc);
}

i64 asm_try_set_global_env_binding(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SetGlobal const*>(&bytecode[pc]);
    auto& cache = vm->current_executable().global_variable_caches[insn.cache()];

    if (!cache.has_environment_binding_index) [[unlikely]]
        return 1;

    auto& current_vm = *vm;
    auto src = vm->get(insn.src());
    ThrowCompletionOr<void> result;
    if (cache.in_module_environment) {
        auto module = current_vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
        if (!module) [[unlikely]]
            return 1;
        result = (*module)->environment()->set_mutable_binding_direct(current_vm, cache.environment_binding_index, src, insn.strict() == Strict::Yes);
    } else {
        result = vm->global_declarative_environment().set_mutable_binding_direct(current_vm, cache.environment_binding_index, src, insn.strict() == Strict::Yes);
    }
    if (result.is_error()) [[unlikely]]
        return 1;
    return 0;
}

i64 asm_slow_path_set_global(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::SetGlobal>(*vm, pc);
}

i64 asm_slow_path_concat_string(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ConcatString const*>(&bytecode[pc]);
    auto string = ASM_TRY(*vm, pc, vm->get(insn.src()).to_primitive_string(*vm));
    vm->set(insn.dst(), PrimitiveString::create(*vm, vm->get(insn.dst()).as_string(), string));
    return static_cast<i64>(pc + sizeof(Op::ConcatString));
}

i64 asm_slow_path_copy_object_excluding_properties(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CopyObjectExcludingProperties const*>(&bytecode[pc]);
    auto& realm = *vm->current_realm();
    auto from_object = vm->get(insn.from_object());
    auto to_object = Object::create(realm, realm.intrinsics().object_prototype());

    GC::ConservativeHashTable<PropertyKey> excluded_names;
    auto excluded_names_operands = insn.excluded_names();
    for (size_t i = 0; i < insn.excluded_names_count(); ++i)
        excluded_names.set(ASM_TRY(*vm, pc, vm->get(excluded_names_operands[i]).to_property_key(*vm)));

    ASM_TRY(*vm, pc, to_object->copy_data_properties(*vm, from_object, excluded_names));
    vm->set(insn.dst(), to_object);
    return static_cast<i64>(pc + insn.length());
}

i64 asm_slow_path_exp(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::Exp const*>(&bytecode[pc]);
    auto result = ASM_TRY(*vm, pc, exp(*vm, vm->get(insn.lhs()), vm->get(insn.rhs())));
    vm->set(insn.dst(), result);
    return static_cast<i64>(pc + sizeof(Op::Exp));
}

i64 asm_slow_path_import_call(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ImportCall const*>(&bytecode[pc]);
    auto specifier = vm->get(insn.specifier());
    auto options_value = vm->get(insn.options());
    vm->set(insn.dst(), ASM_TRY(*vm, pc, perform_import_call(*vm, specifier, options_value)));
    return static_cast<i64>(pc + sizeof(Op::ImportCall));
}

i64 asm_slow_path_new_class(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::NewClass const*>(&bytecode[pc]);

    Value super_class;
    if (insn.super_class().has_value())
        super_class = vm->get(insn.super_class().value());
    GC::RootVector<Value> element_keys;
    element_keys.ensure_capacity(insn.element_keys_count());
    for (size_t i = 0; i < insn.element_keys_count(); ++i) {
        Value element_key;
        if (insn.element_keys()[i].has_value())
            element_key = vm->get(insn.element_keys()[i].value());
        element_keys.unchecked_append(element_key);
    }

    auto& running_execution_context = vm->running_execution_context();
    auto* class_environment = &as<Environment>(vm->get(insn.class_environment()).as_cell());
    auto& outer_environment = running_execution_context.lexical_environment;

    auto const& blueprint = vm->current_executable().class_blueprints[insn.class_blueprint_index()];

    Optional<Utf16FlyString> binding_name;
    Utf16FlyString class_name;
    if (!blueprint.has_name && insn.lhs_name().has_value()) {
        class_name = vm->get_identifier(insn.lhs_name().value());
    } else {
        class_name = blueprint.name;
        binding_name = class_name;
    }

    auto* retval = ASM_TRY(*vm, pc, construct_class(*vm, blueprint, vm->current_executable(), class_environment, outer_environment, super_class, element_keys, binding_name, class_name));
    vm->set(insn.dst(), retval);
    return static_cast<i64>(pc + insn.length());
}

i64 asm_slow_path_call(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Call>(*vm, pc);
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

i64 asm_slow_path_call_direct_eval(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CallDirectEval const*>(&bytecode[pc]);
    ASM_TRY(*vm, pc, call_direct_eval(*vm, vm->get(insn.callee()), vm->get(insn.this_value()), insn.arguments(), insn.dst(), insn.expression_string(), insn.strict()));
    return static_cast<i64>(pc + insn.length());
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
    ScopeGuard deallocate_guard = [&stack, stack_mark] { stack.deallocate(stack_mark); };

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

i64 asm_slow_path_call_with_argument_array(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CallWithArgumentArray const*>(&bytecode[pc]);
    ASM_TRY(*vm, pc, call_with_argument_array(Op::CallType::Call, *vm, vm->get(insn.callee()), vm->get(insn.this_value()), vm->get(insn.arguments()), insn.dst(), insn.expression_string(), insn.strict()));
    return static_cast<i64>(pc + sizeof(Op::CallWithArgumentArray));
}

i64 asm_slow_path_call_direct_eval_with_argument_array(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CallDirectEvalWithArgumentArray const*>(&bytecode[pc]);
    ASM_TRY(*vm, pc, call_with_argument_array(Op::CallType::DirectEval, *vm, vm->get(insn.callee()), vm->get(insn.this_value()), vm->get(insn.arguments()), insn.dst(), insn.expression_string(), insn.strict()));
    return static_cast<i64>(pc + sizeof(Op::CallDirectEvalWithArgumentArray));
}

i64 asm_slow_path_get_object_property_iterator(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetObjectPropertyIterator const*>(&bytecode[pc]);
    auto* cache = &vm->current_executable().object_property_iterator_caches[insn.cache()];
    vm->set(insn.dst_iterator(), ASM_TRY(*vm, pc, asm_get_object_property_iterator(*vm, vm->get(insn.object()), cache)));
    return static_cast<i64>(pc + sizeof(Op::GetObjectPropertyIterator));
}

i64 asm_slow_path_object_property_iterator_next(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ObjectPropertyIteratorNext const*>(&bytecode[pc]);
    auto& iterator = static_cast<PropertyNameIterator&>(vm->get(insn.iterator_object()).as_object());
    Value value;
    bool done = false;
    ASM_TRY(*vm, pc, iterator.next(*vm, done, value));
    vm->set(insn.dst_done(), Value(done));
    if (!done)
        vm->set(insn.dst_value(), value);
    return static_cast<i64>(pc + sizeof(Op::ObjectPropertyIteratorNext));
}

i64 asm_slow_path_iterator_close(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::IteratorClose const*>(&bytecode[pc]);
    auto& iterator_object = vm->get(insn.iterator_object()).as_object();
    auto iterator_next_method = vm->get(insn.iterator_next());
    auto iterator_done_property = vm->get(insn.iterator_done()).as_bool();
    IteratorRecordImpl iterator_record { .done = iterator_done_property, .iterator = iterator_object, .next_method = iterator_next_method };

    ASM_TRY(*vm, pc, iterator_close(*vm, iterator_record, Completion { insn.completion_type(), vm->get(insn.completion_value()) }));
    return static_cast<i64>(pc + sizeof(Op::IteratorClose));
}

i64 asm_slow_path_iterator_next(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::IteratorNext const*>(&bytecode[pc]);
    auto& iterator_object = vm->get(insn.iterator_object()).as_object();
    auto iterator_next_method = vm->get(insn.iterator_next());
    auto iterator_done_property = vm->get(insn.iterator_done()).as_bool();
    IteratorRecordImpl iterator_record { .done = iterator_done_property, .iterator = iterator_object, .next_method = iterator_next_method };
    auto result = iterator_next(*vm, iterator_record);
    if (iterator_record.done)
        vm->set(insn.iterator_done(), Value(true));
    vm->set(insn.dst(), ASM_TRY(*vm, pc, result));
    return static_cast<i64>(pc + sizeof(Op::IteratorNext));
}

i64 asm_slow_path_iterator_next_unpack(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::IteratorNextUnpack const*>(&bytecode[pc]);
    auto& iterator_object = vm->get(insn.iterator_object()).as_object();
    auto iterator_next_method = vm->get(insn.iterator_next());
    auto iterator_done_property = vm->get(insn.iterator_done()).as_bool();
    IteratorRecordImpl iterator_record { .done = iterator_done_property, .iterator = iterator_object, .next_method = iterator_next_method };
    auto iteration_result_or_done_or_error = iterator_step(*vm, iterator_record);
    if (iterator_record.done)
        vm->set(insn.iterator_done(), Value(true));
    auto iteration_result_or_done = ASM_TRY(*vm, pc, iteration_result_or_done_or_error);
    if (iteration_result_or_done.has<IterationDone>()) {
        vm->set(insn.dst_done(), Value(true));
        return static_cast<i64>(pc + sizeof(Op::IteratorNextUnpack));
    }
    auto& iteration_result = iteration_result_or_done.get<IterationResult>();
    vm->set(insn.dst_done(), ASM_TRY(*vm, pc, iteration_result.done));
    auto value = move(iteration_result.value);
    if (value.is_throw_completion())
        vm->set(insn.iterator_done(), Value(true));
    vm->set(insn.dst_value(), ASM_TRY(*vm, pc, value));
    return static_cast<i64>(pc + sizeof(Op::IteratorNextUnpack));
}

i64 asm_slow_path_iterator_to_array(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::IteratorToArray const*>(&bytecode[pc]);
    IteratorRecordImpl iterator_record {
        .done = vm->get(insn.iterator_done_property()).as_bool(),
        .iterator = vm->get(insn.iterator_object()).as_object(),
        .next_method = vm->get(insn.iterator_next_method())
    };

    auto array = MUST(JS::Array::create(*vm->current_realm(), 0));
    size_t index = 0;
    while (true) {
        auto value_or_error = iterator_step_value(*vm, iterator_record);
        if (iterator_record.done)
            vm->set(insn.iterator_done_property(), Value(true));
        auto value = ASM_TRY(*vm, pc, value_or_error);
        if (!value.has_value()) {
            vm->set(insn.dst(), array);
            return static_cast<i64>(pc + sizeof(Op::IteratorToArray));
        }

        MUST(array->create_data_property_or_throw(index, value.release_value()));
        ++index;
    }
}

#define DEFINE_CALL_BUILTIN_SLOW_PATH(name, snake_case_name, ...)    \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM* vm, u32 pc) \
    {                                                                \
        return slow_path_throwing<Op::CallBuiltin##name>(*vm, pc);   \
    }
JS_ENUMERATE_BUILTINS(DEFINE_CALL_BUILTIN_SLOW_PATH)
#undef DEFINE_CALL_BUILTIN_SLOW_PATH

i64 asm_slow_path_call_construct(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::CallConstruct>(*vm, pc);
}

i64 asm_slow_path_call_construct_with_argument_array(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CallConstructWithArgumentArray const*>(&bytecode[pc]);
    ASM_TRY(*vm, pc, call_with_argument_array(Op::CallType::Construct, *vm, vm->get(insn.callee()), js_undefined(), vm->get(insn.arguments()), insn.dst(), insn.expression_string(), insn.strict()));
    return static_cast<i64>(pc + sizeof(Op::CallConstructWithArgumentArray));
}

i64 asm_slow_path_super_call_with_argument_array(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SuperCallWithArgumentArray const*>(&bytecode[pc]);

    auto new_target = vm->get_new_target();
    VERIFY(new_target.is_object());

    auto super_constructor = vm->get(insn.super_constructor());
    if (!super_constructor.is_constructor()) [[unlikely]] {
        vm->running_execution_context().program_counter = pc;
        auto completion = vm->throw_completion<TypeError>(ErrorType::NotAConstructor, "Super constructor");
        return handle_asm_exception(*vm, pc, completion.value());
    }

    auto& function = super_constructor.as_function();

    auto& argument_array = vm->get(insn.arguments()).as_array_exotic_object();
    size_t argument_array_length = 0;

    if (insn.is_synthetic()) {
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
    ScopeGuard deallocate_guard = [&stack, stack_mark] { stack.deallocate(stack_mark); };

    auto* callee_context_argument_values = callee_context->arguments_data();
    auto const callee_context_argument_count = callee_context->argument_count;
    auto const insn_argument_count = argument_array_length;

    if (insn.is_synthetic()) {
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

    vm->set(insn.dst(), result);
    return static_cast<i64>(pc + sizeof(Op::SuperCallWithArgumentArray));
}

i64 asm_slow_path_new_object(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::NewObject const*>(&bytecode[pc]);
    auto& realm = *vm->current_realm();

    if (insn.cache() != NumericLimits<u32>::max()) {
        auto& cache = vm->current_executable().object_shape_caches[insn.cache()];
        auto cached_shape = cache.shape.ptr();
        if (cached_shape) {
            vm->set(insn.dst(), Object::create_with_premade_shape(*cached_shape));
            return static_cast<i64>(pc + sizeof(Op::NewObject));
        }
    }

    vm->set(insn.dst(), Object::create(realm, realm.intrinsics().object_prototype()));
    return static_cast<i64>(pc + sizeof(Op::NewObject));
}

i64 asm_slow_path_new_object_with_no_prototype(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::NewObjectWithNoPrototype const*>(&bytecode[pc]);
    auto& realm = *vm->current_realm();
    vm->set(insn.dst(), Object::create(realm, nullptr));
    return static_cast<i64>(pc + sizeof(Op::NewObjectWithNoPrototype));
}

i64 asm_slow_path_cache_object_shape(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CacheObjectShape const*>(&bytecode[pc]);
    auto& cache = vm->current_executable().object_shape_caches[insn.cache()];
    if (!cache.shape) {
        auto& object = vm->get(insn.object()).as_object();
        if (!object.shape().is_dictionary())
            cache.shape = &object.shape();
    }
    return static_cast<i64>(pc + sizeof(Op::CacheObjectShape));
}

i64 asm_slow_path_init_object_literal_property(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::InitObjectLiteralProperty const*>(&bytecode[pc]);
    auto& object = vm->get(insn.object()).as_object();
    auto value = vm->get(insn.src());
    auto& cache = vm->current_executable().object_shape_caches[insn.shape_cache_index()];

    auto cached_shape = cache.shape.ptr();
    if (cached_shape && &object.shape() == cached_shape && insn.property_slot() < cache.property_offsets.size()) {
        object.put_direct(cache.property_offsets[insn.property_slot()], value);
        return static_cast<i64>(pc + sizeof(Op::InitObjectLiteralProperty));
    }

    auto const& property_key = vm->current_executable().get_property_key(insn.property());
    object.define_direct_property(property_key, value, JS::Attribute::Enumerable | JS::Attribute::Writable | JS::Attribute::Configurable);

    if (!object.shape().is_dictionary()) {
        auto metadata = object.shape().lookup(property_key);
        if (metadata.has_value()) {
            if (insn.property_slot() >= cache.property_offsets.size())
                cache.property_offsets.resize(insn.property_slot() + 1);
            cache.property_offsets[insn.property_slot()] = metadata->offset;
        }
    }

    return static_cast<i64>(pc + sizeof(Op::InitObjectLiteralProperty));
}

i64 asm_slow_path_new_array(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& typed = *reinterpret_cast<Op::NewArray const*>(&bytecode[pc]);
    typed.execute_impl(*vm);
    return static_cast<i64>(pc + typed.length());
}

i64 asm_slow_path_new_primitive_array(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::NewPrimitiveArray const*>(&bytecode[pc]);
    auto array = MUST(JS::Array::create(vm->realm(), insn.element_count()));
    for (size_t i = 0; i < insn.element_count(); ++i)
        array->indexed_put(i, insn.elements()[i]);
    vm->set(insn.dst(), array);
    return static_cast<i64>(pc + insn.length());
}

i64 asm_slow_path_new_regexp(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::NewRegExp const*>(&bytecode[pc]);
    auto& realm = *vm->current_realm();
    auto regexp_object = RegExpObject::create(
        realm,
        vm->current_executable().get_string(insn.source_index()),
        vm->current_executable().get_string(insn.flags_index()));
    regexp_object->set_realm(realm);
    regexp_object->set_legacy_features_enabled(true);
    vm->set(insn.dst(), regexp_object);
    return static_cast<i64>(pc + sizeof(Op::NewRegExp));
}

i64 asm_slow_path_new_reference_error(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::NewReferenceError const*>(&bytecode[pc]);
    auto& realm = *vm->current_realm();
    vm->set(insn.dst(), ReferenceError::create(realm, vm->current_executable().get_string(insn.error_string())));
    return static_cast<i64>(pc + sizeof(Op::NewReferenceError));
}

i64 asm_slow_path_new_type_error(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::NewTypeError const*>(&bytecode[pc]);
    auto& realm = *vm->current_realm();
    vm->set(insn.dst(), TypeError::create(realm, vm->current_executable().get_string(insn.error_string())));
    return static_cast<i64>(pc + sizeof(Op::NewTypeError));
}

i64 asm_slow_path_bitwise_xor(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::BitwiseXor>(*vm, pc);
}

i64 asm_slow_path_bitwise_and(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::BitwiseAnd>(*vm, pc);
}

i64 asm_slow_path_bitwise_or(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::BitwiseOr>(*vm, pc);
}

i64 asm_slow_path_left_shift(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LeftShift>(*vm, pc);
}

i64 asm_slow_path_right_shift(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::RightShift>(*vm, pc);
}

i64 asm_slow_path_unsigned_right_shift(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::UnsignedRightShift>(*vm, pc);
}

i64 asm_slow_path_mod(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Mod>(*vm, pc);
}

i64 asm_slow_path_strictly_equals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::StrictlyEquals>(*vm, pc);
}

i64 asm_slow_path_strictly_inequals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::StrictlyInequals>(*vm, pc);
}

i64 asm_slow_path_unary_minus(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::UnaryMinus>(*vm, pc);
}

i64 asm_slow_path_to_string(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ToString const*>(&bytecode[pc]);
    auto result = ASM_TRY(*vm, pc, vm->get(insn.value()).to_primitive_string(*vm));
    vm->set(insn.dst(), Value { result });
    return static_cast<i64>(pc + sizeof(Op::ToString));
}

i64 asm_slow_path_to_primitive_with_string_hint(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ToPrimitiveWithStringHint const*>(&bytecode[pc]);
    auto result = ASM_TRY(*vm, pc, vm->get(insn.value()).to_primitive(*vm, Value::PreferredType::String));
    vm->set(insn.dst(), result);
    return static_cast<i64>(pc + sizeof(Op::ToPrimitiveWithStringHint));
}

i64 asm_slow_path_to_object(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ToObject const*>(&bytecode[pc]);
    auto result = ASM_TRY(*vm, pc, vm->get(insn.value()).to_object(*vm));
    vm->set(insn.dst(), result);
    return static_cast<i64>(pc + sizeof(Op::ToObject));
}

i64 asm_slow_path_to_length(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ToLength const*>(&bytecode[pc]);
    auto result = ASM_TRY(*vm, pc, vm->get(insn.value()).to_length(*vm));
    vm->set(insn.dst(), Value { result });
    return static_cast<i64>(pc + sizeof(Op::ToLength));
}

i64 asm_slow_path_typeof(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::Typeof const*>(&bytecode[pc]);
    vm->set(insn.dst(), vm->get(insn.src()).typeof_(*vm));
    return static_cast<i64>(pc + sizeof(Op::Typeof));
}

i64 asm_slow_path_postfix_decrement(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::PostfixDecrement>(*vm, pc);
}

i64 asm_slow_path_to_int32(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::ToInt32>(*vm, pc);
}

i64 asm_slow_path_put_by_value(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::PutByValue>(*vm, pc);
}

i64 asm_slow_path_put_by_value_with_this(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutByValueWithThis const*>(&bytecode[pc]);
    auto value = vm->get(insn.src());
    auto base = vm->get(insn.base());
    auto this_value = vm->get(insn.this_value());
    auto property_key = ASM_TRY(*vm, pc, vm->get(insn.property()).to_property_key(*vm));
    ASM_TRY(*vm, pc, put_by_property_key(*vm, base, this_value, value, {}, property_key, insn.kind(), insn.strict()));
    return static_cast<i64>(pc + sizeof(Op::PutByValueWithThis));
}

i64 asm_slow_path_put_by_spread(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutBySpread const*>(&bytecode[pc]);
    auto value = vm->get(insn.src());
    auto base = vm->get(insn.base());

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto object = ASM_TRY(*vm, pc, base.to_object(*vm));

    ASM_TRY(*vm, pc, object->copy_data_properties(*vm, value, {}));
    return static_cast<i64>(pc + sizeof(Op::PutBySpread));
}

i64 asm_try_put_by_value_holey_array(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutByValue const*>(&bytecode[pc]);

    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = vm->get(insn.property());
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

    array.indexed_put(index, vm->get(insn.src()));
    return 0;
}

// Try to inline a JS-to-JS call by building the callee frame through the
// shared VM::push_inline_frame() helper. Returns 0 on success (callee frame
// pushed) and 1 on failure (caller should keep handling the Call itself).
i64 asm_try_inline_call(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::Call const*>(&bytecode[pc]);

    auto callee = vm->get(insn.callee());
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
        insn.arguments(),
        pc + insn.length(),
        insn.dst().raw(),
        vm->get(insn.this_value()),
        nullptr,
        false);

    return callee_context ? 0 : 1;
}

// Fast cache-only PutById. Tries all cache entries for ChangeOwnProperty and
// AddOwnProperty. Returns 0 on cache hit, 1 on miss (caller should use full slow path).
i64 asm_try_put_by_id_cache(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutById const*>(&bytecode[pc]);
    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;
    auto& object = base.as_object();
    auto value = vm->get(insn.src());
    auto& cache = vm->current_executable().property_lookup_caches[insn.cache()];

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
i64 asm_try_get_by_id_cache(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetById const*>(&bytecode[pc]);
    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;
    auto& object = base.as_object();
    auto& shape = object.shape();
    auto& cache = vm->current_executable().property_lookup_caches[insn.cache()];

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
            vm->set(insn.dst(), value);
            return 0;
        } else if (&shape == entry.shape) {
            if (shape.is_dictionary()
                && shape.dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto value = object.get_direct(entry.property_offset);
            if (value.is_accessor()) [[unlikely]]
                return 1;
            vm->set(insn.dst(), value);
            return 0;
        }
    }
    return 1;
}

i64 asm_slow_path_get_binding(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetBinding>(*vm, pc);
}

i64 asm_slow_path_dynamic_get_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DynamicGetBinding const*>(&bytecode[pc]);
    auto& cache = vm->current_executable().environment_coordinate_caches[insn.cache()];
    auto next_pc = asm_dynamic_get_binding<AsmBindingIsKnownToBeInitialized::No>(*vm, pc, insn.dst(), insn.identifier(), insn.strict(), cache);
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::DynamicGetBinding));
}

i64 asm_slow_path_initialize_lexical_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::InitializeLexicalBinding const*>(&bytecode[pc]);
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Initialize>(*vm, pc, insn.strict(), vm->get(insn.src()), insn.cache());
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::InitializeLexicalBinding));
}

i64 asm_slow_path_dynamic_initialize_lexical_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DynamicInitializeLexicalBinding const*>(&bytecode[pc]);
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Initialize>(*vm, pc, insn.identifier(), insn.strict(), vm->get(insn.src()), vm->current_executable().environment_coordinate_caches[insn.cache()]);
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::DynamicInitializeLexicalBinding));
}

i64 asm_slow_path_initialize_variable_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::InitializeVariableBinding const*>(&bytecode[pc]);
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Initialize>(*vm, pc, insn.strict(), vm->get(insn.src()), insn.cache());
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::InitializeVariableBinding));
}

i64 asm_slow_path_dynamic_initialize_variable_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DynamicInitializeVariableBinding const*>(&bytecode[pc]);
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Initialize>(*vm, pc, insn.identifier(), insn.strict(), vm->get(insn.src()), vm->current_executable().environment_coordinate_caches[insn.cache()]);
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::DynamicInitializeVariableBinding));
}

i64 asm_slow_path_set_lexical_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SetLexicalBinding const*>(&bytecode[pc]);
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Set>(*vm, pc, insn.strict(), vm->get(insn.src()), insn.cache());
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::SetLexicalBinding));
}

i64 asm_slow_path_dynamic_set_lexical_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DynamicSetLexicalBinding const*>(&bytecode[pc]);
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Lexical, Op::BindingInitializationMode::Set>(*vm, pc, insn.identifier(), insn.strict(), vm->get(insn.src()), vm->current_executable().environment_coordinate_caches[insn.cache()]);
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::DynamicSetLexicalBinding));
}

i64 asm_slow_path_set_variable_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SetVariableBinding const*>(&bytecode[pc]);
    auto next_pc = asm_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Set>(*vm, pc, insn.strict(), vm->get(insn.src()), insn.cache());
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::SetVariableBinding));
}

i64 asm_slow_path_dynamic_set_variable_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DynamicSetVariableBinding const*>(&bytecode[pc]);
    auto next_pc = asm_dynamic_initialize_or_set_binding<Op::EnvironmentMode::Var, Op::BindingInitializationMode::Set>(*vm, pc, insn.identifier(), insn.strict(), vm->get(insn.src()), vm->current_executable().environment_coordinate_caches[insn.cache()]);
    if (next_pc != static_cast<i64>(pc))
        return next_pc;
    return static_cast<i64>(pc + sizeof(Op::DynamicSetVariableBinding));
}

i64 asm_slow_path_resolve_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ResolveBinding const*>(&bytecode[pc]);
    auto const& identifier = vm->get_identifier(insn.identifier());
    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(identifier, insn.strict()));
    if (reference.is_unresolvable()) {
        vm->set(insn.dst(), js_null());
        return static_cast<i64>(pc + sizeof(Op::ResolveBinding));
    }

    VERIFY(reference.is_environment_reference());
    vm->set(insn.dst(), &reference.base_environment());
    return static_cast<i64>(pc + sizeof(Op::ResolveBinding));
}

i64 asm_slow_path_resolve_super_base(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ResolveSuperBase const*>(&bytecode[pc]);

    auto& environment = as<FunctionEnvironment>(*get_this_environment(*vm));
    VERIFY(environment.has_super_binding());
    auto base_value = ASM_TRY(*vm, pc, environment.get_super_base());
    vm->set(insn.dst(), base_value);
    return static_cast<i64>(pc + sizeof(Op::ResolveSuperBase));
}

i64 asm_slow_path_set_resolved_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SetResolvedBinding const*>(&bytecode[pc]);
    auto const& identifier = vm->get_identifier(insn.identifier());
    auto environment = vm->get(insn.environment());
    auto reference = environment.is_null()
        ? Reference { Reference::BaseType::Unresolvable, PropertyKey { identifier }, insn.strict() }
        : Reference { as<Environment>(environment.as_cell()), identifier, insn.strict() };
    ASM_TRY(*vm, pc, reference.put_value(*vm, vm->get(insn.src())));
    return static_cast<i64>(pc + sizeof(Op::SetResolvedBinding));
}

i64 asm_slow_path_typeof_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::TypeofBinding const*>(&bytecode[pc]);
    VERIFY(insn.cache().is_valid());

    auto const* environment = vm->running_execution_context().lexical_environment.ptr();
    for (size_t i = 0; i < insn.cache().hops; ++i)
        environment = environment->outer_environment();

    auto value = ASM_TRY(*vm, pc, static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(*vm, insn.cache().index));
    vm->set(insn.dst(), value.typeof_(*vm));
    return static_cast<i64>(pc + sizeof(Op::TypeofBinding));
}

i64 asm_slow_path_dynamic_typeof_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DynamicTypeofBinding const*>(&bytecode[pc]);
    auto& cache = vm->current_executable().environment_coordinate_caches[insn.cache()];
    auto const* current_environment = vm->running_execution_context().lexical_environment.ptr();
    if (auto const* environment = asm_get_cached_environment(current_environment, cache)) [[likely]] {
        auto value = ASM_TRY(*vm, pc, static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(*vm, cache.index));
        vm->set(insn.dst(), value.typeof_(*vm));
        return static_cast<i64>(pc + sizeof(Op::DynamicTypeofBinding));
    }

    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(vm->get_identifier(insn.identifier()), insn.strict()));
    if (reference.is_unresolvable()) {
        vm->set(insn.dst(), PrimitiveString::create(*vm, "undefined"_string));
        return static_cast<i64>(pc + sizeof(Op::DynamicTypeofBinding));
    }

    asm_update_environment_coordinate_cache(current_environment, reference, cache);
    auto value = ASM_TRY(*vm, pc, reference.get_value(*vm));
    vm->set(insn.dst(), value.typeof_(*vm));
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

i64 asm_slow_path_has_private_id(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::HasPrivateId const*>(&bytecode[pc]);
    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]] {
        auto completion = vm->throw_completion<TypeError>(ErrorType::InOperatorWithObject);
        return handle_asm_exception(*vm, pc, completion.value());
    }

    auto private_environment = vm->running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(vm->get_identifier(insn.property()));
    vm->set(insn.dst(), Value(base.as_object().private_element_find(private_name) != nullptr));
    return static_cast<i64>(pc + sizeof(Op::HasPrivateId));
}

i64 asm_slow_path_set_function_name(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SetFunctionName const*>(&bytecode[pc]);
    auto function = vm->get(insn.function()).as_if<ECMAScriptFunctionObject>();
    if (!function || !function->name().is_empty())
        return static_cast<i64>(pc + sizeof(Op::SetFunctionName));

    auto property_key = ASM_TRY(*vm, pc, vm->get(insn.name()).to_property_key(*vm));
    function->set_inferred_name(Variant<PropertyKey, PrivateName> { move(property_key) }, asm_function_name_prefix_to_string(insn.prefix()));
    return static_cast<i64>(pc + sizeof(Op::SetFunctionName));
}

i64 asm_slow_path_new_array_with_length(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::NewArrayWithLength const*>(&bytecode[pc]);
    auto length = static_cast<u64>(vm->get(insn.array_length()).as_double());
    auto array = ASM_TRY(*vm, pc, JS::Array::create(vm->realm(), length));
    vm->set(insn.dst(), array);
    return static_cast<i64>(pc + sizeof(Op::NewArrayWithLength));
}

i64 asm_slow_path_array_append(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ArrayAppend const*>(&bytecode[pc]);
    auto rhs = vm->get(insn.src());
    auto& lhs_array = vm->get(insn.dst()).as_array_exotic_object();
    auto lhs_size = lhs_array.indexed_array_like_size();

    if (insn.is_spread()) {
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

i64 asm_slow_path_create_variable(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CreateVariable const*>(&bytecode[pc]);
    auto const& name = vm->get_identifier(insn.identifier());
    ASM_TRY(*vm, pc, asm_create_variable(*vm, name, insn.mode(), insn.is_global(), insn.is_immutable(), insn.is_strict()));
    return static_cast<i64>(pc + sizeof(Op::CreateVariable));
}

i64 asm_slow_path_enter_object_environment(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::EnterObjectEnvironment const*>(&bytecode[pc]);
    auto object = ASM_TRY(*vm, pc, vm->get(insn.object()).to_object(*vm));
    auto& old_environment = vm->running_execution_context().lexical_environment;
    auto new_environment = new_object_environment(*object, true, old_environment);
    vm->set(insn.dst(), new_environment);
    vm->running_execution_context().lexical_environment = new_environment;
    return static_cast<i64>(pc + sizeof(Op::EnterObjectEnvironment));
}

i64 asm_slow_path_bitwise_not(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::BitwiseNot>(*vm, pc);
}

i64 asm_slow_path_unary_plus(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::UnaryPlus>(*vm, pc);
}

i64 asm_slow_path_is_callable(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::IsCallable const*>(&bytecode[pc]);
    vm->set(insn.dst(), Value(vm->get(insn.value()).is_function()));
    return static_cast<i64>(pc + sizeof(Op::IsCallable));
}

i64 asm_slow_path_is_constructor(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::IsConstructor const*>(&bytecode[pc]);
    vm->set(insn.dst(), Value(vm->get(insn.value()).is_constructor()));
    return static_cast<i64>(pc + sizeof(Op::IsConstructor));
}

i64 asm_slow_path_add_private_name(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::AddPrivateName const*>(&bytecode[pc]);
    auto const& name = vm->get_identifier(insn.name());
    vm->running_execution_context().private_environment->add_private_name(name);
    return static_cast<i64>(pc + sizeof(Op::AddPrivateName));
}

i64 asm_slow_path_create_async_from_sync_iterator(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CreateAsyncFromSyncIterator const*>(&bytecode[pc]);
    auto& realm = vm->realm();

    auto& iterator = vm->get(insn.iterator()).as_object();
    auto next_method = vm->get(insn.next_method());
    auto done = vm->get(insn.done()).as_bool();

    auto iterator_record = realm.create<IteratorRecord>(iterator, next_method, done);
    auto async_from_sync_iterator = create_async_from_sync_iterator(*vm, iterator_record);

    auto iterator_object = Object::create(realm, nullptr);
    iterator_object->define_direct_property(vm->names.iterator, async_from_sync_iterator.iterator, default_attributes);
    iterator_object->define_direct_property(vm->names.nextMethod, async_from_sync_iterator.next_method, default_attributes);
    iterator_object->define_direct_property(vm->names.done, Value { async_from_sync_iterator.done }, default_attributes);

    vm->set(insn.dst(), iterator_object);
    return static_cast<i64>(pc + sizeof(Op::CreateAsyncFromSyncIterator));
}

i64 asm_slow_path_create_data_property_or_throw(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CreateDataPropertyOrThrow const*>(&bytecode[pc]);
    auto& object = vm->get(insn.object()).as_object();
    auto property = ASM_TRY(*vm, pc, vm->get(insn.property()).to_property_key(*vm));
    auto value = vm->get(insn.value());
    ASM_TRY(*vm, pc, object.create_data_property_or_throw(property, value));
    return static_cast<i64>(pc + sizeof(Op::CreateDataPropertyOrThrow));
}

i64 asm_slow_path_create_immutable_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CreateImmutableBinding const*>(&bytecode[pc]);
    auto& environment = as<Environment>(vm->get(insn.environment()).as_cell());
    ASM_TRY(*vm, pc, environment.create_immutable_binding(*vm, vm->get_identifier(insn.identifier()), insn.strict_binding()));
    return static_cast<i64>(pc + sizeof(Op::CreateImmutableBinding));
}

i64 asm_slow_path_create_mutable_binding(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CreateMutableBinding const*>(&bytecode[pc]);
    auto& environment = as<Environment>(vm->get(insn.environment()).as_cell());
    ASM_TRY(*vm, pc, environment.create_mutable_binding(*vm, vm->get_identifier(insn.identifier()), insn.can_be_deleted()));
    return static_cast<i64>(pc + sizeof(Op::CreateMutableBinding));
}

i64 asm_slow_path_create_rest_params(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CreateRestParams const*>(&bytecode[pc]);
    auto const arguments = vm->running_execution_context().arguments_span();
    auto arguments_count = vm->running_execution_context().passed_argument_count;
    auto array = MUST(JS::Array::create(vm->realm(), 0));
    for (size_t rest_index = insn.rest_index(); rest_index < arguments_count; ++rest_index)
        array->indexed_append(arguments[rest_index]);
    vm->set(insn.dst(), array);
    return static_cast<i64>(pc + sizeof(Op::CreateRestParams));
}

i64 asm_slow_path_create_arguments(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CreateArguments const*>(&bytecode[pc]);
    auto const& function = vm->running_execution_context().function;
    auto const arguments = vm->running_execution_context().arguments_span();
    auto const& environment = vm->running_execution_context().lexical_environment;

    auto passed_arguments = ReadonlySpan<Value> { arguments.data(), vm->running_execution_context().passed_argument_count };
    Object* arguments_object;
    if (insn.kind() == Op::ArgumentsKind::Mapped) {
        auto const& ecma_function = static_cast<ECMAScriptFunctionObject const&>(*function);
        arguments_object = create_mapped_arguments_object(*vm, *function, ecma_function.parameter_names_for_mapped_arguments(), passed_arguments, *environment);
    } else {
        arguments_object = create_unmapped_arguments_object(*vm, passed_arguments);
    }

    if (insn.dst().has_value()) {
        vm->set(*insn.dst(), arguments_object);
        return static_cast<i64>(pc + sizeof(Op::CreateArguments));
    }

    if (insn.is_immutable()) {
        MUST(environment->create_immutable_binding(*vm, vm->names.arguments.as_string(), false));
    } else {
        MUST(environment->create_mutable_binding(*vm, vm->names.arguments.as_string(), false));
    }
    MUST(environment->initialize_binding(*vm, vm->names.arguments.as_string(), arguments_object, Environment::InitializeBindingHint::Normal));
    return static_cast<i64>(pc + sizeof(Op::CreateArguments));
}

i64 asm_slow_path_await(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::Await const*>(&bytecode[pc]);
    auto yielded_value = vm->get(insn.argument()).is_special_empty_value() ? js_undefined() : vm->get(insn.argument());
    auto& context = vm->running_execution_context();
    context.yield_continuation = insn.continuation_label().address();
    context.yield_is_await = true;
    context.yield_value_is_iterator_result = false;
    vm->do_return(yielded_value);
    return -1;
}

i64 asm_slow_path_create_lexical_environment(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CreateLexicalEnvironment const*>(&bytecode[pc]);
    auto& parent = as<Environment>(vm->get(insn.parent()).as_cell());
    auto environment = new_declarative_environment(parent);
    environment->ensure_capacity(insn.capacity());
    environment->set_is_catch_environment(insn.is_catch_environment());
    vm->set(insn.dst(), environment);
    vm->running_execution_context().lexical_environment = environment;
    return static_cast<i64>(pc + sizeof(Op::CreateLexicalEnvironment));
}

i64 asm_slow_path_create_private_environment(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto& running_execution_context = vm->running_execution_context();
    auto outer_private_environment = running_execution_context.private_environment;
    running_execution_context.private_environment = new_private_environment(*vm, outer_private_environment);
    return static_cast<i64>(pc + sizeof(Op::CreatePrivateEnvironment));
}

i64 asm_slow_path_create_variable_environment(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::CreateVariableEnvironment const*>(&bytecode[pc]);
    auto& running_execution_context = vm->running_execution_context();
    auto var_environment = new_declarative_environment(*running_execution_context.lexical_environment);
    if (auto* shared_data = vm->active_shared_function_data(); shared_data && insn.capacity() == shared_data->m_var_environment_bindings_count)
        var_environment->set_environment_shape_cache(shared_data->m_var_environment_shape, insn.capacity());
    var_environment->ensure_capacity(insn.capacity());
    running_execution_context.variable_environment = var_environment;
    running_execution_context.lexical_environment = var_environment;
    return static_cast<i64>(pc + sizeof(Op::CreateVariableEnvironment));
}

i64 asm_slow_path_delete_by_id(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DeleteById const*>(&bytecode[pc]);
    auto const& property_key = vm->get_property_key(insn.property());
    auto reference = Reference { vm->get(insn.base()), property_key, {}, insn.strict() };
    auto result = ASM_TRY(*vm, pc, reference.delete_(*vm));
    vm->set(insn.dst(), Value(result));
    return static_cast<i64>(pc + sizeof(Op::DeleteById));
}

i64 asm_slow_path_delete_by_value(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DeleteByValue const*>(&bytecode[pc]);
    auto property_key = ASM_TRY(*vm, pc, vm->get(insn.property()).to_property_key(*vm));
    auto reference = Reference { vm->get(insn.base()), property_key, {}, insn.strict() };
    auto result = ASM_TRY(*vm, pc, reference.delete_(*vm));
    vm->set(insn.dst(), Value(result));
    return static_cast<i64>(pc + sizeof(Op::DeleteByValue));
}

i64 asm_slow_path_delete_variable(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::DeleteVariable const*>(&bytecode[pc]);
    auto const& string = vm->get_identifier(insn.identifier());
    auto reference = ASM_TRY(*vm, pc, vm->resolve_binding(string, insn.strict()));
    auto result = ASM_TRY(*vm, pc, reference.delete_(*vm));
    vm->set(insn.dst(), Value(result));
    return static_cast<i64>(pc + sizeof(Op::DeleteVariable));
}

i64 asm_slow_path_get_completion_fields(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetCompletionFields const*>(&bytecode[pc]);
    auto& completion_source = vm->get(insn.completion()).as_object();
    if (is<GeneratorObject>(completion_source)) {
        auto const& generator = as<GeneratorObject>(completion_source);
        vm->set(insn.value_dst(), generator.pending_completion_value());
        vm->set(insn.type_dst(), Value(to_underlying(generator.pending_completion_type())));
        return static_cast<i64>(pc + sizeof(Op::GetCompletionFields));
    }

    auto const& async_generator = as<AsyncGenerator>(completion_source);
    vm->set(insn.value_dst(), async_generator.pending_completion_value());
    vm->set(insn.type_dst(), Value(to_underlying(async_generator.pending_completion_type())));
    return static_cast<i64>(pc + sizeof(Op::GetCompletionFields));
}

i64 asm_slow_path_set_completion_type(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SetCompletionType const*>(&bytecode[pc]);
    auto& completion_source = vm->get(insn.completion()).as_object();
    if (is<GeneratorObject>(completion_source)) {
        as<GeneratorObject>(completion_source).set_pending_completion_type(insn.completion_type());
        return static_cast<i64>(pc + sizeof(Op::SetCompletionType));
    }

    as<AsyncGenerator>(completion_source).set_pending_completion_type(insn.completion_type());
    return static_cast<i64>(pc + sizeof(Op::SetCompletionType));
}

i64 asm_slow_path_get_template_object(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetTemplateObject const*>(&bytecode[pc]);
    auto& cache = *vm->current_executable().template_object_caches[insn.cache()];

    if (cache.cached_template_object) {
        vm->set(insn.dst(), cache.cached_template_object);
        return static_cast<i64>(pc + insn.length());
    }

    auto& realm = *vm->current_realm();
    auto strings = insn.strings();
    u32 count = insn.strings_count() / 2;
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
    vm->set(insn.dst(), template_object);
    return static_cast<i64>(pc + insn.length());
}

i64 asm_slow_path_new_function(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::NewFunction const*>(&bytecode[pc]);
    auto& shared_data = *vm->current_executable().shared_function_data[insn.shared_function_data_index()];
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

    if (insn.home_object().has_value()) {
        auto home_object_value = vm->get(insn.home_object().value());
        function->make_method(home_object_value.as_object());
    }

    vm->set(insn.dst(), function);
    return static_cast<i64>(pc + sizeof(Op::NewFunction));
}

i64 asm_slow_path_leave_private_environment(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto& running_execution_context = vm->running_execution_context();
    running_execution_context.private_environment = running_execution_context.private_environment->outer_environment();
    return static_cast<i64>(pc + sizeof(Op::LeavePrivateEnvironment));
}

i64 asm_slow_path_throw(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::Throw const*>(&bytecode[pc]);
    return handle_asm_exception(*vm, pc, vm->get(insn.src()));
}

i64 asm_slow_path_throw_if_tdz(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ThrowIfTDZ const*>(&bytecode[pc]);
    auto value = vm->get(insn.src());
    if (value.is_special_empty_value()) [[unlikely]] {
        auto completion = vm->throw_completion<ReferenceError>(ErrorType::BindingNotInitialized, value);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    return static_cast<i64>(pc + sizeof(Op::ThrowIfTDZ));
}

i64 asm_slow_path_throw_if_not_object(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ThrowIfNotObject const*>(&bytecode[pc]);
    auto src = vm->get(insn.src());
    if (!src.is_object()) [[unlikely]] {
        auto completion = vm->throw_completion<TypeError>(ErrorType::NotAnObject, src);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    return static_cast<i64>(pc + sizeof(Op::ThrowIfNotObject));
}

i64 asm_slow_path_throw_if_nullish(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::ThrowIfNullish const*>(&bytecode[pc]);
    auto value = vm->get(insn.src());
    if (value.is_nullish()) [[unlikely]] {
        auto completion = vm->throw_completion<TypeError>(ErrorType::NotObjectCoercible, value);
        return handle_asm_exception(*vm, pc, completion.value());
    }
    return static_cast<i64>(pc + sizeof(Op::ThrowIfNullish));
}

i64 asm_slow_path_throw_const_assignment(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto completion = vm->throw_completion<TypeError>(ErrorType::InvalidAssignToConst);
    return handle_asm_exception(*vm, pc, completion.value());
}

i64 asm_slow_path_yield(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::Yield const*>(&bytecode[pc]);
    auto yielded_value = vm->get(insn.value()).is_special_empty_value() ? js_undefined() : vm->get(insn.value());
    auto& context = vm->running_execution_context();
    if (insn.continuation_label().has_value())
        context.yield_continuation = insn.continuation_label()->address();
    else
        context.yield_continuation = ExecutionContext::no_yield_continuation;
    context.yield_is_await = false;
    context.yield_value_is_iterator_result = false;
    vm->do_return(yielded_value);
    return -1;
}

i64 asm_slow_path_yield_iterator_result(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::YieldIteratorResult const*>(&bytecode[pc]);
    auto yielded_value = vm->get(insn.value()).is_special_empty_value() ? js_undefined() : vm->get(insn.value());
    auto& context = vm->running_execution_context();
    context.yield_continuation = insn.continuation_label().address();
    context.yield_is_await = false;
    context.yield_value_is_iterator_result = true;
    vm->do_return(yielded_value);
    return -1;
}

// Fast path for GetByValue on typed arrays.
// Returns 0 on success (result stored in dst), 1 on miss (fall to slow path).
i64 asm_try_get_by_value_typed_array(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetByValue const*>(&bytecode[pc]);

    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = vm->get(insn.property());
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
        vm->set(insn.dst(), js_undefined());
        return 0;
    }

    if (!is_valid_integer_index(typed_array, CanonicalIndex { CanonicalIndex::Type::Index, index })) [[unlikely]] {
        vm->set(insn.dst(), js_undefined());
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

    vm->set(insn.dst(), result);
    return 0;
}

// Fast path for PutByValue on typed arrays.
// Returns 0 on success, 1 on miss (fall to slow path).
i64 asm_try_put_by_value_typed_array(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutByValue const*>(&bytecode[pc]);

    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = vm->get(insn.property());
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

    if (index >= array_length.length()) [[unlikely]]
        return 0;

    if (!is_valid_integer_index(typed_array, CanonicalIndex { CanonicalIndex::Type::Index, index })) [[unlikely]]
        return 0;

    auto* buffer = typed_array.viewed_array_buffer();
    auto* data = buffer->data() + typed_array.byte_offset();
    auto value = vm->get(insn.src());

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

i64 asm_slow_path_instance_of(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::InstanceOf const*>(&bytecode[pc]);
    auto result = ASM_TRY(*vm, pc, instance_of(*vm, vm->get(insn.lhs()), vm->get(insn.rhs())));
    vm->set(insn.dst(), result);
    return static_cast<i64>(pc + sizeof(Op::InstanceOf));
}

i64 asm_slow_path_in(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::In const*>(&bytecode[pc]);
    auto result = ASM_TRY(*vm, pc, in(*vm, vm->get(insn.lhs()), vm->get(insn.rhs())));
    vm->set(insn.dst(), result);
    return static_cast<i64>(pc + sizeof(Op::In));
}

i64 asm_slow_path_resolve_this_binding(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::ResolveThisBinding>(*vm, pc);
}

// Direct handler for GetPrivateById: bypasses Reference indirection.
i64 asm_slow_path_get_private_by_id(VM* vm, u32 pc)
{
    vm->running_execution_context().program_counter = pc;
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetPrivateById const*>(&bytecode[pc]);

    auto base_value = vm->get(insn.base());
    auto& current_vm = *vm;

    if (!base_value.is_object()) [[unlikely]] {
        ASM_TRY(*vm, pc, base_value.to_object(current_vm));
        auto const& name = current_vm.get_identifier(insn.property());
        auto private_name = make_private_reference(current_vm, base_value, name);
        auto result = ASM_TRY(*vm, pc, private_name.get_value(current_vm));
        vm->set(insn.dst(), result);
        return static_cast<i64>(pc + sizeof(Op::GetPrivateById));
    }

    auto const& name = current_vm.get_identifier(insn.property());
    auto private_environment = current_vm.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(name);
    auto result = ASM_TRY(*vm, pc, base_value.as_object().private_get(private_name));
    vm->set(insn.dst(), result);
    return static_cast<i64>(pc + sizeof(Op::GetPrivateById));
}

// Direct handler for PutPrivateById: bypasses Reference indirection.
i64 asm_slow_path_put_private_by_id(VM* vm, u32 pc)
{
    vm->running_execution_context().program_counter = pc;
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutPrivateById const*>(&bytecode[pc]);

    auto base_value = vm->get(insn.base());
    auto& current_vm = *vm;
    auto value = vm->get(insn.src());

    if (!base_value.is_object()) [[unlikely]] {
        auto object = ASM_TRY(*vm, pc, base_value.to_object(current_vm));
        auto const& name = current_vm.get_identifier(insn.property());
        auto private_reference = make_private_reference(current_vm, object, name);
        ASM_TRY(*vm, pc, private_reference.put_value(current_vm, value));
        return static_cast<i64>(pc + sizeof(Op::PutPrivateById));
    }

    auto const& name = current_vm.get_identifier(insn.property());
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
