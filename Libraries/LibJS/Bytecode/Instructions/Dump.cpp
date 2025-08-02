/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/AST.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>

namespace JS::Bytecode {

ByteString Instruction::to_byte_string(Bytecode::Executable const& executable) const
{
#define __BYTECODE_OP(op)       \
    case Instruction::Type::op: \
        return static_cast<Bytecode::Op::op const&>(*this).to_byte_string_impl(executable);

    switch (type()) {
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
    default:
        VERIFY_NOT_REACHED();
    }

#undef __BYTECODE_OP
}

}

namespace JS::Bytecode::Op {

static ByteString format_operand(StringView name, Operand operand, Bytecode::Executable const& executable)
{
    StringBuilder builder;
    if (!name.is_empty())
        builder.appendff("\033[32m{}\033[0m:", name);
    switch (operand.type()) {
    case Operand::Type::Register:
        if (operand.index() == Register::this_value().index()) {
            builder.appendff("\033[33mthis\033[0m");
        } else {
            builder.appendff("\033[33mreg{}\033[0m", operand.index());
        }
        break;
    case Operand::Type::Local:
        builder.appendff("\033[34m{}~{}\033[0m", executable.local_variable_names[operand.index() - executable.local_index_base].name, operand.index() - executable.local_index_base);
        break;
    case Operand::Type::Argument:
        builder.appendff("\033[34marg{}\033[0m", operand.index() - executable.argument_index_base);
        break;
    case Operand::Type::Constant: {
        builder.append("\033[36m"sv);
        auto value = executable.constants[operand.index() - executable.number_of_registers];
        if (value.is_special_empty_value())
            builder.append("<Empty>"sv);
        else if (value.is_boolean())
            builder.appendff("Bool({})", value.as_bool() ? "true"sv : "false"sv);
        else if (value.is_int32())
            builder.appendff("Int32({})", value.as_i32());
        else if (value.is_double())
            builder.appendff("Double({})", value.as_double());
        else if (value.is_bigint())
            builder.appendff("BigInt({})", value.as_bigint().to_byte_string());
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

static ByteString format_operand_list(StringView name, ReadonlySpan<Operand> operands, Bytecode::Executable const& executable)
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

static ByteString format_value_list(StringView name, ReadonlySpan<Value> values)
{
    StringBuilder builder;
    if (!name.is_empty())
        builder.appendff("\033[32m{}\033[0m:[", name);
    builder.join(", "sv, values);
    builder.append("]"sv);
    return builder.to_byte_string();
}

#define JS_DEFINE_TO_BYTE_STRING_FOR_COMMON_BINARY_OP(OpTitleCase, op_snake_case)             \
    ByteString OpTitleCase::to_byte_string_impl(Bytecode::Executable const& executable) const \
    {                                                                                         \
        return ByteString::formatted(#OpTitleCase " {}, {}, {}",                              \
            format_operand("dst"sv, m_dst, executable),                                       \
            format_operand("lhs"sv, m_lhs, executable),                                       \
            format_operand("rhs"sv, m_rhs, executable));                                      \
    }
JS_ENUMERATE_COMMON_BINARY_OPS_WITHOUT_FAST_PATH(JS_DEFINE_TO_BYTE_STRING_FOR_COMMON_BINARY_OP)
JS_ENUMERATE_COMMON_BINARY_OPS_WITH_FAST_PATH(JS_DEFINE_TO_BYTE_STRING_FOR_COMMON_BINARY_OP)

#define JS_DEFINE_COMMON_UNARY_OP(OpTitleCase, op_snake_case)                                 \
    ByteString OpTitleCase::to_byte_string_impl(Bytecode::Executable const& executable) const \
    {                                                                                         \
        return ByteString::formatted(#OpTitleCase " {}, {}",                                  \
            format_operand("dst"sv, dst(), executable),                                       \
            format_operand("src"sv, src(), executable));                                      \
    }
JS_ENUMERATE_COMMON_UNARY_OPS(JS_DEFINE_COMMON_UNARY_OP)

#define JS_DEFINE_NEW_BUILTIN_ERROR_OP(ErrorName)                                                \
    ByteString New##ErrorName::to_byte_string_impl(Bytecode::Executable const& executable) const \
    {                                                                                            \
        return ByteString::formatted("New" #ErrorName " {}, {}",                                 \
            format_operand("dst"sv, m_dst, executable),                                          \
            executable.string_table->get(m_error_string));                                       \
    }
JS_ENUMERATE_NEW_BUILTIN_ERROR_OPS(JS_DEFINE_NEW_BUILTIN_ERROR_OP)

ByteString Mov::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Mov {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("src"sv, m_src, executable));
}

ByteString NewArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("NewArray {}", format_operand("dst"sv, dst(), executable));
    if (m_element_count != 0) {
        builder.appendff(", {}", format_operand_list("args"sv, { m_elements, m_element_count }, executable));
    }
    return builder.to_byte_string();
}

ByteString NewPrimitiveArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("NewPrimitiveArray {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_value_list("elements"sv, elements()));
}

ByteString AddPrivateName::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("AddPrivateName {}", executable.identifier_table->get(m_name));
}

ByteString ArrayAppend::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Append {}, {}{}",
        format_operand("dst"sv, dst(), executable),
        format_operand("src"sv, src(), executable),
        m_is_spread ? " **"sv : ""sv);
}

