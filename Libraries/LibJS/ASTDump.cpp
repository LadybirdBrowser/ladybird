/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/AST.h>
#include <LibJS/Runtime/ModuleRequest.h>

namespace JS {

// ANSI color codes for AST dump colorization.
static constexpr auto s_reset = "\033[0m"sv;
static constexpr auto s_dim = "\033[2m"sv;
static constexpr auto s_green = "\033[32m"sv;
static constexpr auto s_yellow = "\033[33m"sv;
static constexpr auto s_cyan = "\033[36m"sv;
static constexpr auto s_magenta = "\033[35m"sv;
static constexpr auto s_white_bold = "\033[1;37m"sv;

static void print_node(ASTDumpState const& state, StringView text)
{
    if (state.is_root) {
        outln("{}", text);
    } else if (state.use_color) {
        outln("{}{}{}{}{}", state.prefix,
            s_dim, state.is_last ? "\xe2\x94\x94\xe2\x94\x80 "sv : "\xe2\x94\x9c\xe2\x94\x80 "sv,
            s_reset, text);
    } else {
        outln("{}{}{}", state.prefix, state.is_last ? "\xe2\x94\x94\xe2\x94\x80 "sv : "\xe2\x94\x9c\xe2\x94\x80 "sv, text);
    }
}

static ByteString child_prefix(ASTDumpState const& state)
{
    if (state.is_root)
        return {};
    if (state.use_color)
        return ByteString::formatted("{}{}{}{}", state.prefix, s_dim, state.is_last ? "   "sv : "\xe2\x94\x82  "sv, s_reset);
    return ByteString::formatted("{}{}", state.prefix, state.is_last ? "   "sv : "\xe2\x94\x82  "sv);
}

static ASTDumpState child_state(ASTDumpState const& state, bool is_last)
{
    return { child_prefix(state), is_last, false, state.use_color };
}

static ByteString format_position(ASTDumpState const& state, SourceRange const& range)
{
    if (range.start.line == 0)
        return {};
    if (state.use_color)
        return ByteString::formatted(" {}@{}:{}{}", s_dim, range.start.line, range.start.column, s_reset);
    return ByteString::formatted(" @{}:{}", range.start.line, range.start.column);
}

static ByteString color_node_name(ASTDumpState const& state, StringView name)
{
    if (!state.use_color)
        return ByteString(name);
    return ByteString::formatted("{}{}{}", s_white_bold, name, s_reset);
}

template<typename T>
static ByteString color_string(ASTDumpState const& state, T const& value)
{
    if (!state.use_color)
        return ByteString::formatted("\"{}\"", value);
    return ByteString::formatted("{}\"{}\"{}", s_green, value, s_reset);
}

static ByteString color_number(ASTDumpState const& state, auto value)
{
    if (!state.use_color)
        return ByteString::formatted("{}", value);
    return ByteString::formatted("{}{}{}", s_magenta, value, s_reset);
}

static ByteString color_op(ASTDumpState const& state, char const* op)
{
    if (!state.use_color)
        return ByteString::formatted("({})", op);
    return ByteString::formatted("({}{}{})", s_yellow, op, s_reset);
}

static ByteString color_label(ASTDumpState const& state, StringView label)
{
    if (!state.use_color)
        return ByteString(label);
    return ByteString::formatted("{}{}{}", s_dim, label, s_reset);
}

static ByteString color_local(ASTDumpState const& state, Identifier::Local const& local)
{
    auto kind = local.is_argument() ? "argument"sv : "variable"sv;
    if (!state.use_color)
        return ByteString::formatted("[{}:{}]", kind, local.index);
    return ByteString::formatted("{}[{}:{}]{}", s_cyan, kind, local.index, s_reset);
}

static ByteString color_global(ASTDumpState const& state)
{
    if (!state.use_color)
        return "[global]"_string.to_byte_string();
    return ByteString::formatted("{}[global]{}", s_yellow, s_reset);
}

static ByteString color_flag(ASTDumpState const& state, StringView flag)
{
    if (!state.use_color)
        return ByteString::formatted("[{}]", flag);
    return ByteString::formatted("{}[{}]{}", s_dim, flag, s_reset);
}

static char const* binary_op_to_string(BinaryOp op)
{
    switch (op) {
    case BinaryOp::Addition:
        return "+";
    case BinaryOp::Subtraction:
        return "-";
    case BinaryOp::Multiplication:
        return "*";
    case BinaryOp::Division:
        return "/";
    case BinaryOp::Modulo:
        return "%";
    case BinaryOp::Exponentiation:
        return "**";
    case BinaryOp::StrictlyEquals:
        return "===";
    case BinaryOp::StrictlyInequals:
        return "!==";
    case BinaryOp::LooselyEquals:
        return "==";
    case BinaryOp::LooselyInequals:
        return "!=";
    case BinaryOp::GreaterThan:
        return ">";
    case BinaryOp::GreaterThanEquals:
        return ">=";
    case BinaryOp::LessThan:
        return "<";
    case BinaryOp::LessThanEquals:
        return "<=";
    case BinaryOp::BitwiseAnd:
        return "&";
    case BinaryOp::BitwiseOr:
        return "|";
    case BinaryOp::BitwiseXor:
        return "^";
    case BinaryOp::LeftShift:
        return "<<";
    case BinaryOp::RightShift:
        return ">>";
    case BinaryOp::UnsignedRightShift:
        return ">>>";
    case BinaryOp::In:
        return "in";
    case BinaryOp::InstanceOf:
        return "instanceof";
    }
    VERIFY_NOT_REACHED();
}

static char const* logical_op_to_string(LogicalOp op)
{
    switch (op) {
    case LogicalOp::And:
        return "&&";
    case LogicalOp::Or:
        return "||";
    case LogicalOp::NullishCoalescing:
        return "??";
    }
    VERIFY_NOT_REACHED();
}

static char const* unary_op_to_string(UnaryOp op)
{
    switch (op) {
    case UnaryOp::BitwiseNot:
        return "~";
    case UnaryOp::Not:
        return "!";
    case UnaryOp::Plus:
        return "+";
    case UnaryOp::Minus:
        return "-";
    case UnaryOp::Typeof:
        return "typeof";
    case UnaryOp::Void:
        return "void";
    case UnaryOp::Delete:
        return "delete";
    }
    VERIFY_NOT_REACHED();
}

static char const* assignment_op_to_string(AssignmentOp op)
{
    switch (op) {
    case AssignmentOp::Assignment:
        return "=";
    case AssignmentOp::AdditionAssignment:
        return "+=";
    case AssignmentOp::SubtractionAssignment:
        return "-=";
    case AssignmentOp::MultiplicationAssignment:
        return "*=";
    case AssignmentOp::DivisionAssignment:
        return "/=";
    case AssignmentOp::ModuloAssignment:
        return "%=";
    case AssignmentOp::ExponentiationAssignment:
        return "**=";
    case AssignmentOp::BitwiseAndAssignment:
        return "&=";
    case AssignmentOp::BitwiseOrAssignment:
        return "|=";
    case AssignmentOp::BitwiseXorAssignment:
        return "^=";
    case AssignmentOp::LeftShiftAssignment:
        return "<<=";
    case AssignmentOp::RightShiftAssignment:
        return ">>=";
    case AssignmentOp::UnsignedRightShiftAssignment:
        return ">>>=";
    case AssignmentOp::AndAssignment:
        return "&&=";
    case AssignmentOp::OrAssignment:
        return "||=";
    case AssignmentOp::NullishAssignment:
        return "\?\?=";
    }
    VERIFY_NOT_REACHED();
}

static char const* update_op_to_string(UpdateOp op)
{
    switch (op) {
    case UpdateOp::Increment:
        return "++";
    case UpdateOp::Decrement:
        return "--";
    }
    VERIFY_NOT_REACHED();
}

static char const* declaration_kind_to_string(DeclarationKind kind)
{
    switch (kind) {
    case DeclarationKind::None:
        VERIFY_NOT_REACHED();
    case DeclarationKind::Let:
        return "let";
    case DeclarationKind::Var:
        return "var";
    case DeclarationKind::Const:
        return "const";
    }
    VERIFY_NOT_REACHED();
}

static char const* class_method_kind_to_string(ClassMethod::Kind kind)
{
    switch (kind) {
    case ClassMethod::Kind::Method:
        return "method";
    case ClassMethod::Kind::Getter:
        return "getter";
    case ClassMethod::Kind::Setter:
        return "setter";
    }
    VERIFY_NOT_REACHED();
}

static ByteString format_assert_clauses(ModuleRequest const& request)
{
    if (request.attributes.is_empty())
        return {};
    StringBuilder builder;
    builder.append(" ["sv);
    for (size_t i = 0; i < request.attributes.size(); ++i) {
        if (i > 0)
            builder.append(", "sv);
        builder.appendff("{}: {}", request.attributes[i].key, request.attributes[i].value);
    }
    builder.append(']');
    return builder.to_byte_string();
}

void ASTNode::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, class_name()), format_position(state, source_range())));
}

