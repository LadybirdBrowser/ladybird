/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/Platform.h>
#include <AK/ScopeGuard.h>
#include <CraneliftFFI.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Types.h>
#include <errno.h>
#include <stdlib.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#    include <LibSync/Mutex.h>
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
    u32 layout_offset;
    u64 outcome_return;
    u64 output_size;
    u64 total_size;
};

struct InputFunctionEntry {
    u32 insn_offset;
    u32 insn_count;
    u32 result_arity;
    u32 num_locals;
    u32 locals_offset;
    u32 num_params;
};

static_assert(sizeof(InputHeader) == 32);
static_assert(sizeof(InputFunctionEntry) == 24);

struct OutputHeader {
    u32 function_count;
    u32 _pad;
    u64 code_base_offset;
    u64 reloc_region_start;
    u64 total_size;
};

struct OutputFunctionEntry {
    u64 code_offset;
    u32 code_size;
    u32 compiled;
    // Offset (relative to the start of the reloc region) and count of `HelperReloc`
    // entries describing the absolute helper addresses baked into this function's code.
    // On cache install we walk these and rewrite the 8 bytes at code+code_offset+offset
    // with the live address of helper N for the current process.
    u64 reloc_offset;
    u32 reloc_count;
    u32 _padding_after_reloc_count;
    u64 trap_offset;
    u32 trap_count;
    u32 _padding;
};

static_assert(sizeof(OutputHeader) == 32);
static_assert(sizeof(OutputFunctionEntry) == 48);
static_assert(offsetof(OutputFunctionEntry, trap_offset) == 32);

struct CodeMapping {
    void* mapping;
    size_t size;
    Vector<CraneliftTrap> traps;
};

static constexpr size_t oop_code_region_min_size = 256 * KiB;
static constexpr size_t oop_code_bytes_per_insn = 256;
static constexpr size_t oop_reloc_region_min_size = 64 * KiB;
static constexpr size_t oop_reloc_bytes_per_insn = 128;

