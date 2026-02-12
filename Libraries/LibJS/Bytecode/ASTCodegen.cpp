/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Gunnar Beutner <gbeutner@serenityos.org>
 * Copyright (c) 2021, Marcin Gasperowicz <xnooga@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Find.h>
#include <AK/Queue.h>
#include <LibJS/AST.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/Bytecode/StringTable.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/ErrorTypes.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

using namespace JS::Bytecode;

static String bigint_literal_to_decimal_string(BigIntLiteral const& literal)
{
    auto const& raw = literal.raw_value();
    auto integer = [&] {
        if (raw[0] == '0' && raw.length() >= 3) {
            if (raw[1] == 'x' || raw[1] == 'X')
                return MUST(Crypto::SignedBigInteger::from_base(16, raw.substring(2, raw.length() - 3)));
            if (raw[1] == 'o' || raw[1] == 'O')
                return MUST(Crypto::SignedBigInteger::from_base(8, raw.substring(2, raw.length() - 3)));
            if (raw[1] == 'b' || raw[1] == 'B')
                return MUST(Crypto::SignedBigInteger::from_base(2, raw.substring(2, raw.length() - 3)));
        }
        return MUST(Crypto::SignedBigInteger::from_base(10, raw.substring(0, raw.length() - 1)));
    }();
    return MUST(integer.to_base(10));
}

static ScopedOperand choose_dst(Bytecode::Generator& generator, Optional<ScopedOperand> const& preferred_dst)
{
    if (preferred_dst.has_value())
        return preferred_dst.value();
    return generator.allocate_register();
}

Optional<ScopedOperand> ASTNode::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    generator.emit_todo(class_name());
    return {};
}

Optional<ScopedOperand> ScopeNode::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    bool did_create_lexical_environment = false;

    if (is<BlockStatement>(*this)) {
        if (has_lexical_declarations()) {
            did_create_lexical_environment = generator.emit_block_declaration_instantiation(*this);
        }
    } else if (is<Program>(*this)) {
        // GlobalDeclarationInstantiation is handled by the C++ AO.
    } else {
        // FunctionDeclarationInstantiation is handled by the C++ AO.
    }

    Optional<ScopedOperand> last_result;
    for (auto& child : children()) {
        auto result = child->generate_bytecode(generator);
        if (generator.must_propagate_completion()) {
            if (result.has_value()) {
                last_result = result;
                if (!generator.is_current_block_terminated()) {
                    if (auto completion_reg = generator.current_completion_register(); completion_reg.has_value())
                        generator.emit_mov(*completion_reg, *result);
                }
            }
        }
        if (generator.is_current_block_terminated())
            break;
    }

    if (did_create_lexical_environment)
        generator.end_variable_scope();

    return last_result;
}

Optional<ScopedOperand> EmptyStatement::generate_bytecode(Bytecode::Generator&, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    return Optional<ScopedOperand> {};
}

Optional<ScopedOperand> ExpressionStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return m_expression->generate_bytecode(generator);
}

static ThrowCompletionOr<ScopedOperand> constant_fold_unary_expression(Generator& generator, Value value, UnaryOp op)
{
    switch (op) {
    case UnaryOp::Minus:
        return generator.add_constant(Value(-TRY(value.to_double(generator.vm()))));
    case UnaryOp::Plus:
        return generator.add_constant(Value(+TRY(value.to_double(generator.vm()))));
    case UnaryOp::BitwiseNot:
        return generator.add_constant(TRY(bitwise_not(generator.vm(), value)));
    case UnaryOp::Not:
        return generator.add_constant(Value(!value.to_boolean()));
    default:
        return throw_completion(js_null());
    }
}

static Optional<ScopedOperand> try_constant_fold_unary_expression(Generator& generator, ScopedOperand& operand, UnaryOp op)
{
    if (operand.operand().is_constant()) {
        // OPTIMIZATION: Do some basic constant folding for unary operations on numbers.
        auto value = generator.get_constant(operand);
        if (auto result = constant_fold_unary_expression(generator, value, op); !result.is_error())
            return result.release_value();
    }

    return {};
}

static ThrowCompletionOr<ScopedOperand> constant_fold_binary_expression(Generator& generator, Value lhs, Value rhs, BinaryOp m_op)
{
    switch (m_op) {
    case BinaryOp::Addition:
        return generator.add_constant(TRY(add(generator.vm(), lhs, rhs)));
    case BinaryOp::Subtraction:
        return generator.add_constant(TRY(sub(generator.vm(), lhs, rhs)));
    case BinaryOp::Multiplication:
        return generator.add_constant(TRY(mul(generator.vm(), lhs, rhs)));
    case BinaryOp::Division:
        return generator.add_constant(TRY(div(generator.vm(), lhs, rhs)));
    case BinaryOp::Modulo:
        return generator.add_constant(TRY(mod(generator.vm(), lhs, rhs)));
    case BinaryOp::Exponentiation:
        return generator.add_constant(TRY(exp(generator.vm(), lhs, rhs)));
    case BinaryOp::GreaterThan:
        return generator.add_constant(Value { TRY(greater_than(generator.vm(), lhs, rhs)) });
    case BinaryOp::GreaterThanEquals:
        return generator.add_constant(Value { TRY(greater_than_equals(generator.vm(), lhs, rhs)) });
    case BinaryOp::LessThan:
        return generator.add_constant(Value { TRY(less_than(generator.vm(), lhs, rhs)) });
    case BinaryOp::LessThanEquals:
        return generator.add_constant(Value { TRY(less_than_equals(generator.vm(), lhs, rhs)) });
    case BinaryOp::LooselyInequals:
        return generator.add_constant(Value(!TRY(is_loosely_equal(generator.vm(), lhs, rhs))));
    case BinaryOp::LooselyEquals:
        return generator.add_constant(Value(TRY(is_loosely_equal(generator.vm(), lhs, rhs))));
    case BinaryOp::StrictlyInequals:
        return generator.add_constant(Value(!is_strictly_equal(lhs, rhs)));
    case BinaryOp::StrictlyEquals:
        return generator.add_constant(Value(is_strictly_equal(lhs, rhs)));
    case BinaryOp::BitwiseAnd:
        return generator.add_constant(TRY(bitwise_and(generator.vm(), lhs, rhs)));
    case BinaryOp::BitwiseOr:
        return generator.add_constant(TRY(bitwise_or(generator.vm(), lhs, rhs)));
    case BinaryOp::BitwiseXor:
        return generator.add_constant(TRY(bitwise_xor(generator.vm(), lhs, rhs)));
    case BinaryOp::LeftShift:
        return generator.add_constant(TRY(left_shift(generator.vm(), lhs, rhs)));
    case BinaryOp::RightShift:
        return generator.add_constant(TRY(right_shift(generator.vm(), lhs, rhs)));
    case BinaryOp::UnsignedRightShift:
        return generator.add_constant(TRY(unsigned_right_shift(generator.vm(), lhs, rhs)));
    case BinaryOp::In:
    case BinaryOp::InstanceOf:
        // NOTE: We just have to throw *something* to indicate that this is not a constant foldable operation.
        return throw_completion(js_null());
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<ScopedOperand> BinaryExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    if (m_op == BinaryOp::In && is<PrivateIdentifier>(*m_lhs)) {
        auto const& private_identifier = static_cast<PrivateIdentifier const&>(*m_lhs).string();
        auto base = m_rhs->generate_bytecode(generator).value();
        auto dst = choose_dst(generator, preferred_dst);
        generator.emit<Bytecode::Op::HasPrivateId>(dst, base, generator.intern_identifier(private_identifier));
        return dst;
    }

    // OPTIMIZATION: If LHS and/or RHS are numeric literals, we make sure they are converted to i32/u32
    //               as appropriate, to avoid having to perform these conversions at runtime.

    auto get_left_side = [&](Expression const& side) -> Optional<ScopedOperand> {
        switch (m_op) {
        case BinaryOp::BitwiseAnd:
        case BinaryOp::BitwiseOr:
        case BinaryOp::BitwiseXor:
        case BinaryOp::LeftShift:
        case BinaryOp::RightShift:
        case BinaryOp::UnsignedRightShift:
            // LHS will always be converted to i32 for these ops.
            if (side.is_numeric_literal()) {
                auto value = MUST(static_cast<NumericLiteral const&>(side).value().to_i32(generator.vm()));
                return generator.add_constant(Value(value));
            }
            break;
        default:
            break;
        }

        return side.generate_bytecode(generator);
    };

    auto get_right_side = [&](Expression const& side) -> Optional<ScopedOperand> {
        switch (m_op) {
        case BinaryOp::BitwiseAnd:
        case BinaryOp::BitwiseOr:
        case BinaryOp::BitwiseXor:
            // RHS will always be converted to i32 for these ops.
            if (side.is_numeric_literal()) {
                auto value = MUST(static_cast<NumericLiteral const&>(side).value().to_i32(generator.vm()));
                return generator.add_constant(Value(value));
            }
            break;
        case BinaryOp::LeftShift:
        case BinaryOp::RightShift:
        case BinaryOp::UnsignedRightShift:
            // RHS will always be converted to u32 for these ops.
            if (side.is_numeric_literal()) {
                auto value = MUST(static_cast<NumericLiteral const&>(side).value().to_u32(generator.vm()));
                return generator.add_constant(Value(value));
            }
            break;
        default:
            break;
        }

        return side.generate_bytecode(generator);
    };

    auto lhs = get_left_side(*m_lhs).value();
    auto rhs = get_right_side(*m_rhs).value();
    auto dst = choose_dst(generator, preferred_dst);

    // OPTIMIZATION: Do some basic constant folding for binary operations.
    if (lhs.operand().is_constant() && rhs.operand().is_constant()) {
        if (auto result = constant_fold_binary_expression(generator, generator.get_constant(lhs), generator.get_constant(rhs), m_op); !result.is_error())
            return result.release_value();
    }

    switch (m_op) {
    case BinaryOp::Addition:
        generator.emit<Bytecode::Op::Add>(dst, lhs, rhs);
        break;
    case BinaryOp::Subtraction:
        generator.emit<Bytecode::Op::Sub>(dst, lhs, rhs);
        break;
    case BinaryOp::Multiplication:
        generator.emit<Bytecode::Op::Mul>(dst, lhs, rhs);
        break;
    case BinaryOp::Division:
        generator.emit<Bytecode::Op::Div>(dst, lhs, rhs);
        break;
    case BinaryOp::Modulo:
        generator.emit<Bytecode::Op::Mod>(dst, lhs, rhs);
        break;
    case BinaryOp::Exponentiation:
        generator.emit<Bytecode::Op::Exp>(dst, lhs, rhs);
        break;
    case BinaryOp::GreaterThan:
        generator.emit<Bytecode::Op::GreaterThan>(dst, lhs, rhs);
        break;
    case BinaryOp::GreaterThanEquals:
        generator.emit<Bytecode::Op::GreaterThanEquals>(dst, lhs, rhs);
        break;
    case BinaryOp::LessThan:
        generator.emit<Bytecode::Op::LessThan>(dst, lhs, rhs);
        break;
    case BinaryOp::LessThanEquals:
        generator.emit<Bytecode::Op::LessThanEquals>(dst, lhs, rhs);
        break;
    case BinaryOp::LooselyInequals:
        generator.emit<Bytecode::Op::LooselyInequals>(dst, lhs, rhs);
        break;
    case BinaryOp::LooselyEquals:
        generator.emit<Bytecode::Op::LooselyEquals>(dst, lhs, rhs);
        break;
    case BinaryOp::StrictlyInequals:
        generator.emit<Bytecode::Op::StrictlyInequals>(dst, lhs, rhs);
        break;
    case BinaryOp::StrictlyEquals:
        generator.emit<Bytecode::Op::StrictlyEquals>(dst, lhs, rhs);
        break;
    case BinaryOp::BitwiseAnd:
        generator.emit<Bytecode::Op::BitwiseAnd>(dst, lhs, rhs);
        break;
    case BinaryOp::BitwiseOr:
        if (rhs.operand().is_constant() && generator.get_constant(rhs).is_int32() && generator.get_constant(rhs).as_i32() == 0) {
            // OPTIMIZATION: x | 0 == ToInt32(x)
            generator.emit<Bytecode::Op::ToInt32>(dst, lhs);
            break;
        }
        generator.emit<Bytecode::Op::BitwiseOr>(dst, lhs, rhs);
        break;
    case BinaryOp::BitwiseXor:
        generator.emit<Bytecode::Op::BitwiseXor>(dst, lhs, rhs);
        break;
    case BinaryOp::LeftShift:
        generator.emit<Bytecode::Op::LeftShift>(dst, lhs, rhs);
        break;
    case BinaryOp::RightShift:
        generator.emit<Bytecode::Op::RightShift>(dst, lhs, rhs);
        break;
    case BinaryOp::UnsignedRightShift:
        generator.emit<Bytecode::Op::UnsignedRightShift>(dst, lhs, rhs);
        break;
    case BinaryOp::In:
        generator.emit<Bytecode::Op::In>(dst, lhs, rhs);
        break;
    case BinaryOp::InstanceOf:
        generator.emit<Bytecode::Op::InstanceOf>(dst, lhs, rhs);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    return dst;
}

static Optional<ScopedOperand> constant_fold_logical_expression(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst, ScopedOperand& lhs, LogicalExpression const* expr)
{
    auto constant = generator.get_constant(lhs);

    auto return_rhs = [&] -> Optional<ScopedOperand> {
        auto dst = choose_dst(generator, preferred_dst);
        auto rhs = expr->rhs()->generate_bytecode(generator, dst).value();

        if (rhs.operand().is_constant())
            return rhs;

        generator.emit_mov(dst, rhs);
        return dst;
    };

    switch (expr->op()) {
    case LogicalOp::And:
        if (constant.to_boolean_slow_case())
            return return_rhs();
        return lhs;
    case LogicalOp::Or:
        if (constant.to_boolean_slow_case())
            return lhs;
        return return_rhs();
    case LogicalOp::NullishCoalescing:
        if (constant.is_nullish())
            return return_rhs();
        return lhs;
    default:
        VERIFY_NOT_REACHED();
    }

    return Optional<ScopedOperand> {};
}

Optional<ScopedOperand> LogicalExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto lhs = m_lhs->generate_bytecode(generator, preferred_dst).value();

    // OPTIMIZATION: return lhs/rhs directly if we can detect lhs as a truthy/falsey literal
    if (auto constant = generator.try_get_constant(lhs); constant.has_value()) {
        return constant_fold_logical_expression(generator, preferred_dst, lhs, this);
    }

    // lhs
    // jump op (true) end (false) rhs
    // rhs
    // jump always (true) end
    // end

    auto dst = choose_dst(generator, preferred_dst);
    generator.emit_mov(dst, lhs);

    auto& rhs_block = generator.make_block();
    auto& end_block = generator.make_block();

    switch (m_op) {
    case LogicalOp::And:
        generator.emit_jump_if(
            lhs,
            Bytecode::Label { rhs_block },
            Bytecode::Label { end_block });
        break;
    case LogicalOp::Or:
        generator.emit_jump_if(
            lhs,
            Bytecode::Label { end_block },
            Bytecode::Label { rhs_block });
        break;
    case LogicalOp::NullishCoalescing:
        generator.emit<Bytecode::Op::JumpNullish>(
            lhs,
            Bytecode::Label { rhs_block },
            Bytecode::Label { end_block });
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    generator.switch_to_basic_block(rhs_block);

    auto rhs = m_rhs->generate_bytecode(generator, dst).value();

    generator.emit_mov(dst, rhs);
    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { end_block });
    generator.switch_to_basic_block(end_block);
    return dst;
}

Optional<ScopedOperand> UnaryExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    if (m_op == UnaryOp::Delete)
        return generator.emit_delete_reference(m_lhs);

    Optional<ScopedOperand> src;
    // Typeof needs some special handling for when the LHS is an Identifier. Namely, it shouldn't throw on unresolvable references, but instead return "undefined".
    // Skip Not operator as it needs to be evaluated breadth first in order to detect `!!` optimization (otherwise the inner `!x` would eval first).
    if (m_op != UnaryOp::Typeof && m_op != UnaryOp::Not)
        src = m_lhs->generate_bytecode(generator).value();

    auto dst = choose_dst(generator, preferred_dst);

    if (src.has_value()) {
        if (auto result = try_constant_fold_unary_expression(generator, *src, m_op); result.has_value())
            return result.release_value();
    }

    switch (m_op) {
    case UnaryOp::BitwiseNot:
        generator.emit<Bytecode::Op::BitwiseNot>(dst, *src);
        break;
    case UnaryOp::Not:
        if (auto nested = as_if<UnaryExpression>(*m_lhs); nested && nested->op() == UnaryOp::Not) {
            auto value = nested->lhs()->generate_bytecode(generator).value();

            if (value.operand().is_constant())
                return generator.add_constant(Value(generator.get_constant(value).to_boolean()));

            generator.emit<Bytecode::Op::ToBoolean>(dst, value);
            break;
        }

        src = m_lhs->generate_bytecode(generator).value();

        if (auto result = try_constant_fold_unary_expression(generator, *src, m_op); result.has_value())
            return result.release_value();

        generator.emit<Bytecode::Op::Not>(dst, *src);
        break;
    case UnaryOp::Plus:
        generator.emit<Bytecode::Op::UnaryPlus>(dst, *src);
        break;
    case UnaryOp::Minus:
        generator.emit<Bytecode::Op::UnaryMinus>(dst, *src);
        break;
    case UnaryOp::Typeof:
        if (is<Identifier>(*m_lhs)) {
            auto& identifier = static_cast<Identifier const&>(*m_lhs);
            if (!identifier.is_local()) {
                generator.emit<Bytecode::Op::TypeofBinding>(dst, generator.intern_identifier(identifier.string()));
                break;
            }
        }

        src = m_lhs->generate_bytecode(generator).value();
        generator.emit<Bytecode::Op::Typeof>(dst, *src);
        break;
    case UnaryOp::Void:
        return generator.add_constant(js_undefined());
    case UnaryOp::Delete: // Delete is implemented above.
    default:
        VERIFY_NOT_REACHED();
    }

    return dst;
}

Optional<ScopedOperand> NumericLiteral::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generator.add_constant(Value(m_value));
}

Optional<ScopedOperand> BooleanLiteral::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generator.add_constant(Value(m_value));
}

Optional<ScopedOperand> NullLiteral::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generator.add_constant(js_null());
}

Optional<ScopedOperand> BigIntLiteral::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    // 1. Return the NumericValue of NumericLiteral as defined in 12.8.3.
    auto integer = [&] {
        if (m_value[0] == '0' && m_value.length() >= 3)
            if (m_value[1] == 'x' || m_value[1] == 'X')
                return MUST(Crypto::SignedBigInteger::from_base(16, m_value.substring(2, m_value.length() - 3)));
        if (m_value[1] == 'o' || m_value[1] == 'O')
            return MUST(Crypto::SignedBigInteger::from_base(8, m_value.substring(2, m_value.length() - 3)));
        if (m_value[1] == 'b' || m_value[1] == 'B')
            return MUST(Crypto::SignedBigInteger::from_base(2, m_value.substring(2, m_value.length() - 3)));
        return MUST(Crypto::SignedBigInteger::from_base(10, m_value.substring(0, m_value.length() - 1)));
    }();
    return generator.add_constant(BigInt::create(generator.vm(), move(integer)));
}

Optional<ScopedOperand> StringLiteral::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generator.add_constant(PrimitiveString::create(generator.vm(), m_value));
}

Optional<ScopedOperand> RegExpLiteral::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto source_index = generator.intern_string(m_pattern);
    auto flags_index = generator.intern_string(m_flags);
    auto regex_index = generator.intern_regex(Bytecode::ParsedRegex {
        .regex = m_parsed_regex,
        .pattern = m_parsed_pattern,
        .flags = m_parsed_flags,
    });
    auto dst = choose_dst(generator, preferred_dst);
    generator.emit<Bytecode::Op::NewRegExp>(dst, source_index, flags_index, regex_index);
    return dst;
}

Optional<ScopedOperand> Identifier::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    if (is_local()) {
        auto local_index = this->local_index();
        auto local = generator.local(local_index);
        if (!generator.is_local_initialized(local_index)) {
            if (local_index.is_argument()) {
                // Arguments are initialized to undefined by default, so here we need to replace it with the empty value to
                // trigger the TDZ check.
                generator.emit<Bytecode::Op::Mov>(local, generator.add_constant(js_special_empty_value()));
            }
            generator.emit<Bytecode::Op::ThrowIfTDZ>(local);
        }
        return local;
    }

    if (is_global()) {
        auto maybe_constant = generator.maybe_generate_builtin_constant(*this);
        if (maybe_constant.has_value())
            return maybe_constant.release_value();
    }

    auto dst = choose_dst(generator, preferred_dst);
    if (is_global()) {
        generator.emit<Bytecode::Op::GetGlobal>(dst, generator.intern_identifier(m_string), generator.next_global_variable_cache());
    } else {
        if (declaration_kind() == DeclarationKind::Var) {
            generator.emit<Bytecode::Op::GetInitializedBinding>(dst, generator.intern_identifier(m_string));
        } else {
            generator.emit<Bytecode::Op::GetBinding>(dst, generator.intern_identifier(m_string));
        }
    }
    return dst;
}

static Optional<ScopedOperand> arguments_to_array_for_call(Bytecode::Generator& generator, ReadonlySpan<CallExpression::Argument> arguments)
{
    auto dst = generator.allocate_register();
    if (arguments.is_empty()) {
        generator.emit<Bytecode::Op::NewArray>(dst, ReadonlySpan<ScopedOperand> {});
        return dst;
    }

    auto first_spread = find_if(arguments.begin(), arguments.end(), [](auto el) { return el.is_spread; });

    Vector<ScopedOperand> args;
    args.ensure_capacity(first_spread.index());
    for (auto it = arguments.begin(); it != first_spread; ++it) {
        VERIFY(!it->is_spread);
        auto reg = generator.allocate_register();
        auto value = it->value->generate_bytecode(generator).value();
        generator.emit_mov(reg, value);
        args.append(move(reg));
    }

    if (first_spread.index() != 0)
        generator.emit_with_extra_operand_slots<Bytecode::Op::NewArray>(args.size(), dst, args.span());
    else
        generator.emit<Bytecode::Op::NewArray>(dst, ReadonlySpan<ScopedOperand> {});

    if (first_spread != arguments.end()) {
        for (auto it = first_spread; it != arguments.end(); ++it) {
            auto value = it->value->generate_bytecode(generator).value();
            generator.emit<Bytecode::Op::ArrayAppend>(dst, value, it->is_spread);
        }
    }

    return dst;
}