void ScopeNode::dump(ASTDumpState const& state) const
{
    StringBuilder description;
    description.append(color_node_name(state, class_name()));
    if (is<Program>(*this)) {
        auto const& program = static_cast<Program const&>(*this);
        description.appendff(" {}", color_op(state, program.type() == Program::Type::Module ? "module" : "script"));
        if (program.is_strict_mode())
            description.appendff(" {}", color_flag(state, "strict"sv));
        if (program.has_top_level_await())
            description.appendff(" {}", color_flag(state, "top-level-await"sv));
    }
    description.append(format_position(state, source_range()));
    print_node(state, description.to_byte_string());
    for (size_t i = 0; i < m_children.size(); ++i)
        m_children[i]->dump(child_state(state, i == m_children.size() - 1));
}

void LabelledStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "LabelledStatement"sv), color_string(state, m_label), format_position(state, source_range())));
    m_labelled_item->dump(child_state(state, true));
}

void ClassFieldInitializerStatement::dump(ASTDumpState const&) const
{
    // This should not be dumped as it is never part of an actual AST.
    VERIFY_NOT_REACHED();
}

void BinaryExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "BinaryExpression"sv), color_op(state, binary_op_to_string(m_op)), format_position(state, source_range())));
    m_lhs->dump(child_state(state, false));
    m_rhs->dump(child_state(state, true));
}

void LogicalExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "LogicalExpression"sv), color_op(state, logical_op_to_string(m_op)), format_position(state, source_range())));
    m_lhs->dump(child_state(state, false));
    m_rhs->dump(child_state(state, true));
}

void UnaryExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "UnaryExpression"sv), color_op(state, unary_op_to_string(m_op)), format_position(state, source_range())));
    m_lhs->dump(child_state(state, true));
}

void CallExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, is<NewExpression>(*this) ? "NewExpression"sv : "CallExpression"sv), format_position(state, source_range())));
    bool has_arguments = !arguments().is_empty();
    m_callee->dump(child_state(state, !has_arguments));
    for (size_t i = 0; i < arguments().size(); ++i)
        arguments()[i].value->dump(child_state(state, i == arguments().size() - 1));
}

void SuperCall::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "SuperCall"sv), format_position(state, source_range())));
    for (size_t i = 0; i < m_arguments.size(); ++i)
        m_arguments[i].value->dump(child_state(state, i == m_arguments.size() - 1));
}

void ClassDeclaration::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ClassDeclaration"sv), format_position(state, source_range())));
    m_class_expression->dump(child_state(state, true));
}

void ClassExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "ClassExpression"sv), color_string(state, name()), format_position(state, source_range())));
    bool has_super = !m_super_class.is_null();
    bool has_elements = !m_elements.is_empty();

    if (has_super) {
        print_node(child_state(state, false), color_label(state, "super class"sv));
        m_super_class->dump(child_state(child_state(state, false), true));
    }

    print_node(child_state(state, !has_elements), color_label(state, "constructor"sv));
    m_constructor->dump(child_state(child_state(state, !has_elements), true));

    if (has_elements) {
        print_node(child_state(state, true), color_label(state, "elements"sv));
        for (size_t i = 0; i < m_elements.size(); ++i)
            m_elements[i]->dump(child_state(child_state(state, true), i == m_elements.size() - 1));
    }
}

void ClassMethod::dump(ASTDumpState const& state) const
{
    StringBuilder description;
    description.append(color_node_name(state, "ClassMethod"sv));
    if (is_static())
        description.append(" static"sv);
    if (m_kind != Kind::Method)
        description.appendff(" {}", color_op(state, class_method_kind_to_string(m_kind)));
    description.append(format_position(state, source_range()));
    print_node(state, description.to_byte_string());
    m_key->dump(child_state(state, false));
    m_function->dump(child_state(state, true));
}

void ClassField::dump(ASTDumpState const& state) const
{
    StringBuilder description;
    description.append(color_node_name(state, "ClassField"sv));
    if (is_static())
        description.append(" static"sv);
    description.append(format_position(state, source_range()));
    print_node(state, description.to_byte_string());
    m_key->dump(child_state(state, !m_initializer));
    if (m_initializer) {
        print_node(child_state(state, true), color_label(state, "initializer"sv));
        m_initializer->dump(child_state(child_state(state, true), true));
    }
}

void StaticInitializer::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "StaticInitializer"sv), format_position(state, source_range())));
    m_function_body->dump(child_state(state, true));
}

void StringLiteral::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "StringLiteral"sv), color_string(state, m_value), format_position(state, source_range())));
}

void SuperExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "SuperExpression"sv), format_position(state, source_range())));
}

void NumericLiteral::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "NumericLiteral"sv), color_number(state, m_value), format_position(state, source_range())));
}

void BigIntLiteral::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "BigIntLiteral"sv), color_number(state, m_value), format_position(state, source_range())));
}

void BooleanLiteral::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "BooleanLiteral"sv), color_number(state, m_value), format_position(state, source_range())));
}

void NullLiteral::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "NullLiteral"sv), format_position(state, source_range())));
}

void BindingPattern::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}", color_node_name(state, "BindingPattern"sv), color_op(state, kind == Kind::Array ? "array" : "object")));

    for (size_t i = 0; i < entries.size(); ++i) {
        auto const& entry = entries[i];
        auto entry_state = child_state(state, i == entries.size() - 1);

        if (kind == Kind::Array && entry.is_elision()) {
            print_node(entry_state, color_node_name(state, "Elision"sv));
            continue;
        }

        StringBuilder label;
        label.append("entry"sv);
        if (entry.is_rest)
            label.append(" (rest)"sv);
        print_node(entry_state, color_label(state, label.to_byte_string()));

        bool has_alias = entry.alias.has<NonnullRefPtr<Identifier const>>()
            || entry.alias.has<NonnullRefPtr<BindingPattern const>>()
            || entry.alias.has<NonnullRefPtr<MemberExpression const>>();
        bool has_initializer = entry.initializer;

        if (kind == Kind::Object) {
            if (entry.name.has<NonnullRefPtr<Identifier const>>()) {
                print_node(child_state(entry_state, !has_alias && !has_initializer), color_label(state, "name"sv));
                entry.name.get<NonnullRefPtr<Identifier const>>()->dump(child_state(child_state(entry_state, !has_alias && !has_initializer), true));
            } else if (entry.name.has<NonnullRefPtr<Expression const>>()) {
                print_node(child_state(entry_state, !has_alias && !has_initializer), color_label(state, "name (computed)"sv));
                entry.name.get<NonnullRefPtr<Expression const>>()->dump(child_state(child_state(entry_state, !has_alias && !has_initializer), true));
            }
        }

        if (has_alias) {
            print_node(child_state(entry_state, !has_initializer), color_label(state, "alias"sv));
            if (entry.alias.has<NonnullRefPtr<Identifier const>>())
                entry.alias.get<NonnullRefPtr<Identifier const>>()->dump(child_state(child_state(entry_state, !has_initializer), true));
            else if (entry.alias.has<NonnullRefPtr<BindingPattern const>>())
                entry.alias.get<NonnullRefPtr<BindingPattern const>>()->dump(child_state(child_state(entry_state, !has_initializer), true));
            else if (entry.alias.has<NonnullRefPtr<MemberExpression const>>())
                entry.alias.get<NonnullRefPtr<MemberExpression const>>()->dump(child_state(child_state(entry_state, !has_initializer), true));
        }

        if (has_initializer) {
            print_node(child_state(entry_state, true), color_label(state, "initializer"sv));
            entry.initializer->dump(child_state(child_state(entry_state, true), true));
        }
    }
}

void FunctionNode::dump(ASTDumpState const& state, ByteString const& class_name, SourceRange const& range) const
{
    StringBuilder description;
    description.append(color_node_name(state, class_name));
    auto is_async = m_kind == FunctionKind::Async || m_kind == FunctionKind::AsyncGenerator;
    auto is_generator = m_kind == FunctionKind::Generator || m_kind == FunctionKind::AsyncGenerator;
    if (is_async)
        description.append(" async"sv);
    if (is_generator)
        description.append('*');
    description.appendff(" {}", color_string(state, name()));
    if (m_is_strict_mode)
        description.appendff(" {}", color_flag(state, "strict"sv));
    if (m_is_arrow_function)
        description.appendff(" {}", color_flag(state, "arrow"sv));
    if (m_parsing_insights.contains_direct_call_to_eval)
        description.appendff(" {}", color_flag(state, "direct-eval"sv));
    if (m_parsing_insights.uses_this)
        description.appendff(" {}", color_flag(state, "uses-this"sv));
    if (m_parsing_insights.uses_this_from_environment)
        description.appendff(" {}", color_flag(state, "uses-this-from-environment"sv));
    if (m_parsing_insights.might_need_arguments_object)
        description.appendff(" {}", color_flag(state, "might-need-arguments"sv));
    description.append(format_position(state, range));
    print_node(state, description.to_byte_string());

    if (!m_parameters->is_empty()) {
        print_node(child_state(state, false), color_label(state, "parameters"sv));
        auto params_state = child_state(state, false);
        auto const& params = m_parameters->parameters();
        for (size_t i = 0; i < params.size(); ++i) {
            auto const& parameter = params[i];
            auto param_state = child_state(params_state, i == params.size() - 1);
            bool has_default = parameter.default_value;
            if (parameter.is_rest) {
                print_node(param_state, color_label(state, "rest"sv));
                parameter.binding.visit(
                    [&](Identifier const& identifier) {
                        identifier.dump(child_state(param_state, !has_default));
                    },
                    [&](BindingPattern const& pattern) {
                        pattern.dump(child_state(param_state, !has_default));
                    });
            } else {
                parameter.binding.visit(
                    [&](Identifier const& identifier) {
                        identifier.dump(child_state(params_state, i == params.size() - 1));
                    },
                    [&](BindingPattern const& pattern) {
                        pattern.dump(child_state(params_state, i == params.size() - 1));
                    });
            }
            if (has_default) {
                print_node(child_state(param_state, true), color_label(state, "default"sv));
                parameter.default_value->dump(child_state(child_state(param_state, true), true));
            }
        }
    }

    print_node(child_state(state, true), color_label(state, "body"sv));
    body().dump(child_state(child_state(state, true), true));
}