static size_t align_up(size_t value, size_t alignment)
{
    VERIFY(alignment > 0);
    auto remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

static ErrorOr<size_t> compute_output_buffer_size(size_t function_count, size_t instruction_count)
{
    Checked<size_t> output_entries_size = sizeof(OutputFunctionEntry);
    output_entries_size *= function_count;
    if (output_entries_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    Checked<size_t> output_size = sizeof(OutputHeader);
    output_size += output_entries_size.value();
    if (output_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    output_size = align_up(output_size.value(), SERIALIZED_CODE_ALIGNMENT);

    Checked<size_t> maximum_code_size = instruction_count;
    maximum_code_size *= oop_code_bytes_per_insn;
    if (maximum_code_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    output_size += max(oop_code_region_min_size, maximum_code_size.value());
    if (output_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    output_size = align_up(output_size.value(), alignof(HelperReloc));

    Checked<size_t> maximum_reloc_size = instruction_count;
    maximum_reloc_size *= oop_reloc_bytes_per_insn;
    if (maximum_reloc_size.has_overflow())
        return Error::from_string_literal("Cranelift output size overflow");

    output_size += max(oop_reloc_region_min_size, maximum_reloc_size.value());
    if (output_size.has_overflow() || output_size.value() > NumericLimits<u32>::max())
        return Error::from_string_literal("Cranelift output is too large");

    return output_size.value();
}

struct BatchInput {
    Vector<CraneliftInsn> insns;
    u32 result_arity;
    u32 function_index;
    CompiledInstructions* target;
    u32 num_locals;
    u32 num_params;
};

// Disk-cache blob format. Stable: cached files name format_version + layout_hash so
// any rebuild that changes those will simply miss the cache rather than try to
// execute incompatible bytes.
constexpr u64 cache_blob_magic = 0x4354494A4D534157ULL; // "WASMJITC" little-endian
constexpr u32 cache_blob_format_version = 13;

struct CacheBlobHeader {
    u64 magic;
    u32 format_version;
    u32 helper_count;
    u64 layout_hash;
    u8 wasm_hash[32];
    u32 function_count;
    u32 _pad;
};
static_assert(sizeof(CacheBlobHeader) == 64);

struct CacheBlobFunctionEntry {
    u32 function_index;
    u32 code_size;
    u32 reloc_count;
    u32 trap_count;
};
static_assert(sizeof(CacheBlobFunctionEntry) == 16);

struct CacheRecord {
    u32 function_index;
    ByteBuffer unpatched_code;
    Vector<HelperReloc> relocs;
    Vector<CraneliftTrap> traps;
};

// On a cache miss we capture every successful compile so we can hand the blob to a
// store callback after validation finishes. On a cache hit we populate the install
// map up front; the per-function lookup happens inside try_cranelift_compile when
// the dispatch table for that function has just been built and is ready to receive
// a handler_ptr.
struct CacheCaptureState {
    bool capturing { false };
    Vector<CacheRecord> records;
};
struct PendingInstallState {
    bool active { false };
    HashMap<u32, CacheRecord> records;
};
struct CacheState {
    CacheCaptureState cache_capture;
    PendingInstallState pending_install;
    Vector<BatchInput> pending_batch;
};

static thread_local u32 s_active_function_index = NumericLimits<u32>::max();

static CacheState& cranelift_cache_state()
{
    static thread_local auto* state = new CacheState;
    return *state;
}

static CraneliftCompileCallback& cranelift_compile_callback()
{
    static NeverDestroyed<CraneliftCompileCallback> callback;
    return *callback;
}

static u64 compute_layout_hash(RuntimeLayout const& layout)
{
    auto fnv1a = [](u64 hash, u64 value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 0xff;
            hash *= 0x100000001b3ULL;
        }
        return hash;
    };
    u64 hash = 0xcbf29ce484222325ULL;
    hash = fnv1a(hash, layout.regs_offset);
    hash = fnv1a(hash, layout.value_size);
    hash = fnv1a(hash, layout.locals_base_offset);
    hash = fnv1a(hash, layout.memory_instances_offset);
    hash = fnv1a(hash, layout.global_instances_offset);
    hash = fnv1a(hash, layout.global_instance_value_offset);
    hash = fnv1a(hash, layout.memory_instance_data_offset);
    hash = fnv1a(hash, layout.memory_buffer_storage_offset_offset);
    hash = fnv1a(hash, layout.compiled_call_result_scratch_offset);
    hash = fnv1a(hash, layout.value_stack_base_offset);
    hash = fnv1a(hash, layout.value_stack_top_offset);
    hash = fnv1a(hash, layout.call_record_base_offset);
    return hash;
}

using RuntimeHelperAddresses = Array<size_t, HELPER_COUNT>;
static_assert(HELPER_COUNT == 13);

static bool apply_helper_relocs(u8* code_bytes, size_t code_size, HelperReloc const* relocs, size_t reloc_count, RuntimeHelperAddresses const& helper_addresses)
{
    for (size_t i = 0; i < reloc_count; ++i) {
        auto const& r = relocs[i];
        if (r.helper_id >= HELPER_COUNT)
            return false;
        if (static_cast<size_t>(r.code_offset) + sizeof(u64) > code_size)
            return false;
        u64 addr = static_cast<u64>(helper_addresses[r.helper_id]) + static_cast<u64>(r.addend);
        __builtin_memcpy(code_bytes + r.code_offset, &addr, sizeof(addr));
    }
    return true;
}

// Allocate an RX-able page, copy the (still unpatched) machine code into it, apply the
// helper-address patches, and install the resulting function pointer into `target`.
// Used by both the fresh-compile path (bytes come from the subprocess shm) and the
// cache-install path (bytes come from a `.wasmjit` blob).
static bool install_compiled_function(CompiledInstructions& target, ReadonlyBytes code_bytes, HelperReloc const* relocs, size_t reloc_count, ReadonlySpan<CraneliftTrap> traps, RuntimeHelperAddresses const& helper_addresses)
{
    if (target.dispatches.is_empty())
        return false;

    auto const code_size = code_bytes.size();
    if (code_size == 0)
        return false;

#if defined(AK_OS_WINDOWS)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto const page_size = static_cast<size_t>(si.dwPageSize);
    auto const rx_aligned_size = (code_size + page_size - 1) & ~(page_size - 1);
    auto* jit_mem = VirtualAlloc(nullptr, rx_aligned_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!jit_mem)
        return false;
    __builtin_memcpy(jit_mem, code_bytes.data(), code_size);
    if (!apply_helper_relocs(static_cast<u8*>(jit_mem), code_size, relocs, reloc_count, helper_addresses)) {
        VirtualFree(jit_mem, 0, MEM_RELEASE);
        return false;
    }
    DWORD old_protect;
    VirtualProtect(jit_mem, rx_aligned_size, PAGE_EXECUTE_READ, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), jit_mem, code_size);
    auto* func_ptr = static_cast<u8 const*>(jit_mem);
    auto* handle = new CodeMapping { jit_mem, rx_aligned_size, {} };
#elif defined(AK_OS_MACOS)
    auto const page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto const rx_aligned_size = (code_size + page_size - 1) & ~(page_size - 1);
    auto* jit_mapping = mmap(nullptr, rx_aligned_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit_mapping == MAP_FAILED)
        return false;

    pthread_jit_write_protect_np(0);
    __builtin_memcpy(jit_mapping, code_bytes.data(), code_size);
    if (!apply_helper_relocs(static_cast<u8*>(jit_mapping), code_size, relocs, reloc_count, helper_addresses)) {
        munmap(jit_mapping, rx_aligned_size);
        return false;
    }
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(jit_mapping, code_size);
    auto* func_ptr = static_cast<u8 const*>(jit_mapping);
    auto* handle = new CodeMapping { jit_mapping, rx_aligned_size, {} };
#else
    auto const page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto const rx_aligned_size = (code_size + page_size - 1) & ~(page_size - 1);
    auto* rw_mapping = mmap(nullptr, rx_aligned_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rw_mapping == MAP_FAILED)
        return false;
    __builtin_memcpy(rw_mapping, code_bytes.data(), code_size);
    if (!apply_helper_relocs(static_cast<u8*>(rw_mapping), code_size, relocs, reloc_count, helper_addresses) || mprotect(rw_mapping, rx_aligned_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(rw_mapping, rx_aligned_size);
        return false;
    }
    __builtin___clear_cache(static_cast<char*>(rw_mapping), static_cast<char*>(rw_mapping) + code_size);
    auto* func_ptr = static_cast<u8 const*>(rw_mapping);
    auto* handle = new CodeMapping { rw_mapping, rx_aligned_size, {} };
#endif

    handle->traps.ensure_capacity(traps.size());
    for (auto const& trap : traps)
        handle->traps.unchecked_append(trap);

    target.cranelift_code_handle = handle;
    target.cranelift_code_size = code_size;
    target.cranelift_traps = handle->traps.data();
    target.cranelift_trap_count = handle->traps.size();
    target.cranelift_compiled = true;
    publish_cranelift_entry(target, bit_cast<FlatPtr>(func_ptr));
    return true;
}

}

extern "C" {

static ALWAYS_INLINE i32 wasm_cl_run_compiled(BytecodeInterpreter& interpreter, Configuration& config, CompiledFunctionEntry const& entry, Value* callee_locals)
{
    BytecodeInterpreter::CallFrameHandle handle { interpreter, config };
    config.set_frame_lightweight(*entry.module, callee_locals, *entry.expression, entry.arity, entry.max_call_rec_size);
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

static NEVER_INLINE COLD i32 wasm_cl_run_compiled_with_heap_locals(BytecodeInterpreter& interpreter, Configuration& config, CompiledFunctionEntry const& entry, Value const* args, size_t arg_count)
{
    auto total = arg_count + entry.total_local_count;
    Vector<Value, ArgumentsStaticSize> heap_locals;
    heap_locals.ensure_capacity(total);
    heap_locals.resize_and_keep_capacity(total);
    auto* callee_locals = heap_locals.data();
    for (size_t i = 0; i < arg_count; i++)
        callee_locals[i] = args[i];
    // The non-argument slots are left uninitialized; the compiled entry block zeroes its own locals.
    return wasm_cl_run_compiled(interpreter, config, entry, callee_locals);
}

static ALWAYS_INLINE i32 wasm_cl_finish_call(BytecodeInterpreter& interpreter, Configuration& config, FunctionAddress address, Value const* args, size_t arg_count)
{
    if (interpreter.trap_if_insufficient_native_stack_space())
        return 1;

    auto* instance = config.store().unsafe_get(address);

    if (auto* wasm_function = instance->get_pointer<WasmFunction>(); wasm_function
        && !config.should_limit_instruction_count()
        && cranelift_entry_acquire(wasm_function->code().func().body().compiled_instructions) != 0) {

        // Fast compiled-to-compiled call: stack-allocate locals + non-owning frame.
        auto& func = wasm_function->code().func();
        auto& ci = func.body().compiled_instructions;
        CompiledFunctionEntry const entry {
            .handler_ptr = cranelift_entry_acquire(ci),
            .dispatches_ptr = bit_cast<FlatPtr>(ci.dispatches.data()),
            .src_dst_ptr = bit_cast<FlatPtr>(ci.src_dst_mappings.data()),
            .first_insn = ci.dispatches[0].instruction,
            .expression = &func.body(),
            .module = &wasm_function->module(),
            // Match the direct-call table's sizing: the JIT frame is grown by the inlined-callee
            // locals too, so the buffer must cover them or the callee scribbles past its end.
            .total_local_count = static_cast<u32>(func.total_local_count()) + ci.cranelift_inlined_locals,
            .arity = static_cast<u32>(wasm_function->type().results().size()),
            .max_call_rec_size = static_cast<u32>(ci.max_call_rec_size),
        };

        if (arg_count + entry.total_local_count > 64) [[unlikely]]
            return wasm_cl_run_compiled_with_heap_locals(interpreter, config, entry, args, arg_count);

        // Opt out of -ftrivial-auto-var-init: only the argument slots are written below, and the
        // compiled entry block initializes its own locals, so nothing reads the rest.
        __attribute__((uninitialized)) Value callee_locals[64];
        for (size_t i = 0; i < arg_count; i++)
            callee_locals[i] = args[i];
        return wasm_cl_run_compiled(interpreter, config, entry, callee_locals);
    }

    Vector<Value, ArgumentsStaticSize> args_vec;
    config.get_arguments_allocation_if_possible(args_vec, arg_count);
    args_vec.ensure_capacity(arg_count);
    for (size_t i = 0; i < arg_count; i++)
        args_vec.unchecked_append(args[i]);

    // direct-threaded interpreter path:
    if (auto* wasm_function = instance->get_pointer<WasmFunction>(); wasm_function && !config.should_limit_instruction_count() && wasm_function->code().func().body().compiled_instructions.direct) {
        BytecodeInterpreter::CallFrameHandle handle { interpreter, config };
        if (auto prepare_result = config.prepare_wasm_call(*wasm_function, args_vec); prepare_result.is_error()) {
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
        if (config.label_stack().size() > config.frame().label_index())
            config.label_stack().shrink(config.frame().label_index(), true);
        return 0;
    }

    // non-compiled call (interpreter or host function)
    Wasm::Result result { Vector<Value> {} };
    if (instance->has<WasmFunction>()) {
        BytecodeInterpreter::CallFrameHandle handle { interpreter, config };
        result = config.call(interpreter, address, args_vec);
    } else {
        result = config.call(interpreter, address, args_vec);
        config.release_arguments_allocation(args_vec);
    }

    if (result.is_trap()) {
        interpreter.set_trap(move(result.trap()));
        return 1;
    }

    if (!result.values().is_empty())
        config.compiled_call_result_scratch() = result.values().first();
    return 0;
}

i32 wasm_cl_call_function(void* interp_ptr, void* config_ptr, i32 func_index);
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

void wasm_cl_set_trap(void* interp_ptr, u8 const* msg, i32 len);
void wasm_cl_set_trap(void* interp_ptr, u8 const* msg, i32 len)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    interpreter.set_trap(StringView(reinterpret_cast<char const*>(msg), len));
}

static inline MemoryInstance* wasm_cl_get_memory(void* config_ptr, u32 mem_idx)
{
    auto& config = *static_cast<Configuration*>(config_ptr);
    return config.memory_instance(mem_idx);
}

i64 wasm_cl_memory_size(void* config_ptr, u32 mem_idx);
i64 wasm_cl_memory_size(void* config_ptr, u32 mem_idx)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    return static_cast<i64>(memory->size() / Constants::page_size);
}

i32 wasm_cl_memory_grow(void* config_ptr, u32 mem_idx, i32 pages);
i32 wasm_cl_memory_grow(void* config_ptr, u32 mem_idx, i32 pages)
{
    auto* memory = wasm_cl_get_memory(config_ptr, mem_idx);
    auto old_pages = memory->size() / Constants::page_size;
    if (!memory->grow(pages * Constants::page_size))
        return -1;
    return static_cast<i32>(old_pages);
}

i32 wasm_cl_call_indirect(void* interp_ptr, void* config_ptr, i32 table_idx, i32 type_idx, i32 element_index);
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
    // https://webassembly.github.io/spec/core/exec/instructions.html#xref-syntax-instructions-syntax-instr-control-mathsf-call-indirect-x-y
    // call_indirect's runtime check is a defined-type match (a downcast), not structural equality.
    auto const* type_actual = function->visit([](auto& f) { return f.defined_type(); });
    auto const* type_expected = module.canonical_types()[type_idx];
    if (!type_actual || !matches_defined_type(*type_actual, *type_expected))
        return interpreter.set_trap(Trap::from_string("Indirect call type mismatch"));

    auto const& function_type = function->visit([](auto const& value) -> FunctionType const& { return value.type(); });
    if (function_type.results().size() <= 1) {
        auto parameter_count = function_type.parameters().size();
        if (parameter_count > config.value_stack().size())
            return interpreter.set_trap(Trap::from_string("Insufficient arguments for indirect call"));

        auto arguments = config.value_stack().span().slice_from_end(parameter_count);
        config.value_stack().shrink(config.value_stack().size() - parameter_count);
        auto did_trap = wasm_cl_finish_call(interpreter, config, address, arguments.data(), arguments.size());
        if (!did_trap && !function_type.results().is_empty())
            config.value_stack().unchecked_append(config.compiled_call_result_scratch());
        return did_trap;
    }

    SourcesAndDestination addrs {};
    addrs.sources[0] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[1] = Dispatch::RegisterOrStack::Stack;
    addrs.sources[2] = Dispatch::RegisterOrStack::Stack;
    addrs.destination = Dispatch::RegisterOrStack::Stack;

    auto outcome = interpreter.call_address(config, address, addrs, BytecodeInterpreter::CallAddressSource::IndirectCall, BytecodeInterpreter::CallType::UsingStack);

    return outcome == Outcome::Return && interpreter.did_trap() ? 1 : 0;
}

i32 wasm_cl_memory_copy(void* interp_ptr, void* config_ptr, u32 dst_mem, u32 src_mem, i32 dst_offset, i32 src_offset, i32 count);
i32 wasm_cl_memory_copy(void* interp_ptr, void* config_ptr, u32 dst_mem, u32 src_mem, i32 dst_offset, i32 src_offset, i32 count)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto* src_instance = wasm_cl_get_memory(config_ptr, src_mem);
    auto* dst_instance = wasm_cl_get_memory(config_ptr, dst_mem);

    auto src_end = static_cast<u64>(static_cast<u32>(src_offset)) + static_cast<u32>(count);
    auto dst_end = static_cast<u64>(static_cast<u32>(dst_offset)) + static_cast<u32>(count);
    if (src_end > src_instance->size() || dst_end > dst_instance->size())
        return interpreter.set_trap(Trap::from_string("Memory access out of bounds"));

    if (count > 0)
        dst_instance->data().copy_from(src_instance->data(), static_cast<u32>(src_offset), static_cast<u32>(dst_offset), static_cast<u32>(count));

    return 0;
}

i32 wasm_cl_memory_fill(void* interp_ptr, void* config_ptr, u32 mem_idx, i32 offset, i32 value, i32 count);
i32 wasm_cl_memory_fill(void* interp_ptr, void* config_ptr, u32 mem_idx, i32 offset, i32 value, i32 count)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto* instance = wasm_cl_get_memory(config_ptr, mem_idx);

    auto end = static_cast<u64>(static_cast<u32>(offset)) + static_cast<u32>(count);
    if (end > instance->size())
        return interpreter.set_trap(Trap::from_string("Memory access out of bounds"));

    if (count > 0)
        instance->data().fill(static_cast<u32>(offset), static_cast<u8>(value), static_cast<u32>(count));

    return 0;
}

i32 wasm_cl_call_with_record(void* interp_ptr, void* config_ptr, i32 func_index);
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

    return wasm_cl_finish_call(interpreter, config, address, config.call_record_base(), type->parameters().size());
}

