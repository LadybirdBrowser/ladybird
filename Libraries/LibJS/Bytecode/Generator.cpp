/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/QuickSort.h>
#include <AK/TemporaryChange.h>
#include <LibJS/AST.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/NativeJavaScriptBackedFunction.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Bytecode {

Generator::Generator(VM& vm, GC::Ptr<SharedFunctionInstanceData const> shared_function_instance_data, MustPropagateCompletion must_propagate_completion, BuiltinAbstractOperationsEnabled builtin_abstract_operations_enabled)
    : m_vm(vm)
    , m_string_table(make<StringTable>())
    , m_identifier_table(make<IdentifierTable>())
    , m_property_key_table(make<PropertyKeyTable>())
    , m_regex_table(make<RegexTable>())
    , m_constants(vm.heap())
    , m_accumulator(*this, Operand(Register::accumulator()))
    , m_this_value(*this, Operand(Register::this_value()))
    , m_must_propagate_completion(must_propagate_completion == MustPropagateCompletion::Yes)
    , m_builtin_abstract_operations_enabled(builtin_abstract_operations_enabled == BuiltinAbstractOperationsEnabled::Yes)
    , m_shared_function_instance_data(shared_function_instance_data)
{
}

static GC::Ref<SharedFunctionInstanceData> ensure_shared_function_data(VM& vm, FunctionNode const& function_node, Utf16FlyString name)
{
    return SharedFunctionInstanceData::create_for_function_node(vm, function_node, move(name));
}

u32 Generator::register_shared_function_data(GC::Ref<SharedFunctionInstanceData> data)
{
    auto index = static_cast<u32>(m_shared_function_data.size());
    m_shared_function_data.append(data);
    return index;
}

u32 Generator::register_class_blueprint(ClassBlueprint blueprint)
{
    auto index = static_cast<u32>(m_class_blueprints.size());
    m_class_blueprints.append(move(blueprint));
    return index;
}

void Generator::emit_function_declaration_instantiation(SharedFunctionInstanceData const& shared_function_instance_data)
{
    if (shared_function_instance_data.m_has_parameter_expressions) {
        bool has_non_local_parameters = false;
        for (auto const& parameter_name : shared_function_instance_data.m_parameter_names) {
            if (parameter_name.value == SharedFunctionInstanceData::ParameterIsLocal::No) {
                has_non_local_parameters = true;
                break;
            }
        }
        if (has_non_local_parameters) {
            auto parent_environment = m_lexical_environment_register_stack.last();
            auto new_environment = allocate_register();
            emit<Op::CreateLexicalEnvironment>(new_environment, parent_environment, 0);
            m_lexical_environment_register_stack.append(new_environment);
        }
    }

    for (auto const& parameter_name : shared_function_instance_data.m_parameter_names) {
        if (parameter_name.value == SharedFunctionInstanceData::ParameterIsLocal::No) {
            auto id = intern_identifier(parameter_name.key);
            emit<Op::CreateVariable>(id, Op::EnvironmentMode::Lexical, false, false, false);
            if (shared_function_instance_data.m_has_duplicates) {
                emit<Op::InitializeLexicalBinding>(id, add_constant(js_undefined()));
            }
        }
    }

    if (shared_function_instance_data.m_arguments_object_needed) {
        Optional<Operand> dst;
        auto local_var_index = shared_function_instance_data.m_local_variables_names.find_first_index_if([](auto const& local) { return local.declaration_kind == LocalVariable::DeclarationKind::ArgumentsObject; });
        if (local_var_index.has_value())
            dst = local(Identifier::Local::variable(local_var_index.value()));

        if (shared_function_instance_data.m_strict || !shared_function_instance_data.m_has_simple_parameter_list) {
            emit<Op::CreateArguments>(dst, Op::ArgumentsKind::Unmapped, shared_function_instance_data.m_strict);
        } else {
            emit<Op::CreateArguments>(dst, Op::ArgumentsKind::Mapped, shared_function_instance_data.m_strict);
        }

        if (local_var_index.has_value())
            set_local_initialized(Identifier::Local::variable(local_var_index.value()));
    }

    auto const& formal_parameters = *shared_function_instance_data.m_formal_parameters;
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
            auto operand = parameter.default_value->generate_bytecode(*this);
            emit<Op::Mov>(Operand { Operand::Type::Argument, param_index }, *operand);
            emit<Op::Jump>(Label { if_not_undefined_block });

            switch_to_basic_block(if_not_undefined_block);
        }

        if (auto const* identifier = parameter.binding.get_pointer<NonnullRefPtr<Identifier const>>(); identifier) {
            if ((*identifier)->is_local()) {
                set_local_initialized((*identifier)->local_index());
            } else {
                auto id = intern_identifier((*identifier)->string());
                if (shared_function_instance_data.m_has_duplicates) {
                    emit<Op::SetLexicalBinding>(id, Operand { Operand::Type::Argument, param_index });
                } else {
                    emit<Op::InitializeLexicalBinding>(id, Operand { Operand::Type::Argument, param_index });
                }
            }
        } else if (auto const* binding_pattern = parameter.binding.get_pointer<NonnullRefPtr<BindingPattern const>>(); binding_pattern) {
            ScopedOperand argument { *this, Operand { Operand::Type::Argument, param_index } };
            auto init_mode = shared_function_instance_data.m_has_duplicates ? Op::BindingInitializationMode::Set : Bytecode::Op::BindingInitializationMode::Initialize;
            (*binding_pattern)->generate_bytecode(*this, init_mode, argument);
        }
    }

    if (!shared_function_instance_data.m_has_parameter_expressions) {
        if (shared_function_instance_data.m_has_scope_body) {
            for (auto const& var : shared_function_instance_data.m_var_names_to_initialize_binding) {
                if (var.local.is_variable() || var.local.is_argument()) {
                    emit<Op::Mov>(local(var.local), add_constant(js_undefined()));
                } else {
                    auto intern_id = intern_identifier(var.name);
                    emit<Op::CreateVariable>(intern_id, Op::EnvironmentMode::Var, false, false, false);
                    emit<Op::InitializeVariableBinding>(intern_id, add_constant(js_undefined()));
                }
            }
        }
    } else {
        bool has_non_local_vars = false;
        if (shared_function_instance_data.m_has_scope_body) {
            for (auto const& var : shared_function_instance_data.m_var_names_to_initialize_binding) {
                if (!var.local.is_variable() && !var.local.is_argument()) {
                    has_non_local_vars = true;
                    break;
                }
            }
        }

        if (has_non_local_vars) {
            emit<Op::CreateVariableEnvironment>(shared_function_instance_data.m_var_environment_bindings_count);
            auto variable_environment = allocate_register();
            emit<Op::GetLexicalEnvironment>(variable_environment);
            m_lexical_environment_register_stack.append(variable_environment);
        }

        if (shared_function_instance_data.m_has_scope_body) {
            for (auto const& var : shared_function_instance_data.m_var_names_to_initialize_binding) {
                auto initial_value = allocate_register();
                if (!var.parameter_binding || var.function_name) {
                    emit<Op::Mov>(initial_value, add_constant(js_undefined()));
                } else {
                    if (var.local.is_variable() || var.local.is_argument()) {
                        emit<Op::Mov>(initial_value, local(var.local));
                    } else {
                        emit<Op::GetBinding>(initial_value, intern_identifier(var.name));
                    }
                }

                if (var.local.is_variable() || var.local.is_argument()) {
                    emit<Op::Mov>(local(var.local), initial_value);
                } else {
                    auto intern_id = intern_identifier(var.name);
                    emit<Op::CreateVariable>(intern_id, Op::EnvironmentMode::Var, false, false, false);
                    emit<Op::InitializeVariableBinding>(intern_id, initial_value);
                }
            }
        }
    }

    if (!shared_function_instance_data.m_strict && shared_function_instance_data.m_has_scope_body) {
        for (auto const& function_name : shared_function_instance_data.m_function_names_to_initialize_binding) {
            auto intern_id = intern_identifier(function_name);
            emit<Op::CreateVariable>(intern_id, Op::EnvironmentMode::Var, false, false, false);
            emit<Op::InitializeVariableBinding>(intern_id, add_constant(js_undefined()));
        }
    }

    if (!shared_function_instance_data.m_strict) {
        if (shared_function_instance_data.m_has_non_local_lexical_declarations) {
            auto parent_environment = m_lexical_environment_register_stack.last();
            auto new_environment = allocate_register();
            emit<Op::CreateLexicalEnvironment>(new_environment, parent_environment, shared_function_instance_data.m_lex_environment_bindings_count);
            m_lexical_environment_register_stack.append(new_environment);
        }
    }

    for (auto const& binding : shared_function_instance_data.m_lexical_bindings) {
        emit<Op::CreateVariable>(intern_identifier(binding.name),
            Op::EnvironmentMode::Lexical,
            binding.is_constant,
            false,
            binding.is_constant);
    }

    for (auto const& function_to_initialize : shared_function_instance_data.m_functions_to_initialize) {
        auto data_index = register_shared_function_data(function_to_initialize.shared_data);

        if (function_to_initialize.local.is_variable() || function_to_initialize.local.is_argument()) {
            emit<Op::NewFunction>(local(function_to_initialize.local), data_index, OptionalNone {}, OptionalNone {});
            set_local_initialized(function_to_initialize.local);
        } else {
            auto function = allocate_register();
            emit<Op::NewFunction>(function, data_index, OptionalNone {}, OptionalNone {});
            emit<Op::SetVariableBinding>(intern_identifier(function_to_initialize.name), function);
        }
    }
}