void FunctionDeclaration::dump(ASTDumpState const& state) const
{
    FunctionNode::dump(state, class_name(), source_range());
}

void FunctionExpression::dump(ASTDumpState const& state) const
{
    FunctionNode::dump(state, class_name(), source_range());
}

void YieldExpression::dump(ASTDumpState const& state) const
{
    StringBuilder description;
    description.append(color_node_name(state, "YieldExpression"sv));
    if (is_yield_from())
        description.appendff(" {}", color_flag(state, "yield*"sv));
    description.append(format_position(state, source_range()));
    print_node(state, description.to_byte_string());
    if (argument())
        argument()->dump(child_state(state, true));
}

void AwaitExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "AwaitExpression"sv), format_position(state, source_range())));
    m_argument->dump(child_state(state, true));
}

void ReturnStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ReturnStatement"sv), format_position(state, source_range())));
    if (argument())
        argument()->dump(child_state(state, true));
}

void IfStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "IfStatement"sv), format_position(state, source_range())));
    bool has_alternate = alternate();

    print_node(child_state(state, false), color_label(state, "test"sv));
    predicate().dump(child_state(child_state(state, false), true));

    print_node(child_state(state, !has_alternate), color_label(state, "consequent"sv));
    consequent().dump(child_state(child_state(state, !has_alternate), true));

    if (has_alternate) {
        print_node(child_state(state, true), color_label(state, "alternate"sv));
        alternate()->dump(child_state(child_state(state, true), true));
    }
}

void WhileStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "WhileStatement"sv), format_position(state, source_range())));
    print_node(child_state(state, false), color_label(state, "test"sv));
    test().dump(child_state(child_state(state, false), true));
    print_node(child_state(state, true), color_label(state, "body"sv));
    body().dump(child_state(child_state(state, true), true));
}

void WithStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "WithStatement"sv), format_position(state, source_range())));
    print_node(child_state(state, false), color_label(state, "object"sv));
    object().dump(child_state(child_state(state, false), true));
    print_node(child_state(state, true), color_label(state, "body"sv));
    body().dump(child_state(child_state(state, true), true));
}

void DoWhileStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "DoWhileStatement"sv), format_position(state, source_range())));
    print_node(child_state(state, false), color_label(state, "body"sv));
    body().dump(child_state(child_state(state, false), true));
    print_node(child_state(state, true), color_label(state, "test"sv));
    test().dump(child_state(child_state(state, true), true));
}

void ForStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ForStatement"sv), format_position(state, source_range())));

    if (init()) {
        print_node(child_state(state, false), color_label(state, "init"sv));
        init()->dump(child_state(child_state(state, false), true));
    }
    if (test()) {
        print_node(child_state(state, false), color_label(state, "test"sv));
        test()->dump(child_state(child_state(state, false), true));
    }
    if (update()) {
        print_node(child_state(state, false), color_label(state, "update"sv));
        update()->dump(child_state(child_state(state, false), true));
    }
    print_node(child_state(state, true), color_label(state, "body"sv));
    body().dump(child_state(child_state(state, true), true));
}

void ForInStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ForInStatement"sv), format_position(state, source_range())));
    print_node(child_state(state, false), color_label(state, "lhs"sv));
    lhs().visit([&](auto& lhs) { lhs->dump(child_state(child_state(state, false), true)); });
    print_node(child_state(state, false), color_label(state, "rhs"sv));
    rhs().dump(child_state(child_state(state, false), true));
    print_node(child_state(state, true), color_label(state, "body"sv));
    body().dump(child_state(child_state(state, true), true));
}

void ForOfStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ForOfStatement"sv), format_position(state, source_range())));
    print_node(child_state(state, false), color_label(state, "lhs"sv));
    lhs().visit([&](auto& lhs) { lhs->dump(child_state(child_state(state, false), true)); });
    print_node(child_state(state, false), color_label(state, "rhs"sv));
    rhs().dump(child_state(child_state(state, false), true));
    print_node(child_state(state, true), color_label(state, "body"sv));
    body().dump(child_state(child_state(state, true), true));
}

void ForAwaitOfStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ForAwaitOfStatement"sv), format_position(state, source_range())));
    print_node(child_state(state, false), color_label(state, "lhs"sv));
    m_lhs.visit([&](auto& lhs) { lhs->dump(child_state(child_state(state, false), true)); });
    print_node(child_state(state, false), color_label(state, "rhs"sv));
    m_rhs->dump(child_state(child_state(state, false), true));
    print_node(child_state(state, true), color_label(state, "body"sv));
    m_body->dump(child_state(child_state(state, true), true));
}

void Identifier::dump(ASTDumpState const& state) const
{
    StringBuilder description;
    description.append(color_node_name(state, "Identifier"sv));
    description.appendff(" {}", color_string(state, m_string));
    if (is_local()) {
        description.appendff(" {}", color_local(state, local_index()));
    } else if (is_global()) {
        description.appendff(" {}", color_global(state));
    }
    if (m_declaration_kind != DeclarationKind::None)
        description.appendff(" {}", color_op(state, declaration_kind_to_string(m_declaration_kind)));
    if (m_is_inside_scope_with_eval)
        description.appendff(" {}", color_flag(state, "in-eval-scope"sv));
    description.append(format_position(state, source_range()));
    print_node(state, description.to_byte_string());
}

void PrivateIdentifier::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "PrivateIdentifier"sv), color_string(state, m_string), format_position(state, source_range())));
}

void SpreadExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "SpreadExpression"sv), format_position(state, source_range())));
    m_target->dump(child_state(state, true));
}

void ThisExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ThisExpression"sv), format_position(state, source_range())));
}

void AssignmentExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "AssignmentExpression"sv), color_op(state, assignment_op_to_string(m_op)), format_position(state, source_range())));
    m_lhs.visit([&](auto& lhs) { lhs->dump(child_state(state, false)); });
    m_rhs->dump(child_state(state, true));
}

void UpdateExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} ({}, {}){}", color_node_name(state, "UpdateExpression"sv), update_op_to_string(m_op), m_prefixed ? "prefix" : "postfix", format_position(state, source_range())));
    m_argument->dump(child_state(state, true));
}

void VariableDeclaration::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "VariableDeclaration"sv), color_op(state, declaration_kind_to_string(m_declaration_kind)), format_position(state, source_range())));
    for (size_t i = 0; i < m_declarations.size(); ++i)
        m_declarations[i]->dump(child_state(state, i == m_declarations.size() - 1));
}

void UsingDeclaration::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "UsingDeclaration"sv), format_position(state, source_range())));
    for (size_t i = 0; i < m_declarations.size(); ++i)
        m_declarations[i]->dump(child_state(state, i == m_declarations.size() - 1));
}

void VariableDeclarator::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "VariableDeclarator"sv), format_position(state, source_range())));
    bool has_init = m_init;
    m_target.visit([&](auto const& value) { value->dump(child_state(state, !has_init)); });
    if (m_init)
        m_init->dump(child_state(state, true));
}

void ObjectProperty::dump(ASTDumpState const& state) const
{
    if (m_property_type == Type::Spread) {
        print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "ObjectProperty"sv), color_op(state, "spread"), format_position(state, source_range())));
        m_key->dump(child_state(state, true));
    } else {
        StringBuilder description;
        description.append(color_node_name(state, "ObjectProperty"sv));
        if (m_is_method)
            description.appendff(" {}", color_op(state, "method"));
        else if (m_property_type == Type::Getter)
            description.appendff(" {}", color_op(state, "getter"));
        else if (m_property_type == Type::Setter)
            description.appendff(" {}", color_op(state, "setter"));
        description.append(format_position(state, source_range()));
        print_node(state, description.to_byte_string());
        m_key->dump(child_state(state, false));
        m_value->dump(child_state(state, true));
    }
}

void ObjectExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ObjectExpression"sv), format_position(state, source_range())));
    for (size_t i = 0; i < m_properties.size(); ++i)
        m_properties[i]->dump(child_state(state, i == m_properties.size() - 1));
}

void ExpressionStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ExpressionStatement"sv), format_position(state, source_range())));
    m_expression->dump(child_state(state, true));
}

void MemberExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, is_computed() ? "MemberExpression [computed]"sv : "MemberExpression"sv), format_position(state, source_range())));
    m_object->dump(child_state(state, false));
    m_property->dump(child_state(state, true));
}

void OptionalChain::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "OptionalChain"sv), format_position(state, source_range())));
    m_base->dump(child_state(state, m_references.is_empty()));
    for (size_t i = 0; i < m_references.size(); ++i) {
        auto ref_state = child_state(state, i == m_references.size() - 1);
        m_references[i].visit(
            [&](Call const& call) {
                print_node(ref_state, ByteString::formatted("Call({})", call.mode == Mode::Optional ? "optional" : "not optional"));
                for (size_t j = 0; j < call.arguments.size(); ++j)
                    call.arguments[j].value->dump(child_state(ref_state, j == call.arguments.size() - 1));
            },
            [&](ComputedReference const& ref) {
                print_node(ref_state, ByteString::formatted("ComputedReference({})", ref.mode == Mode::Optional ? "optional" : "not optional"));
                ref.expression->dump(child_state(ref_state, true));
            },
            [&](MemberReference const& ref) {
                print_node(ref_state, ByteString::formatted("MemberReference({})", ref.mode == Mode::Optional ? "optional" : "not optional"));
                ref.identifier->dump(child_state(ref_state, true));
            },
            [&](PrivateMemberReference const& ref) {
                print_node(ref_state, ByteString::formatted("PrivateMemberReference({})", ref.mode == Mode::Optional ? "optional" : "not optional"));
                ref.private_identifier->dump(child_state(ref_state, true));
            });
    }
}

void MetaProperty::dump(ASTDumpState const& state) const
{
    char const* name = nullptr;
    switch (m_type) {
    case MetaProperty::Type::NewTarget:
        name = "new.target";
        break;
    case MetaProperty::Type::ImportMeta:
        name = "import.meta";
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "MetaProperty"sv), name, format_position(state, source_range())));
}

void ImportCall::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ImportCall"sv), format_position(state, source_range())));
    m_specifier->dump(child_state(state, !m_options));
    if (m_options) {
        print_node(child_state(state, true), color_label(state, "options"sv));
        m_options->dump(child_state(child_state(state, true), true));
    }
}

void RegExpLiteral::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} /{}/{}{}", color_node_name(state, "RegExpLiteral"sv), pattern(), flags(), format_position(state, source_range())));
}

void ArrayExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ArrayExpression"sv), format_position(state, source_range())));
    for (size_t i = 0; i < m_elements.size(); ++i) {
        if (m_elements[i])
            m_elements[i]->dump(child_state(state, i == m_elements.size() - 1));
        else
            print_node(child_state(state, i == m_elements.size() - 1), "<elision>"sv);
    }
}

void TemplateLiteral::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "TemplateLiteral"sv), format_position(state, source_range())));
    for (size_t i = 0; i < m_expressions.size(); ++i)
        m_expressions[i]->dump(child_state(state, i == m_expressions.size() - 1));
}

void TaggedTemplateLiteral::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "TaggedTemplateLiteral"sv), format_position(state, source_range())));
    print_node(child_state(state, false), color_label(state, "tag"sv));
    m_tag->dump(child_state(child_state(state, false), true));
    print_node(child_state(state, true), color_label(state, "template"sv));
    m_template_literal->dump(child_state(child_state(state, true), true));
}

void TryStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "TryStatement"sv), format_position(state, source_range())));
    bool has_handler = handler();
    bool has_finalizer = finalizer();

    print_node(child_state(state, !has_handler && !has_finalizer), color_label(state, "block"sv));
    block().dump(child_state(child_state(state, !has_handler && !has_finalizer), true));

    if (has_handler) {
        print_node(child_state(state, !has_finalizer), color_label(state, "handler"sv));
        handler()->dump(child_state(child_state(state, !has_finalizer), true));
    }

    if (has_finalizer) {
        print_node(child_state(state, true), color_label(state, "finalizer"sv));
        finalizer()->dump(child_state(child_state(state, true), true));
    }
}

