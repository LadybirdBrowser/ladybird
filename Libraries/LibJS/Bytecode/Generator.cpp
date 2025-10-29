/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/TemporaryChange.h>
#include <LibJS/AST.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Bytecode {

Generator::Generator(VM& vm, GC::Ptr<ECMAScriptFunctionObject const> function, MustPropagateCompletion must_propagate_completion)
    : m_vm(vm)
    , m_string_table(make<StringTable>())
    , m_identifier_table(make<IdentifierTable>())
    , m_regex_table(make<RegexTable>())
    , m_constants(vm.heap())
    , m_accumulator(*this, Operand(Register::accumulator()))
    , m_this_value(*this, Operand(Register::this_value()))
    , m_must_propagate_completion(must_propagate_completion == MustPropagateCompletion::Yes)
    , m_function(function)
{
}

CodeGenerationErrorOr<void> Generator::emit_function_declaration_instantiation(ECMAScriptFunctionObject const& function)
{
    if (function.shared_data().m_has_parameter_expressions) {
        bool has_non_local_parameters = false;
        for (auto const& parameter_name : function.shared_data().m_parameter_names) {
            if (parameter_name.value == SharedFunctionInstanceData::ParameterIsLocal::No) {
                has_non_local_parameters = true;
                break;
            }
        }
        if (has_non_local_parameters)
            emit<Op::CreateLexicalEnvironment>();
    }

    for (auto const& parameter_name : function.shared_data().m_parameter_names) {
        if (parameter_name.value == SharedFunctionInstanceData::ParameterIsLocal::No) {
            auto id = intern_identifier(parameter_name.key);
            emit<Op::CreateVariable>(id, Op::EnvironmentMode::Lexical, false);
            if (function.shared_data().m_has_duplicates) {
                emit<Op::InitializeLexicalBinding>(id, add_constant(js_undefined()));
            }
        }
    }

    if (function.shared_data().m_arguments_object_needed) {
        Optional<Operand> dst;
        auto local_var_index = function.shared_data().m_local_variables_names.find_first_index_if([](auto const& local) { return local.declaration_kind == LocalVariable::DeclarationKind::ArgumentsObject; });
        if (local_var_index.has_value())
            dst = local(Identifier::Local::variable(local_var_index.value()));

        if (function.is_strict_mode() || !function.has_simple_parameter_list()) {
            emit<Op::CreateArguments>(dst, Op::CreateArguments::Kind::Unmapped, function.is_strict_mode());
        } else {
            emit<Op::CreateArguments>(dst, Op::CreateArguments::Kind::Mapped, function.is_strict_mode());
        }

        if (local_var_index.has_value())
            set_local_initialized(Identifier::Local::variable(local_var_index.value()));
    }

    auto const& formal_parameters = function.formal_parameters();
    for (u32 param_index = 0; param_index < formal_parameters.size(); ++param_index) {
        auto const& parameter = formal_parameters.parameters()[param_index];

        if (parameter.is_rest) {
            emit<Op::CreateRestParams>(Operand { Operand::Type::Argument, param_index }, param_index);
        } else if (parameter.default_value) {
            auto& if_undefined_block = make_block();
            auto& if_not_undefined_block = make_block();

            emit<Op::JumpUndefined>(
                Operand { Operand::Type::Argument, param_index },
                Label { if_undefined_block },
                Label { if_not_undefined_block });

            switch_to_basic_block(if_undefined_block);
            auto operand = TRY(parameter.default_value->generate_bytecode(*this));
            emit<Op::Mov>(Operand { Operand::Type::Argument, param_index }, *operand);
            emit<Op::Jump>(Label { if_not_undefined_block });

            switch_to_basic_block(if_not_undefined_block);
        }

        if (auto const* identifier = parameter.binding.get_pointer<NonnullRefPtr<Identifier const>>(); identifier) {
            if ((*identifier)->is_local()) {
                set_local_initialized((*identifier)->local_index());
            } else {
                auto id = intern_identifier((*identifier)->string());
                if (function.shared_data().m_has_duplicates) {
                    emit<Op::SetLexicalBinding>(id, Operand { Operand::Type::Argument, param_index });
                } else {
                    emit<Op::InitializeLexicalBinding>(id, Operand { Operand::Type::Argument, param_index });
                }
            }
        } else if (auto const* binding_pattern = parameter.binding.get_pointer<NonnullRefPtr<BindingPattern const>>(); binding_pattern) {
            ScopedOperand argument { *this, Operand { Operand::Type::Argument, param_index } };
            auto init_mode = function.shared_data().m_has_duplicates ? Op::BindingInitializationMode::Set : Bytecode::Op::BindingInitializationMode::Initialize;
            TRY((*binding_pattern)->generate_bytecode(*this, init_mode, argument));
        }
    }

    ScopeNode const* scope_body = nullptr;
    if (is<ScopeNode>(function.ecmascript_code()))
        scope_body = &static_cast<ScopeNode const&>(function.ecmascript_code());

    if (!function.shared_data().m_has_parameter_expressions) {
        if (scope_body) {
            for (auto const& variable_to_initialize : function.shared_data().m_var_names_to_initialize_binding) {
                auto const& id = variable_to_initialize.identifier;
                if (id.is_local()) {
                    emit<Op::Mov>(local(id.local_index()), add_constant(js_undefined()));
                } else {
                    auto intern_id = intern_identifier(id.string());
                    emit<Op::CreateVariable>(intern_id, Op::EnvironmentMode::Var, false);
                    emit<Op::InitializeVariableBinding>(intern_id, add_constant(js_undefined()));
                }
            }
        }
    } else {
        bool has_non_local_parameters = false;
        if (scope_body) {
            for (auto const& variable_to_initialize : function.shared_data().m_var_names_to_initialize_binding) {
                auto const& id = variable_to_initialize.identifier;
                if (!id.is_local()) {
                    has_non_local_parameters = true;
                    break;
                }
            }
        }

        if (has_non_local_parameters)
            emit<Op::CreateVariableEnvironment>(function.shared_data().m_var_environment_bindings_count);

        if (scope_body) {
            for (auto const& variable_to_initialize : function.shared_data().m_var_names_to_initialize_binding) {
                auto const& id = variable_to_initialize.identifier;
                auto initial_value = allocate_register();
                if (!variable_to_initialize.parameter_binding || variable_to_initialize.function_name) {
                    emit<Op::Mov>(initial_value, add_constant(js_undefined()));
                } else {
                    if (id.is_local()) {
                        emit<Op::Mov>(initial_value, local(id.local_index()));
                    } else {
                        emit<Op::GetBinding>(initial_value, intern_identifier(id.string()));
                    }
                }

                if (id.is_local()) {
                    emit<Op::Mov>(local(id.local_index()), initial_value);
                } else {
                    auto intern_id = intern_identifier(id.string());
                    emit<Op::CreateVariable>(intern_id, Op::EnvironmentMode::Var, false);
                    emit<Op::InitializeVariableBinding>(intern_id, initial_value);
                }
            }
        }
    }

    if (!function.is_strict_mode() && scope_body) {
        for (auto const& function_name : function.shared_data().m_function_names_to_initialize_binding) {
            auto intern_id = intern_identifier(function_name);
            emit<Op::CreateVariable>(intern_id, Op::EnvironmentMode::Var, false);
            emit<Op::InitializeVariableBinding>(intern_id, add_constant(js_undefined()));
        }
    }

    if (!function.is_strict_mode()) {
        bool can_elide_lexical_environment = !scope_body || !scope_body->has_non_local_lexical_declarations();
        if (!can_elide_lexical_environment) {
            emit<Op::CreateLexicalEnvironment>(OptionalNone {}, function.shared_data().m_lex_environment_bindings_count);
        }
    }

    if (scope_body) {
        MUST(scope_body->for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
            MUST(declaration.for_each_bound_identifier([&](auto const& id) {
                if (id.is_local()) {
                    return;
                }

                emit<Op::CreateVariable>(intern_identifier(id.string()),
                    Op::EnvironmentMode::Lexical,
                    declaration.is_constant_declaration(),
                    false,
                    declaration.is_constant_declaration());
            }));
        }));
    }

    for (auto const& declaration : function.shared_data().m_functions_to_initialize) {
        auto const& identifier = *declaration.name_identifier();
        if (identifier.is_local()) {
            auto local_index = identifier.local_index();
            emit<Op::NewFunction>(local(local_index), declaration, OptionalNone {});
            set_local_initialized(local_index);
        } else {
            auto function = allocate_register();
            emit<Op::NewFunction>(function, declaration, OptionalNone {});
            emit<Op::SetVariableBinding>(intern_identifier(declaration.name()), function);
        }
    }

    return {};
}

