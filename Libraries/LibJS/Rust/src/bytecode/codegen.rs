/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bytecode generation from AST.
//!
//! This is the largest module in the parser -- it walks the AST
//! and emits bytecode instructions via the `Generator`.
//!
//! ## Conventions
//!
//! Each AST node's codegen returns `Option<ScopedOperand>`:
//! - `Some(op)` if the node produces a value (expressions)
//! - `None` for statements that don't produce values
//!
//! The `preferred_dst` parameter is a register hint: when the caller
//! already has a destination register (e.g. the LHS of an assignment),
//! codegen writes directly there instead of allocating a temporary.
//!
//! ## File organization
//!
//! The file is organized by AST node type, with section headers:
//!
//! - **Top-level entry points**: `generate_expression`, `generate_statement`
//! - **Literals and identifiers**: numeric, string, boolean, regexp, identifier
//! - **Await/yield**: async/generator control flow helpers
//! - **Operators**: binary, logical, conditional, update, assignment
//! - **Control flow**: if, while, do-while, for, for-in/of, switch, labelled
//! - **Blocks and scopes**: block statements, function bodies, scope children
//! - **Declarations**: variable declarations, using declarations
//! - **Calls**: regular calls, super calls, optional chains, builtin detection
//! - **Templates**: template literals, tagged templates
//! - **Objects and classes**: object expressions, class expressions
//! - **Patterns**: binding pattern destructuring (array and object)
//! - **Try/catch/finally**: try statement codegen
//! - **Functions**: `emit_new_function`, `emit_function_declaration_instantiation`
//! - **Helpers**: constant folding, NaN-boxing, error message utilities

use std::collections::HashSet;

use num_bigint::BigInt;
use num_traits::{One, Signed, ToPrimitive, Zero};

use crate::ast::*;
use crate::lexer::ch;
use crate::u32_from_usize;

use super::generator::{choose_dst, constant_to_boolean, parse_bigint, BlockBoundaryType, ConstantValue, FinallyContext, Generator, ScopedOperand};
use super::instruction::Instruction;
use super::operand::*;

/// Generate bytecode for an expression.
pub fn generate_expression(
    expression: &Expression,
    gen: &mut Generator,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let saved_source_start = gen.current_source_start;
    let saved_source_end = gen.current_source_end;
    gen.current_source_start = expression.range.start.offset;
    gen.current_source_end = expression.range.end.offset;

    let result = generate_expression_inner(expression, gen, preferred_dst);

    gen.current_source_start = saved_source_start;
    gen.current_source_end = saved_source_end;
    result
}

fn generate_expression_inner(
    expression: &Expression,
    gen: &mut Generator,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // NamedEvaluation: only function/class expressions consume pending_lhs_name.
    // Clear it for all other expression types so it doesn't leak through to
    // nested function expressions (e.g. IIFEs: `let x = (function() { ... })()`).
    if !matches!(expression.inner, ExpressionKind::Function(_) | ExpressionKind::Class(_)) {
        gen.pending_lhs_name = None;
    }

    let result = match &expression.inner {
        // === Literals ===
        ExpressionKind::NumericLiteral(value) => Some(gen.add_constant_number(*value)),

        ExpressionKind::BooleanLiteral(value) => Some(gen.add_constant_boolean(*value)),

        ExpressionKind::NullLiteral => Some(gen.add_constant_null()),

        ExpressionKind::StringLiteral(value) => Some(gen.add_constant_string(value.clone())),

        ExpressionKind::BigIntLiteral(value) => {
            // The AST stores the raw value including the 'n' suffix; strip it for codegen.
            let digits = value.strip_suffix('n').unwrap_or(value);
            Some(gen.add_constant_bigint(digits.to_string()))
        }

        ExpressionKind::RegExpLiteral(data) => {
            let source_index = gen.intern_string(&data.pattern);
            let flags_index = gen.intern_string(&data.flags);
            let compiled = data.compiled_regex.take();
            let regex_index = gen.intern_regex(compiled);
            let dst = choose_dst(gen, preferred_dst);
            gen.emit(Instruction::NewRegExp {
                dst: dst.operand(),
                source_index,
                flags_index,
                regex_index,
            });
            Some(dst)
        }

        // === Identifiers ===
        ExpressionKind::Identifier(ident) => generate_identifier(ident, gen, preferred_dst),

        // === This ===
        ExpressionKind::This => {
            // OPTIMIZATION: When function_environment_needed is false, the `this`
            // value is inherited from the outer function and already in the register.
            if gen.function_environment_needed {
                emit_resolve_this_if_needed(gen);
            }
            Some(gen.this_value())
        }

        // === Unary ===
        ExpressionKind::Unary { op, operand } => {
            generate_unary_expression(gen, *op, operand, preferred_dst)
        }

        // === Binary ===
        ExpressionKind::Binary { op, lhs, rhs } => {
            generate_binary_expression(gen, *op, lhs, rhs, preferred_dst)
        }

        // === Logical (short-circuit) ===
        ExpressionKind::Logical { op, lhs, rhs } => {
            generate_logical(gen, *op, lhs, rhs, preferred_dst)
        }

        // === Conditional (ternary) ===
        ExpressionKind::Conditional {
            test,
            consequent,
            alternate,
        } => {
            generate_conditional(gen, test, consequent, alternate, preferred_dst)
        }

        // === Sequence ===
        ExpressionKind::Sequence(expressions) => {
            let mut last = None;
            for expression in expressions {
                last = generate_expression(expression, gen, None);
                if gen.is_current_block_terminated() {
                    break;
                }
            }
            last
        }

        // === Function expressions ===
        ExpressionKind::Function(function_id) => {
            generate_function_expression(gen, *function_id, preferred_dst)
        }

        // === Array ===
        ExpressionKind::Array(elements) => {
            generate_array_expression(gen, elements, preferred_dst)
        }

        // === Member access ===
        ExpressionKind::Member {
            object,
            property,
            computed,
        } => generate_member_expression(gen, object, property, *computed, preferred_dst),

        // === Call ===
        ExpressionKind::Call(data) => {
            generate_call_expression(gen, data, preferred_dst, false)
        }

        // === New ===
        ExpressionKind::New(data) => {
            generate_call_expression(gen, data, preferred_dst, true)
        }

        // === Spread ===
        ExpressionKind::Spread(inner) => {
            // Spread is handled by the caller (Call, Array, Object)
            Some(generate_expression_or_undefined(inner, gen, preferred_dst))
        }

        // === Yield ===
        ExpressionKind::Yield {
            argument,
            is_yield_from,
        } => generate_yield_expression(gen, argument.as_deref(), *is_yield_from),

        // === Await ===
        ExpressionKind::Await(inner) => {
            let value = generate_expression_or_undefined(inner, gen, None);
            // Match C++ AwaitExpression::generate_bytecode which allocates
            // received_completion registers before the await.
            let received_completion = gen.allocate_register();
            let received_completion_type = gen.allocate_register();
            let received_completion_value = gen.allocate_register();
            let acc = gen.accumulator();
            gen.emit_mov(&received_completion, &acc);
            Some(generate_await_with_completions(
                gen, &value,
                &received_completion, &received_completion_type, &received_completion_value,
            ))
        }

        // === MetaProperty ===
        ExpressionKind::MetaProperty(MetaPropertyType::NewTarget) => {
            let dst = choose_dst(gen, preferred_dst);
            gen.emit(Instruction::GetNewTarget { dst: dst.operand() });
            Some(dst)
        }

        ExpressionKind::MetaProperty(MetaPropertyType::ImportMeta) => {
            let dst = choose_dst(gen, preferred_dst);
            gen.emit(Instruction::GetImportMeta { dst: dst.operand() });
            Some(dst)
        }

        // === ImportCall ===
        ExpressionKind::ImportCall { specifier, options } => {
            let spec = generate_expression(specifier, gen, None)?;
            let opts = match options {
                Some(o) => generate_expression(o, gen, None)?,
                None => gen.add_constant_undefined(),
            };
            let dst = choose_dst(gen, preferred_dst);
            gen.emit(Instruction::ImportCall {
                dst: dst.operand(),
                specifier: spec.operand(),
                options: opts.operand(),
            });
            Some(dst)
        }

        // === Update (++/--) ===
        ExpressionKind::Update {
            op,
            argument,
            prefixed,
        } => generate_update_expression(gen, *op, argument, *prefixed, preferred_dst),

        // === Assignment ===
        ExpressionKind::Assignment { op, lhs, rhs } => {
            generate_assignment_expression(gen, *op, lhs, rhs, preferred_dst)
        }

        // === Template literals ===
        ExpressionKind::TemplateLiteral(data) => {
            generate_template_literal(gen, data, preferred_dst)
        }

        // === Tagged template literals ===
        ExpressionKind::TaggedTemplateLiteral { tag, template_literal } => {
            generate_tagged_template_literal(gen, tag, template_literal, preferred_dst)
        }

        // === Object ===
        ExpressionKind::Object(data) => {
            generate_object_expression(gen, data, preferred_dst)
        }

        // === OptionalChain ===
        ExpressionKind::OptionalChain { base, references } => {
            // Match C++ OptionalChain::generate_bytecode: allocate current_base
            // first, current_value second.
            let current_base = gen.allocate_register();
            let current_value = choose_dst(gen, preferred_dst);
            let undef = gen.add_constant_undefined();
            gen.emit_mov(&current_base, &undef);
            generate_optional_chain_inner(gen, base, references, &current_value, &current_base)?;
            Some(current_value)
        }

        // === SuperCall ===
        ExpressionKind::SuperCall(data) => {
            let arguments = if data.is_synthetic {
                // Synthetic constructor: super(...arguments) — single spread argument,
                // don't call @@iterator on %Array.prototype%.
                assert!(data.arguments.len() == 1 && data.arguments[0].is_spread);
                generate_expression_or_undefined(&data.arguments[0].value, gen, None)
            } else {
                generate_arguments_array(gen, &data.arguments)
            };

            let dst = choose_dst(gen, preferred_dst);
            gen.emit(Instruction::SuperCallWithArgumentArray {
                dst: dst.operand(),
                arguments: arguments.operand(),
                is_synthetic: data.is_synthetic,
            });
            Some(dst)
        }

        ExpressionKind::Super => {
            // super keyword as an expression (for super.foo, super[foo])
            // Returns the home object's prototype
            let dst = choose_dst(gen, preferred_dst);
            gen.emit(Instruction::ResolveSuperBase { dst: dst.operand() });
            Some(dst)
        }

        ExpressionKind::Class(data) => {
            generate_class_expression(gen, data, preferred_dst)
        }

        ExpressionKind::PrivateIdentifier(_) => {
            // Private identifiers are handled by member access codegen
            None
        }

        ExpressionKind::Error => None,
    };

    result
}

fn generate_unary_expression(
    gen: &mut Generator,
    op: UnaryOp,
    operand: &Expression,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // typeof and delete on identifiers need special handling BEFORE
    // evaluating the operand to avoid throwing on unresolvable references.
    // C++ allocates dst before evaluating typeof/not operands.
    if op == UnaryOp::Typeof {
        if let ExpressionKind::Identifier(ident) = &operand.inner {
            if !ident.is_local() {
                let dst = choose_dst(gen, preferred_dst);
                let id = gen.intern_identifier(&ident.name);
                gen.emit(Instruction::TypeofBinding {
                    dst: dst.operand(),
                    identifier: id,
                    cache: EnvironmentCoordinate::empty(),
                });
                return Some(dst);
            }
        }
        let dst = choose_dst(gen, preferred_dst);
        let value = generate_expression(operand, gen, None)?;
        gen.emit(Instruction::Typeof {
            dst: dst.operand(),
            src: value.operand(),
        });
        return Some(dst);
    }
    if op == UnaryOp::Delete {
        return Some(emit_delete_reference(gen, operand));
    }

    // Not needs dst allocated before operand to match C++ register order.
    // Also optimize !!x -> ToBoolean(x).
    if op == UnaryOp::Not {
        let dst = choose_dst(gen, preferred_dst);
        if let ExpressionKind::Unary { op: UnaryOp::Not, operand: inner } = &operand.inner {
            let value = generate_expression(inner, gen, None)?;
            if let Some(folded) = try_constant_fold_to_boolean(gen, &value) {
                return Some(folded);
            }
            gen.emit(Instruction::ToBoolean {
                dst: dst.operand(),
                value: value.operand(),
            });
            return Some(dst);
        }
        let value = generate_expression(operand, gen, None)?;
        if let Some(folded) = try_constant_fold_unary(gen, op, &value) {
            return Some(folded);
        }
        gen.emit(Instruction::Not {
            dst: dst.operand(),
            src: value.operand(),
        });
        return Some(dst);
    }

    let value = generate_expression(operand, gen, None)?;

    // OPTIMIZATION: constant fold unary operations on constants.
    if let Some(folded) = try_constant_fold_unary(gen, op, &value) {
        return Some(folded);
    }

    let dst = choose_dst(gen, preferred_dst);
    match op {
        UnaryOp::BitwiseNot => {
            gen.emit(Instruction::BitwiseNot {
                dst: dst.operand(),
                src: value.operand(),
            });
        }
        UnaryOp::Not => unreachable!("Not is handled by early return above"),
        UnaryOp::Plus => {
            gen.emit(Instruction::UnaryPlus {
                dst: dst.operand(),
                src: value.operand(),
            });
        }
        UnaryOp::Minus => {
            gen.emit(Instruction::UnaryMinus {
                dst: dst.operand(),
                src: value.operand(),
            });
        }
        UnaryOp::Typeof => unreachable!("Typeof is handled by early return above"),
        UnaryOp::Void => {
            return Some(gen.add_constant_undefined());
        }
        UnaryOp::Delete => unreachable!("Delete is handled by early return above"),
    }
    Some(dst)
}

fn generate_binary_expression(
    gen: &mut Generator,
    op: BinaryOp,
    lhs: &Expression,
    rhs: &Expression,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // Special case: `#privateId in obj` uses HasPrivateId instead of In.
    if op == BinaryOp::In {
        if let ExpressionKind::PrivateIdentifier(priv_ident) = &lhs.inner {
            let base = generate_expression(rhs, gen, None)?;
            let dst = choose_dst(gen, preferred_dst);
            let id = gen.intern_identifier(&priv_ident.name);
            gen.emit(Instruction::HasPrivateId {
                dst: dst.operand(),
                base: base.operand(),
                property: id,
            });
            return Some(dst);
        }
    }
    // OPTIMIZATION: Pre-convert numeric literal operands of bitwise
    // operations to i32/u32 to avoid runtime conversion (matches C++).
    let lhs_val = match op {
        BinaryOp::BitwiseAnd | BinaryOp::BitwiseOr | BinaryOp::BitwiseXor
        | BinaryOp::LeftShift | BinaryOp::RightShift | BinaryOp::UnsignedRightShift => {
            if let ExpressionKind::NumericLiteral(n) = &lhs.inner {
                gen.add_constant_number(to_int32(*n) as f64)
            } else {
                generate_expression(lhs, gen, None)?
            }
        }
        _ => generate_expression(lhs, gen, None)?,
    };
    let rhs_val = match op {
        BinaryOp::BitwiseAnd | BinaryOp::BitwiseOr | BinaryOp::BitwiseXor => {
            if let ExpressionKind::NumericLiteral(n) = &rhs.inner {
                gen.add_constant_number(to_int32(*n) as f64)
            } else {
                generate_expression(rhs, gen, None)?
            }
        }
        BinaryOp::LeftShift | BinaryOp::RightShift | BinaryOp::UnsignedRightShift => {
            if let ExpressionKind::NumericLiteral(n) = &rhs.inner {
                gen.add_constant_number(to_u32(*n) as f64)
            } else {
                generate_expression(rhs, gen, None)?
            }
        }
        _ => generate_expression(rhs, gen, None)?,
    };
    // OPTIMIZATION: constant folding for binary operations on constants.
    if let Some(folded) = try_constant_fold_binary(gen, op, &lhs_val, &rhs_val) {
        return Some(folded);
    }
    let dst = choose_dst(gen, preferred_dst);
    emit_binary_op(gen, op, &dst, &lhs_val, &rhs_val);
    Some(dst)
}

fn generate_function_expression(
    gen: &mut Generator,
    function_id: FunctionId,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let data = gen.function_table.take(function_id);
    let has_name = data.name.is_some();
    let mut name_id = None;

    // Named function expressions get an intermediate scope so the name
    // is visible inside the function body but not outside.
    if has_name {
        let parent = gen.lexical_environment_register_stack.last().cloned()
            .unwrap_or_else(|| gen.add_constant_undefined());
        let new_env = gen.allocate_register();
        gen.start_boundary(BlockBoundaryType::LeaveLexicalEnvironment);
        gen.emit(Instruction::CreateLexicalEnvironment {
            dst: new_env.operand(),
            parent: parent.operand(),
            capacity: 0,
        });
        gen.lexical_environment_register_stack.push(new_env);

        let id = gen.intern_identifier(&data.name.as_ref().expect("function declaration must have a name").name);
        gen.emit(Instruction::CreateVariable {
            identifier: id,
            mode: EnvironmentMode::Lexical as u32,
            is_immutable: true,
            is_global: false,
            is_strict: false,
        });
        name_id = Some(id);
    }

    let dst = choose_dst(gen, preferred_dst);
    // For anonymous function expressions, use the pending LHS name
    // as the function's .name property.
    let lhs_name = if !has_name { gen.pending_lhs_name.take() } else { None };
    let lhs_name_str: Option<Utf16String> = lhs_name.map(|index| gen.identifier_table[index.0 as usize].clone());
    let name_override = if !has_name {
        lhs_name_str.as_deref()
    } else {
        None
    };
    let shared_function_data_index = emit_new_function(gen, data, name_override);
    let home_object = gen.home_objects.last().map(|ho| ho.operand());
    gen.emit(Instruction::NewFunction {
        dst: dst.operand(),
        shared_function_data_index,
        lhs_name,
        home_object,
    });

    if has_name {
        gen.emit(Instruction::InitializeLexicalBinding {
            identifier: name_id.expect("has_name guarantees name_id is set"),
            src: dst.operand(),
            cache: EnvironmentCoordinate::empty(),
        });

        gen.end_variable_scope();
    }
    Some(dst)
}

fn generate_array_expression(
    gen: &mut Generator,
    elements: &[Option<Expression>],
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // If all elements are constant primitives, emit NewPrimitiveArray.
    if !elements.is_empty() && elements.iter().all(|e| match e {
        None => true, // holes
        Some(e) => matches!(e.inner, ExpressionKind::NumericLiteral(_) | ExpressionKind::BooleanLiteral(_) | ExpressionKind::NullLiteral),
    }) {
        let values: Vec<u64> = elements.iter().map(|e| match e {
            None => nanboxed_empty(),
            Some(e) => match &e.inner {
                ExpressionKind::NumericLiteral(n) => nanboxed_number(*n),
                ExpressionKind::BooleanLiteral(b) => nanboxed_boolean(*b),
                ExpressionKind::NullLiteral => nanboxed_null(),
                _ => unreachable!("all elements verified as primitive literals above"),
            },
        }).collect();
        let dst = choose_dst(gen, preferred_dst);
        gen.emit(Instruction::NewPrimitiveArray {
            dst: dst.operand(),
            element_count: u32_from_usize(values.len()),
            elements: values,
        });
        return Some(dst);
    }

    // Find the first spread element.
    let first_spread = elements.iter().position(|e| {
        matches!(e, Some(element) if matches!(element.inner, ExpressionKind::Spread(_)))
    });

    // Collect elements before the first spread into a NewArray.
    let pre_spread_count = first_spread.unwrap_or(elements.len());
    let mut scoped_arguments: Vec<ScopedOperand> = Vec::with_capacity(pre_spread_count);
    for element in &elements[..pre_spread_count] {
        match element {
            Some(e) => {
                let val = generate_expression_or_undefined(e, gen, None);
                scoped_arguments.push(gen.copy_if_needed_to_preserve_evaluation_order(&val));
            }
            None => {
                scoped_arguments.push(gen.add_constant_empty());
            }
        }
    }
    let dst = choose_dst(gen, preferred_dst);
    let arguments: Vec<Operand> = scoped_arguments.iter().map(|s| s.operand()).collect();
    gen.emit(Instruction::NewArray {
        dst: dst.operand(),
        element_count: u32_from_usize(arguments.len()),
        elements: arguments,
    });

    // NB: Keep scoped_arguments alive until the end of the expression
    // to match C++ register lifetime behavior.

    // Append elements after the first spread using ArrayAppend.
    if let Some(spread_index) = first_spread {
        for element in &elements[spread_index..] {
            match element {
                None => {
                    let empty = gen.add_constant_empty();
                    gen.emit(Instruction::ArrayAppend {
                        dst: dst.operand(),
                        src: empty.operand(),
                        is_spread: false,
                    });
                }
                Some(e) => {
                    let is_spread = matches!(e.inner, ExpressionKind::Spread(_));
                    let val = generate_expression_or_undefined(e, gen, None);
                    gen.emit(Instruction::ArrayAppend {
                        dst: dst.operand(),
                        src: val.operand(),
                        is_spread,
                    });
                }
            }
        }
    }

    Some(dst)
}

fn generate_member_expression(
    gen: &mut Generator,
    object: &Expression,
    property: &Expression,
    computed: bool,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let is_super = matches!(object.inner, ExpressionKind::Super);
    if is_super {
        // Per spec, evaluation order for super property access is:
        // 1. Resolve this binding
        // 2. Evaluate computed property (if any)
        // 3. Resolve super base
        // 4. Property lookup with this
        let this_value = emit_resolve_this_binding(gen);
        let computed_key = if computed {
            Some(generate_expression(property, gen, None)?)
        } else {
            None
        };
        let super_base = gen.allocate_register();
        gen.emit(Instruction::ResolveSuperBase { dst: super_base.operand() });
        let dst = choose_dst(gen, preferred_dst);
        if let Some(key) = computed_key {
            emit_get_by_value_with_this(gen, &dst, &super_base, &key, &this_value);
        } else if let ExpressionKind::Identifier(ident) = &property.inner {
            let key = gen.intern_property_key(&ident.name);
            let cache = gen.next_property_lookup_cache();
            gen.emit(Instruction::GetByIdWithThis {
                dst: dst.operand(),
                base: super_base.operand(),
                property: key,
                this_value: this_value.operand(),
                cache_index: cache,
            });
        }
        return Some(dst);
    }
    let obj = generate_expression(object, gen, None)?;
    let base_id = intern_base_identifier(gen, object);
    if computed {
        let property = generate_expression(property, gen, None)?;
        let dst = choose_dst(gen, preferred_dst);
        emit_get_by_value(gen, &dst, &obj, &property, base_id);
        return Some(dst);
    }
    // Non-computed: property must be an Identifier
    let dst = choose_dst(gen, preferred_dst);
    if let ExpressionKind::Identifier(ident) = &property.inner {
        emit_get_by_id(gen, &dst, &obj, &ident.name, base_id);
    } else if let ExpressionKind::PrivateIdentifier(priv_ident) = &property.inner {
        let id = gen.intern_identifier(&priv_ident.name);
        gen.emit(Instruction::GetPrivateById {
            dst: dst.operand(),
            base: obj.operand(),
            property: id,
        });
    }
    Some(dst)
}

fn generate_yield_expression(
    gen: &mut Generator,
    argument: Option<&Expression>,
    is_yield_from: bool,
) -> Option<ScopedOperand> {
    // Match C++ YieldExpression::generate_bytecode: allocate
    // completion registers before evaluating the argument.
    let received_completion = gen.allocate_register();
    let received_completion_type = gen.allocate_register();
    let received_completion_value = gen.allocate_register();

    let value = if let Some(argument) = argument {
        generate_expression_or_undefined(argument, gen, None)
    } else {
        gen.add_constant_undefined()
    };

    if is_yield_from {
        return Some(generate_yield_from(gen, value, &received_completion, &received_completion_type, &received_completion_value));
    }

    // Match C++ YieldExpression::generate_bytecode: create continuation
    // block, call generate_yield, then handle completion checking.
    let continuation_block = gen.make_block();
    let is_in_finalizer = gen.is_in_finalizer();

    // Save exception register before yielding if in a finalizer,
    // as the act of yielding clears scheduled exceptions.
    let saved_exception = if is_in_finalizer {
        let reg = gen.allocate_register();
        gen.emit_mov_raw(reg.operand(), gen.exception_operand());
        Some(reg)
    } else {
        None
    };

    generate_yield(
        gen,
        continuation_block,
        &value,
        &received_completion,
        &received_completion_type,
        &received_completion_value,
        gen.is_in_async_generator_function(),
    );

    gen.switch_to_basic_block(continuation_block);

    // Restore exception register after resuming.
    if let Some(ref saved) = saved_exception {
        gen.emit_mov_raw(gen.exception_operand(), saved.operand());
    }

    let acc = gen.accumulator();
    gen.emit_mov(&received_completion, &acc);
    gen.emit(Instruction::GetCompletionFields {
        type_dst: received_completion_type.operand(),
        value_dst: received_completion_value.operand(),
        completion: received_completion.operand(),
    });

    let normal_block = gen.make_block();
    let throw_cont = gen.make_block();
    let type_is_normal = gen.allocate_register();
    let normal_type = gen.add_constant_number(CompletionType::Normal.to_f64());
    gen.emit(Instruction::StrictlyEquals {
        dst: type_is_normal.operand(),
        lhs: received_completion_type.operand(),
        rhs: normal_type.operand(),
    });
    gen.emit_jump_if(&type_is_normal, normal_block, throw_cont);

    let throw_value_block = gen.make_block();
    let return_value_block = gen.make_block();

    gen.switch_to_basic_block(throw_cont);
    let type_is_throw = gen.allocate_register();
    let throw_type = gen.add_constant_number(CompletionType::Throw.to_f64());
    gen.emit(Instruction::StrictlyEquals {
        dst: type_is_throw.operand(),
        lhs: received_completion_type.operand(),
        rhs: throw_type.operand(),
    });
    gen.emit_jump_if(&type_is_throw, throw_value_block, return_value_block);

    gen.switch_to_basic_block(throw_value_block);
    gen.emit(Instruction::Throw {
        src: received_completion_value.operand(),
    });

    gen.switch_to_basic_block(return_value_block);
    gen.generate_return(&received_completion_value);

    gen.switch_to_basic_block(normal_block);
    Some(received_completion_value)
}

/// Generate bytecode for an expression, returning `undefined` if the
/// expression produces no value (e.g. the block was already terminated).
fn generate_expression_or_undefined(
    expression: &Expression,
    gen: &mut Generator,
    preferred_dst: Option<&ScopedOperand>,
) -> ScopedOperand {
    generate_expression(expression, gen, preferred_dst)
        .unwrap_or_else(|| gen.add_constant_undefined())
}

/// Generate bytecode for a statement.
pub fn generate_statement(
    statement: &Statement,
    gen: &mut Generator,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let saved_source_start = gen.current_source_start;
    let saved_source_end = gen.current_source_end;
    gen.current_source_start = statement.range.start.offset;
    gen.current_source_end = statement.range.end.offset;

    let result = match &statement.inner {
        StatementKind::Empty | StatementKind::Error | StatementKind::ErrorDeclaration => None,
        StatementKind::Debugger => None,

        // === ExpressionStatement ===
        StatementKind::Expression(expression) => generate_expression(expression, gen, None),

        // === Block ===
        StatementKind::Block(ref scope) => generate_block_statement(gen, &scope.borrow(), preferred_dst),

        // === FunctionBody ===
        StatementKind::FunctionBody { ref scope, .. } => generate_scope_children(gen, &scope.borrow(), preferred_dst),

        // === Program ===
        // Note: GlobalDeclarationInstantiation (GDI) runs before this bytecode
        // executes. GDI hoists top-level function declarations and var bindings
        // to the global scope, including Annex B function-in-block hoisting.
        StatementKind::Program(ref data) => {
            // Populate annexb_function_names so switch codegen can emit
            // GetBinding + SetVariableBinding for AnnexB-hoisted functions
            // (Annex B requires switch cases to copy the block-scoped binding
            // into the var-scoped binding on each case entry).
            let scope = data.scope.borrow();
            for name in &scope.annexb_function_names {
                gen.annexb_function_names.insert(name.clone());
            }
            generate_scope_children(gen, &scope, preferred_dst)
        }

        // === If ===
        StatementKind::If {
            test,
            consequent,
            alternate,
        } => generate_if_statement(gen, test, consequent, alternate.as_deref(), preferred_dst),

        // === While ===
        StatementKind::While { test, body } => {
            generate_while_statement(gen, test, body, preferred_dst)
        }

        // === DoWhile ===
        StatementKind::DoWhile { test, body } => {
            generate_do_while_statement(gen, test, body, preferred_dst)
        }

        // === For ===
        StatementKind::For {
            init,
            test,
            update,
            body,
        } => generate_for_statement(gen, init.as_ref(), test.as_deref(), update.as_deref(), body, preferred_dst),

        // === Return ===
        StatementKind::Return(value) => {
            let val = match value {
                Some(expression) => {
                    let v = generate_expression_or_undefined(expression, gen, None);
                    // Async functions implicitly await an explicit return value.
                    // Bare `return;` does NOT await (matches C++ and spec).
                    if gen.is_in_async_function() {
                        let received_completion = gen.allocate_register();
                        let received_completion_type = gen.allocate_register();
                        let received_completion_value = gen.allocate_register();
                        generate_await_with_completions(
                            gen, &v,
                            &received_completion, &received_completion_type, &received_completion_value,
                        )
                    } else {
                        v
                    }
                }
                None => gen.add_constant_undefined(),
            };
            gen.generate_return(&val);
            None
        }

        // === Throw ===
        StatementKind::Throw(expression) => {
            let val = generate_expression(expression, gen, None)?;
            gen.perform_needed_unwinds();
            gen.emit(Instruction::Throw { src: val.operand() });
            None
        }

        // === Variable declarations ===
        StatementKind::VariableDeclaration { kind, declarations } => {
            generate_variable_declaration(gen, *kind, declarations);
            None
        }

        // === Break ===
        StatementKind::Break { target_label } => {
            gen.generate_break(target_label.as_deref());
            None
        }

        // === Continue ===
        StatementKind::Continue { target_label } => {
            gen.generate_continue(target_label.as_deref());
            None
        }

        // === Labelled ===
        StatementKind::Labelled { label, item } => {
            generate_labelled_statement(gen, label, item, preferred_dst)
        }

        // === Switch ===
        StatementKind::Switch(data) => {
            generate_switch_statement(gen, data, preferred_dst)
        }

        // === Try ===
        StatementKind::Try(data) => {
            generate_try_statement(gen, data, preferred_dst)
        }

        // === FunctionDeclaration ===
        StatementKind::FunctionDeclaration { ref name, ref is_hoisted, .. } => {
            if is_hoisted.get() {
                // Annex B.3.3: Copy the function from the lexical (block) scope
                // to the var scope.
                if let Some(ref name_ident) = name {
                    let id = gen.intern_identifier(&name_ident.name);
                    let value = gen.allocate_register();
                    gen.emit(Instruction::GetBinding {
                        dst: value.operand(),
                        identifier: id,
                        cache: EnvironmentCoordinate::empty(),
                    });
                    gen.emit(Instruction::SetVariableBinding {
                        identifier: id,
                        src: value.operand(),
                        cache: EnvironmentCoordinate::empty(),
                    });
                }
            }
            None
        }

        // === With ===
        StatementKind::With { object, body } => {
            let obj = generate_expression(object, gen, None)?;
            let object_environment = gen.allocate_register();
            gen.emit(Instruction::EnterObjectEnvironment { dst: object_environment.operand(), object: obj.operand() });
            gen.lexical_environment_register_stack.push(object_environment);
            gen.start_boundary(BlockBoundaryType::LeaveLexicalEnvironment);

            let result = generate_statement(body, gen, preferred_dst);

            gen.end_variable_scope();
            // Per spec 13.11.7 step 10: if body completion value is empty,
            // return NormalCompletion(undefined).
            Some(result.unwrap_or_else(|| gen.add_constant_undefined()))
        }

        // === ForIn / ForOf / ForAwaitOf ===
        StatementKind::ForInOf { kind, lhs, rhs, body } => {
            generate_for_in_of_statement(gen, *kind, lhs, rhs, body, preferred_dst)
        }

        // === UsingDeclaration ===
        StatementKind::UsingDeclaration { .. } => {
            // Disposal semantics are not yet implemented.
            let error = gen.allocate_register();
            let msg = gen.intern_string(utf16!("TODO: UsingDeclaration"));
            gen.emit(Instruction::NewTypeError {
                dst: error.operand(),
                error_string: msg,
            });
            gen.perform_needed_unwinds();
            gen.emit(Instruction::Throw { src: error.operand() });
            // Switch to a dead block so subsequent codegen doesn't crash.
            let dead = gen.make_block();
            gen.switch_to_basic_block(dead);
            None
        }

        // === ClassDeclaration ===
        StatementKind::ClassDeclaration(data) => {
            let value = generate_class_expression(gen, data, None);
            // Bind the class name in the outer scope (classes are lexically scoped).
            // Use InitializeLexicalBinding since the name starts in the TDZ
            // (temporal dead zone) until this point, matching `let` semantics.
            // NB: We do NOT mark the local as initialized here, matching C++
            // emit_set_variable(Initialize) behavior. This preserves TDZ checks
            // for subsequent uses of the class name.
            if let (Some(name_ident), Some(val)) = (&data.name, &value) {
                if name_ident.is_local() {
                    let local = gen.resolve_local(name_ident.local_index.get(), name_ident.local_type.get().unwrap());
                    gen.emit_mov(&local, val);
                } else {
                    let id = gen.intern_identifier(&name_ident.name);
                    gen.emit(Instruction::InitializeLexicalBinding {
                        identifier: id,
                        src: val.operand(),
                        cache: EnvironmentCoordinate::empty(),
                    });
                }
            }
            None
        }

        // === Import/Export ===
        StatementKind::Import(_) => None, // Handled by module loading
        StatementKind::Export(export_data) => {
            if !export_data.is_default_export {
                // Non-default export: generate code for the wrapped statement.
                if let Some(ref child_statement) = export_data.statement {
                    generate_statement(child_statement, gen, None)
                } else {
                    None
                }
            } else if let Some(ref child_statement) = export_data.statement {
                match &child_statement.inner {
                    StatementKind::FunctionDeclaration { .. } | StatementKind::ClassDeclaration(_) => {
                        generate_statement(child_statement, gen, None)
                    }
                    _ => {
                        // export default <expression>
                        // The child_statement wraps an Expression via StatementKind::Expression.
                        let default_name: Utf16String = Utf16String::from(utf16!("default"));
                        gen.pending_lhs_name = Some(gen.intern_identifier(&default_name));
                        let value = generate_statement(child_statement, gen, None);
                        gen.pending_lhs_name = None;
                        if let Some(value) = value {
                            let local_name = gen.intern_identifier(
                                utf16!("*default*"),
                            );
                            gen.emit(Instruction::InitializeLexicalBinding {
                                identifier: local_name,
                                src: value.operand(),
                                cache: EnvironmentCoordinate::empty(),
                            });
                            Some(value)
                        } else {
                            None
                        }
                    }
                }
            } else {
                None
            }
        }

        // === ClassFieldInitializer ===
        StatementKind::ClassFieldInitializer { expression, field_name } => {
            gen.pending_lhs_name = Some(gen.intern_identifier(field_name));
            let value = generate_expression_or_undefined(expression, gen, None);
            gen.pending_lhs_name = None;
            gen.emit(Instruction::Return {
                value: value.operand(),
            });
            None
        }
    };

    gen.current_source_start = saved_source_start;
    gen.current_source_end = saved_source_end;
    result
}

// =============================================================================
// Await helper
// =============================================================================

/// Completion::Type values (ABI-compatible).
#[derive(Clone, Copy)]
#[repr(u32)]
enum CompletionType {
    Normal = 1,
    Return = 4,
    Throw = 5,
}

impl CompletionType {
    fn to_f64(self) -> f64 {
        self as u32 as f64
    }
}

/// Environment binding mode.
#[repr(u32)]
enum EnvironmentMode {
    Lexical = 0,
    Var = 1,
}

/// Arguments object creation mode.
#[repr(u32)]
enum ArgumentsKind {
    Mapped = 0,
    Unmapped = 1,
}

/// Class element kind (ABI-compatible with ClassBlueprint::Element::Kind).
#[repr(u8)]
enum ClassElementKind {
    Method = 0,
    Getter = 1,
    Setter = 2,
    Field = 3,
    StaticInitializer = 4,
}

/// Iterator hint (ABI-compatible).
#[repr(u32)]
enum IteratorHint {
    Sync = 0,
    Async = 1,
}

/// Literal value kind for class field initializers
/// (ABI-compatible with BytecodeFactory).
#[repr(u8)]
enum LiteralValueKind {
    None = 0,
    Number = 1,
    BooleanTrue = 2,
    BooleanFalse = 3,
    Null = 4,
    String = 5,
}

