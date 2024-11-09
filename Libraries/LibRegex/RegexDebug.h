/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringBuilder.h>
#include <LibRegex/RegexMatcher.h>

namespace regex {

class RegexDebug {
public:
    RegexDebug(FILE* file = stdout)
        : m_file(file)
    {
    }

    virtual ~RegexDebug() = default;

    template<typename T>
    void print_raw_bytecode(Regex<T>& regex) const
    {
        auto& bytecode = regex.parser_result.bytecode;
        size_t index { 0 };
        for (auto& value : bytecode) {
            outln(m_file, "OpCode i={:3} [{:#02X}]", index, (u32)value);
            ++index;
        }
    }

    template<typename T>
    void print_bytecode(Regex<T> const& regex) const
    {
        print_bytecode(regex.parser_result.bytecode);
    }

    void print_bytecode(ByteCode const& bytecode) const
    {
        MatchState state;
        for (;;) {
            auto& opcode = bytecode.get_opcode(state);
            print_opcode("PrintBytecode", opcode, state);
            out(m_file, "{}", m_debug_stripline);

            if (is<OpCode_Exit>(opcode))
                break;

            state.instruction_position += opcode.size();
        }

        fflush(m_file);
    }

    void print_opcode(ByteString const& system, OpCode& opcode, MatchState& state, size_t recursion = 0, bool newline = true) const
    {
        out(m_file, "{:15} | {:5} | {:9} | {:35} | {:30} | {:20}",
            system.characters(),
            state.instruction_position,
            recursion,
            opcode.to_byte_string().characters(),
            opcode.arguments_string().characters(),
            ByteString::formatted("ip: {:3},   sp: {:3}", state.instruction_position, state.string_position));
        if (newline)
            outln();
        if (newline && is<OpCode_Compare>(opcode)) {
            for (auto& line : to<OpCode_Compare>(opcode).variable_arguments_to_byte_string())
                outln(m_file, "{:15} | {:5} | {:9} | {:35} | {:30} | {:20}", "", "", "", "", line, "");
        }
    }

    void print_result(OpCode const& opcode, ByteCode const& bytecode, MatchInput const& input, MatchState& state, ExecutionResult result) const
    {
        StringBuilder builder;
        builder.append(execution_result_name(result));
        builder.appendff(", fc: {}, ss: {}", input.fail_counter, input.saved_positions.size());
        if (result == ExecutionResult::Succeeded) {
            builder.appendff(", ip: {}/{}, sp: {}/{}", state.instruction_position, bytecode.size() - 1, state.string_position, input.view.length() - 1);
        } else if (result == ExecutionResult::Fork_PrioHigh) {
            builder.appendff(", next ip: {}", state.fork_at_position + opcode.size());
        } else if (result != ExecutionResult::Failed) {
            builder.appendff(", next ip: {}", state.instruction_position + opcode.size());
        }

        outln(m_file, " | {:20}", builder.to_byte_string());

        if (is<OpCode_Compare>(opcode)) {
            for (auto& line : to<OpCode_Compare>(opcode).variable_arguments_to_byte_string(input)) {
                outln(m_file, "{:15} | {:5} | {:9} | {:35} | {:30} | {:20}", "", "", "", "", line, "");
            }
        }

        out(m_file, "{}", m_debug_stripline);
    }

    void print_header()
    {
        StringBuilder builder;
        builder.appendff("{:15} | {:5} | {:9} | {:35} | {:30} | {:20} | {:20}\n", "System", "Index", "Recursion", "OpCode", "Arguments", "State", "Result");
        auto length = builder.length();
        for (size_t i = 0; i < length; ++i) {
            builder.append('=');
        }
        auto str = builder.to_byte_string();
        VERIFY(!str.is_empty());

        outln(m_file, "{}", str);
        fflush(m_file);

        builder.clear();
        for (size_t i = 0; i < length; ++i) {
            builder.append('-');
        }
        builder.append('\n');
        m_debug_stripline = builder.to_byte_string();
    }

private:
    ByteString m_debug_stripline;
    FILE* m_file;
};

}

using regex::RegexDebug;