CodeGenerationErrorOr<GC::Ref<Executable>> Generator::compile(VM& vm, ASTNode const& node, FunctionKind enclosing_function_kind, GC::Ptr<ECMAScriptFunctionObject const> function, MustPropagateCompletion must_propagate_completion, Vector<LocalVariable> local_variable_names)
{
    Generator generator(vm, function, must_propagate_completion);

    if (is<Program>(node))
        generator.m_strict = static_cast<Program const&>(node).is_strict_mode() ? Strict::Yes : Strict::No;
    else if (is<FunctionBody>(node))
        generator.m_strict = static_cast<FunctionBody const&>(node).in_strict_mode() ? Strict::Yes : Strict::No;
    else if (is<FunctionDeclaration>(node))
        generator.m_strict = static_cast<FunctionDeclaration const&>(node).is_strict_mode() ? Strict::Yes : Strict::No;

    generator.m_local_variables = local_variable_names;

    generator.switch_to_basic_block(generator.make_block());
    SourceLocationScope scope(generator, node);
    generator.m_enclosing_function_kind = enclosing_function_kind;
    if (generator.is_in_async_function() && !generator.is_in_generator_function()) {
        // Immediately yield with no value.
        auto& start_block = generator.make_block();
        generator.emit<Bytecode::Op::Yield>(Label { start_block }, generator.add_constant(js_undefined()));
        generator.switch_to_basic_block(start_block);
        // NOTE: This doesn't have to handle received throw/return completions, as GeneratorObject::resume_abrupt
        //       will not enter the generator from the SuspendedStart state and immediately completes the generator.
    }

    if (function)
        TRY(generator.emit_function_declaration_instantiation(*function));

    if (generator.is_in_generator_function()) {
        // Immediately yield with no value.
        auto& start_block = generator.make_block();
        generator.emit<Bytecode::Op::Yield>(Label { start_block }, generator.add_constant(js_undefined()));
        generator.switch_to_basic_block(start_block);
        // NOTE: This doesn't have to handle received throw/return completions, as GeneratorObject::resume_abrupt
        //       will not enter the generator from the SuspendedStart state and immediately completes the generator.
    }

    auto last_value = TRY(node.generate_bytecode(generator));

    if (!generator.current_block().is_terminated() && last_value.has_value()) {
        generator.emit<Bytecode::Op::End>(last_value.value());
    }

    if (generator.is_in_generator_or_async_function()) {
        // Terminate all unterminated blocks with yield return
        for (auto& block : generator.m_root_basic_blocks) {
            if (block->is_terminated())
                continue;
            generator.switch_to_basic_block(*block);
            generator.emit_return<Bytecode::Op::Yield>(generator.add_constant(js_undefined()));
        }
    }

    size_t size_needed = 0;
    for (auto& block : generator.m_root_basic_blocks) {
        size_needed += block->size();
    }

    Vector<u8> bytecode;
    bytecode.ensure_capacity(size_needed);

    Vector<size_t> basic_block_start_offsets;
    basic_block_start_offsets.ensure_capacity(generator.m_root_basic_blocks.size());

    HashMap<BasicBlock const*, size_t> block_offsets;
    Vector<size_t> label_offsets;

    struct UnlinkedExceptionHandlers {
        size_t start_offset;
        size_t end_offset;
        BasicBlock const* handler;
        BasicBlock const* finalizer;
    };
    Vector<UnlinkedExceptionHandlers> unlinked_exception_handlers;

    HashMap<size_t, SourceRecord> source_map;

    Optional<ScopedOperand> undefined_constant;

    for (auto& block : generator.m_root_basic_blocks) {
        if (!block->is_terminated()) {
            // NOTE: We must ensure that the "undefined" constant, which will be used by the not yet
            // emitted End instruction, is taken into account while shifting local operands by the
            // number of constants.
            undefined_constant = generator.add_constant(js_undefined());
            break;
        }
    }

    auto number_of_registers = generator.m_next_register;
    auto number_of_constants = generator.m_constants.size();
    auto number_of_locals = function ? function->local_variables_names().size() : 0;

    // Pass: Rewrite the bytecode to use the correct register and constant indices.
    for (auto& block : generator.m_root_basic_blocks) {
        Bytecode::InstructionStreamIterator it(block->instruction_stream());
        while (!it.at_end()) {
            auto& instruction = const_cast<Instruction&>(*it);

            instruction.visit_operands([number_of_registers, number_of_constants, number_of_locals](Operand& operand) {
                switch (operand.type()) {
                case Operand::Type::Register:
                    break;
                case Operand::Type::Local:
                    operand.offset_index_by(number_of_registers + number_of_constants);
                    break;
                case Operand::Type::Constant:
                    operand.offset_index_by(number_of_registers);
                    break;
                case Operand::Type::Argument:
                    operand.offset_index_by(number_of_registers + number_of_constants + number_of_locals);
                    break;
                default:
                    VERIFY_NOT_REACHED();
                }
            });

            ++it;
        }
    }

    // Also rewrite the `undefined` constant if we have one for inserting End.
    if (undefined_constant.has_value())
        undefined_constant.value().operand().offset_index_by(number_of_registers);

    for (auto& block : generator.m_root_basic_blocks) {
        basic_block_start_offsets.append(bytecode.size());
        if (block->handler() || block->finalizer()) {
            unlinked_exception_handlers.append({
                .start_offset = bytecode.size(),
                .end_offset = 0,
                .handler = block->handler(),
                .finalizer = block->finalizer(),
            });
        }

        block_offsets.set(block.ptr(), bytecode.size());

        for (auto& [offset, source_record] : block->source_map()) {
            source_map.set(bytecode.size() + offset, source_record);
        }

        Bytecode::InstructionStreamIterator it(block->instruction_stream());
        while (!it.at_end()) {
            auto& instruction = const_cast<Instruction&>(*it);

            if (instruction.type() == Instruction::Type::Jump) {
                auto& jump = static_cast<Bytecode::Op::Jump&>(instruction);

                // OPTIMIZATION: Don't emit jumps that just jump to the next block.
                if (jump.target().basic_block_index() == block->index() + 1) {
                    if (basic_block_start_offsets.last() == bytecode.size()) {
                        // This block is empty, just skip it.
                        basic_block_start_offsets.take_last();
                    }
                    ++it;
                    continue;
                }

                // OPTIMIZATION: For jumps to a return-or-end-only block, we can emit a `Return` or `End` directly instead.
                auto& target_block = *generator.m_root_basic_blocks[jump.target().basic_block_index()];
                if (target_block.is_terminated()) {
                    auto target_instruction_iterator = InstructionStreamIterator { target_block.instruction_stream() };
                    auto& target_instruction = *target_instruction_iterator;

                    if (target_instruction.type() == Instruction::Type::Return) {
                        auto& return_instruction = static_cast<Bytecode::Op::Return const&>(target_instruction);
                        Op::Return return_op(return_instruction.value());
                        bytecode.append(reinterpret_cast<u8 const*>(&return_op), return_op.length());
                        ++it;
                        continue;
                    }

                    if (target_instruction.type() == Instruction::Type::End) {
                        auto& return_instruction = static_cast<Bytecode::Op::End const&>(target_instruction);
                        Op::End end_op(return_instruction.value());
                        bytecode.append(reinterpret_cast<u8 const*>(&end_op), end_op.length());
                        ++it;
                        continue;
                    }
                }
            }

            // OPTIMIZATION: For `JumpIf` where one of the targets is the very next block,
            //               we can emit a `JumpTrue` or `JumpFalse` (to the other block) instead.
            if (instruction.type() == Instruction::Type::JumpIf) {
                auto& jump = static_cast<Bytecode::Op::JumpIf&>(instruction);
                if (jump.true_target().basic_block_index() == block->index() + 1) {
                    Op::JumpFalse jump_false(jump.condition(), Label { jump.false_target() });
                    auto& label = jump_false.target();
                    size_t label_offset = bytecode.size() + (bit_cast<FlatPtr>(&label) - bit_cast<FlatPtr>(&jump_false));
                    label_offsets.append(label_offset);
                    bytecode.append(reinterpret_cast<u8 const*>(&jump_false), jump_false.length());
                    ++it;
                    continue;
                }
                if (jump.false_target().basic_block_index() == block->index() + 1) {
                    Op::JumpTrue jump_true(jump.condition(), Label { jump.true_target() });
                    auto& label = jump_true.target();
                    size_t label_offset = bytecode.size() + (bit_cast<FlatPtr>(&label) - bit_cast<FlatPtr>(&jump_true));
                    label_offsets.append(label_offset);
                    bytecode.append(reinterpret_cast<u8 const*>(&jump_true), jump_true.length());
                    ++it;
                    continue;
                }
            }

            instruction.visit_labels([&](Label& label) {
                size_t label_offset = bytecode.size() + (bit_cast<FlatPtr>(&label) - bit_cast<FlatPtr>(&instruction));
                label_offsets.append(label_offset);
            });
            bytecode.append(reinterpret_cast<u8 const*>(&instruction), instruction.length());
            ++it;
        }
        if (!block->is_terminated()) {
            Op::End end(*undefined_constant);
            bytecode.append(reinterpret_cast<u8 const*>(&end), end.length());
        }
        if (block->handler() || block->finalizer()) {
            unlinked_exception_handlers.last().end_offset = bytecode.size();
        }
    }
    for (auto label_offset : label_offsets) {
        auto& label = *reinterpret_cast<Label*>(bytecode.data() + label_offset);
        auto* block = generator.m_root_basic_blocks[label.basic_block_index()].ptr();
        label.set_address(block_offsets.get(block).value());
    }

    auto executable = vm.heap().allocate<Executable>(
        move(bytecode),
        move(generator.m_identifier_table),
        move(generator.m_string_table),
        move(generator.m_regex_table),
        move(generator.m_constants),
        node.source_code(),
        generator.m_next_property_lookup_cache,
        generator.m_next_global_variable_cache,
        generator.m_next_register,
        generator.m_strict);

    Vector<Executable::ExceptionHandlers> linked_exception_handlers;

    for (auto& unlinked_handler : unlinked_exception_handlers) {
        auto start_offset = unlinked_handler.start_offset;
        auto end_offset = unlinked_handler.end_offset;
        auto handler_offset = unlinked_handler.handler ? block_offsets.get(unlinked_handler.handler).value() : Optional<size_t> {};
        auto finalizer_offset = unlinked_handler.finalizer ? block_offsets.get(unlinked_handler.finalizer).value() : Optional<size_t> {};
        linked_exception_handlers.append({ start_offset, end_offset, handler_offset, finalizer_offset });
    }

    quick_sort(linked_exception_handlers, [](auto const& a, auto const& b) {
        return a.start_offset < b.start_offset;
    });

    executable->exception_handlers = move(linked_exception_handlers);
    executable->basic_block_start_offsets = move(basic_block_start_offsets);
    executable->source_map = move(source_map);
    executable->local_variable_names = move(local_variable_names);
    executable->local_index_base = number_of_registers + number_of_constants;
    executable->argument_index_base = number_of_registers + number_of_constants + number_of_locals;
    executable->length_identifier = generator.m_length_identifier;

    generator.m_finished = true;

    return executable;
}