Optional<ScopedOperand> SuperCall::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    Optional<ScopedOperand> arguments;
    if (m_is_synthetic == IsPartOfSyntheticConstructor::Yes) {
        // NOTE: This is the case where we have a fake constructor(...args) { super(...args); } which
        //       shouldn't call @@iterator of %Array.prototype%.
        VERIFY(m_arguments.size() == 1);
        VERIFY(m_arguments[0].is_spread);
        auto const& argument = m_arguments[0];
        // This generates a single argument.
        arguments = argument.value->generate_bytecode(generator);
    } else {
        arguments = arguments_to_array_for_call(generator, m_arguments).value();
    }

    auto dst = choose_dst(generator, preferred_dst);
    generator.emit<Bytecode::Op::SuperCallWithArgumentArray>(dst, *arguments, m_is_synthetic == IsPartOfSyntheticConstructor::Yes);
    return dst;
}

Optional<ScopedOperand> AssignmentExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    if (m_op == AssignmentOp::Assignment) {
        // AssignmentExpression : LeftHandSideExpression = AssignmentExpression
        return m_lhs.visit(
            // 1. If LeftHandSideExpression is neither an ObjectLiteral nor an ArrayLiteral, then
            [&](NonnullRefPtr<Expression const> const& lhs) -> Optional<ScopedOperand> {
                // a. Let lref be the result of evaluating LeftHandSideExpression.
                // b. ReturnIfAbrupt(lref).
                Optional<ScopedOperand> base;
                Optional<ScopedOperand> computed_property;
                Optional<ScopedOperand> this_value;

                bool lhs_is_super_expression = false;

                if (is<MemberExpression>(*lhs)) {
                    auto& expression = static_cast<MemberExpression const&>(*lhs);
                    lhs_is_super_expression = is<SuperExpression>(expression.object());

                    if (!lhs_is_super_expression) {
                        auto generated_base = expression.object().generate_bytecode(generator).value();
                        base = generator.copy_if_needed_to_preserve_evaluation_order(generated_base);
                    } else {
                        // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
                        // 1. Let env be GetThisEnvironment().
                        // 2. Let actualThis be ? env.GetThisBinding().
                        this_value = generator.get_this();

                        // SuperProperty : super [ Expression ]
                        // 3. Let propertyNameReference be ? Evaluation of Expression.
                        // 4. Let propertyNameValue be ? GetValue(propertyNameReference).
                    }

                    if (expression.is_computed()) {
                        auto property = expression.property().generate_bytecode(generator).value();
                        computed_property = generator.copy_if_needed_to_preserve_evaluation_order(property);
                        // To be continued later with PutByValue.
                    } else if (expression.property().is_identifier()) {
                        // Do nothing, this will be handled by PutById later.
                    } else if (expression.property().is_private_identifier()) {
                        // Do nothing, this will be handled by PutPrivateById later.
                    } else {
                        VERIFY_NOT_REACHED();
                    }

                    if (lhs_is_super_expression) {
                        // 5/7. Return ? MakeSuperPropertyReference(actualThis, propertyKey, strict).

                        // https://tc39.es/ecma262/#sec-makesuperpropertyreference
                        // 1. Let env be GetThisEnvironment().
                        // 2. Assert: env.HasSuperBinding() is true.
                        // 3. Let baseValue be ? env.GetSuperBase().
                        // 4. Return the Reference Record { [[Base]]: baseValue, [[ReferencedName]]: propertyKey, [[Strict]]: strict, [[ThisValue]]: actualThis }.
                        base = generator.allocate_register();
                        generator.emit<Bytecode::Op::ResolveSuperBase>(*base);
                    }
                } else if (is<Identifier>(*lhs)) {
                    // NOTE: For Identifiers, we cannot perform GetBinding and then write into the reference it retrieves, only SetVariable can do this.
                    // FIXME: However, this breaks spec as we are doing variable lookup after evaluating the RHS. This is observable in an object environment, where we visibly perform HasOwnProperty and Get(@@unscopables) on the binded object.
                } else {
                    // Per spec 13.15.2 step 1b, we must evaluate the LHS (the call),
                    // then throw ReferenceError before evaluating the RHS.
                    (void)lhs->generate_bytecode(generator);
                    auto exception = generator.allocate_register();
                    generator.emit<Bytecode::Op::NewReferenceError>(exception, generator.intern_string(ErrorType::InvalidLeftHandAssignment.message()));
                    generator.perform_needed_unwinds<Bytecode::Op::Throw>();
                    generator.emit<Bytecode::Op::Throw>(exception);
                    generator.switch_to_basic_block(generator.make_block());
                    return generator.add_constant(js_undefined());
                }

                // c. If IsAnonymousFunctionDefinition(AssignmentExpression) and IsIdentifierRef of LeftHandSideExpression are both true, then
                //    i. Let rval be ? NamedEvaluation of AssignmentExpression with argument lref.[[ReferencedName]].
                // d. Else,
                //    i. Let rref be the result of evaluating AssignmentExpression.
                //    ii. Let rval be ? GetValue(rref).
                auto rval = [&]() -> ScopedOperand {
                    if (lhs->is_identifier()) {
                        return generator.emit_named_evaluation_if_anonymous_function(*m_rhs, generator.intern_identifier(static_cast<Identifier const&>(*lhs).string()));
                    } else {
                        return m_rhs->generate_bytecode(generator).value();
                    }
                }();

                // e. Perform ? PutValue(lref, rval).
                if (is<Identifier>(*lhs)) {
                    auto& identifier = static_cast<Identifier const&>(*lhs);
                    if (identifier.is_local()) {
                        auto is_initialized = generator.is_local_initialized(identifier.local_index());
                        auto is_lexically_declared = generator.is_local_lexically_declared(identifier.local_index());
                        if (is_lexically_declared && !is_initialized) {
                            generator.emit<Bytecode::Op::ThrowIfTDZ>(generator.local(identifier.local_index()));
                        }
                    }
                    generator.emit_set_variable(identifier, rval);
                } else if (is<MemberExpression>(*lhs)) {
                    auto& expression = static_cast<MemberExpression const&>(*lhs);
                    auto base_identifier = generator.intern_identifier_for_expression(expression.object());

                    if (expression.is_computed()) {
                        if (!lhs_is_super_expression)
                            generator.emit_put_by_value(*base, *computed_property, rval, Bytecode::PutKind::Normal, move(base_identifier));
                        else
                            generator.emit_put_by_value_with_this(*base, *computed_property, *this_value, rval, PutKind::Normal);
                    } else if (expression.property().is_identifier()) {
                        auto property_key_table_index = generator.intern_property_key(as<Identifier>(expression.property()).string());
                        if (!lhs_is_super_expression)
                            generator.emit_put_by_id(*base, property_key_table_index, rval, Bytecode::PutKind::Normal, generator.next_property_lookup_cache(), move(base_identifier));
                        else
                            generator.emit<Bytecode::Op::PutNormalByIdWithThis>(*base, *this_value, property_key_table_index, rval, generator.next_property_lookup_cache());
                    } else if (expression.property().is_private_identifier()) {
                        auto identifier_table_ref = generator.intern_identifier(as<PrivateIdentifier>(expression.property()).string());
                        generator.emit<Bytecode::Op::PutPrivateById>(*base, identifier_table_ref, rval);
                    } else {
                        VERIFY_NOT_REACHED();
                    }
                } else {
                    VERIFY_NOT_REACHED();
                }

                // f. Return rval.
                return rval;
            },
            // 2. Let assignmentPattern be the AssignmentPattern that is covered by LeftHandSideExpression.
            [&](NonnullRefPtr<BindingPattern const> const& pattern) -> Optional<ScopedOperand> {
                // 3. Let rref be the result of evaluating AssignmentExpression.
                // 4. Let rval be ? GetValue(rref).
                auto rval = m_rhs->generate_bytecode(generator).value();

                // 5. Perform ? DestructuringAssignmentEvaluation of assignmentPattern with argument rval.
                pattern->generate_bytecode(generator, Bytecode::Op::BindingInitializationMode::Set, rval);

                // 6. Return rval.
                return rval;
            });
    }

    VERIFY(m_lhs.has<NonnullRefPtr<Expression const>>());
    auto& lhs_expression = m_lhs.get<NonnullRefPtr<Expression const>>();

    auto reference_operands = generator.emit_load_from_reference(lhs_expression);
    auto lhs = reference_operands.loaded_value.value();

    Bytecode::BasicBlock* rhs_block_ptr { nullptr };
    Bytecode::BasicBlock* lhs_block_ptr { nullptr };
    Bytecode::BasicBlock* end_block_ptr { nullptr };

    // Logical assignments short circuit.
    if (m_op == AssignmentOp::AndAssignment) { // &&=
        rhs_block_ptr = &generator.make_block();
        lhs_block_ptr = &generator.make_block();
        end_block_ptr = &generator.make_block();

        generator.emit_jump_if(
            lhs,
            Bytecode::Label { *rhs_block_ptr },
            Bytecode::Label { *lhs_block_ptr });
    } else if (m_op == AssignmentOp::OrAssignment) { // ||=
        rhs_block_ptr = &generator.make_block();
        lhs_block_ptr = &generator.make_block();
        end_block_ptr = &generator.make_block();

        generator.emit_jump_if(
            lhs,
            Bytecode::Label { *lhs_block_ptr },
            Bytecode::Label { *rhs_block_ptr });
    } else if (m_op == AssignmentOp::NullishAssignment) { // ??=
        rhs_block_ptr = &generator.make_block();
        lhs_block_ptr = &generator.make_block();
        end_block_ptr = &generator.make_block();

        generator.emit<Bytecode::Op::JumpNullish>(
            lhs,
            Bytecode::Label { *rhs_block_ptr },
            Bytecode::Label { *lhs_block_ptr });
    }

    if (rhs_block_ptr)
        generator.switch_to_basic_block(*rhs_block_ptr);

    auto rhs = [&]() -> ScopedOperand {
        if (lhs_expression->is_identifier()) {
            return generator.emit_named_evaluation_if_anonymous_function(*m_rhs, generator.intern_identifier(static_cast<Identifier const&>(*lhs_expression).string()));
        }
        return m_rhs->generate_bytecode(generator).value();
    }();

    // OPTIMIZATION: If LHS is a local, we can write the result directly into it.
    auto dst = [&] {
        if (lhs.operand().is_local())
            return lhs;
        return choose_dst(generator, preferred_dst);
    }();

    switch (m_op) {
    case AssignmentOp::AdditionAssignment:
        generator.emit<Bytecode::Op::Add>(dst, lhs, rhs);
        break;
    case AssignmentOp::SubtractionAssignment:
        generator.emit<Bytecode::Op::Sub>(dst, lhs, rhs);
        break;
    case AssignmentOp::MultiplicationAssignment:
        generator.emit<Bytecode::Op::Mul>(dst, lhs, rhs);
        break;
    case AssignmentOp::DivisionAssignment:
        generator.emit<Bytecode::Op::Div>(dst, lhs, rhs);
        break;
    case AssignmentOp::ModuloAssignment:
        generator.emit<Bytecode::Op::Mod>(dst, lhs, rhs);
        break;
    case AssignmentOp::ExponentiationAssignment:
        generator.emit<Bytecode::Op::Exp>(dst, lhs, rhs);
        break;
    case AssignmentOp::BitwiseAndAssignment:
        generator.emit<Bytecode::Op::BitwiseAnd>(dst, lhs, rhs);
        break;
    case AssignmentOp::BitwiseOrAssignment:
        generator.emit<Bytecode::Op::BitwiseOr>(dst, lhs, rhs);
        break;
    case AssignmentOp::BitwiseXorAssignment:
        generator.emit<Bytecode::Op::BitwiseXor>(dst, lhs, rhs);
        break;
    case AssignmentOp::LeftShiftAssignment:
        generator.emit<Bytecode::Op::LeftShift>(dst, lhs, rhs);
        break;
    case AssignmentOp::RightShiftAssignment:
        generator.emit<Bytecode::Op::RightShift>(dst, lhs, rhs);
        break;
    case AssignmentOp::UnsignedRightShiftAssignment:
        generator.emit<Bytecode::Op::UnsignedRightShift>(dst, lhs, rhs);
        break;
    case AssignmentOp::AndAssignment:
    case AssignmentOp::OrAssignment:
    case AssignmentOp::NullishAssignment:
        generator.emit_mov(dst, rhs);
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    if (lhs_expression->is_identifier())
        generator.emit_set_variable(static_cast<Identifier const&>(*lhs_expression), dst);
    else
        generator.emit_store_to_reference(reference_operands, dst);

    if (rhs_block_ptr) {
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *end_block_ptr });
    }

    if (lhs_block_ptr) {
        generator.switch_to_basic_block(*lhs_block_ptr);
        generator.emit_mov(dst, lhs);
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *end_block_ptr });
    }

    if (end_block_ptr) {
        generator.switch_to_basic_block(*end_block_ptr);
    }

    return dst;
}

// 14.13.3 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-labelled-statements-runtime-semantics-evaluation
//  LabelledStatement : LabelIdentifier : LabelledItem
Optional<ScopedOperand> LabelledStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    // Return ? LabelledEvaluation of this LabelledStatement with argument « ».
    return generate_labelled_evaluation(generator, {});
}

// 14.13.4 Runtime Semantics: LabelledEvaluation, https://tc39.es/ecma262/#sec-runtime-semantics-labelledevaluation
// LabelledStatement : LabelIdentifier : LabelledItem
Optional<ScopedOperand> LabelledStatement::generate_labelled_evaluation(Bytecode::Generator& generator, Vector<FlyString> const& label_set, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    // Convert the m_labelled_item NNRP to a reference early so we don't have to do it every single time we want to use it.
    auto const& labelled_item = *m_labelled_item;

    // 1. Let label be the StringValue of LabelIdentifier.
    // NOTE: Not necessary, this is m_label.

    // 2. Let newLabelSet be the list-concatenation of labelSet and « label ».
    // FIXME: Avoid copy here.
    auto new_label_set = label_set;
    new_label_set.append(m_label);

    // 3. Let stmtResult be LabelledEvaluation of LabelledItem with argument newLabelSet.
    Optional<ScopedOperand> stmt_result;
    if (is<IterationStatement>(labelled_item)) {
        auto const& iteration_statement = static_cast<IterationStatement const&>(labelled_item);
        stmt_result = iteration_statement.generate_labelled_evaluation(generator, new_label_set);
    } else if (is<SwitchStatement>(labelled_item)) {
        auto const& switch_statement = static_cast<SwitchStatement const&>(labelled_item);
        stmt_result = switch_statement.generate_labelled_evaluation(generator, new_label_set);
    } else if (is<LabelledStatement>(labelled_item)) {
        auto const& labelled_statement = static_cast<LabelledStatement const&>(labelled_item);
        stmt_result = labelled_statement.generate_labelled_evaluation(generator, new_label_set);
    } else {
        auto& labelled_break_block = generator.make_block();

        // NOTE: We do not need a continuable scope as `continue;` is not allowed outside of iteration statements, throwing a SyntaxError in the parser.
        generator.begin_breakable_scope(Bytecode::Label { labelled_break_block }, new_label_set);
        stmt_result = labelled_item.generate_bytecode(generator);
        generator.end_breakable_scope();

        if (!generator.is_current_block_terminated()) {
            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { labelled_break_block });
        }

        generator.switch_to_basic_block(labelled_break_block);
    }

    // 4. If stmtResult.[[Type]] is break and SameValue(stmtResult.[[Target]], label) is true, then
    //    a. Set stmtResult to NormalCompletion(stmtResult.[[Value]]).
    // NOTE: These steps are performed by making labelled break jump straight to the appropriate break block, which preserves the statement result's value in the accumulator.

    // 5. Return Completion(stmtResult).
    return stmt_result;
}

Optional<ScopedOperand> WhileStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generate_labelled_evaluation(generator, {});
}

Optional<ScopedOperand> WhileStatement::generate_labelled_evaluation(Bytecode::Generator& generator, Vector<FlyString> const& label_set, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    auto& test_block = generator.make_block();

    Optional<ScopedOperand> completion;
    if (generator.must_propagate_completion()) {
        completion = generator.allocate_register();
        generator.emit_mov(*completion, generator.add_constant(js_undefined()));
    }

    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { test_block });

    generator.switch_to_basic_block(test_block);
    auto test = m_test->generate_bytecode(generator).value();

    // OPTIMIZATION: If predicate is always false, ignore body and exit early
    if (auto constant = generator.try_get_constant(test); constant.has_value() && !constant->to_boolean_slow_case()) {
        return completion;
    }

    // test
    // jump if_false (true) end (false) body
    // body
    // jump always (true) test
    // end
    auto& body_block = generator.make_block();
    auto& end_block = generator.make_block();

    generator.emit_jump_if(
        test,
        Bytecode::Label { body_block },
        Bytecode::Label { end_block });

    generator.switch_to_basic_block(body_block);
    generator.begin_continuable_scope(Bytecode::Label { test_block }, label_set, completion);
    generator.begin_breakable_scope(Bytecode::Label { end_block }, label_set, completion);
    {
        Optional<Bytecode::Generator::CompletionRegisterScope> completion_scope;
        if (completion.has_value())
            completion_scope.emplace(generator, *completion);
        auto body = m_body->generate_bytecode(generator);
        if (!generator.is_current_block_terminated() && completion.has_value() && body.has_value())
            generator.emit_mov(*completion, *body);
    }
    generator.end_breakable_scope();
    generator.end_continuable_scope();

    if (!generator.is_current_block_terminated())
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { test_block });

    generator.switch_to_basic_block(end_block);
    return completion;
}

Optional<ScopedOperand> DoWhileStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generate_labelled_evaluation(generator, {});
}

Optional<ScopedOperand> DoWhileStatement::generate_labelled_evaluation(Bytecode::Generator& generator, Vector<FlyString> const& label_set, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    // jump always (true) body
    // test
    // jump if_false (true) end (false) body
    // body
    // jump always (true) test
    // end
    auto& body_block = generator.make_block();
    auto& test_block = generator.make_block();
    auto& load_result_and_jump_to_end_block = generator.make_block();
    auto& end_block = generator.make_block();

    Optional<ScopedOperand> completion;
    if (generator.must_propagate_completion()) {
        completion = generator.allocate_register();
        generator.emit_mov(*completion, generator.add_constant(js_undefined()));
    }

    // jump to the body block
    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { body_block });

    generator.switch_to_basic_block(test_block);
    auto test = m_test->generate_bytecode(generator).value();
    generator.emit_jump_if(
        test,
        Bytecode::Label { body_block },
        Bytecode::Label { load_result_and_jump_to_end_block });

    generator.switch_to_basic_block(body_block);
    generator.begin_continuable_scope(Bytecode::Label { test_block }, label_set, completion);
    generator.begin_breakable_scope(Bytecode::Label { end_block }, label_set, completion);
    {
        Optional<Bytecode::Generator::CompletionRegisterScope> completion_scope;
        if (completion.has_value())
            completion_scope.emplace(generator, *completion);
        auto body = m_body->generate_bytecode(generator);
        if (!generator.is_current_block_terminated() && completion.has_value() && body.has_value())
            generator.emit_mov(*completion, *body);
    }
    generator.end_breakable_scope();
    generator.end_continuable_scope();

    if (!generator.is_current_block_terminated())
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { test_block });

    generator.switch_to_basic_block(load_result_and_jump_to_end_block);
    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { end_block });

    generator.switch_to_basic_block(end_block);
    return completion;
}

Optional<ScopedOperand> ForStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generate_labelled_evaluation(generator, {});
}

