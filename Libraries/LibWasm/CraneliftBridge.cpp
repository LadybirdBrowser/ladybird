/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <AK/Platform.h>
#include <AK/ScopeGuard.h>
#include <CraneliftFFI.h>
#include <LibCore/Process.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Types.h>
#include <errno.h>
#include <stdlib.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#else
#    include <fcntl.h>
#    include <sys/mman.h>
#    include <unistd.h>
#endif

#if defined(AK_OS_MACOS)
#    include <libkern/OSCacheControl.h>
#    include <pthread.h>
#endif

using namespace Wasm;
using namespace Cranelift;

namespace {

struct InputHeader {
    u32 function_count;
    u32 helpers_offset;
    u64 outcome_return;
    u64 code_region_start;
    u64 total_size;
};

struct InputFunctionEntry {
    u32 insn_offset;
    u32 insn_count;
    u32 result_arity;
};

struct OutputFunctionEntry {
    u64 code_offset;
    u32 code_size;
    u32 compiled;
};

struct CodeMapping {
    void* mapping;
    size_t size;
};

static constexpr size_t oop_code_region_min_size = 256 * KiB;
static constexpr size_t oop_code_bytes_per_insn = 256;

static size_t align_up(size_t value, size_t alignment)
{
    VERIFY(alignment > 0);
    auto remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

struct BatchInput {
    Vector<CraneliftInsn> insns;
    u32 result_arity;
    CompiledInstructions* target;
};

}

// C helpers called by cranelift-generated code (need C linkage but not external visibility).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
extern "C" {

static ALWAYS_INLINE i32 wasm_cl_finish_call(BytecodeInterpreter& interpreter, Configuration& config, FunctionAddress address, Vector<Value, ArgumentsStaticSize>& args)
{
    if (interpreter.trap_if_insufficient_native_stack_space())
        return 1;

    config.build_compiled_function_table();

    auto* instance = config.store().unsafe_get(address);

    if (auto* wasm_function = instance->get_pointer<WasmFunction>(); wasm_function
        && !config.should_limit_instruction_count()
        && wasm_function->code().func().body().compiled_instructions.cranelift_compiled) {

        // Fast compiled-to-compiled call: stack-allocate locals + non-owning frame.
        auto& func = wasm_function->code().func();
        auto arg_count = args.size();
        auto local_count = func.total_local_count();
        auto total = arg_count + local_count;
        auto callee_arity = wasm_function->type().results().size();

        Value callee_buf[64];
        Value* callee_locals = callee_buf;
        Vector<Value, ArgumentsStaticSize> heap_fallback;
        if (total > 64) [[unlikely]] {
            heap_fallback.ensure_capacity(total);
            heap_fallback.resize_and_keep_capacity(total);
            callee_locals = heap_fallback.data();
        }
        for (size_t i = 0; i < arg_count; i++)
            callee_locals[i] = args[i];
        __builtin_memset(callee_locals + arg_count, 0, local_count * sizeof(Value));

        auto& ci = func.body().compiled_instructions;
        {
            BytecodeInterpreter::CallFrameHandle handle { interpreter, config };
            config.set_frame_lightweight(wasm_function->module(), callee_locals, func.body(), callee_arity);
            config.setup_call_record_for_current_frame();
            config.ip() = 0;

            interpreter.clear_trap();
            auto const* cc = ci.dispatches.data();
            auto const* addrs = ci.src_dst_mappings.data();
            using HandlerFn = Outcome (*)(BytecodeInterpreter&, Configuration&, Instruction const*, u32, Dispatch const*, SourcesAndDestination const*);
            auto const handler = bit_cast<HandlerFn>(cc[0].handler_ptr);
            auto outcome = handler(interpreter, config, cc[0].instruction, 0, cc, addrs);

            if (outcome != Outcome::Return) {
                interpreter.set_trap("Compiled function returned unexpectedly"sv);
                return 1;
            }
            if (interpreter.did_trap())
                return 1;

            if (callee_arity == 1)
                config.compiled_call_result_scratch() = config.value_stack().unsafe_take_last();
            // No label pop: set_frame_lightweight doesn't push labels.
        }
        return 0;
    }

    // direct-threaded interpreter path:
    // CallFrameHandle saves/zeros/restores the direct call counter automatically.
    if (auto* wasm_function = instance->get_pointer<WasmFunction>(); wasm_function && !config.should_limit_instruction_count() && wasm_function->code().func().body().compiled_instructions.direct) {
        BytecodeInterpreter::CallFrameHandle handle { interpreter, config };
        if (auto prepare_result = config.prepare_wasm_call(*wasm_function, args); prepare_result.is_error()) {
            interpreter.set_trap(prepare_result.release_error());
            return 1;
        }
        config.ip() = 0;
        auto outcome = interpreter.run_compiled_function_direct(config);
        if (outcome != Outcome::Return) {
            interpreter.set_trap("Compiled function returned unexpectedly"sv);
            return 1;
        }
        if (interpreter.did_trap())
            return 1;
        if (config.frame().arity() == 1)
            config.compiled_call_result_scratch() = config.value_stack().unsafe_take_last();
        if (!config.label_stack().is_empty())
            config.label_stack().take_last();
        return 0;
    }

    // non-compiled call (interpreter or host function)
    Wasm::Result result { Vector<Value> {} };
    if (instance->has<WasmFunction>()) {
        BytecodeInterpreter::CallFrameHandle handle { interpreter, config };
        result = config.call(interpreter, address, args);
    } else {
        result = config.call(interpreter, address, args);
        config.release_arguments_allocation(args);
    }

    if (result.is_trap()) {
        interpreter.set_trap(move(result.trap()));
        return 1;
    }

    if (!result.values().is_empty())
        config.compiled_call_result_scratch() = result.values().first();
    return 0;
}

i32 wasm_cl_call_function(void* interp_ptr, void* config_ptr, i32 func_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);

    auto const& module = config.frame().module();
    auto const& functions = module.functions();
    if (static_cast<size_t>(func_index) >= functions.size())
        return 1;

    auto address = functions[func_index];

    SourcesAndDestination addrs {};
    addrs.sources[0] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[1] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[2] = Dispatch::RegisterOrStack::Stack;
    addrs.destination = Dispatch::RegisterOrStack::Stack;

    auto outcome = interpreter.call_address(config, address, addrs,
        BytecodeInterpreter::CallAddressSource::DirectCall,
        BytecodeInterpreter::CallType::UsingStack);

    return outcome == Outcome::Return && interpreter.did_trap() ? 1 : 0;
}

void wasm_cl_set_trap(void* interp_ptr, u8 const* msg, i32 len)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    interpreter.set_trap(StringView(reinterpret_cast<char const*>(msg), len));
}