CodeGenerationErrorOr<GC::Ref<Executable>> Generator::generate_from_ast_node(VM& vm, ASTNode const& node, FunctionKind enclosing_function_kind)
{
    Vector<LocalVariable> local_variable_names;
    if (is<ScopeNode>(node))
        local_variable_names = static_cast<ScopeNode const&>(node).local_variables_names();
    return compile(vm, node, enclosing_function_kind, {}, MustPropagateCompletion::Yes, move(local_variable_names));
}

CodeGenerationErrorOr<GC::Ref<Executable>> Generator::generate_from_function(VM& vm, ECMAScriptFunctionObject const& function)
{
    return compile(vm, function.ecmascript_code(), function.kind(), &function, MustPropagateCompletion::No, function.local_variables_names());
}

void Generator::grow(size_t additional_size)
{
    VERIFY(m_current_basic_block);
    m_current_basic_block->grow(additional_size);
}

ScopedOperand Generator::allocate_register()
{
    if (!m_free_registers.is_empty()) {
        return ScopedOperand { *this, Operand { m_free_registers.take_last() } };
    }
    VERIFY(m_next_register != NumericLimits<u32>::max());
    return ScopedOperand { *this, Operand { Register { m_next_register++ } } };
}

void Generator::free_register(Register reg)
{
    m_free_registers.append(reg);
}

ScopedOperand Generator::local(Identifier::Local const& local)
{
    if (local.is_variable())
        return ScopedOperand { *this, Operand { Operand::Type::Local, static_cast<u32>(local.index) } };
    return ScopedOperand { *this, Operand { Operand::Type::Argument, static_cast<u32>(local.index) } };
}

Generator::SourceLocationScope::SourceLocationScope(Generator& generator, ASTNode const& node)
    : m_generator(generator)
    , m_previous_node(m_generator.m_current_ast_node)
{
    m_generator.m_current_ast_node = &node;
}

Generator::SourceLocationScope::~SourceLocationScope()
{
    m_generator.m_current_ast_node = m_previous_node;
}

