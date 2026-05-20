/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DoublyLinkedList.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/Types.h>

namespace Wasm {

enum class SourceAddressMix {
    AllRegisters,
    AllCallRecords,
    AllStack,
    Any,
};

enum class IsTailcall {
    No,
    Yes,
};

class Configuration {
public:
    explicit Configuration(Store& store)
        : m_store(store)
    {
    }

    template<typename... Args>
    void set_frame(IsTailcall is_tailcall, Args&&... frame_init)
    {
        m_frame_stack.empend(forward<Args>(frame_init)...);

        auto& frame = m_frame_stack.last();
        m_locals_base = frame.locals_data();
        auto const& memories = frame.module().memories();
        m_default_memory = memories.is_empty() ? nullptr : m_store.unsafe_get(memories[0]);
        m_default_memory_base = m_default_memory ? m_default_memory->data().data() : nullptr;

        auto continuation = frame.expression().instructions().size() - 1;
        if (auto size = frame.expression().compiled_instructions.dispatches.size(); size > 0)
            continuation = size - 1;
        Label label(frame.arity(), continuation, m_value_stack.size());
        frame.label_index() = m_label_stack.size();
        if (auto hint = frame.expression().stack_usage_hint(); hint.has_value())
            m_value_stack.ensure_capacity(*hint + m_value_stack.size());
        if (is_tailcall == IsTailcall::No) {
            if (auto hint = frame.expression().frame_usage_hint(); hint.has_value())
                m_label_stack.ensure_capacity(*hint + m_label_stack.size());
        }
        m_label_stack.append(label);

        auto max_call_rec_size = frame.expression().compiled_instructions.max_call_rec_size;
        if (max_call_rec_size > 0) {
            get_arguments_allocation_if_possible(m_current_call_record, max_call_rec_size);
            m_current_call_record.resize_and_keep_capacity(max_call_rec_size);
            m_call_record_base = m_current_call_record.data();
        } else {
            m_call_record_base = nullptr;
        }
    }
    // Lightweight set_frame for direct Cranelift-to-Cranelift calls.
    void set_frame_lightweight(ModuleInstance const& module, Value* locals_ptr,
        Expression const& expression, size_t arity)
    {
        m_frame_stack.empend(module, locals_ptr, expression, arity);
        m_locals_base = locals_ptr;
        auto const& memories = module.memories();
        m_default_memory = memories.is_empty() ? nullptr : m_store.unsafe_get(memories[0]);
        m_default_memory_base = m_default_memory ? m_default_memory->data().data() : nullptr;
        // Skip capacity hints and label push (Cranelift uses its own structured control flow).
    }

    void setup_call_record_for_current_frame()
    {
        auto max_call_rec_size = m_frame_stack.last().expression().compiled_instructions.max_call_rec_size;
        if (max_call_rec_size > 0) {
            m_current_call_record.clear_with_capacity();
            m_current_call_record.ensure_capacity(max_call_rec_size);
            m_current_call_record.resize_and_keep_capacity(max_call_rec_size);
            m_call_record_base = m_current_call_record.data();
        } else {
            m_call_record_base = nullptr;
        }
    }

    ALWAYS_INLINE auto& frame() const { return m_frame_stack.last(); }
    ALWAYS_INLINE auto& frame() { return m_frame_stack.last(); }
    ALWAYS_INLINE auto& ip() const { return m_ip; }
    ALWAYS_INLINE auto& ip() { return m_ip; }
    ALWAYS_INLINE auto& depth() const { return m_depth; }
    ALWAYS_INLINE auto& depth() { return m_depth; }
    ALWAYS_INLINE auto& value_stack() const { return m_value_stack; }
    ALWAYS_INLINE auto& value_stack() { return m_value_stack; }
    ALWAYS_INLINE auto& label_stack() const { return m_label_stack; }
    ALWAYS_INLINE auto& label_stack() { return m_label_stack; }
    ALWAYS_INLINE auto& store() const { return m_store; }
    ALWAYS_INLINE auto& store() { return m_store; }
    ALWAYS_INLINE MemoryInstance* default_memory() const { return m_default_memory; }
    ALWAYS_INLINE u8* default_memory_base() const { return m_default_memory_base; }
    ALWAYS_INLINE void refresh_default_memory_base() { m_default_memory_base = m_default_memory ? m_default_memory->data().data() : nullptr; }
    ALWAYS_INLINE Value& compiled_call_result_scratch() { return m_compiled_call_result_scratch; }
    ALWAYS_INLINE Value const& compiled_call_result_scratch() const { return m_compiled_call_result_scratch; }

