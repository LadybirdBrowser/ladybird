/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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
        m_frame_stack.append(move(frame));
        m_label_stack.append(label);
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

private:
    Store& m_store;
    Vector<Value> m_value_stack;
    Vector<Label> m_label_stack;
    Vector<Frame> m_frame_stack;
    size_t m_depth { 0 };
    InstructionPointer m_ip;
    bool m_should_limit_instruction_count { false };
};

}