Generator::UnwindContext::UnwindContext(Generator& generator, Optional<Label> finalizer)
    : m_generator(generator)
    , m_finalizer(finalizer)
    , m_previous_context(m_generator.m_current_unwind_context)
{
    m_generator.m_current_unwind_context = this;
}

Generator::UnwindContext::~UnwindContext()
{
    VERIFY(m_generator.m_current_unwind_context == this);
    m_generator.m_current_unwind_context = m_previous_context;
}

Label Generator::nearest_continuable_scope() const
{
    return m_continuable_scopes.last().bytecode_target;
}

bool Generator::emit_block_declaration_instantiation(ScopeNode const& scope_node)
{
    bool needs_block_declaration_instantiation = false;
    MUST(scope_node.for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
        if (declaration.is_function_declaration()) {
            needs_block_declaration_instantiation = true;
            return;
        }
        MUST(declaration.for_each_bound_identifier([&](auto const& id) {
            if (!id.is_local())
                needs_block_declaration_instantiation = true;
        }));
    }));

    if (!needs_block_declaration_instantiation)
        return false;

    auto environment = allocate_register();
    emit<Bytecode::Op::CreateLexicalEnvironment>(environment);
    start_boundary(BlockBoundaryType::LeaveLexicalEnvironment);

    MUST(scope_node.for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
        auto is_constant_declaration = declaration.is_constant_declaration();
        // NOTE: Due to the use of MUST with `create_immutable_binding` and `create_mutable_binding` below,
        //       an exception should not result from `for_each_bound_name`.
        // a. For each element dn of the BoundNames of d, do
        MUST(declaration.for_each_bound_identifier([&](Identifier const& identifier) {
            if (identifier.is_local()) {
                // NOTE: No need to create bindings for local variables as their values are not stored in an environment.
                return;
            }

            auto const& name = identifier.string();

            // i. If IsConstantDeclaration of d is true, then
            if (is_constant_declaration) {
                // 1. Perform ! env.CreateImmutableBinding(dn, true).
                emit<Bytecode::Op::CreateImmutableBinding>(environment, intern_identifier(name), true);
            }
            // ii. Else,
            else {
                // 1. Perform ! env.CreateMutableBinding(dn, false). NOTE: This step is replaced in section B.3.2.6.
                emit<Bytecode::Op::CreateMutableBinding>(environment, intern_identifier(name), false);
            }
        }));

        // b. If d is either a FunctionDeclaration, a GeneratorDeclaration, an AsyncFunctionDeclaration, or an AsyncGeneratorDeclaration, then
        if (is<FunctionDeclaration>(declaration)) {
            // i. Let fn be the sole element of the BoundNames of d.
            auto& function_declaration = static_cast<FunctionDeclaration const&>(declaration);

            // ii. Let fo be InstantiateFunctionObject of d with arguments env and privateEnv.
            auto fo = allocate_register();
            emit<Bytecode::Op::NewFunction>(fo, function_declaration, OptionalNone {});

            // iii. Perform ! env.InitializeBinding(fn, fo). NOTE: This step is replaced in section B.3.2.6.
            if (function_declaration.name_identifier()->is_local()) {
                auto local_index = function_declaration.name_identifier()->local_index();
                if (local_index.is_variable()) {
                    emit<Bytecode::Op::Mov>(local(local_index), fo);
                } else {
                    VERIFY_NOT_REACHED();
                }
            } else {
                emit<Bytecode::Op::InitializeLexicalBinding>(intern_identifier(function_declaration.name()), fo);
            }
        }
    }));

    return true;
}

void Generator::begin_variable_scope()
{
    start_boundary(BlockBoundaryType::LeaveLexicalEnvironment);
    emit<Bytecode::Op::CreateLexicalEnvironment>();
}

void Generator::end_variable_scope()
{
    end_boundary(BlockBoundaryType::LeaveLexicalEnvironment);

    if (!m_current_basic_block->is_terminated()) {
        emit<Bytecode::Op::LeaveLexicalEnvironment>();
    }
}

void Generator::begin_continuable_scope(Label continue_target, Vector<FlyString> const& language_label_set)
{
    m_continuable_scopes.append({ continue_target, language_label_set });
    start_boundary(BlockBoundaryType::Continue);
}

void Generator::end_continuable_scope()
{
    m_continuable_scopes.take_last();
    end_boundary(BlockBoundaryType::Continue);
}

Label Generator::nearest_breakable_scope() const
{
    return m_breakable_scopes.last().bytecode_target;
}

void Generator::begin_breakable_scope(Label breakable_target, Vector<FlyString> const& language_label_set)
{
    m_breakable_scopes.append({ breakable_target, language_label_set });
    start_boundary(BlockBoundaryType::Break);
}

void Generator::end_breakable_scope()
{
    m_breakable_scopes.take_last();
    end_boundary(BlockBoundaryType::Break);
}

CodeGenerationErrorOr<Generator::ReferenceOperands> Generator::emit_super_reference(MemberExpression const& expression)
{
    VERIFY(is<SuperExpression>(expression.object()));

    // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
    // 1. Let env be GetThisEnvironment().
    // 2. Let actualThis be ? env.GetThisBinding().
    auto actual_this = get_this();

    Optional<ScopedOperand> computed_property_value;
    Optional<IdentifierTableIndex> property_key_id;

    if (expression.is_computed()) {
        // SuperProperty : super [ Expression ]
        // 3. Let propertyNameReference be ? Evaluation of Expression.
        // 4. Let propertyNameValue be ? GetValue(propertyNameReference).
        computed_property_value = TRY(expression.property().generate_bytecode(*this)).value();
    } else {
        // SuperProperty : super . IdentifierName
        // 3. Let propertyKey be the StringValue of IdentifierName.
        auto const identifier_name = as<Identifier>(expression.property()).string();
        property_key_id = intern_identifier(identifier_name);
    }

    // 5/7. Return ? MakeSuperPropertyReference(actualThis, propertyKey, strict).

    // https://tc39.es/ecma262/#sec-makesuperpropertyreference
    // 1. Let env be GetThisEnvironment().
    // 2. Assert: env.HasSuperBinding() is true.
    // 3. Let baseValue be ? env.GetSuperBase().
    auto base_value = allocate_register();
    emit<Bytecode::Op::ResolveSuperBase>(base_value);

    // 4. Return the Reference Record { [[Base]]: baseValue, [[ReferencedName]]: propertyKey, [[Strict]]: strict, [[ThisValue]]: actualThis }.
    return ReferenceOperands {
        .base = base_value,
        .referenced_name = computed_property_value,
        .referenced_identifier = property_key_id,
        .this_value = actual_this,
    };
}