ByteString IteratorToArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("IteratorToArray {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("iterator"sv, iterator(), executable));
}

ByteString NewObject::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("NewObject {}", format_operand("dst"sv, dst(), executable));
}

ByteString NewRegExp::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("NewRegExp {}, source:\"{}\" flags:\"{}\"",
        format_operand("dst"sv, dst(), executable),
        executable.get_string(m_source_index),
        executable.get_string(m_flags_index));
}

ByteString CopyObjectExcludingProperties::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CopyObjectExcludingProperties {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("from"sv, m_from_object, executable));
    if (m_excluded_names_count != 0) {
        builder.append(" excluding:["sv);
        for (size_t i = 0; i < m_excluded_names_count; ++i) {
            if (i != 0)
                builder.append(", "sv);
            builder.append(format_operand("#"sv, m_excluded_names[i], executable));
        }
        builder.append(']');
    }
    return builder.to_byte_string();
}

ByteString ConcatString::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ConcatString {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("src"sv, src(), executable));
}

ByteString GetCalleeAndThisFromEnvironment::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetCalleeAndThisFromEnvironment {}, {} <- {}",
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable),
        executable.identifier_table->get(m_identifier));
}

ByteString GetBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetBinding {}, {}",
        format_operand("dst"sv, dst(), executable),
        executable.identifier_table->get(m_identifier));
}

ByteString GetInitializedBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetInitializedBinding {}, {}",
        format_operand("dst"sv, dst(), executable),
        executable.identifier_table->get(m_identifier));
}

ByteString GetGlobal::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetGlobal {}, {}", format_operand("dst"sv, dst(), executable),
        executable.identifier_table->get(m_identifier));
}

ByteString SetGlobal::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SetGlobal {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

ByteString DeleteVariable::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteVariable {}", executable.identifier_table->get(m_identifier));
}

ByteString CreateLexicalEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "CreateLexicalEnvironment"sv;
}

ByteString CreatePrivateEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "CreatePrivateEnvironment"sv;
}

ByteString CreateVariableEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "CreateVariableEnvironment"sv;
}

ByteString CreateVariable::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto mode_string = m_mode == EnvironmentMode::Lexical ? "Lexical" : "Variable";
    return ByteString::formatted("CreateVariable env:{} immutable:{} global:{} {}", mode_string, m_is_immutable, m_is_global, executable.identifier_table->get(m_identifier));
}

ByteString CreateRestParams::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("CreateRestParams {}, rest_index:{}", format_operand("dst"sv, m_dst, executable), m_rest_index);
}

ByteString CreateArguments::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CreateArguments");
    if (m_dst.has_value())
        builder.appendff(" {}", format_operand("dst"sv, *m_dst, executable));
    builder.appendff(" {} immutable:{}", m_kind == Kind::Mapped ? "mapped"sv : "unmapped"sv, m_is_immutable);
    return builder.to_byte_string();
}

ByteString EnterObjectEnvironment::to_byte_string_impl(Executable const& executable) const
{
    return ByteString::formatted("EnterObjectEnvironment {}",
        format_operand("object"sv, m_object, executable));
}