Optional<ScopedOperand> ForStatement::generate_labelled_evaluation(Bytecode::Generator& generator, Vector<FlyString> const& label_set, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    // init
    // jump always (true) test
    // test
    // jump if_true (true) body (false) end
    // body
    // jump always (true) update
    // update
    // jump always (true) test
    // end

    // If 'test' is missing, fuse the 'test' and 'body' basic blocks
    // If 'update' is missing, fuse the 'body' and 'update' basic blocks

    Bytecode::BasicBlock* test_block_ptr { nullptr };
    Bytecode::BasicBlock* body_block_ptr { nullptr };
    Bytecode::BasicBlock* update_block_ptr { nullptr };

    bool has_lexical_environment = false;
    Vector<IdentifierTableIndex> per_iteration_bindings;

    if (m_init) {
        if (m_init->is_variable_declaration()) {
            auto& variable_declaration = as<VariableDeclaration>(*m_init);

            auto has_non_local_variables = false;
            MUST(variable_declaration.for_each_bound_identifier([&](auto const& identifier) {
                if (!identifier.is_local())
                    has_non_local_variables = true;
            }));

            if (variable_declaration.is_lexical_declaration() && has_non_local_variables) {
                has_lexical_environment = true;
                // Setup variable scope for bound identifiers
                generator.begin_variable_scope();

                bool is_const = variable_declaration.is_constant_declaration();
                // NOTE: Nothing in the callback throws an exception.
                MUST(variable_declaration.for_each_bound_identifier([&](auto const& identifier) {
                    if (identifier.is_local())
                        return;
                    auto index = generator.intern_identifier(identifier.string());
                    generator.emit<Bytecode::Op::CreateVariable>(index, Bytecode::Op::EnvironmentMode::Lexical, is_const, false, false);
                    if (!is_const) {
                        per_iteration_bindings.append(index);
                    }
                }));
            }
        }

        (void)m_init->generate_bytecode(generator);
    }

    // CreatePerIterationEnvironment (https://tc39.es/ecma262/multipage/ecmascript-language-statements-and-declarations.html#sec-createperiterationenvironment)
    auto generate_per_iteration_bindings = [&per_iteration_bindings = static_cast<Vector<IdentifierTableIndex> const&>(per_iteration_bindings),
                                               &generator]() {
        if (per_iteration_bindings.is_empty()) {
            return;
        }

        // Copy all the last values into registers for use in step 1.e.iii
        // Register copies of bindings are required since the changing of the
        // running execution context in the final step requires leaving the
        // current variable scope before creating "thisIterationEnv"
        Vector<ScopedOperand> registers;
        for (auto const& binding : per_iteration_bindings) {
            auto reg = generator.allocate_register();
            generator.emit<Bytecode::Op::GetBinding>(reg, binding);
            registers.append(reg);
        }

        generator.end_variable_scope();
        generator.begin_variable_scope();

        for (size_t i = 0; i < per_iteration_bindings.size(); ++i) {
            generator.emit<Bytecode::Op::CreateVariable>(per_iteration_bindings[i], Bytecode::Op::EnvironmentMode::Lexical, false, false, false);
            generator.emit<Bytecode::Op::InitializeLexicalBinding>(per_iteration_bindings[i], registers[i]);
        }
    };

    if (m_init) {
        // CreatePerIterationEnvironment where lastIterationEnv is the variable
        // scope created above for bound identifiers
        generate_per_iteration_bindings();
    }

    body_block_ptr = &generator.make_block();

    if (m_update)
        update_block_ptr = &generator.make_block();
    else
        update_block_ptr = body_block_ptr;

    if (m_test)
        test_block_ptr = &generator.make_block();
    else
        test_block_ptr = body_block_ptr;

    auto& end_block = generator.make_block();

    Optional<ScopedOperand> completion;
    if (generator.must_propagate_completion()) {
        completion = generator.allocate_register();
        generator.emit_mov(*completion, generator.add_constant(js_undefined()));
    }

    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *test_block_ptr });

    if (m_test) {
        generator.switch_to_basic_block(*test_block_ptr);

        auto test = m_test->generate_bytecode(generator).value();

        // OPTIMIZATION: test value is always falsey, skip body entirely
        if (auto constant = generator.try_get_constant(test); constant.has_value() && !constant->to_boolean_slow_case()) {
            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { end_block });
            generator.switch_to_basic_block(end_block);
            if (has_lexical_environment)
                generator.end_variable_scope();
            return completion;
        }

        generator.emit_jump_if(test, Bytecode::Label { *body_block_ptr }, Bytecode::Label { end_block });
    }

    if (m_update) {
        generator.switch_to_basic_block(*update_block_ptr);

        (void)m_update->generate_bytecode(generator);
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *test_block_ptr });
    }

    generator.switch_to_basic_block(*body_block_ptr);
    generator.begin_continuable_scope(Bytecode::Label { m_update ? *update_block_ptr : *test_block_ptr }, label_set, completion);
    generator.begin_breakable_scope(Bytecode::Label { end_block }, label_set, completion);
    {
        Optional<Bytecode::Generator::CompletionRegisterScope> completion_scope;
        if (completion.has_value())
            completion_scope.emplace(generator, *completion);
        auto body = m_body->generate_bytecode(generator);
        if (!generator.is_current_block_terminated() && completion.has_value() && body.has_value())
            generator.emit_mov(*completion, *body);
    }
    generator.end_breakable_scope();
    generator.end_continuable_scope();

    if (!generator.is_current_block_terminated()) {
        // CreatePerIterationEnvironment where lastIterationEnv is the environment
        // created by the previous CreatePerIterationEnvironment setup
        generate_per_iteration_bindings();

        if (m_update) {
            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *update_block_ptr });
        } else {
            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *test_block_ptr });
        }
    }

    generator.switch_to_basic_block(end_block);

    // Leave the environment setup by CreatePerIterationEnvironment or if there
    // are no perIterationBindings the variable scope created for bound
    // identifiers
    if (has_lexical_environment)
        generator.end_variable_scope();

    return completion;
}

Optional<ScopedOperand> ObjectExpression::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    auto object = choose_dst(generator, preferred_dst);

    // Determine if this is a simple object literal (all KeyValue with StringLiteral keys
    // that are not numeric indices). Simple literals can benefit from shape caching with
    // direct property offset writes. Numeric string keys like "0" are stored in indexed
    // storage rather than shape-based storage, so they can't use the fast path.
    bool is_simple = !m_properties.is_empty();
    for (auto& property : m_properties) {
        if (property->type() != ObjectProperty::Type::KeyValue || !is<StringLiteral>(property->key())) {
            is_simple = false;
            break;
        }
        // Check if the key is a numeric index (would be stored in indexed storage)
        auto const& key = static_cast<StringLiteral const&>(property->key()).value();
        if (!key.is_empty() && !(key.code_unit_at(0) == '0' && key.length_in_code_units() > 1)) {
            auto property_index = key.to_number<u32>(TrimWhitespace::No);
            if (property_index.has_value() && property_index.value() < NumericLimits<u32>::max()) {
                is_simple = false;
                break;
            }
        }
    }

    Optional<u32> shape_cache_index;
    if (is_simple)
        shape_cache_index = generator.next_object_shape_cache();

    generator.emit<Bytecode::Op::NewObject>(object, shape_cache_index.value_or(NumericLimits<u32>::max()));

    if (m_properties.is_empty())
        return object;

    generator.push_home_object(object);

    u32 property_slot = 0;
    for (auto& property : m_properties) {
        Bytecode::PutKind property_kind;
        switch (property->type()) {
        case ObjectProperty::Type::KeyValue:
            property_kind = Bytecode::PutKind::Own;
            break;
        case ObjectProperty::Type::Getter:
            property_kind = Bytecode::PutKind::Getter;
            break;
        case ObjectProperty::Type::Setter:
            property_kind = Bytecode::PutKind::Setter;
            break;
        case ObjectProperty::Type::ProtoSetter:
            property_kind = Bytecode::PutKind::Prototype;
            break;
        case ObjectProperty::Type::Spread:
            generator.emit<Bytecode::Op::PutBySpread>(object, property->key().generate_bytecode(generator).value());
            continue;
        }

        if (is<StringLiteral>(property->key())) {
            auto& string_literal = static_cast<StringLiteral const&>(property->key());

            Optional<ScopedOperand> value;
            if (property_kind == Bytecode::PutKind::Prototype) {
                value = property->value().generate_bytecode(generator).value();
            } else {
                auto identifier = string_literal.value();
                if (property_kind == Bytecode::PutKind::Getter)
                    identifier = Utf16String::formatted("get {}", identifier);
                else if (property_kind == Bytecode::PutKind::Setter)
                    identifier = Utf16String::formatted("set {}", identifier);

                auto name = generator.intern_identifier(identifier);
                value = generator.emit_named_evaluation_if_anonymous_function(property->value(), name, {}, property->is_method());
            }

            auto property_key_table_index = generator.intern_property_key(string_literal.value());

            // For simple object literals, use InitObjectLiteralProperty for direct offset writes
            if (is_simple) {
                generator.emit<Bytecode::Op::InitObjectLiteralProperty>(object, property_key_table_index, *value, *shape_cache_index, property_slot++);
            } else {
                generator.emit_put_by_id(object, property_key_table_index, *value, property_kind, generator.next_property_lookup_cache());
            }
        } else {
            auto property_name = property->key().generate_bytecode(generator).value();

            // ComputedPropertyName evaluation calls ToPropertyKey, which includes ToPrimitive(hint: string).
            // This must happen before the value expression is evaluated per the spec for
            // PropertyDefinitionEvaluation (PropertyDefinition : PropertyName : AssignmentExpression):
            //   1. Let propKey be ? Evaluation of PropertyName.
            //   [then] 5/6. Evaluate the AssignmentExpression.
            // ToPrimitive is the only step in ToPropertyKey with user-observable side effects.
            // After this, the ToPrimitive inside put_by_value's to_property_key is a no-op.
            generator.emit<Bytecode::Op::ToPrimitiveWithStringHint>(property_name, property_name);

            auto value = generator.emit_named_evaluation_if_anonymous_function(property->value(), {}, {}, property->is_method());

            generator.emit_put_by_value(object, property_name, value, property_kind, {});
        }
    }

    generator.pop_home_object();

    if (shape_cache_index.has_value())
        generator.emit<Bytecode::Op::CacheObjectShape>(object, *shape_cache_index);

    return object;
}

Optional<ScopedOperand> ArrayExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    if (m_elements.is_empty()) {
        auto dst = choose_dst(generator, preferred_dst);
        generator.emit<Bytecode::Op::NewArray>(dst, ReadonlySpan<ScopedOperand> {});
        return dst;
    }

    if (all_of(m_elements, [](auto element) { return !element || is<PrimitiveLiteral>(*element); })) {
        // If all elements are constant primitives, we can just emit a single instruction to initialize the array,
        // instead of emitting instructions to manually evaluate them one-by-one
        Vector<Value> values;
        values.resize_with_default_value(m_elements.size(), js_special_empty_value());
        for (auto i = 0u; i < m_elements.size(); ++i) {
            if (!m_elements[i])
                continue;
            values[i] = static_cast<PrimitiveLiteral const&>(*m_elements[i]).value();
        }
        auto dst = choose_dst(generator, preferred_dst);
        generator.emit_with_extra_value_slots<Bytecode::Op::NewPrimitiveArray>(values.size(), dst, values);
        return dst;
    }

    auto first_spread = find_if(m_elements.begin(), m_elements.end(), [](auto el) { return el && is<SpreadExpression>(*el); });

    Vector<ScopedOperand> args;
    args.ensure_capacity(m_elements.size());
    for (auto it = m_elements.begin(); it != first_spread; ++it) {
        if (*it) {
            auto value = (*it)->generate_bytecode(generator).value();
            args.append(generator.copy_if_needed_to_preserve_evaluation_order(value));
        } else {
            args.append(generator.add_constant(js_special_empty_value()));
        }
    }

    auto dst = choose_dst(generator, preferred_dst);
    if (first_spread.index() != 0) {
        generator.emit_with_extra_operand_slots<Bytecode::Op::NewArray>(args.size(), dst, args);
    } else {
        generator.emit<Bytecode::Op::NewArray>(dst, ReadonlySpan<ScopedOperand> {});
    }

    if (first_spread != m_elements.end()) {
        for (auto it = first_spread; it != m_elements.end(); ++it) {
            if (!*it) {
                generator.emit<Bytecode::Op::ArrayAppend>(dst, generator.add_constant(js_special_empty_value()), false);
            } else {
                auto value = (*it)->generate_bytecode(generator).value();
                generator.emit<Bytecode::Op::ArrayAppend>(dst, value, *it && is<SpreadExpression>(**it));
            }
        }
    }

    return dst;
}

Optional<ScopedOperand> MemberExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto reference = generator.emit_load_from_reference(*this, preferred_dst);
    return reference.loaded_value;
}

Optional<ScopedOperand> FunctionDeclaration::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    if (m_is_hoisted) {
        Bytecode::Generator::SourceLocationScope scope(generator, *this);
        auto index = generator.intern_identifier(name());
        auto value = generator.allocate_register();
        generator.emit<Bytecode::Op::GetBinding>(value, index);
        generator.emit<Bytecode::Op::SetVariableBinding>(index, value);
    }
    return Optional<ScopedOperand> {};
}

Optional<ScopedOperand> FunctionExpression::generate_bytecode_with_lhs_name(Bytecode::Generator& generator, Optional<Bytecode::IdentifierTableIndex> lhs_name, Optional<ScopedOperand> preferred_dst, bool is_method) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    bool has_name = !name().is_empty();
    Optional<Bytecode::IdentifierTableIndex> name_identifier;

    if (has_name) {
        generator.begin_variable_scope();

        name_identifier = generator.intern_identifier(name());
        generator.emit<Bytecode::Op::CreateVariable>(*name_identifier, Bytecode::Op::EnvironmentMode::Lexical, true, false, false);
    }

    auto new_function = choose_dst(generator, preferred_dst);
    generator.emit_new_function(new_function, *this, lhs_name, is_method);

    if (has_name) {
        generator.emit<Bytecode::Op::InitializeLexicalBinding>(*name_identifier, new_function);
        generator.end_variable_scope();
    }

    return new_function;
}

Optional<ScopedOperand> FunctionExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generate_bytecode_with_lhs_name(generator, {}, preferred_dst);
}

static void generate_object_binding_pattern_bytecode(Bytecode::Generator& generator, BindingPattern const& pattern, Bytecode::Op::BindingInitializationMode initialization_mode, ScopedOperand const& object)
{
    generator.emit<Bytecode::Op::ThrowIfNullish>(object);

    Vector<ScopedOperand> excluded_property_names;
    auto has_rest = false;
    if (pattern.entries.size() > 0)
        has_rest = pattern.entries[pattern.entries.size() - 1].is_rest;

    for (auto& [name, alias, initializer, is_rest] : pattern.entries) {
        if (is_rest) {
            VERIFY(!initializer);
            if (name.has<NonnullRefPtr<Identifier const>>()) {
                auto identifier = name.get<NonnullRefPtr<Identifier const>>();

                auto copy = generator.allocate_register();
                generator.emit_with_extra_operand_slots<Bytecode::Op::CopyObjectExcludingProperties>(
                    excluded_property_names.size(), copy, object, excluded_property_names);
                generator.emit_set_variable(*identifier, copy, initialization_mode);

                return;
            }
            if (alias.has<NonnullRefPtr<MemberExpression const>>()) {
                auto copy = generator.allocate_register();
                generator.emit_with_extra_operand_slots<Bytecode::Op::CopyObjectExcludingProperties>(
                    excluded_property_names.size(), copy, object, excluded_property_names);
                generator.emit_store_to_reference(alias.get<NonnullRefPtr<MemberExpression const>>(), copy);
                return;
            }
            VERIFY_NOT_REACHED();
        }

        auto value = generator.allocate_register();

        if (name.has<NonnullRefPtr<Identifier const>>()) {
            auto const& identifier = name.get<NonnullRefPtr<Identifier const>>()->string();
            if (has_rest) {
                excluded_property_names.append(generator.add_constant(PrimitiveString::create(generator.vm(), identifier)));
            }
            generator.emit_get_by_id(value, object, generator.intern_property_key(identifier));
        } else {
            auto expression = name.get<NonnullRefPtr<Expression const>>();
            auto property_name = expression->generate_bytecode(generator).value();

            if (has_rest) {
                auto excluded_name = generator.copy_if_needed_to_preserve_evaluation_order(property_name);
                excluded_property_names.append(excluded_name);
            }

            generator.emit_get_by_value(value, object, property_name);
        }

        if (initializer) {
            auto& if_undefined_block = generator.make_block();
            auto& if_not_undefined_block = generator.make_block();

            generator.emit<Bytecode::Op::JumpUndefined>(
                value,
                Bytecode::Label { if_undefined_block },
                Bytecode::Label { if_not_undefined_block });

            generator.switch_to_basic_block(if_undefined_block);
            Optional<ScopedOperand> default_value;
            if (auto const* alias_identifier = alias.get_pointer<NonnullRefPtr<Identifier const>>()) {
                default_value = generator.emit_named_evaluation_if_anonymous_function(*initializer, generator.intern_identifier((*alias_identifier)->string()));
            } else if (auto const* lhs = name.get_pointer<NonnullRefPtr<Identifier const>>()) {
                default_value = generator.emit_named_evaluation_if_anonymous_function(*initializer, generator.intern_identifier((*lhs)->string()));
            } else {
                default_value = initializer->generate_bytecode(generator).value();
            }
            generator.emit_mov(value, *default_value);
            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { if_not_undefined_block });

            generator.switch_to_basic_block(if_not_undefined_block);
        }

        if (alias.has<NonnullRefPtr<BindingPattern const>>()) {
            auto& binding_pattern = *alias.get<NonnullRefPtr<BindingPattern const>>();
            auto nested_value = generator.copy_if_needed_to_preserve_evaluation_order(value);
            binding_pattern.generate_bytecode(generator, initialization_mode, nested_value);
        } else if (alias.has<Empty>()) {
            // NB: Computed property names always require an alias, so name can't be an Expression here.
            VERIFY(!name.has<NonnullRefPtr<Expression const>>());

            auto const& identifier = *name.get<NonnullRefPtr<Identifier const>>();
            generator.emit_set_variable(identifier, value, initialization_mode);
        } else if (alias.has<NonnullRefPtr<MemberExpression const>>()) {
            generator.emit_store_to_reference(alias.get<NonnullRefPtr<MemberExpression const>>(), value);
        } else {
            auto const& identifier = *alias.get<NonnullRefPtr<Identifier const>>();
            generator.emit_set_variable(identifier, value, initialization_mode);
        }
    }
}

static void generate_array_binding_pattern_bytecode(Bytecode::Generator& generator, BindingPattern const& pattern, Bytecode::Op::BindingInitializationMode initialization_mode, ScopedOperand const& input_array, [[maybe_unused]] Optional<ScopedOperand> preferred_dst = {})
{
    /*
     * Consider the following destructuring assignment:
     *
     *     let [a, b, c, d, e] = o;
     *
     * It would be fairly trivial to just loop through this iterator, getting the value
     * at each step and assigning them to the binding sequentially. However, this is not
     * correct: once an iterator is exhausted, it must not be called again. This complicates
     * the bytecode. In order to accomplish this, we do the following:
     *
     * - Reserve a special boolean register which holds 'true' if the iterator is exhausted,
     *   and false otherwise
     * - When we are retrieving the value which should be bound, we first check this register.
     *   If it is 'true', we load undefined. Otherwise, we grab the next value from the iterator.
     *
     * Note that the is_exhausted register does not need to be loaded with false because the
     * first IteratorNext bytecode is _not_ proceeded by an exhausted check, as it is
     * unnecessary.
     */

    auto is_iterator_exhausted = generator.allocate_register();
    generator.emit_mov(is_iterator_exhausted, generator.add_constant(Value(false)));

    auto iterator_object = generator.allocate_register();
    auto iterator_next_method = generator.allocate_register();
    auto iterator_done_property = generator.allocate_register();
    generator.emit<Bytecode::Op::GetIterator>(iterator_object, iterator_next_method, iterator_done_property, input_array, IteratorHint::Sync);
    bool first = true;

    auto assign_value_to_alias = [&](auto& alias, ScopedOperand value) {
        return alias.visit(
            [&](Empty) -> void {
                // This element is an elision
            },
            [&](NonnullRefPtr<Identifier const> const& identifier) -> void {
                generator.emit_set_variable(*identifier, value, initialization_mode);
            },
            [&](NonnullRefPtr<BindingPattern const> const& pattern) -> void {
                pattern->generate_bytecode(generator, initialization_mode, value);
            },
            [&](NonnullRefPtr<MemberExpression const> const& expr) -> void {
                generator.emit_store_to_reference(*expr, value);
            });
    };

    auto temp_iterator_result = generator.allocate_register();

    for (auto& [name, alias, initializer, is_rest] : pattern.entries) {
        VERIFY(name.has<Empty>());

        if (is_rest) {
            VERIFY(!initializer);

            auto value = generator.allocate_register();

            if (first) {
                // The iterator has not been called, and is thus known to be not exhausted
                generator.emit<Bytecode::Op::IteratorToArray>(value, iterator_object, iterator_next_method, iterator_done_property);
            } else {
                auto& if_exhausted_block = generator.make_block();
                auto& if_not_exhausted_block = generator.make_block();
                auto& continuation_block = generator.make_block();

                generator.emit_jump_if(
                    is_iterator_exhausted,
                    Bytecode::Label { if_exhausted_block },
                    Bytecode::Label { if_not_exhausted_block });

                value = generator.allocate_register();

                generator.switch_to_basic_block(if_exhausted_block);
                generator.emit<Bytecode::Op::NewArray>(value, ReadonlySpan<ScopedOperand> {});
                generator.emit<Bytecode::Op::Jump>(Bytecode::Label { continuation_block });

                generator.switch_to_basic_block(if_not_exhausted_block);
                generator.emit<Bytecode::Op::IteratorToArray>(value, iterator_object, iterator_next_method, iterator_done_property);
                generator.emit<Bytecode::Op::Jump>(Bytecode::Label { continuation_block });

                generator.switch_to_basic_block(continuation_block);
            }

            return assign_value_to_alias(alias, value);
        }

        auto& iterator_is_exhausted_block = generator.make_block();

        if (!first) {
            auto& iterator_is_not_exhausted_block = generator.make_block();

            generator.emit_jump_if(
                is_iterator_exhausted,
                Bytecode::Label { iterator_is_exhausted_block },
                Bytecode::Label { iterator_is_not_exhausted_block });

            generator.switch_to_basic_block(iterator_is_not_exhausted_block);
        }

        auto value = generator.allocate_register();
        generator.emit<Bytecode::Op::IteratorNextUnpack>(value, is_iterator_exhausted, iterator_object, iterator_next_method, iterator_done_property);

        // We still have to check for exhaustion here. If the iterator is exhausted,
        // we need to bail before trying to get the value
        auto& no_bail_block = generator.make_block();
        generator.emit_jump_if(
            is_iterator_exhausted,
            Bytecode::Label { iterator_is_exhausted_block },
            Bytecode::Label { no_bail_block });

        generator.switch_to_basic_block(no_bail_block);

        auto& create_binding_block = generator.make_block();
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { create_binding_block });

        // The iterator is exhausted, so we just load undefined and continue binding
        generator.switch_to_basic_block(iterator_is_exhausted_block);
        generator.emit_mov(value, generator.add_constant(js_undefined()));
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { create_binding_block });

        generator.switch_to_basic_block(create_binding_block);

        if (initializer) {
            auto& value_is_undefined_block = generator.make_block();
            auto& value_is_not_undefined_block = generator.make_block();

            generator.emit<Bytecode::Op::JumpUndefined>(
                value,
                Bytecode::Label { value_is_undefined_block },
                Bytecode::Label { value_is_not_undefined_block });

            generator.switch_to_basic_block(value_is_undefined_block);

            Optional<ScopedOperand> default_value;
            if (auto const* alias_identifier = alias.get_pointer<NonnullRefPtr<Identifier const>>()) {
                default_value = generator.emit_named_evaluation_if_anonymous_function(*initializer, generator.intern_identifier((*alias_identifier)->string()));
            } else if (auto const* name_identifier = name.get_pointer<NonnullRefPtr<Identifier const>>()) {
                default_value = generator.emit_named_evaluation_if_anonymous_function(*initializer, generator.intern_identifier((*name_identifier)->string()));
            } else {
                default_value = initializer->generate_bytecode(generator).value();
            }
            generator.emit_mov(value, *default_value);
            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { value_is_not_undefined_block });

            generator.switch_to_basic_block(value_is_not_undefined_block);
        }

        assign_value_to_alias(alias, value);

        first = false;
    }

    auto& done_block = generator.make_block();
    auto& not_done_block = generator.make_block();

    generator.emit_jump_if(
        is_iterator_exhausted,
        Bytecode::Label { done_block },
        Bytecode::Label { not_done_block });

    generator.switch_to_basic_block(not_done_block);
    generator.emit<Bytecode::Op::IteratorClose>(iterator_object, iterator_next_method, iterator_done_property, Completion::Type::Normal, generator.add_constant(js_undefined()));
    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { done_block });

    generator.switch_to_basic_block(done_block);
}