CodeGenerationErrorOr<Generator::ReferenceOperands> Generator::emit_load_from_reference(JS::ASTNode const& node, Optional<ScopedOperand> preferred_dst)
{
    if (is<Identifier>(node)) {
        auto& identifier = static_cast<Identifier const&>(node);
        auto loaded_value = TRY(identifier.generate_bytecode(*this, preferred_dst)).value();
        return ReferenceOperands {
            .loaded_value = loaded_value,
        };
    }
    if (!is<MemberExpression>(node)) {
        return CodeGenerationError {
            &node,
            "Unimplemented/invalid node used as a reference"sv
        };
    }
    auto& expression = static_cast<MemberExpression const&>(node);

    // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
    if (is<SuperExpression>(expression.object())) {
        auto super_reference = TRY(emit_super_reference(expression));
        auto dst = preferred_dst.has_value() ? preferred_dst.value() : allocate_register();

        if (super_reference.referenced_name.has_value()) {
            // 5. Let propertyKey be ? ToPropertyKey(propertyNameValue).
            // FIXME: This does ToPropertyKey out of order, which is observable by Symbol.toPrimitive!
            emit_get_by_value_with_this(dst, *super_reference.base, *super_reference.referenced_name, *super_reference.this_value);
        } else {
            // 3. Let propertyKey be StringValue of IdentifierName.
            auto identifier_table_ref = intern_identifier(as<Identifier>(expression.property()).string());
            emit_get_by_id_with_this(dst, *super_reference.base, identifier_table_ref, *super_reference.this_value);
        }

        super_reference.loaded_value = dst;
        return super_reference;
    }

    auto base = TRY(expression.object().generate_bytecode(*this)).value();
    auto base_identifier = intern_identifier_for_expression(expression.object());

    if (expression.is_computed()) {
        auto property = TRY(expression.property().generate_bytecode(*this)).value();
        auto saved_property = allocate_register();
        emit<Bytecode::Op::Mov>(saved_property, property);
        auto dst = preferred_dst.has_value() ? preferred_dst.value() : allocate_register();
        emit_get_by_value(dst, base, property, move(base_identifier));
        return ReferenceOperands {
            .base = base,
            .referenced_name = saved_property,
            .this_value = base,
            .loaded_value = dst,
        };
    }
    if (expression.property().is_identifier()) {
        auto identifier_table_ref = intern_identifier(as<Identifier>(expression.property()).string());
        auto dst = preferred_dst.has_value() ? preferred_dst.value() : allocate_register();
        emit_get_by_id(dst, base, identifier_table_ref, move(base_identifier));
        return ReferenceOperands {
            .base = base,
            .referenced_identifier = identifier_table_ref,
            .this_value = base,
            .loaded_value = dst,
        };
    }
    if (expression.property().is_private_identifier()) {
        auto identifier_table_ref = intern_identifier(as<PrivateIdentifier>(expression.property()).string());
        auto dst = preferred_dst.has_value() ? preferred_dst.value() : allocate_register();
        emit<Bytecode::Op::GetPrivateById>(dst, base, identifier_table_ref);
        return ReferenceOperands {
            .base = base,
            .referenced_private_identifier = identifier_table_ref,
            .this_value = base,
            .loaded_value = dst,
        };
    }
    return CodeGenerationError {
        &expression,
        "Unimplemented non-computed member expression"sv
    };
}

CodeGenerationErrorOr<void> Generator::emit_store_to_reference(JS::ASTNode const& node, ScopedOperand value)
{
    if (is<Identifier>(node)) {
        auto& identifier = static_cast<Identifier const&>(node);
        emit_set_variable(identifier, value);
        return {};
    }
    if (is<MemberExpression>(node)) {
        auto& expression = static_cast<MemberExpression const&>(node);

        // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
        if (is<SuperExpression>(expression.object())) {
            auto super_reference = TRY(emit_super_reference(expression));

            // 4. Return the Reference Record { [[Base]]: baseValue, [[ReferencedName]]: propertyKey, [[Strict]]: strict, [[ThisValue]]: actualThis }.
            if (super_reference.referenced_name.has_value()) {
                // 5. Let propertyKey be ? ToPropertyKey(propertyNameValue).
                // FIXME: This does ToPropertyKey out of order, which is observable by Symbol.toPrimitive!
                emit_put_by_value_with_this(*super_reference.base, *super_reference.referenced_name, *super_reference.this_value, value, PutKind::Normal);
            } else {
                // 3. Let propertyKey be StringValue of IdentifierName.
                auto identifier_table_ref = intern_identifier(as<Identifier>(expression.property()).string());
                emit<Bytecode::Op::PutNormalByIdWithThis>(*super_reference.base, *super_reference.this_value, identifier_table_ref, value, next_property_lookup_cache());
            }
        } else {
            auto object = TRY(expression.object().generate_bytecode(*this)).value();

            if (expression.is_computed()) {
                auto property = TRY(expression.property().generate_bytecode(*this)).value();
                emit_put_by_value(object, property, value, PutKind::Normal, {});
            } else if (expression.property().is_identifier()) {
                auto identifier_table_ref = intern_identifier(as<Identifier>(expression.property()).string());
                emit_put_by_id(object, identifier_table_ref, value, Bytecode::PutKind::Normal, next_property_lookup_cache());
            } else if (expression.property().is_private_identifier()) {
                auto identifier_table_ref = intern_identifier(as<PrivateIdentifier>(expression.property()).string());
                emit<Bytecode::Op::PutPrivateById>(object, identifier_table_ref, value);
            } else {
                return CodeGenerationError {
                    &expression,
                    "Unimplemented non-computed member expression"sv
                };
            }
        }

        return {};
    }

    return CodeGenerationError {
        &node,
        "Unimplemented/invalid node used a reference"sv
    };
}

CodeGenerationErrorOr<void> Generator::emit_store_to_reference(ReferenceOperands const& reference, ScopedOperand value)
{
    if (reference.referenced_private_identifier.has_value()) {
        emit<Bytecode::Op::PutPrivateById>(*reference.base, *reference.referenced_private_identifier, value);
        return {};
    }
    if (reference.referenced_identifier.has_value()) {
        if (reference.base == reference.this_value)
            emit_put_by_id(*reference.base, *reference.referenced_identifier, value, Bytecode::PutKind::Normal, next_property_lookup_cache());
        else
            emit<Bytecode::Op::PutNormalByIdWithThis>(*reference.base, *reference.this_value, *reference.referenced_identifier, value, next_property_lookup_cache());
        return {};
    }
    if (reference.base == reference.this_value)
        emit_put_by_value(*reference.base, *reference.referenced_name, value, PutKind::Normal, {});
    else
        emit_put_by_value_with_this(*reference.base, *reference.referenced_name, *reference.this_value, value, PutKind::Normal);
    return {};
}

