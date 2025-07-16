/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DoublyLinkedList.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>

namespace Wasm {

class Configuration {
public:
    explicit Configuration(Store& store)
        : m_store(store)
    {
    }

    void set_frame(Frame frame)
    {
        Label label(frame.arity(), frame.expression().instructions().size(), m_value_stack.size());
        frame.label_index() = m_label_stack.size();
        if (auto hint = frame.expression().stack_usage_hint(); hint.has_value())
            m_value_stack.ensure_capacity(*hint + m_value_stack.size());
        m_frame_stack.append(move(frame));
        m_label_stack.append(label);
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

    struct CallFrameHandle {
        explicit CallFrameHandle(Configuration& configuration)
            : ip(configuration.ip())
            , configuration(configuration)
        {
            configuration.depth()++;
        }

        ~CallFrameHandle()
        {
            configuration.unwind({}, *this);
        }

        InstructionPointer ip { 0 };
        Configuration& configuration;
    };

    void unwind(Badge<CallFrameHandle>, CallFrameHandle const&);
    Result call(Interpreter&, FunctionAddress, Vector<Value> arguments);
    Result execute(Interpreter&);

    void enable_instruction_count_limit() { m_should_limit_instruction_count = true; }
    bool should_limit_instruction_count() const { return m_should_limit_instruction_count; }

    void dump_stack();

    ALWAYS_INLINE FLATTEN void push_to_destination(Value value)
    {
        // dbgln("push to {}", destination == Dispatch::RegisterOrStack::Stack ? "stack" : ByteString::formatted("reg{}", to_underlying(destination)));
        if (destination == Dispatch::RegisterOrStack::Stack) {
            value_stack().unchecked_append(value);
            return;
        }
        regs[to_underlying(destination)] = value;
    }

    // Requirements:
    // - The last source in a dispatch *must* be equal to the destination.
    ALWAYS_INLINE FLATTEN Value& source_value(u8 index)
    {
        auto const source = sources[index];
        // dbgln("peek arg{} = {}", index, source == Dispatch::RegisterOrStack::Stack ? "stack" : ByteString::formatted("reg{}", to_underlying(source)));
        if (source == Dispatch::RegisterOrStack::Stack)
            return value_stack().unsafe_last();
        return regs[to_underlying(source)];
    }

    ALWAYS_INLINE FLATTEN Value take_source(u8 index)
    {
        auto const source = sources[index];
        // dbgln("take arg{} = {}", index, source == Dispatch::RegisterOrStack::Stack ? "stack" : ByteString::formatted("reg{}", to_underlying(source)));
        if (source == Dispatch::RegisterOrStack::Stack)
            return value_stack().unsafe_take_last();
        return regs[to_underlying(source)];
    }

    void spill_into_values()
    {
        register_liveness_record.in_live_order([&](auto index) {
            value_stack().unchecked_append(regs[index]);
        });
        register_liveness_record.is_live = 0;
    }

    Dispatch::RegisterOrStack sources[3] = { Dispatch::RegisterOrStack::Stack, Dispatch::RegisterOrStack::Stack, Dispatch::RegisterOrStack::Stack };
    Dispatch::RegisterOrStack destination = Dispatch::RegisterOrStack::Stack;
    Value regs[3] = { Value(0), Value(0), Value(0) };
    Dispatch::LivenessRecord register_liveness_record { Dispatch::LivenessOrder::_012, 0 };

private:
    Store& m_store;
    Vector<Value, 512> m_value_stack;
    DoublyLinkedList<Label, 512> m_label_stack;
    DoublyLinkedList<Frame, 512> m_frame_stack;
    size_t m_depth { 0 };
    u64 m_ip { 0 };
    bool m_should_limit_instruction_count { false };
};

}