void BindingPattern::generate_bytecode(Bytecode::Generator& generator, Bytecode::Op::BindingInitializationMode initialization_mode, ScopedOperand const& input_value) const
{
    if (kind == Kind::Object)
        return generate_object_binding_pattern_bytecode(generator, *this, initialization_mode, input_value);

    return generate_array_binding_pattern_bytecode(generator, *this, initialization_mode, input_value);
}

static void assign_value_to_variable_declarator(Bytecode::Generator& generator, VariableDeclarator const& declarator, VariableDeclaration const& declaration, ScopedOperand value)
{
    auto initialization_mode = declaration.is_lexical_declaration() ? Bytecode::Op::BindingInitializationMode::Initialize : Bytecode::Op::BindingInitializationMode::Set;

    declarator.target().visit(
        [&](NonnullRefPtr<Identifier const> const& id) -> void {
            generator.emit_set_variable(*id, value, initialization_mode);
        },
        [&](NonnullRefPtr<BindingPattern const> const& pattern) -> void {
            pattern->generate_bytecode(generator, initialization_mode, value);
        });
}

Optional<ScopedOperand> VariableDeclaration::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    for (auto& declarator : m_declarations) {
        // NOTE: `var` declarations can have duplicates, but duplicate `let` or `const` bindings are a syntax error.
        //       Because of this, we can sink `let` and `const` directly into the preferred_dst if available.
        //       This is not safe for `var` since the preferred_dst may be used in the initializer.
        Optional<ScopedOperand> init_dst;
        if (declaration_kind() != DeclarationKind::Var) {
            if (auto const* identifier = declarator->target().get_pointer<NonnullRefPtr<Identifier const>>()) {
                if ((*identifier)->is_local()) {
                    init_dst = generator.local((*identifier)->local_index());
                }
            }
        }

        if (declarator->init()) {
            auto value = [&]() -> ScopedOperand {
                if (auto const* lhs = declarator->target().get_pointer<NonnullRefPtr<Identifier const>>()) {
                    return generator.emit_named_evaluation_if_anonymous_function(*declarator->init(), generator.intern_identifier((*lhs)->string()), init_dst);
                } else {
                    return declarator->init()->generate_bytecode(generator, init_dst).value();
                }
            }();
            assign_value_to_variable_declarator(generator, declarator, *this, value);
        } else if (m_declaration_kind != DeclarationKind::Var) {
            assign_value_to_variable_declarator(generator, declarator, *this, generator.add_constant(js_undefined()));
        }

        if (auto const* identifier = declarator->target().get_pointer<NonnullRefPtr<Identifier const>>()) {
            if ((*identifier)->is_local()) {
                generator.set_local_initialized((*identifier)->local_index());
            }
        }
    }

    // NOTE: VariableDeclaration doesn't return a completion value.
    return Optional<ScopedOperand> {};
}

struct BaseAndValue {
    ScopedOperand base;
    ScopedOperand value;
};

static BaseAndValue get_base_and_value_from_member_expression(Bytecode::Generator& generator, MemberExpression const& member_expression)
{
    // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
    if (is<SuperExpression>(member_expression.object())) {
        // 1. Let env be GetThisEnvironment().
        // 2. Let actualThis be ? env.GetThisBinding().
        auto this_value = generator.get_this();

        Optional<ScopedOperand> computed_property;

        if (member_expression.is_computed()) {
            // SuperProperty : super [ Expression ]
            // 3. Let propertyNameReference be ? Evaluation of Expression.
            // 4. Let propertyNameValue be ? GetValue(propertyNameReference).
            computed_property = member_expression.property().generate_bytecode(generator);
        }

        // 5/7. Return ? MakeSuperPropertyReference(actualThis, propertyKey, strict).

        // https://tc39.es/ecma262/#sec-makesuperpropertyreference
        // 1. Let env be GetThisEnvironment().
        // 2. Assert: env.HasSuperBinding() is true.
        // 3. Let baseValue be ? env.GetSuperBase().
        auto super_base = generator.allocate_register();
        generator.emit<Bytecode::Op::ResolveSuperBase>(super_base);

        auto value = generator.allocate_register();

        // 4. Return the Reference Record { [[Base]]: baseValue, [[ReferencedName]]: propertyKey, [[Strict]]: strict, [[ThisValue]]: actualThis }.
        if (computed_property.has_value()) {
            // 5. Let propertyKey be ? ToPropertyKey(propertyNameValue).
            generator.emit_get_by_value_with_this(value, super_base, *computed_property, this_value);
        } else {
            // 3. Let propertyKey be StringValue of IdentifierName.
            auto property_key_table_index = generator.intern_property_key(as<Identifier>(member_expression.property()).string());
            generator.emit_get_by_id_with_this(value, super_base, property_key_table_index, this_value);
        }

        return BaseAndValue { this_value, value };
    }

    auto base = member_expression.object().generate_bytecode(generator).value();
    auto value = generator.allocate_register();
    if (member_expression.is_computed()) {
        auto property = member_expression.property().generate_bytecode(generator).value();
        generator.emit_get_by_value(value, base, property);
    } else if (is<PrivateIdentifier>(member_expression.property())) {
        generator.emit<Bytecode::Op::GetPrivateById>(
            value,
            base,
            generator.intern_identifier(as<PrivateIdentifier>(member_expression.property()).string()));
    } else {
        auto base_identifier = generator.intern_identifier_for_expression(member_expression.object());
        generator.emit_get_by_id(value, base, generator.intern_property_key(as<Identifier>(member_expression.property()).string()), move(base_identifier));
    }

    return BaseAndValue { base, value };
}

static void generate_optional_chain(Bytecode::Generator& generator, OptionalChain const& optional_chain, ScopedOperand current_value, ScopedOperand current_base, [[maybe_unused]] Optional<ScopedOperand> preferred_dst = {});

Optional<ScopedOperand> CallExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    Optional<Bytecode::Builtin> builtin;

    Optional<ScopedOperand> original_callee;
    auto original_this_value = generator.add_constant(js_undefined());
    auto dst = choose_dst(generator, preferred_dst);
    Bytecode::Op::CallType call_type = Bytecode::Op::CallType::Call;

    if (is<NewExpression>(this)) {
        original_callee = m_callee->generate_bytecode(generator).value();
        call_type = Bytecode::Op::CallType::Construct;
    } else if (is<MemberExpression>(*m_callee)) {
        auto& member_expression = static_cast<MemberExpression const&>(*m_callee);
        auto base_and_value = get_base_and_value_from_member_expression(generator, member_expression);
        original_callee = base_and_value.value;
        original_this_value = base_and_value.base;
        builtin = Bytecode::get_builtin(member_expression);
    } else if (is<OptionalChain>(*m_callee)) {
        auto& optional_chain = static_cast<OptionalChain const&>(*m_callee);
        original_callee = generator.allocate_register();
        original_this_value = generator.allocate_register();
        generate_optional_chain(generator, optional_chain, *original_callee, original_this_value);
    } else if (is<Identifier>(*m_callee)) {
        // If the original_callee is an identifier, we may need to extract a `this` value.
        // This is important when we're inside a `with` statement and calling a method on
        // the environment's binding object.
        // NOTE: If the identifier refers to a known "local" or "global", we know it can't be
        //       a `with` binding, so we can skip this.
        auto& identifier = static_cast<Identifier const&>(*m_callee);
        if (generator.builtin_abstract_operations_enabled() && identifier.is_global()) {
            generator.generate_builtin_abstract_operation(identifier, arguments(), dst);
            return dst;
        }

        if (identifier.string() == "eval"sv) {
            call_type = Bytecode::Op::CallType::DirectEval;
        }
        if (identifier.is_local()) {
            auto local = generator.local(identifier.local_index());
            if (!generator.is_local_initialized(local.operand().index())) {
                generator.emit<Bytecode::Op::ThrowIfTDZ>(local);
            }
            original_callee = local;
        } else if (identifier.is_global()) {
            original_callee = m_callee->generate_bytecode(generator).value();
        } else {
            original_callee = generator.allocate_register();
            original_this_value = generator.allocate_register();
            generator.emit<Bytecode::Op::GetCalleeAndThisFromEnvironment>(
                *original_callee,
                original_this_value,
                generator.intern_identifier(identifier.string()));
        }
    } else {
        // NB: For non-Reference calls, EvaluateCall sets thisValue to undefined.
        //     OrdinaryCallBindThis coerces undefined to the global object in sloppy mode at runtime.
        original_callee = m_callee->generate_bytecode(generator).value();
    }

    // NOTE: If the callee/this value isn't already a temporary, we copy them to new registers
    //       to avoid overwriting them while evaluating arguments.
    // Example: foo.bar(Object.getPrototypeOf(foo).bar = null, foo = null)
    auto this_value = generator.copy_if_needed_to_preserve_evaluation_order(original_this_value);
    auto callee = generator.copy_if_needed_to_preserve_evaluation_order(original_callee.value());

    Optional<Bytecode::StringTableIndex> expression_string_index;
    if (auto expression_string = this->expression_string(); expression_string.has_value())
        expression_string_index = generator.intern_string(expression_string.release_value());

    bool has_spread = any_of(arguments(), [](auto& argument) { return argument.is_spread; });

    if (has_spread) {
        auto arguments = arguments_to_array_for_call(generator, this->arguments()).value();
        if (call_type == Op::CallType::Construct) {
            generator.emit<Bytecode::Op::CallConstructWithArgumentArray>(dst, callee, this_value, arguments, expression_string_index);
        } else if (call_type == Op::CallType::DirectEval) {
            generator.emit<Bytecode::Op::CallDirectEvalWithArgumentArray>(dst, callee, this_value, arguments, expression_string_index);
        } else {
            generator.emit<Bytecode::Op::CallWithArgumentArray>(dst, callee, this_value, arguments, expression_string_index);
        }
    } else {
        Vector<ScopedOperand> argument_operands;
        argument_operands.ensure_capacity(arguments().size());
        for (auto const& argument : arguments()) {
            auto argument_value = argument.value->generate_bytecode(generator).value();
            argument_operands.append(generator.copy_if_needed_to_preserve_evaluation_order(argument_value));
        }
        if (builtin.has_value() && builtin_argument_count(builtin.value()) == argument_operands.size()) {
            VERIFY(call_type == Op::CallType::Call);
            generator.emit_with_extra_operand_slots<Bytecode::Op::CallBuiltin>(
                argument_operands.size(),
                dst,
                callee,
                this_value,
                builtin.value(),
                expression_string_index,
                argument_operands);
        } else if (call_type == Op::CallType::Construct) {
            generator.emit_with_extra_operand_slots<Bytecode::Op::CallConstruct>(
                argument_operands.size(),
                dst,
                callee,
                expression_string_index,
                argument_operands);
        } else if (call_type == Op::CallType::DirectEval) {
            generator.emit_with_extra_operand_slots<Bytecode::Op::CallDirectEval>(
                argument_operands.size(),
                dst,
                callee,
                this_value,
                expression_string_index,
                argument_operands);
        } else {
            generator.emit_with_extra_operand_slots<Bytecode::Op::Call>(
                argument_operands.size(),
                dst,
                callee,
                this_value,
                expression_string_index,
                argument_operands);
        }
    }

    return dst;
}

static ScopedOperand generate_await(
    Bytecode::Generator& generator,
    ScopedOperand argument,
    ScopedOperand received_completion,
    ScopedOperand received_completion_type,
    ScopedOperand received_completion_value);

// https://tc39.es/ecma262/#sec-return-statement-runtime-semantics-evaluation
Optional<ScopedOperand> ReturnStatement::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand>) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    Optional<ScopedOperand> return_value;

    if (m_argument) {
        //  ReturnStatement : return Expression ;
        //     1. Let exprRef be ? Evaluation of Expression.
        //     2. Let exprValue be ? GetValue(exprRef).
        return_value = m_argument->generate_bytecode(generator).value();

        //     3. If GetGeneratorKind() is async, set exprValue to ? Await(exprValue).
        // Spec Issue?: The spec doesn't seem to do implicit await on explicit return for async functions, but does for
        //              async generators. However, the major engines do so, and this is observable via constructor lookups
        //              on Promise objects and custom thenables.
        //              See: https://tc39.es/ecma262/#sec-asyncblockstart
        //              c. Assert: If we return here, the async function either threw an exception or performed an implicit or explicit return; all awaiting is done.
        if (generator.is_in_async_function()) {
            auto received_completion = generator.allocate_register();
            auto received_completion_type = generator.allocate_register();
            auto received_completion_value = generator.allocate_register();
            return_value = generate_await(generator, *return_value, received_completion, received_completion_type, received_completion_value);
        }

        //     4. Return Completion Record { [[Type]]: return, [[Value]]: exprValue, [[Target]]: empty }.
    } else {
        //  ReturnStatement : return ;
        //    1. Return Completion Record { [[Type]]: return, [[Value]]: undefined, [[Target]]: empty }.
        return_value = generator.add_constant(js_undefined());
    }

    if (generator.is_in_generator_or_async_function())
        generator.emit_return<Bytecode::Op::Yield>(return_value.value());
    else
        generator.emit_return<Bytecode::Op::Return>(return_value.value());

    return return_value;
}

static void get_received_completion_type_and_value(
    Bytecode::Generator& generator,
    ScopedOperand received_completion,
    ScopedOperand received_completion_type,
    ScopedOperand received_completion_value)
{
    generator.emit<Op::GetCompletionFields>(received_completion_type, received_completion_value, received_completion);
}

enum class AwaitBeforeYield {
    No,
    Yes,
};

static void generate_yield(Bytecode::Generator& generator,
    Bytecode::Label continuation_label,
    ScopedOperand argument,
    ScopedOperand received_completion,
    ScopedOperand received_completion_type,
    ScopedOperand received_completion_value,
    AwaitBeforeYield await_before_yield)
{
    if (!generator.is_in_async_generator_function()) {
        generator.emit<Bytecode::Op::Yield>(Bytecode::Label { continuation_label }, argument);
        return;
    }

    if (await_before_yield == AwaitBeforeYield::Yes)
        argument = generate_await(generator, argument, received_completion, received_completion_type, received_completion_value);

    auto& unwrap_yield_resumption_block = generator.make_block();
    generator.emit<Bytecode::Op::Yield>(Bytecode::Label { unwrap_yield_resumption_block }, argument);
    generator.switch_to_basic_block(unwrap_yield_resumption_block);

    generator.emit_mov(received_completion, generator.accumulator());
    get_received_completion_type_and_value(generator, received_completion, received_completion_type, received_completion_value);

    // 27.6.3.7 AsyncGeneratorUnwrapYieldResumption ( resumptionValue ), https://tc39.es/ecma262/#sec-asyncgeneratorunwrapyieldresumption
    // 1. If resumptionValue.[[Type]] is not return, return ? resumptionValue.
    auto& resumption_value_type_is_return_block = generator.make_block();
    auto resumption_value_type_is_not_return_result = generator.allocate_register();
    generator.emit<Bytecode::Op::StrictlyInequals>(
        resumption_value_type_is_not_return_result,
        received_completion_type,
        generator.add_constant(Value(to_underlying(Completion::Type::Return))));
    generator.emit_jump_if(
        resumption_value_type_is_not_return_result,
        Bytecode::Label { continuation_label },
        Bytecode::Label { resumption_value_type_is_return_block });

    generator.switch_to_basic_block(resumption_value_type_is_return_block);

    // 2. Let awaited be Completion(Await(resumptionValue.[[Value]])).
    generate_await(generator, received_completion_value, received_completion, received_completion_type, received_completion_value);

    // 3. If awaited.[[Type]] is throw, return ? awaited.
    auto& awaited_type_is_normal_block = generator.make_block();
    auto awaited_type_is_throw_result = generator.allocate_register();
    generator.emit<Bytecode::Op::StrictlyEquals>(
        awaited_type_is_throw_result,
        received_completion_type,
        generator.add_constant(Value(to_underlying(Completion::Type::Throw))));
    generator.emit_jump_if(
        awaited_type_is_throw_result,
        Bytecode::Label { continuation_label },
        Bytecode::Label { awaited_type_is_normal_block });

    // 4. Assert: awaited.[[Type]] is normal.
    generator.switch_to_basic_block(awaited_type_is_normal_block);

    // 5. Return Completion Record { [[Type]]: return, [[Value]]: awaited.[[Value]], [[Target]]: empty }.
    generator.emit<Bytecode::Op::SetCompletionType>(received_completion, Completion::Type::Return);
    generator.emit<Bytecode::Op::Jump>(continuation_label);
}