CodeGenerationErrorOr<Optional<ScopedOperand>> Generator::emit_delete_reference(JS::ASTNode const& node)
{
    if (is<Identifier>(node)) {
        auto& identifier = static_cast<Identifier const&>(node);
        if (identifier.is_local()) {
            return add_constant(Value(false));
        }
        auto dst = allocate_register();
        emit<Bytecode::Op::DeleteVariable>(dst, intern_identifier(identifier.string()));
        return dst;
    }

    if (is<MemberExpression>(node)) {
        auto& expression = static_cast<MemberExpression const&>(node);

        // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
        if (is<SuperExpression>(expression.object())) {
            auto super_reference = TRY(emit_super_reference(expression));

            auto dst = allocate_register();
            if (super_reference.referenced_name.has_value()) {
                emit<Bytecode::Op::DeleteByValueWithThis>(dst, *super_reference.base, *super_reference.this_value, *super_reference.referenced_name);
            } else {
                auto identifier_table_ref = intern_identifier(as<Identifier>(expression.property()).string());
                emit<Bytecode::Op::DeleteByIdWithThis>(dst, *super_reference.base, *super_reference.this_value, identifier_table_ref);
            }

            return dst;
        }

        auto object = TRY(expression.object().generate_bytecode(*this)).value();
        auto dst = allocate_register();

        if (expression.is_computed()) {
            auto property = TRY(expression.property().generate_bytecode(*this)).value();
            emit<Bytecode::Op::DeleteByValue>(dst, object, property);
        } else if (expression.property().is_identifier()) {
            auto identifier_table_ref = intern_identifier(as<Identifier>(expression.property()).string());
            emit<Bytecode::Op::DeleteById>(dst, object, identifier_table_ref);
        } else {
            // NOTE: Trying to delete a private field generates a SyntaxError in the parser.
            return CodeGenerationError {
                &expression,
                "Unimplemented non-computed member expression"sv
            };
        }
        return dst;
    }

    // Though this will have no deletion effect, we still have to evaluate the node as it can have side effects.
    // For example: delete a(); delete ++c.b; etc.

    // 13.5.1.2 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
    // 1. Let ref be the result of evaluating UnaryExpression.
    // 2. ReturnIfAbrupt(ref).
    (void)TRY(node.generate_bytecode(*this));

    // 3. If ref is not a Reference Record, return true.
    // NOTE: The rest of the steps are handled by Delete{Variable,ByValue,Id}.
    return add_constant(Value(true));
}

void Generator::emit_set_variable(JS::Identifier const& identifier, ScopedOperand value, Bytecode::Op::BindingInitializationMode initialization_mode, Bytecode::Op::EnvironmentMode environment_mode)
{
    if (identifier.is_local()) {
        auto local_index = identifier.local_index();
        if (value.operand().is_local() && local_index.is_variable() && value.operand().index() == local_index.index) {
            // Moving a local to itself is a no-op.
            return;
        }
        emit<Bytecode::Op::Mov>(local(local_index), value);
    } else {
        auto identifier_index = intern_identifier(identifier.string());
        if (environment_mode == Bytecode::Op::EnvironmentMode::Lexical) {
            if (initialization_mode == Bytecode::Op::BindingInitializationMode::Initialize) {
                emit<Bytecode::Op::InitializeLexicalBinding>(identifier_index, value);
            } else if (initialization_mode == Bytecode::Op::BindingInitializationMode::Set) {
                if (identifier.is_global()) {
                    emit<Bytecode::Op::SetGlobal>(identifier_index, value, next_global_variable_cache());
                } else {
                    emit<Bytecode::Op::SetLexicalBinding>(identifier_index, value);
                }
            }
        } else if (environment_mode == Bytecode::Op::EnvironmentMode::Var) {
            if (initialization_mode == Bytecode::Op::BindingInitializationMode::Initialize) {
                emit<Bytecode::Op::InitializeVariableBinding>(identifier_index, value);
            } else if (initialization_mode == Bytecode::Op::BindingInitializationMode::Set) {
                emit<Bytecode::Op::SetVariableBinding>(identifier_index, value);
            }
        } else {
            VERIFY_NOT_REACHED();
        }
    }
}

static Optional<Utf16String> expression_identifier(Expression const& expression)
{
    if (expression.is_identifier()) {
        auto const& identifier = static_cast<Identifier const&>(expression);
        return identifier.string().to_utf16_string();
    }

    if (expression.is_numeric_literal()) {
        auto const& literal = static_cast<NumericLiteral const&>(expression);
        return literal.value().to_utf16_string_without_side_effects();
    }

    if (expression.is_string_literal()) {
        auto const& literal = static_cast<StringLiteral const&>(expression);
        return Utf16String::formatted("'{}'", literal.value());
    }

    if (expression.is_member_expression()) {
        auto const& member_expression = static_cast<MemberExpression const&>(expression);
        StringBuilder builder(StringBuilder::Mode::UTF16);

        if (auto identifier = expression_identifier(member_expression.object()); identifier.has_value())
            builder.append(*identifier);

        if (auto identifier = expression_identifier(member_expression.property()); identifier.has_value()) {
            if (member_expression.is_computed())
                builder.appendff("[{}]", *identifier);
            else
                builder.appendff(".{}", *identifier);
        }

        return builder.to_utf16_string();
    }

    return {};
}

Optional<IdentifierTableIndex> Generator::intern_identifier_for_expression(Expression const& expression)
{
    if (auto identifier = expression_identifier(expression); identifier.has_value())
        return intern_identifier(identifier.release_value());
    return {};
}

void Generator::generate_scoped_jump(JumpType type)
{
    TemporaryChange temp { m_current_unwind_context, m_current_unwind_context };
    bool last_was_finally = false;
    for (size_t i = m_boundaries.size(); i > 0; --i) {
        auto boundary = m_boundaries[i - 1];
        using enum BlockBoundaryType;
        switch (boundary) {
        case Break:
            if (type == JumpType::Break) {
                emit<Op::Jump>(nearest_breakable_scope());
                return;
            }
            break;
        case Continue:
            if (type == JumpType::Continue) {
                emit<Op::Jump>(nearest_continuable_scope());
                return;
            }
            break;
        case Unwind:
            if (!last_was_finally) {
                VERIFY(m_current_unwind_context && m_current_unwind_context->handler().has_value());
                emit<Bytecode::Op::LeaveUnwindContext>();
                m_current_unwind_context = m_current_unwind_context->previous();
            }
            last_was_finally = false;
            break;
        case LeaveLexicalEnvironment:
            emit<Bytecode::Op::LeaveLexicalEnvironment>();
            break;
        case ReturnToFinally: {
            VERIFY(m_current_unwind_context->finalizer().has_value());
            m_current_unwind_context = m_current_unwind_context->previous();
            auto jump_type_name = type == JumpType::Break ? "break"sv : "continue"sv;
            auto block_name = MUST(String::formatted("{}.{}", current_block().name(), jump_type_name));
            auto& block = make_block(block_name);
            emit<Op::ScheduleJump>(Label { block });
            switch_to_basic_block(block);
            last_was_finally = true;
            break;
        }
        case LeaveFinally:
            emit<Op::LeaveFinally>();
            break;
        }
    }
    VERIFY_NOT_REACHED();
}

void Generator::generate_labelled_jump(JumpType type, FlyString const& label)
{
    TemporaryChange temp { m_current_unwind_context, m_current_unwind_context };
    size_t current_boundary = m_boundaries.size();
    bool last_was_finally = false;

    auto const& jumpable_scopes = type == JumpType::Continue ? m_continuable_scopes : m_breakable_scopes;

    for (auto const& jumpable_scope : jumpable_scopes.in_reverse()) {
        for (; current_boundary > 0; --current_boundary) {
            auto boundary = m_boundaries[current_boundary - 1];
            if (boundary == BlockBoundaryType::Unwind) {
                if (!last_was_finally) {
                    VERIFY(m_current_unwind_context && m_current_unwind_context->handler().has_value());
                    emit<Bytecode::Op::LeaveUnwindContext>();
                    m_current_unwind_context = m_current_unwind_context->previous();
                }
                last_was_finally = false;
            } else if (boundary == BlockBoundaryType::LeaveLexicalEnvironment) {
                emit<Bytecode::Op::LeaveLexicalEnvironment>();
            } else if (boundary == BlockBoundaryType::ReturnToFinally) {
                VERIFY(m_current_unwind_context->finalizer().has_value());
                m_current_unwind_context = m_current_unwind_context->previous();
                auto jump_type_name = type == JumpType::Break ? "break"sv : "continue"sv;
                auto block_name = MUST(String::formatted("{}.{}", current_block().name(), jump_type_name));
                auto& block = make_block(block_name);
                emit<Op::ScheduleJump>(Label { block });
                switch_to_basic_block(block);
                last_was_finally = true;
            } else if ((type == JumpType::Continue && boundary == BlockBoundaryType::Continue) || (type == JumpType::Break && boundary == BlockBoundaryType::Break)) {
                // Make sure we don't process this boundary twice if the current jumpable scope doesn't contain the target label.
                --current_boundary;
                break;
            }
        }

        if (jumpable_scope.language_label_set.contains_slow(label)) {
            emit<Op::Jump>(jumpable_scope.bytecode_target);
            return;
        }
    }

    // We must have a jumpable scope available that contains the label, as this should be enforced by the parser.
    VERIFY_NOT_REACHED();
}

