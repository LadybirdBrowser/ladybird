/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringBuilder.h>
#include <LibRegex/RegexIR.h>
#include <LibRegex/RegexMatcher.h>

namespace regex {

inline void print_ir(RegexIR const& ir, FILE* file = stderr)
{
    for (size_t i = 0; i < ir.insts.size(); ++i) {
        auto const& inst = ir.insts[i];

        switch (inst.op) {
        case IROp::Label:
            outln(file, "L{}:", inst.target);
            continue;
        case IROp::Nop:
            outln(file, "  {:4}  Nop", i);
            continue;
        default:
            break;
        }

        StringBuilder args;

        switch (inst.op) {
        case IROp::Jump:
        case IROp::ForkJump:
        case IROp::ForkStay:
        case IROp::ForkReplaceJump:
        case IROp::ForkReplaceStay:
        case IROp::ForkIf:
        case IROp::JumpNonEmpty:
        case IROp::Repeat:
            args.appendff("to L{}", inst.target);
            break;
        default:
            break;
        }

        switch (inst.op) {
        case IROp::Repeat:
            args.appendff(", count={}, id={}", inst.arg0, inst.arg1);
            break;
        case IROp::JumpNonEmpty:
            args.appendff(", checkpoint={}, form={}", inst.arg0, inst.arg1);
            break;
        case IROp::ForkIf:
            args.appendff(", form={}, cond={}", inst.arg0, inst.arg1);
            break;
        case IROp::GoBack:
        case IROp::SaveLeftCapture:
        case IROp::SaveRightCapture:
        case IROp::ClearCaptureGroup:
        case IROp::FailIfEmpty:
        case IROp::ResetRepeat:
        case IROp::Checkpoint:
        case IROp::CheckBoundary:
        case IROp::RSeekTo:
        case IROp::SaveModifiers:
            args.appendff("{}", inst.arg0);
            break;
        case IROp::SaveRightNamedCapture:
            args.appendff("group={}", inst.arg0);
            break;
        case IROp::SetStepBack:
            if (inst.compare_size > 0)
                args.appendff("{}", static_cast<i64>(ir.compare_data[inst.compare_start]));
            break;
        default:
            break;
        }

        if (inst.op == IROp::Compare && inst.compare_size > 0) {
            auto pairs = ir_flat_compares(
                ir.compare_data.span().slice(inst.compare_start, inst.compare_size),
                inst.arg0);
            args.appendff("argc={} [", inst.arg0);
            bool first = true;
            for (auto const& pair : pairs) {
                if (!first)
                    args.append(", "sv);
                first = false;
                args.append(character_compare_type_name(pair.type));
                switch (pair.type) {
                case CharacterCompareType::Char:
                    if (pair.value >= 0x20 && pair.value <= 0x7e)
                        args.appendff(" '{:c}'", static_cast<char>(pair.value));
                    else
                        args.appendff(" u+{:04x}", static_cast<u32>(pair.value));
                    break;
                case CharacterCompareType::CharClass:
                    args.appendff(" {}", character_class_name(static_cast<CharClass>(pair.value)));
                    break;
                case CharacterCompareType::CharRange: {
                    u32 from = pair.value >> 32;
                    u32 to = pair.value & 0xffffffff;
                    if (from >= 0x20 && from <= 0x7e && to >= 0x20 && to <= 0x7e)
                        args.appendff(" '{:c}'-'{:c}'", static_cast<char>(from), static_cast<char>(to));
                    else
                        args.appendff(" u+{:04x}-u+{:04x}", from, to);
                    break;
                }
                case CharacterCompareType::String:
                    args.appendff(" #{}", static_cast<u32>(pair.value));
                    break;
                case CharacterCompareType::Reference:
                case CharacterCompareType::NamedReference:
                    args.appendff(" \\{}", static_cast<u32>(pair.value));
                    break;
                default:
                    if (pair.value != 0)
                        args.appendff(" {}", static_cast<u32>(pair.value));
                    break;
                }
            }
            args.append(']');
        }

        outln(file, "  {:4}  {:25} {}", i, irop_name(inst.op), args.string_view());
    }
    fflush(file);
}

template<typename ByteCode>
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
        auto& bytecode = regex.parser_result.bytecode.template get<ByteCode>();
        size_t index { 0 };
        for (auto& value : bytecode) {
            outln(m_file, "OpCode i={:3} [{:#02X}]", index, value);
            ++index;
        }
    }

    template<typename T>
    void print_bytecode(Regex<T> const& regex) const
    {
        print_bytecode(regex.parser_result.bytecode.template get<ByteCode>());
    }

    void print_bytecode(ByteCode const& bytecode) const
    {
        auto state = MatchState::only_for_enumeration();
        for (;;) {
            auto& opcode = bytecode.get_opcode(state);
            print_opcode("PrintBytecode", opcode, state);
            out(m_file, "{}", m_debug_stripline);

            if (is<OpCode_Exit>(opcode))
                break;

            state.instruction_position += opcode.size();
        }

        out(m_file, "String Table:\n");
        for (auto const& entry : bytecode.string_table().m_table)
            outln(m_file, "+ {} -> {:x}", entry.key, entry.value);
        out(m_file, "Reverse String Table:\n");
        for (auto const& entry : bytecode.string_table().m_inverse_table)
            outln(m_file, "+ {:x} -> {}", entry.key, entry.value);

        out(m_file, "(u16) String Table:\n");
        for (auto const& entry : bytecode.u16_string_table().m_table)
            outln(m_file, "+ {} -> {:x}", entry.key, entry.value);
        out(m_file, "Reverse (u16) String Table:\n");
        for (auto const& entry : bytecode.u16_string_table().m_inverse_table)
            outln(m_file, "+ {:x} -> {}", entry.key, entry.value);

        fflush(m_file);
    }

    void print_opcode(ByteString const& system, OpCode<ByteCode>& opcode, MatchState& state, size_t recursion = 0, bool newline = true) const
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

    void print_result(OpCode<ByteCode> const& opcode, ByteCode const& bytecode, MatchInput const& input, MatchState& state, ExecutionResult result) const
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

        if (is<OpCode_CheckSavedPosition>(opcode)) {
            auto last_saved = input.saved_positions.is_empty()
                ? "saved: <empty>"_string
                : MUST(String::formatted("saved: {}", input.saved_positions.last()));
            outln(m_file, "{:15} | {:5} | {:9} | {:35} | {:30} | {:20}", "", "", "", "", last_saved, "");
        }
        if (is<OpCode_CheckStepBack>(opcode) || is<OpCode_IncStepBack>(opcode)) {
            auto last_step_back = state.step_backs.is_empty()
                ? "step: <empty>"_string
                : MUST(String::formatted("step: {}", state.step_backs.last()));
            outln(m_file, "{:15} | {:5} | {:9} | {:35} | {:30} | {:20}", "", "", "", "", last_step_back, "");
        }

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