Optional<ScopedOperand> YieldExpression::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    // Note: We need to catch any scheduled exceptions and reschedule them on re-entry
    //       as the act of yielding would otherwise clear them out
    //       This only applies when we are in a finalizer
    bool is_in_finalizer = generator.is_in_finalizer();
    Optional<ScopedOperand> saved_exception;

    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    VERIFY(generator.is_in_generator_function());

    auto received_completion = generator.allocate_register();
    auto received_completion_type = generator.allocate_register();
    auto received_completion_value = generator.allocate_register();

    if (m_is_yield_from) {
        // 15.5.5 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-generator-function-definitions-runtime-semantics-evaluation
        // 1. Let generatorKind be GetGeneratorKind().
        // NOTE: is_in_async_generator_function differentiates the generator kind.

        // 2. Let exprRef be ? Evaluation of AssignmentExpression.
        // 3. Let value be ? GetValue(exprRef).
        VERIFY(m_argument);
        auto value = m_argument->generate_bytecode(generator).value();

        // 4. Let iteratorRecord be ? GetIterator(value, generatorKind).
        // 5. Let iterator be iteratorRecord.[[Iterator]].
        auto iterator = generator.allocate_register();
        auto next_method = generator.allocate_register();
        auto iterator_done_property = generator.allocate_register();
        auto iterator_hint = generator.is_in_async_generator_function() ? IteratorHint::Async : IteratorHint::Sync;
        generator.emit<Bytecode::Op::GetIterator>(iterator, next_method, iterator_done_property, value, iterator_hint);

        // 6. Let received be NormalCompletion(undefined).
        // See get_received_completion_type_and_value above.
        generator.emit_mov(received_completion_type, generator.add_constant(Value(to_underlying(Completion::Type::Normal))));

        generator.emit_mov(received_completion_value, generator.add_constant(js_undefined()));

        // 7. Repeat,
        auto& loop_block = generator.make_block();
        auto& continuation_block = generator.make_block();
        auto& loop_end_block = generator.make_block();

        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { loop_block });
        generator.switch_to_basic_block(loop_block);

        // a. If received.[[Type]] is normal, then
        auto& type_is_normal_block = generator.make_block();
        auto& is_type_throw_block = generator.make_block();

        auto received_completion_type_register_is_normal = generator.allocate_register();
        generator.emit<Bytecode::Op::StrictlyEquals>(
            received_completion_type_register_is_normal,
            received_completion_type,
            generator.add_constant(Value(to_underlying(Completion::Type::Normal))));
        generator.emit_jump_if(
            received_completion_type_register_is_normal,
            Bytecode::Label { type_is_normal_block },
            Bytecode::Label { is_type_throw_block });

        generator.switch_to_basic_block(type_is_normal_block);

        // i. Let innerResult be ? Call(iteratorRecord.[[NextMethod]], iteratorRecord.[[Iterator]], « received.[[Value]] »).
        auto inner_result = generator.allocate_register();
        generator.emit_with_extra_operand_slots<Bytecode::Op::Call>(1, inner_result, next_method, iterator, OptionalNone {}, ReadonlySpan<ScopedOperand> { &received_completion_value, 1 });

        // ii. If generatorKind is async, set innerResult to ? Await(innerResult).
        if (generator.is_in_async_generator_function()) {
            auto new_inner_result = generate_await(generator, inner_result, received_completion, received_completion_type, received_completion_value);
            generator.emit_mov(inner_result, new_inner_result);
        }

        // iii. If innerResult is not an Object, throw a TypeError exception.
        generator.emit<Bytecode::Op::ThrowIfNotObject>(inner_result);

        // iv. Let done be ? IteratorComplete(innerResult).
        auto done = generator.allocate_register();
        generator.emit_iterator_complete(done, inner_result);

        // v. If done is true, then
        auto& type_is_normal_done_block = generator.make_block();
        auto& type_is_normal_not_done_block = generator.make_block();
        generator.emit_jump_if(
            done,
            Bytecode::Label { type_is_normal_done_block },
            Bytecode::Label { type_is_normal_not_done_block });

        generator.switch_to_basic_block(type_is_normal_done_block);

        // 1. Return ? IteratorValue(innerResult).
        auto return_value = generator.allocate_register();
        generator.emit_iterator_value(return_value, inner_result);
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { loop_end_block });

        generator.switch_to_basic_block(type_is_normal_not_done_block);

        // vi. If generatorKind is async, set received to Completion(AsyncGeneratorYield(? IteratorValue(innerResult))).
        // vii. Else, set received to Completion(GeneratorYield(innerResult)).

        {
            // FIXME: Yield currently only accepts a Value, not an object conforming to the IteratorResult interface, so we have to do an observable lookup of `value` here.
            //        This only matters for non-async generators.
            auto current_value = generator.allocate_register();
            generator.emit_iterator_value(current_value, inner_result);

            if (is_in_finalizer) {
                saved_exception = generator.allocate_register();
                generator.emit_mov(Bytecode::Operand(*saved_exception), Bytecode::Operand(Bytecode::Register::exception()));
            }

            generate_yield(generator,
                Bytecode::Label { continuation_block },
                current_value,
                received_completion,
                received_completion_type,
                received_completion_value,
                AwaitBeforeYield::No);
        }

        // b. Else if received.[[Type]] is throw, then
        generator.switch_to_basic_block(is_type_throw_block);
        auto& type_is_throw_block = generator.make_block();
        auto& type_is_return_block = generator.make_block();

        auto received_completion_type_register_is_throw = generator.allocate_register();
        generator.emit<Bytecode::Op::StrictlyEquals>(
            received_completion_type_register_is_throw,
            received_completion_type,
            generator.add_constant(Value(to_underlying(Completion::Type::Throw))));
        generator.emit_jump_if(
            received_completion_type_register_is_throw,
            Bytecode::Label { type_is_throw_block },
            Bytecode::Label { type_is_return_block });

        generator.switch_to_basic_block(type_is_throw_block);

        // i. Let throw be ? GetMethod(iterator, "throw").
        auto throw_method = generator.allocate_register();
        generator.emit<Bytecode::Op::GetMethod>(throw_method, iterator, generator.intern_property_key("throw"_utf16_fly_string));

        // ii. If throw is not undefined, then
        auto& throw_method_is_defined_block = generator.make_block();
        auto& throw_method_is_undefined_block = generator.make_block();
        generator.emit<Bytecode::Op::JumpUndefined>(
            throw_method,
            Bytecode::Label { throw_method_is_undefined_block },
            Bytecode::Label { throw_method_is_defined_block });

        generator.switch_to_basic_block(throw_method_is_defined_block);

        // 1. Let innerResult be ? Call(throw, iterator, « received.[[Value]] »).
        generator.emit_with_extra_operand_slots<Bytecode::Op::Call>(1, inner_result, throw_method, iterator, OptionalNone {}, ReadonlySpan<ScopedOperand> { &received_completion_value, 1 });

        // 2. If generatorKind is async, set innerResult to ? Await(innerResult).
        if (generator.is_in_async_generator_function()) {
            auto new_result = generate_await(generator, inner_result, received_completion, received_completion_type, received_completion_value);
            generator.emit_mov(inner_result, new_result);
        }

        // 3. NOTE: Exceptions from the inner iterator throw method are propagated. Normal completions from an inner throw method are processed similarly to an inner next.
        // 4. If innerResult is not an Object, throw a TypeError exception.
        generator.emit<Bytecode::Op::ThrowIfNotObject>(inner_result);

        // 5. Let done be ? IteratorComplete(innerResult).
        generator.emit_iterator_complete(done, inner_result);

        // 6. If done is true, then
        auto& type_is_throw_done_block = generator.make_block();
        auto& type_is_throw_not_done_block = generator.make_block();
        generator.emit_jump_if(
            done,
            Bytecode::Label { type_is_throw_done_block },
            Bytecode::Label { type_is_throw_not_done_block });

        generator.switch_to_basic_block(type_is_throw_done_block);

        // a. Return ? IteratorValue(innerResult).
        generator.emit_iterator_value(return_value, inner_result);
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { loop_end_block });

        generator.switch_to_basic_block(type_is_throw_not_done_block);

        {
            // 7. If generatorKind is async, set received to Completion(AsyncGeneratorYield(? IteratorValue(innerResult))).
            // 8. Else, set received to Completion(GeneratorYield(innerResult)).

            // FIXME: Yield currently only accepts a Value, not an object conforming to the IteratorResult interface, so we have to do an observable lookup of `value` here.
            //        This only matters for non-async generators.
            auto yield_value = generator.allocate_register();
            generator.emit_iterator_value(yield_value, inner_result);
            generate_yield(generator, Bytecode::Label { continuation_block }, yield_value, received_completion, received_completion_type, received_completion_value, AwaitBeforeYield::No);
        }

        generator.switch_to_basic_block(throw_method_is_undefined_block);

        // 1. NOTE: If iterator does not have a throw method, this throw is going to terminate the yield* loop. But first we need to give iterator a chance to clean up.

        // 2. Let closeCompletion be Completion Record { [[Type]]: normal, [[Value]]: empty, [[Target]]: empty }.
        // 3. If generatorKind is async, perform ? AsyncIteratorClose(iteratorRecord, closeCompletion).
        if (generator.is_in_async_generator_function()) {
            // Inline AsyncIteratorClose with proper Await op to avoid
            // spinning the event loop synchronously.
            auto return_method = generator.allocate_register();
            generator.emit<Bytecode::Op::GetMethod>(return_method, iterator, generator.intern_property_key("return"_utf16_fly_string));

            auto& call_return_block = generator.make_block();
            auto& after_close = generator.make_block();
            generator.emit<Bytecode::Op::JumpUndefined>(return_method, Bytecode::Label { after_close }, Bytecode::Label { call_return_block });
            generator.switch_to_basic_block(call_return_block);

            auto inner_result = generator.allocate_register();
            generator.emit_with_extra_operand_slots<Bytecode::Op::Call>(0, inner_result, return_method, iterator, OptionalNone {}, ReadonlySpan<ScopedOperand> {});

            auto awaited = generate_await(generator, inner_result, received_completion, received_completion_type, received_completion_value);
            generator.emit<Bytecode::Op::ThrowIfNotObject>(awaited);

            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { after_close });
            generator.switch_to_basic_block(after_close);
        }
        // 4. Else, perform ? IteratorClose(iteratorRecord, closeCompletion).
        else {
            generator.emit<Bytecode::Op::IteratorClose>(iterator, next_method, done, Completion::Type::Normal, generator.add_constant(js_undefined()));
        }

        // 5. NOTE: The next step throws a TypeError to indicate that there was a yield* protocol violation: iterator does not have a throw method.
        // 6. Throw a TypeError exception.
        auto exception = generator.allocate_register();
        generator.emit<Bytecode::Op::NewTypeError>(exception, generator.intern_string(ErrorType::YieldFromIteratorMissingThrowMethod.message()));
        generator.perform_needed_unwinds<Bytecode::Op::Throw>();
        generator.emit<Bytecode::Op::Throw>(exception);

        // c. Else,
        // i. Assert: received.[[Type]] is return.
        generator.switch_to_basic_block(type_is_return_block);

        // ii. Let return be ? GetMethod(iterator, "return").
        auto return_method = generator.allocate_register();
        generator.emit<Bytecode::Op::GetMethod>(return_method, iterator, generator.intern_property_key("return"_utf16_fly_string));

        // iii. If return is undefined, then
        auto& return_is_undefined_block = generator.make_block();
        auto& return_is_defined_block = generator.make_block();
        generator.emit<Bytecode::Op::JumpUndefined>(
            return_method,
            Bytecode::Label { return_is_undefined_block },
            Bytecode::Label { return_is_defined_block });

        generator.switch_to_basic_block(return_is_undefined_block);

        // 1. If generatorKind is async, set received.[[Value]] to ? Await(received.[[Value]]).
        if (generator.is_in_async_generator_function()) {
            generate_await(generator, received_completion_value, received_completion, received_completion_type, received_completion_value);
        }

        // 2. Return ? received.
        // NOTE: This will always be a return completion.
        generator.emit_return<Bytecode::Op::Yield>(received_completion_value);

        generator.switch_to_basic_block(return_is_defined_block);

        // iv. Let innerReturnResult be ? Call(return, iterator, « received.[[Value]] »).
        auto inner_return_result = generator.allocate_register();
        generator.emit_with_extra_operand_slots<Bytecode::Op::Call>(1, inner_return_result, return_method, iterator, OptionalNone {}, ReadonlySpan<ScopedOperand> { &received_completion_value, 1 });

        // v. If generatorKind is async, set innerReturnResult to ? Await(innerReturnResult).
        if (generator.is_in_async_generator_function()) {
            auto new_value = generate_await(generator, inner_return_result, received_completion, received_completion_type, received_completion_value);
            generator.emit_mov(inner_return_result, new_value);
        }

        // vi. If innerReturnResult is not an Object, throw a TypeError exception.
        generator.emit<Bytecode::Op::ThrowIfNotObject>(inner_return_result);

        // vii. Let done be ? IteratorComplete(innerReturnResult).
        generator.emit_iterator_complete(done, inner_return_result);

        // viii. If done is true, then
        auto& type_is_return_done_block = generator.make_block();
        auto& type_is_return_not_done_block = generator.make_block();
        generator.emit_jump_if(
            done,
            Bytecode::Label { type_is_return_done_block },
            Bytecode::Label { type_is_return_not_done_block });

        generator.switch_to_basic_block(type_is_return_done_block);

        // 1. Let value be ? IteratorValue(innerReturnResult).
        auto inner_return_result_value = generator.allocate_register();
        generator.emit_iterator_value(inner_return_result_value, inner_return_result);

        // 2. Return Completion Record { [[Type]]: return, [[Value]]: value, [[Target]]: empty }.
        generator.emit_return<Bytecode::Op::Yield>(inner_return_result_value);

        generator.switch_to_basic_block(type_is_return_not_done_block);

        // ix. If generatorKind is async, set received to Completion(AsyncGeneratorYield(? IteratorValue(innerReturnResult))).
        // x. Else, set received to Completion(GeneratorYield(innerReturnResult)).
        // FIXME: Yield currently only accepts a Value, not an object conforming to the IteratorResult interface, so we have to do an observable lookup of `value` here.
        //        This only matters for non-async generators.
        auto received = generator.allocate_register();
        generator.emit_iterator_value(received, inner_return_result);

        generate_yield(generator, Bytecode::Label { continuation_block }, received, received_completion, received_completion_type, received_completion_value, AwaitBeforeYield::No);

        generator.switch_to_basic_block(continuation_block);

        if (is_in_finalizer)
            generator.emit_mov(Bytecode::Operand(Bytecode::Register::exception()), Bytecode::Operand(*saved_exception));

        generator.emit_mov(received_completion, generator.accumulator());
        get_received_completion_type_and_value(generator, received_completion, received_completion_type, received_completion_value);
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { loop_block });

        generator.switch_to_basic_block(loop_end_block);
        return return_value;
    }

    Optional<ScopedOperand> argument;
    if (m_argument)
        argument = m_argument->generate_bytecode(generator).value();
    else
        argument = generator.add_constant(js_undefined());

    auto& continuation_block = generator.make_block();

    if (is_in_finalizer) {
        saved_exception = generator.allocate_register();
        generator.emit_mov(Bytecode::Operand(*saved_exception), Bytecode::Operand(Bytecode::Register::exception()));
    }

    generate_yield(generator, Bytecode::Label { continuation_block }, *argument, received_completion, received_completion_type, received_completion_value, AwaitBeforeYield::Yes);
    generator.switch_to_basic_block(continuation_block);

    if (is_in_finalizer)
        generator.emit_mov(Bytecode::Operand(Bytecode::Register::exception()), Bytecode::Operand(*saved_exception));

    generator.emit_mov(received_completion, generator.accumulator());

    get_received_completion_type_and_value(generator, received_completion, received_completion_type, received_completion_value);

    auto& normal_completion_continuation_block = generator.make_block();
    auto& throw_completion_continuation_block = generator.make_block();

    auto received_completion_type_is_normal = generator.allocate_register();
    generator.emit<Bytecode::Op::StrictlyEquals>(
        received_completion_type_is_normal,
        received_completion_type,
        generator.add_constant(Value(to_underlying(Completion::Type::Normal))));
    generator.emit_jump_if(
        received_completion_type_is_normal,
        Bytecode::Label { normal_completion_continuation_block },
        Bytecode::Label { throw_completion_continuation_block });

    auto& throw_value_block = generator.make_block();
    auto& return_value_block = generator.make_block();

    generator.switch_to_basic_block(throw_completion_continuation_block);
    auto received_completion_type_is_throw = generator.allocate_register();
    generator.emit<Bytecode::Op::StrictlyEquals>(
        received_completion_type_is_throw,
        received_completion_type,
        generator.add_constant(Value(to_underlying(Completion::Type::Throw))));

    // If type is not equal to "throw" or "normal", assume it's "return".
    generator.emit_jump_if(
        received_completion_type_is_throw,
        Bytecode::Label { throw_value_block },
        Bytecode::Label { return_value_block });

    generator.switch_to_basic_block(throw_value_block);
    generator.perform_needed_unwinds<Bytecode::Op::Throw>();
    generator.emit<Bytecode::Op::Throw>(received_completion_value);

    generator.switch_to_basic_block(return_value_block);
    generator.emit_return<Bytecode::Op::Yield>(received_completion_value);

    generator.switch_to_basic_block(normal_completion_continuation_block);
    return received_completion_value;
}

Optional<ScopedOperand> IfStatement::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    // test
    // jump if_true (true) true (false) false
    // true
    // jump always (true) end
    // false
    // jump always (true) end
    // end

    auto predicate = m_predicate->generate_bytecode(generator).value();

    Optional<ScopedOperand> completion;
    if (generator.must_propagate_completion()) {
        completion = choose_dst(generator, preferred_dst);
        generator.emit_mov(*completion, generator.add_constant(js_undefined()));
    }

    auto build_block = [&](auto node, Optional<BasicBlock&> end_block = {}) -> Optional<ScopedOperand> {
        Optional<Bytecode::Generator::CompletionRegisterScope> completion_scope;
        if (completion.has_value())
            completion_scope.emplace(generator, *completion);
        auto value = node->generate_bytecode(generator, completion);
        if (!generator.is_current_block_terminated()) {
            if (generator.must_propagate_completion() && value.has_value())
                generator.emit_mov(*completion, *value);
            if (end_block.has_value())
                generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *end_block });
        }
        return Optional<ScopedOperand> {};
    };

    // OPTIMIZATION: if the predicate is always true/false, only build the consequent/alternate blocks, respectively.
    if (auto constant = generator.try_get_constant(predicate); constant.has_value()) {
        if (constant->to_boolean_slow_case()) {
            (void)build_block(m_consequent);
        } else if (m_alternate) {
            (void)build_block(m_alternate);
        }
        return completion;
    }

    auto& true_block = generator.make_block();
    auto& false_block = generator.make_block();
    // NOTE: if there is no 'else' block the end block is the same as the false block
    auto& end_block = m_alternate ? generator.make_block() : false_block;

    generator.emit_jump_if(
        predicate,
        Bytecode::Label { true_block },
        Bytecode::Label { false_block });

    generator.switch_to_basic_block(true_block);
    (void)build_block(m_consequent, { end_block });

    if (m_alternate) {
        generator.switch_to_basic_block(false_block);
        (void)build_block(m_alternate, { end_block });
    }

    generator.switch_to_basic_block(end_block);

    return completion;
}

Optional<ScopedOperand> ContinueStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    if (!m_target_label.has_value()) {
        generator.generate_continue();
        return Optional<ScopedOperand> {};
    }

    generator.generate_continue(m_target_label.value());
    return Optional<ScopedOperand> {};
}

Optional<ScopedOperand> DebuggerStatement::generate_bytecode(Bytecode::Generator&, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    return Optional<ScopedOperand> {};
}

Optional<ScopedOperand> ConditionalExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    auto test = m_test->generate_bytecode(generator).value();

    // OPTIMIZATION: if the predicate is always true/false, only build the consequent/alternate blocks, respectively.
    if (auto constant = generator.try_get_constant(test); constant.has_value()) {
        auto is_always_true = constant->to_boolean_slow_case();

        if (is_always_true)
            return m_consequent->generate_bytecode(generator).value();
        return m_alternate->generate_bytecode(generator).value();
    }

    // test
    // jump if_true (true) true (false) false
    // true
    // jump always (true) end
    // false
    // jump always (true) end
    // end

    auto& true_block = generator.make_block();
    auto& false_block = generator.make_block();
    auto& end_block = generator.make_block();

    generator.emit_jump_if(
        test,
        Bytecode::Label { true_block },
        Bytecode::Label { false_block });

    auto dst = choose_dst(generator, preferred_dst);

    generator.switch_to_basic_block(true_block);
    auto consequent = m_consequent->generate_bytecode(generator).value();
    generator.emit_mov(dst, consequent);

    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { end_block });

    generator.switch_to_basic_block(false_block);
    auto alternate = m_alternate->generate_bytecode(generator).value();
    generator.emit_mov(dst, alternate);
    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { end_block });

    generator.switch_to_basic_block(end_block);
    return dst;
}

Optional<ScopedOperand> SequenceExpression::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    Optional<ScopedOperand> last_value;
    for (auto& expression : m_expressions) {
        last_value = expression->generate_bytecode(generator);
    }

    return last_value;
}

Optional<ScopedOperand> TemplateLiteral::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    auto dst = choose_dst(generator, preferred_dst);

    Vector segments(m_expressions);

    segments.remove_all_matching([&](auto expr) {
        return expr->is_string_literal() && static_cast<StringLiteral const&>(*expr).value().is_empty();
    });

    // OPTIMIZATION: Empty template literal (``) can be turned into empty string literal ("")
    if (segments.size() == 0)
        return generator.add_constant(Value { GC::Ref { generator.vm().empty_string() } });

    if (segments.size() == 1) {
        auto value = segments[0]->generate_bytecode(generator).value();

        // OPTIMIZATION: String literal template (`xyz`) can be returned directly
        if (value.operand().is_constant())
            return value;

        // OPTIMIZATION: `${x}` can be turned into ToString(x) op
        generator.emit<Bytecode::Op::ToString>(dst, value);

        return dst;
    }

    for (size_t i = 0; i < segments.size(); i++) {
        auto expr = segments[i];

        auto value = expr->generate_bytecode(generator).value();

        if (i == 0) {
            if (expr->is_string_literal()) {
                generator.emit_mov(dst, value);
            } else {
                generator.emit<Bytecode::Op::ToString>(dst, value);
            }
        } else {
            generator.emit<Bytecode::Op::ConcatString>(dst, value);
        }
    }

    return dst;
}

struct TagAndThisValue {
    ScopedOperand tag;
    ScopedOperand this_value;
};

Optional<ScopedOperand> TaggedTemplateLiteral::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto [tag, this_value] = [&]() -> TagAndThisValue {
        if (is<MemberExpression>(*m_tag)) {
            auto& member_expression = static_cast<MemberExpression const&>(*m_tag);
            auto base_and_value = get_base_and_value_from_member_expression(generator, member_expression);
            return TagAndThisValue { .tag = base_and_value.value, .this_value = base_and_value.base };
        }

        if (is<Identifier>(*m_tag)) {
            auto& identifier = static_cast<Identifier const&>(*m_tag);
            if (identifier.is_local() || identifier.is_global()) {
                // Keep the normal Identifier path so local/global tags preserve
                // TDZ behavior; only non-local identifiers need with-aware
                // callee/this extraction.
                auto tag = m_tag->generate_bytecode(generator).value();
                return TagAndThisValue { .tag = tag, .this_value = generator.add_constant(js_undefined()) };
            }

            auto tag = generator.allocate_register();
            auto this_value = generator.allocate_register();
            generator.emit<Bytecode::Op::GetCalleeAndThisFromEnvironment>(
                tag,
                this_value,
                generator.intern_identifier(identifier.string()));
            return TagAndThisValue { .tag = tag, .this_value = this_value };
        }

        auto tag = m_tag->generate_bytecode(generator).value();
        return TagAndThisValue { .tag = tag, .this_value = generator.add_constant(js_undefined()) };
    }();

    // 13.2.8.4 GetTemplateObject ( templateLiteral ), https://tc39.es/ecma262/#sec-gettemplateobject
    Vector<ScopedOperand> string_regs;
    auto& expressions = m_template_literal->expressions();

    for (size_t i = 0; i < expressions.size(); i += 2) {
        // NOTE: If the string contains invalid escapes we get a null expression here,
        //       which we then convert to the expected `undefined` TV. See
        //       12.9.6.1 Static Semantics: TV, https://tc39.es/ecma262/#sec-static-semantics-tv
        if (is<NullLiteral>(expressions[i])) {
            string_regs.append(generator.add_constant(js_undefined()));
        } else {
            auto value = expressions[i]->generate_bytecode(generator).value();
            string_regs.append(move(value));
        }
    }

    auto& raw_strings = m_template_literal->raw_strings();
    for (auto const& raw_string : raw_strings) {
        auto value = raw_string->generate_bytecode(generator).value();
        string_regs.append(move(value));
    }

    auto strings_array = generator.allocate_register();
    generator.emit_with_extra_operand_slots<Bytecode::Op::GetTemplateObject>(
        string_regs.size(),
        strings_array,
        generator.next_template_object_cache(),
        string_regs);

    Vector<ScopedOperand> argument_regs;
    argument_regs.append(strings_array);

    for (size_t i = 1; i < expressions.size(); i += 2) {
        auto argument = expressions[i]->generate_bytecode(generator).value();
        argument_regs.append(move(argument));
    }

    auto dst = choose_dst(generator, preferred_dst);
    generator.emit_with_extra_operand_slots<Bytecode::Op::Call>(argument_regs.size(), dst, tag, this_value, OptionalNone {}, argument_regs);
    return dst;
}

Optional<ScopedOperand> UpdateExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand>) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto reference = generator.emit_load_from_reference(*m_argument);

    Optional<ScopedOperand> previous_value_for_postfix;

    if (m_op == UpdateOp::Increment) {
        if (m_prefixed) {
            generator.emit<Bytecode::Op::Increment>(*reference.loaded_value);
        } else {
            previous_value_for_postfix = generator.allocate_register();
            generator.emit<Bytecode::Op::PostfixIncrement>(*previous_value_for_postfix, *reference.loaded_value);
        }
    } else {
        if (m_prefixed) {
            generator.emit<Bytecode::Op::Decrement>(*reference.loaded_value);
        } else {
            previous_value_for_postfix = generator.allocate_register();
            generator.emit<Bytecode::Op::PostfixDecrement>(*previous_value_for_postfix, *reference.loaded_value);
        }
    }

    if (is<Identifier>(*m_argument))
        generator.emit_store_to_reference(static_cast<Identifier const&>(*m_argument), *reference.loaded_value);
    else
        generator.emit_store_to_reference(reference, *reference.loaded_value);

    if (!m_prefixed)
        return *previous_value_for_postfix;
    return *reference.loaded_value;
}

Optional<ScopedOperand> ThrowStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto argument = m_argument->generate_bytecode(generator).value();
    generator.perform_needed_unwinds<Bytecode::Op::Throw>();
    generator.emit<Bytecode::Op::Throw>(argument);
    return Optional<ScopedOperand> {};
}

Optional<ScopedOperand> BreakStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    // FIXME: Handle finally blocks in a graceful manner
    //        We need to execute the finally block, but tell it to resume
    //        execution at the designated block
    if (!m_target_label.has_value()) {
        generator.generate_break();
        return Optional<ScopedOperand> {};
    }

    generator.generate_break(m_target_label.value());
    return Optional<ScopedOperand> {};
}