static NEVER_INLINE COLD i32 wasm_cl_direct_call_fallback(BytecodeInterpreter& interpreter, Configuration& config, i32 func_index, Value const* args, size_t arg_count)
{
    return wasm_cl_finish_call(interpreter, config, config.frame().module().functions()[func_index], args, arg_count);
}

// Direct compiled-to-compiled call. Falls back to wasm_cl_finish_call for non-compiled targets.
static ALWAYS_INLINE i32 wasm_cl_direct_call_impl(BytecodeInterpreter& interpreter, Configuration& config, i32 func_index, Value* args, size_t arg_count)
{
    auto const* table = config.frame().compiled_fn_table();
    auto index = static_cast<size_t>(func_index);
    if (!table || index >= table->size() || !(*table)[index].module) [[unlikely]]
        return wasm_cl_direct_call_fallback(interpreter, config, func_index, args, arg_count);

    auto const& entry = (*table)[index];

    if (config.depth() > 500) [[unlikely]] {
        interpreter.set_trap(Constants::stack_exhaustion_message);
        return 1;
    }

    if (arg_count + entry.total_local_count > 64) [[unlikely]]
        return wasm_cl_run_compiled_with_heap_locals(interpreter, config, entry, args, arg_count);

    // Opt out of -ftrivial-auto-var-init as in wasm_cl_finish_call: only the argument slots are
    // written, and the compiled entry block initializes its own locals.
    __attribute__((uninitialized)) Value callee_locals[64];
    for (size_t i = 0; i < arg_count; i++)
        callee_locals[i] = args[i];
    return wasm_cl_run_compiled(interpreter, config, entry, callee_locals);
}