/// Like generate_await but uses caller-provided completion registers.
///
/// Returns the received_completion_value on the normal path.
/// Emits a Throw on the throw path.
fn generate_await_with_completions(
    gen: &mut Generator,
    argument: &ScopedOperand,
    received_completion: &ScopedOperand,
    received_completion_type: &ScopedOperand,
    received_completion_value: &ScopedOperand,
) -> ScopedOperand {
    let continuation = gen.make_block();
    gen.emit(Instruction::Await {
        continuation_label: continuation,
        argument: argument.operand(),
    });
    gen.switch_to_basic_block(continuation);

    let acc = gen.accumulator();
    gen.emit_mov(received_completion, &acc);
    gen.emit(Instruction::GetCompletionFields {
        type_dst: received_completion_type.operand(),
        value_dst: received_completion_value.operand(),
        completion: received_completion.operand(),
    });

    let normal_block = gen.make_block();
    let throw_block = gen.make_block();
    let is_normal = gen.allocate_register();
    let normal_type = gen.add_constant_number(CompletionType::Normal.to_f64());
    gen.emit(Instruction::StrictlyEquals {
        dst: is_normal.operand(),
        lhs: received_completion_type.operand(),
        rhs: normal_type.operand(),
    });
    gen.emit_jump_if(&is_normal, normal_block, throw_block);

    gen.switch_to_basic_block(throw_block);
    gen.emit(Instruction::Throw {
        src: received_completion_value.operand(),
    });

    gen.switch_to_basic_block(normal_block);
    received_completion_value.clone()
}