// Try/finally uses an explicit completion record protocol:
//
//   1. Allocate two registers: completion_type and completion_value
//   2. Every path into the finally body sets these before jumping:
//      - Normal exit: completion_type = NORMAL
//      - Exception:   completion_type = THROW, completion_value = exception
//      - Return:      completion_type = RETURN, completion_value = return value
//      - Break/continue: completion_type = FIRST_JUMP_INDEX + n
//   3. After the finally body, a dispatch chain checks completion_type
//      and routes to the correct continuation (next block, jump target,
//      return, or rethrow).
//
// For exceptions, the handler table points to an "exception preamble" block
// that catches the exception into completion_value, sets completion_type to
// THROW, and jumps to the finally body.
//
// For nested finally (e.g. break through two finally blocks), trampoline
// blocks chain through each finally layer, with each inner finally dispatching
// to a trampoline that sets up the outer finally's completion record.
Optional<ScopedOperand> TryStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto& saved_block = generator.current_block();

    Optional<Bytecode::Label> handler_target;
    Optional<Bytecode::Generator::UnwindContext> unwind_context;

    Bytecode::BasicBlock* next_block { nullptr };

    Optional<ScopedOperand> completion;

    Optional<Bytecode::Generator::FinallyContext> finally_context;
    Bytecode::BasicBlock* finally_body_block_ptr { nullptr };

    // Capture the lexical environment at try entry for restoration on catch/exception.
    Optional<ScopedOperand> lexical_environment_at_entry;
    lexical_environment_at_entry = generator.current_lexical_environment_register();

    if (m_finalizer) {
        // Allocate completion record registers.
        auto completion_type = generator.allocate_register();
        auto completion_value = generator.allocate_register();

        // Create the exception preamble block (handler table points here for exceptions).
        auto& exception_preamble_block = generator.make_block();

        // Create the finally body block (all paths converge here).
        auto& finally_body_block = generator.make_block();
        finally_body_block_ptr = &finally_body_block;

        // Set up FinallyContext.
        finally_context.emplace(Bytecode::Generator::FinallyContext {
            .completion_type = completion_type,
            .completion_value = completion_value,
            .finally_body = Bytecode::Label { finally_body_block },
            .exception_preamble = Bytecode::Label { exception_preamble_block },
            .parent = generator.current_finally_context(),
            .registered_jumps = {},
            .next_jump_index = Bytecode::Generator::FinallyContext::FIRST_JUMP_INDEX,
            .lexical_environment_at_entry = lexical_environment_at_entry,
        });
        generator.set_current_finally_context(&*finally_context);

        // Generate exception preamble:
        //   Catch completion_value
        //   SetLexicalEnvironment (restore to try entry)
        //   Mov completion_type, 1 (Throw)
        //   Jump finally_body
        generator.switch_to_basic_block(exception_preamble_block);
        generator.emit<Bytecode::Op::Catch>(completion_value);
        generator.emit<Bytecode::Op::SetLexicalEnvironment>(*lexical_environment_at_entry);
        generator.emit_mov(completion_type, generator.add_constant(Value(Bytecode::Generator::FinallyContext::THROW)));
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { finally_body_block });

        // Set up unwind context with exception_preamble as finalizer.
        generator.start_boundary(Bytecode::Generator::BlockBoundaryType::ReturnToFinally);
        unwind_context.emplace(generator, Bytecode::Label { exception_preamble_block });
    }

    if (m_handler) {
        auto& handler_block = generator.make_block();
        generator.switch_to_basic_block(handler_block);

        auto caught_value = generator.allocate_register();
        generator.emit<Bytecode::Op::Catch>(caught_value);
        generator.emit<Bytecode::Op::SetLexicalEnvironment>(*lexical_environment_at_entry);

        // OPTIMIZATION: We avoid creating a lexical environment if the catch clause has no parameter.
        bool did_create_variable_scope_for_catch_clause = false;

        m_handler->parameter().visit(
            [&](NonnullRefPtr<Identifier const> const& parameter) -> void {
                if (parameter->is_local()) {
                    auto local = generator.local(parameter->local_index());
                    generator.emit_mov(local, caught_value);
                    generator.set_local_initialized(parameter->local_index());
                } else {
                    generator.begin_variable_scope();
                    did_create_variable_scope_for_catch_clause = true;
                    auto parameter_identifier = generator.intern_identifier(parameter->string());
                    generator.emit<Bytecode::Op::CreateVariable>(parameter_identifier, Bytecode::Op::EnvironmentMode::Lexical, false, false, false);
                    generator.emit<Bytecode::Op::InitializeLexicalBinding>(parameter_identifier, caught_value);
                }
            },
            [&](NonnullRefPtr<BindingPattern const> const& binding_pattern) -> void {
                MUST(binding_pattern->for_each_bound_identifier([&](auto const& identifier) {
                    if (!identifier.is_local())
                        did_create_variable_scope_for_catch_clause = true;
                }));

                if (did_create_variable_scope_for_catch_clause)
                    generator.begin_variable_scope();

                MUST(binding_pattern->for_each_bound_identifier([&](auto const& identifier) {
                    if (identifier.is_local())
                        return;
                    auto parameter_identifier = generator.intern_identifier(identifier.string());
                    generator.emit<Bytecode::Op::CreateVariable>(parameter_identifier, Bytecode::Op::EnvironmentMode::Lexical, false, false, false);
                }));

                binding_pattern->generate_bytecode(generator, Bytecode::Op::BindingInitializationMode::Initialize, caught_value);
            },
            [](Empty) -> void {
            });

        Optional<ScopedOperand> catch_completion;
        {
            // NB: The catch body needs its own completion register so that
            // break/continue inside the catch block carries the catch's
            // own completion value rather than leaking a value from an
            // enclosing statement.
            Optional<Bytecode::Generator::CompletionRegisterScope> completion_scope;
            if (generator.must_propagate_completion()) {
                catch_completion = generator.allocate_register();
                generator.emit_mov(*catch_completion, generator.add_constant(js_undefined()));
                completion_scope.emplace(generator, *catch_completion);
            }
            (void)m_handler->body().generate_bytecode(generator);
        }
        if (generator.must_propagate_completion()) {
            if (catch_completion.has_value() && !generator.is_current_block_terminated()) {
                completion = generator.allocate_register();
                generator.emit_mov(*completion, *catch_completion);
            }
        }
        handler_target = Bytecode::Label { handler_block };

        if (did_create_variable_scope_for_catch_clause)
            generator.end_variable_scope();

        if (!generator.is_current_block_terminated()) {
            if (m_finalizer) {
                // Normal exit from catch → set completion_type=Normal, jump to finally.
                generator.emit_mov(finally_context->completion_type, generator.add_constant(Value(Bytecode::Generator::FinallyContext::NORMAL)));
                generator.emit<Bytecode::Op::Jump>(finally_context->finally_body);
            } else {
                VERIFY(!next_block);
                VERIFY(!unwind_context.has_value());
                next_block = &generator.make_block();
                generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *next_block });
            }
        }
    }

    if (m_finalizer)
        generator.end_boundary(Bytecode::Generator::BlockBoundaryType::ReturnToFinally);
    if (m_handler) {
        if (!m_finalizer) {
            auto const* parent_unwind_context = generator.current_unwind_context();
            if (parent_unwind_context)
                unwind_context.emplace(generator, parent_unwind_context->handler());
            else
                unwind_context.emplace(generator, OptionalNone());
        }
        unwind_context->set_handler(handler_target.value());
    }

    auto& target_block = generator.make_block();
    generator.switch_to_basic_block(saved_block);
    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { target_block });
    if (m_finalizer)
        generator.start_boundary(Bytecode::Generator::BlockBoundaryType::ReturnToFinally);

    generator.switch_to_basic_block(target_block);
    Optional<ScopedOperand> try_completion;
    {
        // NB: The try body needs its own completion register so that
        // break/continue inside the try block carries the try's own
        // completion value rather than leaking a value from an enclosing
        // statement.
        Optional<Bytecode::Generator::CompletionRegisterScope> completion_scope;
        if (generator.must_propagate_completion()) {
            try_completion = generator.allocate_register();
            generator.emit_mov(*try_completion, generator.add_constant(js_undefined()));
            completion_scope.emplace(generator, *try_completion);
        }
        (void)m_block->generate_bytecode(generator);
    }
    if (!generator.is_current_block_terminated()) {
        if (generator.must_propagate_completion()) {
            if (try_completion.has_value()) {
                completion = generator.allocate_register();
                generator.emit_mov(*completion, *try_completion);
            }
        }

        if (m_finalizer) {
            // Normal exit from try → set completion_type=Normal, jump to finally.
            generator.emit_mov(finally_context->completion_type, generator.add_constant(Value(Bytecode::Generator::FinallyContext::NORMAL)));
            generator.emit<Bytecode::Op::Jump>(finally_context->finally_body);
        } else {
            VERIFY(unwind_context.has_value());
            unwind_context.clear();
            if (!next_block)
                next_block = &generator.make_block();
            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *next_block });
        }
    }

    if (m_finalizer)
        generator.end_boundary(Bytecode::Generator::BlockBoundaryType::ReturnToFinally);

    // Now generate the finally body and after-finally dispatch.
    // We deferred this so that registered_jumps from break/continue in the try body are available.
    if (m_finalizer && finally_context.has_value()) {
        generator.set_current_finally_context(finally_context->parent);

        // Clear the unwind context so that blocks created during finally body generation
        // don't inherit the inner handler/finalizer (the inner unwind context is already
        // popped at runtime by the time the finally body runs).
        unwind_context.clear();

        generator.switch_to_basic_block(*finally_body_block_ptr);
        generator.start_boundary(Bytecode::Generator::BlockBoundaryType::LeaveFinally);
        {
            // NB: The finally body needs its own completion register so that
            // break/continue inside the finally block carries the finally's
            // own completion value (initialized to undefined) rather than
            // leaking the try/catch block's completion value through.
            Optional<ScopedOperand> finally_completion;
            Optional<Bytecode::Generator::CompletionRegisterScope> completion_scope;
            if (generator.must_propagate_completion()) {
                finally_completion = generator.allocate_register();
                generator.emit_mov(*finally_completion, generator.add_constant(js_undefined()));
                completion_scope.emplace(generator, *finally_completion);
            }
            (void)m_finalizer->generate_bytecode(generator);
        }
        generator.end_boundary(Bytecode::Generator::BlockBoundaryType::LeaveFinally);

        if (!generator.is_current_block_terminated()) {
            if (!next_block)
                next_block = &generator.make_block();

            auto const& completion_type = finally_context->completion_type;
            auto const& completion_value = finally_context->completion_value;

            // After-finally dispatch chain: a series of JumpStrictlyEquals that check
            // completion_type and route to the right continuation. Order:
            //   1. NORMAL → fall through to next block
            //   2. Each registered break/continue target
            //   3. RETURN → return/yield the completion_value
            //   4. Default → rethrow completion_value (must be THROW)
            auto& after_normal_check = generator.make_block();
            generator.emit<Bytecode::Op::JumpStrictlyEquals>(
                completion_type, generator.add_constant(Value(Bytecode::Generator::FinallyContext::NORMAL)),
                Bytecode::Label { *next_block }, Bytecode::Label { after_normal_check });

            generator.switch_to_basic_block(after_normal_check);

            // Registered break/continue jumps (indices 3+)
            for (auto const& jump : finally_context->registered_jumps) {
                auto& after_jump_check = generator.make_block();
                generator.emit<Bytecode::Op::JumpStrictlyEquals>(
                    completion_type, generator.add_constant(Value(jump.index)),
                    jump.target, Bytecode::Label { after_jump_check });
                generator.switch_to_basic_block(after_jump_check);
            }

            auto& return_block = generator.make_block();
            auto& rethrow_block = generator.make_block();
            generator.emit<Bytecode::Op::JumpStrictlyEquals>(
                completion_type, generator.add_constant(Value(Bytecode::Generator::FinallyContext::RETURN)),
                Bytecode::Label { return_block }, Bytecode::Label { rethrow_block });

            // Generate return block.
            generator.switch_to_basic_block(return_block);
            if (finally_context->parent) {
                // Nested finally: copy completion record to outer and jump to outer finally body.
                auto& outer = *finally_context->parent;
                generator.emit_mov(outer.completion_type, completion_type);
                generator.emit_mov(outer.completion_value, completion_value);
                generator.emit<Bytecode::Op::Jump>(outer.finally_body);
            } else {
                if (generator.is_in_generator_function()) {
                    generator.emit<Bytecode::Op::Yield>(OptionalNone {}, completion_value);
                } else {
                    generator.emit<Bytecode::Op::Return>(completion_value);
                }
            }

            // Default: rethrow the exception.
            generator.switch_to_basic_block(rethrow_block);
            generator.emit<Bytecode::Op::Throw>(completion_value);
        }
    }

    generator.switch_to_basic_block(next_block ? *next_block : saved_block);
    if (generator.must_propagate_completion()) {
        if (!completion.has_value())
            return generator.add_constant(js_undefined());
    }
    return completion;
}

Optional<ScopedOperand> SwitchStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generate_labelled_evaluation(generator, {});
}

Optional<ScopedOperand> SwitchStatement::generate_labelled_evaluation(Bytecode::Generator& generator, Vector<FlyString> const& label_set, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);

    Optional<ScopedOperand> completion;
    if (generator.must_propagate_completion()) {
        completion = generator.allocate_register();
        generator.emit_mov(*completion, generator.add_constant(js_undefined()));
    }

    auto discriminant = m_discriminant->generate_bytecode(generator).value();
    Vector<Bytecode::BasicBlock&> case_blocks;
    Bytecode::BasicBlock* entry_block_for_default { nullptr };
    Bytecode::BasicBlock* next_test_block = &generator.make_block();

    bool did_create_lexical_environment = false;
    if (has_lexical_declarations())
        did_create_lexical_environment = generator.emit_block_declaration_instantiation(*this);

    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *next_test_block });

    Queue<Bytecode::BasicBlock*> test_blocks;
    for (auto& switch_case : m_cases) {
        if (switch_case->test())
            test_blocks.enqueue(&generator.make_block());
    }

    for (auto& switch_case : m_cases) {
        auto& case_block = generator.make_block();
        if (switch_case->test()) {
            generator.switch_to_basic_block(*next_test_block);
            auto test_value = switch_case->test()->generate_bytecode(generator).value();
            auto result = generator.allocate_register();
            generator.emit<Bytecode::Op::StrictlyEquals>(result, test_value, discriminant);
            next_test_block = test_blocks.dequeue();
            generator.emit_jump_if(
                result,
                Bytecode::Label { case_block },
                Bytecode::Label { *next_test_block });
        } else {
            entry_block_for_default = &case_block;
        }

        case_blocks.append(case_block);
    }
    generator.switch_to_basic_block(*next_test_block);
    auto& end_block = generator.make_block();

    if (entry_block_for_default != nullptr) {
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *entry_block_for_default });
    } else {
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { end_block });
    }
    auto current_block = case_blocks.begin();
    generator.begin_breakable_scope(Bytecode::Label { end_block }, label_set, completion);
    for (auto& switch_case : m_cases) {
        generator.switch_to_basic_block(*current_block);
        {
            Optional<Bytecode::Generator::CompletionRegisterScope> completion_scope;
            if (completion.has_value())
                completion_scope.emplace(generator, *completion);
            for (auto& statement : switch_case->children()) {
                auto result = statement->generate_bytecode(generator);
                if (generator.is_current_block_terminated())
                    break;
                if (generator.must_propagate_completion()) {
                    if (result.has_value())
                        generator.emit_mov(*completion, *result);
                    else
                        generator.emit_mov(*completion, generator.add_constant(js_undefined()));
                }
            }
        }
        if (!generator.is_current_block_terminated()) {
            auto next_block = current_block;
            next_block++;
            if (next_block.is_end()) {
                generator.emit<Bytecode::Op::Jump>(Bytecode::Label { end_block });
            } else {
                generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *next_block });
            }
        }
        current_block++;
    }
    generator.end_breakable_scope();

    generator.switch_to_basic_block(end_block);

    if (did_create_lexical_environment)
        generator.end_variable_scope();

    return completion;
}

Optional<ScopedOperand> SuperExpression::generate_bytecode(Bytecode::Generator&, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    // The semantics for SuperExpression are handled in CallExpression and SuperCall.
    VERIFY_NOT_REACHED();
}

Optional<ScopedOperand> ClassDeclaration::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto value = m_class_expression->generate_bytecode(generator).value();
    generator.emit_set_variable(*m_class_expression.ptr()->m_name, value, Bytecode::Op::BindingInitializationMode::Initialize);
    // NOTE: ClassDeclaration does not produce a value.
    return Optional<ScopedOperand> {};
}

// 15.7.14 Runtime Semantics: ClassDefinitionEvaluation, https://tc39.es/ecma262/#sec-runtime-semantics-classdefinitionevaluation
Optional<ScopedOperand> ClassExpression::generate_bytecode_with_lhs_name(Bytecode::Generator& generator, Optional<Bytecode::IdentifierTableIndex> lhs_name, Optional<ScopedOperand> preferred_dst) const
{
    // NOTE: Step 2 is not a part of NewClass instruction because it is assumed to be done before super class expression evaluation
    auto parent_environment = generator.current_lexical_environment_register();
    auto class_environment = generator.allocate_register();
    generator.emit<Bytecode::Op::CreateLexicalEnvironment>(class_environment, parent_environment, 0);
    generator.push_lexical_environment_register(class_environment);

    if (has_name() || !lhs_name.has_value()) {
        // NOTE: Step 3.a is not a part of NewClass instruction because it is assumed to be done before super class expression evaluation
        auto interned_index = generator.intern_identifier(name());
        generator.emit<Bytecode::Op::CreateVariable>(interned_index, Bytecode::Op::EnvironmentMode::Lexical, true, false, false);
    }

    Optional<ScopedOperand> super_class;
    if (m_super_class)
        super_class = m_super_class->generate_bytecode(generator).value();

    bool did_emit_private_environment_allocation = false;
    for (auto const& element : m_elements) {
        auto opt_private_name = element->private_bound_identifier();
        if (opt_private_name.has_value()) {
            if (!did_emit_private_environment_allocation) {
                generator.emit<Op::CreatePrivateEnvironment>();
                did_emit_private_environment_allocation = true;
            }
            generator.emit<Op::AddPrivateName>(generator.intern_identifier(*opt_private_name));
        }
    }

    Vector<Optional<ScopedOperand>> elements;
    for (auto const& element : m_elements) {
        Optional<ScopedOperand> key;
        if (is<ClassMethod>(*element)) {
            auto const& class_method = static_cast<ClassMethod const&>(*element);
            if (!is<PrivateIdentifier>(class_method.key()))
                key = class_method.key().generate_bytecode(generator);
        } else if (is<ClassField>(*element)) {
            auto const& class_field = static_cast<ClassField const&>(*element);
            if (!is<PrivateIdentifier>(class_field.key()))
                key = class_field.key().generate_bytecode(generator);
        }

        elements.append({ key });
    }

    // Build a ClassBlueprint that captures all class element metadata at codegen time.
    auto& vm = generator.vm();
    ClassBlueprint blueprint;
    blueprint.has_super_class = !m_super_class.is_null();
    blueprint.has_name = has_name();
    blueprint.name = name();
    blueprint.source_text = source_text();

    // Register shared function data for the constructor.
    auto constructor_shared_data = SharedFunctionInstanceData::create_for_function_node(vm, *m_constructor);
    blueprint.constructor_shared_function_data_index = generator.register_shared_function_data(constructor_shared_data);

    for (auto const& element : m_elements) {
        if (is<ClassMethod>(*element)) {
            auto const& class_method = static_cast<ClassMethod const&>(*element);
            bool is_private = is<PrivateIdentifier>(class_method.key());

            ClassElementDescriptor::Kind descriptor_kind;
            switch (class_method.kind()) {
            case ClassMethod::Kind::Method:
                descriptor_kind = ClassElementDescriptor::Kind::Method;
                break;
            case ClassMethod::Kind::Getter:
                descriptor_kind = ClassElementDescriptor::Kind::Getter;
                break;
            case ClassMethod::Kind::Setter:
                descriptor_kind = ClassElementDescriptor::Kind::Setter;
                break;
            }

            auto shared_data = SharedFunctionInstanceData::create_for_function_node(vm, class_method.function());
            auto data_index = generator.register_shared_function_data(shared_data);

            blueprint.elements.append({
                .kind = descriptor_kind,
                .is_static = element->is_static(),
                .is_private = is_private,
                .private_identifier = is_private ? Optional<Utf16FlyString>(static_cast<PrivateIdentifier const&>(class_method.key()).string()) : Optional<Utf16FlyString>(),
                .shared_function_data_index = data_index,
                .has_initializer = false,
                .literal_value = {},
            });
        } else if (is<ClassField>(*element)) {
            auto const& class_field = static_cast<ClassField const&>(*element);
            bool is_private = is<PrivateIdentifier>(class_field.key());

            Optional<u32> data_index;
            bool has_initializer = class_field.initializer() != nullptr;
            Optional<Value> literal_value;

            if (has_initializer) {
                auto const& initializer = *class_field.initializer();

                // Detect literal initializers and store the value directly,
                // avoiding function creation and calls for simple cases like x = 0.
                if (is<NumericLiteral>(initializer)) {
                    literal_value = static_cast<NumericLiteral const&>(initializer).value();
                } else if (is<BooleanLiteral>(initializer)) {
                    literal_value = static_cast<BooleanLiteral const&>(initializer).value();
                } else if (is<NullLiteral>(initializer)) {
                    literal_value = js_null();
                } else if (is<StringLiteral>(initializer)) {
                    literal_value = Value(PrimitiveString::create(vm, static_cast<StringLiteral const&>(initializer).value()));
                } else if (is<UnaryExpression>(initializer)) {
                    auto const& unary = static_cast<UnaryExpression const&>(initializer);
                    if (unary.op() == UnaryOp::Minus && is<NumericLiteral>(*unary.lhs()))
                        literal_value = Value(-static_cast<NumericLiteral const&>(*unary.lhs()).value().as_double());
                }

                if (!literal_value.has_value()) {
                    // FIXME: For computed-key fields, the field name for anonymous function
                    //        naming is only known at runtime. We use "" here, which means
                    //        e.g. (new (class { [sym] = function(){} }))[sym].name would be
                    //        "" instead of "[sym]". Non-computed keys are handled correctly.
                    Utf16FlyString field_name;
                    if (is_private) {
                        field_name = static_cast<PrivateIdentifier const&>(class_field.key()).string();
                    } else if (is<Identifier>(class_field.key())) {
                        field_name = static_cast<Identifier const&>(class_field.key()).string();
                    } else if (is<StringLiteral>(class_field.key())) {
                        field_name = Utf16FlyString(static_cast<StringLiteral const&>(class_field.key()).value());
                    } else if (is<NumericLiteral>(class_field.key())) {
                        field_name = Utf16FlyString(number_to_utf16_string(static_cast<NumericLiteral const&>(class_field.key()).value().as_double()));
                    } else if (is<BigIntLiteral>(class_field.key())) {
                        field_name = Utf16FlyString::from_utf8(bigint_literal_to_decimal_string(static_cast<BigIntLiteral const&>(class_field.key())));
                    }

                    auto copy_initializer = class_field.initializer();
                    auto function_code = create_ast_node<ClassFieldInitializerStatement>(
                        class_field.initializer()->source_range(),
                        copy_initializer.release_nonnull(),
                        move(field_name));

                    FunctionParsingInsights parsing_insights;
                    parsing_insights.uses_this_from_environment = true;
                    parsing_insights.uses_this = true;

                    auto shared_data = vm.heap().allocate<SharedFunctionInstanceData>(
                        vm,
                        FunctionKind::Normal,
                        "field"_utf16_fly_string,
                        0,
                        FunctionParameters::empty(),
                        *function_code,
                        Utf16View {},
                        true,
                        false,
                        parsing_insights,
                        Vector<LocalVariable> {});

                    // Set class_field_initializer_name for keys known at codegen time.
                    // This is needed so eval("arguments") inside field initializers
                    // correctly throws a SyntaxError.
                    if (is_private) {
                        auto private_name = static_cast<PrivateIdentifier const&>(class_field.key()).string();
                        shared_data->m_class_field_initializer_name = PrivateName(0, private_name);
                    } else if (is<Identifier>(class_field.key())) {
                        auto name = static_cast<Identifier const&>(class_field.key()).string();
                        shared_data->m_class_field_initializer_name = PropertyKey(name.to_utf16_string());
                    } else if (is<StringLiteral>(class_field.key())) {
                        auto name = static_cast<StringLiteral const&>(class_field.key()).value();
                        shared_data->m_class_field_initializer_name = PropertyKey(name);
                    } else if (is<NumericLiteral>(class_field.key())) {
                        auto name = number_to_utf16_string(static_cast<NumericLiteral const&>(class_field.key()).value().as_double());
                        shared_data->m_class_field_initializer_name = PropertyKey(name);
                    } else if (is<BigIntLiteral>(class_field.key())) {
                        auto name = bigint_literal_to_decimal_string(static_cast<BigIntLiteral const&>(class_field.key()));
                        shared_data->m_class_field_initializer_name = PropertyKey(Utf16String::from_utf8(name));
                    }
                    // For computed keys, class_field_initializer_name is set at runtime
                    // in construct_class().

                    data_index = generator.register_shared_function_data(shared_data);
                }
            }

            blueprint.elements.append({
                .kind = ClassElementDescriptor::Kind::Field,
                .is_static = element->is_static(),
                .is_private = is_private,
                .private_identifier = is_private ? Optional<Utf16FlyString>(static_cast<PrivateIdentifier const&>(class_field.key()).string()) : Optional<Utf16FlyString>(),
                .shared_function_data_index = data_index,
                .has_initializer = has_initializer,
                .literal_value = literal_value,
            });
        } else if (is<StaticInitializer>(*element)) {
            auto const& static_init = static_cast<StaticInitializer const&>(*element);

            FunctionParsingInsights parsing_insights;
            parsing_insights.uses_this_from_environment = true;
            parsing_insights.uses_this = true;

            auto shared_data = vm.heap().allocate<SharedFunctionInstanceData>(
                vm,
                FunctionKind::Normal,
                Utf16FlyString {},
                0,
                FunctionParameters::empty(),
                static_init.function_body(),
                Utf16View {},
                true,
                false,
                parsing_insights,
                static_init.function_body().local_variables_names());

            auto data_index = generator.register_shared_function_data(shared_data);

            blueprint.elements.append({
                .kind = ClassElementDescriptor::Kind::StaticInitializer,
                .is_static = true,
                .is_private = false,
                .private_identifier = {},
                .shared_function_data_index = data_index,
                .has_initializer = false,
                .literal_value = {},
            });
        }
    }

    auto blueprint_index = generator.register_class_blueprint(move(blueprint));

    // Restore parent environment before emitting NewClass.
    generator.emit<Bytecode::Op::SetLexicalEnvironment>(parent_environment);
    generator.pop_lexical_environment_register();

    auto dst = choose_dst(generator, preferred_dst);
    generator.emit_with_extra_slots<Op::NewClass, Optional<Operand>>(elements.size(), dst, super_class.has_value() ? super_class->operand() : Optional<Operand> {}, class_environment, blueprint_index, lhs_name, elements);

    if (did_emit_private_environment_allocation) {
        generator.emit<Op::LeavePrivateEnvironment>();
    }

    return dst;
}