static inline MemoryInstance* wasm_cl_get_memory(void* config_ptr, i32 mem_idx)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    if (mem_idx == 0) [[likely]] {
        if (auto* memory = config.default_memory())
            return memory;
    }
    auto const& module = config.frame().module();
    auto const& mem_address = module.memories().data()[mem_idx];
    return config.store().unsafe_get(mem_address);
}

static inline u8 const* wasm_cl_memory_data_if_in_bounds(MemoryInstance* memory, u64 instance_addr, size_t size)
{
    if (instance_addr + size > memory->size())
        return nullptr;
    return memory->data().offset_pointer(instance_addr);
}

i64 wasm_cl_memory_load8_s(void* config_ptr, i32 mem_idx, i64 addr)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto const* data = wasm_cl_memory_data_if_in_bounds(memory, static_cast<u64>(addr), 1);
    if (!data)
        return 0;
    return static_cast<i64>(static_cast<i8>(data[0]));
}

i64 wasm_cl_memory_load8_u(void* config_ptr, i32 mem_idx, i64 addr)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto const* data = wasm_cl_memory_data_if_in_bounds(memory, static_cast<u64>(addr), 1);
    if (!data)
        return 0;
    return static_cast<i64>(data[0]);
}

i64 wasm_cl_memory_load16_s(void* config_ptr, i32 mem_idx, i64 addr)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto const* data = wasm_cl_memory_data_if_in_bounds(memory, static_cast<u64>(addr), 2);
    if (!data)
        return 0;
    u16 val;
    __builtin_memcpy(&val, data, sizeof(val));
    return static_cast<i64>(static_cast<i16>(val));
}

i64 wasm_cl_memory_load16_u(void* config_ptr, i32 mem_idx, i64 addr)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto const* data = wasm_cl_memory_data_if_in_bounds(memory, static_cast<u64>(addr), 2);
    if (!data)
        return 0;
    u16 val;
    __builtin_memcpy(&val, data, sizeof(val));
    return static_cast<i64>(val);
}

i64 wasm_cl_memory_load32_s(void* config_ptr, i32 mem_idx, i64 addr)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto const* data = wasm_cl_memory_data_if_in_bounds(memory, static_cast<u64>(addr), 4);
    if (!data)
        return 0;
    u32 val;
    __builtin_memcpy(&val, data, sizeof(val));
    return static_cast<i64>(static_cast<i32>(val));
}

i64 wasm_cl_memory_load32_u(void* config_ptr, i32 mem_idx, i64 addr)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto const* data = wasm_cl_memory_data_if_in_bounds(memory, static_cast<u64>(addr), 4);
    if (!data)
        return 0;
    u32 val;
    __builtin_memcpy(&val, data, sizeof(val));
    return static_cast<i64>(val);
}

i64 wasm_cl_memory_load64(void* config_ptr, i32 mem_idx, i64 addr)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto const* data = wasm_cl_memory_data_if_in_bounds(memory, static_cast<u64>(addr), 8);
    if (!data)
        return 0;
    u64 val;
    __builtin_memcpy(&val, data, sizeof(val));
    return static_cast<i64>(val);
}

static inline bool wasm_cl_memory_store_in_bounds(void* config_ptr, i32 mem_idx, i64 addr, size_t size, u8*& data)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto instance_addr = static_cast<u64>(addr);
    if (instance_addr + size > memory->size())
        return false;
    data = memory->data().offset_pointer(instance_addr);
    return true;
}

i32 wasm_cl_memory_store8(void* config_ptr, i32 mem_idx, i64 addr, i64 value)
{
    u8* data;
    if (!wasm_cl_memory_store_in_bounds(config_ptr, mem_idx, addr, 1, data))
        return 1; // OOB trap
    data[0] = static_cast<u8>(value);
    return 0;
}

i32 wasm_cl_memory_store16(void* config_ptr, i32 mem_idx, i64 addr, i64 value)
{
    u8* data;
    if (!wasm_cl_memory_store_in_bounds(config_ptr, mem_idx, addr, 2, data))
        return 1;
    u16 val = static_cast<u16>(value);
    __builtin_memcpy(data, &val, sizeof(val));
    return 0;
}

i32 wasm_cl_memory_store32(void* config_ptr, i32 mem_idx, i64 addr, i64 value)
{
    u8* data;
    if (!wasm_cl_memory_store_in_bounds(config_ptr, mem_idx, addr, 4, data))
        return 1;
    u32 val = static_cast<u32>(value);
    __builtin_memcpy(data, &val, sizeof(val));
    return 0;
}

i32 wasm_cl_memory_store64(void* config_ptr, i32 mem_idx, i64 addr, i64 value)
{
    u8* data;
    if (!wasm_cl_memory_store_in_bounds(config_ptr, mem_idx, addr, 8, data))
        return 1;
    u64 val = static_cast<u64>(value);
    __builtin_memcpy(data, &val, sizeof(val));
    return 0;
}

i64 wasm_cl_memory_size(void* config_ptr, i32 mem_idx)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    auto const& module = config.frame().module();
    auto const& mem_address = module.memories().data()[mem_idx];
    auto* memory = config.store().unsafe_get(mem_address);
    return static_cast<i64>(memory->size() / Constants::page_size);
}

i32 wasm_cl_memory_grow(void* config_ptr, i32 mem_idx, i32 pages)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    auto const& module = config.frame().module();
    auto const& mem_address = module.memories().data()[mem_idx];
    auto* memory = config.store().unsafe_get(mem_address);
    auto old_pages = memory->size() / Constants::page_size;
    if (!memory->grow(pages * Constants::page_size))
        return -1;
    if (mem_idx == 0)
        config.refresh_default_memory_base();
    return static_cast<i32>(old_pages);
}

