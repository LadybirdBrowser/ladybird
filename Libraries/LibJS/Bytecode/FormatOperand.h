/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Operand.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Bytecode {

inline ByteString format_operand(StringView name, Operand encoded_operand, Bytecode::Executable const& executable)
{
    StringBuilder builder;
    if (!name.is_empty())
        builder.appendff("\033[32m{}\033[0m:", name);
    auto operand = executable.original_operand_from_raw(encoded_operand.raw());
    switch (operand.type()) {
    case Operand::Type::Register:
        if (operand.index() == Register::this_value().index()) {
            builder.appendff("\033[33mthis\033[0m");
        } else {
            builder.appendff("\033[33mreg{}\033[0m", operand.index());
        }
        break;
    case Operand::Type::Local:
        builder.appendff("\033[34m{}~{}\033[0m", executable.local_variable_names[operand.index()].name, operand.index());
        break;
    case Operand::Type::Argument:
        builder.appendff("\033[34marg{}\033[0m", operand.index());
        break;
    case Operand::Type::Constant: {
        builder.append("\033[36m"sv);
        auto value = executable.constants[operand.index()];
        if (value.is_special_empty_value())
            builder.append("<Empty>"sv);
        else if (value.is_boolean())
            builder.appendff("Bool({})", value.as_bool() ? "true"sv : "false"sv);
        else if (value.is_int32())
            builder.appendff("Int32({})", value.as_i32());
        else if (value.is_double())
            builder.appendff("Double({})", value.as_double());
        else if (value.is_bigint())
            builder.appendff("BigInt({})", MUST(value.as_bigint().to_string()));
        else if (value.is_string())
            builder.appendff("String(\"{}\")", value.as_string().utf8_string_view());
        else if (value.is_undefined())
            builder.append("Undefined"sv);
        else if (value.is_null())
            builder.append("Null"sv);
        else
            builder.appendff("Value: {}", value);
        builder.append("\033[0m"sv);
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }
    return builder.to_byte_string();
}

inline ByteString format_operand_list(StringView name, ReadonlySpan<Operand> operands, Bytecode::Executable const& executable)
{
    StringBuilder builder;
    if (!name.is_empty())
        builder.appendff("\033[32m{}\033[0m:[", name);
    for (size_t i = 0; i < operands.size(); ++i) {
        if (i != 0)
            builder.append(", "sv);
        builder.appendff("{}", format_operand(""sv, operands[i], executable));
    }
    builder.append("]"sv);
    return builder.to_byte_string();
}

inline ByteString format_value_list(StringView name, ReadonlySpan<Value> values)
{
    StringBuilder builder;
    if (!name.is_empty())
        builder.appendff("\033[32m{}\033[0m:[", name);
    builder.join(", "sv, values);
    builder.append("]"sv);
    return builder.to_byte_string();
}

}