/// Yield* (yield from) delegation.
///
/// Implements the iterator delegation protocol from
/// https://tc39.es/ecma262/#sec-generator-function-definitions-runtime-semantics-evaluation
///
/// The delegating generator forwards next/throw/return to the inner iterator.
fn generate_yield_from(
    gen: &mut Generator,
    value: ScopedOperand,
    received_completion: &ScopedOperand,
    received_completion_type: &ScopedOperand,
    received_completion_value: &ScopedOperand,
) -> ScopedOperand {
    let is_async = gen.is_in_async_generator_function();

    // 4. Let iteratorRecord be ? GetIterator(value, generatorKind).
    let iterator = gen.allocate_register();
    let next_method = gen.allocate_register();
    let iterator_done_property = gen.allocate_register();
    let hint = if is_async { IteratorHint::Async } else { IteratorHint::Sync } as u32;
    gen.emit(Instruction::GetIterator {
        dst_iterator_object: iterator.operand(),
        dst_iterator_next: next_method.operand(),
        dst_iterator_done: iterator_done_property.operand(),
        iterable: value.operand(),
        hint,
    });

    // 6. Let received be NormalCompletion(undefined).
    let normal_const = gen.add_constant_number(CompletionType::Normal.to_f64());
    gen.emit_mov(received_completion_type, &normal_const);
    let undef = gen.add_constant_undefined();
    gen.emit_mov(received_completion_value, &undef);

    // 7. Repeat,
    let loop_block = gen.make_block();
    let continuation_block = gen.make_block();
    let loop_end_block = gen.make_block();
    gen.emit(Instruction::Jump {
        target: loop_block,
    });
    gen.switch_to_basic_block(loop_block);

    // Branch on received.[[Type]].
    let type_is_normal_block = gen.make_block();
    let is_type_throw_block = gen.make_block();
    let is_normal = gen.allocate_register();
    gen.emit(Instruction::StrictlyEquals {
        dst: is_normal.operand(),
        lhs: received_completion_type.operand(),
        rhs: normal_const.operand(),
    });
    gen.emit_jump_if(&is_normal, type_is_normal_block, is_type_throw_block);

    // =========================================================================
    // a. If received.[[Type]] is normal, then
    // =========================================================================
    gen.switch_to_basic_block(type_is_normal_block);

    // i. Let innerResult be ? Call(next, iterator, « received.[[Value]] »).
    let inner_result = gen.allocate_register();
    gen.emit(Instruction::Call {
        dst: inner_result.operand(),
        callee: next_method.operand(),
        this_value: iterator.operand(),
        argument_count: 1,
        expression_string: None,
        arguments: vec![received_completion_value.operand()],
    });

    // ii. If generatorKind is async, set innerResult to ? Await(innerResult).
    if is_async {
        let awaited = generate_await_with_completions(
            gen,
            &inner_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        gen.emit_mov(&inner_result, &awaited);
    }

    // iii. If innerResult is not an Object, throw a TypeError exception.
    gen.emit(Instruction::ThrowIfNotObject {
        src: inner_result.operand(),
    });

    // iv. Let done be ? IteratorComplete(innerResult).
    let done = gen.allocate_register();
    emit_get_by_id(gen, &done, &inner_result, utf16!("done"), None);

    // v. If done is true, then return ? IteratorValue(innerResult).
    let type_is_normal_done_block = gen.make_block();
    let type_is_normal_not_done_block = gen.make_block();
    gen.emit_jump_if(&done, type_is_normal_done_block, type_is_normal_not_done_block);

    gen.switch_to_basic_block(type_is_normal_done_block);
    let return_value = gen.allocate_register();
    emit_get_by_id(gen, &return_value, &inner_result, utf16!("value"), None);
    gen.emit(Instruction::Jump {
        target: loop_end_block,
    });

    // vi/vii. Yield IteratorValue(innerResult), receive new completion.
    gen.switch_to_basic_block(type_is_normal_not_done_block);
    {
        let current_value = gen.allocate_register();
        emit_get_by_id(gen, &current_value, &inner_result, utf16!("value"), None);

        generate_yield(
            gen,
            continuation_block,
            &current_value,
            received_completion,
            received_completion_type,
            received_completion_value,
            false,
        );
    }

    // =========================================================================
    // b. Else if received.[[Type]] is throw, then
    // =========================================================================
    gen.switch_to_basic_block(is_type_throw_block);
    let type_is_throw_block = gen.make_block();
    let type_is_return_block = gen.make_block();
    let throw_const = gen.add_constant_number(CompletionType::Throw.to_f64());
    let is_throw = gen.allocate_register();
    gen.emit(Instruction::StrictlyEquals {
        dst: is_throw.operand(),
        lhs: received_completion_type.operand(),
        rhs: throw_const.operand(),
    });
    gen.emit_jump_if(&is_throw, type_is_throw_block, type_is_return_block);

    gen.switch_to_basic_block(type_is_throw_block);

    // i. Let throw be ? GetMethod(iterator, "throw").
    let throw_method = gen.allocate_register();
    let throw_key = gen.intern_property_key(utf16!("throw"));
    gen.emit(Instruction::GetMethod {
        dst: throw_method.operand(),
        object: iterator.operand(),
        property: throw_key,
    });

    // ii. If throw is not undefined, then
    let throw_method_defined_block = gen.make_block();
    let throw_method_undefined_block = gen.make_block();
    gen.emit(Instruction::JumpUndefined {
        condition: throw_method.operand(),
        true_target: throw_method_undefined_block,
        false_target: throw_method_defined_block,
    });

    gen.switch_to_basic_block(throw_method_defined_block);

    // 1. Let innerResult be ? Call(throw, iterator, « received.[[Value]] »).
    gen.emit(Instruction::Call {
        dst: inner_result.operand(),
        callee: throw_method.operand(),
        this_value: iterator.operand(),
        argument_count: 1,
        expression_string: None,
        arguments: vec![received_completion_value.operand()],
    });

    // 2. If generatorKind is async, set innerResult to ? Await(innerResult).
    if is_async {
        let awaited = generate_await_with_completions(
            gen,
            &inner_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        gen.emit_mov(&inner_result, &awaited);
    }

    // 4. If innerResult is not an Object, throw a TypeError exception.
    gen.emit(Instruction::ThrowIfNotObject {
        src: inner_result.operand(),
    });

    // 5. Let done be ? IteratorComplete(innerResult).
    emit_get_by_id(gen, &done, &inner_result, utf16!("done"), None);

    // 6. If done is true, return ? IteratorValue(innerResult).
    let type_is_throw_done_block = gen.make_block();
    let type_is_throw_not_done_block = gen.make_block();
    gen.emit_jump_if(&done, type_is_throw_done_block, type_is_throw_not_done_block);

    gen.switch_to_basic_block(type_is_throw_done_block);
    emit_get_by_id(gen, &return_value, &inner_result, utf16!("value"), None);
    gen.emit(Instruction::Jump {
        target: loop_end_block,
    });

    // 7/8. Yield IteratorValue(innerResult), receive new completion.
    gen.switch_to_basic_block(type_is_throw_not_done_block);
    {
        let yield_value = gen.allocate_register();
        emit_get_by_id(gen, &yield_value, &inner_result, utf16!("value"), None);
        generate_yield(
            gen,
            continuation_block,
            &yield_value,
            received_completion,
            received_completion_type,
            received_completion_value,
            false,
        );
    }

    // throw is undefined: close iterator, throw TypeError.
    gen.switch_to_basic_block(throw_method_undefined_block);

    if is_async {
        // AsyncIteratorClose: get return method, call it, await, check object.
        let return_method = gen.allocate_register();
        let return_key = gen.intern_property_key(utf16!("return"));
        gen.emit(Instruction::GetMethod {
            dst: return_method.operand(),
            object: iterator.operand(),
            property: return_key,
        });

        let call_return_block = gen.make_block();
        let after_close = gen.make_block();
        gen.emit(Instruction::JumpUndefined {
            condition: return_method.operand(),
            true_target: after_close,
            false_target: call_return_block,
        });
        gen.switch_to_basic_block(call_return_block);

        let close_result = gen.allocate_register();
        gen.emit(Instruction::Call {
            dst: close_result.operand(),
            callee: return_method.operand(),
            this_value: iterator.operand(),
            argument_count: 0,
            expression_string: None,
            arguments: vec![],
        });

        let awaited = generate_await_with_completions(
            gen,
            &close_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        gen.emit(Instruction::ThrowIfNotObject {
            src: awaited.operand(),
        });

        gen.emit(Instruction::Jump {
            target: after_close,
        });
        gen.switch_to_basic_block(after_close);
    } else {
        // Sync: IteratorClose with Normal completion.
        let undef = gen.add_constant_undefined();
        gen.emit(Instruction::IteratorClose {
            iterator_object: iterator.operand(),
            iterator_next: next_method.operand(),
            iterator_done: done.operand(),
            completion_type: CompletionType::Normal as u32,
            completion_value: undef.operand(),
        });
    }

    // Throw a TypeError: iterator does not have a throw method.
    let exception = gen.allocate_register();
    let error_string = gen.intern_string(utf16!("yield* protocol violation: iterator must have a throw method"));
    gen.emit(Instruction::NewTypeError {
        dst: exception.operand(),
        error_string,
    });
    gen.emit(Instruction::Throw {
        src: exception.operand(),
    });

    // =========================================================================
    // c. Else (received.[[Type]] is return)
    // =========================================================================
    gen.switch_to_basic_block(type_is_return_block);

    // ii. Let return be ? GetMethod(iterator, "return").
    let return_method = gen.allocate_register();
    let return_key = gen.intern_property_key(utf16!("return"));
    gen.emit(Instruction::GetMethod {
        dst: return_method.operand(),
        object: iterator.operand(),
        property: return_key,
    });

    // iii. If return is undefined, then return received.[[Value]].
    let return_is_undefined_block = gen.make_block();
    let return_is_defined_block = gen.make_block();
    gen.emit(Instruction::JumpUndefined {
        condition: return_method.operand(),
        true_target: return_is_undefined_block,
        false_target: return_is_defined_block,
    });

    gen.switch_to_basic_block(return_is_undefined_block);
    // 1. If generatorKind is async, set received.[[Value]] to ? Await(received.[[Value]]).
    if is_async {
        generate_await_with_completions(
            gen,
            received_completion_value,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
    }
    // 2. Return received (return completion).
    gen.generate_return(received_completion_value);

    gen.switch_to_basic_block(return_is_defined_block);

    // iv. Let innerReturnResult be ? Call(return, iterator, « received.[[Value]] »).
    let inner_return_result = gen.allocate_register();
    gen.emit(Instruction::Call {
        dst: inner_return_result.operand(),
        callee: return_method.operand(),
        this_value: iterator.operand(),
        argument_count: 1,
        expression_string: None,
        arguments: vec![received_completion_value.operand()],
    });

    // v. If generatorKind is async, set innerReturnResult to ? Await(innerReturnResult).
    if is_async {
        let awaited = generate_await_with_completions(
            gen,
            &inner_return_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        gen.emit_mov(&inner_return_result, &awaited);
    }

    // vi. If innerReturnResult is not an Object, throw a TypeError exception.
    gen.emit(Instruction::ThrowIfNotObject {
        src: inner_return_result.operand(),
    });

    // vii. Let done be ? IteratorComplete(innerReturnResult).
    emit_get_by_id(gen, &done, &inner_return_result, utf16!("done"), None);

    // viii. If done is true, return IteratorValue(innerReturnResult).
    let type_is_return_done_block = gen.make_block();
    let type_is_return_not_done_block = gen.make_block();
    gen.emit_jump_if(&done, type_is_return_done_block, type_is_return_not_done_block);

    gen.switch_to_basic_block(type_is_return_done_block);
    let inner_return_result_value = gen.allocate_register();
    emit_get_by_id(gen, &inner_return_result_value, &inner_return_result, utf16!("value"), None);
    gen.generate_return(&inner_return_result_value);

    // ix/x. Yield IteratorValue(innerReturnResult), receive new completion.
    gen.switch_to_basic_block(type_is_return_not_done_block);
    let received = gen.allocate_register();
    emit_get_by_id(gen, &received, &inner_return_result, utf16!("value"), None);
    generate_yield(
        gen,
        continuation_block,
        &received,
        received_completion,
        received_completion_type,
        received_completion_value,
        false,
    );

    // =========================================================================
    // Continuation block: resume after any yield, extract completion, loop back.
    // =========================================================================
    gen.switch_to_basic_block(continuation_block);
    let acc = gen.accumulator();
    gen.emit_mov(received_completion, &acc);
    gen.emit(Instruction::GetCompletionFields {
        type_dst: received_completion_type.operand(),
        value_dst: received_completion_value.operand(),
        completion: received_completion.operand(),
    });
    gen.emit(Instruction::Jump {
        target: loop_block,
    });

    // =========================================================================
    // Loop end: return the accumulated return_value.
    // =========================================================================
    gen.switch_to_basic_block(loop_end_block);
    return_value
}

/// Unified yield function matching C++ generate_yield().
///
/// For non-async generators: just emits a Yield instruction.
/// For async generators: optionally awaits the argument first, then yields,
/// then handles AsyncGeneratorUnwrapYieldResumption (check return type,
/// await return value, re-classify).
/// Jumps to continuation_label for the "not return" and "throw after await" paths.
fn generate_yield(
    gen: &mut Generator,
    continuation_label: Label,
    argument: &ScopedOperand,
    received_completion: &ScopedOperand,
    received_completion_type: &ScopedOperand,
    received_completion_value: &ScopedOperand,
    await_before_yield: bool,
) {
    if !gen.is_in_async_generator_function() {
        gen.emit(Instruction::Yield {
            continuation_label: Some(continuation_label),
            value: argument.operand(),
        });
        return;
    }

    let argument = if await_before_yield {
        generate_await_with_completions(
            gen, argument,
            received_completion, received_completion_type, received_completion_value,
        )
    } else {
        argument.clone()
    };

    // Yield, then UnwrapYieldResumption.
    let unwrap_block = gen.make_block();
    gen.emit(Instruction::Yield {
        continuation_label: Some(unwrap_block),
        value: argument.operand(),
    });
    gen.switch_to_basic_block(unwrap_block);

    let acc = gen.accumulator();
    gen.emit_mov(received_completion, &acc);
    gen.emit(Instruction::GetCompletionFields {
        type_dst: received_completion_type.operand(),
        value_dst: received_completion_value.operand(),
        completion: received_completion.operand(),
    });

    // If resumptionValue.[[Type]] is not return, jump to continuation.
    let return_block = gen.make_block();
    let is_not_return = gen.allocate_register();
    let return_type = gen.add_constant_number(CompletionType::Return.to_f64());
    gen.emit(Instruction::StrictlyInequals {
        dst: is_not_return.operand(),
        lhs: received_completion_type.operand(),
        rhs: return_type.operand(),
    });
    gen.emit_jump_if(&is_not_return, continuation_label, return_block);

    // Return path: Await(resumptionValue.[[Value]]).
    gen.switch_to_basic_block(return_block);
    generate_await_with_completions(
        gen,
        received_completion_value,
        received_completion,
        received_completion_type,
        received_completion_value,
    );

    // If awaited.[[Type]] is throw, jump to continuation.
    let awaited_normal_block = gen.make_block();
    let is_throw = gen.allocate_register();
    let throw_type = gen.add_constant_number(CompletionType::Throw.to_f64());
    gen.emit(Instruction::StrictlyEquals {
        dst: is_throw.operand(),
        lhs: received_completion_type.operand(),
        rhs: throw_type.operand(),
    });
    gen.emit_jump_if(&is_throw, continuation_label, awaited_normal_block);

    // awaited.[[Type]] is normal: set type to Return and jump to continuation.
    gen.switch_to_basic_block(awaited_normal_block);
    gen.emit(Instruction::SetCompletionType {
        completion: received_completion.operand(),
        completion_type: CompletionType::Return as u32,
    });
    gen.emit(Instruction::Jump {
        target: continuation_label,
    });
}

// =============================================================================
// Identifier codegen
// =============================================================================

/// Generate bytecode for an identifier reference.
///
/// Scope analysis determines how the identifier is resolved:
/// - **Local**: direct register/local access (with TDZ check for let/const)
/// - **Global**: GetGlobal instruction (with inline cache)
/// - **Environment**: GetBinding/GetInitializedBinding (with environment coordinate cache)
fn generate_identifier(
    ident: &Identifier,
    gen: &mut Generator,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    if ident.is_local() {
        let local_index = ident.local_index.get();
        let local = gen.resolve_local(local_index, ident.local_type.get().unwrap());
        // Check TDZ for uninitialized bindings.
        // Arguments may need TDZ during default parameter evaluation;
        // for variable-type locals, only lexically-declared (let/const) need TDZ.
        // Matching C++ Identifier::generate_bytecode.
        let needs_tdz_check = if ident.local_type.get() == Some(LocalType::Argument) {
            !gen.is_argument_initialized(local_index)
        } else {
            gen.is_local_lexically_declared(local_index) && !gen.is_local_initialized(local_index)
        };
        if needs_tdz_check {
            if ident.local_type.get() == Some(LocalType::Argument) {
                // Arguments are initialized to undefined by default, so we
                // need to replace the value with the empty sentinel to
                // trigger the TDZ check.
                let empty = gen.add_constant_empty();
                gen.emit_mov(&local, &empty);
            }
            gen.emit(Instruction::ThrowIfTDZ {
                src: local.operand(),
            });
        }
        return Some(local);
    }

    // OPTIMIZATION: Generate builtin constants (undefined, NaN, Infinity) directly.
    if ident.is_global.get() {
        if let Some(constant) = maybe_generate_builtin_constant(gen, &ident.name) {
            return Some(constant);
        }
    }

    let dst = choose_dst(gen, preferred_dst);
    if ident.is_global.get() {
        let id = gen.intern_identifier(&ident.name);
        let cache = gen.next_global_variable_cache();
        gen.emit(Instruction::GetGlobal {
            dst: dst.operand(),
            identifier: id,
            cache_index: cache,
        });
    } else if ident.declaration_kind.get() == Some(DeclarationKind::Var) {
        let id = gen.intern_identifier(&ident.name);
        gen.emit(Instruction::GetInitializedBinding {
            dst: dst.operand(),
            identifier: id,
            cache: EnvironmentCoordinate::empty(),
        });
    } else {
        let id = gen.intern_identifier(&ident.name);
        gen.emit(Instruction::GetBinding {
            dst: dst.operand(),
            identifier: id,
            cache: EnvironmentCoordinate::empty(),
        });
    }
    Some(dst)
}

fn maybe_generate_builtin_constant(gen: &mut Generator, name: &[u16]) -> Option<ScopedOperand> {
    if name == utf16!("undefined") {
        return Some(gen.add_constant_undefined());
    }
    if name == utf16!("NaN") {
        return Some(gen.add_constant_number(f64::NAN));
    }
    if name == utf16!("Infinity") {
        return Some(gen.add_constant_number(f64::INFINITY));
    }
    if let Some(op) = try_generate_builtin_constant(gen, &name.into()) {
        return Some(op);
    }
    None
}

// =============================================================================
// Binary operator emission
// =============================================================================

fn emit_binary_op(
    gen: &mut Generator,
    op: BinaryOp,
    dst: &ScopedOperand,
    lhs: &ScopedOperand,
    rhs: &ScopedOperand,
) {
    let dst_op = dst.operand();
    let lhs_op = lhs.operand();
    let rhs_op = rhs.operand();
    match op {
        BinaryOp::Addition => gen.emit(Instruction::Add { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::Subtraction => gen.emit(Instruction::Sub { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::Multiplication => gen.emit(Instruction::Mul { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::Division => gen.emit(Instruction::Div { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::Modulo => gen.emit(Instruction::Mod { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::Exponentiation => gen.emit(Instruction::Exp { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::StrictlyEquals => gen.emit(Instruction::StrictlyEquals { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::StrictlyInequals => gen.emit(Instruction::StrictlyInequals { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::LooselyEquals => gen.emit(Instruction::LooselyEquals { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::LooselyInequals => gen.emit(Instruction::LooselyInequals { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::GreaterThan => gen.emit(Instruction::GreaterThan { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::GreaterThanEquals => gen.emit(Instruction::GreaterThanEquals { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::LessThan => gen.emit(Instruction::LessThan { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::LessThanEquals => gen.emit(Instruction::LessThanEquals { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::BitwiseAnd => gen.emit(Instruction::BitwiseAnd { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::BitwiseOr => {
            // OPTIMIZATION: x | 0 == ToInt32(x) (matches C++)
            if let Some(ConstantValue::Number(n)) = gen.get_constant(rhs) {
                if *n == 0.0 && n.is_sign_positive() {
                    gen.emit(Instruction::ToInt32 { dst: dst_op, value: lhs_op });
                } else {
                    gen.emit(Instruction::BitwiseOr { dst: dst_op, lhs: lhs_op, rhs: rhs_op });
                }
            } else {
                gen.emit(Instruction::BitwiseOr { dst: dst_op, lhs: lhs_op, rhs: rhs_op });
            }
        }
        BinaryOp::BitwiseXor => gen.emit(Instruction::BitwiseXor { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::LeftShift => gen.emit(Instruction::LeftShift { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::RightShift => gen.emit(Instruction::RightShift { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::UnsignedRightShift => gen.emit(Instruction::UnsignedRightShift { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::In => gen.emit(Instruction::In { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        BinaryOp::InstanceOf => gen.emit(Instruction::InstanceOf { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
    }
}

// =============================================================================
// Logical expression (short-circuit)
// =============================================================================

fn generate_logical(
    gen: &mut Generator,
    op: LogicalOp,
    lhs: &Expression,
    rhs: &Expression,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let lhs_val = generate_expression(lhs, gen, preferred_dst)?;

    // Constant-fold: if LHS is a constant, we can statically determine the branch.
    if let Some(constant) = gen.get_constant(&lhs_val) {
        let is_nullish = matches!(constant, ConstantValue::Null | ConstantValue::Undefined);
        if let Some(is_truthy) = constant_to_boolean(constant) {
            let take_rhs = match op {
                LogicalOp::And => is_truthy,
                LogicalOp::Or => !is_truthy,
                LogicalOp::NullishCoalescing => is_nullish,
            };
            if take_rhs {
                let dst = choose_dst(gen, preferred_dst);
                let rhs_val = generate_expression(rhs, gen, Some(&dst))?;
                if rhs_val.operand().is_constant() {
                    return Some(rhs_val);
                }
                gen.emit_mov(&dst, &rhs_val);
                return Some(dst);
            }
            return Some(lhs_val);
        }
    }

    let dst = choose_dst(gen, preferred_dst);
    gen.emit_mov(&dst, &lhs_val);

    let rhs_block = gen.make_block();
    let end_block = gen.make_block();

    match op {
        LogicalOp::And => {
            // If lhs is falsy, short-circuit to end
            gen.emit_jump_if(&lhs_val, rhs_block, end_block);
        }
        LogicalOp::Or => {
            // If lhs is truthy, short-circuit to end
            gen.emit_jump_if(&lhs_val, end_block, rhs_block);
        }
        LogicalOp::NullishCoalescing => {
            gen.emit(Instruction::JumpNullish {
                condition: lhs_val.operand(),
                true_target: rhs_block,
                false_target: end_block,
            });
        }
    }

    gen.switch_to_basic_block(rhs_block);
    let rhs_val = generate_expression(rhs, gen, Some(&dst));
    if let Some(rhs_val) = &rhs_val {
        gen.emit_mov(&dst, rhs_val);
    }
    if !gen.is_current_block_terminated() {
        gen.emit(Instruction::Jump {
            target: end_block,
        });
    }

    gen.switch_to_basic_block(end_block);
    Some(dst)
}

// =============================================================================
// Conditional expression (ternary)
// =============================================================================

fn generate_conditional(
    gen: &mut Generator,
    test: &Expression,
    consequent: &Expression,
    alternate: &Expression,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let predicate = generate_expression(test, gen, None)?;

    // OPTIMIZATION: if the predicate is always true/false, only generate the taken expression.
    if let Some(constant) = gen.get_constant(&predicate) {
        if let Some(is_truthy) = constant_to_boolean(constant) {
            if is_truthy {
                return generate_expression(consequent, gen, preferred_dst);
            }
            return generate_expression(alternate, gen, preferred_dst);
        }
    }

    let true_block = gen.make_block();
    let false_block = gen.make_block();
    let end_block = gen.make_block();

    gen.emit_jump_if(&predicate, true_block, false_block);

    let dst = choose_dst(gen, preferred_dst);

    gen.switch_to_basic_block(true_block);
    let cons_val = generate_expression(consequent, gen, None);
    if let Some(val) = &cons_val {
        gen.emit_mov(&dst, val);
    }
    if !gen.is_current_block_terminated() {
        gen.emit(Instruction::Jump {
            target: end_block,
        });
    }

    gen.switch_to_basic_block(false_block);
    let alt_val = generate_expression(alternate, gen, None);
    if let Some(val) = &alt_val {
        gen.emit_mov(&dst, val);
    }
    if !gen.is_current_block_terminated() {
        gen.emit(Instruction::Jump {
            target: end_block,
        });
    }

    gen.switch_to_basic_block(end_block);
    Some(dst)
}

/// Generate a statement while propagating the completion register.
///
/// Saves and restores `gen.current_completion_register`, and emits a mov
/// from the statement's result to the completion register when appropriate.
fn generate_with_completion(
    body: &Statement,
    gen: &mut Generator,
    completion: &Option<ScopedOperand>,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let saved = gen.current_completion_register.clone();
    if let Some(ref c) = completion {
        gen.current_completion_register = Some(c.clone());
    }
    let result = generate_statement(body, gen, preferred_dst);
    if !gen.is_current_block_terminated() {
        if let (Some(ref c), Some(ref val)) = (completion, &result) {
            gen.emit_mov(c, val);
        }
    }
    gen.current_completion_register = saved;
    result
}

// =============================================================================
// If statement
// =============================================================================

fn generate_if_statement(
    gen: &mut Generator,
    test: &Expression,
    consequent: &Statement,
    alternate: Option<&Statement>,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let pred = generate_expression_or_undefined(test, gen, None);

    let completion = if gen.must_propagate_completion {
        let reg = choose_dst(gen, preferred_dst);
        let undef = gen.add_constant_undefined();
        gen.emit_mov(&reg, &undef);
        Some(reg)
    } else {
        None
    };

    // OPTIMIZATION: if the predicate is always true/false, only build the taken branch.
    if let Some(constant) = gen.get_constant(&pred) {
        if let Some(is_truthy) = constant_to_boolean(constant) {
            // Pass the completion register as preferred_dst so nested
            // if-statements reuse the same register (matching C++).
            let child_dst = completion.as_ref().or(preferred_dst);
            if is_truthy {
                generate_with_completion(consequent, gen, &completion, child_dst);
            } else if let Some(alt) = alternate {
                generate_with_completion(alt, gen, &completion, child_dst);
            }
            return completion;
        }
    }

    let true_block = gen.make_block();
    let false_block = gen.make_block();
    let has_alternate = alternate.is_some();
    let end_block = if has_alternate { gen.make_block() } else { false_block };

    gen.emit_jump_if(&pred, true_block, false_block);

    // Pass completion as preferred_dst to children so nested if-else chains
    // reuse the same completion register, matching C++ behavior.
    let child_preferred_dst = completion.as_ref();

    // Consequent
    gen.switch_to_basic_block(true_block);
    let saved_completion = gen.current_completion_register.clone();
    if let Some(ref c) = completion {
        gen.current_completion_register = Some(c.clone());
    }
    let cons_result = generate_statement(consequent, gen, child_preferred_dst);
    if !gen.is_current_block_terminated() {
        if let (Some(ref c), Some(ref val)) = (&completion, &cons_result) {
            gen.emit_mov(c, val);
        }
        gen.emit(Instruction::Jump {
            target: end_block,
        });
    }
    // FIXME: Remove this manual drop() when we no longer need to match C++ register allocation.
    drop(cons_result);
    gen.current_completion_register = saved_completion.clone();

    // Alternate
    if let Some(alt) = alternate {
        gen.switch_to_basic_block(false_block);
        if let Some(ref c) = completion {
            gen.current_completion_register = Some(c.clone());
        }
        let alt_result = generate_statement(alt, gen, child_preferred_dst);
        if !gen.is_current_block_terminated() {
            if let (Some(ref c), Some(ref val)) = (&completion, &alt_result) {
                gen.emit_mov(c, val);
            }
            gen.emit(Instruction::Jump {
                target: end_block,
            });
        }
        gen.current_completion_register = saved_completion;
    }

    gen.switch_to_basic_block(end_block);
    completion
}

// =============================================================================
// While statement
// =============================================================================

fn generate_while_statement(
    gen: &mut Generator,
    test: &Expression,
    body: &Statement,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let test_block = gen.make_block();

    let completion = gen.allocate_completion_register();

    gen.emit(Instruction::Jump {
        target: test_block,
    });

    gen.switch_to_basic_block(test_block);
    let test_val = generate_expression_or_undefined(test, gen, None);

    // OPTIMIZATION: If predicate is always false, ignore body and exit early.
    if let Some(constant) = gen.get_constant(&test_val) {
        if constant_to_boolean(constant) == Some(false) {
            return completion;
        }
    }

    let body_block = gen.make_block();
    let end_block = gen.make_block();

    gen.emit_jump_if(&test_val, body_block, end_block);

    gen.switch_to_basic_block(body_block);
    let labels = std::mem::take(&mut gen.pending_labels);
    gen.begin_continuable_scope(test_block, labels.clone(), completion.clone());
    gen.begin_breakable_scope(end_block, labels, completion.clone());

    generate_with_completion(body, gen, &completion, preferred_dst);

    gen.end_breakable_scope();
    gen.end_continuable_scope();
    if !gen.is_current_block_terminated() {
        gen.emit(Instruction::Jump {
            target: test_block,
        });
    }

    gen.switch_to_basic_block(end_block);
    completion
}

// =============================================================================
// DoWhile statement
// =============================================================================

fn generate_do_while_statement(
    gen: &mut Generator,
    test: &Expression,
    body: &Statement,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let body_block = gen.make_block();
    let test_block = gen.make_block();
    let load_result_and_jump_to_end_block = gen.make_block();
    let end_block = gen.make_block();

    let completion = gen.allocate_completion_register();

    gen.emit(Instruction::Jump {
        target: body_block,
    });

    // Generate test FIRST, matching C++ which keeps the test ScopedOperand
    // alive during body generation, consuming a register from the free pool.
    gen.switch_to_basic_block(test_block);
    let test_val = generate_expression_or_undefined(test, gen, None);
    gen.emit_jump_if(
        &test_val,
        body_block,
        load_result_and_jump_to_end_block,
    );

    // Generate body SECOND (test_val still alive, matching C++ register allocation).
    gen.switch_to_basic_block(body_block);
    let labels = std::mem::take(&mut gen.pending_labels);
    gen.begin_continuable_scope(test_block, labels.clone(), completion.clone());
    gen.begin_breakable_scope(end_block, labels, completion.clone());

    generate_with_completion(body, gen, &completion, preferred_dst);

    gen.end_breakable_scope();
    gen.end_continuable_scope();
    if !gen.is_current_block_terminated() {
        gen.emit(Instruction::Jump {
            target: test_block,
        });
    }

    gen.switch_to_basic_block(load_result_and_jump_to_end_block);
    gen.emit(Instruction::Jump {
        target: end_block,
    });

    gen.switch_to_basic_block(end_block);
    completion
}

// =============================================================================
// For statement
// =============================================================================

fn generate_for_statement(
    gen: &mut Generator,
    init: Option<&ForInit>,
    test: Option<&Expression>,
    update: Option<&Expression>,
    body: &Statement,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // Check if init is a lexical declaration (let/const) with non-local variables.
    // If so, we need to create a lexical environment for the loop variables and
    // implement per-iteration copy semantics (CreatePerIterationEnvironment).
    let mut has_lexical_environment = false;
    let mut per_iteration_binding_names: Vec<Utf16String> = Vec::new();

    if let Some(ForInit::Declaration(init)) = init {
        if let StatementKind::VariableDeclaration { kind, declarations } = &init.inner {
            if *kind == DeclarationKind::Let || *kind == DeclarationKind::Const {
                let mut non_local_names: Vec<(Utf16String, bool)> = Vec::new();
                for declaration in declarations {
                    collect_target_names(&declaration.target, &mut non_local_names);
                }
                if !non_local_names.is_empty() {
                    has_lexical_environment = true;
                    let is_const = *kind == DeclarationKind::Const;

                    // begin_variable_scope: CreateLexicalEnvironment
                    gen.push_new_lexical_environment(0);

                    for (name, _) in &non_local_names {
                        let id = gen.intern_identifier(name);
                        gen.emit(Instruction::CreateVariable {
                            identifier: id,
                            mode: EnvironmentMode::Lexical as u32,
                            is_immutable: is_const,
                            is_global: false,
                            is_strict: false,
                        });
                        if !is_const {
                            per_iteration_binding_names.push(name.clone());
                        }
                    }
                }
            }
        }
    }

    // Init
    match init {
        Some(ForInit::Declaration(decl)) => { generate_statement(decl, gen, None); }
        Some(ForInit::Expression(expr)) => { generate_expression(expr, gen, None); }
        None => {}
    }

    // CreatePerIterationEnvironment after init (first iteration setup).
    emit_per_iteration_bindings(gen, &per_iteration_binding_names);

    // Block creation order matches C++: body → update (if exists) → test (if exists) → end.
    // If 'test' is missing, fuse 'test' and 'body' blocks.
    // If 'update' is missing, fuse 'body' and 'update' blocks.
    let body_block = gen.make_block();
    let update_block = if update.is_some() { gen.make_block() } else { body_block };
    let test_block = if test.is_some() { gen.make_block() } else { body_block };
    let end_block = gen.make_block();

    let completion = gen.allocate_completion_register();

    gen.emit(Instruction::Jump {
        target: test_block,
    });

    // Test
    if let Some(test_expression) = test {
        gen.switch_to_basic_block(test_block);
        let test_val = generate_expression_or_undefined(test_expression, gen, None);

        // OPTIMIZATION: test value is always falsey, skip body entirely.
        if let Some(constant) = gen.get_constant(&test_val) {
            if constant_to_boolean(constant) == Some(false) {
                gen.emit(Instruction::Jump {
                    target: end_block,
                });
                gen.switch_to_basic_block(end_block);
                if has_lexical_environment {
                    gen.lexical_environment_register_stack.pop();
                    if !gen.is_current_block_terminated() {
                        let parent = gen.current_lexical_environment();
                        gen.emit(Instruction::SetLexicalEnvironment { environment: parent.operand() });
                    }
                }
                return completion;
            }
        }

        gen.emit_jump_if(&test_val, body_block, end_block);
    }

    // Update
    if let Some(update_expression) = update {
        gen.switch_to_basic_block(update_block);
        generate_expression(update_expression, gen, None);
        gen.emit(Instruction::Jump {
            target: test_block,
        });
    }

    // Body
    gen.switch_to_basic_block(body_block);
    let labels = std::mem::take(&mut gen.pending_labels);
    let continue_target = if update.is_some() { update_block } else { test_block };
    gen.begin_continuable_scope(continue_target, labels.clone(), completion.clone());
    gen.begin_breakable_scope(end_block, labels, completion.clone());

    generate_with_completion(body, gen, &completion, preferred_dst);

    gen.end_breakable_scope();
    gen.end_continuable_scope();
    if !gen.is_current_block_terminated() {
        // CreatePerIterationEnvironment at end of each iteration.
        emit_per_iteration_bindings(gen, &per_iteration_binding_names);
        if update.is_some() {
            gen.emit(Instruction::Jump {
                target: update_block,
            });
        } else {
            gen.emit(Instruction::Jump {
                target: test_block,
            });
        }
    }

    gen.switch_to_basic_block(end_block);

    // end_variable_scope: restore parent environment
    if has_lexical_environment {
        gen.lexical_environment_register_stack.pop();
        if !gen.is_current_block_terminated() {
            let parent = gen.current_lexical_environment();
            gen.emit(Instruction::SetLexicalEnvironment { environment: parent.operand() });
        }
    }

    completion
}

/// Emit CreatePerIterationEnvironment: save current binding values, pop env,
/// push new env, re-create variables, and re-initialize from saved values.
/// This implements per-iteration lexical scoping for `for (let ...)` loops.
fn emit_per_iteration_bindings(gen: &mut Generator, bindings: &[Utf16String]) {
    if bindings.is_empty() {
        return;
    }

    // Save current values into registers.
    let mut saved: Vec<(ScopedOperand, IdentifierTableIndex)> = Vec::with_capacity(bindings.len());
    for name in bindings {
        let id = gen.intern_identifier(name);
        let reg = gen.allocate_register();
        gen.emit(Instruction::GetBinding {
            dst: reg.operand(),
            identifier: id,
            cache: EnvironmentCoordinate::empty(),
        });
        saved.push((reg, id));
    }

    // Pop current environment (end_variable_scope).
    gen.lexical_environment_register_stack.pop();
    let parent = gen.current_lexical_environment();
    gen.emit(Instruction::SetLexicalEnvironment { environment: parent.operand() });

    // Push new environment (begin_variable_scope).
    gen.push_new_lexical_environment(0);

    // Re-create variables and initialize from saved values.
    for (reg, id) in &saved {
        gen.emit(Instruction::CreateVariable {
            identifier: *id,
            mode: EnvironmentMode::Lexical as u32,
            is_immutable: false,
            is_global: false,
            is_strict: false,
        });
        gen.emit(Instruction::InitializeLexicalBinding {
            identifier: *id,
            src: reg.operand(),
            cache: EnvironmentCoordinate::empty(),
        });
    }
}

// =============================================================================
// Scope children (Block, FunctionBody, Program)
// =============================================================================

fn generate_scope_children(
    gen: &mut Generator,
    scope: &ScopeData,
    _preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let mut last_result = None;
    for child in &scope.children {
        let result = generate_statement(child, gen, None);
        if gen.must_propagate_completion {
            if let Some(ref val) = result {
                last_result = result.clone();
                if !gen.is_current_block_terminated() {
                    if let Some(ref completion_reg) = gen.current_completion_register.clone() {
                        gen.emit_mov(completion_reg, val);
                    }
                }
            }
        }
        // NB: When must_propagate_completion is false, we intentionally do NOT
        // accumulate results into last_result. This matches C++ behavior where
        // `result` goes out of scope at the end of each loop iteration, freeing
        // any temporary registers immediately.
        if gen.is_current_block_terminated() {
            break;
        }
    }
    last_result
}

/// Generate bytecode for a block statement, creating a lexical environment
/// if the block has non-local lexical declarations (let/const/class).
fn generate_block_statement(
    gen: &mut Generator,
    scope: &ScopeData,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let did_create_env = emit_block_declaration_instantiation(gen, scope);
    if did_create_env {
        gen.start_boundary(BlockBoundaryType::LeaveLexicalEnvironment);
    }

    // The parser wraps for-loop statements in a Block for scope tracking
    // (via close_for_loop_scope). When the block doesn't create a lexical
    // environment and its only child is a for-loop variant, skip
    // generate_scope_children and generate the child directly to avoid
    // emitting a redundant completion Mov that C++ doesn't produce.
    let result = if !did_create_env && scope.children.len() == 1 && is_for_loop(&scope.children[0]) {
        generate_statement(&scope.children[0], gen, preferred_dst)
    } else {
        generate_scope_children(gen, scope, preferred_dst)
    };

    if did_create_env {
        gen.end_variable_scope();
    }
    result
}

/// Create lexical bindings and instantiate function declarations for a block.
/// For each declaration, creates bindings and immediately instantiates functions
/// (single pass, not two separate passes).
fn emit_lexical_declarations_for_block<'a>(gen: &mut Generator, environment: &ScopedOperand, children: impl Iterator<Item = &'a Statement>) {
    for child in children {
        match &child.inner {
            StatementKind::VariableDeclaration { kind, declarations } => {
                if *kind == DeclarationKind::Let || *kind == DeclarationKind::Const {
                    let is_constant = *kind == DeclarationKind::Const;
                    for declaration in declarations {
                        let mut names = Vec::new();
                        collect_target_names(&declaration.target, &mut names);
                        for (name, _) in &names {
                            let id = gen.intern_identifier(name);
                            if is_constant {
                                gen.emit(Instruction::CreateImmutableBinding {
                                    environment: environment.operand(),
                                    identifier: id,
                                    strict_binding: true,
                                });
                            } else {
                                gen.emit(Instruction::CreateMutableBinding {
                                    environment: environment.operand(),
                                    identifier: id,
                                    can_be_deleted: false,
                                });
                            }
                        }
                    }
                }
            }
            StatementKind::UsingDeclaration { declarations } => {
                for declaration in declarations {
                    let mut names = Vec::new();
                    collect_target_names(&declaration.target, &mut names);
                    for (name, _) in &names {
                        let id = gen.intern_identifier(name);
                        gen.emit(Instruction::CreateImmutableBinding {
                            environment: environment.operand(),
                            identifier: id,
                            strict_binding: true,
                        });
                    }
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(ref name_ident) = class_data.name {
                    if !name_ident.is_local() {
                        let id = gen.intern_identifier(&name_ident.name);
                        gen.emit(Instruction::CreateMutableBinding {
                            environment: environment.operand(),
                            identifier: id,
                            can_be_deleted: false,
                        });
                    }
                }
            }
            StatementKind::FunctionDeclaration { function_id, name: Some(ref name_ident), .. } => {
                // a. Create binding.
                if !name_ident.is_local() {
                    let id = gen.intern_identifier(&name_ident.name);
                    gen.emit(Instruction::CreateMutableBinding {
                        environment: environment.operand(),
                        identifier: id,
                        can_be_deleted: false,
                    });
                }
                // b. Instantiate function object.
                let function_data = gen.function_table.take(*function_id);
                let sfd_index = emit_new_function(gen, function_data, None);
                let fo = gen.allocate_register();
                gen.emit(Instruction::NewFunction {
                    dst: fo.operand(),
                    shared_function_data_index: sfd_index,
                    home_object: None,
                    lhs_name: None,
                });
                if name_ident.is_local() {
                    let local_index = name_ident.local_index.get();
                    let local = gen.local(local_index);
                    gen.emit_mov(&local, &fo);
                    gen.mark_local_initialized(local_index);
                } else {
                    let id = gen.intern_identifier(&name_ident.name);
                    gen.emit(Instruction::InitializeLexicalBinding {
                        identifier: id,
                        src: fo.operand(),
                        cache: EnvironmentCoordinate::empty(),
                    });
                }
            }
            _ => {}
        }
    }
}

fn emit_block_declaration_instantiation(gen: &mut Generator, scope: &ScopeData) -> bool {
    if !needs_block_declaration_instantiation(scope) {
        return false;
    }

    let new_env = gen.push_new_lexical_environment(0);

    emit_lexical_declarations_for_block(gen, &new_env, scope.children.iter());

    true
}

// =============================================================================
// Variable declaration
// =============================================================================

fn generate_variable_declaration(
    gen: &mut Generator,
    kind: DeclarationKind,
    declarations: &[VariableDeclarator],
) {
    for declaration in declarations {
        // OPTIMIZATION: For let/const declarations where the target is a local identifier,
        // pass the local as preferred_dst to the initializer. This allows NewArray, NewFunction,
        // Add, etc. to write directly to the local instead of temp+Mov.
        // NB: Not safe for `var` since var declarations can have duplicates, meaning the
        // preferred_dst could be used as input in the initializer.
        let init_dst = if kind != DeclarationKind::Var {
            if let VariableDeclaratorTarget::Identifier(ident) = &declaration.target {
                if ident.is_local() && ident.local_type.get() == Some(LocalType::Variable) {
                    Some(gen.local(ident.local_index.get()))
                } else {
                    None
                }
            } else {
                None
            }
        } else {
            None
        };

        // Set pending LHS name for function name inference.
        if let VariableDeclaratorTarget::Identifier(ident) = &declaration.target {
            gen.pending_lhs_name = Some(gen.intern_identifier(&ident.name));
        }
        let init_value = declaration.init.as_ref().and_then(|init| generate_expression(init, gen, init_dst.as_ref()));
        gen.pending_lhs_name = None;

        match &declaration.target {
            VariableDeclaratorTarget::Identifier(ident) => {
                // var declarations without initializer don't need to assign undefined.
                // The FDI already handles initialization for var bindings.
                if init_value.is_none() && kind == DeclarationKind::Var {
                    if ident.is_local() {
                        gen.mark_local_initialized(ident.local_index.get());
                    }
                    continue;
                }
                let value = init_value.unwrap_or_else(|| gen.add_constant_undefined());
                if ident.is_local() {
                    let local_index = ident.local_index.get();
                    let local = gen.resolve_local(local_index, ident.local_type.get().unwrap());
                    gen.emit_mov(&local, &value);
                    gen.mark_local_initialized(local_index);
                } else {
                    let id = gen.intern_identifier(&ident.name);
                    match kind {
                        DeclarationKind::Var => {
                            if ident.is_global.get() {
                                let cache = gen.next_global_variable_cache();
                                gen.emit(Instruction::SetGlobal {
                                    identifier: id,
                                    src: value.operand(),
                                    cache_index: cache,
                                });
                            } else {
                                gen.emit(Instruction::SetLexicalBinding {
                                    identifier: id,
                                    src: value.operand(),
                                    cache: EnvironmentCoordinate::empty(),
                                });
                            }
                        }
                        DeclarationKind::Let | DeclarationKind::Const => {
                            gen.emit(Instruction::InitializeLexicalBinding {
                                identifier: id,
                                src: value.operand(),
                                cache: EnvironmentCoordinate::empty(),
                            });
                        }
                    }
                }
            }
            VariableDeclaratorTarget::BindingPattern(pattern) => {
                if let Some(value) = init_value {
                    let mode = match kind {
                        DeclarationKind::Var => BindingMode::Set,
                        DeclarationKind::Let | DeclarationKind::Const => {
                            BindingMode::InitializeLexical
                        }
                    };
                    generate_binding_pattern_bytecode(gen, pattern, mode, &value);
                }
            }
        }
    }
}

// =============================================================================
// Call expression
// =============================================================================

fn try_generate_builtin_abstract_operation(
    gen: &mut Generator,
    data: &CallExpressionData,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<Option<ScopedOperand>> {
    if !gen.builtin_abstract_operations_enabled {
        return None;
    }
    let name = match &data.callee.inner {
        ExpressionKind::Identifier(ident) => &ident.name,
        _ => return None,
    };
    if data.arguments.iter().any(|a| a.is_spread) {
        return None;
    }

    let dst = choose_dst(gen, preferred_dst);

    // Operations that map to dedicated bytecode instructions.
    if name == utf16!("IsCallable") {
        let value = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        gen.emit(Instruction::IsCallable { dst: dst.operand(), value: value.operand() });
        return Some(Some(dst));
    }
    if name == utf16!("IsConstructor") {
        let value = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        gen.emit(Instruction::IsConstructor { dst: dst.operand(), value: value.operand() });
        return Some(Some(dst));
    }
    if name == utf16!("ToBoolean") {
        let value = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        gen.emit(Instruction::ToBoolean { dst: dst.operand(), value: value.operand() });
        return Some(Some(dst));
    }
    if name == utf16!("ToObject") {
        let value = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        gen.emit(Instruction::ToObject { dst: dst.operand(), value: value.operand() });
        return Some(Some(dst));
    }
    if name == utf16!("ToLength") {
        let value = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        gen.emit(Instruction::ToLength { dst: dst.operand(), value: value.operand() });
        return Some(Some(dst));
    }
    if name == utf16!("ThrowIfNotObject") {
        let src = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        gen.emit(Instruction::ThrowIfNotObject { src: src.operand() });
        return Some(Some(dst));
    }
    if name == utf16!("ThrowTypeError") {
        if let ExpressionKind::StringLiteral(ref s) = data.arguments[0].value.inner {
            let message_string = gen.intern_string(s);
            let type_error_register = gen.allocate_register();
            gen.emit(Instruction::NewTypeError {
                dst: type_error_register.operand(),
                error_string: message_string,
            });
            gen.perform_needed_unwinds();
            gen.emit(Instruction::Throw { src: type_error_register.operand() });
            return Some(Some(dst));
        }
        return None;
    }
    if name == utf16!("NewTypeError") {
        if let ExpressionKind::StringLiteral(ref s) = data.arguments[0].value.inner {
            let message_string = gen.intern_string(s);
            gen.emit(Instruction::NewTypeError {
                dst: dst.operand(),
                error_string: message_string,
            });
            return Some(Some(dst));
        }
        return None;
    }
    if name == utf16!("NewObjectWithNoPrototype") {
        gen.emit(Instruction::NewObjectWithNoPrototype { dst: dst.operand() });
        return Some(Some(dst));
    }
    if name == utf16!("NewArrayWithLength") {
        let length = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        gen.emit(Instruction::NewArrayWithLength {
            dst: dst.operand(),
            array_length: length.operand(),
        });
        return Some(Some(dst));
    }
    if name == utf16!("CreateAsyncFromSyncIterator") {
        let iterator = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        let next_method = generate_expression_or_undefined(&data.arguments[1].value, gen, None);
        let done = generate_expression_or_undefined(&data.arguments[2].value, gen, None);
        gen.emit(Instruction::CreateAsyncFromSyncIterator {
            dst: dst.operand(),
            iterator: iterator.operand(),
            next_method: next_method.operand(),
            done: done.operand(),
        });
        return Some(Some(dst));
    }
    if name == utf16!("CreateDataPropertyOrThrow") {
        let object = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        let property = generate_expression_or_undefined(&data.arguments[1].value, gen, None);
        let value = generate_expression_or_undefined(&data.arguments[2].value, gen, None);
        gen.emit(Instruction::CreateDataPropertyOrThrow {
            object: object.operand(),
            property: property.operand(),
            value: value.operand(),
        });
        return Some(Some(dst));
    }
    if name == utf16!("Call") {
        let callee = generate_expression_or_undefined(&data.arguments[0].value, gen, None);
        let this_value = generate_expression_or_undefined(&data.arguments[1].value, gen, None);
        let extra_args = &data.arguments[2..];
        let mut argument_holders = Vec::with_capacity(extra_args.len());
        for argument in extra_args {
            let val = generate_expression_or_undefined(&argument.value, gen, None);
            argument_holders.push(gen.copy_if_needed_to_preserve_evaluation_order(&val));
        }
        let callee_name = expression_string_approximation(&data.arguments[0].value)
            .map(|s| gen.intern_string(&s));
        let arguments: Vec<Operand> = argument_holders.iter().map(|a| a.operand()).collect();
        gen.emit(Instruction::Call {
            dst: dst.operand(),
            callee: callee.operand(),
            this_value: this_value.operand(),
            argument_count: u32_from_usize(arguments.len()),
            expression_string: callee_name,
            arguments,
        });
        return Some(Some(dst));
    }

    // Operations that map to intrinsic function calls.
    let known_operations: &[&[u16]] = &[
        utf16!("AsyncIteratorClose"),
        utf16!("GetMethod"),
        utf16!("GetIteratorDirect"),
        utf16!("GetIteratorFromMethod"),
        utf16!("IteratorComplete"),
    ];
    for &op_name in known_operations {
        if *name == op_name {
            let intrinsic_value = unsafe {
                super::ffi::get_abstract_operation_function(gen.vm_ptr, op_name.as_ptr(), op_name.len())
            };
            let callee = gen.add_constant_raw_value(intrinsic_value);
            let undefined = gen.add_constant_undefined();
            let expression_string = gen.intern_string(name);
            let mut argument_holders = Vec::with_capacity(data.arguments.len());
            for argument in &data.arguments {
                let val = generate_expression_or_undefined(&argument.value, gen, None);
                argument_holders.push(gen.copy_if_needed_to_preserve_evaluation_order(&val));
            }
            let arguments: Vec<Operand> = argument_holders.iter().map(|a| a.operand()).collect();
            gen.emit(Instruction::Call {
                dst: dst.operand(),
                callee: callee.operand(),
                this_value: undefined.operand(),
                argument_count: u32_from_usize(arguments.len()),
                expression_string: Some(expression_string),
                arguments,
            });
            return Some(Some(dst));
        }
    }

    None
}

/// Try to generate a builtin constant (e.g. SYMBOL_ITERATOR).
/// Returns Some(operand) if the identifier is a known builtin constant.
fn try_generate_builtin_constant(gen: &mut Generator, name: &Utf16String) -> Option<ScopedOperand> {
    if !gen.builtin_abstract_operations_enabled {
        return None;
    }
    if *name == utf16!("SYMBOL_ITERATOR") {
        let value = unsafe { super::ffi::get_well_known_symbol(gen.vm_ptr, 0) };
        return Some(gen.add_constant_raw_value(value));
    }
    if *name == utf16!("SYMBOL_ASYNC_ITERATOR") {
        let value = unsafe { super::ffi::get_well_known_symbol(gen.vm_ptr, 1) };
        return Some(gen.add_constant_raw_value(value));
    }
    if *name == utf16!("MAX_ARRAY_LIKE_INDEX") {
        return Some(gen.add_constant_number(9007199254740991.0));
    }
    None
}

/// Generate bytecode for a call expression (`f()`) or new expression (`new C()`).
///
/// Handles several special forms:
/// - Direct `eval()` calls (CallWithArgumentArray with IsDirectEval flag)
/// - Member calls (`obj.f()`) that need to pass `this`
/// - Super calls (`super()`)
/// - Spread arguments (CallWithArgumentArray)
/// - Builtin abstract operation detection for built-in JS files
fn generate_call_expression(
    gen: &mut Generator,
    data: &CallExpressionData,
    preferred_dst: Option<&ScopedOperand>,
    is_new: bool,
) -> Option<ScopedOperand> {
    // Check for builtin abstract operations before anything else.
    if !is_new {
        if let Some(result) = try_generate_builtin_abstract_operation(gen, data, preferred_dst) {
            return result;
        }
    }

    let dst = choose_dst(gen, preferred_dst);

    // Compute expression_string for error messages (e.g. "true is not a function (evaluated from 'a')").
    let expression_string: Option<StringTableIndex> =
        expression_string_approximation(&data.callee).map(|s| gen.intern_string(&s));

    // Detect direct eval calls: bare identifier "eval" as callee.
    let is_direct_eval = !is_new
        && matches!(&data.callee.inner, ExpressionKind::Identifier(ident) if ident.name == utf16!("eval"));

    // Detect known builtins for member expression callees (e.g. Math.abs).
    let builtin: Option<u8> = if !is_new {
        get_builtin(&data.callee)
    } else {
        None
    };

    // For method calls (obj.method()), we need to use the object as `this`.
    let (callee, this_value) = if !is_new {
        match &data.callee.inner {
            ExpressionKind::Member {
                object,
                property,
                computed,
            } if matches!(object.inner, ExpressionKind::Super) => {
                // Super member call: super.method() or super[expr]()
                // Must match C++ evaluation order:
                // 1. ResolveThisBinding
                // 2. Evaluate computed property (if any)
                // 3. ResolveSuperBase
                // 4. GetByIdWithThis / GetByValueWithThis
                let this_value = emit_resolve_this_binding(gen);
                let computed_key = if *computed {
                    Some(generate_expression_or_undefined(property, gen, None))
                } else {
                    None
                };
                let super_base = gen.allocate_register();
                gen.emit(Instruction::ResolveSuperBase { dst: super_base.operand() });
                let method = gen.allocate_register();
                if let Some(key) = computed_key {
                    emit_get_by_value_with_this(gen, &method, &super_base, &key, &this_value);
                } else if let ExpressionKind::Identifier(ident) = &property.inner {
                    let key = gen.intern_property_key(&ident.name);
                    let cache = gen.next_property_lookup_cache();
                    gen.emit(Instruction::GetByIdWithThis {
                        dst: method.operand(),
                        base: super_base.operand(),
                        property: key,
                        this_value: this_value.operand(),
                        cache_index: cache,
                    });
                }
                (method, Some(this_value))
            }
            ExpressionKind::Member {
                object,
                property,
                computed,
            } => {
                let obj = generate_expression_or_undefined(object, gen, None);
                let base_id = intern_base_identifier(gen, object);
                let method = gen.allocate_register();
                if *computed {
                    let property = generate_expression_or_undefined(property, gen, None);
                    emit_get_by_value(gen, &method, &obj, &property, None);
                } else if let ExpressionKind::Identifier(ident) = &property.inner {
                    emit_get_by_id(gen, &method, &obj, &ident.name, base_id);
                } else if let ExpressionKind::PrivateIdentifier(priv_ident) = &property.inner {
                    let id = gen.intern_identifier(&priv_ident.name);
                    gen.emit(Instruction::GetPrivateById {
                        dst: method.operand(),
                        base: obj.operand(),
                        property: id,
                    });
                }
                (method, Some(obj))
            }
            ExpressionKind::Identifier(ident) if ident.is_local() => {
                // Local identifier: use the local directly, with ThrowIfTDZ
                // if not yet initialized (matching C++ CallExpression codegen).
                let local = gen.resolve_local(ident.local_index.get(), ident.local_type.get().unwrap());
                let needs_tdz = if ident.local_type.get() == Some(LocalType::Argument) {
                    !gen.is_argument_initialized(ident.local_index.get())
                } else {
                    gen.is_local_lexically_declared(ident.local_index.get()) && !gen.is_local_initialized(ident.local_index.get())
                };
                if needs_tdz {
                    gen.emit(Instruction::ThrowIfTDZ {
                        src: local.operand(),
                    });
                }
                (local, None)
            }
            ExpressionKind::Identifier(ident) if !ident.is_global.get() => {
                // Non-local, non-global identifier: use GetCalleeAndThisFromEnvironment
                // to properly handle with-statement bindings and eval.
                let callee_reg = gen.allocate_register();
                let this_reg = gen.allocate_register();
                let id = gen.intern_identifier(&ident.name);
                gen.emit(Instruction::GetCalleeAndThisFromEnvironment {
                    callee: callee_reg.operand(),
                    this_value: this_reg.operand(),
                    identifier: id,
                    cache: EnvironmentCoordinate::empty(),
                });
                (callee_reg, Some(this_reg))
            }
            ExpressionKind::OptionalChain { base, references } => {
                // Match C++ CallExpression::generate_bytecode: allocate callee
                // (current_value) first, this_value (current_base) second,
                // and do NOT emit Mov Undefined for current_base.
                let callee = gen.allocate_register();
                let this_value = gen.allocate_register();
                generate_optional_chain_inner(gen, base, references, &callee, &this_value)?;
                (callee, Some(this_value))
            }
            _ => {
                let callee = generate_expression_or_undefined(&data.callee, gen, None);
                (callee, None)
            }
        }
    } else {
        let callee = generate_expression_or_undefined(&data.callee, gen, None);
        (callee, None)
    };

    // Copy callee/this into fresh registers so argument evaluation
    // cannot mutate them (e.g. `foo.bar(foo = null)`).
    let this_value = this_value.map(|tv| gen.copy_if_needed_to_preserve_evaluation_order(&tv));
    let callee = gen.copy_if_needed_to_preserve_evaluation_order(&callee);

    // Unwrap this_value at function scope so its register lifetime outlives argument temporaries.
    let this_value = this_value.unwrap_or_else(|| gen.add_constant_undefined());

    let has_spread = data.arguments.iter().any(|a| a.is_spread);

    if has_spread {
        // Build an arguments array using NewArray + ArrayAppend for spread elements.
        let arguments_array = gen.allocate_register();
        let first_spread = data.arguments.iter().position(|a| a.is_spread).unwrap_or(0);

        let mut pre_holders = Vec::with_capacity(first_spread);
        for argument in &data.arguments[..first_spread] {
            let reg = gen.allocate_register();
            let val = generate_expression_or_undefined(&argument.value, gen, None);
            gen.emit_mov(&reg, &val);
            pre_holders.push(reg);
        }
        let pre_arguments: Vec<Operand> = pre_holders.iter().map(|a| a.operand()).collect();
        gen.emit(Instruction::NewArray {
            dst: arguments_array.operand(),
            element_count: u32_from_usize(pre_arguments.len()),
            elements: pre_arguments,
        });

        for argument in &data.arguments[first_spread..] {
            let val = generate_expression_or_undefined(&argument.value, gen, None);
            gen.emit(Instruction::ArrayAppend {
                dst: arguments_array.operand(),
                src: val.operand(),
                is_spread: argument.is_spread,
            });
        }
        // FIXME: Remove this manual drop() when we no longer need to match C++ register allocation.
        drop(pre_holders);

        if is_new {
            gen.emit(Instruction::CallConstructWithArgumentArray {
                dst: dst.operand(),
                callee: callee.operand(),
                this_value: this_value.operand(),
                arguments: arguments_array.operand(),
                expression_string,
            });
        } else if is_direct_eval {
            gen.emit(Instruction::CallDirectEvalWithArgumentArray {
                dst: dst.operand(),
                callee: callee.operand(),
                this_value: this_value.operand(),
                arguments: arguments_array.operand(),
                expression_string,
            });
        } else {
            gen.emit(Instruction::CallWithArgumentArray {
                dst: dst.operand(),
                callee: callee.operand(),
                this_value: this_value.operand(),
                arguments: arguments_array.operand(),
                expression_string,
            });
        }
    } else {
        // Copy local variables into fresh registers so that evaluating
        // later arguments cannot mutate earlier argument values (e.g.
        // `bar(i, i++)` — the first argument must be the pre-increment value).
        let mut argument_holders = Vec::with_capacity(data.arguments.len());
        for argument in &data.arguments {
            let val = generate_expression_or_undefined(&argument.value, gen, None);
            argument_holders.push(gen.copy_if_needed_to_preserve_evaluation_order(&val));
        }
        let arguments: Vec<Operand> = argument_holders.iter().map(|a| a.operand()).collect();

        if is_new {
            gen.emit(Instruction::CallConstruct {
                dst: dst.operand(),
                callee: callee.operand(),
                argument_count: u32_from_usize(arguments.len()),
                expression_string,
                arguments,
            });
        } else if is_direct_eval {
            gen.emit(Instruction::CallDirectEval {
                dst: dst.operand(),
                callee: callee.operand(),
                this_value: this_value.operand(),
                argument_count: u32_from_usize(arguments.len()),
                expression_string,
                arguments,
            });
        } else if let Some(b) = builtin {
            if builtin_argument_count(b) == arguments.len() {
                gen.emit(Instruction::CallBuiltin {
                    dst: dst.operand(),
                    callee: callee.operand(),
                    this_value: this_value.operand(),
                    argument_count: u32_from_usize(arguments.len()),
                    builtin: b,
                    expression_string,
                    arguments,
                });
            } else {
                gen.emit(Instruction::Call {
                    dst: dst.operand(),
                    callee: callee.operand(),
                    this_value: this_value.operand(),
                    argument_count: u32_from_usize(arguments.len()),
                    expression_string,
                    arguments,
                });
            }
        } else {
            gen.emit(Instruction::Call {
                dst: dst.operand(),
                callee: callee.operand(),
                this_value: this_value.operand(),
                argument_count: u32_from_usize(arguments.len()),
                expression_string,
                arguments,
            });
        }
    }

    Some(dst)
}

// =============================================================================
// Update expression (++/--)
// =============================================================================

/// Emit the increment/decrement operation for an update expression.
/// Returns the result operand: `value` for prefix, a new `dst` for postfix.
fn emit_update_op(
    gen: &mut Generator,
    op: UpdateOp,
    prefixed: bool,
    value: &ScopedOperand,
    preferred_dst: Option<&ScopedOperand>,
) -> ScopedOperand {
    if prefixed {
        match op {
            UpdateOp::Increment => gen.emit(Instruction::Increment { dst: value.operand() }),
            UpdateOp::Decrement => gen.emit(Instruction::Decrement { dst: value.operand() }),
        }
        value.clone()
    } else {
        let dst = choose_dst(gen, preferred_dst);
        match op {
            UpdateOp::Increment => gen.emit(Instruction::PostfixIncrement {
                dst: dst.operand(),
                src: value.operand(),
            }),
            UpdateOp::Decrement => gen.emit(Instruction::PostfixDecrement {
                dst: dst.operand(),
                src: value.operand(),
            }),
        }
        dst
    }
}

fn generate_update_expression(
    gen: &mut Generator,
    op: UpdateOp,
    argument: &Expression,
    prefixed: bool,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // Load the value, keeping track of the base for member expressions
    // so we can store back without re-evaluating.
    match &argument.inner {
        ExpressionKind::Identifier(ident) => {
            let value = generate_identifier(ident, gen, None)?;
            let result = emit_update_op(gen, op, prefixed, &value, preferred_dst);
            emit_set_variable(gen, ident, &value);
            Some(result)
        }
        ExpressionKind::Member { object, property, computed } => {
            let is_super = matches!(object.inner, ExpressionKind::Super);

            if is_super {
                // Per spec, evaluation order for super property access is:
                // 1. ResolveThisBinding
                // 2. Evaluate computed property (if any)
                // 3. ResolveSuperBase
                // 4. Property lookup with this
                let this_value = emit_resolve_this_binding(gen);
                let computed_key = if *computed {
                    Some(generate_expression_or_undefined(property, gen, None))
                } else {
                    None
                };
                let base = gen.allocate_register();
                gen.emit(Instruction::ResolveSuperBase { dst: base.operand() });
                let value = gen.allocate_register();
                if let Some(ref key) = computed_key {
                    emit_get_by_value_with_this(gen, &value, &base, key, &this_value);
                } else if let ExpressionKind::Identifier(ident) = &property.inner {
                    let key = gen.intern_property_key(&ident.name);
                    let cache = gen.next_property_lookup_cache();
                    gen.emit(Instruction::GetByIdWithThis {
                        dst: value.operand(),
                        base: base.operand(),
                        property: key,
                        this_value: this_value.operand(),
                        cache_index: cache,
                    });
                }
                let result = emit_update_op(gen, op, prefixed, &value, preferred_dst);
                emit_super_put(gen, &base, property, *computed, &this_value, &value, computed_key.as_ref());
                Some(result)
            } else {
                // Non-super member update expression.
                let base = generate_expression(object, gen, None)?;
                let base_id = intern_base_identifier(gen, object);
                if *computed {
                let property = generate_expression(property, gen, None)?;
                let value = gen.allocate_register();
                emit_get_by_value(gen, &value, &base, &property, base_id);
                // Save property for store-back (matching C++ emit_load_from_reference).
                let saved_property = gen.allocate_register();
                gen.emit_mov(&saved_property, &property);
                // FIXME: Remove these manual drop() calls when we no longer need to match C++ register allocation.
                drop(property);
                let result = emit_update_op(gen, op, prefixed, &value, preferred_dst);
                emit_put_normal_by_value(gen, &base, &saved_property, &value, None);
                // FIXME: Remove this manual drop() when we no longer need to match C++ register allocation.
                if !prefixed { drop(value); }
                Some(result)
                } else if let ExpressionKind::Identifier(property_ident) = &property.inner {
                    let value = gen.allocate_register();
                    emit_get_by_id(gen, &value, &base, &property_ident.name, base_id);
                    let key = gen.intern_property_key(&property_ident.name);
                    let result = emit_update_op(gen, op, prefixed, &value, preferred_dst);
                    let cache2 = gen.next_property_lookup_cache();
                    gen.emit(Instruction::PutNormalById {
                        base: base.operand(),
                        property: key,
                        src: value.operand(),
                        cache_index: cache2,
                        base_identifier: None,
                    });
                    Some(result)
                } else {
                    // Fallback: just evaluate, no store-back
                    let value = gen.allocate_register();
                    Some(value)
                }
            }
        }
        _ => {
            // Invalid update target (e.g. foo()++). Per spec, evaluate the
            // expression first, then throw ReferenceError.
            generate_expression(argument, gen, None);
            emit_invalid_lhs_error(gen);
            Some(gen.add_constant_undefined())
        }
    }
}

// =============================================================================
// Assignment expression
// =============================================================================

/// Generate bytecode for all forms of assignment: simple (`=`), compound
/// (`+=`, `-=`, etc.), and logical (`&&=`, `||=`, `??=`).
///
/// Handles identifiers (local, global, environment), member expressions
/// (by-id, by-value, super, private), and destructuring patterns.
fn generate_assignment_expression(
    gen: &mut Generator,
    op: AssignmentOp,
    lhs: &AssignmentLhs,
    rhs: &Expression,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    match lhs {
        AssignmentLhs::Expression(lhs_expression) => {
            // Simple assignment to identifier
            if let ExpressionKind::Identifier(ident) = &lhs_expression.inner {
                if op == AssignmentOp::Assignment {
                    gen.pending_lhs_name = Some(gen.intern_identifier(&ident.name));
                    let rhs_val = generate_expression(rhs, gen, None)?;
                    gen.pending_lhs_name = None;
                    emit_set_variable(gen, ident, &rhs_val);
                    return Some(rhs_val);
                }

                // Load LHS value first (needed for both compound and logical assignments).
                let lhs_val = generate_identifier(ident, gen, None)?;

                let is_logical = matches!(op, AssignmentOp::AndAssignment | AssignmentOp::OrAssignment | AssignmentOp::NullishAssignment);
                if is_logical {
                    // Logical assignments short-circuit: evaluate RHS only if condition met.
                    let rhs_block = gen.make_block();
                    let lhs_block = gen.make_block();
                    let end_block = gen.make_block();
                    match op {
                        AssignmentOp::AndAssignment => {
                            gen.emit_jump_if(&lhs_val, rhs_block, lhs_block);
                        }
                        AssignmentOp::OrAssignment => {
                            gen.emit_jump_if(&lhs_val, lhs_block, rhs_block);
                        }
                        AssignmentOp::NullishAssignment => {
                            gen.emit(Instruction::JumpNullish {
                                condition: lhs_val.operand(),
                                true_target: rhs_block,
                                false_target: lhs_block,
                            });
                        }
                        _ => unreachable!("only logical assignment ops reach this branch"),
                    }
                    // RHS block: evaluate RHS, assign, jump to end.
                    gen.switch_to_basic_block(rhs_block);
                    gen.pending_lhs_name = Some(gen.intern_identifier(&ident.name));
                    let rhs_val = generate_expression(rhs, gen, None)?;
                    gen.pending_lhs_name = None;
                    // Allocate dst after RHS evaluation to match C++ register order.
                    let dst = if lhs_val.operand().is_local() {
                        lhs_val.clone()
                    } else {
                        choose_dst(gen, preferred_dst)
                    };
                    gen.emit_mov(&dst, &rhs_val);
                    emit_set_variable(gen, ident, &dst);
                    gen.emit(Instruction::Jump { target: end_block });
                    // LHS block: keep original value.
                    gen.switch_to_basic_block(lhs_block);
                    gen.emit_mov(&dst, &lhs_val);
                    gen.emit(Instruction::Jump { target: end_block });
                    gen.switch_to_basic_block(end_block);
                    return Some(dst);
                }

                // Regular compound assignment (+=, -=, etc.)
                let rhs_val = generate_expression(rhs, gen, None)?;
                // OPTIMIZATION: If LHS is a local, write directly into it.
                let dst = if lhs_val.operand().is_local() {
                    lhs_val.clone()
                } else {
                    choose_dst(gen, preferred_dst)
                };
                emit_compound_assignment(gen, op, &dst, &lhs_val, &rhs_val);
                emit_set_variable(gen, ident, &dst);
                return Some(dst);
            }
            // Member expression LHS (e.g., obj.foo = x, obj[key] = x)
            if let ExpressionKind::Member { object, property, computed } = &lhs_expression.inner {
                let is_super = matches!(object.inner, ExpressionKind::Super);

                if is_super {
                    // Per spec, evaluation order for super property reference is:
                    // 1. ResolveThisBinding
                    // 2. Evaluate computed property (if any)
                    // 3. ResolveSuperBase
                    let super_this = emit_resolve_this_binding(gen);

                    if op == AssignmentOp::Assignment {
                        let computed_key = if *computed {
                            Some(generate_expression_or_undefined(property, gen, None))
                        } else {
                            None
                        };
                        let base = gen.allocate_register();
                        gen.emit(Instruction::ResolveSuperBase { dst: base.operand() });
                        let rhs_val = generate_expression(rhs, gen, None)?;
                        emit_super_put(gen, &base, property, *computed, &super_this, &rhs_val, computed_key.as_ref());
                        return Some(rhs_val);
                    }

                    // Compound/logical assignment: evaluate property, resolve
                    // super base, then get old value.
                    let computed_key = if *computed {
                        Some(generate_expression_or_undefined(property, gen, None))
                    } else {
                        None
                    };
                    let base = gen.allocate_register();
                    gen.emit(Instruction::ResolveSuperBase { dst: base.operand() });
                    let old_val = gen.allocate_register();
                    if let Some(ref key) = computed_key {
                        emit_get_by_value_with_this(gen, &old_val, &base, key, &super_this);
                    } else if let ExpressionKind::Identifier(ident) = &property.inner {
                        let key = gen.intern_property_key(&ident.name);
                        let cache = gen.next_property_lookup_cache();
                        gen.emit(Instruction::GetByIdWithThis {
                            dst: old_val.operand(),
                            base: base.operand(),
                            property: key,
                            this_value: super_this.operand(),
                            cache_index: cache,
                        });
                    }
                    let is_logical = matches!(op, AssignmentOp::AndAssignment | AssignmentOp::OrAssignment | AssignmentOp::NullishAssignment);
                    if is_logical {
                        let rhs_block = gen.make_block();
                        let lhs_block = gen.make_block();
                        let end_block = gen.make_block();
                        emit_logical_jump(gen, op, &old_val, rhs_block, lhs_block);
                        gen.switch_to_basic_block(rhs_block);
                        let rhs_val = generate_expression(rhs, gen, None)?;
                        let dst = choose_dst(gen, preferred_dst);
                        gen.emit_mov(&dst, &rhs_val);
                        emit_super_put(gen, &base, property, *computed, &super_this, &dst, computed_key.as_ref());
                        gen.emit(Instruction::Jump { target: end_block });
                        gen.switch_to_basic_block(lhs_block);
                        gen.emit_mov(&dst, &old_val);
                        gen.emit(Instruction::Jump { target: end_block });
                        gen.switch_to_basic_block(end_block);
                        return Some(dst);
                    }
                    let rhs_val = generate_expression(rhs, gen, None)?;
                    let dst = choose_dst(gen, preferred_dst);
                    emit_compound_assignment(gen, op, &dst, &old_val, &rhs_val);
                    emit_super_put(gen, &base, property, *computed, &super_this, &dst, computed_key.as_ref());
                    return Some(dst);
                }

                // Non-super member assignment.
                let base_raw = generate_expression(object, gen, None)?;

                if op == AssignmentOp::Assignment {
                    let base = gen.copy_if_needed_to_preserve_evaluation_order(&base_raw);
                    let precomputed_key = if *computed {
                        let key_val = generate_expression_or_undefined(property, gen, None);
                        Some(gen.copy_if_needed_to_preserve_evaluation_order(&key_val))
                    } else {
                        None
                    };
                    let rhs_val = generate_expression(rhs, gen, None)?;
                    if let Some(key) = precomputed_key {
                        let base_id = intern_base_identifier(gen, object);
                        emit_put_normal_by_value(gen, &base, &key, &rhs_val, base_id);
                    } else {
                        emit_put_to_member(gen, &base, property, false, &rhs_val, Some(object));
                    }
                    return Some(rhs_val);
                }

                // Compound/logical member assignment.
                let base = base_raw;
                let base_id = intern_base_identifier(gen, object);
                let is_logical = matches!(op, AssignmentOp::AndAssignment | AssignmentOp::OrAssignment | AssignmentOp::NullishAssignment);

                if *computed {
                    let property = generate_expression(property, gen, None)?;
                    let old_val = gen.allocate_register();
                    emit_get_by_value(gen, &old_val, &base, &property, base_id);
                    // Save property for store-back (matching C++ emit_load_from_reference).
                    let saved_property = gen.allocate_register();
                    gen.emit_mov(&saved_property, &property);
                    // FIXME: Remove this manual drop() when we no longer need to match C++ register allocation.
                    drop(property);
                    if is_logical {
                        let rhs_block = gen.make_block();
                        let lhs_block = gen.make_block();
                        let end_block = gen.make_block();
                        emit_logical_jump(gen, op, &old_val, rhs_block, lhs_block);
                        gen.switch_to_basic_block(rhs_block);
                        let rhs_val = generate_expression(rhs, gen, None)?;
                        let dst = choose_dst(gen, preferred_dst);
                        gen.emit_mov(&dst, &rhs_val);
                        emit_put_normal_by_value(gen, &base, &saved_property, &dst, None);
                        gen.emit(Instruction::Jump { target: end_block });
                        gen.switch_to_basic_block(lhs_block);
                        gen.emit_mov(&dst, &old_val);
                        gen.emit(Instruction::Jump { target: end_block });
                        gen.switch_to_basic_block(end_block);
                        // FIXME: Remove these manual drop() calls when we no longer need to match C++ register allocation.
                        drop(rhs_val);
                        drop(old_val);
                        drop(saved_property);
                        return Some(dst);
                    }
                    let rhs_val = generate_expression(rhs, gen, None)?;
                    let dst = choose_dst(gen, preferred_dst);
                    emit_compound_assignment(gen, op, &dst, &old_val, &rhs_val);
                    emit_put_normal_by_value(gen, &base, &saved_property, &dst, None);
                    // FIXME: Remove these manual drop() calls when we no longer need to match C++ register allocation.
                    drop(rhs_val);
                    drop(old_val);
                    drop(saved_property);
                    return Some(dst);
                } else if let ExpressionKind::Identifier(ident) = &property.inner {
                    let old_val = gen.allocate_register();
                    emit_get_by_id(gen, &old_val, &base, &ident.name, base_id);
                    if is_logical {
                        let rhs_block = gen.make_block();
                        let lhs_block = gen.make_block();
                        let end_block = gen.make_block();
                        emit_logical_jump(gen, op, &old_val, rhs_block, lhs_block);
                        gen.switch_to_basic_block(rhs_block);
                        let rhs_val = generate_expression(rhs, gen, None)?;
                        let dst = choose_dst(gen, preferred_dst);
                        gen.emit_mov(&dst, &rhs_val);
                        let key = gen.intern_property_key(&ident.name);
                        let cache2 = gen.next_property_lookup_cache();
                        gen.emit(Instruction::PutNormalById {
                            base: base.operand(),
                            property: key,
                            src: dst.operand(),
                            cache_index: cache2,
                            base_identifier: None,
                        });
                        gen.emit(Instruction::Jump { target: end_block });
                        gen.switch_to_basic_block(lhs_block);
                        gen.emit_mov(&dst, &old_val);
                        gen.emit(Instruction::Jump { target: end_block });
                        gen.switch_to_basic_block(end_block);
                        return Some(dst);
                    }
                    let rhs_val = generate_expression(rhs, gen, None)?;
                    let dst = choose_dst(gen, preferred_dst);
                    emit_compound_assignment(gen, op, &dst, &old_val, &rhs_val);
                    let key = gen.intern_property_key(&ident.name);
                    let cache2 = gen.next_property_lookup_cache();
                    gen.emit(Instruction::PutNormalById {
                        base: base.operand(),
                        property: key,
                        src: dst.operand(),
                        cache_index: cache2,
                        base_identifier: None,
                    });
                    return Some(dst);
                } else if let ExpressionKind::PrivateIdentifier(priv_ident) = &property.inner {
                    let old_val = gen.allocate_register();
                    let id = gen.intern_identifier(&priv_ident.name);
                    gen.emit(Instruction::GetPrivateById {
                        dst: old_val.operand(),
                        base: base.operand(),
                        property: id,
                    });
                    if is_logical {
                        let rhs_block = gen.make_block();
                        let lhs_block = gen.make_block();
                        let end_block = gen.make_block();
                        let dst = choose_dst(gen, preferred_dst);
                        emit_logical_jump(gen, op, &old_val, rhs_block, lhs_block);
                        gen.switch_to_basic_block(rhs_block);
                        let rhs_val = generate_expression(rhs, gen, None)?;
                        gen.emit_mov(&dst, &rhs_val);
                        let id2 = gen.intern_identifier(&priv_ident.name);
                        gen.emit(Instruction::PutPrivateById {
                            base: base.operand(),
                            property: id2,
                            src: dst.operand(),
                        });
                        gen.emit(Instruction::Jump { target: end_block });
                        gen.switch_to_basic_block(lhs_block);
                        gen.emit_mov(&dst, &old_val);
                        gen.emit(Instruction::Jump { target: end_block });
                        gen.switch_to_basic_block(end_block);
                        return Some(dst);
                    }
                    let rhs_val = generate_expression(rhs, gen, None)?;
                    let dst = choose_dst(gen, preferred_dst);
                    emit_compound_assignment(gen, op, &dst, &old_val, &rhs_val);
                    let id2 = gen.intern_identifier(&priv_ident.name);
                    gen.emit(Instruction::PutPrivateById {
                        base: base.operand(),
                        property: id2,
                        src: dst.operand(),
                    });
                    return Some(dst);
                }
            }
            // LHS is not an identifier or member expression (e.g. a function call).
            // Per spec 13.15.2 step 1b, evaluate the LHS, then throw ReferenceError
            // before evaluating the RHS.
            generate_expression(lhs_expression, gen, None);
            emit_invalid_lhs_error(gen);
            Some(gen.add_constant_undefined())
        }
        AssignmentLhs::Pattern(pattern) => {
            let rhs_val = generate_expression(rhs, gen, preferred_dst)?;
            generate_binding_pattern_bytecode(gen, pattern, BindingMode::Set, &rhs_val);
            Some(rhs_val)
        }
    }
}

/// Emit ResolveThisBinding (if not already resolved in current block) and return
/// the this value register.
fn emit_resolve_this_binding(gen: &mut Generator) -> ScopedOperand {
    emit_resolve_this_if_needed(gen);
    gen.this_value()
}

/// Emit ResolveThisBinding only if not already resolved in the current or entry block.
fn emit_resolve_this_if_needed(gen: &mut Generator) {
    let index = gen.current_block_index().basic_block_index();
    if gen.basic_blocks[index].resolved_this {
        return;
    }
    if gen.basic_blocks[0].resolved_this {
        gen.basic_blocks[index].resolved_this = true;
        return;
    }
    gen.emit(Instruction::ResolveThisBinding);
    let index = gen.current_block_index().basic_block_index();
    gen.basic_blocks[index].resolved_this = true;
}

/// Emit a super property get (uses WithThis variants).
/// For computed access, evaluates the property expression.
/// Returns the evaluated property operand for computed access (so callers
/// can reuse it for a subsequent put).
fn emit_super_get(
    gen: &mut Generator,
    dst: &ScopedOperand,
    base: &ScopedOperand,
    property: &Expression,
    computed: bool,
    this_value: &ScopedOperand,
) -> Option<ScopedOperand> {
    if computed {
        let property = generate_expression_or_undefined(property, gen, None);
        emit_get_by_value_with_this(gen, dst, base, &property, this_value);
        Some(property)
    } else if let ExpressionKind::Identifier(ident) = &property.inner {
        let key = gen.intern_property_key(&ident.name);
        let cache = gen.next_property_lookup_cache();
        gen.emit(Instruction::GetByIdWithThis {
            dst: dst.operand(),
            base: base.operand(),
            property: key,
            this_value: this_value.operand(),
            cache_index: cache,
        });
        None
    } else {
        None
    }
}

/// Emit a super property put (uses WithThis variants).
/// For computed access, `computed_key` should be the operand returned by
/// `emit_super_get` so the property is not re-evaluated. If `None` for
/// computed access, the property expression will be evaluated.
fn emit_super_put(
    gen: &mut Generator,
    base: &ScopedOperand,
    property: &Expression,
    computed: bool,
    this_value: &ScopedOperand,
    value: &ScopedOperand,
    computed_key: Option<&ScopedOperand>,
) {
    if computed {
        let property = match computed_key {
            Some(k) => k.clone(),
            None => generate_expression_or_undefined(property, gen, None),
        };
        emit_put_normal_by_value_with_this(gen, base, &property, this_value, value);
    } else if let ExpressionKind::Identifier(ident) = &property.inner {
        let key = gen.intern_property_key(&ident.name);
        let cache = gen.next_property_lookup_cache();
        gen.emit(Instruction::PutNormalByIdWithThis {
            base: base.operand(),
            this_value: this_value.operand(),
            property: key,
            src: value.operand(),
            cache_index: cache,
        });
    }
}

/// Emit a property access by name, using GetLength for the "length" property.
fn emit_get_by_id(
    gen: &mut Generator,
    dst: &ScopedOperand,
    base: &ScopedOperand,
    property_name: &[u16],
    base_identifier: Option<IdentifierTableIndex>,
) {
    let key = gen.intern_property_key(property_name);
    if property_name == utf16!("length") {
        gen.length_identifier = Some(key);
        let cache = gen.next_property_lookup_cache();
        gen.emit(Instruction::GetLength {
            dst: dst.operand(),
            base: base.operand(),
            base_identifier,
            cache_index: cache,
        });
    } else {
        let cache = gen.next_property_lookup_cache();
        gen.emit(Instruction::GetById {
            dst: dst.operand(),
            base: base.operand(),
            property: key,
            base_identifier,
            cache_index: cache,
        });
    }
}

/// Emit a "Invalid left-hand side in assignment" ReferenceError followed by Throw,
/// then switch to a dead block for subsequent codegen.
fn emit_invalid_lhs_error(gen: &mut Generator) {
    let exception = gen.allocate_register();
    let error_string = gen.intern_string(utf16!("Invalid left-hand side in assignment"));
    gen.emit(Instruction::NewReferenceError {
        dst: exception.operand(),
        error_string,
    });
    gen.emit(Instruction::Throw { src: exception.operand() });
    let dead_block = gen.make_block();
    gen.switch_to_basic_block(dead_block);
}

/// Check if a UTF-16 string is a canonical array index (non-negative integer < 2^32 - 1).
/// Matches the behavior of C++ to_property_key: these strings become integer PropertyKeys,
/// not string PropertyKeys, so they must NOT be optimized to GetById/PutById.
pub(crate) fn is_array_index(s: &[u16]) -> bool {
    if s.is_empty() || s.len() > 10 {
        return false;
    }
    // Must not have leading zeros (except "0" itself)
    if s.len() > 1 && s[0] == ch(b'0') {
        return false;
    }
    let mut value: u64 = 0;
    for &c in s {
        if c < ch(b'0') || c > ch(b'9') {
            return false;
        }
        value = value * 10 + (c - ch(b'0')) as u64;
    }
    value <= 0xFFFF_FFFE
}

/// Emit a property read by value, optimizing constant string properties to GetById.
fn emit_get_by_value(
    gen: &mut Generator,
    dst: &ScopedOperand,
    base: &ScopedOperand,
    property: &ScopedOperand,
    base_identifier: Option<IdentifierTableIndex>,
) {
    if let Some(key) = gen.try_constant_string_to_property_key(property) {
        if gen.property_key_table[key.0 as usize].0 == utf16!("length") {
            gen.length_identifier = Some(key);
            let cache = gen.next_property_lookup_cache();
            gen.emit(Instruction::GetLength {
                dst: dst.operand(),
                base: base.operand(),
                base_identifier,
                cache_index: cache,
            });
        } else {
            let cache = gen.next_property_lookup_cache();
            gen.emit(Instruction::GetById {
                dst: dst.operand(),
                base: base.operand(),
                property: key,
                base_identifier,
                cache_index: cache,
            });
        }
        return;
    }
    gen.emit(Instruction::GetByValue {
        dst: dst.operand(),
        base: base.operand(),
        property: property.operand(),
        base_identifier,
    });
}

/// Emit a property read by value with explicit this, optimizing constant string properties.
fn emit_get_by_value_with_this(
    gen: &mut Generator,
    dst: &ScopedOperand,
    base: &ScopedOperand,
    property: &ScopedOperand,
    this_value: &ScopedOperand,
) {
    if let Some(key) = gen.try_constant_string_to_property_key(property) {
        let cache = gen.next_property_lookup_cache();
        gen.emit(Instruction::GetByIdWithThis {
            dst: dst.operand(),
            base: base.operand(),
            property: key,
            this_value: this_value.operand(),
            cache_index: cache,
        });
        return;
    }
    gen.emit(Instruction::GetByValueWithThis {
        dst: dst.operand(),
        base: base.operand(),
        property: property.operand(),
        this_value: this_value.operand(),
    });
}

/// Emit a normal property write by value, optimizing constant string properties to PutNormalById.
fn emit_put_normal_by_value(
    gen: &mut Generator,
    base: &ScopedOperand,
    property: &ScopedOperand,
    src: &ScopedOperand,
    base_identifier: Option<IdentifierTableIndex>,
) {
    if let Some(key) = gen.try_constant_string_to_property_key(property) {
        let cache = gen.next_property_lookup_cache();
        gen.emit(Instruction::PutNormalById {
            base: base.operand(),
            property: key,
            src: src.operand(),
            cache_index: cache,
            base_identifier,
        });
        return;
    }
    gen.emit(Instruction::PutNormalByValue {
        base: base.operand(),
        property: property.operand(),
        src: src.operand(),
        base_identifier,
    });
}

/// Emit a normal property write by value with explicit this, optimizing constant string properties.
fn emit_put_normal_by_value_with_this(
    gen: &mut Generator,
    base: &ScopedOperand,
    property: &ScopedOperand,
    this_value: &ScopedOperand,
    src: &ScopedOperand,
) {
    if let Some(key) = gen.try_constant_string_to_property_key(property) {
        let cache = gen.next_property_lookup_cache();
        gen.emit(Instruction::PutNormalByIdWithThis {
            base: base.operand(),
            this_value: this_value.operand(),
            property: key,
            src: src.operand(),
            cache_index: cache,
        });
        return;
    }
    gen.emit(Instruction::PutNormalByValueWithThis {
        base: base.operand(),
        property: property.operand(),
        this_value: this_value.operand(),
        src: src.operand(),
    });
}

enum PutKind {
    Own,
    Getter,
    Setter,
}

/// Emit a property write by value, optimizing constant string properties to the ById variant.
fn emit_put_by_value(
    gen: &mut Generator,
    base: &ScopedOperand,
    property: &ScopedOperand,
    src: &ScopedOperand,
    kind: PutKind,
) {
    if let Some(key) = gen.try_constant_string_to_property_key(property) {
        let cache = gen.next_property_lookup_cache();
        match kind {
            PutKind::Own => {
                gen.emit(Instruction::PutOwnById {
                    base: base.operand(),
                    property: key,
                    src: src.operand(),
                    cache_index: cache,
                    base_identifier: None,
                });
            }
            PutKind::Getter => {
                gen.emit(Instruction::PutGetterById {
                    base: base.operand(),
                    property: key,
                    src: src.operand(),
                    cache_index: cache,
                    base_identifier: None,
                });
            }
            PutKind::Setter => {
                gen.emit(Instruction::PutSetterById {
                    base: base.operand(),
                    property: key,
                    src: src.operand(),
                    cache_index: cache,
                    base_identifier: None,
                });
            }
        }
        return;
    }
    match kind {
        PutKind::Own => {
            gen.emit(Instruction::PutOwnByValue {
                base: base.operand(),
                property: property.operand(),
                src: src.operand(),
                base_identifier: None,
            });
        }
        PutKind::Getter => {
            gen.emit(Instruction::PutGetterByValue {
                base: base.operand(),
                property: property.operand(),
                src: src.operand(),
                base_identifier: None,
            });
        }
        PutKind::Setter => {
            gen.emit(Instruction::PutSetterByValue {
                base: base.operand(),
                property: property.operand(),
                src: src.operand(),
                base_identifier: None,
            });
        }
    }
}

fn emit_set_variable(gen: &mut Generator, ident: &Identifier, value: &ScopedOperand) {
    if ident.is_local() {
        if ident.declaration_kind.get() == Some(DeclarationKind::Const) {
            // Emit TDZ check before const assignment error, matching C++ which
            // calls emit_tdz_check_if_needed() in the caller before emit_set_variable().
            let local_index = ident.local_index.get();
            if gen.is_local_lexically_declared(local_index)
                && !gen.is_local_initialized(local_index)
            {
                let local = gen.resolve_local(local_index, ident.local_type.get().unwrap());
                gen.emit(Instruction::ThrowIfTDZ {
                    src: local.operand(),
                });
            }
            gen.emit(Instruction::ThrowConstAssignment {});
            return;
        }
        let local_index = ident.local_index.get();
        let local = gen.resolve_local(local_index, ident.local_type.get().unwrap());
        // TDZ check: throw ReferenceError if assigning to an uninitialized let/const binding.
        // Matching C++ AssignmentExpression: check is_lexically_declared && !is_initialized.
        if gen.is_local_lexically_declared(local_index)
            && !gen.is_local_initialized(local_index)
        {
            gen.emit(Instruction::ThrowIfTDZ {
                src: local.operand(),
            });
        }
        // Match C++ emit_set_variable: only skip self-move for variable locals,
        // not for arguments.
        let is_variable_self_move = ident.local_type.get() == Some(LocalType::Variable)
            && value.operand().is_local()
            && value.operand().index() == local_index;
        if !is_variable_self_move {
            gen.emit(Instruction::Mov {
                dst: local.operand(),
                src: value.operand(),
            });
        }
    } else if ident.is_global.get() {
        let id = gen.intern_identifier(&ident.name);
        let cache = gen.next_global_variable_cache();
        gen.emit(Instruction::SetGlobal {
            identifier: id,
            src: value.operand(),
            cache_index: cache,
        });
    } else {
        // Non-local, non-global: use SetLexicalBinding which searches
        // the lexical environment chain (important for with-statement support).
        let id = gen.intern_identifier(&ident.name);
        gen.emit(Instruction::SetLexicalBinding {
            identifier: id,
            src: value.operand(),
            cache: EnvironmentCoordinate::empty(),
        });
    }
}

fn emit_put_to_member(
    gen: &mut Generator,
    base: &ScopedOperand,
    property: &Expression,
    computed: bool,
    value: &ScopedOperand,
    base_object: Option<&Expression>,
) {
    let base_id = base_object.and_then(|obj| intern_base_identifier(gen, obj));
    if computed {
        let property = generate_expression_or_undefined(property, gen, None);
        emit_put_normal_by_value(gen, base, &property, value, base_id);
    } else if let ExpressionKind::Identifier(ident) = &property.inner {
        let key = gen.intern_property_key(&ident.name);
        let cache = gen.next_property_lookup_cache();
        gen.emit(Instruction::PutNormalById {
            base: base.operand(),
            property: key,
            src: value.operand(),
            cache_index: cache,
            base_identifier: base_id,
        });
    } else if let ExpressionKind::PrivateIdentifier(priv_ident) = &property.inner {
        let id = gen.intern_identifier(&priv_ident.name);
        gen.emit(Instruction::PutPrivateById {
            base: base.operand(),
            property: id,
            src: value.operand(),
        });
    }
}

/// Emit bytecode for `delete <expression>`.
fn emit_delete_reference(
    gen: &mut Generator,
    operand: &Expression,
) -> ScopedOperand {
    match &operand.inner {
        ExpressionKind::Identifier(ident) => {
            if ident.is_local() {
                return gen.add_constant_boolean(false);
            }
            let dst = gen.allocate_register();
            let id = gen.intern_identifier(&ident.name);
            gen.emit(Instruction::DeleteVariable {
                dst: dst.operand(),
                identifier: id,
            });
            dst
        }
        ExpressionKind::Member { object, property, computed } => {
            // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
            // Deleting a super property is always a ReferenceError.
            if matches!(object.inner, ExpressionKind::Super) {
                let this_value = emit_resolve_this_binding(gen);
                let super_base = gen.allocate_register();
                gen.emit(Instruction::ResolveSuperBase { dst: super_base.operand() });
                // Evaluate computed property for side effects before throwing.
                if *computed {
                    generate_expression_or_undefined(property, gen, None);
                }
                let exception = gen.allocate_register();
                let error_string = gen.intern_string(utf16!("Can't delete a property on 'super'"));
                gen.emit(Instruction::NewReferenceError {
                    dst: exception.operand(),
                    error_string,
                });
                gen.perform_needed_unwinds();
                gen.emit(Instruction::Throw { src: exception.operand() });
                let dead_block = gen.make_block();
                gen.switch_to_basic_block(dead_block);
                let _ = this_value;
                return gen.add_constant_undefined();
            }
            let base = generate_expression_or_undefined(object, gen, None);
            let dst = gen.allocate_register();
            if *computed {
                let key = generate_expression_or_undefined(property, gen, None);
                gen.emit(Instruction::DeleteByValue {
                    dst: dst.operand(),
                    base: base.operand(),
                    property: key.operand(),
                });
            } else if let ExpressionKind::Identifier(property_ident) = &property.inner {
                let key = gen.intern_property_key(&property_ident.name);
                gen.emit(Instruction::DeleteById {
                    dst: dst.operand(),
                    base: base.operand(),
                    property: key,
                });
            } else {
                return gen.add_constant_boolean(true);
            }
            dst
        }
        _ => {
            // delete on non-reference: evaluate for side effects, return true
            generate_expression(operand, gen, None);
            gen.add_constant_boolean(true)
        }
    }
}


/// Pre-evaluated reference operands for deferred store.
/// Used when the spec requires evaluating the assignment target reference
/// before performing some other operation (like iterating a spread element).
enum EvaluatedReference {
    Member {
        base: ScopedOperand,
        property: ScopedOperand,
        base_identifier: Option<IdentifierTableIndex>,
    },
    MemberId {
        base: ScopedOperand,
        property: PropertyKeyTableIndex,
        cache: u32,
        base_identifier: Option<IdentifierTableIndex>,
    },
    PrivateMember {
        base: ScopedOperand,
        property: IdentifierTableIndex,
    },
    SuperMember {
        base: ScopedOperand,
        property: ScopedOperand,
        this_value: ScopedOperand,
    },
    SuperMemberId {
        base: ScopedOperand,
        property: PropertyKeyTableIndex,
        cache: u32,
        this_value: ScopedOperand,
    },
}

/// Evaluate a member expression target to get pre-computed reference operands
/// without performing a load. This implements the "Let lref be ? Evaluation of
/// DestructuringAssignmentTarget" step from the spec.
fn emit_evaluate_member_reference(gen: &mut Generator, target: &Expression) -> EvaluatedReference {
    if let ExpressionKind::Member { object, property, computed } = &target.inner {
        let is_super = matches!(object.inner, ExpressionKind::Super);
        let base = generate_expression_or_undefined(object, gen, None);

        if is_super {
            let this_value = emit_resolve_this_binding(gen);
            if *computed {
                let property = generate_expression_or_undefined(property, gen, None);
                let saved_property = gen.allocate_register();
                gen.emit_mov(&saved_property, &property);
                EvaluatedReference::SuperMember { base, property: saved_property, this_value }
            } else if let ExpressionKind::Identifier(ident) = &property.inner {
                let key = gen.intern_property_key(&ident.name);
                let cache = gen.next_property_lookup_cache();
                EvaluatedReference::SuperMemberId { base, property: key, cache, this_value }
            } else {
                unreachable!("non-computed super member property must be an identifier")
            }
        } else if *computed {
            let property = generate_expression_or_undefined(property, gen, None);
            let saved_property = gen.allocate_register();
            gen.emit_mov(&saved_property, &property);
            EvaluatedReference::Member { base, property: saved_property, base_identifier: None }
        } else if let ExpressionKind::Identifier(ident) = &property.inner {
            let key = gen.intern_property_key(&ident.name);
            let cache = gen.next_property_lookup_cache();
            EvaluatedReference::MemberId { base, property: key, cache, base_identifier: None }
        } else if let ExpressionKind::PrivateIdentifier(priv_ident) = &property.inner {
            let id = gen.intern_identifier(&priv_ident.name);
            EvaluatedReference::PrivateMember { base, property: id }
        } else {
            unreachable!("non-computed member property must be an identifier or private identifier")
        }
    } else {
        unreachable!("emit_evaluate_member_reference called on non-member expression")
    }
}

/// Store a value to a pre-evaluated reference.
fn emit_store_to_evaluated_reference(gen: &mut Generator, reference: &EvaluatedReference, value: &ScopedOperand) {
    match reference {
        EvaluatedReference::Member { base, property, base_identifier } => {
            emit_put_normal_by_value(gen, base, property, value, *base_identifier);
        }
        EvaluatedReference::MemberId { base, property, cache, base_identifier } => {
            gen.emit(Instruction::PutNormalById {
                base: base.operand(),
                property: *property,
                src: value.operand(),
                cache_index: *cache,
                base_identifier: *base_identifier,
            });
        }
        EvaluatedReference::PrivateMember { base, property } => {
            gen.emit(Instruction::PutPrivateById {
                base: base.operand(),
                property: *property,
                src: value.operand(),
            });
        }
        EvaluatedReference::SuperMember { base, property, this_value } => {
            emit_put_normal_by_value_with_this(gen, base, property, this_value, value);
        }
        EvaluatedReference::SuperMemberId { base, property, cache, this_value } => {
            gen.emit(Instruction::PutNormalByIdWithThis {
                base: base.operand(),
                this_value: this_value.operand(),
                property: *property,
                src: value.operand(),
                cache_index: *cache,
            });
        }
    }
}

fn emit_store_to_reference(
    gen: &mut Generator,
    target: &Expression,
    value: &ScopedOperand,
) {
    match &target.inner {
        ExpressionKind::Identifier(ident) => {
            emit_set_variable(gen, ident, value);
        }
        ExpressionKind::Member { object, property, computed } => {
            let is_super = matches!(object.inner, ExpressionKind::Super);
            let base = generate_expression_or_undefined(object, gen, None);
            if is_super {
                let this_value = emit_resolve_this_binding(gen);
                emit_super_put(gen, &base, property, *computed, &this_value, value, None);
            } else {
                emit_put_to_member(gen, &base, property, *computed, value, None);
            }
        }
        _ => {
            // Evaluate the expression for side effects, then throw ReferenceError.
            generate_expression(target, gen, None);
            emit_invalid_lhs_error(gen);
        }
    }
}

/// Emit the conditional jump for a logical assignment (&&=, ||=, ??=).
fn emit_logical_jump(gen: &mut Generator, op: AssignmentOp, condition: &ScopedOperand, rhs_block: Label, lhs_block: Label) {
    match op {
        AssignmentOp::AndAssignment => {
            gen.emit_jump_if(condition, rhs_block, lhs_block);
        }
        AssignmentOp::OrAssignment => {
            gen.emit_jump_if(condition, lhs_block, rhs_block);
        }
        AssignmentOp::NullishAssignment => {
            gen.emit(Instruction::JumpNullish {
                condition: condition.operand(),
                true_target: rhs_block,
                false_target: lhs_block,
            });
        }
        _ => unreachable!("only logical assignment ops are passed to emit_logical_jump"),
    }
}

fn emit_compound_assignment(
    gen: &mut Generator,
    op: AssignmentOp,
    dst: &ScopedOperand,
    lhs: &ScopedOperand,
    rhs: &ScopedOperand,
) {
    let dst_op = dst.operand();
    let lhs_op = lhs.operand();
    let rhs_op = rhs.operand();
    match op {
        AssignmentOp::AdditionAssignment => gen.emit(Instruction::Add { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::SubtractionAssignment => gen.emit(Instruction::Sub { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::MultiplicationAssignment => gen.emit(Instruction::Mul { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::DivisionAssignment => gen.emit(Instruction::Div { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::ModuloAssignment => gen.emit(Instruction::Mod { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::ExponentiationAssignment => gen.emit(Instruction::Exp { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::BitwiseAndAssignment => gen.emit(Instruction::BitwiseAnd { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::BitwiseOrAssignment => gen.emit(Instruction::BitwiseOr { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::BitwiseXorAssignment => gen.emit(Instruction::BitwiseXor { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::LeftShiftAssignment => gen.emit(Instruction::LeftShift { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::RightShiftAssignment => gen.emit(Instruction::RightShift { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::UnsignedRightShiftAssignment => gen.emit(Instruction::UnsignedRightShift { dst: dst_op, lhs: lhs_op, rhs: rhs_op }),
        AssignmentOp::AndAssignment | AssignmentOp::OrAssignment | AssignmentOp::NullishAssignment => {
            unreachable!("logical assignment in compound path")
        }
        AssignmentOp::Assignment => unreachable!("plain assignment in compound path"),
    }
}

// =============================================================================
// Template literal
// =============================================================================

fn generate_template_literal(
    gen: &mut Generator,
    data: &TemplateLiteralData,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // The parser stores ALL parts (string segments AND interpolated expressions)
    // in data.expressions. raw_strings is only populated for tagged templates.

    // OPTIMIZATION: Filter out empty string segments.
    let segments: Vec<&Expression> = data.expressions.iter().filter(|e| {
        !matches!(&e.inner, ExpressionKind::StringLiteral(s) if s.is_empty())
    }).collect();

    if segments.is_empty() {
        return Some(gen.add_constant_string(Utf16String::new()));
    }

    // Allocate dst before generating expressions to match C++ register order.
    let dst = choose_dst(gen, preferred_dst);

    if segments.len() == 1 {
        let val = generate_expression(segments[0], gen, None)?;
        // If it's a constant, return directly.
        if val.operand().is_constant() {
            return Some(val);
        }
        // Otherwise, emit ToString.
        gen.emit(Instruction::ToString {
            dst: dst.operand(),
            value: val.operand(),
        });
        return Some(dst);
    }

    for (index, expression) in segments.iter().enumerate() {
        let val = generate_expression_or_undefined(expression, gen, None);
        if index == 0 {
            if matches!(&expression.inner, ExpressionKind::StringLiteral(_)) {
                gen.emit_mov(&dst, &val);
            } else {
                gen.emit(Instruction::ToString {
                    dst: dst.operand(),
                    value: val.operand(),
                });
            }
        } else {
            gen.emit(Instruction::ConcatString {
                dst: dst.operand(),
                src: val.operand(),
            });
        }
    }

    Some(dst)
}

// =============================================================================
// Tagged template literal
// =============================================================================

fn generate_tagged_template_literal(
    gen: &mut Generator,
    tag: &Expression,
    template_literal: &Expression,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // Resolve tag and this_value based on the tag expression type.
    let (tag_reg, this_value) = match &tag.inner {
        ExpressionKind::Member { object, property, computed } if matches!(object.inner, ExpressionKind::Super) => {
            // super.func`` or super["func"]``
            let this_value = emit_resolve_this_binding(gen);
            let super_base = gen.allocate_register();
            gen.emit(Instruction::ResolveSuperBase { dst: super_base.operand() });
            let method = gen.allocate_register();
            emit_super_get(gen, &method, &super_base, property, *computed, &this_value);
            (method, Some(this_value))
        }
        ExpressionKind::Member { object, property, computed } => {
            let obj = generate_expression_or_undefined(object, gen, None);
            let method = gen.allocate_register();
            if *computed {
                let property = generate_expression_or_undefined(property, gen, None);
                emit_get_by_value(gen, &method, &obj, &property, None);
            } else if let ExpressionKind::Identifier(ident) = &property.inner {
                let base_id = intern_base_identifier(gen, object);
                emit_get_by_id(gen, &method, &obj, &ident.name, base_id);
            } else if let ExpressionKind::PrivateIdentifier(priv_ident) = &property.inner {
                let id = gen.intern_identifier(&priv_ident.name);
                gen.emit(Instruction::GetPrivateById {
                    dst: method.operand(),
                    base: obj.operand(),
                    property: id,
                });
            }
            (method, Some(obj))
        }
        ExpressionKind::Identifier(ident) if ident.is_local() || ident.is_global.get() => {
            let tag_val = generate_expression_or_undefined(tag, gen, None);
            (tag_val, None)
        }
        ExpressionKind::Identifier(ident) => {
            // Non-local, non-global identifier: use GetCalleeAndThisFromEnvironment
            // to properly handle with-statement bindings.
            let callee_reg = gen.allocate_register();
            let this_reg = gen.allocate_register();
            let id = gen.intern_identifier(&ident.name);
            gen.emit(Instruction::GetCalleeAndThisFromEnvironment {
                callee: callee_reg.operand(),
                this_value: this_reg.operand(),
                identifier: id,
                cache: EnvironmentCoordinate::empty(),
            });
            (callee_reg, Some(this_reg))
        }
        _ => {
            let tag_val = generate_expression_or_undefined(tag, gen, None);
            (tag_val, None)
        }
    };

    // Build template strings for GetTemplateObject.
    // expressions has alternating: string_0, expression_0, string_1, expression_1, ..., string_n
    let data = match &template_literal.inner {
        ExpressionKind::TemplateLiteral(d) => d,
        _ => unreachable!("TaggedTemplateLiteral template must be TemplateLiteral"),
    };

    // Collect cooked strings (even indices). NullLiteral means invalid escape → undefined.
    let mut string_regs = Vec::new();
    for i in (0..data.expressions.len()).step_by(2) {
        if matches!(&data.expressions[i].inner, ExpressionKind::NullLiteral) {
            string_regs.push(gen.add_constant_undefined());
        } else {
            let val = generate_expression_or_undefined(&data.expressions[i], gen, None);
            string_regs.push(val);
        }
    }

    // Append raw strings.
    for raw in &data.raw_strings {
        let val = gen.add_constant_string(raw.clone());
        string_regs.push(val);
    }

    // Emit GetTemplateObject.
    let strings_array = gen.allocate_register();
    let string_ops: Vec<Operand> = string_regs.iter().map(|s| s.operand()).collect();
    let cache_index = gen.next_template_object_cache();
    gen.emit(Instruction::GetTemplateObject {
        dst: strings_array.operand(),
        strings_count: u32_from_usize(string_ops.len()),
        cache_index,
        strings: string_ops,
    });

    // Build arguments: [template_object, ...interpolated_expressions]
    let mut argument_regs = vec![strings_array];
    for i in (1..data.expressions.len()).step_by(2) {
        let val = generate_expression_or_undefined(&data.expressions[i], gen, None);
        argument_regs.push(val);
    }

    let dst = choose_dst(gen, preferred_dst);
    let this_op = this_value.unwrap_or_else(|| gen.add_constant_undefined());
    let arguments: Vec<Operand> = argument_regs.iter().map(|a| a.operand()).collect();
    gen.emit(Instruction::Call {
        dst: dst.operand(),
        callee: tag_reg.operand(),
        this_value: this_op.operand(),
        argument_count: u32_from_usize(arguments.len()),
        expression_string: None,
        arguments,
    });

    // FIXME: Remove these manual drop() calls when we no longer need to match C++ register allocation.
    drop(argument_regs);
    drop(this_op);

    Some(dst)
}

// =============================================================================
// Switch statement
// =============================================================================

fn generate_switch_statement(
    gen: &mut Generator,
    data: &SwitchStatementData,
    _preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let completion = gen.allocate_completion_register();

    let discriminant = generate_expression(&data.discriminant, gen, None)?;

    // Block declaration instantiation: create lexical environment for
    // function declarations and let/const across all switch cases.
    let did_create_env = emit_switch_block_declaration_instantiation(gen, data);

    // Create first test block and jump to it (matching C++ structure).
    let first_test_block = gen.make_block();
    gen.emit(Instruction::Jump {
        target: first_test_block,
    });

    // Pre-allocate test blocks for each case with a test expression.
    let mut test_blocks: Vec<Label> = Vec::with_capacity(data.cases.len());
    for case in &data.cases {
        if case.test.is_some() {
            test_blocks.push(gen.make_block());
        }
    }

    // Emit comparison chain: for each case, create the case body block,
    // switch to the test block, evaluate the test, and emit comparison.
    // This matches C++ block creation order: test blocks interleaved with
    // case body blocks.
    let mut next_test_block = first_test_block;
    let mut case_blocks: Vec<Label> = Vec::with_capacity(data.cases.len());
    let mut default_block = None;
    let mut test_block_index = 0;

    for case in &data.cases {
        let case_block = gen.make_block();
        if let Some(test) = &case.test {
            gen.switch_to_basic_block(next_test_block);
            let test_val = generate_expression(test, gen, None)?;
            let cmp = gen.allocate_register();
            // NB: test_value is LHS, discriminant is RHS (matching C++).
            gen.emit(Instruction::StrictlyEquals {
                dst: cmp.operand(),
                lhs: test_val.operand(),
                rhs: discriminant.operand(),
            });
            next_test_block = test_blocks[test_block_index];
            test_block_index += 1;
            gen.emit_jump_if(&cmp, case_block, next_test_block);
        } else {
            default_block = Some(case_block);
        }
        case_blocks.push(case_block);
    }

    // Switch to the last test block and create end block.
    gen.switch_to_basic_block(next_test_block);
    let end_block = gen.make_block();
    let labels = std::mem::take(&mut gen.pending_labels);

    // Jump to default case or end block.
    let fallthrough_target = default_block.unwrap_or(end_block);
    gen.emit(Instruction::Jump {
        target: fallthrough_target,
    });

    gen.begin_breakable_scope(end_block, labels, completion.clone());

    // Emit case bodies (fall-through by default).
    for (i, case) in data.cases.iter().enumerate() {
        gen.switch_to_basic_block(case_blocks[i]);

        let saved_completion = gen.current_completion_register.clone();
        if let Some(ref c) = completion {
            gen.current_completion_register = Some(c.clone());
        }

        let case_scope = case.scope.borrow();
        for child in &case_scope.children {
            // For function declarations in switch cases: emit AnnexB hoisting
            // only if the scope collector approved it (name is in annexb_function_names).
            if did_create_env {
                if let StatementKind::FunctionDeclaration { name: Some(ref name_ident), .. } = child.inner {
                    if gen.annexb_function_names.contains(&name_ident.name) {
                        let id = gen.intern_identifier(&name_ident.name);
                        let value = gen.allocate_register();
                        gen.emit(Instruction::GetBinding {
                            dst: value.operand(),
                            identifier: id,
                            cache: EnvironmentCoordinate::empty(),
                        });
                        gen.emit(Instruction::SetVariableBinding {
                            identifier: id,
                            src: value.operand(),
                            cache: EnvironmentCoordinate::empty(),
                        });
                    }
                }
            }
            let result = generate_statement(child, gen, None);
            if gen.is_current_block_terminated() {
                break;
            }
            if gen.must_propagate_completion {
                if let (Some(ref c), Some(ref val)) = (&completion, &result) {
                    gen.emit_mov(c, val);
                }
            }
        }

        gen.current_completion_register = saved_completion;

        // Fall through to next case
        if !gen.is_current_block_terminated() && i + 1 < case_blocks.len() {
            gen.emit(Instruction::Jump {
                target: case_blocks[i + 1],
            });
        } else if !gen.is_current_block_terminated() {
            gen.emit(Instruction::Jump {
                target: end_block,
            });
        }
    }

    gen.end_breakable_scope();
    gen.switch_to_basic_block(end_block);

    if did_create_env {
        gen.lexical_environment_register_stack.pop();
        if !gen.is_current_block_terminated() {
            let parent = gen.current_lexical_environment();
            gen.emit(Instruction::SetLexicalEnvironment { environment: parent.operand() });
        }
    }
    completion
}

/// Create block declaration instantiation for switch statements.
/// Function declarations and let/const declarations across all cases
/// share a single lexical environment.
fn emit_switch_block_declaration_instantiation(
    gen: &mut Generator,
    data: &SwitchStatementData,
) -> bool {
    // Collect all statements across all cases.
    let case_scopes: Vec<_> = data.cases.iter().map(|c| c.scope.borrow()).collect();
    let all_children: Vec<&Statement> = case_scopes.iter()
        .flat_map(|scope| scope.children.iter())
        .collect();

    // Check if we need a lexical environment.
    // Only needed if there are non-local lexical declarations.
    let needs_env = all_children.iter().any(|child| match &child.inner {
        StatementKind::FunctionDeclaration { .. } => true,
        StatementKind::VariableDeclaration { kind, declarations } => {
            if *kind == DeclarationKind::Let || *kind == DeclarationKind::Const {
                declarations.iter().any(|declaration| {
                    let mut names = Vec::new();
                    collect_target_names(&declaration.target, &mut names);
                    !names.is_empty()
                })
            } else {
                false
            }
        }
        StatementKind::ClassDeclaration(class_data) => {
            class_data.name.as_ref().is_some_and(|n| !n.is_local())
        }
        _ => false,
    });

    if !needs_env {
        return false;
    }

    let new_env = gen.push_new_lexical_environment(0);

    emit_lexical_declarations_for_block(gen, &new_env, all_children.iter().copied());

    true
}

// =============================================================================
// Object expression
// =============================================================================

/// Generate bytecode for an object literal expression.
///
/// Objects whose shape can be determined at compile time (only simple
/// key-value properties with identifier or non-numeric string keys)
/// get shape caching for faster allocation.
fn generate_object_expression(
    gen: &mut Generator,
    properties: &[ObjectProperty],
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let dst = choose_dst(gen, preferred_dst);

    // Determine if this is a simple object literal (all KeyValue with non-computed
    // string/identifier keys that are not numeric indices). Simple literals can
    // benefit from shape caching. Numeric string keys like "0" are stored in
    // indexed storage rather than shape-based storage, so they can't use the fast path.
    //
    // NB: The C++ parser treats {["x"]: 1} identically to {"x": 1} at the AST level
    // (both produce a StringLiteral key with is_computed=false). The parser keeps
    // is_computed=true for bracket-enclosed keys. We normalize here by treating
    // StringLiteral keys as non-computed regardless of is_computed.
    let is_simple = !properties.is_empty() && properties.iter().all(|p| {
        if p.property_type != ObjectPropertyType::KeyValue {
            return false;
        }
        match &p.key.inner {
            ExpressionKind::Identifier(_) if !p.is_computed => true,
            ExpressionKind::StringLiteral(s) => !is_numeric_index_key(s),
            _ => false,
        }
    });

    let cache_index = if is_simple {
        gen.next_object_shape_cache()
    } else {
        u32::MAX
    };
    gen.emit(Instruction::NewObject {
        dst: dst.operand(),
        cache_index,
    });

    if properties.is_empty() {
        return Some(dst);
    }

    for (slot, property) in properties.iter().enumerate() {
        if property.property_type == ObjectPropertyType::Spread {
            // For spread, the source expression is in `key`, not `value`.
            let src = generate_expression_or_undefined(&property.key, gen, None);
            gen.emit(Instruction::PutBySpread {
                base: dst.operand(),
                src: src.operand(),
            });
            continue;
        }

        // For non-string keys (computed, numeric, etc.), evaluate key before value
        // (spec evaluation order). C++ treats all non-StringLiteral keys the same:
        // generate key → ToPrimitiveWithStringHint → generate value → PutByValue.
        //
        // NB: StringLiteral keys are always treated as non-computed (see is_simple comment).
        let is_string_literal_key = matches!(&property.key.inner, ExpressionKind::StringLiteral(_));
        let is_string_key = is_string_literal_key || matches!(&property.key.inner, ExpressionKind::Identifier(_));
        let effectively_computed = property.is_computed && !is_string_literal_key;
        let computed_key = if effectively_computed || !is_string_key {
            let key = generate_expression_or_undefined(&property.key, gen, None);
            gen.emit(Instruction::ToPrimitiveWithStringHint {
                dst: key.operand(),
                value: key.operand(),
            });
            Some(key)
        } else {
            None
        };

        // Set pending LHS name for function name inference on non-computed properties.
        // ProtoSetter (__proto__) skips NamedEvaluation per spec.
        if !effectively_computed && property.property_type != ObjectPropertyType::ProtoSetter {
            let base_name: Option<Utf16String> = match &property.key.inner {
                ExpressionKind::StringLiteral(s) => Some(s.clone()),
                ExpressionKind::Identifier(ident) => Some(ident.name.clone()),
                _ => None,
            };
            if let Some(name) = base_name {
                let full_name: Utf16String = match property.property_type {
                    ObjectPropertyType::Getter => {
                        let mut prefixed = Utf16String(utf16!("get ").to_vec());
                        prefixed.0.extend_from_slice(&name);
                        prefixed
                    }
                    ObjectPropertyType::Setter => {
                        let mut prefixed = Utf16String(utf16!("set ").to_vec());
                        prefixed.0.extend_from_slice(&name);
                        prefixed
                    }
                    _ => name,
                };
                gen.pending_lhs_name = Some(gen.intern_identifier(&full_name));
            } else {
                gen.pending_lhs_name = None;
            }
        } else {
            gen.pending_lhs_name = None;
        }
        // Methods, getters, and setters need the object as their [[HomeObject]]
        // so that super property lookups work.
        let is_method_like = property.is_method
            || property.property_type == ObjectPropertyType::Getter
            || property.property_type == ObjectPropertyType::Setter;
        if is_method_like {
            gen.home_objects.push(dst.clone());
        }
        let value = property.value.as_ref().and_then(|v| generate_expression(v, gen, None))
            .unwrap_or_else(|| gen.add_constant_undefined());
        if is_method_like {
            gen.home_objects.pop();
        }
        gen.pending_lhs_name = None;

        match property.property_type {
            ObjectPropertyType::Spread => unreachable!("spread properties are handled before this point"),
            ObjectPropertyType::KeyValue => {
                if let Some(key_val) = &computed_key {
                    emit_put_by_value(gen, &dst, key_val, &value, PutKind::Own);
                } else if is_simple {
                    emit_object_property_set_by_key(gen, &dst, &property.key, &value, u32_from_usize(slot), cache_index, false);
                } else {
                    // Non-simple object: use PutOwnById instead of InitObjectLiteralProperty
                    let property_key = match &property.key.inner {
                        ExpressionKind::Identifier(ident) => gen.intern_property_key(&ident.name),
                        ExpressionKind::StringLiteral(s) => gen.intern_property_key(s),
                        _ => {
                            emit_object_property_set_by_key(gen, &dst, &property.key, &value, u32_from_usize(slot), cache_index, false);
                            continue;
                        }
                    };
                    let cache = gen.next_property_lookup_cache();
                    gen.emit(Instruction::PutOwnById {
                        base: dst.operand(),
                        property: property_key,
                        src: value.operand(),
                        cache_index: cache,
                        base_identifier: None,
                    });
                }
            }
            ObjectPropertyType::Getter => {
                if let Some(key_val) = &computed_key {
                    emit_put_by_value(gen, &dst, key_val, &value, PutKind::Getter);
                } else {
                    emit_object_accessor_by_key(gen, &dst, &property.key, &value, true, false);
                }
            }
            ObjectPropertyType::Setter => {
                if let Some(key_val) = &computed_key {
                    emit_put_by_value(gen, &dst, key_val, &value, PutKind::Setter);
                } else {
                    emit_object_accessor_by_key(gen, &dst, &property.key, &value, false, false);
                }
            }
            ObjectPropertyType::ProtoSetter => {
                let key = gen.intern_property_key(utf16!("__proto__"));
                let cache = gen.next_property_lookup_cache();
                gen.emit(Instruction::PutPrototypeById {
                    base: dst.operand(),
                    property: key,
                    src: value.operand(),
                    cache_index: cache,
                    base_identifier: None,
                });
            }
        }
    }

    if is_simple {
        gen.emit(Instruction::CacheObjectShape {
            object: dst.operand(),
            cache_index,
        });
    }

    Some(dst)
}

/// Emit a property set for an object literal key (static or computed).
fn emit_object_property_set_by_key(
    gen: &mut Generator,
    object: &ScopedOperand,
    key: &Expression,
    value: &ScopedOperand,
    slot: u32,
    cache_index: u32,
    is_computed: bool,
) {
    if is_computed {
        let key_val = generate_expression_or_undefined(key, gen, None);
        gen.emit(Instruction::PutOwnByValue {
            base: object.operand(),
            property: key_val.operand(),
            src: value.operand(),
            base_identifier: None,
        });
        return;
    }
    match &key.inner {
        ExpressionKind::Identifier(ident) => {
            let property_key = gen.intern_property_key(&ident.name);
            gen.emit(Instruction::InitObjectLiteralProperty {
                object: object.operand(),
                property: property_key,
                src: value.operand(),
                shape_cache_index: cache_index,
                property_slot: slot,
            });
        }
        ExpressionKind::StringLiteral(s) => {
            let property_key = gen.intern_property_key(s);
            gen.emit(Instruction::InitObjectLiteralProperty {
                object: object.operand(),
                property: property_key,
                src: value.operand(),
                shape_cache_index: cache_index,
                property_slot: slot,
            });
        }
        ExpressionKind::NumericLiteral(n) => {
            let key_val = gen.add_constant_number(*n);
            gen.emit(Instruction::PutOwnByValue {
                base: object.operand(),
                property: key_val.operand(),
                src: value.operand(),
                base_identifier: None,
            });
        }
        _ => {
            // Computed key
            let key_val = generate_expression_or_undefined(key, gen, None);
            gen.emit(Instruction::PutOwnByValue {
                base: object.operand(),
                property: key_val.operand(),
                src: value.operand(),
                base_identifier: None,
            });
        }
    }
}

/// Emit a getter/setter for an object literal key.
fn emit_object_accessor_by_key(
    gen: &mut Generator,
    object: &ScopedOperand,
    key: &Expression,
    value: &ScopedOperand,
    is_getter: bool,
    is_computed: bool,
) {
    let emit_by_id = |gen: &mut Generator, name: &[u16]| {
        let property_key = gen.intern_property_key(name);
        let cache = gen.next_property_lookup_cache();
        if is_getter {
            gen.emit(Instruction::PutGetterById {
                base: object.operand(),
                property: property_key,
                src: value.operand(),
                cache_index: cache,
                base_identifier: None,
            });
        } else {
            gen.emit(Instruction::PutSetterById {
                base: object.operand(),
                property: property_key,
                src: value.operand(),
                cache_index: cache,
                base_identifier: None,
            });
        }
    };

    let emit_by_value = |gen: &mut Generator, key: &Expression| {
        let key_val = generate_expression_or_undefined(key, gen, None);
        if is_getter {
            gen.emit(Instruction::PutGetterByValue {
                base: object.operand(),
                property: key_val.operand(),
                src: value.operand(),
                base_identifier: None,
            });
        } else {
            gen.emit(Instruction::PutSetterByValue {
                base: object.operand(),
                property: key_val.operand(),
                src: value.operand(),
                base_identifier: None,
            });
        }
    };

    if is_computed {
        emit_by_value(gen, key);
        return;
    }

    match &key.inner {
        ExpressionKind::Identifier(ident) => emit_by_id(gen, &ident.name),
        ExpressionKind::StringLiteral(s) => emit_by_id(gen, s),
        _ => emit_by_value(gen, key),
    }
}

// =============================================================================
// Optional chain
// =============================================================================

/// Generate an optional chain, writing results into pre-allocated current_value
/// and current_base registers.
fn generate_optional_chain_inner(
    gen: &mut Generator,
    base: &Expression,
    references: &[OptionalChainReference],
    current_value: &ScopedOperand,
    current_base: &ScopedOperand,
) -> Option<()> {
    // Evaluate base expression.
    let new_current_value = match &base.inner {
        ExpressionKind::Member { object, property, computed } => {
            let is_super = matches!(object.inner, ExpressionKind::Super);
            // For super property access, resolve this binding first (before
            // ResolveSuperBase) to match C++ evaluation order.
            let this_value = if is_super { Some(emit_resolve_this_binding(gen)) } else { None };
            let obj = generate_expression(object, gen, None)?;
            let val = gen.allocate_register();
            if is_super {
                let this_value = this_value.unwrap();
                emit_super_get(gen, &val, &obj, property, *computed, &this_value);
                gen.emit_mov(current_base, &this_value);
            } else if *computed {
                let property = generate_expression(property, gen, None)?;
                emit_get_by_value(gen, &val, &obj, &property, None);
                gen.emit_mov(current_base, &obj);
            } else if let ExpressionKind::Identifier(ident) = &property.inner {
                let base_id = intern_base_identifier(gen, object);
                emit_get_by_id(gen, &val, &obj, &ident.name, base_id);
                gen.emit_mov(current_base, &obj);
            } else if let ExpressionKind::PrivateIdentifier(name) = &property.inner {
                let id = gen.intern_identifier(&name.name);
                gen.emit(Instruction::GetPrivateById {
                    dst: val.operand(),
                    base: obj.operand(),
                    property: id,
                });
                gen.emit_mov(current_base, &obj);
            } else {
                let property = generate_expression(property, gen, None)?;
                emit_get_by_value(gen, &val, &obj, &property, None);
                gen.emit_mov(current_base, &obj);
            }
            val
        }
        ExpressionKind::OptionalChain { base: inner_base, references: inner_refs } => {
            generate_optional_chain_inner(gen, inner_base, inner_refs, current_value, current_base)?;
            current_value.clone()
        }
        _ => generate_expression(base, gen, None)?,
    };

    gen.emit_mov(current_value, &new_current_value);

    // Create shared blocks: load_undefined_block is reused for all optional
    // short-circuits (matching C++ block layout).
    let load_undefined_block = gen.make_block();
    let end_block = gen.make_block();

    for reference in references {
        let is_optional = match reference {
            OptionalChainReference::Call { mode, .. }
            | OptionalChainReference::ComputedReference { mode, .. }
            | OptionalChainReference::MemberReference { mode, .. }
            | OptionalChainReference::PrivateMemberReference { mode, .. } => {
                *mode == OptionalChainMode::Optional
            }
        };

        if is_optional {
            let not_nullish_block = gen.make_block();
            gen.emit(Instruction::JumpNullish {
                condition: current_value.operand(),
                true_target: load_undefined_block,
                false_target: not_nullish_block,
            });
            gen.switch_to_basic_block(not_nullish_block);
        }

        match reference {
            OptionalChainReference::MemberReference { identifier, .. } => {
                gen.emit_mov(current_base, current_value);
                emit_get_by_id(gen, current_value, current_value, &identifier.name, None);
            }
            OptionalChainReference::ComputedReference { expression, .. } => {
                gen.emit_mov(current_base, current_value);
                let property = generate_expression(expression, gen, None)?;
                emit_get_by_value(gen, current_value, current_value, &property, None);
            }
            OptionalChainReference::Call { arguments, .. } => {
                let arguments_array = generate_arguments_array(gen, arguments);
                gen.emit(Instruction::CallWithArgumentArray {
                    dst: current_value.operand(),
                    callee: current_value.operand(),
                    this_value: current_base.operand(),
                    arguments: arguments_array.operand(),
                    expression_string: None,
                });
                let undef = gen.add_constant_undefined();
                gen.emit_mov(current_base, &undef);
            }
            OptionalChainReference::PrivateMemberReference { private_identifier, .. } => {
                gen.emit_mov(current_base, current_value);
                let id = gen.intern_identifier(&private_identifier.name);
                gen.emit(Instruction::GetPrivateById {
                    dst: current_value.operand(),
                    base: current_value.operand(),
                    property: id,
                });
            }
        }
    }

    gen.emit(Instruction::Jump {
        target: end_block,
    });

    gen.switch_to_basic_block(load_undefined_block);
    let undef = gen.add_constant_undefined();
    gen.emit_mov(current_value, &undef);
    gen.emit(Instruction::Jump {
        target: end_block,
    });

    gen.switch_to_basic_block(end_block);
    Some(())
}

/// Convert arguments to an array for CallWithArgumentArray (matching C++
/// arguments_to_array_for_call).
fn generate_arguments_array(
    gen: &mut Generator,
    arguments: &[CallArgument],
) -> ScopedOperand {
    let dst = gen.allocate_register();
    if arguments.is_empty() {
        gen.emit(Instruction::NewArray {
            dst: dst.operand(),
            element_count: 0,
            elements: vec![],
        });
        return dst;
    }

    let first_spread = arguments.iter().position(|a| a.is_spread).unwrap_or(arguments.len());

    let mut arg_holders = Vec::with_capacity(first_spread);
    for argument in &arguments[..first_spread] {
        let reg = gen.allocate_register();
        let val = generate_expression_or_undefined(&argument.value, gen, None);
        gen.emit_mov(&reg, &val);
        arg_holders.push(reg);
    }

    let arg_ops: Vec<Operand> = arg_holders.iter().map(|a| a.operand()).collect();
    gen.emit(Instruction::NewArray {
        dst: dst.operand(),
        element_count: u32_from_usize(arg_ops.len()),
        elements: arg_ops,
    });
    // FIXME: Remove this manual drop() when we no longer need to match C++ register allocation.
    drop(arg_holders);

    for argument in &arguments[first_spread..] {
        let val = generate_expression_or_undefined(&argument.value, gen, None);
        gen.emit(Instruction::ArrayAppend {
            dst: dst.operand(),
            src: val.operand(),
            is_spread: argument.is_spread,
        });
    }

    dst
}

// =============================================================================
// Class expression
// =============================================================================

/// Generate bytecode for a class expression or declaration.
///
/// Creates a ClassBlueprint via FFI containing the constructor SFD,
/// class elements (methods, fields, accessors, static initializers),
/// and then emits a NewClass instruction that creates the class at runtime.
fn generate_class_expression(
    gen: &mut Generator,
    data: &ClassData,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let has_super = data.super_class.is_some();
    let lhs_name = if data.name.is_none() { gen.pending_lhs_name.take() } else { None };

    // Step 2: Save parent environment, create class lexical environment.
    let parent_env = gen.current_lexical_environment();
    let class_env = gen.allocate_register();
    gen.emit(Instruction::CreateLexicalEnvironment {
        dst: class_env.operand(),
        parent: parent_env.operand(),
        capacity: 0,
    });
    gen.lexical_environment_register_stack.push(class_env.clone());

    // Step 3.a: Create binding for the class name in the class environment.
    // Only emit when the class has a name, or when there's no lhs_name
    // (matching C++ behavior which skips this for anonymous classes with lhs_name).
    if data.name.is_some() || lhs_name.is_none() {
        let name = if let Some(name_ident) = &data.name {
            name_ident.name.clone()
        } else {
            Utf16String::new()
        };
        let name_id = gen.intern_identifier(&name);
        gen.emit(Instruction::CreateVariable {
            identifier: name_id,
            mode: EnvironmentMode::Lexical as u32,
            is_immutable: true,
            is_global: false,
            is_strict: false,
        });
    }

    // Evaluate super class if present
    let super_class = if let Some(super_expression) = &data.super_class {
        generate_expression(super_expression, gen, None)
    } else {
        None
    };

    // Create private environment for private class elements.
    let mut has_private_env = false;
    for element_node in &data.elements {
        let priv_name = match &element_node.inner {
            ClassElement::Method { key, .. } | ClassElement::Field { key, .. } => {
                if let ExpressionKind::PrivateIdentifier(ident) = &key.inner {
                    Some(ident.name.clone())
                } else {
                    None
                }
            }
            ClassElement::StaticInitializer { .. } => None,
        };
        if let Some(name) = priv_name {
            if !has_private_env {
                gen.emit(Instruction::CreatePrivateEnvironment);
                has_private_env = true;
            }
            let name_id = gen.intern_identifier(&name);
            gen.emit(Instruction::AddPrivateName { name: name_id });
        }
    }

    // First pass: evaluate all computed property keys.
    // This must happen before registering the constructor and method SFDs,
    // matching the C++ pipeline's two-loop structure.
    let mut element_keys: Vec<Option<ScopedOperand>> = Vec::with_capacity(data.elements.len());
    for element_node in &data.elements {
        match &element_node.inner {
            ClassElement::Method { key, .. } => {
                if !is_private_key(key) {
                    let key_val = generate_expression(key, gen, None);
                    element_keys.push(key_val);
                } else {
                    element_keys.push(None);
                }
            }
            ClassElement::Field { key, .. } => {
                if !is_private_key(key) {
                    let key_val = generate_expression(key, gen, None);
                    element_keys.push(key_val);
                } else {
                    element_keys.push(None);
                }
            }
            ClassElement::StaticInitializer { .. } => {
                element_keys.push(None);
            }
        }
    }

    // Create SharedFunctionInstanceData for constructor
    let constructor_sfd_index = if let Some(ctor_expression) = &data.constructor {
        // Explicit constructor — extract FunctionData from the expression
        if let ExpressionKind::Function(function_id) = &ctor_expression.inner {
            let function_data = gen.function_table.take(*function_id);
            emit_new_function(gen, function_data, None)
        } else {
            // Fallback: synthesize a default constructor
            emit_default_constructor(gen, has_super)
        }
    } else {
        // No explicit constructor — synthesize a default one
        emit_default_constructor(gen, has_super)
    };

    // Second pass: register method/field SFDs and build element descriptors.
    let mut ffi_elements = Vec::with_capacity(data.elements.len());
    // Keep literal string data alive until FFI call.
    let mut literal_string_storage: Vec<Utf16String> = Vec::new();

    for element_node in &data.elements {
        match &element_node.inner {
            ClassElement::Method {
                key,
                function,
                kind,
                is_static,
            } => {
                let ffi_kind = match kind {
                    ClassMethodKind::Method => ClassElementKind::Method as u8,
                    ClassMethodKind::Getter => ClassElementKind::Getter as u8,
                    ClassMethodKind::Setter => ClassElementKind::Setter as u8,
                };

                // Create SFD for the method function.
                // Don't set the method name here — the runtime's update_function_name
                // in construct_class sets it from the evaluated property key, which
                // correctly handles computed keys (Symbols, etc).
                let sfd_index = if let ExpressionKind::Function(function_id) = &function.inner {
                    let function_data = gen.function_table.take(*function_id);
                    super::ffi::FFIOptionalU32::some(emit_new_function(
                        gen,
                        function_data,
                        None,
                    ))
                } else {
                    super::ffi::FFIOptionalU32::none()
                };

                // Handle computed vs static keys
                let is_private = is_private_key(key);

                // Point directly into the AST's PrivateIdentifier name (stable address).
                let (priv_ptr, priv_len) = get_private_identifier_ptr(key);

                ffi_elements.push(super::ffi::FFIClassElement {
                    kind: ffi_kind,
                    is_static: *is_static,
                    is_private,
                    private_identifier: priv_ptr,
                    private_identifier_len: priv_len,
                    shared_function_data_index: sfd_index,
                    has_initializer: false,
                    literal_value_kind: LiteralValueKind::None as u8,
                    literal_value_number: 0.0,
                    literal_value_string: std::ptr::null(),
                    literal_value_string_len: 0,
                });
            }
            ClassElement::Field {
                key,
                initializer,
                is_static,
            } => {
                // Detect literal initializers and store the value directly,
                // avoiding function creation for simple cases like x = 0.
                // This matches C++ ASTCodegen.cpp behavior.
                let mut literal_value_kind = LiteralValueKind::None as u8;
                let mut literal_value_number: f64 = 0.0;
                let mut literal_value_string = Utf16String::new();
                let mut sfd_index = super::ffi::FFIOptionalU32::none();

                if let Some(init_expression) = initializer {
                    let is_literal = match &init_expression.inner {
                        ExpressionKind::NumericLiteral(n) => {
                            literal_value_kind = LiteralValueKind::Number as u8;
                            literal_value_number = *n;
                            true
                        }
                        ExpressionKind::BooleanLiteral(b) => {
                            literal_value_kind = if *b {
                                LiteralValueKind::BooleanTrue as u8
                            } else {
                                LiteralValueKind::BooleanFalse as u8
                            };
                            true
                        }
                        ExpressionKind::NullLiteral => {
                            literal_value_kind = LiteralValueKind::Null as u8;
                            true
                        }
                        ExpressionKind::StringLiteral(s) => {
                            literal_value_kind = LiteralValueKind::String as u8;
                            literal_value_string = s.clone();
                            true
                        }
                        ExpressionKind::Unary { op, operand } if *op == UnaryOp::Minus => {
                            if let ExpressionKind::NumericLiteral(n) = &operand.inner {
                                literal_value_kind = LiteralValueKind::Number as u8;
                                literal_value_number = -n;
                                true
                            } else {
                                false
                            }
                        }
                        _ => false,
                    };

                    if !is_literal {
                        // Determine field name for anonymous function naming.
                        let field_name = match &key.inner {
                            ExpressionKind::Identifier(ident) => ident.name.clone(),
                            ExpressionKind::StringLiteral(s) => s.clone(),
                            ExpressionKind::PrivateIdentifier(p) => p.name.clone(),
                            ExpressionKind::NumericLiteral(n) => super::ffi::js_number_to_utf16(*n),
                            ExpressionKind::BigIntLiteral(s) => {
                                let digits = s.strip_suffix('n').unwrap_or(s);
                                Utf16String(digits.encode_utf16().collect())
                            }
                            _ => Utf16String::new(),
                        };

                        // Wrap the expression in a ClassFieldInitializer statement.
                        let body_statement = Statement::new(
                            init_expression.range,
                            StatementKind::ClassFieldInitializer {
                                expression: Box::new(init_expression.as_ref().clone()),
                                field_name,
                            },
                        );
                        let wrapper_body = Statement::new(
                            init_expression.range,
                            StatementKind::Block(ScopeData::shared_with_children(vec![body_statement])),
                        );

                        // Class bodies are always strict mode.
                        let function_data = Box::new(FunctionData {
                            name: None,
                            source_text_start: init_expression.range.start.offset,
                            source_text_end: init_expression.range.end.offset,
                            body: Box::new(wrapper_body),
                            parameters: Vec::new(),
                            function_length: 0,
                            kind: FunctionKind::Normal,
                            is_strict_mode: true,
                            is_arrow_function: false,
                            parsing_insights: FunctionParsingInsights {
                                uses_this: true,
                                uses_this_from_environment: true,
                                ..Default::default()
                            },
                        });
                        let index = emit_new_function(gen, function_data, Some(utf16!("field")));

                        // Set class_field_initializer_name on the SFD.
                        let sfd_ptr = gen.shared_function_data[index as usize];
                        let key_is_private = is_private_key(key);
                        let key_name: Utf16String = match &key.inner {
                            ExpressionKind::PrivateIdentifier(ident) => ident.name.clone(),
                            ExpressionKind::Identifier(ident) => ident.name.clone(),
                            ExpressionKind::StringLiteral(s) => s.clone(),
                            ExpressionKind::NumericLiteral(n) => {
                                super::ffi::js_number_to_utf16(*n)
                            }
                            _ => Utf16String::new(),
                        };
                        if !key_name.is_empty() {
                            unsafe {
                                super::ffi::rust_sfd_set_class_field_initializer_name(
                                    sfd_ptr,
                                    key_name.as_ptr(),
                                    key_name.len(),
                                    key_is_private,
                                );
                            }
                        }

                        sfd_index = super::ffi::FFIOptionalU32::some(index);
                    }
                }

                let is_private = is_private_key(key);

                let (priv_ptr, priv_len) = get_private_identifier_ptr(key);

                // Keep literal string data alive until FFI call.
                let (str_ptr, str_len) = if !literal_value_string.is_empty() {
                    literal_string_storage.push(literal_value_string);
                    let s = literal_string_storage.last().expect("just pushed an element");
                    (s.as_ptr(), s.len())
                } else {
                    (std::ptr::null(), 0)
                };

                ffi_elements.push(super::ffi::FFIClassElement {
                    kind: ClassElementKind::Field as u8,
                    is_static: *is_static,
                    is_private,
                    private_identifier: priv_ptr,
                    private_identifier_len: priv_len,
                    shared_function_data_index: sfd_index,
                    has_initializer: initializer.is_some(),
                    literal_value_kind,
                    literal_value_number,
                    literal_value_string: str_ptr,
                    literal_value_string_len: str_len,
                });
            }
            ClassElement::StaticInitializer { body } => {
                // Wrap the static block body in a function.
                // Class bodies are always strict mode.
                let function_data = Box::new(FunctionData {
                    name: None,
                    source_text_start: body.range.start.offset,
                    source_text_end: body.range.end.offset,
                    body: body.clone(),
                    parameters: Vec::new(),
                    function_length: 0,
                    kind: FunctionKind::Normal,
                    is_strict_mode: true,
                    is_arrow_function: false,
                    parsing_insights: FunctionParsingInsights {
                        uses_this: true,
                        uses_this_from_environment: true,
                        ..Default::default()
                    },
                });
                let sfd_index = super::ffi::FFIOptionalU32::some(emit_new_function(gen, function_data, None));

                ffi_elements.push(super::ffi::FFIClassElement {
                    kind: ClassElementKind::StaticInitializer as u8,
                    is_static: true,
                    is_private: false,
                    private_identifier: std::ptr::null(),
                    private_identifier_len: 0,
                    shared_function_data_index: sfd_index,
                    has_initializer: false,
                    literal_value_kind: LiteralValueKind::None as u8,
                    literal_value_number: 0.0,
                    literal_value_string: std::ptr::null(),
                    literal_value_string_len: 0,
                });
            }
        }
    }

    // Get class name and source text
    let class_name: Option<&[u16]> = data.name.as_ref().map(|n| &*n.name);
    let has_name = data.name.is_some();
    let (name_ptr, name_len) = class_name
        .map(|n| (n.as_ptr(), n.len()))
        .unwrap_or((std::ptr::null(), 0));

    let source_start = data.source_text_start as usize;
    let source_end = data.source_text_end as usize;
    let source_text_len = source_end - source_start;

    // Create the ClassBlueprint via FFI
    let bp_ptr = unsafe {
        super::ffi::rust_create_class_blueprint(
            gen.vm_ptr,
            gen.source_code_ptr,
            name_ptr,
            name_len,
            source_start,
            source_text_len,
            constructor_sfd_index,
            has_super,
            has_name,
            ffi_elements.as_ptr(),
            ffi_elements.len(),
        )
    };
    assert!(!bp_ptr.is_null(), "rust_create_class_blueprint returned null");
    let blueprint_index = gen.register_class_blueprint(bp_ptr);

    // Build element_keys operands for the NewClass instruction
    let element_key_ops: Vec<Option<Operand>> = element_keys
        .iter()
        .map(|k| k.as_ref().map(|s| s.operand()))
        .collect();

    // Restore parent environment before emitting NewClass.
    gen.emit(Instruction::SetLexicalEnvironment { environment: parent_env.operand() });
    gen.lexical_environment_register_stack.pop();

    // Allocate dst after element keys (matching C++ register ordering).
    let dst = choose_dst(gen, preferred_dst);

    // Emit NewClass instruction
    gen.emit(Instruction::NewClass {
        dst: dst.operand(),
        super_class: super_class.as_ref().map(|s| s.operand()),
        class_environment: class_env.operand(),
        class_blueprint_index: blueprint_index,
        lhs_name,
        element_keys_count: u32_from_usize(element_key_ops.len()),
        element_keys: element_key_ops,
    });

    if has_private_env {
        gen.emit(Instruction::LeavePrivateEnvironment);
    }

    Some(dst)
}

/// Synthesize a default constructor SharedFunctionInstanceData.
fn emit_default_constructor(gen: &mut Generator, has_super: bool) -> u32 {
    use crate::parser::{Parser, ProgramType};

    // Wrap in "function" keyword so it parses as a FunctionDeclaration.
    let source: Utf16String = if has_super {
        Utf16String::from(utf16!("function constructor(...arguments) { super(...arguments); }"))
    } else {
        Utf16String::from(utf16!("function constructor() {}"))
    };

    let mut parser = Parser::new(&source, ProgramType::Script);
    if has_super {
        parser.flags.allow_super_constructor_call = true;
    }
    let program = parser.parse_program(false);
    parser.scope_collector.analyze(false);

    assert!(
        !parser.has_errors(),
        "default constructor parse failed"
    );

    // Extract FunctionData from the parsed program.
    let function_id = if let StatementKind::Program(ref data) = program.inner {
        let scope = data.scope.borrow();
        scope.children.iter().find_map(|child| {
            if let StatementKind::FunctionDeclaration { function_id, .. } = &child.inner {
                Some(*function_id)
            } else {
                None
            }
        })
    } else {
        None
    };

    let function_id = function_id.expect("default constructor: no FunctionDeclaration found");
    let mut function_data = parser.function_table.take(function_id);

    // Zero out source text range since this is synthetic source,
    // not part of the original source code buffer.
    function_data.source_text_start = 0;
    function_data.source_text_end = 0;

    let subtable = parser.function_table.extract_reachable(&function_data);
    let sfd_ptr = unsafe {
        super::ffi::create_shared_function_data(
            function_data,
            subtable,
            gen.vm_ptr,
            gen.source_code_ptr,
            gen.strict,
            None,
        )
    };
    assert!(
        !sfd_ptr.is_null(),
        "default constructor creation returned null"
    );
    gen.register_shared_function_data(sfd_ptr)
}

/// Check if a key expression is a private identifier, return (is_private, private_name).
fn is_private_key(key: &Expression) -> bool {
    matches!(&key.inner, ExpressionKind::PrivateIdentifier(_))
}

/// Get a pointer directly into the AST's PrivateIdentifier name.
/// The pointer remains valid as long as the AST is alive.
fn get_private_identifier_ptr(key: &Expression) -> (*const u16, usize) {
    if let ExpressionKind::PrivateIdentifier(ident) = &key.inner {
        (ident.name.as_ptr(), ident.name.len())
    } else {
        (std::ptr::null(), 0)
    }
}

/// Check if a for-in/for-of LHS is a `let`/`const` declaration with non-local identifiers,
/// meaning we need a per-iteration lexical environment.
fn for_in_of_needs_lexical_env(lhs: &ForInOfLhs) -> bool {
    if let ForInOfLhs::Declaration(statement) = lhs {
        if let StatementKind::VariableDeclaration { kind, declarations } = &statement.inner {
            if *kind == DeclarationKind::Let || *kind == DeclarationKind::Const {
                let mut names = Vec::new();
                for declaration in declarations {
                    collect_target_names(&declaration.target, &mut names);
                }
                return !names.is_empty();
            }
        }
    }
    false
}

/// Collect all non-local binding names from a variable declarator target.
fn collect_target_names(target: &VariableDeclaratorTarget, names: &mut Vec<(Utf16String, bool)>) {
    match target {
        VariableDeclaratorTarget::Identifier(ident) => {
            if !ident.is_local() {
                names.push((ident.name.clone(), false));
            }
        }
        VariableDeclaratorTarget::BindingPattern(pattern) => {
            collect_pattern_binding_names(pattern, names);
        }
    }
}

/// Collect all non-local binding names from a binding pattern (recursive).
fn collect_pattern_binding_names(pattern: &BindingPattern, names: &mut Vec<(Utf16String, bool)>) {
    for entry in &pattern.entries {
        match &entry.alias {
            Some(BindingEntryAlias::Identifier(ident)) => {
                if !ident.is_local() {
                    names.push((ident.name.clone(), false));
                }
            }
            Some(BindingEntryAlias::BindingPattern(sub)) => {
                collect_pattern_binding_names(sub, names);
            }
            None => {
                if let Some(BindingEntryName::Identifier(ident)) = &entry.name {
                    if !ident.is_local() {
                        names.push((ident.name.clone(), false));
                    }
                }
            }
            Some(BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}

/// Create a per-iteration lexical environment for for-in/for-of `let`/`const` declarations.
/// Returns the parent environment register so we can restore it later.
fn create_for_in_of_lexical_env(gen: &mut Generator, lhs: &ForInOfLhs) -> ScopedOperand {
    let parent = gen.current_lexical_environment();

    // Collect all binding names to determine capacity.
    let mut binding_names: Vec<(Utf16String, bool)> = Vec::new();
    let mut is_constant = false;
    if let ForInOfLhs::Declaration(statement) = lhs {
        if let StatementKind::VariableDeclaration { kind, declarations } = &statement.inner {
            is_constant = *kind == DeclarationKind::Const;
            for declaration in declarations {
                collect_target_names(&declaration.target, &mut binding_names);
            }
        }
    }

    gen.push_new_lexical_environment(0);

    // Create variable bindings in the new environment.
    for (name, _) in &binding_names {
        let id = gen.intern_identifier(name);
        gen.emit(Instruction::CreateVariable {
            identifier: id,
            mode: EnvironmentMode::Lexical as u32,
            is_immutable: is_constant,
            is_global: false,
            is_strict: is_constant,
        });
    }

    parent
}

// =============================================================================
// For-in statement
// =============================================================================

/// Create a TDZ environment for lexical declarations in for-in/for-of heads.
/// Returns true if a TDZ scope was entered (must call leave_for_in_of_head_tdz after RHS eval).
fn enter_for_in_of_head_tdz(gen: &mut Generator, lhs: &ForInOfLhs) -> bool {
    if let ForInOfLhs::Declaration(statement) = lhs {
        if let StatementKind::VariableDeclaration { kind, declarations } = &statement.inner {
            if *kind == DeclarationKind::Let || *kind == DeclarationKind::Const {
                let mut names = Vec::new();
                for declaration in declarations {
                    collect_target_names(&declaration.target, &mut names);
                }
                if !names.is_empty() {
                    gen.push_new_lexical_environment(0);
                    for (name, _) in &names {
                        let id = gen.intern_identifier(name);
                        gen.emit(Instruction::CreateVariable {
                            identifier: id,
                            mode: EnvironmentMode::Lexical as u32,
                            is_immutable: false,
                            is_global: false,
                            is_strict: false,
                        });
                    }
                    return true;
                }
            }
        }
    }
    false
}

/// Tear down the TDZ environment after RHS evaluation.
fn leave_for_in_of_head_tdz(gen: &mut Generator) {
    gen.lexical_environment_register_stack.pop();
    if !gen.is_current_block_terminated() {
        let parent = gen.current_lexical_environment();
        gen.emit(Instruction::SetLexicalEnvironment { environment: parent.operand() });
    }
}

fn generate_for_in_of_statement(
    gen: &mut Generator,
    kind: ForInOfKind,
    lhs: &ForInOfLhs,
    rhs: &Expression,
    body: &Statement,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    match kind {
        ForInOfKind::ForIn => generate_for_in_statement(gen, lhs, rhs, body, preferred_dst),
        ForInOfKind::ForOf => generate_for_of_statement_inner(gen, lhs, rhs, body, preferred_dst, false),
        ForInOfKind::ForAwaitOf => generate_for_of_statement_inner(gen, lhs, rhs, body, preferred_dst, true),
    }
}

fn generate_for_in_statement(
    gen: &mut Generator,
    lhs: &ForInOfLhs,
    rhs: &Expression,
    body: &Statement,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // B.3.5 Initializers in ForIn Statement Heads
    // Evaluate the initializer for `for (var x = init in obj)` before the RHS.
    if let ForInOfLhs::Declaration(statement) = lhs {
        if let StatementKind::VariableDeclaration { kind: DeclarationKind::Var, declarations } = &statement.inner {
            if let Some(declaration) = declarations.first() {
                if let (VariableDeclaratorTarget::Identifier(ident), Some(init)) = (&declaration.target, &declaration.init) {
                    gen.pending_lhs_name = Some(gen.intern_identifier(&ident.name));
                    let value = generate_expression_or_undefined(init, gen, None);
                    gen.pending_lhs_name = None;
                    emit_set_variable(gen, ident, &value);
                }
            }
        }
    }

    // Match C++ block creation order: end_block and update_block first,
    // then nullish_block and continuation_block during head evaluation.
    let end_block = gen.make_block();
    let update_block = gen.make_block();
    let needs_lexical_env = for_in_of_needs_lexical_env(lhs);

    // B.3.5 Initializers in ForIn Statement Heads: evaluate initializer before RHS.
    // Create TDZ for lexical declarations before evaluating the RHS expression.
    let entered_tdz = enter_for_in_of_head_tdz(gen, lhs);
    let object = generate_expression_or_undefined(rhs, gen, None);
    if entered_tdz {
        leave_for_in_of_head_tdz(gen);
    }

    // Allocate iterator registers (matching C++ order in for_in_of_head_evaluation).
    let iterator_object = gen.allocate_register();
    let iterator_next_method = gen.allocate_register();
    let iterator_done = gen.allocate_register();

    // Check for null/undefined
    let nullish_block = gen.make_block();
    let continue_block = gen.make_block();
    gen.emit(Instruction::JumpNullish {
        condition: object.operand(),
        true_target: nullish_block,
        false_target: continue_block,
    });

    gen.switch_to_basic_block(nullish_block);
    gen.emit(Instruction::Jump {
        target: end_block,
    });

    gen.switch_to_basic_block(continue_block);

    // Get property iterator
    gen.emit(Instruction::GetObjectPropertyIterator {
        dst_iterator_object: iterator_object.operand(),
        dst_iterator_next: iterator_next_method.operand(),
        dst_iterator_done: iterator_done.operand(),
        object: object.operand(),
    });
    // FIXME: Remove this manual drop() when we no longer need to match C++ register allocation.
    drop(object);

    // Body evaluation: completion, then jump to update block.
    let completion = gen.allocate_completion_register();

    gen.emit(Instruction::Jump {
        target: update_block,
    });

    // Update: get next value
    gen.switch_to_basic_block(update_block);
    let next_value = gen.allocate_register();
    let done = gen.allocate_register();
    gen.emit(Instruction::IteratorNextUnpack {
        dst_value: next_value.operand(),
        dst_done: done.operand(),
        iterator_object: iterator_object.operand(),
        iterator_next: iterator_next_method.operand(),
        iterator_done: iterator_done.operand(),
    });

    let loop_continue_block = gen.make_block();
    gen.emit_jump_if(&done, end_block, loop_continue_block);
    gen.switch_to_basic_block(loop_continue_block);

    // Create per-iteration lexical environment for let/const declarations.
    if needs_lexical_env {
        create_for_in_of_lexical_env(gen, lhs);
    }

    // Assign to LHS
    assign_to_for_in_of_lhs(gen, lhs, &next_value);

    // Body — break/continue handle environment restoration via LeaveLexicalEnvironment boundary.
    let labels = std::mem::take(&mut gen.pending_labels);
    gen.begin_continuable_scope(update_block, labels.clone(), completion.clone());
    gen.begin_breakable_scope(end_block, labels, completion.clone());
    if needs_lexical_env {
        gen.start_boundary(BlockBoundaryType::LeaveLexicalEnvironment);
    }

    generate_with_completion(body, gen, &completion, preferred_dst);

    if needs_lexical_env {
        gen.end_variable_scope();
    }

    gen.end_breakable_scope();
    gen.end_continuable_scope();

    if !gen.is_current_block_terminated() {
        gen.emit(Instruction::Jump { target: update_block });
    }

    gen.switch_to_basic_block(end_block);
    completion
}

// =============================================================================
// Labelled statement
// =============================================================================

fn generate_labelled_statement(
    gen: &mut Generator,
    label: &Utf16String,
    item: &Statement,
    preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    // Collect all labels from nested Labelled statements.
    let mut labels = vec![label.clone()];
    let mut inner = item;
    while let StatementKind::Labelled { label: next_label, item: next_item } = &inner.inner {
        labels.push(next_label.clone());
        inner = next_item;
    }

    // For iteration/switch statements, set pending_labels so that
    // begin_breakable_scope/begin_continuable_scope pick them up.
    // NB: The parser wraps for/for-in/for-of loops in a Block for scope
    // management, so we look through single-child Block wrappers.
    let block_scope_borrow;
    let effective_inner = if let StatementKind::Block(ref scope) = inner.inner {
        block_scope_borrow = scope.borrow();
        if block_scope_borrow.children.len() == 1 {
            &block_scope_borrow.children[0]
        } else {
            inner
        }
    } else {
        inner
    };
    let is_iteration_or_switch = matches!(
        &effective_inner.inner,
        StatementKind::For { .. }
            | StatementKind::ForInOf { .. }
            | StatementKind::While { .. }
            | StatementKind::DoWhile { .. }
            | StatementKind::Switch(_)
    );

    if is_iteration_or_switch {
        let previous_labels = std::mem::replace(&mut gen.pending_labels, labels);
        let result = generate_statement(inner, gen, preferred_dst);
        gen.pending_labels = previous_labels;
        result
    } else {
        // Non-iteration: wrap in a breakable scope so `break label;` works.
        let end_block = gen.make_block();
        gen.begin_breakable_scope(end_block, labels, None);
        let result = generate_statement(inner, gen, preferred_dst);
        gen.end_breakable_scope();
        if !gen.is_current_block_terminated() {
            gen.emit(Instruction::Jump {
                target: end_block,
            });
        }
        gen.switch_to_basic_block(end_block);
        result
    }
}

// =============================================================================
// For-of statement
// =============================================================================

/// Shared implementation for for-of and for-await-of with iterator close.
fn generate_for_of_statement_inner(
    gen: &mut Generator,
    lhs: &ForInOfLhs,
    rhs: &Expression,
    body: &Statement,
    preferred_dst: Option<&ScopedOperand>,
    is_await: bool,
) -> Option<ScopedOperand> {
    // Create TDZ for lexical declarations before evaluating the RHS expression.
    let entered_tdz = enter_for_in_of_head_tdz(gen, lhs);
    let object = generate_expression_or_undefined(rhs, gen, None);
    if entered_tdz {
        leave_for_in_of_head_tdz(gen);
    }
    let end_block = gen.make_block();
    let needs_lexical_env = for_in_of_needs_lexical_env(lhs);
    let old_handler = gen.current_unwind_handler;

    // NB: Create update_block before exception blocks to match C++ block ordering
    // (C++ creates loop_end and loop_update in ForOfStatement, then exception blocks
    // in for_in_of_body_evaluation).
    let update_block = gen.make_block();

    // Get iterator
    let iterator_object = gen.allocate_register();
    let iterator_next_method = gen.allocate_register();
    let iterator_done = gen.allocate_register();
    gen.emit(Instruction::GetIterator {
        dst_iterator_object: iterator_object.operand(),
        dst_iterator_next: iterator_next_method.operand(),
        dst_iterator_done: iterator_done.operand(),
        iterable: object.operand(),
        hint: if is_await { IteratorHint::Async } else { IteratorHint::Sync } as u32,
    });

    // FIXME: Remove this manual drop() when we no longer need to match C++ register allocation.
    drop(object);

    let completion = gen.allocate_completion_register();

    // Set up iterator close via synthetic FinallyContext.
    let close_completion_type = gen.allocate_register();
    let close_completion_value = gen.allocate_register();
    let exception_preamble_block = gen.make_block();
    let iterator_close_body_block = gen.make_block();
    let lexical_env_at_entry = gen.lexical_environment_register_stack.last().cloned();

    let parent_index = gen.current_finally_context;
    gen.push_finally_context(FinallyContext {
        completion_type: close_completion_type.clone(),
        completion_value: close_completion_value.clone(),
        finally_body: iterator_close_body_block,
        exception_preamble: exception_preamble_block,
        parent_index,
        registered_jumps: Vec::new(),
        next_jump_index: FinallyContext::FIRST_JUMP_INDEX,
        lexical_environment_at_entry: lexical_env_at_entry.clone(),
    });

    // Break scope wraps the ReturnToFinally so break hits ReturnToFinally first.
    let labels = std::mem::take(&mut gen.pending_labels);
    gen.begin_breakable_scope(end_block, labels.clone(), completion.clone());
    gen.start_boundary(BlockBoundaryType::ReturnToFinally);

    gen.emit(Instruction::Jump {
        target: update_block,
    });

    // Update: get next value
    gen.switch_to_basic_block(update_block);
    let next_value = gen.allocate_register();
    let done = gen.allocate_register();

    if is_await {
        // For-await-of: Call iterator.next(), await the result, then unpack.
        let next_result = gen.allocate_register();
        gen.emit(Instruction::IteratorNext {
            dst: next_result.operand(),
            iterator_object: iterator_object.operand(),
            iterator_next: iterator_next_method.operand(),
            iterator_done: iterator_done.operand(),
        });
        // Await the next result. Pre-allocate completion registers and emit
        // Mov(received_completion, accumulator) before Await to match C++.
        let received_completion = gen.allocate_register();
        let received_completion_type = gen.allocate_register();
        let received_completion_value = gen.allocate_register();
        let acc = gen.accumulator();
        gen.emit_mov(&received_completion, &acc);
        let awaited = generate_await_with_completions(
            gen, &next_result,
            &received_completion, &received_completion_type, &received_completion_value,
        );
        gen.emit_mov(&next_result, &awaited);
        // Type check
        gen.emit(Instruction::ThrowIfNotObject {
            src: next_result.operand(),
        });
        // IteratorComplete — get .done property
        emit_get_by_id(gen, &done, &next_result, utf16!("done"), None);

        let loop_continue_block = gen.make_block();
        gen.emit_jump_if(&done, end_block, loop_continue_block);
        gen.switch_to_basic_block(loop_continue_block);

        // IteratorValue — get .value property
        emit_get_by_id(gen, &next_value, &next_result, utf16!("value"), None);
    } else {
        gen.emit(Instruction::IteratorNextUnpack {
            dst_value: next_value.operand(),
            dst_done: done.operand(),
            iterator_object: iterator_object.operand(),
            iterator_next: iterator_next_method.operand(),
            iterator_done: iterator_done.operand(),
        });

        let loop_continue_block = gen.make_block();
        gen.emit_jump_if(&done, end_block, loop_continue_block);
        gen.switch_to_basic_block(loop_continue_block);
    }

    // Set up exception handler AFTER iterator-next section.
    // Per spec, exceptions from IteratorNext/Await/IteratorComplete/IteratorValue
    // propagate directly; only LHS assignment and body exceptions trigger close.
    gen.current_unwind_handler = Some(exception_preamble_block);
    let loop_body_block = gen.make_block();
    gen.emit(Instruction::Jump {
        target: loop_body_block,
    });
    gen.switch_to_basic_block(loop_body_block);

    // Create per-iteration lexical environment for let/const declarations.
    let parent_env = if needs_lexical_env {
        Some(create_for_in_of_lexical_env(gen, lhs))
    } else {
        None
    };

    // Assign to LHS
    assign_to_for_in_of_lhs(gen, lhs, &next_value);

    // Body
    gen.begin_continuable_scope(update_block, labels, completion.clone());

    generate_with_completion(body, gen, &completion, preferred_dst);

    // Restore lexical env before continuing
    if needs_lexical_env {
        gen.lexical_environment_register_stack.pop();
    }
    gen.end_continuable_scope();

    gen.end_boundary(BlockBoundaryType::ReturnToFinally);
    gen.end_breakable_scope();

    // Pop the FinallyContext.
    let finally_ctx_index = gen.current_finally_context.expect("no active finally context");
    gen.current_finally_context = gen.finally_contexts[finally_ctx_index].parent_index;

    // Restore unwind handler
    gen.current_unwind_handler = old_handler;

    if !gen.is_current_block_terminated() {
        if needs_lexical_env {
            let parent = parent_env.as_ref().expect("parent_env must be set when restoring lexical environment");
            gen.emit(Instruction::SetLexicalEnvironment { environment: parent.operand() });
        }
        gen.emit(Instruction::Jump { target: update_block });
    }

    // --- Exception preamble: catch thrown exception, route to iterator close ---
    gen.switch_to_basic_block(exception_preamble_block);
    gen.emit(Instruction::Catch {
        dst: close_completion_value.operand(),
    });
    if let Some(env) = &lexical_env_at_entry {
        gen.emit(Instruction::SetLexicalEnvironment {
            environment: env.operand(),
        });
    }
    let throw_const = gen.add_constant_i32(FinallyContext::THROW);
    gen.emit_mov(&close_completion_type, &throw_const);
    gen.emit(Instruction::Jump {
        target: iterator_close_body_block,
    });

    // --- Iterator close body: dispatch based on completion type ---
    gen.switch_to_basic_block(iterator_close_body_block);

    // THROW path
    let throw_close_block = gen.make_block();
    let non_throw_close_block = gen.make_block();
    let throw_check_const = gen.add_constant_i32(FinallyContext::THROW);
    gen.emit(Instruction::JumpStrictlyEquals {
        lhs: close_completion_type.operand(),
        rhs: throw_check_const.operand(),
        true_target: throw_close_block,
        false_target: non_throw_close_block,
    });

    // Non-throw close: close the iterator with Normal completion, then dispatch.
    gen.switch_to_basic_block(non_throw_close_block);

    if is_await {
        // For async iterators, inline AsyncIteratorClose using GetMethod+Call+Await
        // instead of the synchronous IteratorClose instruction. This avoids spinning
        // the event loop inside bytecode execution.
        let after_close = gen.make_block();
        let return_method = gen.allocate_register();
        let return_key = gen.intern_property_key(utf16!("return"));
        gen.emit(Instruction::GetMethod {
            dst: return_method.operand(),
            object: iterator_object.operand(),
            property: return_key,
        });
        let call_return_block = gen.make_block();
        gen.emit(Instruction::JumpUndefined {
            condition: return_method.operand(),
            true_target: after_close,
            false_target: call_return_block,
        });
        gen.switch_to_basic_block(call_return_block);

        let inner_result = gen.allocate_register();
        gen.emit(Instruction::Call {
            dst: inner_result.operand(),
            callee: return_method.operand(),
            this_value: iterator_object.operand(),
            expression_string: None,
            argument_count: 0,
            arguments: vec![],
        });

        // Pre-allocate completion registers in this scope so they're freed
        // together with return_method and inner_result (matching C++ destructor order).
        let rc = gen.allocate_register();
        let rct = gen.allocate_register();
        let rcv = gen.allocate_register();
        let awaited = generate_await_with_completions(
            gen, &inner_result, &rc, &rct, &rcv,
        );
        gen.emit(Instruction::ThrowIfNotObject { src: awaited.operand() });
        gen.emit(Instruction::Jump { target: after_close });
        gen.switch_to_basic_block(after_close);
    } else {
        let undef = gen.add_constant_undefined();
        gen.emit(Instruction::IteratorClose {
            iterator_object: iterator_object.operand(),
            iterator_next: iterator_next_method.operand(),
            iterator_done: iterator_done.operand(),
            completion_type: CompletionType::Normal as u32,
            completion_value: undef.operand(),
        });
    }

    // Dispatch registered jumps (break/continue targets).
    let registered_jumps = std::mem::take(&mut gen.finally_contexts[finally_ctx_index].registered_jumps);
    for jump in &registered_jumps {
        let after_check = gen.make_block();
        let jump_const = gen.add_constant_i32(jump.index);
        gen.emit(Instruction::JumpStrictlyEquals {
            lhs: close_completion_type.operand(),
            rhs: jump_const.operand(),
            true_target: jump.target,
            false_target: after_check,
        });
        gen.switch_to_basic_block(after_check);
    }

    // RETURN path
    let return_block = gen.make_block();
    let unreachable_block = gen.make_block();
    let return_const = gen.add_constant_i32(FinallyContext::RETURN);
    gen.emit(Instruction::JumpStrictlyEquals {
        lhs: close_completion_type.operand(),
        rhs: return_const.operand(),
        true_target: return_block,
        false_target: unreachable_block,
    });

    gen.switch_to_basic_block(return_block);
    if let Some(outer_index) = gen.current_finally_context {
        let outer_ct = gen.finally_contexts[outer_index].completion_type.clone();
        let outer_cv = gen.finally_contexts[outer_index].completion_value.clone();
        let outer_fb = gen.finally_contexts[outer_index].finally_body;
        gen.emit_mov(&outer_ct, &close_completion_type);
        gen.emit_mov(&outer_cv, &close_completion_value);
        gen.emit(Instruction::Jump { target: outer_fb });
    } else if gen.is_in_generator_function() {
        gen.emit(Instruction::Yield {
            continuation_label: None,
            value: close_completion_value.operand(),
        });
    } else {
        gen.emit(Instruction::Return {
            value: close_completion_value.operand(),
        });
    }

    // Unreachable default: throw the value.
    gen.switch_to_basic_block(unreachable_block);
    gen.emit(Instruction::Throw {
        src: close_completion_value.operand(),
    });

    // Throw close: close iterator then rethrow original exception.
    gen.switch_to_basic_block(throw_close_block);

    if is_await {
        // Inline AsyncIteratorClose with exception handler: any error from the close
        // steps is discarded and the original exception is rethrown.
        let rethrow_block = gen.make_block();
        let close_catch_block = gen.make_block();

        // Set up an exception handler that catches errors from the close and rethrows original.
        let old_close_handler = gen.current_unwind_handler;
        gen.current_unwind_handler = Some(close_catch_block);

        // Jump to a block created inside the unwind context so that
        // GetMethod/Call/Await all have the exception handler set.
        let close_try_block = gen.make_block();
        gen.emit(Instruction::Jump { target: close_try_block });
        gen.switch_to_basic_block(close_try_block);

        let return_method = gen.allocate_register();
        let return_key = gen.intern_property_key(utf16!("return"));
        gen.emit(Instruction::GetMethod {
            dst: return_method.operand(),
            object: iterator_object.operand(),
            property: return_key,
        });

        let call_return_block = gen.make_block();
        gen.emit(Instruction::JumpUndefined {
            condition: return_method.operand(),
            true_target: rethrow_block,
            false_target: call_return_block,
        });
        gen.switch_to_basic_block(call_return_block);

        let inner_result = gen.allocate_register();
        gen.emit(Instruction::Call {
            dst: inner_result.operand(),
            callee: return_method.operand(),
            this_value: iterator_object.operand(),
            expression_string: None,
            argument_count: 0,
            arguments: vec![],
        });

        // Pre-allocate completion registers in this scope so they're freed
        // together with return_method and inner_result (matching C++ destructor order).
        let rc = gen.allocate_register();
        let rct = gen.allocate_register();
        let rcv = gen.allocate_register();
        generate_await_with_completions(gen, &inner_result, &rc, &rct, &rcv);

        // Even if close succeeded, rethrow original (spec step 5).
        gen.emit(Instruction::Jump { target: rethrow_block });

        // FIXME: Remove these manual drop() calls when we no longer need to match C++ register allocation.
        drop(rcv);
        drop(rct);
        drop(rc);
        drop(inner_result);
        drop(return_method);

        // Exception handler: discard close error, rethrow original.
        gen.current_unwind_handler = old_close_handler;
        gen.switch_to_basic_block(close_catch_block);
        let discarded = gen.allocate_register();
        gen.emit(Instruction::Catch { dst: discarded.operand() });
        gen.emit(Instruction::Jump { target: rethrow_block });

        gen.switch_to_basic_block(rethrow_block);
        gen.emit(Instruction::Throw { src: close_completion_value.operand() });
    } else {
        gen.emit(Instruction::IteratorClose {
            iterator_object: iterator_object.operand(),
            iterator_next: iterator_next_method.operand(),
            iterator_done: iterator_done.operand(),
            completion_type: CompletionType::Throw as u32,
            completion_value: close_completion_value.operand(),
        });
        if !gen.is_current_block_terminated() {
            gen.emit(Instruction::Throw {
                src: close_completion_value.operand(),
            });
        }
    }

    // Release the FinallyContext's ScopedOperands so their registers
    // can be reused (matching C++ where the FinallyContext goes out
    // of scope on the stack after for-of codegen).
    let dummy = gen.add_constant_undefined();
    gen.finally_contexts[finally_ctx_index].completion_type = dummy.clone();
    gen.finally_contexts[finally_ctx_index].completion_value = dummy;
    gen.finally_contexts[finally_ctx_index].lexical_environment_at_entry = None;

    gen.switch_to_basic_block(end_block);
    completion
}

fn assign_to_for_in_of_lhs(
    gen: &mut Generator,
    lhs: &ForInOfLhs,
    value: &ScopedOperand,
) {
    match lhs {
        ForInOfLhs::Declaration(statement) => {
            // UsingDeclaration: disposal semantics not yet implemented.
            // Match C++ behavior: UsingDeclaration is not recognized as a
            // VariableDeclaration in for_in_of_head_evaluation, so C++ treats
            // it as Assignment lhs_kind. emit_store_to_reference then calls
            // UsingDeclaration::generate_bytecode (producing NewTypeError +
            // Throw) and follows with NewReferenceError + Throw (dead code).
            if matches!(statement.inner, StatementKind::UsingDeclaration { .. }) {
                generate_statement(statement, gen, None);
                let exception = gen.allocate_register();
                let error_string = gen.intern_string(utf16!("Invalid left-hand side in assignment"));
                gen.emit(Instruction::NewReferenceError {
                    dst: exception.operand(),
                    error_string,
                });
                gen.perform_needed_unwinds();
                gen.emit(Instruction::Throw { src: exception.operand() });
                // Switch to a dead block so the caller can continue
                // generating body code (matching C++ emit_store_to_reference).
                let dead = gen.make_block();
                gen.switch_to_basic_block(dead);
                return;
            }
            // The declaration is a VariableDeclaration with a single declarator
            if let StatementKind::VariableDeclaration { kind, declarations } = &statement.inner {
                if let Some(declaration) = declarations.first() {
                    // For var: FDI already initialized the binding, so use Set.
                    // For let/const: per-iteration env created new bindings needing Initialize.
                    let mode = match kind {
                        DeclarationKind::Var => BindingMode::Set,
                        DeclarationKind::Let | DeclarationKind::Const => {
                            BindingMode::InitializeLexical
                        }
                    };
                    match &declaration.target {
                        VariableDeclaratorTarget::Identifier(ident) => {
                            emit_set_variable_with_mode(gen, ident, value, mode);
                        }
                        VariableDeclaratorTarget::BindingPattern(pattern) => {
                            generate_binding_pattern_bytecode(gen, pattern, mode, value);
                        }
                    }
                }
            }
        }
        ForInOfLhs::Expression(expression) => {
            emit_store_to_reference(gen, expression, value);
        }
        ForInOfLhs::Pattern(pattern) => {
            generate_binding_pattern_bytecode(gen, pattern, BindingMode::Set, value);
        }
    }
}

// =============================================================================
// Binding pattern destructuring
// =============================================================================

/// Whether we are initializing a new binding or setting an existing one.
#[derive(Clone, Copy)]
enum BindingMode {
    /// `const` or `let` declarations: emit InitializeLexicalBinding.
    InitializeLexical,
    /// Assignment expressions / var iteration: emit SetLexicalBinding or SetGlobal.
    Set,
}

fn set_pending_lhs_name_for_entry(gen: &mut Generator, entry: &BindingEntry) {
    let name = match &entry.alias {
        Some(BindingEntryAlias::Identifier(id)) => Some(&id.name),
        None => {
            if let Some(BindingEntryName::Identifier(id)) = &entry.name {
                Some(&id.name)
            } else {
                None
            }
        }
        _ => None,
    };
    if let Some(name) = name {
        gen.pending_lhs_name = Some(gen.intern_identifier(name));
    }
}

fn generate_binding_pattern_bytecode(
    gen: &mut Generator,
    pattern: &BindingPattern,
    mode: BindingMode,
    input_value: &ScopedOperand,
) {
    match pattern.kind {
        BindingPatternKind::Array => {
            generate_array_binding_pattern(gen, pattern, mode, input_value);
        }
        BindingPatternKind::Object => {
            generate_object_binding_pattern(gen, pattern, mode, input_value);
        }
    }
}

fn emit_set_variable_with_mode(
    gen: &mut Generator,
    ident: &Identifier,
    value: &ScopedOperand,
    mode: BindingMode,
) {
    if ident.is_local() {
        let local = gen.resolve_local(ident.local_index.get(), ident.local_type.get().unwrap());
        gen.emit_mov(&local, value);
    } else {
        let id = gen.intern_identifier(&ident.name);
        match mode {
            BindingMode::InitializeLexical => {
                gen.emit(Instruction::InitializeLexicalBinding {
                    identifier: id,
                    src: value.operand(),
                    cache: EnvironmentCoordinate::empty(),
                });
            }
            BindingMode::Set => {
                if ident.is_global.get() {
                    let cache = gen.next_global_variable_cache();
                    gen.emit(Instruction::SetGlobal {
                        identifier: id,
                        src: value.operand(),
                        cache_index: cache,
                    });
                } else {
                    gen.emit(Instruction::SetLexicalBinding {
                        identifier: id,
                        src: value.operand(),
                        cache: EnvironmentCoordinate::empty(),
                    });
                }
            }
        }
    }
}

fn assign_binding_entry_alias(
    gen: &mut Generator,
    entry: &BindingEntry,
    value: &ScopedOperand,
    mode: BindingMode,
) {
    match &entry.alias {
        None => {
            // Name IS the binding target (e.g., `{ x }` or array element).
            if let Some(BindingEntryName::Identifier(ident)) = &entry.name {
                emit_set_variable_with_mode(gen, ident, value, mode);
            }
        }
        Some(BindingEntryAlias::Identifier(ident)) => {
            emit_set_variable_with_mode(gen, ident, value, mode);
        }
        Some(BindingEntryAlias::BindingPattern(sub_pattern)) => {
            generate_binding_pattern_bytecode(gen, sub_pattern, mode, value);
        }
        Some(BindingEntryAlias::MemberExpression(expression)) => {
            emit_store_to_reference(gen, expression, value);
        }
    }
}

fn generate_array_binding_pattern(
    gen: &mut Generator,
    pattern: &BindingPattern,
    mode: BindingMode,
    input_array: &ScopedOperand,
) {
    let is_exhausted = gen.allocate_register();
    let false_val = gen.add_constant_boolean(false);
    gen.emit_mov(&is_exhausted, &false_val);

    let iterator_object = gen.allocate_register();
    let iterator_next = gen.allocate_register();
    let iterator_done = gen.allocate_register();
    gen.emit(Instruction::GetIterator {
        dst_iterator_object: iterator_object.operand(),
        dst_iterator_next: iterator_next.operand(),
        dst_iterator_done: iterator_done.operand(),
        iterable: input_array.operand(),
        hint: IteratorHint::Sync as u32,
    });

    for (index, entry) in pattern.entries.iter().enumerate() {
        if entry.is_rest {
            // 13.15.5.3 AssignmentRestElement: ... DestructuringAssignmentTarget
            // Step 1: Evaluate the reference BEFORE iterating remaining elements.
            let evaluated_ref = if let Some(BindingEntryAlias::MemberExpression(expression)) = &entry.alias {
                Some(emit_evaluate_member_reference(gen, expression))
            } else {
                None
            };

            // Rest element: collect remaining into array.
            // NB: Allocate register unconditionally, then re-allocate in the
            // else branch (matching C++ which allocates before the if/else,
            // then re-allocates after emit_jump_if in the !first path).
            let mut value = gen.allocate_register();
            if index == 0 {
                gen.emit(Instruction::IteratorToArray {
                    dst: value.operand(),
                    iterator_object: iterator_object.operand(),
                    iterator_next_method: iterator_next.operand(),
                    iterator_done_property: iterator_done.operand(),
                });
            } else {
                let if_exhausted = gen.make_block();
                let if_not_exhausted = gen.make_block();
                let continuation = gen.make_block();

                gen.emit_jump_if(&is_exhausted, if_exhausted, if_not_exhausted);

                value = gen.allocate_register();

                gen.switch_to_basic_block(if_exhausted);
                gen.emit(Instruction::NewArray {
                    dst: value.operand(),
                    element_count: 0,
                    elements: Vec::new(),
                });
                gen.emit(Instruction::Jump {
                    target: continuation,
                });

                gen.switch_to_basic_block(if_not_exhausted);
                gen.emit(Instruction::IteratorToArray {
                    dst: value.operand(),
                    iterator_object: iterator_object.operand(),
                    iterator_next_method: iterator_next.operand(),
                    iterator_done_property: iterator_done.operand(),
                });
                gen.emit(Instruction::Jump {
                    target: continuation,
                });

                gen.switch_to_basic_block(continuation);
            }

            if let Some(ref eref) = evaluated_ref {
                emit_store_to_evaluated_reference(gen, eref, &value);
            } else {
                assign_binding_entry_alias(gen, entry, &value, mode);
            }
            return; // rest consumes the iterator
        }

        // 13.15.5.5 AssignmentElement: DestructuringAssignmentTarget Initializer(opt)
        // Step 1: Evaluate the reference BEFORE calling IteratorStepValue.
        let evaluated_ref = if let Some(BindingEntryAlias::MemberExpression(expression)) = &entry.alias {
            Some(emit_evaluate_member_reference(gen, expression))
        } else {
            None
        };

        // For elisions (name is None), we still advance the iterator
        // but don't bind anything.
        let is_elision = entry.name.is_none() && entry.alias.is_none();

        let exhausted_block = gen.make_block();

        if index != 0 {
            let not_exhausted_block = gen.make_block();
            gen.emit_jump_if(&is_exhausted, exhausted_block, not_exhausted_block);
            gen.switch_to_basic_block(not_exhausted_block);
        }

        let value = gen.allocate_register();
        gen.emit(Instruction::IteratorNextUnpack {
            dst_value: value.operand(),
            dst_done: is_exhausted.operand(),
            iterator_object: iterator_object.operand(),
            iterator_next: iterator_next.operand(),
            iterator_done: iterator_done.operand(),
        });

        // Check if iterator got exhausted by this step.
        let no_bail_block = gen.make_block();
        gen.emit_jump_if(&is_exhausted, exhausted_block, no_bail_block);

        gen.switch_to_basic_block(no_bail_block);
        let create_binding_block = gen.make_block();
        gen.emit(Instruction::Jump {
            target: create_binding_block,
        });

        // Exhausted: load undefined.
        gen.switch_to_basic_block(exhausted_block);
        let undef = gen.add_constant_undefined();
        gen.emit_mov(&value, &undef);
        gen.emit(Instruction::Jump {
            target: create_binding_block,
        });

        gen.switch_to_basic_block(create_binding_block);

        // Handle default initializer.
        if let Some(ref initializer) = entry.initializer {
            let if_undefined = gen.make_block();
            let if_not_undefined = gen.make_block();
            gen.emit(Instruction::JumpUndefined {
                condition: value.operand(),
                true_target: if_undefined,
                false_target: if_not_undefined,
            });
            gen.switch_to_basic_block(if_undefined);
            set_pending_lhs_name_for_entry(gen, entry);
            if let Some(default_value) = generate_expression(initializer, gen, None) {
                gen.emit_mov(&value, &default_value);
            }
            gen.emit(Instruction::Jump {
                target: if_not_undefined,
            });
            gen.switch_to_basic_block(if_not_undefined);
        }

        if !is_elision {
            if let Some(ref eref) = evaluated_ref {
                emit_store_to_evaluated_reference(gen, eref, &value);
            } else {
                assign_binding_entry_alias(gen, entry, &value, mode);
            }
        }
    }

    // Close iterator if not exhausted.
    let done_block = gen.make_block();
    let not_done_block = gen.make_block();
    gen.emit_jump_if(&is_exhausted, done_block, not_done_block);
    gen.switch_to_basic_block(not_done_block);
    let undef = gen.add_constant_undefined();
    gen.emit(Instruction::IteratorClose {
        iterator_object: iterator_object.operand(),
        iterator_next: iterator_next.operand(),
        iterator_done: iterator_done.operand(),
        completion_type: CompletionType::Normal as u32,
        completion_value: undef.operand(),
    });
    gen.emit(Instruction::Jump {
        target: done_block,
    });
    gen.switch_to_basic_block(done_block);
}

fn generate_object_binding_pattern(
    gen: &mut Generator,
    pattern: &BindingPattern,
    mode: BindingMode,
    object: &ScopedOperand,
) {
    gen.emit(Instruction::ThrowIfNullish {
        src: object.operand(),
    });

    let mut excluded_names: Vec<ScopedOperand> = Vec::new();
    let has_rest = pattern
        .entries
        .last()
        .is_some_and(|e| e.is_rest);

    for entry in &pattern.entries {
        if entry.is_rest {
            // Rest element: copy object excluding already-destructured properties.
            let copy = gen.allocate_register();
            gen.emit(Instruction::CopyObjectExcludingProperties {
                dst: copy.operand(),
                from_object: object.operand(),
                excluded_names_count: u32_from_usize(excluded_names.len()),
                excluded_names: excluded_names.iter().map(|o| o.operand()).collect(),
            });
            assign_binding_entry_alias(gen, entry, &copy, mode);
            return;
        }

        let value = gen.allocate_register();

        match &entry.name {
            Some(BindingEntryName::Identifier(ident)) => {
                emit_get_by_id(gen, &value, object, &ident.name, None);
                if has_rest {
                    let name_val = gen.add_constant_string(ident.name.clone());
                    excluded_names.push(name_val);
                }
            }
            Some(BindingEntryName::Expression(expression)) => {
                let property_name = generate_expression_or_undefined(expression, gen, None);
                if has_rest {
                    // Only copy to a new register if the property name is a local variable,
                    // since locals can be reassigned. Registers are temporaries and won't change.
                    if property_name.operand().is_local() {
                        let excluded_name = gen.allocate_register();
                        gen.emit_mov(&excluded_name, &property_name);
                        excluded_names.push(excluded_name);
                    } else {
                        excluded_names.push(property_name.clone());
                    }
                }
                emit_get_by_value(gen, &value, object, &property_name, None);
            }
            None => {
                // Should not happen for object patterns
                continue;
            }
        }

        // Handle default initializer.
        if let Some(ref initializer) = entry.initializer {
            let if_undefined = gen.make_block();
            let if_not_undefined = gen.make_block();
            gen.emit(Instruction::JumpUndefined {
                condition: value.operand(),
                true_target: if_undefined,
                false_target: if_not_undefined,
            });
            gen.switch_to_basic_block(if_undefined);
            set_pending_lhs_name_for_entry(gen, entry);
            if let Some(default_value) = generate_expression(initializer, gen, None) {
                gen.emit_mov(&value, &default_value);
            }
            gen.emit(Instruction::Jump {
                target: if_not_undefined,
            });
            gen.switch_to_basic_block(if_not_undefined);
        }

        assign_binding_entry_alias(gen, entry, &value, mode);
    }
}

// =============================================================================
// Try statement
// =============================================================================

/// Generate bytecode for a try/catch/finally statement.
///
/// The structure is:
/// 1. Set up FinallyContext (if finally block exists)
/// 2. Set up exception handler pointing to catch/exception preamble
/// 3. Generate try body
/// 4. Generate catch block (if present)
/// 5. Generate finally block (if present) with LeaveFinally dispatch
fn generate_try_statement(
    gen: &mut Generator,
    data: &TryStatementData,
    _preferred_dst: Option<&ScopedOperand>,
) -> Option<ScopedOperand> {
    let old_handler = gen.current_unwind_handler;
    let saved_block = gen.current_block_index();

    // Save lexical environment for restoration in catch/exception handler.
    let saved_env = gen.current_lexical_environment();

    let mut next_block: Option<Label> = None;
    let mut completion: Option<ScopedOperand> = None;

    // --- Set up FinallyContext if we have a finalizer ---
    let has_finally = data.finalizer.is_some();
    let mut finally_body_block: Option<Label> = None;

    if has_finally {
        let completion_type = gen.allocate_register();
        let completion_value = gen.allocate_register();

        let exception_preamble_block = gen.make_block();
        let fb_block = gen.make_block();
        finally_body_block = Some(fb_block);

        // Save the parent FinallyContext and install new one.
        let parent_index = gen.current_finally_context;
        gen.push_finally_context(FinallyContext {
            completion_type,
            completion_value,
            finally_body: fb_block,
            exception_preamble: exception_preamble_block,
            parent_index,
            registered_jumps: Vec::new(),
            next_jump_index: FinallyContext::FIRST_JUMP_INDEX,
            lexical_environment_at_entry: Some(saved_env.clone()),
        });

        // Generate exception preamble block:
        //   Catch → completion_value
        //   SetLexicalEnvironment (restore to entry)
        //   completion_type = THROW
        //   Jump → finally_body
        gen.switch_to_basic_block(exception_preamble_block);
        let ctx_index = gen.current_finally_context.expect("no active finally context");
        let cv = gen.finally_contexts[ctx_index].completion_value.clone();
        let ct = gen.finally_contexts[ctx_index].completion_type.clone();
        gen.emit(Instruction::Catch { dst: cv.operand() });
        gen.emit(Instruction::SetLexicalEnvironment {
            environment: saved_env.operand(),
        });
        let throw_const = gen.add_constant_i32(FinallyContext::THROW);
        gen.emit_mov(&ct, &throw_const);
        gen.emit(Instruction::Jump {
            target: fb_block,
        });

        // Set exception_preamble as default handler for blocks created below.
        // The catch body gets this as its handler (exceptions in catch → finally).
        gen.current_unwind_handler = Some(exception_preamble_block);
        gen.start_boundary(BlockBoundaryType::ReturnToFinally);
    }

    // --- Generate catch handler block (if present) ---
    let mut handler_block: Option<Label> = None;
    if let Some(catch) = &data.handler {
        let hb = gen.make_block();
        handler_block = Some(hb);
        gen.switch_to_basic_block(hb);

        let caught_value = gen.allocate_register();
        gen.emit(Instruction::Catch {
            dst: caught_value.operand(),
        });
        gen.emit(Instruction::SetLexicalEnvironment {
            environment: saved_env.operand(),
        });

        // Bind the catch parameter.
        let mut created_catch_scope = false;
        if let Some(parameter) = &catch.parameter {
            match parameter {
                CatchBinding::Identifier(ident) => {
                    if ident.is_local() {
                        let local = gen.local(ident.local_index.get());
                        gen.emit_mov(&local, &caught_value);
                        gen.mark_local_initialized(ident.local_index.get());
                    } else {
                        gen.push_new_lexical_environment(0);
                        gen.start_boundary(BlockBoundaryType::LeaveLexicalEnvironment);
                        created_catch_scope = true;

                        let id = gen.intern_identifier(&ident.name);
                        gen.emit(Instruction::CreateVariable {
                            identifier: id,
                            mode: EnvironmentMode::Lexical as u32,
                            is_immutable: false,
                            is_global: false,
                            is_strict: false,
                        });
                        gen.emit(Instruction::InitializeLexicalBinding {
                            identifier: id,
                            src: caught_value.operand(),
                            cache: EnvironmentCoordinate::empty(),
                        });
                    }
                }
                CatchBinding::BindingPattern(pattern) => {
                    let mut names: Vec<(Utf16String, bool)> = Vec::new();
                    collect_pattern_binding_names(pattern, &mut names);

                    if !names.is_empty() {
                        gen.push_new_lexical_environment(0);
                        gen.start_boundary(BlockBoundaryType::LeaveLexicalEnvironment);
                        created_catch_scope = true;

                        for (name, _) in &names {
                            let id = gen.intern_identifier(name);
                            gen.emit(Instruction::CreateVariable {
                                identifier: id,
                                mode: EnvironmentMode::Lexical as u32,
                                is_immutable: false,
                                is_global: false,
                                is_strict: false,
                            });
                        }
                    }

                    generate_binding_pattern_bytecode(gen, pattern, BindingMode::InitializeLexical, &caught_value);
                }
            }
        }

        // Catch body gets its own completion register to prevent
        // break/continue inside catch from leaking values.
        let mut catch_completion: Option<ScopedOperand> = None;
        {
            let saved_completion = gen.current_completion_register.clone();
            if gen.must_propagate_completion {
                let reg = gen.allocate_register();
                let undef = gen.add_constant_undefined();
                gen.emit_mov(&reg, &undef);
                gen.current_completion_register = Some(reg.clone());
                catch_completion = Some(reg);
            }
            generate_statement(&catch.body, gen, None);
            gen.current_completion_register = saved_completion;
        }

        // Match C++ ordering: save catch completion BEFORE restoring
        // the lexical environment from the catch scope.
        if gen.must_propagate_completion {
            if let Some(ref cc) = catch_completion {
                if !gen.is_current_block_terminated() {
                    let reg = gen.allocate_register();
                    gen.emit_mov(&reg, cc);
                    completion = Some(reg);
                }
            }
        }

        if created_catch_scope {
            gen.end_variable_scope();
        }

        if !gen.is_current_block_terminated() {
            if has_finally {
                // Normal exit from catch → completion_type = NORMAL, jump to finally.
                let ctx_index = gen.current_finally_context.expect("no active finally context");
                let ct = gen.finally_contexts[ctx_index].completion_type.clone();
                let fb = gen.finally_contexts[ctx_index].finally_body;
                let normal_const = gen.add_constant_i32(FinallyContext::NORMAL);
                gen.emit_mov(&ct, &normal_const);
                gen.emit(Instruction::Jump { target: fb });
            } else {
                if next_block.is_none() {
                    next_block = Some(gen.make_block());
                }
                gen.emit(Instruction::Jump {
                    target: next_block.expect("next_block must be set"),
                });
            }
        }
    }

    if has_finally {
        gen.end_boundary(BlockBoundaryType::ReturnToFinally);
    }

    // --- Generate try body ---

    // Set handler BEFORE creating the try body block, so make_block()
    // captures the correct handler for exception routing.
    // For try-catch-finally: catch handler is inner (exceptions → catch → exception_preamble → finally).
    // For try-catch: catch handler.
    // For try-finally: exception_preamble.
    if let Some(hb) = handler_block {
        gen.current_unwind_handler = Some(hb);
    } else if has_finally {
        if let Some(ctx_index) = gen.current_finally_context {
            let ep = gen.finally_contexts[ctx_index].exception_preamble;
            gen.current_unwind_handler = Some(ep);
        }
    }

    let try_body_block = gen.make_block();
    gen.switch_to_basic_block(saved_block);
    gen.emit(Instruction::Jump {
        target: try_body_block,
    });

    if has_finally {
        gen.start_boundary(BlockBoundaryType::ReturnToFinally);
    }

    gen.switch_to_basic_block(try_body_block);

    // Try body gets its own completion register to prevent
    // break/continue inside try from leaking values.
    // NB: try_completion must be declared outside the inner scope so its
    // register stays alive during finally body generation, matching C++.
    let mut try_completion: Option<ScopedOperand> = None;
    {
        let saved_completion = gen.current_completion_register.clone();
        if gen.must_propagate_completion {
            let reg = gen.allocate_register();
            let undef = gen.add_constant_undefined();
            gen.emit_mov(&reg, &undef);
            gen.current_completion_register = Some(reg.clone());
            try_completion = Some(reg);
        }
        generate_statement(&data.block, gen, None);
        gen.current_completion_register = saved_completion;

        if !gen.is_current_block_terminated()
            && gen.must_propagate_completion {
                if let Some(ref tc) = try_completion {
                    let reg = gen.allocate_register();
                    gen.emit_mov(&reg, tc);
                    completion = Some(reg);
                }
            }
    }

    if !gen.is_current_block_terminated() {
        if has_finally {
            // Normal exit from try → completion_type = NORMAL, jump to finally.
            let ctx_index = gen.current_finally_context.expect("no active finally context");
            let ct = gen.finally_contexts[ctx_index].completion_type.clone();
            let fb = gen.finally_contexts[ctx_index].finally_body;
            let normal_const = gen.add_constant_i32(FinallyContext::NORMAL);
            gen.emit_mov(&ct, &normal_const);
            gen.emit(Instruction::Jump { target: fb });
        } else {
            gen.current_unwind_handler = old_handler;
            if next_block.is_none() {
                next_block = Some(gen.make_block());
            }
            gen.emit(Instruction::Jump {
                target: next_block.expect("next_block must be set"),
            });
        }
    }

    if has_finally {
        gen.end_boundary(BlockBoundaryType::ReturnToFinally);
    }

    // Restore old unwind handler.
    gen.current_unwind_handler = old_handler;

    // --- Generate finally body and after-finally dispatch ---
    if let Some(fb_block) = finally_body_block {
        // Pop FinallyContext.
        let ctx_index = gen.current_finally_context.expect("no active finally context");
        gen.current_finally_context = gen.finally_contexts[ctx_index].parent_index;

        // Extract fields needed for dispatch (to avoid borrow conflicts).
        let ctx_ct = gen.finally_contexts[ctx_index].completion_type.clone();
        let ctx_cv = gen.finally_contexts[ctx_index].completion_value.clone();

        gen.switch_to_basic_block(fb_block);
        gen.start_boundary(BlockBoundaryType::LeaveFinally);

        // Generate the finally body with a throwaway completion register
        // to prevent break/continue in finally from leaking the try/catch
        // completion value.
        if let Some(finalizer) = &data.finalizer {
            let saved_completion = gen.current_completion_register.clone();
            if gen.must_propagate_completion {
                let finally_completion = gen.allocate_register();
                let undef = gen.add_constant_undefined();
                gen.emit_mov(&finally_completion, &undef);
                gen.current_completion_register = Some(finally_completion);
            }
            generate_statement(finalizer, gen, None);
            gen.current_completion_register = saved_completion;
        }

        gen.end_boundary(BlockBoundaryType::LeaveFinally);

        if !gen.is_current_block_terminated() {
            if next_block.is_none() {
                next_block = Some(gen.make_block());
            }
            let nb = next_block.expect("next_block must be set");

            // After-finally dispatch chain:
            // 1. NORMAL → next block
            let after_normal_check = gen.make_block();
            let normal_const = gen.add_constant_i32(FinallyContext::NORMAL);
            gen.emit(Instruction::JumpStrictlyEquals {
                lhs: ctx_ct.operand(),
                rhs: normal_const.operand(),
                true_target: nb,
                false_target: after_normal_check,
            });
            gen.switch_to_basic_block(after_normal_check);

            // 2. Registered break/continue jumps
            let registered_jumps = std::mem::take(&mut gen.finally_contexts[ctx_index].registered_jumps);
            for jump in &registered_jumps {
                let after_jump_check = gen.make_block();
                let jump_const = gen.add_constant_i32(jump.index);
                gen.emit(Instruction::JumpStrictlyEquals {
                    lhs: ctx_ct.operand(),
                    rhs: jump_const.operand(),
                    true_target: jump.target,
                    false_target: after_jump_check,
                });
                gen.switch_to_basic_block(after_jump_check);
            }

            // 3. RETURN → actually return the completion_value
            let return_block = gen.make_block();
            let rethrow_block = gen.make_block();
            let return_const = gen.add_constant_i32(FinallyContext::RETURN);
            gen.emit(Instruction::JumpStrictlyEquals {
                lhs: ctx_ct.operand(),
                rhs: return_const.operand(),
                true_target: return_block,
                false_target: rethrow_block,
            });

            // Generate return block.
            gen.switch_to_basic_block(return_block);
            if let Some(outer_index) = gen.current_finally_context {
                // Nested finally: copy completion record to outer and jump to outer finally.
                let outer_ct = gen.finally_contexts[outer_index].completion_type.clone();
                let outer_cv = gen.finally_contexts[outer_index].completion_value.clone();
                let outer_fb = gen.finally_contexts[outer_index].finally_body;
                gen.emit_mov(&outer_ct, &ctx_ct);
                gen.emit_mov(&outer_cv, &ctx_cv);
                gen.emit(Instruction::Jump { target: outer_fb });
            } else if gen.is_in_generator_function() {
                gen.emit(Instruction::Yield {
                    continuation_label: None,
                    value: ctx_cv.operand(),
                });
            } else {
                gen.emit(Instruction::Return {
                    value: ctx_cv.operand(),
                });
            }

            // 4. Default → rethrow the exception.
            gen.switch_to_basic_block(rethrow_block);
            gen.emit(Instruction::Throw {
                src: ctx_cv.operand(),
            });
        }

        // FIXME: Remove these manual drop() calls when we no longer need to match C++ register allocation.
        drop(try_completion);
        drop(ctx_ct);
        drop(ctx_cv);
        gen.finally_contexts[ctx_index].lexical_environment_at_entry = None;
        let dummy = gen.add_constant_undefined();
        gen.finally_contexts[ctx_index].completion_value = dummy.clone();
        gen.finally_contexts[ctx_index].completion_type = dummy;
    }

    // Switch to the next block for code after the try statement.
    // When next_block is None, all paths are terminated; switch back to
    // saved_block (which is already terminated) so no dead block is emitted.
    gen.switch_to_basic_block(next_block.unwrap_or(saved_block));

    if gen.must_propagate_completion
        && completion.is_none() {
            return Some(gen.add_constant_undefined());
        }
    completion
}

/// Create a SharedFunctionInstanceData for a function expression/declaration
/// and register it with the generator.
///
/// Returns the shared_function_data_index for use in NewFunction instructions.
fn emit_new_function(
    gen: &mut Generator,
    data: Box<FunctionData>,
    name_override: Option<&[u16]>,
) -> u32 {
    assert!(
        data.source_text_end as usize <= gen.source_len,
        "Function source range out of bounds: {}..{} (source len {})",
        data.source_text_start,
        data.source_text_end,
        gen.source_len
    );

    let subtable = gen.function_table.extract_reachable(&data);
    let sfd_ptr = unsafe {
        super::ffi::create_shared_function_data(
            data,
            subtable,
            gen.vm_ptr,
            gen.source_code_ptr,
            gen.strict,
            name_override,
        )
    };

    gen.register_shared_function_data(sfd_ptr)
}

// =============================================================================
// FunctionDeclarationInstantiation (FDI)
// =============================================================================

/// Emit FDI bytecode for a function body.
///
/// Creates environment bindings, initializes parameters, creates arguments
/// objects, and hoists function declarations.
pub fn emit_function_declaration_instantiation(
    gen: &mut Generator,
    function_data: &FunctionData,
    body_scope: &ScopeData,
) {
    let strict = function_data.is_strict_mode || gen.strict;
    let is_arrow = function_data.is_arrow_function;

    // --- Compute FDI metadata ---

    // Check for parameter expressions (default values or binding patterns with defaults).
    let has_parameter_expressions = function_data.parameters.iter().any(|p| {
        p.default_value.is_some()
            || matches!(p.binding, FunctionParameterBinding::BindingPattern(_))
    });

    // Build parameter_names map and check for duplicates.
    let mut parameter_names: Vec<FdiParameterName> = Vec::new();
    let mut seen_names: HashSet<Utf16String> = HashSet::new();
    let mut has_duplicates = false;

    for parameter in &function_data.parameters {
        match &parameter.binding {
            FunctionParameterBinding::Identifier(ident) => {
                let name = ident.name.clone();
                let is_local = ident.is_local();
                if !seen_names.insert(name.clone()) {
                    has_duplicates = true;
                } else {
                    parameter_names.push(FdiParameterName { name, is_local });
                }
            }
            FunctionParameterBinding::BindingPattern(pattern) => {
                collect_binding_pattern_names(pattern, &mut parameter_names, &mut seen_names, &mut has_duplicates);
            }
        }
    }

    // Determine if arguments object is needed (from parsing insights).
    let mut arguments_object_needed = function_data.parsing_insights.might_need_arguments_object;

    if is_arrow || parameter_names.iter().any(|p| p.name == utf16!("arguments")) {
        arguments_object_needed = false;
    }

    let function_scope_data = body_scope.function_scope_data.as_ref();

    if let Some(fsd) = function_scope_data {
        if !has_parameter_expressions && fsd.has_function_named_arguments {
            arguments_object_needed = false;
        }
        if !has_parameter_expressions && arguments_object_needed && fsd.has_lexically_declared_arguments
        {
            arguments_object_needed = false;
        }
    }

    // --- Step 1: Parameter scope for parameter expressions ---

    if has_parameter_expressions {
        let has_non_local_parameters = parameter_names.iter().any(|p| !p.is_local);
        if has_non_local_parameters {
            gen.push_new_lexical_environment(0);
        }
    }

    // --- Step 2: Create bindings for non-local parameters ---

    for param in &parameter_names {
        if !param.is_local {
            let id = gen.intern_identifier(&param.name);
            gen.emit(Instruction::CreateVariable {
                identifier: id,
                mode: EnvironmentMode::Lexical as u32,
                is_immutable: false,
                is_global: false,
                is_strict: false,
            });
            if has_duplicates {
                let undef = gen.add_constant_undefined();
                gen.emit(Instruction::InitializeLexicalBinding {
                    identifier: id,
                    src: undef.operand(),
                    cache: EnvironmentCoordinate::empty(),
                });
            }
        }
    }

    // --- Step 3: Create arguments object ---

    if arguments_object_needed {
        // Find local variable index for ArgumentsObject, if any.
        let arguments_local_index = gen.local_variables.iter().position(|lv| {
            lv.name == utf16!("arguments") && !lv.is_lexically_declared
        });

        let dst = arguments_local_index.map(|index| Operand::local(u32_from_usize(index)));

        let kind = if strict || !function_data.parameters.iter().all(|p| {
            !p.is_rest
                && p.default_value.is_none()
                && matches!(p.binding, FunctionParameterBinding::Identifier(_))
        }) {
            ArgumentsKind::Unmapped as u32
        } else {
            ArgumentsKind::Mapped as u32
        };

        gen.emit(Instruction::CreateArguments {
            dst,
            kind,
            is_immutable: strict,
        });

        if let Some(index) = arguments_local_index {
            gen.mark_local_initialized(u32_from_usize(index));
        }
    }

    // --- Step 4: Bind formal parameters ---

    for (parameter_index, parameter) in function_data.parameters.iter().enumerate() {
        let parameter_index = u32_from_usize(parameter_index);

        if parameter.is_rest {
            let dst = gen.scoped_operand(Operand::argument(parameter_index));
            gen.emit(Instruction::CreateRestParams {
                dst: dst.operand(),
                rest_index: parameter_index,
            });
        } else if parameter.default_value.is_some() {
            let if_undefined_block = gen.make_block();
            let if_not_undefined_block = gen.make_block();

            gen.emit(Instruction::JumpUndefined {
                condition: Operand::argument(parameter_index),
                true_target: if_undefined_block,
                false_target: if_not_undefined_block,
            });

            gen.switch_to_basic_block(if_undefined_block);
            if let Some(value) = generate_expression(parameter.default_value.as_ref().expect("guarded by has_default_value check"), gen, None) {
                gen.emit_mov_raw(Operand::argument(parameter_index), value.operand());
            }
            gen.emit(Instruction::Jump {
                target: if_not_undefined_block,
            });

            gen.switch_to_basic_block(if_not_undefined_block);
        }

        match &parameter.binding {
            FunctionParameterBinding::Identifier(ident) => {
                if ident.is_local() {
                    let local_index = ident.local_index.get();
                    match ident.local_type.get() {
                        Some(LocalType::Variable) => gen.mark_local_initialized(local_index),
                        Some(LocalType::Argument) => gen.mark_argument_initialized(local_index),
                        None => {}
                    }
                } else {
                    let id = gen.intern_identifier(&ident.name);
                    if has_duplicates {
                        gen.emit(Instruction::SetLexicalBinding {
                            identifier: id,
                            src: Operand::argument(parameter_index),
                            cache: EnvironmentCoordinate::empty(),
                        });
                    } else {
                        gen.emit(Instruction::InitializeLexicalBinding {
                            identifier: id,
                            src: Operand::argument(parameter_index),
                            cache: EnvironmentCoordinate::empty(),
                        });
                    }
                }
            }
            FunctionParameterBinding::BindingPattern(pattern) => {
                let argument = gen.scoped_operand(Operand::argument(parameter_index));
                let mode = if has_duplicates {
                    BindingMode::Set
                } else {
                    BindingMode::InitializeLexical
                };
                generate_binding_pattern_bytecode(gen, pattern, mode, &argument);
            }
        }
    }

    // --- Step 5: Initialize var bindings ---

    if let Some(fsd) = function_scope_data {
        if !has_parameter_expressions {
            // Simple case: vars share the parameter environment.
            for var in &fsd.vars_to_initialize {
                if var.is_parameter {
                    continue;
                }
                if arguments_object_needed && var.name == utf16!("arguments") {
                    continue;
                }

                if let Some(local_binding) = var.local {
                    let undef = gen.add_constant_undefined();
                    let local = var_local_operand(gen, local_binding.local_type, local_binding.index);
                    gen.emit_mov(&local, &undef);
                } else {
                    let id = gen.intern_identifier(&var.name);
                    let undef = gen.add_constant_undefined();
                    gen.emit(Instruction::CreateVariable {
                        identifier: id,
                        mode: EnvironmentMode::Var as u32,
                        is_immutable: false,
                        is_global: false,
                        is_strict: false,
                    });
                    gen.emit(Instruction::InitializeVariableBinding {
                        identifier: id,
                        src: undef.operand(),
                        cache: EnvironmentCoordinate::empty(),
                    });
                }
            }
        } else {
            // Parameter expressions: vars get a separate environment.
            let has_non_local_vars = fsd.vars_to_initialize.iter().any(|v| v.local.is_none());

            if has_non_local_vars {
                gen.emit(Instruction::CreateVariableEnvironment {
                    capacity: u32_from_usize(fsd.non_local_var_count_for_parameter_expressions),
                });
                // After CreateVariableEnvironment, re-read the lexical environment
                // (which was also updated) and push it onto the register stack.
                // This ensures subsequent CreateLexicalEnvironment instructions
                // (e.g. Step 7) use the var environment as their parent, not the
                // parameter scope.
                let var_env = gen.allocate_register();
                gen.emit(Instruction::GetLexicalEnvironment {
                    dst: var_env.operand(),
                });
                gen.lexical_environment_register_stack.push(var_env);
            }

            for var in &fsd.vars_to_initialize {
                let is_in_parameter_bindings = var.is_parameter
                    || (arguments_object_needed && var.name == utf16!("arguments"));

                let initial_value = if !is_in_parameter_bindings || var.is_function_name {
                    let value = gen.allocate_register();
                    let undef = gen.add_constant_undefined();
                    gen.emit_mov(&value, &undef);
                    value
                } else if let Some(local_binding) = var.local {
                    let local = var_local_operand(gen, local_binding.local_type, local_binding.index);
                    let value = gen.allocate_register();
                    gen.emit_mov(&value, &local);
                    value
                } else {
                    let id = gen.intern_identifier(&var.name);
                    let value = gen.allocate_register();
                    gen.emit(Instruction::GetBinding {
                        dst: value.operand(),
                        identifier: id,
                        cache: EnvironmentCoordinate::empty(),
                    });
                    value
                };

                if let Some(local_binding) = var.local {
                    let local = var_local_operand(gen, local_binding.local_type, local_binding.index);
                    gen.emit_mov(&local, &initial_value);
                } else {
                    let id = gen.intern_identifier(&var.name);
                    gen.emit(Instruction::CreateVariable {
                        identifier: id,
                        mode: EnvironmentMode::Var as u32,
                        is_immutable: false,
                        is_global: false,
                        is_strict: false,
                    });
                    gen.emit(Instruction::InitializeVariableBinding {
                        identifier: id,
                        src: initial_value.operand(),
                        cache: EnvironmentCoordinate::empty(),
                    });
                }
            }
        }
    }

    // --- Step 6: AnnexB function name bindings (non-strict only) ---
    if !strict {
        let var_names = function_scope_data.map(|fsd| &fsd.var_names);
        for name in &body_scope.annexb_function_names {
            gen.annexb_function_names.insert(name.clone());
            // Skip creating a var binding if this name is already declared as a var.
            if var_names.is_some_and(|names| names.contains(name)) {
                continue;
            }
            let id = gen.intern_identifier(name);
            gen.emit(Instruction::CreateVariable {
                identifier: id,
                mode: EnvironmentMode::Var as u32,
                is_immutable: false,
                is_global: false,
                is_strict: false,
            });
            let undef = gen.add_constant_undefined();
            gen.emit(Instruction::InitializeVariableBinding {
                identifier: id,
                src: undef.operand(),
                cache: EnvironmentCoordinate::empty(),
            });
        }
    }

    // --- Step 7: Lexical environment for non-local declarations ---
    // Note: this counts only let/const/class declarations (not function declarations,
    // which are var-hoisted in function bodies). Matches C++ has_non_local_lexical_declarations().
    let lex_bindings_count = count_non_local_lexical_bindings(body_scope);

    if !strict && lex_bindings_count > 0 {
        gen.push_new_lexical_environment(lex_bindings_count);
    }

    // --- Step 8: Create lexical bindings ---

    for child in &body_scope.children {
        match &child.inner {
            StatementKind::VariableDeclaration { kind, declarations } => {
                if *kind == DeclarationKind::Let || *kind == DeclarationKind::Const {
                    let is_constant = *kind == DeclarationKind::Const;
                    for declaration in declarations {
                        let mut names = Vec::new();
                        collect_target_names(&declaration.target, &mut names);
                        for (name, _) in names {
                            let id = gen.intern_identifier(&name);
                            gen.emit(Instruction::CreateVariable {
                                identifier: id,
                                mode: EnvironmentMode::Lexical as u32,
                                is_immutable: is_constant,
                                is_global: false,
                                is_strict: is_constant,
                            });
                        }
                    }
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                // Class declarations are lexically scoped (like const).
                if let Some(ref name_ident) = class_data.name {
                    if !name_ident.is_local() {
                        let id = gen.intern_identifier(&name_ident.name);
                        gen.emit(Instruction::CreateVariable {
                            identifier: id,
                            mode: EnvironmentMode::Lexical as u32,
                            is_immutable: false,
                            is_global: false,
                            is_strict: false,
                        });
                    }
                }
            }
            _ => {}
        }
    }

    // --- Step 9: Initialize hoisted function declarations ---

    if let Some(fsd) = function_scope_data {
        for function_to_init in &fsd.functions_to_initialize {
            let child = &body_scope.children[function_to_init.child_index];
            if let StatementKind::FunctionDeclaration { function_id, ref name, .. } = child.inner {
                let inner_function_data = gen.function_table.take(function_id);
                let sfd_index = emit_new_function(gen, inner_function_data, None);

                // Check if the function name identifier is local.
                if let Some(ref name_ident) = name {
                    if name_ident.is_local() {
                        let local_index = name_ident.local_index.get();
                        let local = gen.local(local_index);
                        gen.emit(Instruction::NewFunction {
                            dst: local.operand(),
                            shared_function_data_index: sfd_index,
                            home_object: None,
                            lhs_name: None,
                        });
                        gen.mark_local_initialized(local_index);
                    } else {
                        let function_reg = gen.allocate_register();
                        gen.emit(Instruction::NewFunction {
                            dst: function_reg.operand(),
                            shared_function_data_index: sfd_index,
                            home_object: None,
                            lhs_name: None,
                        });
                        let id = gen.intern_identifier(&name_ident.name);
                        gen.emit(Instruction::SetVariableBinding {
                            identifier: id,
                            src: function_reg.operand(),
                            cache: EnvironmentCoordinate::empty(),
                        });
                    }
                }
            }
        }
    }
}

/// Check if a statement is a for-loop variant (for, for-in, for-of, for-await-of).
fn is_for_loop(statement: &Statement) -> bool {
    matches!(
        statement.inner,
        StatementKind::For { .. }
            | StatementKind::ForInOf { .. }
    )
}

/// Check if a block needs block declaration instantiation.
/// True when the block has function declarations or non-local let/const/class.
fn needs_block_declaration_instantiation(scope: &ScopeData) -> bool {
    for child in &scope.children {
        match &child.inner {
            StatementKind::FunctionDeclaration { .. } => {
                return true;
            }
            StatementKind::VariableDeclaration { kind, declarations } => {
                if *kind == DeclarationKind::Let || *kind == DeclarationKind::Const {
                    for declaration in declarations {
                        let mut names = Vec::new();
                        collect_target_names(&declaration.target, &mut names);
                        if !names.is_empty() {
                            return true;
                        }
                    }
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(ref name_ident) = class_data.name {
                    if !name_ident.is_local() {
                        return true;
                    }
                }
            }
            _ => {}
        }
    }
    false
}

/// Count non-local lexical bindings in a function body scope.
fn count_non_local_lexical_bindings(scope: &ScopeData) -> u32 {
    let mut count = 0u32;
    for child in &scope.children {
        match &child.inner {
            StatementKind::VariableDeclaration { kind, declarations } => {
                if *kind == DeclarationKind::Let || *kind == DeclarationKind::Const {
                    for declaration in declarations {
                        let mut names = Vec::new();
                        collect_target_names(&declaration.target, &mut names);
                        count += u32_from_usize(names.len());
                    }
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if class_data.name.as_ref().is_some_and(|n| !n.is_local()) {
                    count += 1;
                }
            }
            _ => {}
        }
    }
    count
}

/// Create a ScopedOperand for a VarToInit's local variable (argument or variable).
fn var_local_operand(gen: &mut Generator, local_type: LocalType, index: u32) -> ScopedOperand {
    gen.resolve_local(index, local_type)
}

/// Collect bound names from a binding pattern into the parameter_names list.
fn collect_binding_pattern_names(
    pattern: &BindingPattern,
    parameter_names: &mut Vec<FdiParameterName>,
    seen_names: &mut HashSet<Utf16String>,
    has_duplicates: &mut bool,
) {
    for entry in &pattern.entries {
        // The bound name can be in the alias (for object patterns) or name (for array patterns).
        match &entry.alias {
            Some(BindingEntryAlias::Identifier(ident)) => {
                let name = ident.name.clone();
                let is_local = ident.is_local();
                if !seen_names.insert(name.clone()) {
                    *has_duplicates = true;
                } else {
                    parameter_names.push(FdiParameterName { name, is_local });
                }
            }
            Some(BindingEntryAlias::BindingPattern(sub_pattern)) => {
                collect_binding_pattern_names(sub_pattern, parameter_names, seen_names, has_duplicates);
            }
            None => {
                // No alias — the name itself is the binding.
                if let Some(BindingEntryName::Identifier(ident)) = &entry.name {
                    let name = ident.name.clone();
                    let is_local = ident.is_local();
                    if !seen_names.insert(name.clone()) {
                        *has_duplicates = true;
                    } else {
                        parameter_names.push(FdiParameterName { name, is_local });
                    }
                }
            }
            Some(BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}

/// A parameter name with its locality (used during FDI).
struct FdiParameterName {
    name: Utf16String,
    is_local: bool,
}

// Builtin IDs matching JS_ENUMERATE_BUILTINS in Builtins.h.
const BUILTIN_MATH_ABS: u8 = 0;
const BUILTIN_MATH_LOG: u8 = 1;
const BUILTIN_MATH_POW: u8 = 2;
const BUILTIN_MATH_EXP: u8 = 3;
const BUILTIN_MATH_CEIL: u8 = 4;
const BUILTIN_MATH_FLOOR: u8 = 5;
const BUILTIN_MATH_IMUL: u8 = 6;
const BUILTIN_MATH_RANDOM: u8 = 7;
const BUILTIN_MATH_ROUND: u8 = 8;
const BUILTIN_MATH_SQRT: u8 = 9;
const BUILTIN_MATH_SIN: u8 = 10;
const BUILTIN_MATH_COS: u8 = 11;
const BUILTIN_MATH_TAN: u8 = 12;
const BUILTIN_REGEXP_PROTOTYPE_EXEC: u8 = 13;
const BUILTIN_REGEXP_PROTOTYPE_REPLACE: u8 = 14;
const BUILTIN_REGEXP_PROTOTYPE_SPLIT: u8 = 15;
const BUILTIN_ORDINARY_HAS_INSTANCE: u8 = 16;
const BUILTIN_ARRAY_ITERATOR_PROTOTYPE_NEXT: u8 = 17;
const BUILTIN_MAP_ITERATOR_PROTOTYPE_NEXT: u8 = 18;
const BUILTIN_SET_ITERATOR_PROTOTYPE_NEXT: u8 = 19;
const BUILTIN_STRING_ITERATOR_PROTOTYPE_NEXT: u8 = 20;

/// Detect known builtin methods from a callee expression (e.g. Math.abs).
/// Returns the Builtin enum value as u8, matching C++ Builtins.h ordering.
fn get_builtin(callee: &Expression) -> Option<u8> {
    let ExpressionKind::Member { object, property, computed } = &callee.inner else {
        return None;
    };
    if *computed {
        return None;
    }
    let ExpressionKind::Identifier(base_ident) = &object.inner else {
        return None;
    };
    let ExpressionKind::Identifier(property_ident) = &property.inner else {
        return None;
    };
    // Must match JS_ENUMERATE_BUILTINS order in Builtins.h.
    static BUILTINS: &[(&[u16], &[u16], u8)] = &[
        (utf16!("Math"), utf16!("abs"), BUILTIN_MATH_ABS),
        (utf16!("Math"), utf16!("log"), BUILTIN_MATH_LOG),
        (utf16!("Math"), utf16!("pow"), BUILTIN_MATH_POW),
        (utf16!("Math"), utf16!("exp"), BUILTIN_MATH_EXP),
        (utf16!("Math"), utf16!("ceil"), BUILTIN_MATH_CEIL),
        (utf16!("Math"), utf16!("floor"), BUILTIN_MATH_FLOOR),
        (utf16!("Math"), utf16!("imul"), BUILTIN_MATH_IMUL),
        (utf16!("Math"), utf16!("random"), BUILTIN_MATH_RANDOM),
        (utf16!("Math"), utf16!("round"), BUILTIN_MATH_ROUND),
        (utf16!("Math"), utf16!("sqrt"), BUILTIN_MATH_SQRT),
        (utf16!("Math"), utf16!("sin"), BUILTIN_MATH_SIN),
        (utf16!("Math"), utf16!("cos"), BUILTIN_MATH_COS),
        (utf16!("Math"), utf16!("tan"), BUILTIN_MATH_TAN),
        (utf16!("RegExpPrototype"), utf16!("exec"), BUILTIN_REGEXP_PROTOTYPE_EXEC),
        (utf16!("RegExpPrototype"), utf16!("replace"), BUILTIN_REGEXP_PROTOTYPE_REPLACE),
        (utf16!("RegExpPrototype"), utf16!("split"), BUILTIN_REGEXP_PROTOTYPE_SPLIT),
        (utf16!("InternalBuiltin"), utf16!("ordinary_has_instance"), BUILTIN_ORDINARY_HAS_INSTANCE),
        (utf16!("ArrayIteratorPrototype"), utf16!("next"), BUILTIN_ARRAY_ITERATOR_PROTOTYPE_NEXT),
        (utf16!("MapIteratorPrototype"), utf16!("next"), BUILTIN_MAP_ITERATOR_PROTOTYPE_NEXT),
        (utf16!("SetIteratorPrototype"), utf16!("next"), BUILTIN_SET_ITERATOR_PROTOTYPE_NEXT),
        (utf16!("StringIteratorPrototype"), utf16!("next"), BUILTIN_STRING_ITERATOR_PROTOTYPE_NEXT),
    ];
    for &(base, property, id) in BUILTINS {
        if base_ident.name == base && property_ident.name == property {
            return Some(id);
        }
    }
    None
}

fn builtin_argument_count(builtin: u8) -> usize {
    // Must match JS_ENUMERATE_BUILTINS argument counts in Builtins.h.
    match builtin {
        BUILTIN_MATH_ABS => 1,
        BUILTIN_MATH_LOG => 1,
        BUILTIN_MATH_POW => 2,
        BUILTIN_MATH_EXP => 1,
        BUILTIN_MATH_CEIL => 1,
        BUILTIN_MATH_FLOOR => 1,
        BUILTIN_MATH_IMUL => 2,
        BUILTIN_MATH_RANDOM => 0,
        BUILTIN_MATH_ROUND => 1,
        BUILTIN_MATH_SQRT => 1,
        BUILTIN_MATH_SIN => 1,
        BUILTIN_MATH_COS => 1,
        BUILTIN_MATH_TAN => 1,
        BUILTIN_REGEXP_PROTOTYPE_EXEC => 1,
        BUILTIN_REGEXP_PROTOTYPE_REPLACE => 2,
        BUILTIN_REGEXP_PROTOTYPE_SPLIT => 2,
        BUILTIN_ORDINARY_HAS_INSTANCE => 1,
        BUILTIN_ARRAY_ITERATOR_PROTOTYPE_NEXT => 0,
        BUILTIN_MAP_ITERATOR_PROTOTYPE_NEXT => 0,
        BUILTIN_SET_ITERATOR_PROTOTYPE_NEXT => 0,
        BUILTIN_STRING_ITERATOR_PROTOTYPE_NEXT => 0,
        _ => usize::MAX,
    }
}

/// JS ToInt32 conversion (ECMA-262 7.1.6).
fn to_int32(n: f64) -> i32 {
    if n.is_nan() || n.is_infinite() || n == 0.0 {
        return 0;
    }
    let int_val = n.signum() * n.abs().floor();
    let int32bit = int_val % 4294967296.0; // 2^32
    let int32bit = if int32bit < 0.0 { int32bit + 4294967296.0 } else { int32bit };
    if int32bit >= 2147483648.0 {
        (int32bit - 4294967296.0) as i32
    } else {
        int32bit as i32
    }
}

/// JS ToUint32 conversion (ECMA-262 7.1.7).
fn to_u32(n: f64) -> u32 {
    if n.is_nan() || n.is_infinite() || n == 0.0 {
        return 0;
    }
    let int_val = n.signum() * n.abs().floor();
    let int32bit = int_val % 4294967296.0; // 2^32
    if int32bit < 0.0 { (int32bit + 4294967296.0) as u32 } else { int32bit as u32 }
}

/// Check if a string key is a numeric index (valid u32 < u32::MAX).
/// Numeric indices are stored in indexed storage rather than shape-based storage.
fn is_numeric_index_key(key: &[u16]) -> bool {
    if key.is_empty() {
        return false;
    }
    // Exclude leading zeros in multi-digit strings (e.g., "01")
    if key[0] == ch(b'0') && key.len() > 1 {
        return false;
    }
    // Must be all ASCII digits
    if !key.iter().all(|&c| c >= ch(b'0') && c <= ch(b'9')) {
        return false;
    }
    // Parse as u64 first to avoid overflow, then check < u32::MAX
    let mut n: u64 = 0;
    for &c in key {
        n = n * 10 + (c - ch(b'0')) as u64;
        if n > u32::MAX as u64 {
            return false;
        }
    }
    (n as u32) < u32::MAX
}

// =============================================================================
// Constant folding
// =============================================================================

/// Try to constant-fold a unary operation when the operand is a constant.
fn try_constant_fold_unary(
    gen: &mut Generator,
    op: UnaryOp,
    operand: &ScopedOperand,
) -> Option<ScopedOperand> {
    let constant = gen.get_constant(operand)?;
    match op {
        UnaryOp::Minus => {
            let n = constant_to_number(constant)?;
            Some(gen.add_constant_number(-n))
        }
        UnaryOp::Plus => {
            let n = constant_to_number(constant)?;
            Some(gen.add_constant_number(n))
        }
        UnaryOp::BitwiseNot => {
            if let ConstantValue::BigInt(s) = constant {
                let n = parse_bigint(&s.clone())?;
                return Some(gen.add_constant_bigint((-(n + BigInt::one())).to_string()));
            }
            let n = constant_to_number(constant)?;
            Some(gen.add_constant_number((!to_int32(n)) as f64))
        }
        UnaryOp::Not => {
            let as_bool = constant_to_boolean(constant)?;
            Some(gen.add_constant_boolean(!as_bool))
        }
        _ => None,
    }
}

/// Constant-fold !!x when x is a constant, returning Boolean(x).
fn try_constant_fold_to_boolean(
    gen: &mut Generator,
    operand: &ScopedOperand,
) -> Option<ScopedOperand> {
    let constant = gen.get_constant(operand)?;
    let as_bool = constant_to_boolean(constant)?;
    Some(gen.add_constant_boolean(as_bool))
}

/// Implement IsLooselyEqual for constant values.
/// https://tc39.es/ecma262/#sec-islooselyequal
fn try_constant_loosely_equals(lhs: &ConstantValue, rhs: &ConstantValue) -> Option<bool> {
    // Same type: use strict equality rules.
    match (lhs, rhs) {
        (ConstantValue::Null, ConstantValue::Null)
        | (ConstantValue::Null, ConstantValue::Undefined)
        | (ConstantValue::Undefined, ConstantValue::Null)
        | (ConstantValue::Undefined, ConstantValue::Undefined) => return Some(true),
        (ConstantValue::Null, _) | (ConstantValue::Undefined, _)
        | (_, ConstantValue::Null) | (_, ConstantValue::Undefined) => return Some(false),
        (ConstantValue::Number(a), ConstantValue::Number(b)) => return Some(a == b),
        (ConstantValue::String(a), ConstantValue::String(b)) => return Some(a == b),
        (ConstantValue::Boolean(a), ConstantValue::Boolean(b)) => return Some(a == b),
        (ConstantValue::BigInt(a), ConstantValue::BigInt(b)) => return Some(a == b),
        _ => {}
    }
    // Cross-type comparisons: Boolean → Number first, then retry.
    match (lhs, rhs) {
        (ConstantValue::Boolean(b), _) => {
            let coerced = ConstantValue::Number(if *b { 1.0 } else { 0.0 });
            return try_constant_loosely_equals(&coerced, rhs);
        }
        (_, ConstantValue::Boolean(b)) => {
            let coerced = ConstantValue::Number(if *b { 1.0 } else { 0.0 });
            return try_constant_loosely_equals(lhs, &coerced);
        }
        _ => {}
    }
    // Number == String → compare ToNumber(string) to number.
    // String == Number → compare number to ToNumber(string).
    match (lhs, rhs) {
        (ConstantValue::Number(n), ConstantValue::String(s))
        | (ConstantValue::String(s), ConstantValue::Number(n)) => {
            Some(*n == string_to_number(s))
        }
        // BigInt == Number or Number == BigInt: compare mathematical values.
        (ConstantValue::BigInt(b), ConstantValue::Number(n))
        | (ConstantValue::Number(n), ConstantValue::BigInt(b)) => {
            if n.is_nan() || n.is_infinite() {
                return Some(false);
            }
            if n.fract() != 0.0 {
                return Some(false);
            }
            let bi = parse_bigint(b)?;
            // Compare: the number must be a safe integer that equals the BigInt.
            // Only fold if the f64 value fits in i64 range for lossless conversion.
            if *n > i64::MAX as f64 || *n < i64::MIN as f64 {
                return None;
            }
            let n_i64 = *n as i64;
            if n_i64 as f64 != *n {
                return None;
            }
            Some(BigInt::from(n_i64) == bi)
        }
        // BigInt == String or String == BigInt: parse string as BigInt per StringToBigInt.
        // If the string cannot be parsed, the result is false (not equal).
        (ConstantValue::BigInt(b), ConstantValue::String(s))
        | (ConstantValue::String(s), ConstantValue::BigInt(b)) => {
            match string_to_bigint(s) {
                Some(s_bi) => {
                    let bi = parse_bigint(b)?;
                    Some(bi == s_bi)
                }
                None => Some(false),
            }
        }
        _ => None,
    }
}

/// Implements StringToBigInt per https://tc39.es/ecma262/#sec-stringtobigint.
/// Trims whitespace, handles optional sign (decimal only), and handles
/// 0b/0o/0x prefixes. Returns None if the string is not a valid
/// StringIntegerLiteral.
fn string_to_bigint(s: &Utf16String) -> Option<BigInt> {
    use num_bigint::BigInt;
    let s_utf8: String = char::decode_utf16(s.0.iter().copied())
        .map(|r| r.unwrap_or('\u{FFFD}'))
        .collect();
    let s_trimmed = s_utf8.trim();
    if s_trimmed.is_empty() {
        return Some(BigInt::from(0));
    }
    // Check for non-decimal prefixes (no sign allowed).
    if s_trimmed.len() > 2 {
        let (prefix, rest) = s_trimmed.split_at(2);
        match prefix {
            "0b" | "0B" => return BigInt::parse_bytes(rest.as_bytes(), 2),
            "0o" | "0O" => return BigInt::parse_bytes(rest.as_bytes(), 8),
            "0x" | "0X" => return BigInt::parse_bytes(rest.as_bytes(), 16),
            _ => {}
        }
    }
    // Decimal with optional sign. Only allow digits (no dots, no exponents).
    let (is_negative, digits) = if let Some(rest) = s_trimmed.strip_prefix('-') {
        (true, rest)
    } else if let Some(rest) = s_trimmed.strip_prefix('+') {
        (false, rest)
    } else {
        (false, s_trimmed)
    };
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let bi = BigInt::parse_bytes(digits.as_bytes(), 10)?;
    Some(if is_negative { -bi } else { bi })
}

/// Constant-fold a binary operation on two BigInt operands.
fn try_constant_fold_bigint_binary(
    gen: &mut Generator,
    op: BinaryOp,
    a_str: &str,
    b_str: &str,
) -> Option<ScopedOperand> {
    let a = parse_bigint(a_str)?;
    let b = parse_bigint(b_str)?;
    match op {
        // Arithmetic operations: produce a BigInt result.
        BinaryOp::Addition => {
            Some(gen.add_constant_bigint((&a + &b).to_string()))
        }
        BinaryOp::Subtraction => {
            Some(gen.add_constant_bigint((&a - &b).to_string()))
        }
        BinaryOp::Multiplication => {
            Some(gen.add_constant_bigint((&a * &b).to_string()))
        }
        BinaryOp::Division => {
            if b.is_zero() {
                return None; // Division by zero throws at runtime.
            }
            use num_integer::Integer;
            let (quotient, _) = a.div_rem(&b);
            Some(gen.add_constant_bigint(quotient.to_string()))
        }
        BinaryOp::Modulo => {
            if b.is_zero() {
                return None; // Modulo by zero throws at runtime.
            }
            // JS BigInt remainder has the sign of the dividend (truncated division).
            Some(gen.add_constant_bigint((&a % &b).to_string()))
        }
        BinaryOp::Exponentiation => {
            if b.is_negative() {
                return None; // Negative exponent throws at runtime.
            }
            let exp = b.to_u32()?;
            // Only fold small exponents to avoid huge results.
            if exp > 1000 {
                return None;
            }
            Some(gen.add_constant_bigint(num_traits::pow::pow(a, exp as usize).to_string()))
        }
        // Comparison operations: produce a Boolean result.
        BinaryOp::StrictlyEquals | BinaryOp::LooselyEquals => {
            Some(gen.add_constant_boolean(a == b))
        }
        BinaryOp::StrictlyInequals | BinaryOp::LooselyInequals => {
            Some(gen.add_constant_boolean(a != b))
        }
        BinaryOp::LessThan => {
            Some(gen.add_constant_boolean(a < b))
        }
        BinaryOp::LessThanEquals => {
            Some(gen.add_constant_boolean(a <= b))
        }
        BinaryOp::GreaterThan => {
            Some(gen.add_constant_boolean(a > b))
        }
        BinaryOp::GreaterThanEquals => {
            Some(gen.add_constant_boolean(a >= b))
        }
        // Bitwise operations on BigInt.
        BinaryOp::BitwiseAnd => {
            Some(gen.add_constant_bigint((&a & &b).to_string()))
        }
        BinaryOp::BitwiseOr => {
            Some(gen.add_constant_bigint((&a | &b).to_string()))
        }
        BinaryOp::BitwiseXor => {
            Some(gen.add_constant_bigint((&a ^ &b).to_string()))
        }
        BinaryOp::LeftShift => {
            let shift = b.to_u64()?;
            if shift > 512 {
                return None;
            }
            Some(gen.add_constant_bigint((&a << shift as usize).to_string()))
        }
        BinaryOp::RightShift => {
            let shift = b.to_u64()?;
            // BigInt right shift for negative numbers floors toward negative infinity.
            Some(gen.add_constant_bigint(bigint_right_shift(&a, shift as usize).to_string()))
        }
        // UnsignedRightShift throws TypeError for BigInt.
        _ => None,
    }
}

/// BigInt arithmetic right shift that floors toward negative infinity
/// (matching JS spec 6.1.6.2.9 BigInt::signedRightShift).
fn bigint_right_shift(value: &BigInt, shift: usize) -> BigInt {
    if !value.is_negative() || shift == 0 {
        return value >> shift;
    }
    // For negative values, we need floor division behavior.
    // Check if any of the shifted-out bits are set.
    let divisor = BigInt::one() << shift;
    use num_integer::Integer;
    value.div_floor(&divisor)
}

/// Try to constant-fold a binary operation when both operands are constants.
// 6.1.6.1.3 Number::exponentiate ( base, exponent )
// https://tc39.es/ecma262/#sec-numeric-types-number-exponentiate
// Rust's f64::powf follows C's pow() which returns 1.0 for pow(±1, ±∞),
// but JS specifies NaN when abs(base) is 1 and exponent is ±∞.
fn js_exponentiate(base: f64, exponent: f64) -> f64 {
    if exponent.is_infinite() && base.abs() == 1.0 {
        return f64::NAN;
    }
    base.powf(exponent)
}

/// Parse a non-decimal integer string via BigInt to f64, matching C++
/// UnsignedBigInteger::from_base() + to_double(). This avoids u64 overflow
/// for large literals like 0x10000000000000000.
fn bigint_string_to_f64(s: &str, radix: u32) -> f64 {
    use num_bigint::BigUint;
    match BigUint::parse_bytes(s.as_bytes(), radix) {
        Some(n) => {
            use num_traits::ToPrimitive;
            n.to_f64().unwrap_or(f64::INFINITY)
        }
        None => f64::NAN,
    }
}

// 7.1.4.1.1 StringToNumber ( str ), https://tc39.es/ecma262/#sec-stringtonumber
fn string_to_number(s: &Utf16String) -> f64 {
    let text: String = char::decode_utf16(s.0.iter().copied())
        .map(|r| r.unwrap_or('\u{FFFD}'))
        .collect();
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return 0.0;
    }
    if trimmed == "Infinity" || trimmed == "+Infinity" {
        return f64::INFINITY;
    }
    if trimmed == "-Infinity" {
        return f64::NEG_INFINITY;
    }
    if trimmed.len() > 2 {
        let (prefix, rest) = trimmed.split_at(2);
        match prefix {
            "0b" | "0B" => {
                return if rest.bytes().all(|b| b == b'0' || b == b'1') {
                    bigint_string_to_f64(rest, 2)
                } else {
                    f64::NAN
                };
            }
            "0o" | "0O" => {
                return if rest.bytes().all(|b| b.is_ascii_digit() && b < b'8') {
                    bigint_string_to_f64(rest, 8)
                } else {
                    f64::NAN
                };
            }
            "0x" | "0X" => {
                return if rest.bytes().all(|b| b.is_ascii_hexdigit()) {
                    bigint_string_to_f64(rest, 16)
                } else {
                    f64::NAN
                };
            }
            _ => {}
        }
    }
    if !trimmed
        .bytes()
        .all(|b| b.is_ascii_digit() || b == b'.' || b == b'e' || b == b'E' || b == b'+' || b == b'-')
    {
        return f64::NAN;
    }
    trimmed.parse::<f64>().unwrap_or(f64::NAN)
}

/// Convert a constant value to a JS number for constant folding purposes.
fn constant_to_number(val: &ConstantValue) -> Option<f64> {
    match val {
        ConstantValue::Number(n) => Some(*n),
        ConstantValue::Boolean(b) => Some(if *b { 1.0 } else { 0.0 }),
        ConstantValue::Null => Some(0.0),
        ConstantValue::Undefined => Some(f64::NAN),
        ConstantValue::String(s) => Some(string_to_number(s)),
        _ => None,
    }
}

/// Convert a constant value to a JS string (ToString) for string concatenation.
/// Returns None for types where ToString requires runtime (e.g. objects).
fn constant_to_string(val: &ConstantValue) -> Option<Utf16String> {
    match val {
        ConstantValue::String(s) => Some(s.clone()),
        ConstantValue::Number(n) => Some(super::ffi::js_number_to_utf16(*n)),
        ConstantValue::Boolean(b) => {
            Some(if *b { Utf16String(utf16!("true").to_vec()) } else { Utf16String(utf16!("false").to_vec()) })
        }
        ConstantValue::Null => Some(Utf16String(utf16!("null").to_vec())),
        ConstantValue::Undefined => Some(Utf16String(utf16!("undefined").to_vec())),
        ConstantValue::BigInt(s) => {
            // BigInt.prototype.toString() returns the decimal string representation.
            Some(Utf16String(s.encode_utf16().collect()))
        }
        _ => None,
    }
}

/// Compare a BigInt and a Number using Abstract Relational Comparison semantics.
/// Returns None if comparison cannot be folded (non-safe integer, etc).
/// Returns Some(None) for NaN (undefined result - all comparisons return false).
/// Returns Some(Some(Ordering)) for the BigInt relative to the Number.
fn compare_bigint_and_number(bigint_str: &str, number: f64) -> Option<Option<std::cmp::Ordering>> {
    use std::cmp::Ordering;
    if number.is_nan() {
        return Some(None); // Comparisons with NaN return undefined → false.
    }
    if number == f64::INFINITY {
        return Some(Some(Ordering::Less)); // Any BigInt < +Infinity
    }
    if number == f64::NEG_INFINITY {
        return Some(Some(Ordering::Greater)); // Any BigInt > -Infinity
    }
    let bigint = parse_bigint(bigint_str)?;
    // Only fold if the f64 value is a safe integer for lossless comparison.
    if number.fract() != 0.0 {
        // The number has a fractional part, so we can still compare:
        // floor(number) < bigint means bigint > number, etc.
        let floored = number.floor();
        if floored > i64::MAX as f64 || floored < i64::MIN as f64 {
            return None;
        }
        let floored_i64 = floored as i64;
        let floored_bigint = BigInt::from(floored_i64);
        if bigint <= floored_bigint {
            // bigint <= floor(number) means bigint < number
            return Some(Some(Ordering::Less));
        } else {
            // bigint > floor(number) means bigint > number (since number = floor + fract where fract > 0)
            return Some(Some(Ordering::Greater));
        }
    }
    if number > i64::MAX as f64 || number < i64::MIN as f64 {
        return None;
    }
    let number_i64 = number as i64;
    if number_i64 as f64 != number {
        return None;
    }
    let number_bigint = BigInt::from(number_i64);
    Some(Some(bigint.cmp(&number_bigint)))
}

fn try_constant_fold_binary(
    gen: &mut Generator,
    op: BinaryOp,
    lhs: &ScopedOperand,
    rhs: &ScopedOperand,
) -> Option<ScopedOperand> {
    let lhs_const = gen.get_constant(lhs)?;
    let rhs_const = gen.get_constant(rhs)?;

    // BigInt constant folding: if both operands are BigInt, handle separately.
    // Clone strings to release the immutable borrow on gen before the mutable call.
    if let (ConstantValue::BigInt(a), ConstantValue::BigInt(b)) = (lhs_const, rhs_const) {
        let a = a.clone();
        let b = b.clone();
        return try_constant_fold_bigint_binary(gen, op, &a, &b);
    }

    match op {
        BinaryOp::Addition => {
            // If either operand is a string, do string concatenation using ToString.
            if matches!(lhs_const, ConstantValue::String(_)) || matches!(rhs_const, ConstantValue::String(_)) {
                let a = constant_to_string(lhs_const)?;
                let b = constant_to_string(rhs_const)?;
                let mut result = a;
                result.0.extend_from_slice(&b);
                return Some(gen.add_constant_string(result));
            }
            // Numeric addition: both operands coerced to number
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number(a + b))
        }
        BinaryOp::Subtraction => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number(a - b))
        }
        BinaryOp::Multiplication => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number(a * b))
        }
        BinaryOp::Division => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number(a / b))
        }
        BinaryOp::Modulo => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number(a % b))
        }
        BinaryOp::Exponentiation => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number(js_exponentiate(a, b)))
        }
        BinaryOp::StrictlyEquals | BinaryOp::StrictlyInequals => {
            let equal = match (lhs_const, rhs_const) {
                (ConstantValue::Number(a), ConstantValue::Number(b)) => a == b,
                (ConstantValue::String(a), ConstantValue::String(b)) => a == b,
                (ConstantValue::Boolean(a), ConstantValue::Boolean(b)) => a == b,
                (ConstantValue::Null, ConstantValue::Null) => true,
                (ConstantValue::Undefined, ConstantValue::Undefined) => true,
                // Different types are never strictly equal.
                _ => false
            };
            let result = if op == BinaryOp::StrictlyInequals { !equal } else { equal };
            Some(gen.add_constant_boolean(result))
        }
        BinaryOp::GreaterThan
        | BinaryOp::GreaterThanEquals
        | BinaryOp::LessThan
        | BinaryOp::LessThanEquals => {
            // String-string comparison is lexicographic (by UTF-16 code units).
            if let (ConstantValue::String(a), ConstantValue::String(b)) = (lhs_const, rhs_const) {
                let result = match op {
                    BinaryOp::GreaterThan => a.0 > b.0,
                    BinaryOp::GreaterThanEquals => a.0 >= b.0,
                    BinaryOp::LessThan => a.0 < b.0,
                    BinaryOp::LessThanEquals => a.0 <= b.0,
                    _ => unreachable!("outer match arm only matches comparison operators"),
                };
                return Some(gen.add_constant_boolean(result));
            }
            // BigInt-Number/Boolean cross-type comparison.
            // Per spec, Booleans are converted to Number first.
            let lhs_for_cmp = match lhs_const {
                ConstantValue::Boolean(b) => &ConstantValue::Number(if *b { 1.0 } else { 0.0 }),
                other => other,
            };
            let rhs_for_cmp = match rhs_const {
                ConstantValue::Boolean(b) => &ConstantValue::Number(if *b { 1.0 } else { 0.0 }),
                other => other,
            };
            // BigInt vs Number comparison using Abstract Relational Comparison.
            let bigint_ord = match (lhs_for_cmp, rhs_for_cmp) {
                (ConstantValue::BigInt(b), ConstantValue::Number(n)) => {
                    compare_bigint_and_number(b, *n)
                }
                (ConstantValue::Number(n), ConstantValue::BigInt(b)) => {
                    compare_bigint_and_number(b, *n).map(|o| o.map(|o| o.reverse()))
                }
                // BigInt vs String: per spec, parse the string as a BigInt
                // using StringToBigInt, then compare the two BigInts.
                // If the string is not a valid StringIntegerLiteral,
                // the result is undefined (= false for all comparisons).
                (ConstantValue::BigInt(b), ConstantValue::String(s)) => {
                    match string_to_bigint(s) {
                        Some(rhs_bi) => {
                            let lhs_bi = parse_bigint(b)?;
                            Some(Some(lhs_bi.cmp(&rhs_bi)))
                        }
                        None => Some(None),
                    }
                }
                (ConstantValue::String(s), ConstantValue::BigInt(b)) => {
                    match string_to_bigint(s) {
                        Some(lhs_bi) => {
                            let rhs_bi = parse_bigint(b)?;
                            Some(Some(lhs_bi.cmp(&rhs_bi)))
                        }
                        None => Some(None),
                    }
                }
                _ => None,
            };
            if let Some(ord) = bigint_ord {
                let result = match op {
                    BinaryOp::GreaterThan => matches!(ord, Some(std::cmp::Ordering::Greater)),
                    BinaryOp::GreaterThanEquals => matches!(ord, Some(std::cmp::Ordering::Greater | std::cmp::Ordering::Equal)),
                    BinaryOp::LessThan => matches!(ord, Some(std::cmp::Ordering::Less)),
                    BinaryOp::LessThanEquals => matches!(ord, Some(std::cmp::Ordering::Less | std::cmp::Ordering::Equal)),
                    _ => unreachable!("outer match arm only matches comparison operators"),
                };
                return Some(gen.add_constant_boolean(result));
            }
            let a = constant_to_number(lhs_for_cmp)?;
            let b = constant_to_number(rhs_for_cmp)?;
            let result = match op {
                BinaryOp::GreaterThan => a > b,
                BinaryOp::GreaterThanEquals => a >= b,
                BinaryOp::LessThan => a < b,
                BinaryOp::LessThanEquals => a <= b,
                _ => unreachable!("outer match arm only matches comparison operators"),
            };
            Some(gen.add_constant_boolean(result))
        }
        BinaryOp::LooselyEquals => {
            let result = try_constant_loosely_equals(lhs_const, rhs_const)?;
            Some(gen.add_constant_boolean(result))
        }
        BinaryOp::LooselyInequals => {
            let result = try_constant_loosely_equals(lhs_const, rhs_const)?;
            Some(gen.add_constant_boolean(!result))
        }
        BinaryOp::BitwiseAnd => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number((to_int32(a) & to_int32(b)) as f64))
        }
        BinaryOp::BitwiseOr => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number((to_int32(a) | to_int32(b)) as f64))
        }
        BinaryOp::BitwiseXor => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number((to_int32(a) ^ to_int32(b)) as f64))
        }
        BinaryOp::LeftShift => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number((to_int32(a) << (to_u32(b) & 0x1f)) as f64))
        }
        BinaryOp::RightShift => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number((to_int32(a) >> (to_u32(b) & 0x1f)) as f64))
        }
        BinaryOp::UnsignedRightShift => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(gen.add_constant_number((to_u32(a) >> (to_u32(b) & 0x1f)) as f64))
        }
        _ => None,
    }
}

// =============================================================================
// NaN-boxing helpers
// =============================================================================

// NanBoxed Value encoding helpers (ABI-compatible with GC::NanBoxedValue).
// Used by NewPrimitiveArray to encode constant primitive values inline.
const NANBOX_TAG_SHIFT: u64 = 48;
const NANBOX_BASE_TAG: u64 = 0x7FF8;
const NANBOX_INT32_TAG: u64 = 0b010 | NANBOX_BASE_TAG;
const NANBOX_BOOLEAN_TAG: u64 = 0b001 | NANBOX_BASE_TAG;
const NANBOX_NULL_TAG: u64 = 0b111 | NANBOX_BASE_TAG;
const NANBOX_EMPTY_TAG: u64 = 0b011 | NANBOX_BASE_TAG;
const NEGATIVE_ZERO_BITS: u64 = 1u64 << 63;

fn nanboxed_number(value: f64) -> u64 {
    let is_negative_zero = value.to_bits() == NEGATIVE_ZERO_BITS;
    if value >= i32::MIN as f64
        && value <= i32::MAX as f64
        && value.trunc() == value
        && !is_negative_zero
    {
        (NANBOX_INT32_TAG << NANBOX_TAG_SHIFT) | ((value as i32 as u32) as u64)
    } else if value.is_nan() {
        // Canon NaN
        0x7FF8_0000_0000_0000u64
    } else {
        value.to_bits()
    }
}

fn nanboxed_boolean(value: bool) -> u64 {
    (NANBOX_BOOLEAN_TAG << NANBOX_TAG_SHIFT) | (value as u64)
}

fn nanboxed_null() -> u64 {
    NANBOX_NULL_TAG << NANBOX_TAG_SHIFT
}

fn nanboxed_empty() -> u64 {
    NANBOX_EMPTY_TAG << NANBOX_TAG_SHIFT
}

// =============================================================================
// Error message utilities
// =============================================================================

/// Intern the base expression as an identifier for error messages like
/// "Cannot access property X on null object Y".
fn intern_base_identifier(gen: &mut Generator, base: &Expression) -> Option<IdentifierTableIndex> {
    expression_identifier(base).map(|s| gen.intern_identifier(&s))
}

/// Try to produce a human-readable name for an expression (for error messages).
/// Returns None for expressions that have no meaningful name.
fn expression_identifier(expression: &Expression) -> Option<Utf16String> {
    match &expression.inner {
        ExpressionKind::Identifier(ident) => Some(ident.name.clone()),
        ExpressionKind::StringLiteral(s) => {
            let mut result = Utf16String(utf16!("'").to_vec());
            result.0.extend_from_slice(s);
            result.0.extend_from_slice(utf16!("'"));
            Some(result)
        }
        ExpressionKind::NumericLiteral(n) => Some(super::ffi::js_number_to_utf16(*n)),
        ExpressionKind::This => Some(Utf16String(utf16!("this").to_vec())),
        ExpressionKind::Member { object, property, computed } => {
            let mut s = Utf16String::new();
            if let Some(obj_id) = expression_identifier(object) {
                s.0.extend_from_slice(&obj_id);
            }
            if let Some(property_id) = expression_identifier(property) {
                if *computed {
                    s.0.extend_from_slice(utf16!("["));
                    s.0.extend_from_slice(&property_id);
                    s.0.extend_from_slice(utf16!("]"));
                } else {
                    s.0.extend_from_slice(utf16!("."));
                    s.0.extend_from_slice(&property_id);
                }
            }
            Some(s)
        }
        _ => None,
    }
}

/// Produce a human-readable string for call expression error messages.
/// Unlike expression_identifier, this always produces output for known types
/// (using "<object>" for unrecognized sub-expressions).
fn expression_string_approximation(expression: &Expression) -> Option<Utf16String> {
    match &expression.inner {
        ExpressionKind::Identifier(ident) => Some(ident.name.clone()),
        ExpressionKind::Member { .. } => Some(member_to_string_approximation(expression)),
        _ => None,
    }
}

fn member_to_string_approximation(expression: &Expression) -> Utf16String {
    match &expression.inner {
        ExpressionKind::Identifier(ident) => ident.name.clone(),
        ExpressionKind::Member { object, property, computed } => {
            let mut s = member_to_string_approximation(object);
            let property_str = member_to_string_approximation(property);
            if *computed {
                s.0.extend_from_slice(utf16!("["));
                s.0.extend_from_slice(&property_str);
                s.0.extend_from_slice(utf16!("]"));
            } else {
                s.0.extend_from_slice(utf16!("."));
                s.0.extend_from_slice(&property_str);
            }
            s
        }
        ExpressionKind::StringLiteral(s) => {
            let mut result = Utf16String(utf16!("'").to_vec());
            result.0.extend_from_slice(s);
            result.0.extend_from_slice(utf16!("'"));
            result
        }
        ExpressionKind::NumericLiteral(n) => {
            let s = format_double_for_display(*n);
            s.encode_utf16().collect()
        }
        ExpressionKind::This => Utf16String(utf16!("this").to_vec()),
        ExpressionKind::PrivateIdentifier(ident) => ident.name.clone(),
        _ => Utf16String(utf16!("<object>").to_vec()),
    }
}

/// Format a double matching AK's `Utf16String::formatted("{}", double)`.
/// Uses ECMA-262 rules: scientific notation when the decimal exponent n
/// satisfies n < -5 or n > 21, otherwise regular decimal notation.
fn format_double_for_display(n: f64) -> String {
    if n.is_nan() {
        return "NaN".to_string();
    }
    if n.is_infinite() {
        return if n > 0.0 { "Infinity" } else { "-Infinity" }.to_string();
    }
    if n == 0.0 {
        return "0".to_string();
    }
    // Get the scientific notation representation to extract the exponent.
    let e_str = format!("{:e}", n);
    if let Some(e_pos) = e_str.find('e') {
        let exp_str = &e_str[e_pos + 1..];
        let displayed_exponent = exp_str.parse::<i32>().unwrap_or(0);
        // AK uses: n < -5 || n > 21 where n = displayed_exponent + 1.
        // Equivalently: displayed_exponent < -6 || displayed_exponent > 20.
        if !(-6..=20).contains(&displayed_exponent) {
            let mantissa_part = &e_str[..e_pos];
            if displayed_exponent < 0 {
                return format!("{}e{}", mantissa_part, displayed_exponent);
            } else {
                return format!("{}e+{}", mantissa_part, displayed_exponent);
            }
        }
    }
    format!("{}", n)
}