    ALWAYS_INLINE Value const& local(LocalIndex index) const { return m_locals_base[index.value()]; }
    ALWAYS_INLINE Value& local(LocalIndex index) { return m_locals_base[index.value()]; }
    ALWAYS_INLINE Value* locals_base() const { return m_locals_base; }
    ALWAYS_INLINE void set_locals_base(Value* base) { m_locals_base = base; }

    // When > 0, unwind_impl skips the frame pop (the direct call didn't push a frame).
    size_t m_compiled_direct_call_depth { 0 };

    // Per-function entry for the compiled function table (direct calls from Cranelift).
    struct CompiledFunctionEntry {
        FlatPtr handler_ptr { 0 };    // 0 = not compiled, use slow path
        FlatPtr dispatches_ptr { 0 }; // Dispatch const* for the handler call
        FlatPtr src_dst_ptr { 0 };    // SourcesAndDestination const*
        Instruction const* first_insn { nullptr };
        Expression const* expression { nullptr };
        ModuleInstance const* module { nullptr };
        u32 total_local_count { 0 };
        u32 arity { 0 };
        u32 max_call_rec_size { 0 };
    };
    HashMap<u32, CompiledFunctionEntry> m_compiled_fn_table;
    ModuleInstance const* m_compiled_fn_table_module { nullptr };

    void build_compiled_function_table();

    static constexpr size_t locals_base_offset() { return __builtin_offsetof(Configuration, m_locals_base); }
    static constexpr size_t default_memory_base_offset() { return __builtin_offsetof(Configuration, m_default_memory_base); }
    static constexpr size_t compiled_call_result_scratch_offset() { return __builtin_offsetof(Configuration, m_compiled_call_result_scratch); }

    ALWAYS_INLINE Value& call_record_entry(size_t index) { return m_call_record_base[index]; }
    ALWAYS_INLINE Value const& call_record_entry(size_t index) const { return m_call_record_base[index]; }
    ALWAYS_INLINE Value* call_record_base() const { return m_call_record_base; }
    ALWAYS_INLINE void set_call_record_base(Value* base) { m_call_record_base = base; }
    ALWAYS_INLINE void setup_call_record(size_t max_call_rec_size)
    {
        get_arguments_allocation_if_possible(m_current_call_record, max_call_rec_size);
        m_current_call_record.resize_and_keep_capacity(max_call_rec_size);
        m_call_record_base = m_current_call_record.data();
    }
    ALWAYS_INLINE Vector<Value, ArgumentsStaticSize> take_call_record_vector()
    {
        auto result = move(m_current_call_record);
        m_call_record_base = nullptr;
        return result;
    }
    ALWAYS_INLINE void restore_call_record_vector(Vector<Value, ArgumentsStaticSize>&& vec)
    {
        m_current_call_record = move(vec);
        m_call_record_base = m_current_call_record.data();
    }

    struct CallFrameHandle {
        explicit CallFrameHandle(Configuration& configuration)
            : configuration(configuration)
            , saved_direct_call_depth(configuration.m_compiled_direct_call_depth)
        {
            if (configuration.m_call_record_base)
                moved_call_record = move(configuration.m_current_call_record);
            configuration.m_compiled_direct_call_depth = 0;
            configuration.depth()++;
            configuration.m_call_record_base = nullptr;
        }

        ~CallFrameHandle()
        {
            if (moved_call_record.has_value()) {
                configuration.m_current_call_record = moved_call_record.release_value();
                configuration.m_call_record_base = configuration.m_current_call_record.data();
            } else {
                configuration.m_call_record_base = nullptr;
            }
            configuration.unwind({}, *this);
            configuration.m_compiled_direct_call_depth = saved_direct_call_depth;
        }