i64 wasm_cl_read_global(void* config_ptr, i32 index)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    auto const& module = config.frame().module();
    auto global_address = module.globals().data()[index];
    auto* global = config.store().get(global_address);
    return global->value().to<i64>();
}

void wasm_cl_write_global(void* config_ptr, i32 index, i64 value)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    auto const& module = config.frame().module();
    auto global_address = module.globals().data()[index];
    auto* global = config.store().get(global_address);
    global->set_value(Value(value));
}

i32 wasm_cl_call_indirect(void* interp_ptr, void* config_ptr, i32 table_idx, i32 type_idx, i32 element_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);

    auto const& module = config.frame().module();
    auto table_address = module.tables()[table_idx];
    auto* table_instance = config.store().get(table_address);
    if (!table_instance || element_index < 0 || static_cast<size_t>(element_index) >= table_instance->elements().size())
        return interpreter.set_trap(Trap::from_string("Table index out of bounds"));

    auto& element = table_instance->elements()[element_index];
    if (!element.ref().has<Reference::Func>())
        return interpreter.set_trap(Trap::from_string("Table element is not a function reference"));

    auto address = element.ref().get<Reference::Func>().address;
    auto* function = config.store().get(address);
    if (!function)
        return interpreter.set_trap(Trap::from_string("Indirect call to freed function"));
    auto const& type_actual = function->visit([](auto& f) -> decltype(auto) { return f.type(); });
    auto const& type_expected = module.types()[type_idx].unsafe_function();
    if (type_actual.parameters() != type_expected.parameters() || type_actual.results() != type_expected.results())
        return interpreter.set_trap(Trap::from_string("Indirect call type mismatch"));

    SourcesAndDestination addrs {};
    addrs.sources[0] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[1] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[2] = Dispatch::RegisterOrStack::Stack;
    addrs.destination = Dispatch::RegisterOrStack::Stack;

    auto outcome = interpreter.call_address(config, address, addrs, BytecodeInterpreter::CallAddressSource::IndirectCall, BytecodeInterpreter::CallType::UsingStack);

    return outcome == Outcome::Return && interpreter.did_trap() ? 1 : 0;
}

i32 wasm_cl_memory_copy(void* interp_ptr, void* config_ptr, i32 dst_mem, i32 src_mem, i32 dst_offset, i32 src_offset, i32 count)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);

    auto const& module = config.frame().module();
    auto* src_instance = config.store().unsafe_get(module.memories().data()[src_mem]);
    auto* dst_instance = config.store().unsafe_get(module.memories().data()[dst_mem]);

    auto src_end = static_cast<u64>(static_cast<u32>(src_offset)) + static_cast<u32>(count);
    auto dst_end = static_cast<u64>(static_cast<u32>(dst_offset)) + static_cast<u32>(count);
    if (src_end > src_instance->size() || dst_end > dst_instance->size())
        return interpreter.set_trap(Trap::from_string("Memory access out of bounds"));

    if (count > 0)
        __builtin_memmove(dst_instance->data().data() + static_cast<u32>(dst_offset), src_instance->data().data() + static_cast<u32>(src_offset), static_cast<u32>(count));

    return 0;
}

i32 wasm_cl_memory_fill(void* interp_ptr, void* config_ptr, i32 mem_idx, i32 offset, i32 value, i32 count)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);

    auto const& module = config.frame().module();
    auto* instance = config.store().unsafe_get(module.memories().data()[mem_idx]);

    auto end = static_cast<u64>(static_cast<u32>(offset)) + static_cast<u32>(count);
    if (end > instance->size())
        return interpreter.set_trap(Trap::from_string("Memory access out of bounds"));

    if (count > 0)
        __builtin_memset(instance->data().data() + static_cast<u32>(offset), static_cast<u8>(value), static_cast<u32>(count));

    return 0;
}

void wasm_cl_stack_push(void* config_ptr, i64 value)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    config.value_stack().append(Value(value));
}

i64 wasm_cl_stack_pop(void* config_ptr)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    return config.value_stack().unsafe_take_last().to<i64>();
}

i64 wasm_cl_stack_size(void* config_ptr)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    return static_cast<i64>(config.value_stack().size());
}

void wasm_cl_stack_cleanup(void* config_ptr, i64 initial_size, i32 result_arity)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    auto& stack = config.value_stack();
    auto expected = static_cast<size_t>(initial_size) + static_cast<size_t>(result_arity);
    if (stack.size() == expected)
        return;
    if (stack.size() < expected) {
        // Under-push (e.g. trap path), fill with zeros so the caller has something to pop.
        while (stack.size() < expected)
            stack.append(Value(static_cast<i64>(0)));
        return;
    }

    // Take results, trim stack, and put them back.
    Value saved[8];
    auto n = min(static_cast<size_t>(result_arity), size_t(8));
    for (size_t i = 0; i < n; i++)
        saved[i] = stack.unsafe_take_last();

    while (stack.size() > static_cast<size_t>(initial_size))
        stack.unsafe_take_last();

    for (size_t i = n; i > 0; i--)
        stack.append(saved[i - 1]);
}

i64 wasm_cl_callrec_read(void* config_ptr, i32 index)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    return config.call_record_entry(index).to<i64>();
}

void wasm_cl_callrec_write(void* config_ptr, i32 index, i64 value)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    if (!config.call_record_base()) [[unlikely]] {
        // No call record yet, allocate now.
        auto size = config.frame().expression().compiled_instructions.max_call_rec_size;
        if (size == 0)
            size = static_cast<size_t>(index) + 1;
        config.setup_call_record(size);
    }
    config.call_record_entry(index) = Value(value);
}

i32 wasm_cl_call_with_record(void* interp_ptr, void* config_ptr, i32 func_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);

    auto const& module = config.frame().module();
    auto const& functions = module.functions();
    if (static_cast<size_t>(func_index) >= functions.size())
        return 1;

    auto address = functions[func_index];
    auto* instance = config.store().get(address);
    if (!instance) {
        interpreter.set_trap("Attempt to call nonexistent function by address"sv);
        return 1;
    }

    FunctionType const* type { nullptr };
    instance->visit([&](auto const& function) { type = &function.type(); });

    Vector<Value, ArgumentsStaticSize> args;
    config.take_call_record(args);
    args.shrink(type->parameters().size(), true);
    return wasm_cl_finish_call(interpreter, config, address, args);
}