ByteString InitializeLexicalBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("InitializeLexicalBinding {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

ByteString InitializeVariableBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("InitializeVariableBinding {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

ByteString SetLexicalBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SetLexicalBinding {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

ByteString SetVariableBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SetVariableBinding {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

static StringView property_kind_to_string(PropertyKind kind)
{
    switch (kind) {
    case PropertyKind::Getter:
        return "getter"sv;
    case PropertyKind::Setter:
        return "setter"sv;
    case PropertyKind::KeyValue:
        return "key-value"sv;
    case PropertyKind::DirectKeyValue:
        return "direct-key-value"sv;
    case PropertyKind::ProtoSetter:
        return "proto-setter"sv;
    }
    VERIFY_NOT_REACHED();
}

ByteString PutBySpread::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("PutBySpread {}, {}",
        format_operand("base"sv, m_base, executable),
        format_operand("src"sv, m_src, executable));
}

ByteString PutById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted("PutById {}, {}, {}, kind:{}",
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("src"sv, m_src, executable),
        kind);
}

ByteString PutByIdWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted("PutByIdWithThis {}, {}, {}, {}, kind:{}",
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("src"sv, m_src, executable),
        format_operand("this"sv, m_this_value, executable),
        kind);
}

ByteString PutPrivateById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted(
        "PutPrivateById {}, {}, {}, kind:{} ",
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("src"sv, m_src, executable),
        kind);
}

ByteString GetById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetById {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property));
}

ByteString GetByIdWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetByIdWithThis {}, {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("this"sv, m_this_value, executable));
}

ByteString GetLength::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetLength {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable));
}

ByteString GetLengthWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetLengthWithThis {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        format_operand("this"sv, m_this_value, executable));
}

ByteString GetPrivateById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetPrivateById {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property));
}

ByteString HasPrivateId::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("HasPrivateId {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property));
}

ByteString DeleteById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteById {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property));
}

ByteString DeleteByIdWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteByIdWithThis {}, {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("this"sv, m_this_value, executable));
}

ByteString Jump::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("Jump {}", m_target);
}

ByteString JumpIf::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpIf {}, \033[32mtrue\033[0m:{} \033[32mfalse\033[0m:{}",
        format_operand("condition"sv, m_condition, executable),
        m_true_target,
        m_false_target);
}

ByteString JumpTrue::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpTrue {}, {}",
        format_operand("condition"sv, m_condition, executable),
        m_target);
}

ByteString JumpFalse::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpFalse {}, {}",
        format_operand("condition"sv, m_condition, executable),
        m_target);
}

ByteString JumpNullish::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpNullish {}, null:{} nonnull:{}",
        format_operand("condition"sv, m_condition, executable),
        m_true_target,
        m_false_target);
}

#define HANDLE_COMPARISON_OP(op_TitleCase, op_snake_case, numeric_operator)                          \
    ByteString Jump##op_TitleCase::to_byte_string_impl(Bytecode::Executable const& executable) const \
    {                                                                                                \
        return ByteString::formatted("Jump" #op_TitleCase " {}, {}, true:{}, false:{}",              \
            format_operand("lhs"sv, m_lhs, executable),                                              \
            format_operand("rhs"sv, m_rhs, executable),                                              \
            m_true_target,                                                                           \
            m_false_target);                                                                         \
    }

JS_ENUMERATE_COMPARISON_OPS(HANDLE_COMPARISON_OP)

ByteString JumpUndefined::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpUndefined {}, undefined:{} defined:{}",
        format_operand("condition"sv, m_condition, executable),
        m_true_target,
        m_false_target);
}

static StringView call_type_to_string(CallType type)
{
    switch (type) {
    case CallType::Call:
        return ""sv;
    case CallType::Construct:
        return " (Construct)"sv;
    case CallType::DirectEval:
        return " (DirectEval)"sv;
    }
    VERIFY_NOT_REACHED();
}

ByteString Call::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("Call {}, {}, {}, ",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable));

    builder.append(format_operand_list("args"sv, { m_arguments, m_argument_count }, executable));

    if (m_expression_string.has_value()) {
        builder.appendff(", `{}`", executable.get_string(m_expression_string.value()));
    }

    return builder.to_byte_string();
}

ByteString CallConstruct::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CallConstruct {}, {}, ",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable));

    builder.append(format_operand_list("args"sv, { m_arguments, m_argument_count }, executable));

    if (m_expression_string.has_value()) {
        builder.appendff(", `{}`", executable.get_string(m_expression_string.value()));
    }

    return builder.to_byte_string();
}

ByteString CallDirectEval::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CallDirectEval {}, {}, {}, ",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable));

    builder.append(format_operand_list("args"sv, { m_arguments, m_argument_count }, executable));

    if (m_expression_string.has_value()) {
        builder.appendff(", `{}`", executable.get_string(m_expression_string.value()));
    }

    return builder.to_byte_string();
}