void Generator::generate_break()
{
    generate_scoped_jump(JumpType::Break);
}

void Generator::generate_break(FlyString const& break_label)
{
    generate_labelled_jump(JumpType::Break, break_label);
}

void Generator::generate_continue()
{
    generate_scoped_jump(JumpType::Continue);
}

void Generator::generate_continue(FlyString const& continue_label)
{
    generate_labelled_jump(JumpType::Continue, continue_label);
}

void Generator::push_home_object(ScopedOperand object)
{
    m_home_objects.append(object);
}

void Generator::pop_home_object()
{
    m_home_objects.take_last();
}

void Generator::emit_new_function(ScopedOperand dst, FunctionExpression const& function_node, Optional<IdentifierTableIndex> lhs_name)
{
    if (m_home_objects.is_empty()) {
        emit<Op::NewFunction>(dst, function_node, lhs_name);
    } else {
        emit<Op::NewFunction>(dst, function_node, lhs_name, m_home_objects.last());
    }
}

CodeGenerationErrorOr<ScopedOperand> Generator::emit_named_evaluation_if_anonymous_function(Expression const& expression, Optional<IdentifierTableIndex> lhs_name, Optional<ScopedOperand> preferred_dst)
{
    if (is<FunctionExpression>(expression)) {
        auto const& function_expression = static_cast<FunctionExpression const&>(expression);
        if (!function_expression.has_name()) {
            return TRY(function_expression.generate_bytecode_with_lhs_name(*this, move(lhs_name), preferred_dst)).value();
        }
    }

    if (is<ClassExpression>(expression)) {
        auto const& class_expression = static_cast<ClassExpression const&>(expression);
        if (!class_expression.has_name()) {
            return TRY(class_expression.generate_bytecode_with_lhs_name(*this, move(lhs_name), preferred_dst)).value();
        }
    }

    return TRY(expression.generate_bytecode(*this, preferred_dst)).value();
}

void Generator::emit_get_by_id(ScopedOperand dst, ScopedOperand base, IdentifierTableIndex property_identifier, Optional<IdentifierTableIndex> base_identifier)
{
    if (m_identifier_table->get(property_identifier) == "length"sv) {
        m_length_identifier = property_identifier;
        emit<Op::GetLength>(dst, base, move(base_identifier), m_next_property_lookup_cache++);
        return;
    }
    emit<Op::GetById>(dst, base, property_identifier, move(base_identifier), m_next_property_lookup_cache++);
}

void Generator::emit_get_by_id_with_this(ScopedOperand dst, ScopedOperand base, IdentifierTableIndex id, ScopedOperand this_value)
{
    if (m_identifier_table->get(id) == "length"sv) {
        m_length_identifier = id;
        emit<Op::GetLengthWithThis>(dst, base, this_value, m_next_property_lookup_cache++);
        return;
    }
    emit<Op::GetByIdWithThis>(dst, base, id, this_value, m_next_property_lookup_cache++);
}

void Generator::emit_get_by_value(ScopedOperand dst, ScopedOperand base, ScopedOperand property, Optional<IdentifierTableIndex> base_identifier)
{
    if (property.operand().is_constant() && get_constant(property).is_string()) {
        auto property_key = MUST(get_constant(property).to_property_key(vm()));
        if (property_key.is_string()) {
            emit_get_by_id(dst, base, intern_identifier(property_key.as_string()), base_identifier);
            return;
        }
    }
    emit<Op::GetByValue>(dst, base, property, base_identifier);
}

void Generator::emit_get_by_value_with_this(ScopedOperand dst, ScopedOperand base, ScopedOperand property, ScopedOperand this_value)
{
    if (property.operand().is_constant() && get_constant(property).is_string()) {
        auto property_key = MUST(get_constant(property).to_property_key(vm()));
        if (property_key.is_string()) {
            emit_get_by_id_with_this(dst, base, intern_identifier(property_key.as_string()), this_value);
            return;
        }
    }
    emit<Op::GetByValueWithThis>(dst, base, property, this_value);
}

void Generator::emit_put_by_id(Operand base, IdentifierTableIndex property, Operand src, PutKind kind, u32 cache_index, Optional<IdentifierTableIndex> base_identifier)
{
    auto string = m_identifier_table->get(property);
    if (!string.is_empty() && !(string.code_unit_at(0) == '0' && string.length_in_code_units() > 1)) {
        auto property_index = string.to_number<u32>(TrimWhitespace::No);
        if (property_index.has_value() && property_index.value() < NumericLimits<u32>::max()) {
#define EMIT_PUT_BY_NUMERIC_ID(kind)                                                                                     \
    case PutKind::kind:                                                                                                  \
        emit<Op::Put##kind##ByNumericId>(base, property_index.release_value(), src, cache_index, move(base_identifier)); \
        break;
            switch (kind) {
                JS_ENUMERATE_PUT_KINDS(EMIT_PUT_BY_NUMERIC_ID)
            default:
                VERIFY_NOT_REACHED();
            }
#undef EMIT_PUT_BY_NUMERIC_ID
            return;
        }
    }
#define EMIT_PUT_BY_ID(kind)                                                                \
    case PutKind::kind:                                                                     \
        emit<Op::Put##kind##ById>(base, property, src, cache_index, move(base_identifier)); \
        break;
    switch (kind) {
        JS_ENUMERATE_PUT_KINDS(EMIT_PUT_BY_ID)
    default:
        VERIFY_NOT_REACHED();
    }
#undef EMIT_PUT_BY_ID
}

void Generator::emit_put_by_value(ScopedOperand base, ScopedOperand property, ScopedOperand src, Bytecode::PutKind kind, Optional<IdentifierTableIndex> base_identifier)
{
    if (property.operand().is_constant() && get_constant(property).is_string()) {
        auto property_key = MUST(get_constant(property).to_property_key(vm()));
        if (property_key.is_string()) {
            emit_put_by_id(base, intern_identifier(property_key.as_string()), src, kind, m_next_property_lookup_cache++, base_identifier);
            return;
        }
    }
#define EMIT_PUT_BY_VALUE(kind)                                                   \
    case PutKind::kind:                                                           \
        emit<Op::Put##kind##ByValue>(base, property, src, move(base_identifier)); \
        break;
    switch (kind) {
        JS_ENUMERATE_PUT_KINDS(EMIT_PUT_BY_VALUE)
    default:
        VERIFY_NOT_REACHED();
    }
#undef EMIT_PUT_BY_VALUE
}