// Direct compiled-to-compiled call. Falls back to wasm_cl_finish_call for non-compiled targets.
static ALWAYS_INLINE i32 wasm_cl_direct_call_impl(BytecodeInterpreter& interpreter, Configuration& config, i32 func_index, Value* args, size_t arg_count)
{
    config.build_compiled_function_table();
    auto it = config.m_compiled_fn_table.find(static_cast<u32>(func_index));
    if (it == config.m_compiled_fn_table.end()) [[unlikely]] {
        // Not Cranelift-compiled, fall back to full path.
        Vector<Value, ArgumentsStaticSize> args_vec;
        args_vec.ensure_capacity(arg_count);
        for (size_t i = 0; i < arg_count; i++)
            args_vec.unchecked_append(args[i]);
        return wasm_cl_finish_call(interpreter, config, config.frame().module().functions()[func_index], args_vec);
    }

    auto const& entry = it->value;

    if (config.depth() > 500) [[unlikely]] {
        interpreter.set_trap("call stack exhausted"sv);
        return 1;
    }

    // Stack-allocate callee locals: args + zero-initialized locals.
    auto total = arg_count + entry.total_local_count;
    Value callee_locals_buf[64];
    Value* callee_locals = callee_locals_buf;
    Vector<Value, ArgumentsStaticSize> heap_buf;
    if (total > 64) [[unlikely]] {
        heap_buf.ensure_capacity(total);
        heap_buf.resize_and_keep_capacity(total);
        callee_locals = heap_buf.data();
    }
    for (size_t i = 0; i < arg_count; i++)
        callee_locals[i] = args[i];
    __builtin_memset(callee_locals + arg_count, 0, entry.total_local_count * sizeof(Value));

    // Lightweight non-owning frame push + direct handler call.
    BytecodeInterpreter::CallFrameHandle handle { interpreter, config };
    config.set_frame_lightweight(*entry.module, callee_locals, *entry.expression, entry.arity);
    config.setup_call_record_for_current_frame();
    config.ip() = 0;

    interpreter.clear_trap();
    using HandlerFn = Outcome (*)(BytecodeInterpreter&, Configuration&, Instruction const*, u32, Dispatch const*, SourcesAndDestination const*);
    auto const handler = bit_cast<HandlerFn>(entry.handler_ptr);
    auto outcome = handler(interpreter, config, entry.first_insn, 0, bit_cast<Dispatch const*>(entry.dispatches_ptr), bit_cast<SourcesAndDestination const*>(entry.src_dst_ptr));

    if (outcome != Outcome::Return) {
        interpreter.set_trap("Compiled function returned unexpectedly"sv);
        return 1;
    }
    if (interpreter.did_trap())
        return 1;
    if (entry.arity == 1)
        config.compiled_call_result_scratch() = config.value_stack().unsafe_take_last();
    // No label pop: set_frame_lightweight doesn't push labels.
    return 0;
}

i32 wasm_cl_direct_call_0(void* interp_ptr, void* config_ptr, i32 func_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    return wasm_cl_direct_call_impl(interpreter, config, func_index, nullptr, 0);
}

i32 wasm_cl_direct_call_1(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    Value args[] = { Value(arg0) };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, args, 1);
}

i32 wasm_cl_direct_call_2(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    Value args[] = { Value(arg0), Value(arg1) };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, args, 2);
}

i32 wasm_cl_direct_call_3(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1, i64 arg2)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    Value args[] = { Value(arg0), Value(arg1), Value(arg2) };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, args, 3);
}

// Thin frame push for direct compiled-to-compiled calls. Returns 1 on trap, 0 on success.
i32 wasm_cl_push_frame(void* interp_ptr, void* config_ptr, Value* locals_ptr, u32 /* total_locals */,
    void const* module_ptr, void const* expression_ptr, u32 arity, u32 max_call_rec_size)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    auto const& module = *static_cast<ModuleInstance const*>(module_ptr);
    auto const& expression = *static_cast<Expression const*>(expression_ptr);

    if (interpreter.trap_if_insufficient_native_stack_space())
        return 1;

    config.set_frame_lightweight(module, locals_ptr, expression, arity);
    config.depth()++;

    // Set up call record for the callee if needed.
    if (max_call_rec_size > 0) {
        config.set_call_record_base(nullptr);
        config.setup_call_record(max_call_rec_size);
    }
    return 0;
}

// Thin frame pop for direct compiled-to-compiled calls.
void wasm_cl_pop_frame(void* config_ptr, u32 arity)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    if (arity == 1)
        config.compiled_call_result_scratch() = config.value_stack().unsafe_take_last();
    if (!config.label_stack().is_empty())
        config.label_stack().take_last();
    config.unwind_impl();
}
}
#pragma GCC diagnostic pop