ByteString CallBuiltin::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CallBuiltin {}, {}, {}, ",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable));

    builder.append(format_operand_list("args"sv, { m_arguments, m_argument_count }, executable));

    builder.appendff(", (builtin:{})", m_builtin);

    if (m_expression_string.has_value()) {
        builder.appendff(", `{}`", executable.get_string(m_expression_string.value()));
    }

    return builder.to_byte_string();
}

ByteString CallWithArgumentArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto type = call_type_to_string(m_type);
    StringBuilder builder;
    builder.appendff("CallWithArgumentArray{} {}, {}, {}, {}",
        type,
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable),
        format_operand("arguments"sv, m_arguments, executable));

    if (m_expression_string.has_value())
        builder.appendff(" ({})", executable.get_string(m_expression_string.value()));
    return builder.to_byte_string();
}

ByteString SuperCallWithArgumentArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SuperCallWithArgumentArray {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("arguments"sv, m_arguments, executable));
}

ByteString NewFunction::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("NewFunction {}",
        format_operand("dst"sv, m_dst, executable));
    if (m_function_node.has_name())
        builder.appendff(" name:{}", m_function_node.name());
    if (m_lhs_name.has_value())
        builder.appendff(" lhs_name:{}", executable.get_identifier(m_lhs_name.value()));
    if (m_home_object.has_value())
        builder.appendff(", {}", format_operand("home_object"sv, m_home_object.value(), executable));
    return builder.to_byte_string();
}

ByteString NewClass::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    auto name = m_class_expression.name();
    builder.appendff("NewClass {}",
        format_operand("dst"sv, m_dst, executable));
    if (m_super_class.has_value())
        builder.appendff(", {}", format_operand("super_class"sv, *m_super_class, executable));
    if (!name.is_empty())
        builder.appendff(", {}", name);
    if (m_lhs_name.has_value())
        builder.appendff(", lhs_name:{}", executable.get_identifier(m_lhs_name.value()));
    return builder.to_byte_string();
}

ByteString Return::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Return {}", format_operand("value"sv, m_value, executable));
}

ByteString Increment::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Increment {}", format_operand("dst"sv, m_dst, executable));
}

ByteString PostfixIncrement::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("PostfixIncrement {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("src"sv, m_src, executable));
}

ByteString Decrement::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Decrement {}", format_operand("dst"sv, m_dst, executable));
}

ByteString PostfixDecrement::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("PostfixDecrement {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("src"sv, m_src, executable));
}

ByteString Throw::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Throw {}",
        format_operand("src"sv, m_src, executable));
}

ByteString ThrowIfNotObject::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ThrowIfNotObject {}",
        format_operand("src"sv, m_src, executable));
}

ByteString ThrowIfNullish::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ThrowIfNullish {}",
        format_operand("src"sv, m_src, executable));
}

ByteString ThrowIfTDZ::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ThrowIfTDZ {}",
        format_operand("src"sv, m_src, executable));
}

ByteString EnterUnwindContext::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("EnterUnwindContext entry:{}", m_entry_point);
}

ByteString ScheduleJump::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("ScheduleJump {}", m_target);
}

ByteString LeaveLexicalEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "LeaveLexicalEnvironment"sv;
}

ByteString LeavePrivateEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "LeavePrivateEnvironment"sv;
}

ByteString LeaveUnwindContext::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "LeaveUnwindContext";
}

ByteString ContinuePendingUnwind::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("ContinuePendingUnwind resume:{}", m_resume_target);
}

ByteString Yield::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    if (m_continuation_label.has_value()) {
        return ByteString::formatted("Yield continuation:{}, {}",
            m_continuation_label.value(),
            format_operand("value"sv, m_value, executable));
    }
    return ByteString::formatted("Yield return {}",
        format_operand("value"sv, m_value, executable));
}

ByteString PrepareYield::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("PrepareYield {}, {}",
        format_operand("dst"sv, m_dest, executable),
        format_operand("value"sv, m_value, executable));
}

ByteString Await::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Await {}, continuation:{}",
        format_operand("argument"sv, m_argument, executable),
        m_continuation_label);
}

ByteString GetByValue::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetByValue {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable));
}

ByteString GetByValueWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetByValueWithThis {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable));
}

ByteString PutByValue::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted("PutByValue {}, {}, {}, kind:{}",
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable),
        format_operand("src"sv, m_src, executable),
        kind);
}

ByteString PutByValueWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted("PutByValueWithThis {}, {}, {}, {}, kind:{}",
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable),
        format_operand("src"sv, m_src, executable),
        format_operand("this"sv, m_this_value, executable),
        kind);
}

ByteString DeleteByValue::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteByValue {}, {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable));
}