i32 wasm_cl_direct_call_0(void* interp_ptr, void* config_ptr, i32 func_index);
i32 wasm_cl_direct_call_0(void* interp_ptr, void* config_ptr, i32 func_index)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    return wasm_cl_direct_call_impl(interpreter, config, func_index, nullptr, 0);
}

i32 wasm_cl_direct_call_1(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0);
i32 wasm_cl_direct_call_1(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    Value args[] = { Value(arg0) };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, args, 1);
}

i32 wasm_cl_direct_call_2(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1);
i32 wasm_cl_direct_call_2(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    Value args[] = { Value(arg0), Value(arg1) };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, args, 2);
}

i32 wasm_cl_direct_call_3(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1, i64 arg2);
i32 wasm_cl_direct_call_3(void* interp_ptr, void* config_ptr, i32 func_index, i64 arg0, i64 arg1, i64 arg2)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    Value args[] = { Value(arg0), Value(arg1), Value(arg2) };
    return wasm_cl_direct_call_impl(interpreter, config, func_index, args, 3);
}

// Thin frame push for direct compiled-to-compiled calls. Returns 1 on trap, 0 on success.
i32 wasm_cl_push_frame(void* interp_ptr, void* config_ptr, Value* locals_ptr, u32, void const* module_ptr, void const* expression_ptr, u32 arity, u32 max_call_rec_size);
i32 wasm_cl_push_frame(void* interp_ptr, void* config_ptr, Value* locals_ptr, u32 /* total_locals */, void const* module_ptr, void const* expression_ptr, u32 arity, u32 max_call_rec_size)
{
    auto& interpreter = *static_cast<BytecodeInterpreter*>(interp_ptr);
    auto& config = *static_cast<Configuration*>(config_ptr);
    auto const& module = *static_cast<ModuleInstance const*>(module_ptr);
    auto const& expression = *static_cast<Expression const*>(expression_ptr);

    if (interpreter.trap_if_insufficient_native_stack_space())
        return 1;

    config.set_frame_lightweight(module, locals_ptr, expression, arity, max_call_rec_size);
    config.depth()++;
    return 0;
}

// Thin frame pop for direct compiled-to-compiled calls.
void wasm_cl_pop_frame(void* config_ptr, u32 arity);
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