namespace Wasm {

static RuntimeHelpers make_runtime_helpers()
{
    return RuntimeHelpers {
        .call_function = bit_cast<uintptr_t>(&wasm_cl_call_function),
        .set_trap = bit_cast<uintptr_t>(&wasm_cl_set_trap),
        .memory_load8_s = bit_cast<uintptr_t>(&wasm_cl_memory_load8_s),
        .memory_load8_u = bit_cast<uintptr_t>(&wasm_cl_memory_load8_u),
        .memory_load16_s = bit_cast<uintptr_t>(&wasm_cl_memory_load16_s),
        .memory_load16_u = bit_cast<uintptr_t>(&wasm_cl_memory_load16_u),
        .memory_load32_s = bit_cast<uintptr_t>(&wasm_cl_memory_load32_s),
        .memory_load32_u = bit_cast<uintptr_t>(&wasm_cl_memory_load32_u),
        .memory_load64 = bit_cast<uintptr_t>(&wasm_cl_memory_load64),
        .memory_store8 = bit_cast<uintptr_t>(&wasm_cl_memory_store8),
        .memory_store16 = bit_cast<uintptr_t>(&wasm_cl_memory_store16),
        .memory_store32 = bit_cast<uintptr_t>(&wasm_cl_memory_store32),
        .memory_store64 = bit_cast<uintptr_t>(&wasm_cl_memory_store64),
        .memory_size = bit_cast<uintptr_t>(&wasm_cl_memory_size),
        .memory_grow = bit_cast<uintptr_t>(&wasm_cl_memory_grow),
        .read_global = bit_cast<uintptr_t>(&wasm_cl_read_global),
        .write_global = bit_cast<uintptr_t>(&wasm_cl_write_global),
        .stack_push = bit_cast<uintptr_t>(&wasm_cl_stack_push),
        .stack_pop = bit_cast<uintptr_t>(&wasm_cl_stack_pop),
        .stack_size = bit_cast<uintptr_t>(&wasm_cl_stack_size),
        .stack_cleanup = bit_cast<uintptr_t>(&wasm_cl_stack_cleanup),
        .callrec_read = bit_cast<uintptr_t>(&wasm_cl_callrec_read),
        .callrec_write = bit_cast<uintptr_t>(&wasm_cl_callrec_write),
        .call_with_record = bit_cast<uintptr_t>(&wasm_cl_call_with_record),
        .direct_call_0 = bit_cast<uintptr_t>(&wasm_cl_direct_call_0),
        .direct_call_1 = bit_cast<uintptr_t>(&wasm_cl_direct_call_1),
        .direct_call_2 = bit_cast<uintptr_t>(&wasm_cl_direct_call_2),
        .direct_call_3 = bit_cast<uintptr_t>(&wasm_cl_direct_call_3),
        .call_indirect = bit_cast<uintptr_t>(&wasm_cl_call_indirect),
        .memory_copy = bit_cast<uintptr_t>(&wasm_cl_memory_copy),
        .memory_fill = bit_cast<uintptr_t>(&wasm_cl_memory_fill),
        .regs_offset = static_cast<u32>(offsetof(Configuration, regs)),
        .value_size = static_cast<u32>(sizeof(Value)),
        .locals_base_offset = static_cast<u32>(Configuration::locals_base_offset()),
        .default_memory_base_offset = static_cast<u32>(Configuration::default_memory_base_offset()),
        .compiled_call_result_scratch_offset = static_cast<u32>(Configuration::compiled_call_result_scratch_offset()),
    };
}

static CraneliftInsn serialize_insn(Dispatch const& dispatch, SourcesAndDestination const& addr)
{
    CraneliftInsn out {};
    auto const* insn = dispatch.instruction;
    out.opcode = insn->opcode().value();
    out.sources[0] = addr.sources[0];
    out.sources[1] = addr.sources[1];
    out.sources[2] = addr.sources[2];
    out.destination = addr.destination;
    out.imm1 = 0;
    out.imm2 = 0;
    out.imm3 = 0;

    auto const& args = insn->arguments();
    u64 opc = out.opcode;

    if (opc == Instructions::i32_const.value()) {
        out.imm1 = static_cast<i64>(args.get<i32>());
    } else if (opc == Instructions::i64_const.value()) {
        out.imm1 = args.get<i64>();
    } else if (opc == Instructions::f32_const.value()) {
        out.imm1 = static_cast<i64>(bit_cast<i32>(args.get<float>()));
    } else if (opc == Instructions::f64_const.value()) {
        out.imm1 = bit_cast<i64>(args.get<double>());
    } else if (opc == Instructions::local_get.value() || opc == Instructions::local_set.value() || opc == Instructions::local_tee.value()) {
        out.imm1 = static_cast<i64>(insn->local_index().value());
    } else if (opc == Instructions::global_get.value() || opc == Instructions::global_set.value()) {
        out.imm1 = static_cast<i64>(args.get<GlobalIndex>().value());
    } else if (opc == Instructions::br.value() || opc == Instructions::br_if.value()) {
        auto const& br_args = args.get<Instruction::BranchArgs>();
        out.imm1 = static_cast<i64>(br_args.label.value());
    } else if (opc == Instructions::block.value() || opc == Instructions::loop.value() || opc == Instructions::if_.value()) {
        auto const& struct_args = args.get<Instruction::StructuredInstructionArgs>();
        out.imm1 = static_cast<i64>(struct_args.end_ip.value());
        out.imm2 = struct_args.else_ip.has_value()
            ? static_cast<i64>(struct_args.else_ip->value())
            : -1;
        u32 arity = struct_args.meta.has_value() ? struct_args.meta->arity : 0;
        u32 param_count = struct_args.meta.has_value() ? struct_args.meta->parameter_count : 0;
        out.imm3 = arity | (param_count << 16);
    } else if (opc == Instructions::br_table.value()) {
        auto const& table_args = args.get<Instruction::TableBranchArgs>();
        if (table_args.default_.value() > NumericLimits<u16>::max()) {
            out.imm3 = 0xff;
            return out;
        }
        for (auto const& label : table_args.labels) {
            if (label.value() > NumericLimits<u16>::max()) {
                out.imm3 = 0xff;
                return out;
            }
        }

        // Pack: imm3 low byte = min(label_count, 8), upper bits = default label.
        // First 4 labels in imm1, next 4 in imm2 (16 bits each).
        // If more than 8 labels, continuation instructions carry the rest.
        auto const total_labels = table_args.labels.size();
        auto const inline_count = min(total_labels, static_cast<size_t>(8));
        out.imm3 = static_cast<u32>(inline_count) | (static_cast<u32>(table_args.default_.value()) << 8);
        for (size_t i = 0; i < inline_count; ++i) {
            auto const encoded = static_cast<u64>(table_args.labels[i].value()) << ((i % 4) * 16);
            if (i < 4)
                out.imm1 |= static_cast<i64>(encoded);
            else
                out.imm2 |= static_cast<i64>(encoded);
        }
    } else if (opc == Instructions::call.value()) {
        out.imm1 = static_cast<i64>(args.get<FunctionIndex>().value());
    } else if (opc == Instructions::call_indirect.value()) {
        auto const& indirect_args = args.get<Instruction::IndirectCallArgs>();
        out.imm1 = static_cast<i64>(indirect_args.type.value());
        out.imm2 = static_cast<i64>(indirect_args.table.value());
    } else if (opc == Instructions::memory_copy.value()) {
        auto const& copy_args = args.get<Instruction::MemoryCopyArgs>();
        out.imm1 = static_cast<i64>(copy_args.dst_index.value());
        out.imm2 = static_cast<i64>(copy_args.src_index.value());
    } else if (opc == Instructions::memory_fill.value()) {
        auto const& fill_args = args.get<Instruction::MemoryIndexArgument>();
        out.imm1 = static_cast<i64>(fill_args.memory_index.value());
    } else if (opc >= Instructions::i32_load.value() && opc <= Instructions::i64_store32.value()) {
        auto const& mem_arg = args.get<Instruction::MemoryArgument>();
        out.imm1 = static_cast<i64>(mem_arg.offset);
        out.imm3 = static_cast<u32>(mem_arg.memory_index.value()) | (mem_arg.memory_index.value() == 0 ? (1u << 31) : 0);
    } else if (opc == Instructions::memory_size.value()
        || opc == Instructions::memory_grow.value()) {
        auto const& mem_idx_arg = args.get<Instruction::MemoryIndexArgument>();
        out.imm1 = static_cast<i64>(mem_idx_arg.memory_index.value());
    }

    auto is_syn = [opc](OpCode op) { return opc == op.value(); };
    auto syn_between = [opc](OpCode lo, OpCode hi) { return opc >= lo.value() && opc <= hi.value(); };

    if (opc >= Instructions::SyntheticInstructionBase.value()) {
        if (syn_between(Instructions::synthetic_call_00, Instructions::synthetic_call_31)) {
            out.imm1 = static_cast<i64>(args.get<FunctionIndex>().value());
        } else if (is_syn(Instructions::synthetic_call_with_record_0) || is_syn(Instructions::synthetic_call_with_record_1)) {
            out.imm1 = static_cast<i64>(args.get<FunctionIndex>().value());
        } else if (is_syn(Instructions::synthetic_br_nostack) || is_syn(Instructions::synthetic_br_if_nostack)) {
            auto const& br_args = args.get<Instruction::BranchArgs>();
            out.imm1 = static_cast<i64>(br_args.label.value());
        } else if (is_syn(Instructions::synthetic_local_copy)) {
            out.imm1 = static_cast<i64>(insn->local_index().value());
            out.imm2 = static_cast<i64>(args.get<LocalIndex>().value());
        } else if (is_syn(Instructions::synthetic_i32_add2local)
            || syn_between(Instructions::synthetic_i32_sub2local, Instructions::synthetic_i32_shrs2local)
            || is_syn(Instructions::synthetic_i64_add2local)
            || syn_between(Instructions::synthetic_i64_sub2local, Instructions::synthetic_i64_shrs2local)) {
            out.imm1 = static_cast<i64>(insn->local_index().value());
            out.imm2 = static_cast<i64>(args.get<LocalIndex>().value());
        } else if (is_syn(Instructions::synthetic_i32_addconstlocal) || is_syn(Instructions::synthetic_i32_andconstlocal)) {
            out.imm1 = static_cast<i64>(args.get<i32>());
            out.imm2 = static_cast<i64>(insn->local_index().value());
        } else if (is_syn(Instructions::synthetic_i64_addconstlocal) || is_syn(Instructions::synthetic_i64_andconstlocal)) {
            out.imm1 = args.get<i64>();
            out.imm2 = static_cast<i64>(insn->local_index().value());
        } else if (is_syn(Instructions::synthetic_i32_storelocal) || is_syn(Instructions::synthetic_i64_storelocal)) {
            auto const& mem_arg = args.get<Instruction::MemoryArgument>();
            out.imm1 = static_cast<i64>(mem_arg.offset);
            out.imm2 = static_cast<i64>(insn->local_index().value());
            out.imm3 = static_cast<u32>(mem_arg.memory_index.value()) | (mem_arg.memory_index.value() == 0 ? (1u << 31) : 0);
        } else if (is_syn(Instructions::synthetic_local_seti32_const)) {
            out.imm1 = static_cast<i64>(args.get<i32>());
            out.imm2 = static_cast<i64>(insn->local_index().value());
        } else if (is_syn(Instructions::synthetic_local_seti64_const)) {
            out.imm1 = args.get<i64>();
            out.imm2 = static_cast<i64>(insn->local_index().value());
        } else if (syn_between(Instructions::synthetic_argument_get, Instructions::synthetic_argument_tee)) {
            out.imm1 = static_cast<i64>(insn->local_index().value());
        }
    }

    return out;
}

static void try_cranelift_compile_batch(Vector<BatchInput>& batch)
{
    if (batch.is_empty())
        return;

    static auto helpers = make_runtime_helpers();
    u64 outcome_return = to_underlying(Outcome::Return);

    size_t function_count = batch.size();
    auto const entries_offset = sizeof(InputHeader);
    auto const entries_size = sizeof(InputFunctionEntry) * function_count;

    size_t total_insn_count = 0;
    for (auto& entry : batch)
        total_insn_count += entry.insns.size();

    auto const insn_region_offset = align_up(entries_offset + entries_size, alignof(CraneliftInsn));
    auto const insn_bytes = total_insn_count * sizeof(CraneliftInsn);
    auto const helpers_offset = align_up(insn_region_offset + insn_bytes, alignof(RuntimeHelpers));
    auto const code_region_start = align_up(helpers_offset + sizeof(RuntimeHelpers), alignof(OutputFunctionEntry));
    auto const code_region_size = max(oop_code_region_min_size, total_insn_count * oop_code_bytes_per_insn);
    auto const total_size = code_region_start + sizeof(OutputFunctionEntry) * function_count + code_region_size;

#if defined(AK_OS_WINDOWS)
    DWORD size_hi = static_cast<DWORD>(static_cast<u64>(total_size) >> 32);
    DWORD size_lo = static_cast<DWORD>(total_size & 0xFFFFFFFF);
    HANDLE section_handle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, size_hi, size_lo, NULL);
    if (!section_handle)
        return;
    SetHandleInformation(section_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    ScopeGuard close_handle = [section_handle] { CloseHandle(section_handle); };

    auto* mapping = MapViewOfFile(section_handle, FILE_MAP_ALL_ACCESS, 0, 0, total_size);
    if (!mapping)
        return;
    ScopeGuard unmap = [mapping] { UnmapViewOfFile(mapping); };
#elif defined(AK_OS_MACOS)
    // macOS lacks memfd_create; use shm_open + shm_unlink for an anonymous fd.
    char shm_name[] = "/libwasm-cranelift-XXXXXX";
    arc4random_buf(shm_name + 21, 6);
    for (int i = 21; i < 27; ++i)
        shm_name[i] = 'A' + (static_cast<unsigned char>(shm_name[i]) % 26);
    int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return;
    shm_unlink(shm_name);
    ScopeGuard close_fd = [fd] { close(fd); };
    if (ftruncate(fd, static_cast<off_t>(total_size)) < 0)
        return;

    auto* mapping = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
        return;
    ScopeGuard unmap = [mapping, total_size] { munmap(mapping, total_size); };
#else
    int fd = memfd_create("libwasm-cranelift", 0);
    if (fd < 0)
        return;
    ScopeGuard close_fd = [fd] { close(fd); };
    if (ftruncate(fd, static_cast<off_t>(total_size)) < 0)
        return;

    auto* mapping = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
        return;
    ScopeGuard unmap = [mapping, total_size] { munmap(mapping, total_size); };
#endif

    auto* base = static_cast<u8*>(mapping);
    __builtin_memset(base, 0, total_size);

    auto* header = reinterpret_cast<InputHeader*>(base);
    *header = InputHeader {
        .function_count = static_cast<u32>(function_count),
        .helpers_offset = static_cast<u32>(helpers_offset),
        .outcome_return = outcome_return,
        .code_region_start = code_region_start,
        .total_size = total_size,
    };

    size_t insn_cursor = insn_region_offset;
    for (size_t i = 0; i < function_count; ++i) {
        auto& input = batch[i];
        auto* entry = reinterpret_cast<InputFunctionEntry*>(base + entries_offset + i * sizeof(InputFunctionEntry));
        *entry = InputFunctionEntry {
            .insn_offset = static_cast<u32>(insn_cursor),
            .insn_count = static_cast<u32>(input.insns.size()),
            .result_arity = input.result_arity,
        };
        __builtin_memcpy(base + insn_cursor, input.insns.data(), input.insns.size() * sizeof(CraneliftInsn));
        insn_cursor += input.insns.size() * sizeof(CraneliftInsn);
    }

    __builtin_memcpy(base + helpers_offset, &helpers, sizeof(helpers));

    Vector<ByteString> arguments;
#if defined(AK_OS_WINDOWS)
    arguments.append(ByteString::number(reinterpret_cast<uintptr_t>(section_handle)));
#else
    arguments.append(ByteString::number(fd));
#endif

    auto process_result = Core::Process::spawn({
        .name = "cranelift-compiler"sv,
        .executable = WASM_CRANELIFT_COMPILER_PATH,
        .arguments = arguments,
    });
    if (process_result.is_error())
        return;
    auto status_result = process_result.release_value().wait_for_termination();
    if (status_result.is_error() || status_result.value() != 0)
        return;

    // Extract results for each function.
    auto const code_base_offset = code_region_start + sizeof(OutputFunctionEntry) * function_count;

    for (size_t i = 0; i < function_count; ++i) {
        auto const* output = reinterpret_cast<OutputFunctionEntry const*>(base + code_region_start + i * sizeof(OutputFunctionEntry));
        if (!output->compiled)
            continue;

        auto code_offset = static_cast<size_t>(output->code_offset);
        auto code_size = static_cast<size_t>(output->code_size);
        auto code_start = code_base_offset + code_offset;
        if (code_start + code_size > total_size)
            continue;

#if defined(AK_OS_WINDOWS)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        auto const page_size = static_cast<size_t>(si.dwPageSize);
        auto const rx_aligned_size = (code_size + page_size - 1) & ~(page_size - 1);
        auto* jit_mem = VirtualAlloc(nullptr, rx_aligned_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!jit_mem)
            continue;
        __builtin_memcpy(jit_mem, base + code_start, code_size);
        DWORD old_protect;
        VirtualProtect(jit_mem, rx_aligned_size, PAGE_EXECUTE_READ, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), jit_mem, code_size);

        auto* func_ptr = static_cast<u8 const*>(jit_mem);
        auto* handle = new CodeMapping { jit_mem, rx_aligned_size };
#elif defined(AK_OS_MACOS)
        // We can't pull the map-as-rx/rw-across-processes trick on macos, so just do MAP_JIT with the typical jit mapping dance.
        auto const page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        auto const rx_aligned_size = (code_size + page_size - 1) & ~(page_size - 1);
        auto* jit_mapping = mmap(nullptr, rx_aligned_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (jit_mapping == MAP_FAILED)
            continue;

        pthread_jit_write_protect_np(0);
        __builtin_memcpy(jit_mapping, base + code_start, code_size);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(jit_mapping, code_size);

        auto* func_ptr = static_cast<u8 const*>(jit_mapping);
        auto* handle = new CodeMapping { jit_mapping, rx_aligned_size };
#else
        auto const page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        auto const page_aligned_offset = code_start & ~(page_size - 1);
        auto const offset_within_page = code_start - page_aligned_offset;
        auto const rx_map_size = offset_within_page + code_size;
        auto const rx_aligned_size = (rx_map_size + page_size - 1) & ~(page_size - 1);

        auto* rx_mapping = mmap(nullptr, rx_aligned_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, static_cast<off_t>(page_aligned_offset));
        if (rx_mapping == MAP_FAILED)
            continue;

        auto* func_ptr = static_cast<u8 const*>(rx_mapping) + offset_within_page;
        auto* handle = new CodeMapping { rx_mapping, rx_aligned_size };
#endif

        auto& compiled = *batch[i].target;
        compiled.dispatches[0].handler_ptr = bit_cast<FlatPtr>(func_ptr);
        compiled.cranelift_code_handle = handle;
        compiled.cranelift_code_size = code_size;
        compiled.cranelift_compiled = true;
    }
}

static Vector<BatchInput> s_pending_batch;

bool try_cranelift_compile(CompiledInstructions& compiled, u32 result_arity)
{
#if !WASM_COMPILED_FAULT_RECOVERY_SUPPORTED
    (void)compiled;
    (void)result_arity;
    return false;
#else
    auto const& dispatches = compiled.dispatches;
    auto const& addresses = compiled.src_dst_mappings;

    if (dispatches.is_empty())
        return false;

    if constexpr (WASM_CRANELIFT_DEBUG) {
        // CRANELIFT_MAX_INSNS=N       skip functions with more than N dispatches.
        // CRANELIFT_MIN_INSNS=N       skip functions with fewer than N dispatches.
        // CRANELIFT_MIN_FN=N          skip functions with id < N.
        // CRANELIFT_MAX_FN=N          skip functions with id > N.
        // CRANELIFT_SKIP_FN=a,b,c     skip listed function ids.
        // CRANELIFT_ONLY_FN=a,b,c     only compile listed function ids.
        // CRANELIFT_TRACE=1           log a line per compiled function.
        static auto const read_size_env = [](char const* name, size_t fallback) {
            if (auto* env = getenv(name))
                return static_cast<size_t>(atol(env));
            return fallback;
        };
        static auto const read_set_env = [](char const* name) {
            HashTable<size_t> out;
            auto* env = getenv(name);
            if (!env || !*env)
                return out;
            StringView view { env, strlen(env) };
            view.for_each_split_view(',', SplitBehavior::Nothing, [&](auto part) {
                if (auto n = part.template to_number<size_t>(); n.has_value())
                    out.set(n.value());
            });
            return out;
        };
        static size_t s_max_insns = read_size_env("CRANELIFT_MAX_INSNS", NumericLimits<size_t>::max());
        static size_t s_min_insns = read_size_env("CRANELIFT_MIN_INSNS", 0);
        static size_t s_min_fn = read_size_env("CRANELIFT_MIN_FN", 0);
        static size_t s_max_fn = read_size_env("CRANELIFT_MAX_FN", NumericLimits<size_t>::max());
        static auto s_skip_fn = read_set_env("CRANELIFT_SKIP_FN");
        static auto s_only_fn = read_set_env("CRANELIFT_ONLY_FN");
        static auto s_dump_fn = read_set_env("CRANELIFT_DUMP_FN");
        static bool s_trace = getenv("CRANELIFT_TRACE") != nullptr;

        static size_t s_func_counter = 0;
        size_t func_id = s_func_counter++;

        if (dispatches.size() > s_max_insns || dispatches.size() < s_min_insns)
            return false;
        if (func_id < s_min_fn || func_id > s_max_fn)
            return false;
        if (s_skip_fn.contains(func_id))
            return false;
        if (!s_only_fn.is_empty() && !s_only_fn.contains(func_id))
            return false;
        if (s_trace)
            warnln("cranelift: compiling fn#{} ({} dispatches)", func_id, dispatches.size());

        if (s_dump_fn.contains(func_id)) {
            warnln("cranelift: dump fn#{} ({} dispatches)", func_id, dispatches.size());
            auto reg_name = [](Dispatch::RegisterOrStack reg) -> ByteString {
                if (reg == Dispatch::RegisterOrStack::Stack)
                    return "stack";
                if (reg >= Dispatch::RegisterOrStack::CallRecord)
                    return ByteString::formatted("cr{}", to_underlying(reg) - to_underlying(Dispatch::RegisterOrStack::CallRecord));
                return ByteString::formatted("reg{}", to_underlying(reg));
            };
            for (size_t ip = 0; ip < dispatches.size(); ++ip) {
                auto const& dispatch = dispatches[ip];
                auto const& addr = addresses[ip];
                ssize_t in_count = 0;
                ssize_t out_count = 0;
#    define M(name, _, ins, outs)    \
    case Instructions::name.value(): \
        in_count = ins;              \
        out_count = outs;            \
        break;
                switch (dispatch.instruction->opcode().value()) {
                    ENUMERATE_WASM_OPCODES(M)
                }
#    undef M
                StringBuilder regs;
                regs.append('(');
                for (ssize_t j = 0; j < (in_count < 0 ? 3 : in_count); ++j) {
                    if (j > 0)
                        regs.append(", "sv);
                    regs.append(reg_name(addr.sources[j]));
                }
                regs.append(')');
                if (out_count > 0) {
                    regs.appendff(" -> {}", reg_name(addr.destination));
                }
                warnln("  [{:>03}] {} {} dst={}", ip, instruction_name(dispatch.instruction->opcode()), regs.to_byte_string(), reg_name(addr.destination));
            }
        }
    }

    Vector<CraneliftInsn> flat;
    flat.ensure_capacity(dispatches.size());
    for (size_t i = 0; i < dispatches.size(); ++i) {
        flat.append(serialize_insn(dispatches[i], addresses[i]));

        if (dispatches[i].instruction->opcode().value() == Instructions::br_table.value()) {
            auto const& table_args = dispatches[i].instruction->arguments().get<Instruction::TableBranchArgs>();
            auto const total = table_args.labels.size();
            for (size_t base = 8; base < total; base += 8) {
                CraneliftInsn cont {};
                cont.opcode = Instructions::synthetic_br_table_cont.value();
                auto const chunk = min(total - base, static_cast<size_t>(8));
                cont.imm3 = static_cast<u32>(chunk);
                for (size_t j = 0; j < chunk; ++j) {
                    auto const encoded = static_cast<u64>(table_args.labels[base + j].value()) << ((j % 4) * 16);
                    if (j < 4)
                        cont.imm1 |= static_cast<i64>(encoded);
                    else
                        cont.imm2 |= static_cast<i64>(encoded);
                }
                flat.append(cont);
            }
        }
    }

    s_pending_batch.append({ move(flat), result_arity, &compiled });
    return false; // Not compiled yet, will be compiled in flush.
#endif
}

void flush_cranelift_batch()
{
    if (s_pending_batch.is_empty())
        return;
    try_cranelift_compile_batch(s_pending_batch);
    s_pending_batch.clear();
}

void free_cranelift_code(void* handle)
{
    if (handle) {
        auto* mapping = static_cast<CodeMapping*>(handle);
#if defined(AK_OS_WINDOWS)
        VirtualFree(mapping->mapping, 0, MEM_RELEASE);
#else
        munmap(mapping->mapping, mapping->size);
#endif
        delete mapping;
    }
}

}