ByteString DeleteByValueWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteByValueWithThis {}, {}, {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable),
        format_operand("this"sv, m_this_value, executable));
}

ByteString GetIterator::to_byte_string_impl(Executable const& executable) const
{
    auto hint = m_hint == IteratorHint::Sync ? "sync" : "async";
    return ByteString::formatted("GetIterator {}, {}, hint:{}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("iterable"sv, m_iterable, executable),
        hint);
}

ByteString GetMethod::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetMethod {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("object"sv, m_object, executable),
        executable.identifier_table->get(m_property));
}

ByteString GetObjectPropertyIterator::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetObjectPropertyIterator {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("object"sv, object(), executable));
}

ByteString IteratorClose::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    if (!m_completion_value.has_value())
        return ByteString::formatted("IteratorClose {}, completion_type={} completion_value=<empty>",
            format_operand("iterator_record"sv, m_iterator_record, executable),
            to_underlying(m_completion_type));

    auto completion_value_string = m_completion_value->to_string_without_side_effects();
    return ByteString::formatted("IteratorClose {}, completion_type={} completion_value={}",
        format_operand("iterator_record"sv, m_iterator_record, executable),
        to_underlying(m_completion_type), completion_value_string);
}

ByteString AsyncIteratorClose::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    if (!m_completion_value.has_value()) {
        return ByteString::formatted("AsyncIteratorClose {}, completion_type:{} completion_value:<empty>",
            format_operand("iterator_record"sv, m_iterator_record, executable),
            to_underlying(m_completion_type));
    }

    return ByteString::formatted("AsyncIteratorClose {}, completion_type:{}, completion_value:{}",
        format_operand("iterator_record"sv, m_iterator_record, executable),
        to_underlying(m_completion_type), m_completion_value);
}

ByteString IteratorNext::to_byte_string_impl(Executable const& executable) const
{
    return ByteString::formatted("IteratorNext {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("iterator_record"sv, m_iterator_record, executable));
}

ByteString IteratorNextUnpack::to_byte_string_impl(Executable const& executable) const
{
    return ByteString::formatted("IteratorNextUnpack {}, {}, {}",
        format_operand("dst_value"sv, m_dst_value, executable),
        format_operand("dst_done"sv, m_dst_done, executable),
        format_operand("iterator_record"sv, m_iterator_record, executable));
}

ByteString ResolveThisBinding::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "ResolveThisBinding"sv;
}

ByteString ResolveSuperBase::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ResolveSuperBase {}",
        format_operand("dst"sv, m_dst, executable));
}

ByteString GetNewTarget::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetNewTarget {}", format_operand("dst"sv, m_dst, executable));
}

ByteString GetImportMeta::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetImportMeta {}", format_operand("dst"sv, m_dst, executable));
}

ByteString TypeofBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("TypeofBinding {}, {}",
        format_operand("dst"sv, m_dst, executable),
        executable.identifier_table->get(m_identifier));
}

ByteString BlockDeclarationInstantiation::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "BlockDeclarationInstantiation"sv;
}

ByteString ImportCall::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ImportCall {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("specifier"sv, m_specifier, executable),
        format_operand("options"sv, m_options, executable));
}

ByteString Catch::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Catch {}",
        format_operand("dst"sv, m_dst, executable));
}

ByteString LeaveFinally::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("LeaveFinally");
}

ByteString RestoreScheduledJump::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("RestoreScheduledJump");
}

ByteString GetObjectFromIteratorRecord::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetObjectFromIteratorRecord {}, {}",
        format_operand("object"sv, m_object, executable),
        format_operand("iterator_record"sv, m_iterator_record, executable));
}

ByteString GetNextMethodFromIteratorRecord::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetNextMethodFromIteratorRecord {}, {}",
        format_operand("next_method"sv, m_next_method, executable),
        format_operand("iterator_record"sv, m_iterator_record, executable));
}

ByteString End::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("End {}", format_operand("value"sv, m_value, executable));
}

ByteString Dump::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Dump '{}', {}", m_text,
        format_operand("value"sv, m_value, executable));
}

ByteString GetCompletionFields::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetCompletionFields {}, {}, {}",
        format_operand("value_dst"sv, m_value_dst, executable),
        format_operand("type_dst"sv, m_type_dst, executable),
        format_operand("completion"sv, m_completion, executable));
}

ByteString SetCompletionType::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SetCompletionType {}, type={}",
        format_operand("completion"sv, m_completion, executable),
        to_underlying(m_type));
}

}
