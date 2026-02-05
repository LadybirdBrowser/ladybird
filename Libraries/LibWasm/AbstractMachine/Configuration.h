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
        m_frame_stack.append(forward<Args>(frame_init)...);

        auto& frame = m_frame_stack.unchecked_last();
        m_locals_base = frame.locals().data();

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
    ALWAYS_INLINE auto& frame() const { return m_frame_stack.unchecked_last(); }
    ALWAYS_INLINE auto& frame() { return m_frame_stack.unchecked_last(); }
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

    ALWAYS_INLINE Value const& local(LocalIndex index) const { return m_locals_base[index.value()]; }
    ALWAYS_INLINE Value& local(LocalIndex index) { return m_locals_base[index.value()]; }

    struct CallFrameHandle {
        explicit CallFrameHandle(Configuration& configuration)
            : configuration(configuration)
        {
            if (configuration.m_call_record_base)
                moved_call_record = move(configuration.m_current_call_record);
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
        }

        Configuration& configuration;
        Optional<Vector<Value, ArgumentsStaticSize>> moved_call_record;
    };

    void unwind(Badge<CallFrameHandle>, CallFrameHandle const&) { unwind_impl(); }
    ErrorOr<Optional<HostFunction&>, Trap> prepare_call(FunctionAddress, Vector<Value, ArgumentsStaticSize>& arguments, bool is_tailcall = false);
    Result call(Interpreter&, FunctionAddress, Vector<Value, ArgumentsStaticSize>& arguments);
    Result execute(Interpreter&);

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

            VERIFY(m_current_call_record.size() >= size);
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

private:
    void unwind_impl();

    Store& m_store;
    Vector<Value, 64, FastLastAccess::Yes> m_value_stack;
    Vector<Label, 64, FastLastAccess::Yes> m_label_stack;
    DoublyLinkedList<Frame, 512> m_frame_stack;
    Vector<Value, ArgumentsStaticSize> m_current_call_record;
    Vector<Vector<Value, ArgumentsStaticSize>, 16, FastLastAccess::Yes> m_call_argument_freelist;
    size_t m_depth { 0 };
    u64 m_ip { 0 };
    bool m_should_limit_instruction_count { false };
    Value* m_locals_base { nullptr };
    Value* m_call_record_base { nullptr };
};

}