        Configuration& configuration;
        Optional<Vector<Value, ArgumentsStaticSize>> moved_call_record;
        size_t saved_direct_call_depth;
    };

    void unwind(Badge<CallFrameHandle>, CallFrameHandle const&) { unwind_impl(); }
    ErrorOr<Optional<HostFunction&>, Trap> prepare_call(FunctionAddress, Vector<Value, ArgumentsStaticSize>& arguments, bool is_tailcall = false);
    ALWAYS_INLINE ErrorOr<void, Trap> prepare_wasm_call(WasmFunction const& wasm_function, Vector<Value, ArgumentsStaticSize>& arguments, bool is_tailcall = false)
    {
        if (is_tailcall)
            unwind_impl();

        arguments.ensure_capacity(arguments.size() + wasm_function.code().func().total_local_count());
        for (auto const& local : wasm_function.code().func().locals()) {
            for (size_t i = 0; i < local.n(); ++i)
                arguments.unchecked_append(Value(local.type()));
        }

        set_frame(
            is_tailcall ? IsTailcall::Yes : IsTailcall::No,
            wasm_function.module(),
            move(arguments),
            wasm_function.code().func().body(),
            wasm_function.type().results().size());
        return {};
    }
    Result call(Interpreter&, FunctionAddress, Vector<Value, ArgumentsStaticSize>& arguments);
    Result execute(Interpreter&);
    ErrorOr<void, Trap> execute_for_compiled_call(Interpreter&, Value* single_result = nullptr);

    void enable_instruction_count_limit() { m_should_limit_instruction_count = true; }
    bool should_limit_instruction_count() const { return m_should_limit_instruction_count; }

    void dump_stack();

    void get_arguments_allocation_if_possible(Vector<Value, ArgumentsStaticSize>& arguments, size_t max_size)
    {
        if (arguments.capacity() != ArgumentsStaticSize || max_size <= ArgumentsStaticSize)
            return; // Already heap allocated, or we just don't need to allocate anything.

        // _arguments_ is still in static storage, pull something from the freelist if it fits.
        if (auto index = m_call_argument_freelist.find_first_index_if([&](auto& entry) { return entry.capacity() >= max_size; }); index.has_value()) {
            arguments = m_call_argument_freelist.take(*index);
            return;
        }

        if (!m_call_argument_freelist.is_empty())
            arguments = m_call_argument_freelist.take_last();

        arguments.ensure_capacity(max(max_size, frame().module().cached_minimum_call_record_allocation_size));
    }

    void release_arguments_allocation(Vector<Value, ArgumentsStaticSize>& arguments, bool expect_frame = true)
    {
        arguments.clear_with_capacity(); // Clear to avoid copying, but keep capacity for reuse.
        auto size = expect_frame ? frame().expression().compiled_instructions.max_call_rec_size : 0;

        if (size > 0) {
            // If we need a call record, keep this as the current one.
            if (!m_call_record_base) {
                m_current_call_record = move(arguments);
                m_current_call_record.resize_and_keep_capacity(size);
                m_call_record_base = m_current_call_record.data();
                return;
            }

            if (m_current_call_record.size() < size)
                m_current_call_record.resize_and_keep_capacity(size);
        }

        if (arguments.capacity() != ArgumentsStaticSize) {
            if (m_call_argument_freelist.size() >= 16) {
                // Don't grow to heap.
                return;
            }

            m_call_argument_freelist.unchecked_append(move(arguments));
        }
    }

    void take_call_record(Vector<Value, ArgumentsStaticSize>& call_record)
    {
        call_record = move(m_current_call_record);
        m_call_record_base = nullptr;
    }

