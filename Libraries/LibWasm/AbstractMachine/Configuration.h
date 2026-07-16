/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DoublyLinkedList.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/ValueStack.h>
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
    AK_MAKE_NONCOPYABLE(Configuration);
    AK_MAKE_NONMOVABLE(Configuration);

public:
    explicit Configuration(Store& store)
        : m_store(store)
    {
        m_store.register_configuration({}, *this);
    }

    ~Configuration()
    {
        m_store.unregister_configuration({}, *this);
    }

    void set_frame(IsTailcall is_tailcall, ModuleInstance const& module, Vector<Value, ArgumentsStaticSize> locals, Expression const& expression, size_t arity)
    {
        m_owned_locals_stack.append(move(locals));
        auto* locals_ptr = m_owned_locals_stack.last().data();
        m_frame_stack.empend(module, locals_ptr, expression, arity, /* owns_locals = */ true);

        auto& frame = m_frame_stack.last();
        m_locals_base = locals_ptr;
        auto const& memories = frame.module().memories();
        m_default_memory = memories.is_empty() ? nullptr : m_store.unsafe_get(memories[0]);
        frame.set_compiled_fn_table(&frame.module().compiled_fn_table(m_store));

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

        // A tail call replaces the current frame, so release its record first; otherwise a
        // tail-call loop would grow the record region without bound.
        if (is_tailcall == IsTailcall::Yes && m_call_record_base)
            m_call_record_stack.release_to(m_call_record_base);
        auto max_call_rec_size = frame.expression().compiled_instructions.max_call_rec_size;
        if (max_call_rec_size > 0)
            m_call_record_base = m_call_record_stack.allocate(max_call_rec_size);
        else
            m_call_record_base = nullptr;
    }
    // Lightweight set_frame for direct Cranelift-to-Cranelift calls.
    void set_frame_lightweight(ModuleInstance const& module, Value* locals_ptr,
        Expression const& expression, size_t arity)
    {
        VERIFY(bit_cast<FlatPtr>(&module) != 0);

        // For the (common) same-module call case, we already have the compiled-fn-table pointer cached on the caller's frame, so reuse it instead of re-resolving through the store on every call.
        bool const same_module = !m_frame_stack.is_empty() && &m_frame_stack.last().module() == &module;
        auto const* table = same_module ? m_frame_stack.last().compiled_fn_table() : &module.compiled_fn_table(m_store);
        m_frame_stack.empend(module, locals_ptr, expression, arity);
        m_frame_stack.last().set_compiled_fn_table(table);
        m_locals_base = locals_ptr;
        if (!same_module) {
            auto const& memories = module.memories();
            m_default_memory = memories.is_empty() ? nullptr : m_store.unsafe_get(memories[0]);
        }
        // Compiled code pushes to the value stack without bounds checks, so verify here that the
        // frame's whole stack usage fits the reservation.
        if (auto hint = expression.stack_usage_hint(); hint.has_value())
            m_value_stack.ensure_capacity(m_value_stack.size() + *hint);
        // Skip the label push (Cranelift uses its own structured control flow).
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
    ALWAYS_INLINE Value& compiled_call_result_scratch() { return m_compiled_call_result_scratch; }
    ALWAYS_INLINE Value const& compiled_call_result_scratch() const { return m_compiled_call_result_scratch; }

    ALWAYS_INLINE Value const& local(LocalIndex index) const { return m_locals_base[index.value()]; }
    ALWAYS_INLINE Value& local(LocalIndex index) { return m_locals_base[index.value()]; }
    ALWAYS_INLINE Value* locals_base() const { return m_locals_base; }
    ALWAYS_INLINE void set_locals_base(Value* base) { m_locals_base = base; }

    static constexpr size_t locals_base_offset() { return __builtin_offsetof(Configuration, m_locals_base); }
    static constexpr size_t default_memory_offset() { return __builtin_offsetof(Configuration, m_default_memory); }
    static constexpr size_t compiled_call_result_scratch_offset() { return __builtin_offsetof(Configuration, m_compiled_call_result_scratch); }
    static constexpr size_t value_stack_base_offset() { return __builtin_offsetof(Configuration, m_value_stack) + ValueStack::base_offset(); }
    static constexpr size_t value_stack_top_offset() { return __builtin_offsetof(Configuration, m_value_stack) + ValueStack::top_offset(); }

    ALWAYS_INLINE Value& call_record_entry(size_t index) { return m_call_record_base[index]; }
    ALWAYS_INLINE Value const& call_record_entry(size_t index) const { return m_call_record_base[index]; }
    ALWAYS_INLINE Value* call_record_base() const { return m_call_record_base; }
    ALWAYS_INLINE void set_call_record_base(Value* base) { m_call_record_base = base; }
    ALWAYS_INLINE void setup_call_record(size_t max_call_rec_size)
    {
        m_call_record_base = m_call_record_stack.allocate(max_call_rec_size);
    }

    struct CallFrameHandle {
        explicit CallFrameHandle(Configuration& configuration)
            : configuration(configuration)
            , saved_call_record_base(configuration.m_call_record_base)
            , saved_call_record_mark(configuration.m_call_record_stack.mark())
        {
            configuration.depth()++;
            configuration.m_call_record_base = nullptr;
        }

        ~CallFrameHandle()
        {
            configuration.m_call_record_stack.release_to(saved_call_record_mark);
            configuration.m_call_record_base = saved_call_record_base;
            configuration.unwind({}, *this);
        }

        Configuration& configuration;
        Value* saved_call_record_base;
        Value* saved_call_record_mark;
    };

    void unwind(Badge<CallFrameHandle>, CallFrameHandle const&) { unwind_impl(); }
    ErrorOr<Optional<HostFunction&>, Trap> prepare_call(FunctionAddress, Vector<Value, ArgumentsStaticSize>& arguments, bool is_tailcall = false);
    ErrorOr<void, Trap> prepare_wasm_call(WasmFunction const& wasm_function, Vector<Value, ArgumentsStaticSize>& arguments, bool is_tailcall = false);
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

    void release_arguments_allocation(Vector<Value, ArgumentsStaticSize>& arguments)
    {
        arguments.clear_with_capacity(); // Clear to avoid copying, but keep capacity for reuse.

        if (arguments.capacity() != ArgumentsStaticSize) {
            if (m_call_argument_freelist.size() >= 16) {
                // Don't grow to heap.
                return;
            }

            m_call_argument_freelist.unchecked_append(move(arguments));
        }
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
    ValueStack m_value_stack;
    ValueStack m_call_record_stack;
    Vector<Label, 64, FastLastAccess::Yes> m_label_stack;
    Vector<Frame> m_frame_stack;
    Vector<Vector<Value, ArgumentsStaticSize>> m_owned_locals_stack;
    Vector<Vector<Value, ArgumentsStaticSize>, 16, FastLastAccess::Yes> m_call_argument_freelist;
    size_t m_depth { 0 };
    u64 m_ip { 0 };
    bool m_should_limit_instruction_count { false };
    Value* m_locals_base { nullptr };
    Value* m_call_record_base { nullptr };
    MemoryInstance* m_default_memory { nullptr };
    Value m_compiled_call_result_scratch;
};

}