GC::Ref<Executable> Generator::compile(VM& vm, ASTNode const& node, FunctionKind enclosing_function_kind, GC::Ptr<SharedFunctionInstanceData const> shared_function_instance_data, MustPropagateCompletion must_propagate_completion, BuiltinAbstractOperationsEnabled builtin_abstract_operations_enabled, Vector<LocalVariable> local_variable_names)
{
    Generator generator(vm, shared_function_instance_data, must_propagate_completion, builtin_abstract_operations_enabled);

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

    // NOTE: We eagerly initialize the saved lexical environment register here,
    //       before any AST codegen runs, so that GetLexicalEnvironment is emitted
    //       at the function entry point, dominating all uses.
    generator.ensure_lexical_environment_register_initialized();

    if (shared_function_instance_data)
        generator.emit_function_declaration_instantiation(*shared_function_instance_data);

    if (generator.is_in_generator_function()) {
        // Immediately yield with no value.
        auto& start_block = generator.make_block();
        generator.emit<Bytecode::Op::Yield>(Label { start_block }, generator.add_constant(js_undefined()));
        generator.switch_to_basic_block(start_block);
        // NOTE: This doesn't have to handle received throw/return completions, as GeneratorObject::resume_abrupt
        //       will not enter the generator from the SuspendedStart state and immediately completes the generator.
    }

    auto last_value = node.generate_bytecode(generator);

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
    };
    Vector<UnlinkedExceptionHandlers> unlinked_exception_handlers;

    Vector<SourceMapEntry> source_map;

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
    auto number_of_locals = local_variable_names.size();

    u32 max_argument_index = 0;

    // Pass: Rewrite the bytecode to use the correct register and constant indices.
    for (auto& block : generator.m_root_basic_blocks) {
        Bytecode::InstructionStreamIterator it(block->instruction_stream());
        while (!it.at_end()) {
            auto& instruction = const_cast<Instruction&>(*it);

            // NB: The layout in ExecutionContext is: [registers | locals | constants | arguments]
            instruction.visit_operands([number_of_registers, number_of_constants, number_of_locals, &max_argument_index](Operand& operand) {
                switch (operand.type()) {
                case Operand::Type::Register:
                    break;
                case Operand::Type::Local:
                    operand.offset_index_by(number_of_registers);
                    break;
                case Operand::Type::Constant:
                    operand.offset_index_by(number_of_registers + number_of_locals);
                    break;
                case Operand::Type::Argument:
                    max_argument_index = max(max_argument_index, operand.index());
                    operand.offset_index_by(number_of_registers + number_of_locals + number_of_constants);
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
        undefined_constant.value().operand().offset_index_by(number_of_registers + number_of_locals);

    for (auto& block : generator.m_root_basic_blocks) {
        basic_block_start_offsets.append(bytecode.size());
        if (block->handler()) {
            unlinked_exception_handlers.append({
                .start_offset = bytecode.size(),
                .end_offset = 0,
                .handler = block->handler(),
            });
        }

        block_offsets.set(block.ptr(), bytecode.size());

        // NB: Source map entries are added inline with instruction emission
        //     to avoid phantom entries from skipped/replaced instructions.
        //     When a block has multiple source map entries at the same offset
        //     (due to rewind in fuse_compare_and_jump), we use the last one.
        auto const& source_map_entries = block->source_map();
        size_t source_map_cursor = 0;

        auto emit_source_map_entry = [&](size_t block_offset) {
            SourceRecord record = {};
            while (source_map_cursor < source_map_entries.size() && source_map_entries[source_map_cursor].bytecode_offset <= static_cast<u32>(block_offset)) {
                if (source_map_entries[source_map_cursor].bytecode_offset == static_cast<u32>(block_offset))
                    record = source_map_entries[source_map_cursor].source_record;
                ++source_map_cursor;
            }
            source_map.append({ static_cast<u32>(bytecode.size()), record });
        };

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
                        emit_source_map_entry(it.offset());
                        bytecode.append(reinterpret_cast<u8 const*>(&return_op), return_op.length());
                        ++it;
                        continue;
                    }

                    if (target_instruction.type() == Instruction::Type::End) {
                        auto& return_instruction = static_cast<Bytecode::Op::End const&>(target_instruction);
                        Op::End end_op(return_instruction.value());
                        emit_source_map_entry(it.offset());
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
                    emit_source_map_entry(it.offset());
                    bytecode.append(reinterpret_cast<u8 const*>(&jump_false), jump_false.length());
                    ++it;
                    continue;
                }
                if (jump.false_target().basic_block_index() == block->index() + 1) {
                    Op::JumpTrue jump_true(jump.condition(), Label { jump.true_target() });
                    auto& label = jump_true.target();
                    size_t label_offset = bytecode.size() + (bit_cast<FlatPtr>(&label) - bit_cast<FlatPtr>(&jump_true));
                    label_offsets.append(label_offset);
                    emit_source_map_entry(it.offset());
                    bytecode.append(reinterpret_cast<u8 const*>(&jump_true), jump_true.length());
                    ++it;
                    continue;
                }
            }

            instruction.visit_labels([&](Label& label) {
                size_t label_offset = bytecode.size() + (bit_cast<FlatPtr>(&label) - bit_cast<FlatPtr>(&instruction));
                label_offsets.append(label_offset);
            });
            emit_source_map_entry(it.offset());
            bytecode.append(reinterpret_cast<u8 const*>(&instruction), instruction.length());
            ++it;
        }
        if (!block->is_terminated()) {
            Op::End end(*undefined_constant);
            bytecode.append(reinterpret_cast<u8 const*>(&end), end.length());
        }
        if (block->handler()) {
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
        move(generator.m_property_key_table),
        move(generator.m_string_table),
        move(generator.m_regex_table),
        move(generator.m_constants),
        node.source_code(),
        generator.m_next_property_lookup_cache,
        generator.m_next_global_variable_cache,
        generator.m_next_template_object_cache,
        generator.m_next_object_shape_cache,
        generator.m_next_register,
        generator.m_strict);

    Vector<Executable::ExceptionHandlers> linked_exception_handlers;

    for (auto& unlinked_handler : unlinked_exception_handlers) {
        auto start_offset = unlinked_handler.start_offset;
        auto end_offset = unlinked_handler.end_offset;
        auto handler_offset = block_offsets.get(unlinked_handler.handler).value();

        auto maybe_exception_handler_to_merge_with = linked_exception_handlers.find_if([&](Executable::ExceptionHandlers const& exception_handler) {
            return exception_handler.end_offset == start_offset && exception_handler.handler_offset == handler_offset;
        });

        if (!maybe_exception_handler_to_merge_with.is_end()) {
            auto& exception_handler_to_merge_with = *maybe_exception_handler_to_merge_with;
            exception_handler_to_merge_with.end_offset = end_offset;
        } else {
            linked_exception_handlers.append({ start_offset, end_offset, handler_offset });
        }
    }

    quick_sort(linked_exception_handlers, [](auto const& a, auto const& b) {
        return a.start_offset < b.start_offset;
    });

    executable->exception_handlers = move(linked_exception_handlers);
    executable->basic_block_start_offsets = move(basic_block_start_offsets);
    executable->source_map = move(source_map);
    executable->local_variable_names = move(local_variable_names);

    executable->shared_function_data.ensure_capacity(generator.m_shared_function_data.size());
    for (auto& root : generator.m_shared_function_data)
        executable->shared_function_data.append(root.ptr());

    executable->class_blueprints = move(generator.m_class_blueprints);

    // NB: Layout is [registers | locals | constants | arguments]
    executable->local_index_base = number_of_registers;

    VERIFY(!Checked<u32>::addition_would_overflow(number_of_registers, number_of_locals, number_of_constants));
    executable->argument_index_base = number_of_registers + number_of_locals + number_of_constants;

    // NB: Operand indices are stored in 29 bits, so the max operand index must fit.
    VERIFY(!Checked<u32>::addition_would_overflow(executable->argument_index_base, max_argument_index));
    VERIFY(executable->argument_index_base + max_argument_index <= 0x1FFFFFFFu);

    executable->length_identifier = generator.m_length_identifier;

    VERIFY(!Checked<u32>::addition_would_overflow(executable->number_of_registers, executable->local_variable_names.size()));
    executable->registers_and_locals_count = executable->number_of_registers + executable->local_variable_names.size();

    VERIFY(!Checked<u32>::addition_would_overflow(executable->registers_and_locals_count, executable->constants.size()));
    executable->registers_and_locals_and_constants_count = executable->registers_and_locals_count + executable->constants.size();

    // Sanity check: ensure offset computation values match Executable values.
    VERIFY(number_of_registers == executable->number_of_registers);
    VERIFY(number_of_locals == executable->local_variable_names.size());
    VERIFY(number_of_constants == executable->constants.size());

    generator.m_finished = true;

    return executable;
}

GC::Ref<Executable> Generator::generate_from_ast_node(VM& vm, ASTNode const& node, FunctionKind enclosing_function_kind)
{
    Vector<LocalVariable> local_variable_names;
    if (is<ScopeNode>(node))
        local_variable_names = static_cast<ScopeNode const&>(node).local_variables_names();
    return compile(vm, node, enclosing_function_kind, {}, MustPropagateCompletion::Yes, BuiltinAbstractOperationsEnabled::No, move(local_variable_names));
}

GC::Ref<Executable> Generator::generate_from_function(VM& vm, GC::Ref<SharedFunctionInstanceData const> shared_function_instance_data, BuiltinAbstractOperationsEnabled builtin_abstract_operations_enabled)
{
    VERIFY(!shared_function_instance_data->m_executable);
    return compile(vm, *shared_function_instance_data->m_ecmascript_code, shared_function_instance_data->m_kind, shared_function_instance_data, MustPropagateCompletion::No, builtin_abstract_operations_enabled, shared_function_instance_data->m_local_variables_names);
}

void Generator::grow(size_t additional_size)
{
    VERIFY(m_current_basic_block);
    m_current_basic_block->grow(additional_size);
}

ScopedOperand Generator::allocate_register()
{
    if (!m_free_registers.is_empty()) {
        // Always allocate the lowest-numbered free register to ensure
        // deterministic allocation regardless of operand drop order.
        size_t min_index = 0;
        for (size_t i = 1; i < m_free_registers.size(); ++i) {
            if (m_free_registers[i].index() < m_free_registers[min_index].index())
                min_index = i;
        }
        auto reg = m_free_registers[min_index];
        m_free_registers.remove(min_index);
        return ScopedOperand { *this, Operand { reg } };
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

ScopedOperand Generator::local(FunctionLocal const& local)
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

Generator::UnwindContext::UnwindContext(Generator& generator, Optional<Label> handler)
    : m_generator(generator)
    , m_handler(handler)
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

    auto parent_environment = m_lexical_environment_register_stack.last();
    auto environment = allocate_register();
    emit<Bytecode::Op::CreateLexicalEnvironment>(environment, parent_environment, 0);
    m_lexical_environment_register_stack.append(environment);
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
            auto shared_data = ensure_shared_function_data(m_vm, function_declaration, function_declaration.name());
            auto data_index = register_shared_function_data(shared_data);
            auto fo = allocate_register();
            emit<Bytecode::Op::NewFunction>(fo, data_index, OptionalNone {}, OptionalNone {});

            // iii. Perform ! env.InitializeBinding(fn, fo). NOTE: This step is replaced in section B.3.2.6.
            if (function_declaration.name_identifier()->is_local()) {
                auto local_index = function_declaration.name_identifier()->local_index();
                if (local_index.is_variable()) {
                    emit<Bytecode::Op::Mov>(local(local_index), fo);
                    set_local_initialized(local_index);
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
    auto parent_environment = m_lexical_environment_register_stack.last();
    auto new_environment = allocate_register();
    start_boundary(BlockBoundaryType::LeaveLexicalEnvironment);
    emit<Bytecode::Op::CreateLexicalEnvironment>(new_environment, parent_environment, 0);
    m_lexical_environment_register_stack.append(new_environment);
}

void Generator::end_variable_scope()
{
    end_boundary(BlockBoundaryType::LeaveLexicalEnvironment);
    m_lexical_environment_register_stack.take_last();

    if (!m_current_basic_block->is_terminated())
        emit<Bytecode::Op::SetLexicalEnvironment>(m_lexical_environment_register_stack.last());
}

void Generator::ensure_lexical_environment_register_initialized()
{
    if (m_lexical_environment_register_stack.is_empty()) {
        auto environment_register = ScopedOperand { *this, Operand { Register::saved_lexical_environment() } };
        emit<Op::GetLexicalEnvironment>(environment_register);
        m_lexical_environment_register_stack.append(environment_register);
    }
}

ScopedOperand Generator::current_lexical_environment_register() const
{
    VERIFY(!m_lexical_environment_register_stack.is_empty());
    return m_lexical_environment_register_stack.last();
}

void Generator::push_lexical_environment_register(ScopedOperand const& environment)
{
    m_lexical_environment_register_stack.append(environment);
}

void Generator::pop_lexical_environment_register()
{
    m_lexical_environment_register_stack.take_last();
}

void Generator::begin_continuable_scope(Label continue_target, Vector<FlyString> const& language_label_set, Optional<ScopedOperand> completion_register)
{
    m_continuable_scopes.append({ continue_target, language_label_set, move(completion_register) });
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

void Generator::begin_breakable_scope(Label breakable_target, Vector<FlyString> const& language_label_set, Optional<ScopedOperand> completion_register)
{
    m_breakable_scopes.append({ breakable_target, language_label_set, move(completion_register) });
    start_boundary(BlockBoundaryType::Break);
}

void Generator::end_breakable_scope()
{
    m_breakable_scopes.take_last();
    end_boundary(BlockBoundaryType::Break);
}

Generator::ReferenceOperands Generator::emit_super_reference(MemberExpression const& expression)
{
    VERIFY(is<SuperExpression>(expression.object()));

    // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
    // 1. Let env be GetThisEnvironment().
    // 2. Let actualThis be ? env.GetThisBinding().
    auto actual_this = get_this();

    Optional<ScopedOperand> computed_property_value;
    Optional<PropertyKeyTableIndex> property_key_id;

    if (expression.is_computed()) {
        // SuperProperty : super [ Expression ]
        // 3. Let propertyNameReference be ? Evaluation of Expression.
        // 4. Let propertyNameValue be ? GetValue(propertyNameReference).
        computed_property_value = expression.property().generate_bytecode(*this).value();
    } else {
        // SuperProperty : super . IdentifierName
        // 3. Let propertyKey be the StringValue of IdentifierName.
        auto const identifier_name = as<Identifier>(expression.property()).string();
        property_key_id = intern_property_key(identifier_name);
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

Generator::ReferenceOperands Generator::emit_load_from_reference(JS::ASTNode const& node, Optional<ScopedOperand> preferred_dst, ReferenceMode mode)
{
    if (is<Identifier>(node)) {
        auto& identifier = static_cast<Identifier const&>(node);
        auto loaded_value = identifier.generate_bytecode(*this, preferred_dst).value();
        return ReferenceOperands {
            .loaded_value = loaded_value,
        };
    }
    if (!is<MemberExpression>(node)) {
        // Per spec, evaluate the expression (e.g. the call in f()++) before
        // throwing ReferenceError for invalid assignment target.
        (void)node.generate_bytecode(*this);
        auto exception = allocate_register();
        emit<Bytecode::Op::NewReferenceError>(exception, intern_string(ErrorType::InvalidLeftHandAssignment.message()));
        perform_needed_unwinds<Op::Throw>();
        emit<Bytecode::Op::Throw>(exception);
        switch_to_basic_block(make_block());
        auto dummy = add_constant(js_undefined());
        return ReferenceOperands {
            .base = dummy,
            .referenced_name = dummy,
            .this_value = dummy,
            .loaded_value = dummy,
        };
    }
    auto& expression = static_cast<MemberExpression const&>(node);

    // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
    if (is<SuperExpression>(expression.object())) {
        auto super_reference = emit_super_reference(expression);
        auto dst = preferred_dst.has_value() ? preferred_dst.value() : allocate_register();

        if (super_reference.referenced_name.has_value()) {
            // 5. Let propertyKey be ? ToPropertyKey(propertyNameValue).
            emit_get_by_value_with_this(dst, *super_reference.base, *super_reference.referenced_name, *super_reference.this_value);
        } else {
            // 3. Let propertyKey be StringValue of IdentifierName.
            auto property_key_table_index = intern_property_key(as<Identifier>(expression.property()).string());
            emit_get_by_id_with_this(dst, *super_reference.base, property_key_table_index, *super_reference.this_value);
        }

        super_reference.loaded_value = dst;
        return super_reference;
    }

    auto base = expression.object().generate_bytecode(*this).value();
    auto base_identifier = intern_identifier_for_expression(expression.object());

    if (expression.is_computed()) {
        auto property = expression.property().generate_bytecode(*this).value();
        auto dst = preferred_dst.has_value() ? preferred_dst.value() : allocate_register();
        emit_get_by_value(dst, base, property, move(base_identifier));
        if (mode == ReferenceMode::LoadOnly) {
            return ReferenceOperands {
                .loaded_value = dst,
            };
        }
        auto saved_property = allocate_register();
        emit<Bytecode::Op::Mov>(saved_property, property);
        return ReferenceOperands {
            .base = base,
            .referenced_name = saved_property,
            .this_value = base,
            .loaded_value = dst,
        };
    }
    if (expression.property().is_identifier()) {
        auto property_key_table_index = intern_property_key(as<Identifier>(expression.property()).string());
        auto dst = preferred_dst.has_value() ? preferred_dst.value() : allocate_register();
        emit_get_by_id(dst, base, property_key_table_index, move(base_identifier));
        return ReferenceOperands {
            .base = base,
            .referenced_identifier = property_key_table_index,
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
    VERIFY_NOT_REACHED();
}

Generator::ReferenceOperands Generator::emit_evaluate_reference(MemberExpression const& expression)
{
    // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
    if (is<SuperExpression>(expression.object())) {
        return emit_super_reference(expression);
    }

    auto base = expression.object().generate_bytecode(*this).value();

    if (expression.is_computed()) {
        auto property = expression.property().generate_bytecode(*this).value();
        auto saved_property = allocate_register();
        emit<Bytecode::Op::Mov>(saved_property, property);
        return ReferenceOperands {
            .base = base,
            .referenced_name = saved_property,
            .this_value = base,
        };
    }
    if (expression.property().is_identifier()) {
        auto property_key_table_index = intern_property_key(as<Identifier>(expression.property()).string());
        return ReferenceOperands {
            .base = base,
            .referenced_identifier = property_key_table_index,
            .this_value = base,
        };
    }
    if (expression.property().is_private_identifier()) {
        auto identifier_table_ref = intern_identifier(as<PrivateIdentifier>(expression.property()).string());
        return ReferenceOperands {
            .base = base,
            .referenced_private_identifier = identifier_table_ref,
            .this_value = base,
        };
    }
    VERIFY_NOT_REACHED();
}

void Generator::emit_store_to_reference(JS::ASTNode const& node, ScopedOperand value)
{
    if (is<Identifier>(node)) {
        auto& identifier = static_cast<Identifier const&>(node);
        emit_set_variable(identifier, value);
        return;
    }
    if (is<MemberExpression>(node)) {
        auto& expression = static_cast<MemberExpression const&>(node);

        // https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
        if (is<SuperExpression>(expression.object())) {
            auto super_reference = emit_super_reference(expression);

            // 4. Return the Reference Record { [[Base]]: baseValue, [[ReferencedName]]: propertyKey, [[Strict]]: strict, [[ThisValue]]: actualThis }.
            if (super_reference.referenced_name.has_value()) {
                // 5. Let propertyKey be ? ToPropertyKey(propertyNameValue).
                emit_put_by_value_with_this(*super_reference.base, *super_reference.referenced_name, *super_reference.this_value, value, PutKind::Normal);
            } else {
                // 3. Let propertyKey be StringValue of IdentifierName.
                auto property_key_table_index = intern_property_key(as<Identifier>(expression.property()).string());
                emit<Bytecode::Op::PutNormalByIdWithThis>(*super_reference.base, *super_reference.this_value, property_key_table_index, value, next_property_lookup_cache());
            }
        } else {
            auto object = expression.object().generate_bytecode(*this).value();

            if (expression.is_computed()) {
                auto property = expression.property().generate_bytecode(*this).value();
                emit_put_by_value(object, property, value, PutKind::Normal, {});
            } else if (expression.property().is_identifier()) {
                auto property_key_table_index = intern_property_key(as<Identifier>(expression.property()).string());
                emit_put_by_id(object, property_key_table_index, value, Bytecode::PutKind::Normal, next_property_lookup_cache());
            } else if (expression.property().is_private_identifier()) {
                auto identifier_table_ref = intern_identifier(as<PrivateIdentifier>(expression.property()).string());
                emit<Bytecode::Op::PutPrivateById>(object, identifier_table_ref, value);
            } else {
                VERIFY_NOT_REACHED();
            }
        }

        return;
    }

    // Per spec, evaluate the expression (e.g. the call in for(f() in ...))
    // before throwing ReferenceError for invalid assignment target.
    (void)node.generate_bytecode(*this);
    auto exception = allocate_register();
    emit<Bytecode::Op::NewReferenceError>(exception, intern_string(ErrorType::InvalidLeftHandAssignment.message()));
    perform_needed_unwinds<Op::Throw>();
    emit<Bytecode::Op::Throw>(exception);
    switch_to_basic_block(make_block());
}

void Generator::emit_store_to_reference(ReferenceOperands const& reference, ScopedOperand value)
{
    if (reference.referenced_private_identifier.has_value()) {
        emit<Bytecode::Op::PutPrivateById>(*reference.base, *reference.referenced_private_identifier, value);
        return;
    }
    if (reference.referenced_identifier.has_value()) {
        if (reference.base == reference.this_value)
            emit_put_by_id(*reference.base, *reference.referenced_identifier, value, Bytecode::PutKind::Normal, next_property_lookup_cache());
        else
            emit<Bytecode::Op::PutNormalByIdWithThis>(*reference.base, *reference.this_value, *reference.referenced_identifier, value, next_property_lookup_cache());
        return;
    }
    if (reference.base == reference.this_value)
        emit_put_by_value(*reference.base, *reference.referenced_name, value, PutKind::Normal, {});
    else
        emit_put_by_value_with_this(*reference.base, *reference.referenced_name, *reference.this_value, value, PutKind::Normal);
    return;
}

Optional<ScopedOperand> Generator::emit_delete_reference(JS::ASTNode const& node)
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
            auto super_reference = emit_super_reference(expression);

            auto exception = allocate_register();
            emit<Bytecode::Op::NewReferenceError>(exception, intern_string(ErrorType::UnsupportedDeleteSuperProperty.message()));
            perform_needed_unwinds<Op::Throw>();
            emit<Bytecode::Op::Throw>(exception);

            // Switch to a new block so callers can continue emitting code
            // (which will be unreachable, but avoids a terminated-block VERIFY).
            switch_to_basic_block(make_block());
            return add_constant(js_undefined());
        }

        auto object = expression.object().generate_bytecode(*this).value();
        auto dst = allocate_register();

        if (expression.is_computed()) {
            auto property = expression.property().generate_bytecode(*this).value();
            emit<Bytecode::Op::DeleteByValue>(dst, object, property);
        } else if (expression.property().is_identifier()) {
            auto property_key_table_index = intern_property_key(as<Identifier>(expression.property()).string());
            emit<Bytecode::Op::DeleteById>(dst, object, property_key_table_index);
        } else {
            // NB: Trying to delete a private field generates a SyntaxError in the parser.
            VERIFY_NOT_REACHED();
        }
        return dst;
    }

    // Though this will have no deletion effect, we still have to evaluate the node as it can have side effects.
    // For example: delete a(); delete ++c.b; etc.

    // 13.5.1.2 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
    // 1. Let ref be the result of evaluating UnaryExpression.
    // 2. ReturnIfAbrupt(ref).
    (void)node.generate_bytecode(*this);

    // 3. If ref is not a Reference Record, return true.
    // NOTE: The rest of the steps are handled by Delete{Variable,ByValue,Id}.
    return add_constant(Value(true));
}

void Generator::emit_set_variable(JS::Identifier const& identifier, ScopedOperand value, Bytecode::Op::BindingInitializationMode initialization_mode, Bytecode::Op::EnvironmentMode environment_mode)
{
    if (identifier.is_local()) {
        if (initialization_mode == Bytecode::Op::BindingInitializationMode::Set && identifier.declaration_kind() == DeclarationKind::Const) {
            emit<Bytecode::Op::ThrowConstAssignment>();
            return;
        }
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

    if (is<ThisExpression>(expression))
        return "this"_utf16;

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

// Scans outward from boundary_index looking for another ReturnToFinally boundary
// between the current position and the break/continue target. If found, the jump
// must chain through multiple finally blocks via trampolines rather than jumping
// directly to the target after a single finally.
bool Generator::has_outer_finally_before_target(JumpType type, size_t boundary_index) const
{
    using enum BlockBoundaryType;
    for (size_t j = boundary_index - 1; j > 0; --j) {
        auto inner = m_boundaries[j - 1];
        if ((type == JumpType::Break && inner == Break) || (type == JumpType::Continue && inner == Continue))
            return false;
        if (inner == ReturnToFinally)
            return true;
    }
    return false;
}

// Register a jump target with the current FinallyContext. Assigns a unique
// completion_type index, records the target in registered_jumps (so the
// after-finally dispatch chain can route to it), and emits bytecode to set
// completion_type and jump to the finally body.
void Generator::register_jump_in_finally_context(Label target)
{
    VERIFY(m_current_finally_context);
    auto& finally_context = *m_current_finally_context;
    VERIFY(finally_context.next_jump_index < NumericLimits<i32>::max());
    auto jump_index = finally_context.next_jump_index++;
    finally_context.registered_jumps.append({ jump_index, target });
    emit_mov(finally_context.completion_type, add_constant(Value(jump_index)));
    emit<Op::Jump>(finally_context.finally_body);
}

// For break/continue through nested finally blocks: creates an intermediate
// "trampoline" block that the inner finally dispatches to, which then continues
// unwinding through the next outer finally. Each trampoline is registered as a
// jump target in the inner finally's dispatch chain.
void Generator::emit_trampoline_through_finally(JumpType type)
{
    VERIFY(m_current_finally_context);
    auto block_name = MUST(String::formatted("{}.{}", current_block().name(), type == JumpType::Break ? "break"sv : "continue"sv));
    auto& trampoline_block = make_block(block_name);
    register_jump_in_finally_context(Label { trampoline_block });
    switch_to_basic_block(trampoline_block);
    m_current_unwind_context = m_current_unwind_context->previous();
    m_current_finally_context = m_current_finally_context->parent;
}

void Generator::generate_scoped_jump(JumpType type)
{
    TemporaryChange temp { m_current_unwind_context, m_current_unwind_context };
    TemporaryChange finally_temp { m_current_finally_context, m_current_finally_context };
    auto environment_stack_offset = m_lexical_environment_register_stack.size();
    for (size_t i = m_boundaries.size(); i > 0; --i) {
        auto boundary = m_boundaries[i - 1];
        using enum BlockBoundaryType;
        switch (boundary) {
        case Break:
            if (type == JumpType::Break) {
                auto const& target_scope = m_breakable_scopes.last();
                if (m_current_completion_register.has_value() && target_scope.completion_register.has_value()
                    && *m_current_completion_register != *target_scope.completion_register) {
                    emit_mov(*target_scope.completion_register, *m_current_completion_register);
                }
                emit<Op::Jump>(target_scope.bytecode_target);
                return;
            }
            break;
        case Continue:
            if (type == JumpType::Continue) {
                auto const& target_scope = m_continuable_scopes.last();
                if (m_current_completion_register.has_value() && target_scope.completion_register.has_value()
                    && *m_current_completion_register != *target_scope.completion_register) {
                    emit_mov(*target_scope.completion_register, *m_current_completion_register);
                }
                emit<Op::Jump>(target_scope.bytecode_target);
                return;
            }
            break;
        case LeaveLexicalEnvironment:
            --environment_stack_offset;
            emit<Bytecode::Op::SetLexicalEnvironment>(m_lexical_environment_register_stack[environment_stack_offset - 1]);
            break;
        case ReturnToFinally: {
            VERIFY(m_current_finally_context);
            if (!has_outer_finally_before_target(type, i)) {
                auto const& target_scope = type == JumpType::Break ? m_breakable_scopes.last() : m_continuable_scopes.last();
                if (m_current_completion_register.has_value() && target_scope.completion_register.has_value()
                    && *m_current_completion_register != *target_scope.completion_register) {
                    emit_mov(*target_scope.completion_register, *m_current_completion_register);
                }
                register_jump_in_finally_context(target_scope.bytecode_target);
                return;
            }
            emit_trampoline_through_finally(type);
            break;
        }
        case LeaveFinally:
            break;
        }
    }
    VERIFY_NOT_REACHED();
}

void Generator::generate_labelled_jump(JumpType type, FlyString const& label)
{
    TemporaryChange temp { m_current_unwind_context, m_current_unwind_context };
    TemporaryChange finally_temp { m_current_finally_context, m_current_finally_context };
    size_t current_boundary = m_boundaries.size();
    auto environment_stack_offset = m_lexical_environment_register_stack.size();

    auto const& jumpable_scopes = type == JumpType::Continue ? m_continuable_scopes : m_breakable_scopes;

    for (auto const& jumpable_scope : jumpable_scopes.in_reverse()) {
        for (; current_boundary > 0; --current_boundary) {
            auto boundary = m_boundaries[current_boundary - 1];
            if (boundary == BlockBoundaryType::LeaveLexicalEnvironment) {
                --environment_stack_offset;
                emit<Bytecode::Op::SetLexicalEnvironment>(m_lexical_environment_register_stack[environment_stack_offset - 1]);
            } else if (boundary == BlockBoundaryType::ReturnToFinally) {
                VERIFY(m_current_finally_context);
                if (!has_outer_finally_before_target(type, current_boundary) && jumpable_scope.language_label_set.contains_slow(label)) {
                    if (m_current_completion_register.has_value() && jumpable_scope.completion_register.has_value()
                        && *m_current_completion_register != *jumpable_scope.completion_register) {
                        emit_mov(*jumpable_scope.completion_register, *m_current_completion_register);
                    }
                    register_jump_in_finally_context(jumpable_scope.bytecode_target);
                    return;
                }
                emit_trampoline_through_finally(type);
            } else if ((type == JumpType::Continue && boundary == BlockBoundaryType::Continue) || (type == JumpType::Break && boundary == BlockBoundaryType::Break)) {
                // Make sure we don't process this boundary twice if the current jumpable scope doesn't contain the target label.
                --current_boundary;
                break;
            }
        }

        if (jumpable_scope.language_label_set.contains_slow(label)) {
            if (m_current_completion_register.has_value() && jumpable_scope.completion_register.has_value()
                && *m_current_completion_register != *jumpable_scope.completion_register) {
                emit_mov(*jumpable_scope.completion_register, *m_current_completion_register);
            }
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

void Generator::emit_new_function(ScopedOperand dst, FunctionExpression const& function_node, Optional<IdentifierTableIndex> lhs_name, bool is_method)
{
    Utf16FlyString name;
    if (function_node.has_name())
        name = function_node.name();
    else if (lhs_name.has_value())
        name = m_identifier_table->get(lhs_name.value());

    auto shared_data = ensure_shared_function_data(m_vm, function_node, move(name));
    auto data_index = register_shared_function_data(shared_data);

    if (!is_method || m_home_objects.is_empty()) {
        emit<Op::NewFunction>(dst, data_index, lhs_name, OptionalNone {});
    } else {
        emit<Op::NewFunction>(dst, data_index, lhs_name, m_home_objects.last());
    }
}

ScopedOperand Generator::emit_named_evaluation_if_anonymous_function(Expression const& expression, Optional<IdentifierTableIndex> lhs_name, Optional<ScopedOperand> preferred_dst, bool is_method)
{
    if (is<FunctionExpression>(expression)) {
        auto const& function_expression = static_cast<FunctionExpression const&>(expression);
        if (!function_expression.has_name()) {
            return function_expression.generate_bytecode_with_lhs_name(*this, move(lhs_name), preferred_dst, is_method).value();
        }
    }

    if (is<ClassExpression>(expression)) {
        auto const& class_expression = static_cast<ClassExpression const&>(expression);
        if (!class_expression.has_name()) {
            return class_expression.generate_bytecode_with_lhs_name(*this, move(lhs_name), preferred_dst).value();
        }
    }

    return expression.generate_bytecode(*this, preferred_dst).value();
}

void Generator::emit_get_by_id(ScopedOperand dst, ScopedOperand base, PropertyKeyTableIndex property_key_table_index, Optional<IdentifierTableIndex> base_identifier)
{
    auto& property_key = m_property_key_table->get(property_key_table_index);
    if (property_key.is_string() && property_key.as_string() == "length"sv) {
        m_length_identifier = property_key_table_index;
        emit<Op::GetLength>(dst, base, move(base_identifier), m_next_property_lookup_cache++);
        return;
    }
    emit<Op::GetById>(dst, base, property_key_table_index, move(base_identifier), m_next_property_lookup_cache++);
}

void Generator::emit_get_by_id_with_this(ScopedOperand dst, ScopedOperand base, PropertyKeyTableIndex id, ScopedOperand this_value)
{
    if (m_property_key_table->get(id).as_string() == "length"sv) {
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
            emit_get_by_id(dst, base, intern_property_key(property_key.as_string()), base_identifier);
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
            emit_get_by_id_with_this(dst, base, intern_property_key(property_key.as_string()), this_value);
            return;
        }
    }
    emit<Op::GetByValueWithThis>(dst, base, property, this_value);
}

void Generator::emit_put_by_id(Operand base, PropertyKeyTableIndex property, Operand src, PutKind kind, u32 cache_index, Optional<IdentifierTableIndex> base_identifier)
{
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
            emit_put_by_id(base, intern_property_key(property_key.as_string()), src, kind, m_next_property_lookup_cache++, base_identifier);
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
#define EMIT_PUT_BY_ID_WITH_THIS(kind)                                                                                               \
    case PutKind::kind:                                                                                                              \
        emit<Op::Put##kind##ByIdWithThis>(base, this_value, intern_property_key(property_key), src, m_next_property_lookup_cache++); \
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
    emit_get_by_id(dst, result, intern_property_key("value"_utf16_fly_string));
}

void Generator::emit_iterator_complete(ScopedOperand dst, ScopedOperand result)
{
    emit_get_by_id(dst, result, intern_property_key("done"_utf16_fly_string));
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

void Generator::set_local_initialized(FunctionLocal const& local)
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

void Generator::emit_tdz_check_if_needed(Identifier const& identifier)
{
    VERIFY(identifier.is_local());
    auto local_index = identifier.local_index();
    bool needs_tdz_check = local_index.is_argument()
        ? !is_local_initialized(local_index)
        : is_local_lexically_declared(local_index) && !is_local_initialized(local_index);
    if (needs_tdz_check) {
        auto operand = local(local_index);
        if (local_index.is_argument()) {
            // Arguments are initialized to undefined by default, so here we need to replace it
            // with the empty value to trigger the TDZ check.
            emit<Bytecode::Op::Mov>(operand, add_constant(js_special_empty_value()));
        }
        emit<Bytecode::Op::ThrowIfTDZ>(operand);
    }
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
    if (m_shared_function_instance_data && !m_shared_function_instance_data->m_function_environment_needed)
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

void Generator::emit_todo(StringView message)
{
    auto error_message = MUST(String::formatted("TODO: {}", message));
    auto message_string = intern_string(Utf16String::from_utf8(error_message));
    auto error_register = allocate_register();
    emit<Op::NewTypeError>(error_register, message_string);
    perform_needed_unwinds<Op::Throw>();
    emit<Op::Throw>(error_register);
    // Switch to a new block so subsequent codegen doesn't crash trying to
    // emit into a terminated block.
    auto& dead_block = make_block("dead"_string);
    switch_to_basic_block(dead_block);
}

void Generator::emit_jump_if(ScopedOperand const& condition, Label true_target, Label false_target)
{
    if (condition.operand().is_constant()) {
        auto value = get_constant(condition);

        auto is_always_true = value.to_boolean_slow_case();
        emit<Op::Jump>(is_always_true ? true_target : false_target);
        return;
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

void Generator::generate_builtin_abstract_operation(Identifier const& builtin_identifier, ReadonlySpan<CallExpression::Argument> arguments, ScopedOperand const& dst)
{
    VERIFY(m_builtin_abstract_operations_enabled);
    for (auto const& argument : arguments)
        VERIFY(!argument.is_spread);

    auto const& operation_name = builtin_identifier.string();

    if (operation_name == "IsCallable"sv) {
        VERIFY(arguments.size() == 1);
        auto source = arguments[0].value->generate_bytecode(*this).value();
        emit<Op::IsCallable>(dst, source);
        return;
    }

    if (operation_name == "IsConstructor"sv) {
        VERIFY(arguments.size() == 1);
        auto source = arguments[0].value->generate_bytecode(*this).value();
        emit<Op::IsConstructor>(dst, source);
        return;
    }

    if (operation_name == "ToBoolean"sv) {
        VERIFY(arguments.size() == 1);
        auto source = arguments[0].value->generate_bytecode(*this).value();
        emit<Op::ToBoolean>(dst, source);
        return;
    }

    if (operation_name == "ToObject"sv) {
        VERIFY(arguments.size() == 1);
        auto source = arguments[0].value->generate_bytecode(*this).value();
        emit<Op::ToObject>(dst, source);
        return;
    }

    if (operation_name == "ThrowTypeError"sv) {
        VERIFY(arguments.size() == 1);
        auto const* message = as_if<StringLiteral>(*arguments[0].value);
        VERIFY(message);

        auto message_string = intern_string(message->value());
        auto type_error_register = allocate_register();
        emit<Op::NewTypeError>(type_error_register, message_string);
        perform_needed_unwinds<Op::Throw>();
        emit<Op::Throw>(type_error_register);
        return;
    }

    if (operation_name == "ThrowIfNotObject"sv) {
        VERIFY(arguments.size() == 1);
        auto source = arguments[0].value->generate_bytecode(*this).value();
        emit<Op::ThrowIfNotObject>(source);
        return;
    }

    if (operation_name == "Call"sv) {
        VERIFY(arguments.size() >= 2);

        auto const& callee_argument = arguments[0].value;
        auto callee = callee_argument->generate_bytecode(*this).value();
        auto this_value = arguments[1].value->generate_bytecode(*this).value();
        auto arguments_to_call_with = arguments.slice(2);

        Vector<ScopedOperand> argument_operands;
        argument_operands.ensure_capacity(arguments_to_call_with.size());
        for (auto const& argument : arguments_to_call_with) {
            auto argument_value = argument.value->generate_bytecode(*this).value();
            argument_operands.unchecked_append(copy_if_needed_to_preserve_evaluation_order(argument_value));
        }

        auto expression_string = ([&callee_argument] -> Optional<Utf16String> {
            if (auto const* identifier = as_if<Identifier>(*callee_argument))
                return identifier->string().to_utf16_string();

            if (auto const* member_expression = as_if<MemberExpression>(*callee_argument))
                return member_expression->to_string_approximation();

            return {};
        })();

        Optional<Bytecode::StringTableIndex> expression_string_index;
        if (expression_string.has_value())
            expression_string_index = intern_string(expression_string.release_value());

        emit_with_extra_operand_slots<Bytecode::Op::Call>(
            argument_operands.size(),
            dst,
            callee,
            this_value,
            expression_string_index,
            argument_operands);
        return;
    }

    if (operation_name == "NewObjectWithNoPrototype"sv) {
        VERIFY(arguments.is_empty());
        emit<Op::NewObjectWithNoPrototype>(dst);
        return;
    }

    if (operation_name == "CreateAsyncFromSyncIterator"sv) {
        VERIFY(arguments.size() == 3);
        auto iterator = arguments[0].value->generate_bytecode(*this).value();
        auto next_method = arguments[1].value->generate_bytecode(*this).value();
        auto done = arguments[2].value->generate_bytecode(*this).value();

        emit<Op::CreateAsyncFromSyncIterator>(dst, iterator, next_method, done);
        return;
    }

    if (operation_name == "ToLength"sv) {
        VERIFY(arguments.size() == 1);
        auto value = arguments[0].value->generate_bytecode(*this).value();
        emit<Op::ToLength>(dst, value);
        return;
    }

    if (operation_name == "NewTypeError"sv) {
        VERIFY(arguments.size() == 1);
        auto const* message = as_if<StringLiteral>(*arguments[0].value);
        VERIFY(message);

        auto message_string = intern_string(message->value());
        emit<Op::NewTypeError>(dst, message_string);
        return;
    }

    if (operation_name == "NewArrayWithLength"sv) {
        VERIFY(arguments.size() == 1);
        auto length = arguments[0].value->generate_bytecode(*this).value();
        emit<Op::NewArrayWithLength>(dst, length);
        return;
    }

    if (operation_name == "CreateDataPropertyOrThrow"sv) {
        VERIFY(arguments.size() == 3);
        auto object = arguments[0].value->generate_bytecode(*this).value();
        auto property = arguments[1].value->generate_bytecode(*this).value();
        auto value = arguments[2].value->generate_bytecode(*this).value();
        emit<Op::CreateDataPropertyOrThrow>(object, property, value);
        return;
    }

#define __JS_ENUMERATE(snake_name, functionName, length)                                                     \
    if (operation_name == #functionName##sv) {                                                               \
        Vector<ScopedOperand> argument_operands;                                                             \
        argument_operands.ensure_capacity(arguments.size());                                                 \
        for (auto const& argument : arguments) {                                                             \
            auto argument_value = argument.value->generate_bytecode(*this).value();                          \
            argument_operands.unchecked_append(copy_if_needed_to_preserve_evaluation_order(argument_value)); \
        }                                                                                                    \
        emit_with_extra_operand_slots<Bytecode::Op::Call>(                                                   \
            argument_operands.size(),                                                                        \
            dst,                                                                                             \
            add_constant(m_vm.current_realm()->intrinsics().snake_name##_abstract_operation_function()),     \
            add_constant(js_undefined()),                                                                    \
            intern_string(builtin_identifier.string().to_utf16_string()),                                    \
            argument_operands);                                                                              \
        return;                                                                                              \
    }
    JS_ENUMERATE_NATIVE_JAVASCRIPT_BACKED_ABSTRACT_OPERATIONS
#undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

Optional<ScopedOperand> Generator::maybe_generate_builtin_constant(Identifier const& builtin_identifier)
{
    auto const& constant_name = builtin_identifier.string();

    if (constant_name == "undefined"sv) {
        return add_constant(js_undefined());
    }

    if (constant_name == "NaN"sv) {
        return add_constant(js_nan());
    }

    if (constant_name == "Infinity"sv) {
        return add_constant(js_infinity());
    }

    if (!m_builtin_abstract_operations_enabled)
        return OptionalNone {};

    if (constant_name == "SYMBOL_ITERATOR"sv) {
        return add_constant(vm().well_known_symbol_iterator());
    }

    if (constant_name == "SYMBOL_ASYNC_ITERATOR"sv) {
        return add_constant(vm().well_known_symbol_async_iterator());
    }

    if (constant_name == "MAX_ARRAY_LIKE_INDEX"sv) {
        return add_constant(Value(MAX_ARRAY_LIKE_INDEX));
    }

    VERIFY_NOT_REACHED();
}

}