void CatchClause::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "CatchClause"sv), format_position(state, source_range())));
    bool has_parameter = !m_parameter.has<Empty>();
    if (has_parameter) {
        m_parameter.visit(
            [&](NonnullRefPtr<Identifier const> const& parameter) {
                print_node(child_state(state, false), color_label(state, "parameter"sv));
                parameter->dump(child_state(child_state(state, false), true));
            },
            [&](NonnullRefPtr<BindingPattern const> const& pattern) {
                print_node(child_state(state, false), color_label(state, "parameter"sv));
                pattern->dump(child_state(child_state(state, false), true));
            },
            [&](Empty) {});
    }
    body().dump(child_state(state, true));
}

void ThrowStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ThrowStatement"sv), format_position(state, source_range())));
    argument().dump(child_state(state, true));
}

void SwitchStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "SwitchStatement"sv), format_position(state, source_range())));
    print_node(child_state(state, m_cases.is_empty()), color_label(state, "discriminant"sv));
    m_discriminant->dump(child_state(child_state(state, m_cases.is_empty()), true));
    for (size_t i = 0; i < m_cases.size(); ++i)
        m_cases[i]->dump(child_state(state, i == m_cases.size() - 1));
}

void SwitchCase::dump(ASTDumpState const& state) const
{
    if (m_test) {
        print_node(state, ByteString::formatted("{}{}", color_node_name(state, "SwitchCase"sv), format_position(state, source_range())));
        print_node(child_state(state, false), color_label(state, "test"sv));
        m_test->dump(child_state(child_state(state, false), true));
    } else {
        print_node(state, ByteString::formatted("{} {}{}", color_node_name(state, "SwitchCase"sv), color_op(state, "default"), format_position(state, source_range())));
    }
    print_node(child_state(state, true), color_label(state, "consequent"sv));
    auto consequent_state = child_state(child_state(state, true), true);
    // Dump children from ScopeNode inline without an extra "BlockStatement" wrapper.
    for (size_t i = 0; i < children().size(); ++i)
        children()[i]->dump(child_state(consequent_state, i == children().size() - 1));
}

void ConditionalExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ConditionalExpression"sv), format_position(state, source_range())));
    print_node(child_state(state, false), color_label(state, "test"sv));
    m_test->dump(child_state(child_state(state, false), true));
    print_node(child_state(state, false), color_label(state, "consequent"sv));
    m_consequent->dump(child_state(child_state(state, false), true));
    print_node(child_state(state, true), color_label(state, "alternate"sv));
    m_alternate->dump(child_state(child_state(state, true), true));
}

void SequenceExpression::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "SequenceExpression"sv), format_position(state, source_range())));
    for (size_t i = 0; i < m_expressions.size(); ++i)
        m_expressions[i]->dump(child_state(state, i == m_expressions.size() - 1));
}

void ExportStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{}{}", color_node_name(state, "ExportStatement"sv), format_position(state, source_range())));

    auto string_or_null = []<typename T>(Optional<T> const& string) -> ByteString {
        if (!string.has_value())
            return "null";
        return ByteString::formatted("\"{}\"", string);
    };

    bool has_statement = m_statement;
    bool has_entries = !m_entries.is_empty();

    if (has_entries) {
        print_node(child_state(state, !has_statement), color_label(state, "entries"sv));
        auto entries_state = child_state(state, !has_statement);
        for (size_t i = 0; i < m_entries.size(); ++i) {
            auto const& entry = m_entries[i];
            StringBuilder desc;
            desc.appendff("ExportName: {}, LocalName: {}",
                string_or_null(entry.export_name),
                entry.is_module_request() ? ByteString("null") : string_or_null(entry.local_or_import_name));
            if (entry.is_module_request())
                desc.appendff(", ModuleRequest: {}{}", entry.m_module_request->module_specifier, format_assert_clauses(*entry.m_module_request));
            print_node(child_state(entries_state, i == m_entries.size() - 1), desc.to_byte_string());
        }
    }

    if (has_statement) {
        print_node(child_state(state, true), color_label(state, "statement"sv));
        m_statement->dump(child_state(child_state(state, true), true));
    }
}

void ImportStatement::dump(ASTDumpState const& state) const
{
    print_node(state, ByteString::formatted("{} from {}{}{}", color_node_name(state, "ImportStatement"sv), color_string(state, m_module_request.module_specifier), format_assert_clauses(m_module_request), format_position(state, source_range())));

    if (m_entries.is_empty())
        return;

    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto const& entry = m_entries[i];
        print_node(child_state(state, i == m_entries.size() - 1),
            ByteString::formatted("ImportName: {}, LocalName: {}", entry.import_name, entry.local_name));
    }
}

}