    template<SourceAddressMix mix>
    ALWAYS_INLINE FLATTEN void push_to_destination(Value value, Dispatch::RegisterOrStack destination)
    {
        if constexpr (mix == SourceAddressMix::AllRegisters) {
            regs.data()[to_underlying(destination)] = value;
            return;
        } else if constexpr (mix == SourceAddressMix::AllCallRecords) {
            m_call_record_base[to_underlying(destination) - Dispatch::RegisterOrStack::CallRecord] = value;
            return;
        } else if constexpr (mix == SourceAddressMix::AllStack) {
            value_stack().unchecked_append(value);
            return;
        } else if constexpr (mix == SourceAddressMix::Any) {
            if (!(destination & ~(Dispatch::Stack - 1))) [[likely]] {
                regs.data()[to_underlying(destination)] = value;
                return;
            }
        }

        if constexpr (mix == SourceAddressMix::Any) {
            if (destination == Dispatch::RegisterOrStack::Stack) [[unlikely]] {
                value_stack().unchecked_append(value);
                return;
            }

            m_call_record_base[to_underlying(destination) - Dispatch::RegisterOrStack::CallRecord] = value;
            return;
        }

        VERIFY_NOT_REACHED();
    }

    template<SourceAddressMix mix>
    ALWAYS_INLINE FLATTEN Value& source_value(u8 index, Dispatch::RegisterOrStack const* sources)
    {
        // Note: The last source in a dispatch *must* be equal to the destination for this to be valid.
        auto const source = sources[index];

        if constexpr (mix == SourceAddressMix::AllRegisters) {
            return regs.data()[to_underlying(source)];
        } else if constexpr (mix == SourceAddressMix::AllCallRecords) {
            return m_call_record_base[to_underlying(source) - Dispatch::RegisterOrStack::CallRecord];
        } else if constexpr (mix == SourceAddressMix::AllStack) {
            return value_stack().unsafe_last();
        } else if constexpr (mix == SourceAddressMix::Any) {
            if (!(source & ~(Dispatch::Stack - 1))) [[likely]]
                return regs.data()[to_underlying(source)];
        }

        if constexpr (mix == SourceAddressMix::Any) {
            if (source == Dispatch::RegisterOrStack::Stack) [[unlikely]]
                return value_stack().unsafe_last();

            return m_call_record_base[to_underlying(source) - Dispatch::RegisterOrStack::CallRecord];
        }

        VERIFY_NOT_REACHED();
    }

    template<SourceAddressMix mix>
    ALWAYS_INLINE FLATTEN Value take_source(u8 index, Dispatch::RegisterOrStack const* sources)
    {
        auto const source = sources[index];
        if constexpr (mix == SourceAddressMix::AllRegisters) {
            return regs.data()[to_underlying(source)];
        } else if constexpr (mix == SourceAddressMix::AllCallRecords) {
            return m_call_record_base[to_underlying(source) - Dispatch::RegisterOrStack::CallRecord];
        } else if constexpr (mix == SourceAddressMix::AllStack) {
            return value_stack().unsafe_take_last();
        } else if constexpr (mix == SourceAddressMix::Any) {
            if (!(source & ~(Dispatch::Stack - 1))) [[likely]]
                return regs.data()[to_underlying(source)];
        }

        if constexpr (mix == SourceAddressMix::Any) {
            if (source == Dispatch::RegisterOrStack::Stack) [[unlikely]]
                return value_stack().unsafe_take_last();

            return m_call_record_base[to_underlying(source) - Dispatch::RegisterOrStack::CallRecord];
        }

        VERIFY_NOT_REACHED();
    }

    Array<Value, Dispatch::RegisterOrStack::CountRegisters> regs = {
        Value(0),
        Value(0),
        Value(0),
        Value(0),
        Value(0),
        Value(0),
        Value(0),
        Value(0),
    };

    // Public for CraneliftBridge direct call pop_frame.
    void unwind_impl();

    Store& m_store;
    Vector<Value, 64, FastLastAccess::Yes> m_value_stack;
    Vector<Label, 64, FastLastAccess::Yes> m_label_stack;
    Vector<Frame> m_frame_stack;
    Vector<Value, ArgumentsStaticSize> m_current_call_record;
    Vector<Vector<Value, ArgumentsStaticSize>, 16, FastLastAccess::Yes> m_call_argument_freelist;
    size_t m_depth { 0 };
    u64 m_ip { 0 };
    bool m_should_limit_instruction_count { false };
    Value* m_locals_base { nullptr };
    Value* m_call_record_base { nullptr };
    MemoryInstance* m_default_memory { nullptr };
    u8* m_default_memory_base { nullptr };
    Value m_compiled_call_result_scratch;
};

}