Optional<ScopedOperand> ClassExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generate_bytecode_with_lhs_name(generator, {}, preferred_dst);
}

Optional<ScopedOperand> SpreadExpression::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    // NOTE: All users of this should handle the behaviour of this on their own,
    //       assuming it returns an Array-like object
    return m_target->generate_bytecode(generator);
}

Optional<ScopedOperand> ThisExpression::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generator.get_this(preferred_dst);
}

static ScopedOperand generate_await(
    Bytecode::Generator& generator,
    ScopedOperand argument,
    ScopedOperand received_completion,
    ScopedOperand received_completion_type,
    ScopedOperand received_completion_value)
{
    VERIFY(generator.is_in_async_function());

    auto& continuation_block = generator.make_block();
    generator.emit<Bytecode::Op::Await>(Bytecode::Label { continuation_block }, argument);
    generator.switch_to_basic_block(continuation_block);

    // FIXME: It's really magical that we can just assume that the completion value is in register 0.
    //        It ends up there because we "return" from the Await instruction above via the synthetic
    //        generator function that actually drives async execution.
    generator.emit_mov(received_completion, generator.accumulator());
    get_received_completion_type_and_value(generator, received_completion, received_completion_type, received_completion_value);

    auto& normal_completion_continuation_block = generator.make_block();
    auto& throw_value_block = generator.make_block();

    auto received_completion_type_is_normal = generator.allocate_register();
    generator.emit<Bytecode::Op::StrictlyEquals>(
        received_completion_type_is_normal,
        received_completion_type,
        generator.add_constant(Value(to_underlying(Completion::Type::Normal))));
    generator.emit_jump_if(
        received_completion_type_is_normal,
        Bytecode::Label { normal_completion_continuation_block },
        Bytecode::Label { throw_value_block });

    // Simplification: The only abrupt completion we receive from AsyncFunctionDriverWrapper or AsyncGenerator is Type::Throw
    //                 So we do not need to account for the Type::Return path
    generator.switch_to_basic_block(throw_value_block);
    generator.perform_needed_unwinds<Bytecode::Op::Throw>();
    generator.emit<Bytecode::Op::Throw>(received_completion_value);

    generator.switch_to_basic_block(normal_completion_continuation_block);
    return received_completion_value;
}

Optional<ScopedOperand> AwaitExpression::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto argument = m_argument->generate_bytecode(generator).value();

    auto received_completion = generator.allocate_register();
    auto received_completion_type = generator.allocate_register();
    auto received_completion_value = generator.allocate_register();

    generator.emit_mov(received_completion, generator.accumulator());

    return generate_await(generator, argument, received_completion, received_completion_type, received_completion_value);
}

Optional<ScopedOperand> WithStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto object = m_object->generate_bytecode(generator).value();

    auto object_environment = generator.allocate_register();
    generator.emit<Bytecode::Op::EnterObjectEnvironment>(object_environment, object);
    generator.push_lexical_environment_register(object_environment);

    // EnterObjectEnvironment sets the running execution context's lexical_environment to a new Object Environment.
    generator.start_boundary(Bytecode::Generator::BlockBoundaryType::LeaveLexicalEnvironment);

    auto body_result = m_body->generate_bytecode(generator);
    if (!body_result.has_value())
        body_result = generator.add_constant(js_undefined());
    generator.end_boundary(Bytecode::Generator::BlockBoundaryType::LeaveLexicalEnvironment);
    generator.pop_lexical_environment_register();

    if (!generator.is_current_block_terminated())
        generator.emit<Bytecode::Op::SetLexicalEnvironment>(generator.current_lexical_environment_register());

    return body_result;
}

enum class LHSKind {
    Assignment,
    VarBinding,
    LexicalBinding,
};

enum class IterationKind {
    Enumerate,
    Iterate,
    AsyncIterate,
};

// 14.7.5.6 ForIn/OfHeadEvaluation ( uninitializedBoundNames, expr, iterationKind ), https://tc39.es/ecma262/#sec-runtime-semantics-forinofheadevaluation
struct ForInOfHeadEvaluationResult {
    bool is_destructuring { false };
    LHSKind lhs_kind { LHSKind::Assignment };
    Optional<ScopedOperand> iterator_object;
    Optional<ScopedOperand> iterator_next_method;
    Optional<ScopedOperand> iterator_done_property;
};
static ForInOfHeadEvaluationResult for_in_of_head_evaluation(Bytecode::Generator& generator, IterationKind iteration_kind, Variant<NonnullRefPtr<ASTNode const>, NonnullRefPtr<BindingPattern const>> const& lhs, NonnullRefPtr<ASTNode const> const& rhs)
{
    ForInOfHeadEvaluationResult result {};

    bool entered_lexical_scope = false;
    if (auto* ast_ptr = lhs.get_pointer<NonnullRefPtr<ASTNode const>>(); ast_ptr && is<VariableDeclaration>(**ast_ptr)) {
        // Runtime Semantics: ForInOfLoopEvaluation, for any of:
        //  ForInOfStatement : for ( var ForBinding in Expression ) Statement
        //  ForInOfStatement : for ( ForDeclaration in Expression ) Statement
        //  ForInOfStatement : for ( var ForBinding of AssignmentExpression ) Statement
        //  ForInOfStatement : for ( ForDeclaration of AssignmentExpression ) Statement

        auto& variable_declaration = static_cast<VariableDeclaration const&>(**ast_ptr);
        result.is_destructuring = variable_declaration.declarations().first()->target().has<NonnullRefPtr<BindingPattern const>>();
        result.lhs_kind = variable_declaration.is_lexical_declaration() ? LHSKind::LexicalBinding : LHSKind::VarBinding;

        if (variable_declaration.declaration_kind() == DeclarationKind::Var) {
            // B.3.5 Initializers in ForIn Statement Heads, https://tc39.es/ecma262/#sec-initializers-in-forin-statement-heads
            auto& variable = variable_declaration.declarations().first();
            if (variable->init()) {
                VERIFY(variable->target().has<NonnullRefPtr<Identifier const>>());
                auto identifier = variable->target().get<NonnullRefPtr<Identifier const>>();
                auto identifier_table_ref = generator.intern_identifier(identifier->string());
                auto value = generator.emit_named_evaluation_if_anonymous_function(*variable->init(), identifier_table_ref);
                generator.emit_set_variable(*identifier, value);
            }
        } else {
            auto has_non_local_variables = false;
            MUST(variable_declaration.for_each_bound_identifier([&](auto const& identifier) {
                if (!identifier.is_local())
                    has_non_local_variables = true;
            }));

            if (has_non_local_variables) {
                // 1. Let oldEnv be the running execution context's LexicalEnvironment.
                // NOTE: 'uninitializedBoundNames' refers to the lexical bindings (i.e. Const/Let) present in the second and last form.
                // 2. If uninitializedBoundNames is not an empty List, then
                entered_lexical_scope = true;
                // a. Assert: uninitializedBoundNames has no duplicate entries.
                // b. Let newEnv be NewDeclarativeEnvironment(oldEnv).
                generator.begin_variable_scope();
                // c. For each String name of uninitializedBoundNames, do
                // NOTE: Nothing in the callback throws an exception.
                MUST(variable_declaration.for_each_bound_identifier([&](auto const& identifier) {
                    if (identifier.is_local())
                        return;
                    // i. Perform ! newEnv.CreateMutableBinding(name, false).
                    auto interned_identifier = generator.intern_identifier(identifier.string());
                    generator.emit<Bytecode::Op::CreateVariable>(interned_identifier, Bytecode::Op::EnvironmentMode::Lexical, false, false, false);
                }));
                // d. Set the running execution context's LexicalEnvironment to newEnv.
                // NOTE: Done by CreateLexicalEnvironment.
            }
        }
    } else {
        // Runtime Semantics: ForInOfLoopEvaluation, for any of:
        //  ForInOfStatement : for ( LeftHandSideExpression in Expression ) Statement
        //  ForInOfStatement : for ( LeftHandSideExpression of AssignmentExpression ) Statement
        result.lhs_kind = LHSKind::Assignment;
    }

    // 3. Let exprRef be the result of evaluating expr.
    auto object = rhs->generate_bytecode(generator).value();

    // 4. Set the running execution context's LexicalEnvironment to oldEnv.
    if (entered_lexical_scope)
        generator.end_variable_scope();

    // 5. Let exprValue be ? GetValue(exprRef).
    // NOTE: No need to store this anywhere.

    auto iterator_object = generator.allocate_register();
    auto iterator_next_method = generator.allocate_register();
    auto iterator_done_property = generator.allocate_register();

    // 6. If iterationKind is enumerate, then
    if (iteration_kind == IterationKind::Enumerate) {
        // a. If exprValue is undefined or null, then
        auto& nullish_block = generator.make_block();
        auto& continuation_block = generator.make_block();
        generator.emit<Bytecode::Op::JumpNullish>(
            object,
            Bytecode::Label { nullish_block },
            Bytecode::Label { continuation_block });

        // i. Return Completion Record { [[Type]]: break, [[Value]]: empty, [[Target]]: empty }.
        generator.switch_to_basic_block(nullish_block);
        generator.generate_break();

        generator.switch_to_basic_block(continuation_block);
        // b. Let obj be ! ToObject(exprValue).
        // NOTE: GetObjectPropertyIterator does this.
        // c. Let iterator be EnumerateObjectProperties(obj).
        // d. Let nextMethod be ! GetV(iterator, "next").
        // e. Return the Iterator Record { [[Iterator]]: iterator, [[NextMethod]]: nextMethod, [[Done]]: false }.
        generator.emit<Bytecode::Op::GetObjectPropertyIterator>(iterator_object, iterator_next_method, iterator_done_property, object);
    }
    // 7. Else,
    else {
        // a. Assert: iterationKind is iterate or async-iterate.
        // b. If iterationKind is async-iterate, let iteratorKind be async.
        // c. Else, let iteratorKind be sync.
        auto iterator_kind = iteration_kind == IterationKind::AsyncIterate ? IteratorHint::Async : IteratorHint::Sync;

        // d. Return ? GetIterator(exprValue, iteratorKind).
        generator.emit<Bytecode::Op::GetIterator>(iterator_object, iterator_next_method, iterator_done_property, object, iterator_kind);
    }

    result.iterator_object = iterator_object;
    result.iterator_next_method = iterator_next_method;
    result.iterator_done_property = iterator_done_property;
    return result;
}