namespace Wasm {

static RuntimeHelperAddresses make_runtime_helper_addresses()
{
    RuntimeHelperAddresses addresses {};
    addresses[to_underlying(HelperId::call_function)] = bit_cast<uintptr_t>(&wasm_cl_call_function);
    addresses[to_underlying(HelperId::set_trap)] = bit_cast<uintptr_t>(&wasm_cl_set_trap);
    addresses[to_underlying(HelperId::memory_size)] = bit_cast<uintptr_t>(&wasm_cl_memory_size);
    addresses[to_underlying(HelperId::memory_grow)] = bit_cast<uintptr_t>(&wasm_cl_memory_grow);
    addresses[to_underlying(HelperId::call_with_record)] = bit_cast<uintptr_t>(&wasm_cl_call_with_record);
    addresses[to_underlying(HelperId::direct_call_0)] = bit_cast<uintptr_t>(&wasm_cl_direct_call_0);
    addresses[to_underlying(HelperId::direct_call_1)] = bit_cast<uintptr_t>(&wasm_cl_direct_call_1);
    addresses[to_underlying(HelperId::direct_call_2)] = bit_cast<uintptr_t>(&wasm_cl_direct_call_2);
    addresses[to_underlying(HelperId::direct_call_3)] = bit_cast<uintptr_t>(&wasm_cl_direct_call_3);
    addresses[to_underlying(HelperId::call_indirect)] = bit_cast<uintptr_t>(&wasm_cl_call_indirect);
    addresses[to_underlying(HelperId::memory_copy)] = bit_cast<uintptr_t>(&wasm_cl_memory_copy);
    addresses[to_underlying(HelperId::memory_fill)] = bit_cast<uintptr_t>(&wasm_cl_memory_fill);
    addresses[to_underlying(HelperId::primitive_storage_cage_base)] = bit_cast<uintptr_t>(&js_primitive_storage_cage_base);
    return addresses;
}

static RuntimeLayout make_runtime_layout()
{
    return RuntimeLayout {
        .regs_offset = static_cast<u32>(offsetof(Configuration, regs)),
        .value_size = static_cast<u32>(sizeof(Value)),
        .locals_base_offset = static_cast<u32>(Configuration::locals_base_offset()),
        .memory_instances_offset = static_cast<u32>(Configuration::memory_instances_offset()),
        .global_instances_offset = static_cast<u32>(Configuration::global_instances_offset()),
        .global_instance_value_offset = static_cast<u32>(GlobalInstance::value_offset()),
        .memory_instance_data_offset = static_cast<u32>(MemoryInstance::data_offset()),
        .memory_buffer_storage_offset_offset = static_cast<u32>(MemoryBuffer::storage_offset_offset()),
        .compiled_call_result_scratch_offset = static_cast<u32>(Configuration::compiled_call_result_scratch_offset()),
        .value_stack_base_offset = static_cast<u32>(Configuration::value_stack_base_offset()),
        .value_stack_top_offset = static_cast<u32>(Configuration::value_stack_top_offset()),
        .call_record_base_offset = static_cast<u32>(Configuration::call_record_base_offset()),
    };
}

static RuntimeLayout const& runtime_layout()
{
    static auto const layout = make_runtime_layout();
    return layout;
}

static u64 runtime_layout_hash()
{
    static auto const hash = compute_layout_hash(runtime_layout());
    return hash;
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
    u32 opc = out.opcode;

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
        out.imm2 = struct_args.else_ip().has_value()
            ? static_cast<i64>(struct_args.else_ip()->value())
            : -1;
        u32 arity = struct_args.meta.arity;
        u32 param_count = struct_args.meta.parameter_count;
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
        out.imm3 = static_cast<u32>(mem_arg.memory_index.value());
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
            out.imm3 = static_cast<u32>(mem_arg.memory_index.value());
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

ByteString const& cranelift_compiler_path()
{
    // Lookup order: LADYBIRD_CRANELIFT_COMPILER, compile-time path, sibling-of-self.
    static NeverDestroyed<ByteString> s_path = []() -> ByteString {
        auto file_exists = [](ByteString const& path) {
            return FileSystem::exists(path);
        };

        if (auto const* env = getenv("LADYBIRD_CRANELIFT_COMPILER"); env && *env) {
            if (file_exists(env))
                return ByteString { env };
        }

        if (file_exists(WASM_CRANELIFT_COMPILER_PATH))
            return WASM_CRANELIFT_COMPILER_PATH;

        if (auto self_path = Core::System::current_executable_path(); !self_path.is_error()) {
            auto sibling = LexicalPath::join(LexicalPath::dirname(self_path.value()), "cranelift-compiler"sv).string();
            if (file_exists(sibling))
                return sibling;
        }

        return WASM_CRANELIFT_COMPILER_PATH;
    }();

    return *s_path;
}

template<typename T>
static ErrorOr<T> read_cranelift_input(ReadonlyBytes input, size_t offset)
{
    Checked<size_t> end = offset;
    end += sizeof(T);
    if (end.has_overflow() || end.value() > input.size())
        return Error::from_string_literal("Cranelift input is truncated");

    T value;
    input.slice(offset, sizeof(T)).copy_to({ &value, sizeof(T) });
    return value;
}

static ErrorOr<Core::AnonymousBuffer> create_cranelift_output_buffer(ReadonlyBytes input)
{
    if (input.size() < sizeof(InputHeader))
        return Error::from_string_literal("Cranelift input header is truncated");

    auto header = TRY(read_cranelift_input<InputHeader>(input, 0));
    if (header.total_size != input.size())
        return Error::from_string_literal("Cranelift input size does not match its header");

    Checked<size_t> entries_size = sizeof(InputFunctionEntry);
    entries_size *= header.function_count;
    if (entries_size.has_overflow())
        return Error::from_string_literal("Cranelift input entries are too large");

    Checked<size_t> entries_end = sizeof(InputHeader);
    entries_end += entries_size.value();
    if (entries_end.has_overflow() || entries_end.value() > input.size())
        return Error::from_string_literal("Cranelift input entries are truncated");

    size_t instruction_count = 0;
    size_t locals_size = 0;
    for (size_t i = 0; i < header.function_count; ++i) {
        auto entry = TRY(read_cranelift_input<InputFunctionEntry>(input, sizeof(InputHeader) + i * sizeof(InputFunctionEntry)));
        Checked<size_t> new_instruction_count = instruction_count;
        new_instruction_count += entry.insn_count;
        if (new_instruction_count.has_overflow())
            return Error::from_string_literal("Cranelift instruction count overflow");
        instruction_count = new_instruction_count.value();

        Checked<size_t> new_locals_size = locals_size;
        new_locals_size += entry.num_locals;
        if (new_locals_size.has_overflow())
            return Error::from_string_literal("Cranelift locals size overflow");
        locals_size = new_locals_size.value();
    }

    auto insn_region_offset = align_up(entries_end.value(), alignof(CraneliftInsn));
    Checked<size_t> insn_region_size = instruction_count;
    insn_region_size *= sizeof(CraneliftInsn);
    if (insn_region_size.has_overflow())
        return Error::from_string_literal("Cranelift instruction region is too large");

    Checked<size_t> locals_region_offset = insn_region_offset;
    locals_region_offset += insn_region_size.value();
    if (locals_region_offset.has_overflow())
        return Error::from_string_literal("Cranelift locals region offset overflow");

    Checked<size_t> layout_offset = locals_region_offset.value();
    layout_offset += locals_size;
    if (layout_offset.has_overflow())
        return Error::from_string_literal("Cranelift layout offset overflow");
    auto aligned_layout_offset = align_up(layout_offset.value(), alignof(RuntimeLayout));

    Checked<size_t> expected_input_size = aligned_layout_offset;
    expected_input_size += sizeof(RuntimeLayout);
    if (expected_input_size.has_overflow() || expected_input_size.value() != input.size() || header.layout_offset != aligned_layout_offset)
        return Error::from_string_literal("Cranelift input regions are not canonical");

    size_t insn_cursor = insn_region_offset;
    size_t locals_cursor = locals_region_offset.value();
    for (size_t i = 0; i < header.function_count; ++i) {
        auto entry = TRY(read_cranelift_input<InputFunctionEntry>(input, sizeof(InputHeader) + i * sizeof(InputFunctionEntry)));
        if (entry.insn_offset != insn_cursor || entry.locals_offset != locals_cursor)
            return Error::from_string_literal("Cranelift input regions are not canonical");

        insn_cursor += static_cast<size_t>(entry.insn_count) * sizeof(CraneliftInsn);
        locals_cursor += entry.num_locals;
    }

    auto output_size = TRY(compute_output_buffer_size(header.function_count, instruction_count));
    if (header.output_size != output_size)
        return Error::from_string_literal("Cranelift output size does not match its input");

    return Core::AnonymousBuffer::create_with_size(output_size, Core::AnonymousBuffer::Sealability::Sealable);
}

static ErrorOr<Core::AnonymousBuffer> finalize_cranelift_output_buffer(Core::AnonymousBuffer const& output)
{
    if (output.size() < sizeof(OutputHeader))
        return Error::from_string_literal("Cranelift output header is truncated");

    auto const& header = *output.data<OutputHeader>();
    auto output_size = static_cast<size_t>(header.total_size);
    if (output_size < sizeof(OutputHeader) || output_size > output.size())
        return Error::from_string_literal("Cranelift output size is invalid");

    auto output_fd = TRY(Core::System::dup(output.fd()));
    return Core::AnonymousBuffer::create_from_anon_fd(output_fd, output_size);
}

ErrorOr<Core::AnonymousBuffer> compile_cranelift_buffer(Core::AnonymousBuffer const& input)
{
    auto output = TRY(create_cranelift_output_buffer(input.bytes()));

#if defined(AK_OS_WINDOWS)
    auto process = TRY([&]() -> ErrorOr<Core::Process> {
        // FIXME: Use Core::FileAction::DupFd once it is supported on Windows so spawning does not require temporarily
        //        inheritable handles.
        static Sync::Mutex spawn_mutex;
        Sync::MutexLocker locker(spawn_mutex);

        auto inherited_input_fd = TRY(Core::System::dup(input.fd()));
        ScopeGuard close_inherited_input_fd { [&]() { (void)Core::System::close(inherited_input_fd); } };
        TRY(Core::System::set_close_on_exec(inherited_input_fd, false));

        auto inherited_output_fd = TRY(Core::System::dup(output.fd()));
        ScopeGuard close_inherited_output_fd { [&]() { (void)Core::System::close(inherited_output_fd); } };
        TRY(Core::System::set_close_on_exec(inherited_output_fd, false));

        Vector<ByteString> arguments;
        arguments.append(ByteString::number(reinterpret_cast<uintptr_t>(to_handle(inherited_input_fd))));
        arguments.append(ByteString::number(input.size()));
        arguments.append(ByteString::number(reinterpret_cast<uintptr_t>(to_handle(inherited_output_fd))));
        arguments.append(ByteString::number(output.size()));

        return Core::Process::spawn({
            .name = "cranelift-compiler"sv,
            .executable = cranelift_compiler_path(),
            .die_with_parent = true,
            .arguments = arguments,
        });
    }());
#else
    // Reserve the child descriptor numbers with close-on-exec duplicates. This prevents concurrent spawns from claiming
    // or inheriting them before the child-side DupFd actions replace them and clear close-on-exec.
    auto compiler_input_fd = TRY(Core::System::fcntl(input.fd(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1));
    ScopeGuard close_compiler_input_fd { [&]() { (void)Core::System::close(compiler_input_fd); } };

    auto compiler_output_fd = TRY(Core::System::fcntl(output.fd(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1));
    ScopeGuard close_compiler_output_fd { [&]() { (void)Core::System::close(compiler_output_fd); } };

    Vector<ByteString> arguments;
    arguments.append(ByteString::number(compiler_input_fd));
    arguments.append(ByteString::number(input.size()));
    arguments.append(ByteString::number(compiler_output_fd));
    arguments.append(ByteString::number(output.size()));

    auto process = TRY(Core::Process::spawn({
        .name = "cranelift-compiler"sv,
        .executable = cranelift_compiler_path(),
        .die_with_parent = true,
        .arguments = arguments,
        .file_actions = {
            Core::FileAction::DupFd { .write_fd = input.fd(), .fd = compiler_input_fd },
            Core::FileAction::DupFd { .write_fd = output.fd(), .fd = compiler_output_fd },
        },
    }));
#endif

    if (TRY(process.wait_for_termination()) != 0)
        return Error::from_string_literal("Failed to compile a WebAssembly module");

    return finalize_cranelift_output_buffer(output);
}

static ErrorOr<void> try_cranelift_compile_batch(ReadonlySpan<BatchInput> batch)
{
    if (batch.is_empty())
        return {};

    static auto helper_addresses = make_runtime_helper_addresses();
    auto const& layout = runtime_layout();
    u64 outcome_return = to_underlying(Outcome::Return);

    size_t function_count = batch.size();
    auto const entries_offset = sizeof(InputHeader);
    auto const entries_size = sizeof(InputFunctionEntry) * function_count;

    size_t total_insn_count = 0;
    size_t total_locals_bytes = 0;
    for (auto const& entry : batch) {
        total_insn_count += entry.insns.size();
        total_locals_bytes += entry.num_locals;
    }

    auto const insn_region_offset = align_up(entries_offset + entries_size, alignof(CraneliftInsn));
    auto const insn_bytes = total_insn_count * sizeof(CraneliftInsn);
    auto const locals_region_offset = insn_region_offset + insn_bytes; // u8, no alignment needed
    auto const layout_offset = align_up(locals_region_offset + total_locals_bytes, alignof(RuntimeLayout));
    auto const total_size = layout_offset + sizeof(RuntimeLayout);
    auto const output_size = TRY(compute_output_buffer_size(function_count, total_insn_count));

    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(total_size, Core::AnonymousBuffer::Sealability::Sealable));
    auto* base = buffer.data<u8>();
    __builtin_memset(base, 0, total_size);

    auto* header = reinterpret_cast<InputHeader*>(base);
    *header = InputHeader {
        .function_count = static_cast<u32>(function_count),
        .layout_offset = static_cast<u32>(layout_offset),
        .outcome_return = outcome_return,
        .output_size = output_size,
        .total_size = total_size,
    };

    size_t insn_cursor = insn_region_offset;
    size_t locals_cursor = locals_region_offset;
    for (size_t i = 0; i < function_count; ++i) {
        auto const& input = batch[i];
        auto* entry = reinterpret_cast<InputFunctionEntry*>(base + entries_offset + i * sizeof(InputFunctionEntry));
        *entry = InputFunctionEntry {
            .insn_offset = static_cast<u32>(insn_cursor),
            .insn_count = static_cast<u32>(input.insns.size()),
            .result_arity = input.result_arity,
            .num_locals = input.num_locals,
            .locals_offset = static_cast<u32>(locals_cursor),
            .num_params = input.num_params,
        };
        __builtin_memcpy(base + insn_cursor, input.insns.data(), input.insns.size() * sizeof(CraneliftInsn));
        insn_cursor += input.insns.size() * sizeof(CraneliftInsn);

        auto const& local_types = input.target->cranelift_local_types;
        for (u32 l = 0; l < input.num_locals; ++l)
            base[locals_cursor + l] = l < local_types.size() ? local_types[l] : static_cast<u8>(ValueType::I64);
        locals_cursor += input.num_locals;
    }

    __builtin_memcpy(base + layout_offset, &layout, sizeof(layout));

    auto output_buffer = TRY([&]() -> ErrorOr<Core::AnonymousBuffer> {
        if (auto& callback = cranelift_compile_callback()) {
            auto output = callback(buffer);
            return output.snapshot();
        }
        return compile_cranelift_buffer(buffer);
    }());
    if (!output_buffer.is_valid() || output_buffer.size() < sizeof(OutputHeader))
        return Error::from_string_literal("Failed to compile a WebAssembly module");

    // Extract results for each function.
    auto const* output_base = output_buffer.data<u8>();
    auto const& output_header = *reinterpret_cast<OutputHeader const*>(output_base);
    auto const output_entries_offset = sizeof(OutputHeader);
    auto const output_entries_size = sizeof(OutputFunctionEntry) * function_count;
    auto const code_base_offset = static_cast<size_t>(output_header.code_base_offset);
    auto const reloc_region_start = static_cast<size_t>(output_header.reloc_region_start);
    auto const compact_output_size = static_cast<size_t>(output_header.total_size);
    if (output_header.function_count != function_count
        || compact_output_size != output_buffer.size()
        || code_base_offset < output_entries_offset + output_entries_size
        || code_base_offset % SERIALIZED_CODE_ALIGNMENT != 0
        || reloc_region_start < code_base_offset
        || reloc_region_start % alignof(HelperReloc) != 0
        || reloc_region_start > compact_output_size)
        return Error::from_string_literal("Cranelift compiler returned an invalid buffer");

    auto const code_region_size = reloc_region_start - code_base_offset;
    auto const reloc_region_size = compact_output_size - reloc_region_start;

    for (size_t i = 0; i < function_count; ++i) {
        auto const& output = *reinterpret_cast<OutputFunctionEntry const*>(output_base + output_entries_offset + i * sizeof(OutputFunctionEntry));
        if (!output.compiled)
            continue;

        auto code_offset = static_cast<size_t>(output.code_offset);
        auto code_size = static_cast<size_t>(output.code_size);
        if (code_offset > code_region_size || code_size > code_region_size - code_offset)
            continue;
        auto code_start = code_base_offset + code_offset;
        if (code_start + code_size > compact_output_size)
            continue;

        auto const reloc_offset = static_cast<size_t>(output.reloc_offset);
        auto const reloc_count = static_cast<size_t>(output.reloc_count);
        auto const reloc_bytes = reloc_count * sizeof(HelperReloc);
        if (reloc_count != 0 && reloc_bytes / sizeof(HelperReloc) != reloc_count)
            continue;
        if (reloc_offset % alignof(HelperReloc) != 0)
            continue;
        if (reloc_offset > reloc_region_size || reloc_bytes > reloc_region_size - reloc_offset)
            continue;
        if (reloc_region_start + reloc_offset + reloc_bytes > compact_output_size)
            continue;

        auto const trap_offset = static_cast<size_t>(output.trap_offset);
        auto const trap_count = static_cast<size_t>(output.trap_count);
        auto const trap_bytes = trap_count * sizeof(CraneliftTrap);
        if (trap_count != 0 && trap_bytes / sizeof(CraneliftTrap) != trap_count)
            continue;
        if (trap_offset % alignof(CraneliftTrap) != 0)
            continue;
        if (trap_offset > reloc_region_size || trap_bytes > reloc_region_size - trap_offset)
            continue;
        if (reloc_region_start + trap_offset + trap_bytes > compact_output_size)
            continue;

        auto code_bytes = ReadonlyBytes { output_base + code_start, code_size };
        auto const* relocs = reloc_count == 0
            ? nullptr
            : reinterpret_cast<HelperReloc const*>(output_base + reloc_region_start + reloc_offset);
        auto traps = trap_count == 0
            ? ReadonlySpan<CraneliftTrap> {}
            : ReadonlySpan<CraneliftTrap> { reinterpret_cast<CraneliftTrap const*>(output_base + reloc_region_start + trap_offset), trap_count };

        auto& capture = cranelift_cache_state().cache_capture;
        if (capture.capturing && batch[i].function_index != NumericLimits<u32>::max()) {
            if (auto copy = ByteBuffer::copy(code_bytes.data(), code_bytes.size()); !copy.is_error()) {
                CacheRecord rec;
                rec.function_index = batch[i].function_index;
                rec.unpatched_code = copy.release_value();
                rec.relocs.ensure_capacity(reloc_count);
                for (size_t j = 0; j < reloc_count; ++j)
                    rec.relocs.unchecked_append(relocs[j]);
                rec.traps.ensure_capacity(trap_count);
                for (size_t j = 0; j < trap_count; ++j)
                    rec.traps.unchecked_append(traps[j]);
                capture.records.append(move(rec));
            }
        }

        install_compiled_function(*batch[i].target, code_bytes, relocs, reloc_count, traps, helper_addresses);
    }

    return {};
}

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

    // Already installed (either from a prior compile or a previous cache install in this
    // same validation pass) -- nothing to do.
    if (compiled.cranelift_compiled)
        return true;

    if (s_active_function_index == NumericLimits<u32>::max())
        return false;

    // Cache hit: install from the parsed blob instead of going through cranelift.
    //            dispatches[] has just been populated by try_compile_instructions, so handler_ptr is ready to be set.
    if (cranelift_cache_state().pending_install.active && s_active_function_index != NumericLimits<u32>::max()) {
        auto record = cranelift_cache_state().pending_install.records.take(s_active_function_index);
        if (record.has_value()) {
            static auto cache_install_helper_addresses = make_runtime_helper_addresses();
            if (install_compiled_function(
                    compiled,
                    record->unpatched_code.bytes(),
                    record->relocs.is_empty() ? nullptr : record->relocs.data(),
                    record->relocs.size(),
                    record->traps.span(),
                    cache_install_helper_addresses)) {
                return true;
            }
            // Put it back so we can try later.
            cranelift_cache_state().pending_install.records.set(s_active_function_index, record.release_value());
        }
    }

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
        static auto& s_skip_fn = *new HashTable<size_t>(read_set_env("CRANELIFT_SKIP_FN"));
        static auto& s_only_fn = *new HashTable<size_t>(read_set_env("CRANELIFT_ONLY_FN"));
        static auto& s_dump_fn = *new HashTable<size_t>(read_set_env("CRANELIFT_DUMP_FN"));
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

        if (dispatches[i].instruction->opcode().value() == Instructions::synthetic_tier_up.value())
            flat.last().imm1 = static_cast<i64>(i);

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

    cranelift_cache_state().pending_batch.append({ move(flat), result_arity, s_active_function_index, &compiled, compiled.cranelift_local_count, compiled.cranelift_param_count });
    return false; // Not compiled yet, will be compiled in flush.
#endif
}

void flush_cranelift_batch()
{
    if (cranelift_cache_state().pending_batch.is_empty())
        return;

    auto result = try_cranelift_compile_batch(cranelift_cache_state().pending_batch);
    if (result.is_error())
        warnln("Cranelift compilation failed: {}", result.error());

    cranelift_cache_state().pending_batch.clear();
}

void discard_cranelift_batch()
{
    cranelift_cache_state().pending_batch.clear();
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

void set_cranelift_active_function_index(u32 function_index)
{
    s_active_function_index = function_index;
}

void begin_cranelift_cache_capture()
{
    cranelift_cache_state().cache_capture.capturing = true;
    cranelift_cache_state().cache_capture.records.clear();
}

void abort_cranelift_cache_capture()
{
    cranelift_cache_state().cache_capture.capturing = false;
    cranelift_cache_state().cache_capture.records.clear();
}

void abort_cranelift_cache_install()
{
    cranelift_cache_state().pending_install.active = false;
    cranelift_cache_state().pending_install.records.clear();
}

Optional<ByteBuffer> serialize_cranelift_cache_blob(ReadonlyBytes wasm_hash)
{
    ScopeGuard reset = [] {
        cranelift_cache_state().cache_capture.capturing = false;
        cranelift_cache_state().cache_capture.records.clear();
    };

    auto const& capture = cranelift_cache_state().cache_capture;

    if (!capture.capturing || capture.records.is_empty())
        return {};
    if (wasm_hash.size() != 32)
        return {};

    size_t total_size = sizeof(CacheBlobHeader);
    for (auto const& r : capture.records) {
        total_size += sizeof(CacheBlobFunctionEntry);
        total_size += align_up(r.unpatched_code.size(), 16);
        total_size += r.relocs.size() * sizeof(HelperReloc);
        total_size += r.traps.size() * sizeof(CraneliftTrap);
    }

    auto blob_or_error = ByteBuffer::create_zeroed(total_size);
    if (blob_or_error.is_error())
        return {};
    auto blob = blob_or_error.release_value();
    auto* out = blob.data();

    auto* header = reinterpret_cast<CacheBlobHeader*>(out);
    header->magic = cache_blob_magic;
    header->format_version = cache_blob_format_version;
    header->helper_count = HELPER_COUNT;
    header->layout_hash = runtime_layout_hash();
    __builtin_memcpy(header->wasm_hash, wasm_hash.data(), 32);
    header->function_count = static_cast<u32>(capture.records.size());

    size_t offset = sizeof(CacheBlobHeader);
    for (auto const& r : capture.records) {
        auto* entry = reinterpret_cast<CacheBlobFunctionEntry*>(out + offset);
        entry->function_index = r.function_index;
        entry->code_size = static_cast<u32>(r.unpatched_code.size());
        entry->reloc_count = static_cast<u32>(r.relocs.size());
        entry->trap_count = static_cast<u32>(r.traps.size());
        offset += sizeof(CacheBlobFunctionEntry);

        __builtin_memcpy(out + offset, r.unpatched_code.data(), r.unpatched_code.size());
        offset += align_up(r.unpatched_code.size(), 16);

        auto reloc_bytes = r.relocs.size() * sizeof(HelperReloc);
        if (reloc_bytes > 0)
            __builtin_memcpy(out + offset, r.relocs.data(), reloc_bytes);
        offset += reloc_bytes;

        auto trap_bytes = r.traps.size() * sizeof(CraneliftTrap);
        if (trap_bytes > 0)
            __builtin_memcpy(out + offset, r.traps.data(), trap_bytes);
        offset += trap_bytes;
    }

    return blob;
}

bool try_install_cranelift_cache_blob(ReadonlyBytes expected_wasm_hash, ReadonlyBytes blob)
{
    abort_cranelift_cache_install();

    if (expected_wasm_hash.size() != 32 || blob.size() < sizeof(CacheBlobHeader))
        return false;

    auto const* header = reinterpret_cast<CacheBlobHeader const*>(blob.data());
    if (header->magic != cache_blob_magic)
        return false;
    if (header->format_version != cache_blob_format_version)
        return false;
    if (header->helper_count != HELPER_COUNT)
        return false;
    if (__builtin_memcmp(header->wasm_hash, expected_wasm_hash.data(), 32) != 0)
        return false;

    if (header->layout_hash != runtime_layout_hash())
        return false;

    size_t offset = sizeof(CacheBlobHeader);
    for (u32 i = 0; i < header->function_count; ++i) {
        if (offset + sizeof(CacheBlobFunctionEntry) > blob.size())
            return false;
        auto const* entry = reinterpret_cast<CacheBlobFunctionEntry const*>(blob.data() + offset);
        offset += sizeof(CacheBlobFunctionEntry);

        auto code_off = offset;
        auto aligned_code_size = align_up(entry->code_size, 16);
        if (code_off + aligned_code_size > blob.size())
            return false;
        offset += aligned_code_size;

        auto reloc_off = offset;
        auto reloc_bytes = static_cast<size_t>(entry->reloc_count) * sizeof(HelperReloc);
        if (reloc_off + reloc_bytes > blob.size())
            return false;
        offset += reloc_bytes;

        auto trap_off = offset;
        auto trap_bytes = static_cast<size_t>(entry->trap_count) * sizeof(CraneliftTrap);
        if (trap_off + trap_bytes > blob.size())
            return false;
        offset += trap_bytes;

        auto code_copy = ByteBuffer::copy(blob.data() + code_off, entry->code_size);
        if (code_copy.is_error())
            return false;

        CacheRecord rec;
        rec.function_index = entry->function_index;
        rec.unpatched_code = code_copy.release_value();
        rec.relocs.ensure_capacity(entry->reloc_count);
        for (u32 j = 0; j < entry->reloc_count; ++j) {
            HelperReloc reloc;
            __builtin_memcpy(&reloc, blob.data() + reloc_off + j * sizeof(HelperReloc), sizeof(HelperReloc));
            rec.relocs.unchecked_append(reloc);
        }
        rec.traps.ensure_capacity(entry->trap_count);
        for (u32 j = 0; j < entry->trap_count; ++j) {
            CraneliftTrap trap;
            __builtin_memcpy(&trap, blob.data() + trap_off + j * sizeof(CraneliftTrap), sizeof(CraneliftTrap));
            rec.traps.unchecked_append(trap);
        }
        cranelift_cache_state().pending_install.records.set(entry->function_index, move(rec));
    }

    cranelift_cache_state().pending_install.active = true;
    return true;
}

void set_cranelift_compile_callback(CraneliftCompileCallback callback)
{
    auto& installed_callback = cranelift_compile_callback();
    VERIFY(!installed_callback);

    installed_callback = move(callback);
}

}