void Generator::emit_put_by_value_with_this(ScopedOperand base, ScopedOperand property, ScopedOperand this_value, ScopedOperand src, Bytecode::PutKind kind)
{
    if (property.operand().is_constant() && get_constant(property).is_string()) {
        auto property_key = MUST(get_constant(property).to_property_key(vm()));
        if (property_key.is_string()) {
#define EMIT_PUT_BY_ID_WITH_THIS(kind)                                                                                                         \
    case PutKind::kind:                                                                                                                        \
        emit<Op::Put##kind##ByIdWithThis>(base, this_value, intern_identifier(property_key.as_string()), src, m_next_property_lookup_cache++); \
        break;
            switch (kind) {
                JS_ENUMERATE_PUT_KINDS(EMIT_PUT_BY_ID_WITH_THIS)
            default:
                VERIFY_NOT_REACHED();
            }
#undef EMIT_PUT_BY_ID_WITH_THIS
            return;
        }
    }
#define EMIT_PUT_BY_VALUE_WITH_THIS(kind)                                      \
    case PutKind::kind:                                                        \
        emit<Op::Put##kind##ByValueWithThis>(base, property, this_value, src); \
        break;
    switch (kind) {
        JS_ENUMERATE_PUT_KINDS(EMIT_PUT_BY_VALUE_WITH_THIS)
    default:
        VERIFY_NOT_REACHED();
    }
#undef EMIT_PUT_BY_VALUE_WITH_THIS
}

void Generator::emit_iterator_value(ScopedOperand dst, ScopedOperand result)
{
    emit_get_by_id(dst, result, intern_identifier("value"_utf16_fly_string));
}

void Generator::emit_iterator_complete(ScopedOperand dst, ScopedOperand result)
{
    emit_get_by_id(dst, result, intern_identifier("done"_utf16_fly_string));
}

bool Generator::is_local_initialized(u32 local_index) const
{
    return m_initialized_locals.find(local_index) != m_initialized_locals.end();
}

bool Generator::is_local_initialized(Identifier::Local const& local) const
{
    if (local.is_variable())
        return m_initialized_locals.find(local.index) != m_initialized_locals.end();
    if (local.is_argument())
        return m_initialized_arguments.find(local.index) != m_initialized_arguments.end();
    return true;
}

void Generator::set_local_initialized(Identifier::Local const& local)
{
    if (local.is_variable()) {
        m_initialized_locals.set(local.index);
    } else if (local.is_argument()) {
        m_initialized_arguments.set(local.index);
    } else {
        VERIFY_NOT_REACHED();
    }
}

bool Generator::is_local_lexically_declared(Identifier::Local const& local) const
{
    if (local.is_argument())
        return false;
    return m_local_variables[local.index].declaration_kind == LocalVariable::DeclarationKind::LetOrConst;
}

ScopedOperand Generator::get_this(Optional<ScopedOperand> preferred_dst)
{
    if (m_current_basic_block->has_resolved_this())
        return this_value();
    if (m_root_basic_blocks[0]->has_resolved_this()) {
        m_current_basic_block->set_has_resolved_this();
        return this_value();
    }

    // OPTIMIZATION: If we're compiling a function that doesn't allocate a FunctionEnvironment,
    //               it will always have the same `this` value as the outer function,
    //               and so the `this` value is already in the `this` register!
    if (m_function && !m_function->allocates_function_environment())
        return this_value();

    auto dst = preferred_dst.has_value() ? preferred_dst.value() : allocate_register();
    emit<Bytecode::Op::ResolveThisBinding>();
    m_current_basic_block->set_has_resolved_this();
    return this_value();
}

ScopedOperand Generator::accumulator()
{
    return m_accumulator;
}

ScopedOperand Generator::this_value()
{
    return m_this_value;
}

bool Generator::fuse_compare_and_jump(ScopedOperand const& condition, Label true_target, Label false_target)
{
    auto& last_instruction = *reinterpret_cast<Instruction const*>(m_current_basic_block->data() + m_current_basic_block->last_instruction_start_offset());

#define HANDLE_COMPARISON_OP(op_TitleCase, op_snake_case, numeric_operator)        \
    if (last_instruction.type() == Instruction::Type::op_TitleCase) {              \
        auto& comparison = static_cast<Op::op_TitleCase const&>(last_instruction); \
        VERIFY(comparison.dst() == condition);                                     \
        auto lhs = comparison.lhs();                                               \
        auto rhs = comparison.rhs();                                               \
        m_current_basic_block->rewind();                                           \
        emit<Op::Jump##op_TitleCase>(lhs, rhs, true_target, false_target);         \
        return true;                                                               \
    }

    JS_ENUMERATE_COMPARISON_OPS(HANDLE_COMPARISON_OP);
#undef HANDLE_COMPARISON_OP

    return false;
}

void Generator::emit_jump_if(ScopedOperand const& condition, Label true_target, Label false_target)
{
    if (condition.operand().is_constant()) {
        auto value = m_constants[condition.operand().index()];
        if (value.is_boolean()) {
            if (value.as_bool()) {
                emit<Op::Jump>(true_target);
            } else {
                emit<Op::Jump>(false_target);
            }
            return;
        }
    }

    // NOTE: It's only safe to fuse compare-and-jump if the condition is a temporary with no other dependents.
    if (condition.operand().is_register()
        && condition.ref_count() == 1
        && m_current_basic_block->size() > 0) {
        if (fuse_compare_and_jump(condition, true_target, false_target))
            return;
    }

    emit<Op::JumpIf>(condition, true_target, false_target);
}

ScopedOperand Generator::copy_if_needed_to_preserve_evaluation_order(ScopedOperand const& operand)
{
    if (!operand.operand().is_local())
        return operand;
    auto new_register = allocate_register();
    emit<Bytecode::Op::Mov>(new_register, operand);
    return new_register;
}

ScopedOperand Generator::add_constant(Value value)
{
    auto append_new_constant = [&] {
        m_constants.append(value);
        return ScopedOperand { *this, Operand(Operand::Type::Constant, m_constants.size() - 1) };
    };

    if (value.is_boolean()) {
        if (value.as_bool()) {
            if (!m_true_constant.has_value())
                m_true_constant = append_new_constant();
            return m_true_constant.value();
        } else {
            if (!m_false_constant.has_value())
                m_false_constant = append_new_constant();
            return m_false_constant.value();
        }
    }
    if (value.is_undefined()) {
        if (!m_undefined_constant.has_value())
            m_undefined_constant = append_new_constant();
        return m_undefined_constant.value();
    }
    if (value.is_null()) {
        if (!m_null_constant.has_value())
            m_null_constant = append_new_constant();
        return m_null_constant.value();
    }
    if (value.is_special_empty_value()) {
        if (!m_empty_constant.has_value())
            m_empty_constant = append_new_constant();
        return m_empty_constant.value();
    }
    if (value.is_int32()) {
        auto as_int32 = value.as_i32();
        return m_int32_constants.ensure(as_int32, [&] {
            return append_new_constant();
        });
    }
    if (value.is_string()) {
        auto as_string = value.as_string().utf16_string();
        return m_string_constants.ensure(as_string, [&] {
            return append_new_constant();
        });
    }
    return append_new_constant();
}

}