// 14.7.5.7 ForIn/OfBodyEvaluation ( lhs, stmt, iteratorRecord, iterationKind, lhsKind, labelSet [ , iteratorKind ] ), https://tc39.es/ecma262/#sec-runtime-semantics-forin-div-ofbodyevaluation-lhs-stmt-iterator-lhskind-labelset
static Optional<ScopedOperand> for_in_of_body_evaluation(Bytecode::Generator& generator, Variant<NonnullRefPtr<ASTNode const>, NonnullRefPtr<BindingPattern const>> const& lhs, ASTNode const& body, ForInOfHeadEvaluationResult const& head_result, IterationKind iteration_kind, Vector<FlyString> const& label_set, Bytecode::BasicBlock& loop_end, Bytecode::BasicBlock& loop_update, IteratorHint iterator_kind = IteratorHint::Sync, [[maybe_unused]] Optional<ScopedOperand> preferred_dst = {})
{
    // 1. If iteratorKind is not present, set iteratorKind to sync.

    // 2. Let oldEnv be the running execution context's LexicalEnvironment.
    bool has_lexical_binding = false;

    // 3. Let V be undefined.
    Optional<ScopedOperand> completion;
    if (generator.must_propagate_completion()) {
        completion = generator.allocate_register();
        generator.emit_mov(*completion, generator.add_constant(js_undefined()));
    }

    // 4. Let destructuring be IsDestructuring of lhs.
    auto destructuring = head_result.is_destructuring;

    // 5. If destructuring is true and if lhsKind is assignment, then
    // NB: is_destructuring is only set for VariableDeclaration lhs (which always has lhs_kind
    // VarBinding or LexicalBinding), so this combination is unreachable.
    VERIFY(!(destructuring && head_result.lhs_kind == LHSKind::Assignment));
    if (completion.has_value())
        generator.set_current_breakable_scope_completion_register(*completion);

    // For for-of and for-await-of, set up a synthetic FinallyContext so that
    // IteratorClose/AsyncIteratorClose is called on abrupt completion (break,
    // return, throw, or continue-to-outer-loop). for-in (enumerate) does not
    // need iterator close per spec.
    bool needs_iterator_close = (iteration_kind != IterationKind::Enumerate);

    Optional<Bytecode::Generator::FinallyContext> iterator_close_finally_context;
    Optional<Bytecode::Generator::UnwindContext> iterator_close_unwind_context;
    Optional<ScopedOperand> close_completion_type;
    Optional<ScopedOperand> close_completion_value;
    Bytecode::BasicBlock* exception_preamble_block { nullptr };
    Bytecode::BasicBlock* iterator_close_body_block { nullptr };
    Optional<ScopedOperand> lexical_environment_at_entry;

    if (needs_iterator_close) {
        lexical_environment_at_entry = generator.current_lexical_environment_register();
        close_completion_type = generator.allocate_register();
        close_completion_value = generator.allocate_register();

        exception_preamble_block = &generator.make_block();
        iterator_close_body_block = &generator.make_block();

        iterator_close_finally_context.emplace(Bytecode::Generator::FinallyContext {
            .completion_type = *close_completion_type,
            .completion_value = *close_completion_value,
            .finally_body = Bytecode::Label { *iterator_close_body_block },
            .exception_preamble = Bytecode::Label { *exception_preamble_block },
            .parent = generator.current_finally_context(),
            .registered_jumps = {},
            .next_jump_index = Bytecode::Generator::FinallyContext::FIRST_JUMP_INDEX,
            .lexical_environment_at_entry = lexical_environment_at_entry,
        });
        generator.set_current_finally_context(&*iterator_close_finally_context);

        // Place ReturnToFinally between Break (pushed by caller) and Continue
        // (pushed by begin_continuable_scope below). This ensures:
        //   - continue to this loop: hits Continue first -> direct jump (no close)
        //   - break/return/throw/continue-to-outer: hits ReturnToFinally -> close
        generator.start_boundary(Bytecode::Generator::BlockBoundaryType::ReturnToFinally);

        // NB: The UnwindContext (exception handler) is set up later, after
        //     the iterator-next section. Per spec, exceptions from steps a-f
        //     (IteratorNext, Await, IteratorComplete, IteratorValue) propagate
        //     directly without calling IteratorClose. Only exceptions from
        //     LHS assignment (steps g-j) and the loop body (step l) should
        //     trigger iterator close.
    }

    // 6. Repeat,
    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { loop_update });
    generator.switch_to_basic_block(loop_update);
    generator.begin_continuable_scope(Bytecode::Label { loop_update }, label_set, completion);

    // a. Let nextResult be ? Call(iteratorRecord.[[NextMethod]], iteratorRecord.[[Iterator]]).
    auto next_value = generator.allocate_register();
    auto done = generator.allocate_register();

    if (iterator_kind == IteratorHint::Sync) {
        generator.emit<Bytecode::Op::IteratorNextUnpack>(next_value, done, *head_result.iterator_object, *head_result.iterator_next_method, *head_result.iterator_done_property);

        auto& loop_continue = generator.make_block();
        generator.emit_jump_if(
            done,
            Bytecode::Label { loop_end },
            Bytecode::Label { loop_continue });
        generator.switch_to_basic_block(loop_continue);
    } else {
        auto next_result = generator.allocate_register();
        generator.emit<Bytecode::Op::IteratorNext>(next_result, *head_result.iterator_object, *head_result.iterator_next_method, *head_result.iterator_done_property);

        // b. If iteratorKind is async, set nextResult to ? Await(nextResult).
        auto received_completion = generator.allocate_register();
        auto received_completion_type = generator.allocate_register();
        auto received_completion_value = generator.allocate_register();

        generator.emit_mov(received_completion, generator.accumulator());
        auto new_result = generate_await(generator, next_result, received_completion, received_completion_type, received_completion_value);
        generator.emit_mov(next_result, new_result);

        // c. If Type(nextResult) is not Object, throw a TypeError exception.
        generator.emit<Bytecode::Op::ThrowIfNotObject>(next_result);

        // d. Let done be ? IteratorComplete(nextResult).
        generator.emit_iterator_complete(done, next_result);

        // e. If done is true, return V.
        auto& loop_continue = generator.make_block();
        generator.emit_jump_if(
            done,
            Bytecode::Label { loop_end },
            Bytecode::Label { loop_continue });
        generator.switch_to_basic_block(loop_continue);

        // f. Let nextValue be ? IteratorValue(nextResult).
        generator.emit_iterator_value(next_value, next_result);
    }

    // Set up the exception handler now, after the iterator-next section.
    // This ensures only LHS assignment and body exceptions trigger close.
    // We must also switch to a fresh block so that subsequent code gets the
    // new handler (make_block sets the handler at creation time).
    if (needs_iterator_close) {
        iterator_close_unwind_context.emplace(generator, Bytecode::Label { *exception_preamble_block });
        auto& loop_body = generator.make_block();
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { loop_body });
        generator.switch_to_basic_block(loop_body);
    }

    // g. If lhsKind is either assignment or varBinding, then
    if (head_result.lhs_kind != LHSKind::LexicalBinding) {
        // i. If destructuring is false, then
        if (!destructuring) {
            // 1. Let lhsRef be the result of evaluating lhs. (It may be evaluated repeatedly.)
            // NOTE: We're skipping all the completion stuff that the spec does, as the unwinding mechanism will take case of doing that.
            if (head_result.lhs_kind == LHSKind::VarBinding) {
                auto& declaration = static_cast<VariableDeclaration const&>(*lhs.get<NonnullRefPtr<ASTNode const>>());
                VERIFY(declaration.declarations().size() == 1);
                assign_value_to_variable_declarator(generator, declaration.declarations().first(), declaration, next_value);
            } else {
                if (auto ptr = lhs.get_pointer<NonnullRefPtr<ASTNode const>>()) {
                    generator.emit_store_to_reference(**ptr, next_value);
                } else {
                    auto& binding_pattern = lhs.get<NonnullRefPtr<BindingPattern const>>();
                    binding_pattern->generate_bytecode(generator, Bytecode::Op::BindingInitializationMode::Set, next_value);
                }
            }
        }
    }
    // h. Else,
    else {
        // i. Assert: lhsKind is lexicalBinding.
        // ii. Assert: lhs is a ForDeclaration.
        // iii. Let iterationEnv be NewDeclarativeEnvironment(oldEnv).
        // iv. Perform ForDeclarationBindingInstantiation of lhs with argument iterationEnv.
        // v. Set the running execution context's LexicalEnvironment to iterationEnv.

        // 14.7.5.4 Runtime Semantics: ForDeclarationBindingInstantiation, https://tc39.es/ecma262/#sec-runtime-semantics-fordeclarationbindinginstantiation
        // 1. Assert: environment is a declarative Environment Record.
        // NOTE: We just made it.
        auto& variable_declaration = static_cast<VariableDeclaration const&>(*lhs.get<NonnullRefPtr<ASTNode const>>());
        // 2. For each element name of the BoundNames of ForBinding, do
        // NOTE: Nothing in the callback throws an exception.

        auto has_non_local_variables = false;
        MUST(variable_declaration.for_each_bound_identifier([&](auto const& identifier) {
            if (!identifier.is_local())
                has_non_local_variables = true;
        }));

        if (has_non_local_variables) {
            generator.begin_variable_scope();
            has_lexical_binding = true;

            MUST(variable_declaration.for_each_bound_identifier([&](auto const& identifier) {
                if (identifier.is_local())
                    return;
                auto interned_identifier = generator.intern_identifier(identifier.string());
                // a. If IsConstantDeclaration of LetOrConst is true, then
                if (variable_declaration.is_constant_declaration()) {
                    // i. Perform ! environment.CreateImmutableBinding(name, true).
                    generator.emit<Bytecode::Op::CreateVariable>(interned_identifier, Bytecode::Op::EnvironmentMode::Lexical, true, false, true);
                }
                // b. Else,
                else {
                    // i. Perform ! environment.CreateMutableBinding(name, false).
                    generator.emit<Bytecode::Op::CreateVariable>(interned_identifier, Bytecode::Op::EnvironmentMode::Lexical, false, false, false);
                }
            }));
            // 3. Return unused.
            // NOTE: No need to do that as we've inlined this.
        }
        // vi. If destructuring is false, then
        if (!destructuring) {
            // 1. Assert: lhs binds a single name.
            // 2. Let lhsName be the sole element of BoundNames of lhs.
            auto lhs_name = variable_declaration.declarations().first()->target().get<NonnullRefPtr<Identifier const>>();
            // 3. Let lhsRef be ! ResolveBinding(lhsName).
            // NOTE: We're skipping all the completion stuff that the spec does, as the unwinding mechanism will take case of doing that.

            generator.emit_set_variable(*lhs_name, next_value, Bytecode::Op::BindingInitializationMode::Initialize, Bytecode::Op::EnvironmentMode::Lexical);
        }
    }
    // i. If destructuring is false, then
    if (!destructuring) {
        // i. If lhsRef is an abrupt completion, then
        //     1. Let status be lhsRef.
        // ii. Else if lhsKind is lexicalBinding, then
        //     1. Let status be Completion(InitializeReferencedBinding(lhsRef, nextValue)).
        // iii. Else,
        //     1. Let status be Completion(PutValue(lhsRef, nextValue)).
        // NOTE: This is performed above.
    }
    //    j. Else,
    else {
        // FIXME: i. If lhsKind is assignment, then
        //           1. Let status be Completion(DestructuringAssignmentEvaluation of assignmentPattern with argument nextValue).

        //  ii. Else if lhsKind is varBinding, then
        //      1. Assert: lhs is a ForBinding.
        //      2. Let status be Completion(BindingInitialization of lhs with arguments nextValue and undefined).
        //  iii. Else,
        //      1. Assert: lhsKind is lexicalBinding.
        //      2. Assert: lhs is a ForDeclaration.
        //      3. Let status be Completion(ForDeclarationBindingInitialization of lhs with arguments nextValue and iterationEnv).
        if (head_result.lhs_kind == LHSKind::VarBinding || head_result.lhs_kind == LHSKind::LexicalBinding) {
            auto& declaration = static_cast<VariableDeclaration const&>(*lhs.get<NonnullRefPtr<ASTNode const>>());
            VERIFY(declaration.declarations().size() == 1);
            auto& binding_pattern = declaration.declarations().first()->target().get<NonnullRefPtr<BindingPattern const>>();
            binding_pattern->generate_bytecode(
                generator,
                head_result.lhs_kind == LHSKind::VarBinding ? Bytecode::Op::BindingInitializationMode::Set : Bytecode::Op::BindingInitializationMode::Initialize,
                next_value);
        } else {
            // NB: lhs_kind is Assignment only when is_destructuring is false, so this is unreachable.
            VERIFY_NOT_REACHED();
        }
    }

    // k. If status is an abrupt completion, then
    //     i. Set the running execution context's LexicalEnvironment to oldEnv.
    //     ii. If iteratorKind is async, return ? AsyncIteratorClose(iteratorRecord, status).
    //     iii. If iterationKind is enumerate, then
    //         1. Return ? status.
    //     iv. Else,
    //         1. Assert: iterationKind is iterate.
    //         2. Return ? IteratorClose(iteratorRecord, status).
    // NB: Abrupt completions from LHS assignment and the loop body are handled
    //     by the synthetic FinallyContext set up above (for iterate/async-iterate).

    // l. Let result be the result of evaluating stmt.
    {
        Optional<Bytecode::Generator::CompletionRegisterScope> completion_scope;
        if (completion.has_value())
            completion_scope.emplace(generator, *completion);
        auto result = body.generate_bytecode(generator);
        if (!generator.is_current_block_terminated() && completion.has_value() && result.has_value())
            generator.emit_mov(*completion, *result);
    }

    // m. Set the running execution context's LexicalEnvironment to oldEnv.
    if (has_lexical_binding)
        generator.end_variable_scope();
    generator.end_continuable_scope();

    if (needs_iterator_close) {
        generator.end_boundary(Bytecode::Generator::BlockBoundaryType::ReturnToFinally);
        generator.set_current_finally_context(iterator_close_finally_context->parent);
        iterator_close_unwind_context.clear();
    }

    generator.end_breakable_scope();

    // The body can contain an unconditional block terminator (e.g. return, throw), so we have to check for that before generating the Jump.
    if (!generator.is_current_block_terminated())
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { loop_update });

    // Generate iterator close blocks for for-of/for-await-of.
    if (needs_iterator_close) {
        auto undefined_value = generator.add_constant(js_undefined());

        // Exception preamble: catches thrown exceptions and routes to iterator close.
        generator.switch_to_basic_block(*exception_preamble_block);
        generator.emit<Bytecode::Op::Catch>(*close_completion_value);
        generator.emit<Bytecode::Op::SetLexicalEnvironment>(*lexical_environment_at_entry);
        generator.emit_mov(*close_completion_type, generator.add_constant(Value(Bytecode::Generator::FinallyContext::THROW)));
        generator.emit<Bytecode::Op::Jump>(Bytecode::Label { *iterator_close_body_block });

        // Iterator close body: dispatch chain based on completion type.
        generator.switch_to_basic_block(*iterator_close_body_block);

        // THROW path: IteratorClose with Throw completion (original throw always wins).
        auto& throw_close_block = generator.make_block();
        auto& non_throw_close_block = generator.make_block();
        generator.emit<Bytecode::Op::JumpStrictlyEquals>(
            *close_completion_type, generator.add_constant(Value(Bytecode::Generator::FinallyContext::THROW)),
            Bytecode::Label { throw_close_block }, Bytecode::Label { non_throw_close_block });

        // Non-throw abrupt path (break/return/continue-to-outer): close with Normal completion.
        generator.switch_to_basic_block(non_throw_close_block);

        if (iterator_kind == IteratorHint::Async) {
            // For async iterators, we inline the AsyncIteratorClose steps
            // using a proper Await op instead of the synchronous await()
            // that the AsyncIteratorClose C++ op uses. The synchronous await
            // spins the event loop inside bytecode execution, which violates
            // the microtask checkpoint assertion.
            auto& after_close = generator.make_block();

            // Spec: 7.4.13 AsyncIteratorClose ( iteratorRecord, completion )
            // 3. Let innerResult be Completion(GetMethod(iterator, "return")).
            auto return_method = generator.allocate_register();
            generator.emit<Bytecode::Op::GetMethod>(return_method, *head_result.iterator_object, generator.intern_property_key("return"_utf16_fly_string));

            // 4a/b. If return is undefined, skip close.
            auto& call_return_block = generator.make_block();
            generator.emit<Bytecode::Op::JumpUndefined>(return_method, Bytecode::Label { after_close }, Bytecode::Label { call_return_block });
            generator.switch_to_basic_block(call_return_block);

            // 4c. Set innerResult to Completion(Call(return, iterator)).
            auto inner_result = generator.allocate_register();
            generator.emit_with_extra_operand_slots<Bytecode::Op::Call>(0, inner_result, return_method, *head_result.iterator_object, OptionalNone {}, ReadonlySpan<ScopedOperand> {});

            // 4d. Set innerResult to Completion(Await(innerResult.[[Value]])).
            auto received_completion = generator.allocate_register();
            auto received_completion_type = generator.allocate_register();
            auto received_completion_value = generator.allocate_register();
            auto awaited = generate_await(generator, inner_result, received_completion, received_completion_type, received_completion_value);

            // 7. If Type(innerResult.[[Value]]) is not Object, throw a TypeError exception.
            generator.emit<Bytecode::Op::ThrowIfNotObject>(awaited);

            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { after_close });
            generator.switch_to_basic_block(after_close);
        } else {
            generator.emit<Bytecode::Op::IteratorClose>(*head_result.iterator_object, *head_result.iterator_next_method, *head_result.iterator_done_property, Completion::Type::Normal, undefined_value);
        }

        // Dispatch registered jumps (break/continue targets, indices 3+).
        for (auto const& jump : iterator_close_finally_context->registered_jumps) {
            auto& after_jump_check = generator.make_block();
            generator.emit<Bytecode::Op::JumpStrictlyEquals>(
                *close_completion_type, generator.add_constant(Value(jump.index)),
                jump.target, Bytecode::Label { after_jump_check });
            generator.switch_to_basic_block(after_jump_check);
        }

        // RETURN path.
        auto& return_block = generator.make_block();
        auto& unreachable_block = generator.make_block();
        generator.emit<Bytecode::Op::JumpStrictlyEquals>(
            *close_completion_type, generator.add_constant(Value(Bytecode::Generator::FinallyContext::RETURN)),
            Bytecode::Label { return_block }, Bytecode::Label { unreachable_block });

        generator.switch_to_basic_block(return_block);
        if (iterator_close_finally_context->parent) {
            // Nested finally: copy completion record to outer and jump to outer finally body.
            auto& outer = *iterator_close_finally_context->parent;
            generator.emit_mov(outer.completion_type, *close_completion_type);
            generator.emit_mov(outer.completion_value, *close_completion_value);
            generator.emit<Bytecode::Op::Jump>(outer.finally_body);
        } else {
            if (generator.is_in_generator_function())
                generator.emit<Bytecode::Op::Yield>(OptionalNone {}, *close_completion_value);
            else
                generator.emit<Bytecode::Op::Return>(*close_completion_value);
        }

        // Default: unreachable (all completion types have been dispatched).
        generator.switch_to_basic_block(unreachable_block);
        generator.emit<Bytecode::Op::Throw>(*close_completion_value);

        // Throw close block: IteratorClose with Throw completion, then rethrow.
        // Per spec step 5, the original throw always takes precedence.
        generator.switch_to_basic_block(throw_close_block);
        if (iterator_kind == IteratorHint::Async) {
            // Inline AsyncIteratorClose with exception handler: any error from
            // the close steps is discarded and the original exception is rethrown.
            auto& rethrow_block = generator.make_block();
            auto& close_catch_block = generator.make_block();

            {
                Bytecode::Generator::UnwindContext close_unwind(generator, Bytecode::Label { close_catch_block });

                // Jump to a block created inside the UnwindContext so that
                // GetMethod/Call/Await all have the exception handler set.
                // throw_close_block was created before the UnwindContext and
                // doesn't have the handler.
                auto& close_try_block = generator.make_block();
                generator.emit<Bytecode::Op::Jump>(Bytecode::Label { close_try_block });
                generator.switch_to_basic_block(close_try_block);

                auto return_method = generator.allocate_register();
                generator.emit<Bytecode::Op::GetMethod>(return_method, *head_result.iterator_object, generator.intern_property_key("return"_utf16_fly_string));

                auto& call_return_block = generator.make_block();
                generator.emit<Bytecode::Op::JumpUndefined>(return_method, Bytecode::Label { rethrow_block }, Bytecode::Label { call_return_block });
                generator.switch_to_basic_block(call_return_block);

                auto inner_result = generator.allocate_register();
                generator.emit_with_extra_operand_slots<Bytecode::Op::Call>(0, inner_result, return_method, *head_result.iterator_object, OptionalNone {}, ReadonlySpan<ScopedOperand> {});

                auto received_completion = generator.allocate_register();
                auto received_completion_type = generator.allocate_register();
                auto received_completion_value = generator.allocate_register();
                generate_await(generator, inner_result, received_completion, received_completion_type, received_completion_value);

                // Even if close succeeded, rethrow original (spec step 5).
                generator.emit<Bytecode::Op::Jump>(Bytecode::Label { rethrow_block });
            }

            // Exception handler: discard close error, rethrow original.
            generator.switch_to_basic_block(close_catch_block);
            auto discarded = generator.allocate_register();
            generator.emit<Bytecode::Op::Catch>(discarded);
            generator.emit<Bytecode::Op::Jump>(Bytecode::Label { rethrow_block });

            generator.switch_to_basic_block(rethrow_block);
            generator.emit<Bytecode::Op::Throw>(*close_completion_value);
        } else {
            generator.emit<Bytecode::Op::IteratorClose>(*head_result.iterator_object, *head_result.iterator_next_method, *head_result.iterator_done_property, Completion::Type::Throw, *close_completion_value);
            // iterator_close with Throw completion always re-throws, but if it
            // somehow returns normally, rethrow the original exception.
            if (!generator.is_current_block_terminated())
                generator.emit<Bytecode::Op::Throw>(*close_completion_value);
        }
    }

    generator.switch_to_basic_block(loop_end);
    return completion;
}

Optional<ScopedOperand> ForInStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generate_labelled_evaluation(generator, {});
}

// 14.7.5.5 Runtime Semantics: ForInOfLoopEvaluation, https://tc39.es/ecma262/#sec-runtime-semantics-forinofloopevaluation
Optional<ScopedOperand> ForInStatement::generate_labelled_evaluation(Bytecode::Generator& generator, Vector<FlyString> const& label_set, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    auto& loop_end = generator.make_block();
    auto& loop_update = generator.make_block();
    generator.begin_breakable_scope(Bytecode::Label { loop_end }, label_set);

    auto head_result = for_in_of_head_evaluation(generator, IterationKind::Enumerate, m_lhs, m_rhs);
    return for_in_of_body_evaluation(generator, m_lhs, body(), head_result, IterationKind::Enumerate, label_set, loop_end, loop_update);
}

Optional<ScopedOperand> ForOfStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generate_labelled_evaluation(generator, {});
}

Optional<ScopedOperand> ForOfStatement::generate_labelled_evaluation(Bytecode::Generator& generator, Vector<FlyString> const& label_set, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    auto& loop_end = generator.make_block();
    auto& loop_update = generator.make_block();
    generator.begin_breakable_scope(Bytecode::Label { loop_end }, label_set);

    auto head_result = for_in_of_head_evaluation(generator, IterationKind::Iterate, m_lhs, m_rhs);
    return for_in_of_body_evaluation(generator, m_lhs, body(), head_result, IterationKind::Iterate, label_set, loop_end, loop_update);
}

Optional<ScopedOperand> ForAwaitOfStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    return generate_labelled_evaluation(generator, {});
}

Optional<ScopedOperand> ForAwaitOfStatement::generate_labelled_evaluation(Bytecode::Generator& generator, Vector<FlyString> const& label_set, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    auto& loop_end = generator.make_block();
    auto& loop_update = generator.make_block();
    generator.begin_breakable_scope(Bytecode::Label { loop_end }, label_set);

    auto head_result = for_in_of_head_evaluation(generator, IterationKind::AsyncIterate, m_lhs, m_rhs);
    return for_in_of_body_evaluation(generator, m_lhs, m_body, head_result, IterationKind::AsyncIterate, label_set, loop_end, loop_update, IteratorHint::Async);
}

// 13.3.12.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-meta-properties-runtime-semantics-evaluation
Optional<ScopedOperand> MetaProperty::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    // NewTarget : new . target
    if (m_type == MetaProperty::Type::NewTarget) {
        // 1. Return GetNewTarget().
        auto dst = choose_dst(generator, preferred_dst);
        generator.emit<Bytecode::Op::GetNewTarget>(dst);
        return dst;
    }

    // ImportMeta : import . meta
    if (m_type == MetaProperty::Type::ImportMeta) {
        auto dst = choose_dst(generator, preferred_dst);
        generator.emit<Bytecode::Op::GetImportMeta>(dst);
        return dst;
    }

    VERIFY_NOT_REACHED();
}

Optional<ScopedOperand> ClassFieldInitializerStatement::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto value = generator.emit_named_evaluation_if_anonymous_function(*m_expression, generator.intern_identifier(m_class_field_identifier_name), preferred_dst);
    generator.perform_needed_unwinds<Bytecode::Op::Return>();
    generator.emit<Bytecode::Op::Return>(value.operand());
    return value;
}

static void generate_optional_chain(Bytecode::Generator& generator, OptionalChain const& optional_chain, ScopedOperand current_value, ScopedOperand current_base, [[maybe_unused]] Optional<ScopedOperand> preferred_dst)
{
    Optional<ScopedOperand> new_current_value;
    if (is<MemberExpression>(optional_chain.base())) {
        auto& member_expression = static_cast<MemberExpression const&>(optional_chain.base());
        auto base_and_value = get_base_and_value_from_member_expression(generator, member_expression);
        new_current_value = base_and_value.value;
        generator.emit_mov(current_base, base_and_value.base);
    } else if (is<OptionalChain>(optional_chain.base())) {
        auto& sub_optional_chain = static_cast<OptionalChain const&>(optional_chain.base());
        generate_optional_chain(generator, sub_optional_chain, current_value, current_base);
        new_current_value = current_value;
    } else {
        new_current_value = optional_chain.base().generate_bytecode(generator).value();
    }

    generator.emit_mov(current_value, *new_current_value);

    auto& load_undefined_and_jump_to_end_block = generator.make_block();
    auto& end_block = generator.make_block();

    for (auto& reference : optional_chain.references()) {
        auto is_optional = reference.visit([](auto& ref) { return ref.mode; }) == OptionalChain::Mode::Optional;
        if (is_optional) {
            auto& not_nullish_block = generator.make_block();
            generator.emit<Bytecode::Op::JumpNullish>(
                current_value,
                Bytecode::Label { load_undefined_and_jump_to_end_block },
                Bytecode::Label { not_nullish_block });
            generator.switch_to_basic_block(not_nullish_block);
        }

        reference.visit(
            [&](OptionalChain::Call const& call) -> void {
                auto arguments = arguments_to_array_for_call(generator, call.arguments).value();
                generator.emit<Bytecode::Op::CallWithArgumentArray>(current_value, current_value, current_base, arguments, OptionalNone {});
                generator.emit_mov(current_base, generator.add_constant(js_undefined()));
            },
            [&](OptionalChain::ComputedReference const& ref) -> void {
                generator.emit_mov(current_base, current_value);
                auto property = ref.expression->generate_bytecode(generator).value();
                generator.emit_get_by_value(current_value, current_value, property);
            },
            [&](OptionalChain::MemberReference const& ref) -> void {
                generator.emit_mov(current_base, current_value);
                generator.emit_get_by_id(current_value, current_value, generator.intern_property_key(ref.identifier->string()));
            },
            [&](OptionalChain::PrivateMemberReference const& ref) -> void {
                generator.emit_mov(current_base, current_value);
                generator.emit<Bytecode::Op::GetPrivateById>(current_value, current_value, generator.intern_identifier(ref.private_identifier->string()));
            });
    }

    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { end_block });

    generator.switch_to_basic_block(load_undefined_and_jump_to_end_block);
    generator.emit_mov(current_value, generator.add_constant(js_undefined()));
    generator.emit<Bytecode::Op::Jump>(Bytecode::Label { end_block });

    generator.switch_to_basic_block(end_block);
}

Optional<ScopedOperand> OptionalChain::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto current_base = generator.allocate_register();
    auto current_value = choose_dst(generator, preferred_dst);
    generator.emit_mov(current_base, generator.add_constant(js_undefined()));
    generate_optional_chain(generator, *this, current_value, current_base);
    return current_value;
}

Optional<ScopedOperand> ImportCall::generate_bytecode(Bytecode::Generator& generator, Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    auto specifier = m_specifier->generate_bytecode(generator).value();

    Optional<ScopedOperand> options;
    if (m_options) {
        options = m_options->generate_bytecode(generator).value();
    } else {
        options = generator.add_constant(js_undefined());
    }
    auto dst = choose_dst(generator, preferred_dst);
    generator.emit<Bytecode::Op::ImportCall>(dst, specifier, *options);
    return dst;
}

Optional<ScopedOperand> ExportStatement::generate_bytecode(Bytecode::Generator& generator, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    Bytecode::Generator::SourceLocationScope scope(generator, *this);
    if (!is_default_export()) {
        if (m_statement) {
            return m_statement->generate_bytecode(generator);
        }
        return Optional<ScopedOperand> {};
    }

    VERIFY(m_statement);

    if (is<FunctionDeclaration>(*m_statement) || is<ClassDeclaration>(*m_statement)) {
        return m_statement->generate_bytecode(generator);
    }

    if (is<ClassExpression>(*m_statement)) {
        auto value = generator.emit_named_evaluation_if_anonymous_function(static_cast<ClassExpression const&>(*m_statement), generator.intern_identifier("default"_utf16_fly_string));

        if (!static_cast<ClassExpression const&>(*m_statement).has_name()) {
            generator.emit<Bytecode::Op::InitializeLexicalBinding>(
                generator.intern_identifier(ExportStatement::local_name_for_default),
                value);
        }

        return value;
    }

    // ExportDeclaration : export default AssignmentExpression ;
    VERIFY(is<Expression>(*m_statement));
    auto value = generator.emit_named_evaluation_if_anonymous_function(static_cast<Expression const&>(*m_statement), generator.intern_identifier("default"_utf16_fly_string));
    generator.emit<Bytecode::Op::InitializeLexicalBinding>(
        generator.intern_identifier(ExportStatement::local_name_for_default),
        value);
    return value;
}

Optional<ScopedOperand> ImportStatement::generate_bytecode(Bytecode::Generator&, [[maybe_unused]] Optional<ScopedOperand> preferred_dst) const
{
    return Optional<ScopedOperand> {};
}

}
