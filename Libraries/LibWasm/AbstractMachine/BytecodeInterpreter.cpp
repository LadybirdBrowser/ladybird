/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AK/QuickSort.h"

#include <AK/ByteReader.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <AK/SIMDExtras.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Operators.h>
#include <LibWasm/Opcode.h>
#include <LibWasm/Printer/Printer.h>

using namespace AK::SIMD;

namespace Wasm {

#define TRAP_IF_NOT(x, ...)                                                                    \
    do {                                                                                       \
        if (trap_if_not(x, #x##sv __VA_OPT__(, ) __VA_ARGS__)) {                               \
            dbgln_if(WASM_TRACE_DEBUG, "Trapped because {} failed, at line {}", #x, __LINE__); \
            return true;                                                                       \
        }                                                                                      \
    } while (false)

#define TRAP_IN_LOOP_IF_NOT(x, ...)                                                            \
    do {                                                                                       \
        if (trap_if_not(x, #x##sv __VA_OPT__(, ) __VA_ARGS__)) {                               \
            dbgln_if(WASM_TRACE_DEBUG, "Trapped because {} failed, at line {}", #x, __LINE__); \
            return;                                                                            \
        }                                                                                      \
    } while (false)

void BytecodeInterpreter::interpret(Configuration& configuration)
{
    m_trap = Empty {};
    auto& instructions = configuration.frame().expression().instructions();
    auto max_ip_value = InstructionPointer { instructions.size() };
    auto& current_ip_value = configuration.ip();
    auto const should_limit_instruction_count = configuration.should_limit_instruction_count();
    u64 executed_instructions = 0;

    // static Array<u64, 256> instruction_counts {0};
    // static ScopeGuard instruction_counts_guard = [&] {
    //     for (size_t i = 0; i < instruction_counts.size(); ++i) {
    //         if (instruction_counts[i] > 0)
    //             dbgln("Instruction {} executed {} times", instruction_name(OpCode { i }), instruction_counts[i]);
    //     }
    // };

    while (current_ip_value < max_ip_value) {
        if (should_limit_instruction_count) {
            if (executed_instructions++ >= Constants::max_allowed_executed_instructions_per_call) [[unlikely]] {
                m_trap = Trap::from_string("Exceeded maximum allowed number of instructions");
                return;
            }
        }
        // bounds checked by loop condition.
        auto old_ip = current_ip_value;
        {
            enum class CouldHaveChangedIP {
                No,
                Yes
            };

#define DISPATCH()                                                   \
    if (opcode = instruction->opcode().value(); opcode < 256) \
        goto* single_byte_handlers[opcode];                          \
    goto start;

#define RUN_NEXT_INSTRUCTION(could_have_changed_ip)                                                                                                                                   \
    do {                                                                                                                                                                              \
        if constexpr (could_have_changed_ip == CouldHaveChangedIP::Yes) {                                                                                                             \
            if (current_ip_value == old_ip)                                                                                                                                           \
                ++current_ip_value;                                                                                                                                                   \
        } else {                                                                                                                                                                      \
            ++current_ip_value;                                                                                                                                                       \
        }                                                                                                                                                                             \
        if (current_ip_value >= max_ip_value || (configuration.should_limit_instruction_count() && ++executed_instructions >= Constants::max_allowed_executed_instructions_per_call)) [[unlikely]] \
            goto continue_interpretation;                                                                                                                                             \
        old_ip = current_ip_value;                                                                                                                                                    \
        instruction = &instructions.data()[old_ip.value()];                                                                                                                           \
        DISPATCH();                                                                                                                                                                   \
    } while (0)

            static constexpr void* single_byte_handlers[256] {
#define M(opcode, value) [value] = &&handle_##opcode,
                ENUMERATE_SINGLE_BYTE_WASM_OPCODES(M)
#undef M
            };

#define M(opcode, value) (void)&&handle_##opcode;
            ENUMERATE_MULTI_BYTE_WASM_OPCODES(M)
#undef M

            auto* instruction = &instructions.data()[old_ip.value()];
            auto opcode = instruction->opcode().value();
            if (opcode < 256)
                goto* single_byte_handlers[opcode];

        start:
            dbgln_if(WASM_TRACE_DEBUG, "Executing instruction {} at current_ip_value {}", instruction_name(instruction->opcode()), current_ip_value.value());
            switch (opcode) {
            handle_unreachable:
            case Instructions::unreachable.value():
                m_trap = Trap::from_string("Unreachable");
                return;
            handle_nop:
            case Instructions::nop.value():
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_local_get:
            case Instructions::local_get.value():
                // bounds checked by verifier.
                configuration.value_stack().unchecked_append(Value(configuration.frame().locals().data()[instruction->local_index().value()]));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_local_set:
            case Instructions::local_set.value(): {
                // bounds checked by verifier.
                auto value = configuration.value_stack().unsafe_take_last();
                configuration.frame().locals().data()[instruction->local_index().value()] = value;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_i32_const:
            case Instructions::i32_const.value():
                configuration.value_stack().unchecked_append(Value(instruction->arguments().get<i32>()));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_i64_const:
            case Instructions::i64_const.value():
                configuration.value_stack().unchecked_append(Value(instruction->arguments().get<i64>()));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_f32_const:
            case Instructions::f32_const.value():
                configuration.value_stack().unchecked_append(Value(instruction->arguments().get<float>()));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_f64_const:
            case Instructions::f64_const.value():
                configuration.value_stack().unchecked_append(Value(instruction->arguments().get<double>()));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_block:
            case Instructions::block.value(): {
                size_t arity = 0;
                size_t param_arity = 0;
                auto& args = instruction->arguments().get<Instruction::StructuredInstructionArgs>();
                switch (args.block_type.kind()) {
                case BlockType::Empty:
                    break;
                case BlockType::Type:
                    arity = 1;
                    break;
                case BlockType::Index: {
                    auto& type = configuration.frame().module().types()[args.block_type.type_index().value()];
                    arity = type.results().size();
                    param_arity = type.parameters().size();
                }
                }

                configuration.label_stack().append(Label(arity, args.end_ip, configuration.value_stack().size() - param_arity));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }
            handle_loop:
            case Instructions::loop.value(): {
                auto& args = instruction->arguments().get<Instruction::StructuredInstructionArgs>();
                size_t arity = 0;
                if (args.block_type.kind() == BlockType::Index) {
                    auto& type = configuration.frame().module().types()[args.block_type.type_index().value()];
                    arity = type.parameters().size();
                }
                configuration.label_stack().append(Label(arity, current_ip_value.value() + 1, configuration.value_stack().size() - arity));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }
            handle_if_:
            case Instructions::if_.value(): {
                size_t arity = 0;
                size_t param_arity = 0;
                auto& args = instruction->arguments().get<Instruction::StructuredInstructionArgs>();
                switch (args.block_type.kind()) {
                case BlockType::Empty:
                    break;
                case BlockType::Type:
                    arity = 1;
                    break;
                case BlockType::Index: {
                    auto& type = configuration.frame().module().types()[args.block_type.type_index().value()];
                    arity = type.results().size();
                    param_arity = type.parameters().size();
                }
                }

                auto value = configuration.value_stack().unsafe_take_last().to<i32>();
                auto end_label = Label(arity, args.end_ip.value(), configuration.value_stack().size() - param_arity);
                if (value == 0) {
                    if (args.else_ip.has_value()) {
                        configuration.ip() = args.else_ip.value();
                        configuration.label_stack().append(end_label);
                    } else {
                        configuration.ip() = args.end_ip.value() + 1;
                    }
                } else {
                    configuration.label_stack().append(end_label);
                }
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }
            handle_structured_end:
            case Instructions::structured_end.value():
                configuration.label_stack().take_last();
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_structured_else:
            case Instructions::structured_else.value(): {
                auto label = configuration.label_stack().take_last();
                // Jump to the end label
                configuration.ip() = label.continuation();
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }
            handle_return_:
            case Instructions::return_.value(): {
                while (configuration.label_stack().size() - 1 != configuration.frame().label_index())
                    configuration.label_stack().take_last();
                configuration.ip() = configuration.frame().expression().instructions().size();
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }
            handle_br:
            case Instructions::br.value():
                branch_to_label(configuration, instruction->arguments().get<LabelIndex>());
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_br_if:
            case Instructions::br_if.value(): {
                // bounds checked by verifier.
                auto cond = configuration.value_stack().unsafe_take_last().to<i32>();
                if (cond == 0)
                    RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
                branch_to_label(configuration, instruction->arguments().get<LabelIndex>());
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }
            handle_br_table:
            case Instructions::br_table.value(): {
                auto& arguments = instruction->arguments().get<Instruction::TableBranchArgs>();
                auto i = configuration.value_stack().unsafe_take_last().to<u32>();

                if (i >= arguments.labels.size()) {
                    branch_to_label(configuration, arguments.default_);
                    RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
                }
                branch_to_label(configuration, arguments.labels[i]);
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }
            handle_call:
            case Instructions::call.value(): {
                auto index = instruction->arguments().get<FunctionIndex>();
                auto address = configuration.frame().module().functions()[index.value()];
                dbgln_if(WASM_TRACE_DEBUG, "call({})", address.value());
                if (call_address(configuration, address))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }
            handle_call_indirect:
            case Instructions::call_indirect.value(): {
                auto& args = instruction->arguments().get<Instruction::IndirectCallArgs>();
                auto table_address = configuration.frame().module().tables()[args.table.value()];
                auto table_instance = configuration.store().get(table_address);
                // bounds checked by verifier.
                auto index = configuration.value_stack().unsafe_take_last().to<i32>();
                TRAP_IN_LOOP_IF_NOT(index >= 0);
                TRAP_IN_LOOP_IF_NOT(static_cast<size_t>(index) < table_instance->elements().size());
                auto& element = table_instance->elements()[index];
                TRAP_IN_LOOP_IF_NOT(element.ref().has<Reference::Func>());
                auto address = element.ref().get<Reference::Func>().address;
                auto const& type_actual = configuration.store().get(address)->visit([](auto& f) -> decltype(auto) { return f.type(); });
                auto const& type_expected = configuration.frame().module().types()[args.type.value()];
                TRAP_IN_LOOP_IF_NOT(type_actual.parameters().size() == type_expected.parameters().size());
                TRAP_IN_LOOP_IF_NOT(type_actual.results().size() == type_expected.results().size());
                TRAP_IN_LOOP_IF_NOT(type_actual.parameters() == type_expected.parameters());
                TRAP_IN_LOOP_IF_NOT(type_actual.results() == type_expected.results());

                dbgln_if(WASM_TRACE_DEBUG, "call_indirect({} -> {})", index, address.value());
                if (call_address(configuration, address, CallAddressSource::IndirectCall))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }
            handle_i32_load:
            case Instructions::i32_load.value():
                if (load_and_push<i32, i32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_load:
            case Instructions::i64_load.value():
                if (load_and_push<i64, i64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_load:
            case Instructions::f32_load.value():
                if (load_and_push<float, float>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_load:
            case Instructions::f64_load.value():
                if (load_and_push<double, double>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_load8_s:
            case Instructions::i32_load8_s.value():
                if (load_and_push<i8, i32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_load8_u:
            case Instructions::i32_load8_u.value():
                if (load_and_push<u8, i32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_load16_s:
            case Instructions::i32_load16_s.value():
                if (load_and_push<i16, i32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_load16_u:
            case Instructions::i32_load16_u.value():
                if (load_and_push<u16, i32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_load8_s:
            case Instructions::i64_load8_s.value():
                if (load_and_push<i8, i64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_load8_u:
            case Instructions::i64_load8_u.value():
                if (load_and_push<u8, i64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_load16_s:
            case Instructions::i64_load16_s.value():
                if (load_and_push<i16, i64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_load16_u:
            case Instructions::i64_load16_u.value():
                if (load_and_push<u16, i64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_load32_s:
            case Instructions::i64_load32_s.value():
                if (load_and_push<i32, i64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_load32_u:
            case Instructions::i64_load32_u.value():
                if (load_and_push<u32, i64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_store:
            case Instructions::i32_store.value():
                if (pop_and_store<i32, i32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_store:
            case Instructions::i64_store.value():
                if (pop_and_store<i64, i64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_store:
            case Instructions::f32_store.value():
                if (pop_and_store<float, float>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_store:
            case Instructions::f64_store.value():
                if (pop_and_store<double, double>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_store8:
            case Instructions::i32_store8.value():
                if (pop_and_store<i32, i8>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_store16:
            case Instructions::i32_store16.value():
                if (pop_and_store<i32, i16>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_store8:
            case Instructions::i64_store8.value():
                if (pop_and_store<i64, i8>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_store16:
            case Instructions::i64_store16.value():
                if (pop_and_store<i64, i16>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_store32:
            case Instructions::i64_store32.value():
                if (pop_and_store<i64, i32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_local_tee:
            case Instructions::local_tee.value(): {
                auto value = configuration.value_stack().unsafe_last(); // bounds checked by verifier.
                auto local_index = instruction->local_index();
                dbgln_if(WASM_TRACE_DEBUG, "stack:peek -> locals({})", local_index.value());
                configuration.frame().locals()[local_index.value()] = value;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_global_get:
            case Instructions::global_get.value(): {
                auto global_index = instruction->arguments().get<GlobalIndex>();
                // This check here is for const expressions. In non-const expressions,
                // a validation error would have been thrown.
                TRAP_IN_LOOP_IF_NOT(global_index < configuration.frame().module().globals().size());
                auto address = configuration.frame().module().globals()[global_index.value()];
                dbgln_if(WASM_TRACE_DEBUG, "global({}) -> stack", address.value());
                auto global = configuration.store().get(address);
                configuration.value_stack().unchecked_append(global->value());
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_global_set:
            case Instructions::global_set.value(): {
                auto global_index = instruction->arguments().get<GlobalIndex>();
                auto address = configuration.frame().module().globals()[global_index.value()];
                // bounds checked by verifier.
                auto value = configuration.value_stack().unsafe_take_last();
                dbgln_if(WASM_TRACE_DEBUG, "stack -> global({})", address.value());
                auto global = configuration.store().get(address);
                global->set_value(value);
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_memory_size:
            case Instructions::memory_size.value(): {
                auto& args = instruction->arguments().get<Instruction::MemoryIndexArgument>();
                auto address = configuration.frame().module().memories()[args.memory_index.value()];
                auto instance = configuration.store().get(address);
                auto pages = instance->size() / Constants::page_size;
                dbgln_if(WASM_TRACE_DEBUG, "memory.size -> stack({})", pages);
                configuration.value_stack().unchecked_append(Value((i32)pages));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_memory_grow:
            case Instructions::memory_grow.value(): {
                auto& args = instruction->arguments().get<Instruction::MemoryIndexArgument>();
                auto address = configuration.frame().module().memories()[args.memory_index.value()];
                auto instance = configuration.store().get(address);
                i32 old_pages = instance->size() / Constants::page_size;
                auto& entry = configuration.value_stack().unsafe_last(); // bounds checked by verifier.
                auto new_pages = entry.to<i32>();
                dbgln_if(WASM_TRACE_DEBUG, "memory.grow({}), previously {} pages...", new_pages, old_pages);
                if (instance->grow(new_pages * Constants::page_size))
                    entry = Value((i32)old_pages);
                else
                    entry = Value((i32)-1);
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            // https://webassembly.github.io/spec/core/bikeshed/#exec-memory-fill
            handle_memory_fill:
            case Instructions::memory_fill.value(): {
                auto& args = instruction->arguments().get<Instruction::MemoryIndexArgument>();
                auto address = configuration.frame().module().memories()[args.memory_index.value()];
                auto instance = configuration.store().get(address);
                // bounds checked by verifier.
                auto count = configuration.value_stack().unsafe_take_last().to<u32>();
                u8 value = static_cast<u8>(configuration.value_stack().unsafe_take_last().to<u32>());
                auto destination_offset = configuration.value_stack().unsafe_take_last().to<u32>();

                TRAP_IN_LOOP_IF_NOT(static_cast<size_t>(destination_offset + count) <= instance->data().size());

                if (count == 0)
                    RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);

                for (u32 i = 0; i < count; ++i) {
                    if (!store_to_memory(configuration, Instruction::MemoryArgument { 0, 0 }, { &value, sizeof(value) }, destination_offset + i))
                        return;
                }

                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            // https://webassembly.github.io/spec/core/bikeshed/#exec-memory-copy
            handle_memory_copy:
            case Instructions::memory_copy.value(): {
                auto& args = instruction->arguments().get<Instruction::MemoryCopyArgs>();
                auto source_address = configuration.frame().module().memories()[args.src_index.value()];
                auto destination_address = configuration.frame().module().memories()[args.dst_index.value()];
                auto source_instance = configuration.store().get(source_address);
                auto destination_instance = configuration.store().get(destination_address);

                // bounds checked by verifier.
                auto count = configuration.value_stack().unsafe_take_last().to<i32>();
                auto source_offset = configuration.value_stack().unsafe_take_last().to<i32>();
                auto destination_offset = configuration.value_stack().unsafe_take_last().to<i32>();

                Checked<size_t> source_position = source_offset;
                source_position.saturating_add(count);
                Checked<size_t> destination_position = destination_offset;
                destination_position.saturating_add(count);
                TRAP_IN_LOOP_IF_NOT(source_position <= source_instance->data().size());
                TRAP_IN_LOOP_IF_NOT(destination_position <= destination_instance->data().size());

                if (count == 0)
                    RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);

                Instruction::MemoryArgument memarg { 0, 0, args.dst_index };
                if (destination_offset <= source_offset) {
                    for (auto i = 0; i < count; ++i) {
                        auto value = source_instance->data()[source_offset + i];
                        if (!store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i))
                            return;
                    }
                } else {
                    for (auto i = count - 1; i >= 0; --i) {
                        auto value = source_instance->data()[source_offset + i];
                        if (!store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i))
                            return;
                    }
                }

                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            // https://webassembly.github.io/spec/core/bikeshed/#exec-memory-init
            handle_memory_init:
            case Instructions::memory_init.value(): {
                auto& args = instruction->arguments().get<Instruction::MemoryInitArgs>();
                auto& data_address = configuration.frame().module().datas()[args.data_index.value()];
                auto& data = *configuration.store().get(data_address);
                auto memory_address = configuration.frame().module().memories()[args.memory_index.value()];
                auto memory = configuration.store().get(memory_address);
                // bounds checked by verifier.
                auto count = configuration.value_stack().unsafe_take_last().to<u32>();
                auto source_offset = configuration.value_stack().unsafe_take_last().to<u32>();
                auto destination_offset = configuration.value_stack().unsafe_take_last().to<u32>();

                Checked<size_t> source_position = source_offset;
                source_position.saturating_add(count);
                Checked<size_t> destination_position = destination_offset;
                destination_position.saturating_add(count);
                TRAP_IN_LOOP_IF_NOT(source_position <= data.data().size());
                TRAP_IN_LOOP_IF_NOT(destination_position <= memory->data().size());

                if (count == 0)
                    RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);

                Instruction::MemoryArgument memarg { 0, 0, args.memory_index };
                for (size_t i = 0; i < (size_t)count; ++i) {
                    auto value = data.data()[source_offset + i];
                    if (!store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i))
                        return;
                }
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            // https://webassembly.github.io/spec/core/bikeshed/#exec-data-drop
            handle_data_drop:
            case Instructions::data_drop.value(): {
                auto data_index = instruction->arguments().get<DataIndex>();
                auto data_address = configuration.frame().module().datas()[data_index.value()];
                *configuration.store().get(data_address) = DataInstance({});
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_elem_drop:
            case Instructions::elem_drop.value(): {
                auto elem_index = instruction->arguments().get<ElementIndex>();
                auto address = configuration.frame().module().elements()[elem_index.value()];
                auto elem = configuration.store().get(address);
                *configuration.store().get(address) = ElementInstance(elem->type(), {});
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_table_init:
            case Instructions::table_init.value(): {
                auto& args = instruction->arguments().get<Instruction::TableElementArgs>();
                auto table_address = configuration.frame().module().tables()[args.table_index.value()];
                auto table = configuration.store().get(table_address);
                auto element_address = configuration.frame().module().elements()[args.element_index.value()];
                auto element = configuration.store().get(element_address);
                // bounds checked by verifier.
                auto count = configuration.value_stack().unsafe_take_last().to<u32>();
                auto source_offset = configuration.value_stack().unsafe_take_last().to<u32>();
                auto destination_offset = configuration.value_stack().unsafe_take_last().to<u32>();

                Checked<u32> checked_source_offset = source_offset;
                Checked<u32> checked_destination_offset = destination_offset;
                checked_source_offset += count;
                checked_destination_offset += count;
                TRAP_IN_LOOP_IF_NOT(!checked_source_offset.has_overflow() && checked_source_offset <= (u32)element->references().size());
                TRAP_IN_LOOP_IF_NOT(!checked_destination_offset.has_overflow() && checked_destination_offset <= (u32)table->elements().size());

                for (u32 i = 0; i < count; ++i)
                    table->elements()[destination_offset + i] = element->references()[source_offset + i];
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_table_copy:
            case Instructions::table_copy.value(): {
                auto& args = instruction->arguments().get<Instruction::TableTableArgs>();
                auto source_address = configuration.frame().module().tables()[args.rhs.value()];
                auto destination_address = configuration.frame().module().tables()[args.lhs.value()];
                auto source_instance = configuration.store().get(source_address);
                auto destination_instance = configuration.store().get(destination_address);

                // bounds checked by verifier.
                auto count = configuration.value_stack().unsafe_take_last().to<u32>();
                auto source_offset = configuration.value_stack().unsafe_take_last().to<u32>();
                auto destination_offset = configuration.value_stack().unsafe_take_last().to<u32>();

                Checked<size_t> source_position = source_offset;
                source_position.saturating_add(count);
                Checked<size_t> destination_position = destination_offset;
                destination_position.saturating_add(count);
                TRAP_IN_LOOP_IF_NOT(source_position <= source_instance->elements().size());
                TRAP_IN_LOOP_IF_NOT(destination_position <= destination_instance->elements().size());

                if (count == 0)
                    RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);

                if (destination_offset <= source_offset) {
                    for (u32 i = 0; i < count; ++i) {
                        auto value = source_instance->elements()[source_offset + i];
                        destination_instance->elements()[destination_offset + i] = value;
                    }
                } else {
                    for (u32 i = count - 1; i != NumericLimits<u32>::max(); --i) {
                        auto value = source_instance->elements()[source_offset + i];
                        destination_instance->elements()[destination_offset + i] = value;
                    }
                }

                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_table_fill:
            case Instructions::table_fill.value(): {
                auto table_index = instruction->arguments().get<TableIndex>();
                auto address = configuration.frame().module().tables()[table_index.value()];
                auto table = configuration.store().get(address);
                // bounds checked by verifier.
                auto count = configuration.value_stack().unsafe_take_last().to<u32>();
                auto value = configuration.value_stack().unsafe_take_last();
                auto start = configuration.value_stack().unsafe_take_last().to<u32>();

                Checked<u32> checked_offset = start;
                checked_offset += count;
                TRAP_IN_LOOP_IF_NOT(!checked_offset.has_overflow() && checked_offset <= (u32)table->elements().size());

                for (u32 i = 0; i < count; ++i)
                    table->elements()[start + i] = value.to<Reference>();
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_table_set:
            case Instructions::table_set.value(): {
                // bounds checked by verifier.
                auto ref = configuration.value_stack().unsafe_take_last();
                auto index = (size_t)(configuration.value_stack().unsafe_take_last().to<i32>());
                auto table_index = instruction->arguments().get<TableIndex>();
                auto address = configuration.frame().module().tables()[table_index.value()];
                auto table = configuration.store().get(address);
                TRAP_IN_LOOP_IF_NOT(index < table->elements().size());
                table->elements()[index] = ref.to<Reference>();
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_table_get:
            case Instructions::table_get.value(): {
                // bounds checked by verifier.
                auto index = (size_t)(configuration.value_stack().unsafe_take_last().to<i32>());
                auto table_index = instruction->arguments().get<TableIndex>();
                auto address = configuration.frame().module().tables()[table_index.value()];
                auto table = configuration.store().get(address);
                TRAP_IN_LOOP_IF_NOT(index < table->elements().size());
                configuration.value_stack().unchecked_append(Value(table->elements()[index]));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_table_grow:
            case Instructions::table_grow.value(): {
                // bounds checked by verifier.
                auto size = configuration.value_stack().unsafe_take_last().to<u32>();
                auto fill_value = configuration.value_stack().unsafe_take_last();
                auto table_index = instruction->arguments().get<TableIndex>();
                auto address = configuration.frame().module().tables()[table_index.value()];
                auto table = configuration.store().get(address);
                auto previous_size = table->elements().size();
                auto did_grow = table->grow(size, fill_value.to<Reference>());
                if (!did_grow) {
                    configuration.value_stack().unchecked_append(Value((i32)-1));
                } else {
                    configuration.value_stack().unchecked_append(Value((i32)previous_size));
                }
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_table_size:
            case Instructions::table_size.value(): {
                auto table_index = instruction->arguments().get<TableIndex>();
                auto address = configuration.frame().module().tables()[table_index.value()];
                auto table = configuration.store().get(address);
                configuration.value_stack().unchecked_append(Value((i32)table->elements().size()));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_ref_null:
            case Instructions::ref_null.value(): {
                auto type = instruction->arguments().get<ValueType>();
                configuration.value_stack().unchecked_append(Value(Reference(Reference::Null { type })));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            };
            handle_ref_func:
            case Instructions::ref_func.value(): {
                auto index = instruction->arguments().get<FunctionIndex>().value();
                auto& functions = configuration.frame().module().functions();
                auto address = functions[index];
                configuration.value_stack().unchecked_append(Value(Reference { Reference::Func { address, configuration.store().get_module_for(address) } }));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_ref_is_null:
            case Instructions::ref_is_null.value(): {
                // bounds checked by verifier.
                auto ref = configuration.value_stack().unsafe_take_last();
                configuration.value_stack().unchecked_append(Value(static_cast<i32>(ref.to<Reference>().ref().has<Reference::Null>() ? 1 : 0)));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_drop:
            case Instructions::drop.value():
                // bounds checked by verifier.
                configuration.value_stack().unsafe_take_last();
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_select:
            case Instructions::select.value():
            handle_select_typed:
            case Instructions::select_typed.value(): {
                // Note: The type seems to only be used for validation.
                auto value = configuration.value_stack().unsafe_take_last().to<i32>(); // bounds checked by verifier.
                dbgln_if(WASM_TRACE_DEBUG, "select({})", value);
                auto rhs = configuration.value_stack().take_last();
                auto& lhs = configuration.value_stack().unsafe_last(); // bounds checked by verifier.
                lhs = value != 0 ? lhs : rhs;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_i32_eqz:
            case Instructions::i32_eqz.value():
                if (unary_operation<i32, i32, Operators::EqualsZero>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_eq:
            case Instructions::i32_eq.value():
                if (binary_numeric_operation<i32, i32, Operators::Equals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_ne:
            case Instructions::i32_ne.value():
                if (binary_numeric_operation<i32, i32, Operators::NotEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_lts:
            case Instructions::i32_lts.value():
                if (binary_numeric_operation<i32, i32, Operators::LessThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_ltu:
            case Instructions::i32_ltu.value():
                if (binary_numeric_operation<u32, i32, Operators::LessThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_gts:
            case Instructions::i32_gts.value():
                if (binary_numeric_operation<i32, i32, Operators::GreaterThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_gtu:
            case Instructions::i32_gtu.value():
                if (binary_numeric_operation<u32, i32, Operators::GreaterThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_les:
            case Instructions::i32_les.value():
                if (binary_numeric_operation<i32, i32, Operators::LessThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_leu:
            case Instructions::i32_leu.value():
                if (binary_numeric_operation<u32, i32, Operators::LessThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_ges:
            case Instructions::i32_ges.value():
                if (binary_numeric_operation<i32, i32, Operators::GreaterThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_geu:
            case Instructions::i32_geu.value():
                if (binary_numeric_operation<u32, i32, Operators::GreaterThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_eqz:
            case Instructions::i64_eqz.value():
                if (unary_operation<i64, i32, Operators::EqualsZero>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_eq:
            case Instructions::i64_eq.value():
                if (binary_numeric_operation<i64, i32, Operators::Equals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_ne:
            case Instructions::i64_ne.value():
                if (binary_numeric_operation<i64, i32, Operators::NotEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_lts:
            case Instructions::i64_lts.value():
                if (binary_numeric_operation<i64, i32, Operators::LessThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_ltu:
            case Instructions::i64_ltu.value():
                if (binary_numeric_operation<u64, i32, Operators::LessThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_gts:
            case Instructions::i64_gts.value():
                if (binary_numeric_operation<i64, i32, Operators::GreaterThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_gtu:
            case Instructions::i64_gtu.value():
                if (binary_numeric_operation<u64, i32, Operators::GreaterThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_les:
            case Instructions::i64_les.value():
                if (binary_numeric_operation<i64, i32, Operators::LessThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_leu:
            case Instructions::i64_leu.value():
                if (binary_numeric_operation<u64, i32, Operators::LessThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_ges:
            case Instructions::i64_ges.value():
                if (binary_numeric_operation<i64, i32, Operators::GreaterThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_geu:
            case Instructions::i64_geu.value():
                if (binary_numeric_operation<u64, i32, Operators::GreaterThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_eq:
            case Instructions::f32_eq.value():
                if (binary_numeric_operation<float, i32, Operators::Equals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_ne:
            case Instructions::f32_ne.value():
                if (binary_numeric_operation<float, i32, Operators::NotEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_lt:
            case Instructions::f32_lt.value():
                if (binary_numeric_operation<float, i32, Operators::LessThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_gt:
            case Instructions::f32_gt.value():
                if (binary_numeric_operation<float, i32, Operators::GreaterThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_le:
            case Instructions::f32_le.value():
                if (binary_numeric_operation<float, i32, Operators::LessThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_ge:
            case Instructions::f32_ge.value():
                if (binary_numeric_operation<float, i32, Operators::GreaterThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_eq:
            case Instructions::f64_eq.value():
                if (binary_numeric_operation<double, i32, Operators::Equals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_ne:
            case Instructions::f64_ne.value():
                if (binary_numeric_operation<double, i32, Operators::NotEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_lt:
            case Instructions::f64_lt.value():
                if (binary_numeric_operation<double, i32, Operators::LessThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_gt:
            case Instructions::f64_gt.value():
                if (binary_numeric_operation<double, i32, Operators::GreaterThan>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_le:
            case Instructions::f64_le.value():
                if (binary_numeric_operation<double, i32, Operators::LessThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_ge:
            case Instructions::f64_ge.value():
                if (binary_numeric_operation<double, i32, Operators::GreaterThanOrEquals>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_clz:
            case Instructions::i32_clz.value():
                if (unary_operation<i32, i32, Operators::CountLeadingZeros>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_ctz:
            case Instructions::i32_ctz.value():
                if (unary_operation<i32, i32, Operators::CountTrailingZeros>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_popcnt:
            case Instructions::i32_popcnt.value():
                if (unary_operation<i32, i32, Operators::PopCount>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_add:
            case Instructions::i32_add.value():
                if (binary_numeric_operation<u32, i32, Operators::Add>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_sub:
            case Instructions::i32_sub.value():
                if (binary_numeric_operation<u32, i32, Operators::Subtract>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_mul:
            case Instructions::i32_mul.value():
                if (binary_numeric_operation<u32, i32, Operators::Multiply>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_divs:
            case Instructions::i32_divs.value():
                if (binary_numeric_operation<i32, i32, Operators::Divide>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_divu:
            case Instructions::i32_divu.value():
                if (binary_numeric_operation<u32, i32, Operators::Divide>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_rems:
            case Instructions::i32_rems.value():
                if (binary_numeric_operation<i32, i32, Operators::Modulo>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_remu:
            case Instructions::i32_remu.value():
                if (binary_numeric_operation<u32, i32, Operators::Modulo>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_and:
            case Instructions::i32_and.value():
                if (binary_numeric_operation<i32, i32, Operators::BitAnd>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_or:
            case Instructions::i32_or.value():
                if (binary_numeric_operation<i32, i32, Operators::BitOr>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_xor:
            case Instructions::i32_xor.value():
                if (binary_numeric_operation<i32, i32, Operators::BitXor>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_shl:
            case Instructions::i32_shl.value():
                if (binary_numeric_operation<u32, i32, Operators::BitShiftLeft>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_shrs:
            case Instructions::i32_shrs.value():
                if (binary_numeric_operation<i32, i32, Operators::BitShiftRight>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_shru:
            case Instructions::i32_shru.value():
                if (binary_numeric_operation<u32, i32, Operators::BitShiftRight>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_rotl:
            case Instructions::i32_rotl.value():
                if (binary_numeric_operation<u32, i32, Operators::BitRotateLeft>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_rotr:
            case Instructions::i32_rotr.value():
                if (binary_numeric_operation<u32, i32, Operators::BitRotateRight>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_clz:
            case Instructions::i64_clz.value():
                if (unary_operation<i64, i64, Operators::CountLeadingZeros>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_ctz:
            case Instructions::i64_ctz.value():
                if (unary_operation<i64, i64, Operators::CountTrailingZeros>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_popcnt:
            case Instructions::i64_popcnt.value():
                if (unary_operation<i64, i64, Operators::PopCount>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_add:
            case Instructions::i64_add.value():
                if (binary_numeric_operation<u64, i64, Operators::Add>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_sub:
            case Instructions::i64_sub.value():
                if (binary_numeric_operation<u64, i64, Operators::Subtract>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_mul:
            case Instructions::i64_mul.value():
                if (binary_numeric_operation<u64, i64, Operators::Multiply>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_divs:
            case Instructions::i64_divs.value():
                if (binary_numeric_operation<i64, i64, Operators::Divide>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_divu:
            case Instructions::i64_divu.value():
                if (binary_numeric_operation<u64, i64, Operators::Divide>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_rems:
            case Instructions::i64_rems.value():
                if (binary_numeric_operation<i64, i64, Operators::Modulo>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_remu:
            case Instructions::i64_remu.value():
                if (binary_numeric_operation<u64, i64, Operators::Modulo>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_and:
            case Instructions::i64_and.value():
                if (binary_numeric_operation<i64, i64, Operators::BitAnd>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_or:
            case Instructions::i64_or.value():
                if (binary_numeric_operation<i64, i64, Operators::BitOr>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_xor:
            case Instructions::i64_xor.value():
                if (binary_numeric_operation<i64, i64, Operators::BitXor>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_shl:
            case Instructions::i64_shl.value():
                if (binary_numeric_operation<u64, i64, Operators::BitShiftLeft>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_shrs:
            case Instructions::i64_shrs.value():
                if (binary_numeric_operation<i64, i64, Operators::BitShiftRight>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_shru:
            case Instructions::i64_shru.value():
                if (binary_numeric_operation<u64, i64, Operators::BitShiftRight>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_rotl:
            case Instructions::i64_rotl.value():
                if (binary_numeric_operation<u64, i64, Operators::BitRotateLeft>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_rotr:
            case Instructions::i64_rotr.value():
                if (binary_numeric_operation<u64, i64, Operators::BitRotateRight>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_abs:
            case Instructions::f32_abs.value():
                if (unary_operation<float, float, Operators::Absolute>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_neg:
            case Instructions::f32_neg.value():
                if (unary_operation<float, float, Operators::Negate>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_ceil:
            case Instructions::f32_ceil.value():
                if (unary_operation<float, float, Operators::Ceil>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_floor:
            case Instructions::f32_floor.value():
                if (unary_operation<float, float, Operators::Floor>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_trunc:
            case Instructions::f32_trunc.value():
                if (unary_operation<float, float, Operators::Truncate>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_nearest:
            case Instructions::f32_nearest.value():
                if (unary_operation<float, float, Operators::NearbyIntegral>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_sqrt:
            case Instructions::f32_sqrt.value():
                if (unary_operation<float, float, Operators::SquareRoot>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_add:
            case Instructions::f32_add.value():
                if (binary_numeric_operation<float, float, Operators::Add>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_sub:
            case Instructions::f32_sub.value():
                if (binary_numeric_operation<float, float, Operators::Subtract>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_mul:
            case Instructions::f32_mul.value():
                if (binary_numeric_operation<float, float, Operators::Multiply>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_div:
            case Instructions::f32_div.value():
                if (binary_numeric_operation<float, float, Operators::Divide>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_min:
            case Instructions::f32_min.value():
                if (binary_numeric_operation<float, float, Operators::Minimum>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_max:
            case Instructions::f32_max.value():
                if (binary_numeric_operation<float, float, Operators::Maximum>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_copysign:
            case Instructions::f32_copysign.value():
                if (binary_numeric_operation<float, float, Operators::CopySign>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_abs:
            case Instructions::f64_abs.value():
                if (unary_operation<double, double, Operators::Absolute>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_neg:
            case Instructions::f64_neg.value():
                if (unary_operation<double, double, Operators::Negate>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_ceil:
            case Instructions::f64_ceil.value():
                if (unary_operation<double, double, Operators::Ceil>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_floor:
            case Instructions::f64_floor.value():
                if (unary_operation<double, double, Operators::Floor>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_trunc:
            case Instructions::f64_trunc.value():
                if (unary_operation<double, double, Operators::Truncate>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_nearest:
            case Instructions::f64_nearest.value():
                if (unary_operation<double, double, Operators::NearbyIntegral>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_sqrt:
            case Instructions::f64_sqrt.value():
                if (unary_operation<double, double, Operators::SquareRoot>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_add:
            case Instructions::f64_add.value():
                if (binary_numeric_operation<double, double, Operators::Add>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_sub:
            case Instructions::f64_sub.value():
                if (binary_numeric_operation<double, double, Operators::Subtract>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_mul:
            case Instructions::f64_mul.value():
                if (binary_numeric_operation<double, double, Operators::Multiply>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_div:
            case Instructions::f64_div.value():
                if (binary_numeric_operation<double, double, Operators::Divide>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_min:
            case Instructions::f64_min.value():
                if (binary_numeric_operation<double, double, Operators::Minimum>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_max:
            case Instructions::f64_max.value():
                if (binary_numeric_operation<double, double, Operators::Maximum>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_copysign:
            case Instructions::f64_copysign.value():
                if (binary_numeric_operation<double, double, Operators::CopySign>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_wrap_i64:
            case Instructions::i32_wrap_i64.value():
                if (unary_operation<i64, i32, Operators::Wrap<i32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_trunc_sf32:
            case Instructions::i32_trunc_sf32.value():
                if (unary_operation<float, i32, Operators::CheckedTruncate<i32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_trunc_uf32:
            case Instructions::i32_trunc_uf32.value():
                if (unary_operation<float, i32, Operators::CheckedTruncate<u32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_trunc_sf64:
            case Instructions::i32_trunc_sf64.value():
                if (unary_operation<double, i32, Operators::CheckedTruncate<i32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_trunc_uf64:
            case Instructions::i32_trunc_uf64.value():
                if (unary_operation<double, i32, Operators::CheckedTruncate<u32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_trunc_sf32:
            case Instructions::i64_trunc_sf32.value():
                if (unary_operation<float, i64, Operators::CheckedTruncate<i64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_trunc_uf32:
            case Instructions::i64_trunc_uf32.value():
                if (unary_operation<float, i64, Operators::CheckedTruncate<u64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_trunc_sf64:
            case Instructions::i64_trunc_sf64.value():
                if (unary_operation<double, i64, Operators::CheckedTruncate<i64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_trunc_uf64:
            case Instructions::i64_trunc_uf64.value():
                if (unary_operation<double, i64, Operators::CheckedTruncate<u64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_extend_si32:
            case Instructions::i64_extend_si32.value():
                if (unary_operation<i32, i64, Operators::Extend<i64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_extend_ui32:
            case Instructions::i64_extend_ui32.value():
                if (unary_operation<u32, i64, Operators::Extend<i64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_convert_si32:
            case Instructions::f32_convert_si32.value():
                if (unary_operation<i32, float, Operators::Convert<float>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_convert_ui32:
            case Instructions::f32_convert_ui32.value():
                if (unary_operation<u32, float, Operators::Convert<float>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_convert_si64:
            case Instructions::f32_convert_si64.value():
                if (unary_operation<i64, float, Operators::Convert<float>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_convert_ui64:
            case Instructions::f32_convert_ui64.value():
                if (unary_operation<u64, float, Operators::Convert<float>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_demote_f64:
            case Instructions::f32_demote_f64.value():
                if (unary_operation<double, float, Operators::Demote>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_convert_si32:
            case Instructions::f64_convert_si32.value():
                if (unary_operation<i32, double, Operators::Convert<double>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_convert_ui32:
            case Instructions::f64_convert_ui32.value():
                if (unary_operation<u32, double, Operators::Convert<double>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_convert_si64:
            case Instructions::f64_convert_si64.value():
                if (unary_operation<i64, double, Operators::Convert<double>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_convert_ui64:
            case Instructions::f64_convert_ui64.value():
                if (unary_operation<u64, double, Operators::Convert<double>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_promote_f32:
            case Instructions::f64_promote_f32.value():
                if (unary_operation<float, double, Operators::Promote>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_reinterpret_f32:
            case Instructions::i32_reinterpret_f32.value():
                if (unary_operation<float, i32, Operators::Reinterpret<i32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_reinterpret_f64:
            case Instructions::i64_reinterpret_f64.value():
                if (unary_operation<double, i64, Operators::Reinterpret<i64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32_reinterpret_i32:
            case Instructions::f32_reinterpret_i32.value():
                if (unary_operation<i32, float, Operators::Reinterpret<float>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64_reinterpret_i64:
            case Instructions::f64_reinterpret_i64.value():
                if (unary_operation<i64, double, Operators::Reinterpret<double>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_extend8_s:
            case Instructions::i32_extend8_s.value():
                if (unary_operation<i32, i32, Operators::SignExtend<i8>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_extend16_s:
            case Instructions::i32_extend16_s.value():
                if (unary_operation<i32, i32, Operators::SignExtend<i16>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_extend8_s:
            case Instructions::i64_extend8_s.value():
                if (unary_operation<i64, i64, Operators::SignExtend<i8>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_extend16_s:
            case Instructions::i64_extend16_s.value():
                if (unary_operation<i64, i64, Operators::SignExtend<i16>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_extend32_s:
            case Instructions::i64_extend32_s.value():
                if (unary_operation<i64, i64, Operators::SignExtend<i32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_trunc_sat_f32_s:
            case Instructions::i32_trunc_sat_f32_s.value():
                if (unary_operation<float, i32, Operators::SaturatingTruncate<i32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_trunc_sat_f32_u:
            case Instructions::i32_trunc_sat_f32_u.value():
                if (unary_operation<float, i32, Operators::SaturatingTruncate<u32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_trunc_sat_f64_s:
            case Instructions::i32_trunc_sat_f64_s.value():
                if (unary_operation<double, i32, Operators::SaturatingTruncate<i32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32_trunc_sat_f64_u:
            case Instructions::i32_trunc_sat_f64_u.value():
                if (unary_operation<double, i32, Operators::SaturatingTruncate<u32>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_trunc_sat_f32_s:
            case Instructions::i64_trunc_sat_f32_s.value():
                if (unary_operation<float, i64, Operators::SaturatingTruncate<i64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_trunc_sat_f32_u:
            case Instructions::i64_trunc_sat_f32_u.value():
                if (unary_operation<float, i64, Operators::SaturatingTruncate<u64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_trunc_sat_f64_s:
            case Instructions::i64_trunc_sat_f64_s.value():
                if (unary_operation<double, i64, Operators::SaturatingTruncate<i64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64_trunc_sat_f64_u:
            case Instructions::i64_trunc_sat_f64_u.value():
                if (unary_operation<double, i64, Operators::SaturatingTruncate<u64>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_const:
            case Instructions::v128_const.value():
                configuration.value_stack().unchecked_append(Value(instruction->arguments().get<u128>()));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_v128_load:
            case Instructions::v128_load.value():
                if (load_and_push<u128, u128>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load8x8_s:
            case Instructions::v128_load8x8_s.value():
                if (load_and_push_mxn<8, 8, MakeSigned>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load8x8_u:
            case Instructions::v128_load8x8_u.value():
                if (load_and_push_mxn<8, 8, MakeUnsigned>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load16x4_s:
            case Instructions::v128_load16x4_s.value():
                if (load_and_push_mxn<16, 4, MakeSigned>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load16x4_u:
            case Instructions::v128_load16x4_u.value():
                if (load_and_push_mxn<16, 4, MakeUnsigned>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load32x2_s:
            case Instructions::v128_load32x2_s.value():
                if (load_and_push_mxn<32, 2, MakeSigned>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load32x2_u:
            case Instructions::v128_load32x2_u.value():
                if (load_and_push_mxn<32, 2, MakeUnsigned>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load8_splat:
            case Instructions::v128_load8_splat.value():
                if (load_and_push_m_splat<8>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load16_splat:
            case Instructions::v128_load16_splat.value():
                if (load_and_push_m_splat<16>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load32_splat:
            case Instructions::v128_load32_splat.value():
                if (load_and_push_m_splat<32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load64_splat:
            case Instructions::v128_load64_splat.value():
                if (load_and_push_m_splat<64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_splat:
            case Instructions::i8x16_splat.value():
                pop_and_push_m_splat<8, NativeIntegralType>(configuration, *instruction);
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_i16x8_splat:
            case Instructions::i16x8_splat.value():
                pop_and_push_m_splat<16, NativeIntegralType>(configuration, *instruction);
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_i32x4_splat:
            case Instructions::i32x4_splat.value():
                pop_and_push_m_splat<32, NativeIntegralType>(configuration, *instruction);
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_i64x2_splat:
            case Instructions::i64x2_splat.value():
                pop_and_push_m_splat<64, NativeIntegralType>(configuration, *instruction);
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_f32x4_splat:
            case Instructions::f32x4_splat.value():
                pop_and_push_m_splat<32, NativeFloatingType>(configuration, *instruction);
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_f64x2_splat:
            case Instructions::f64x2_splat.value():
                pop_and_push_m_splat<64, NativeFloatingType>(configuration, *instruction);
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            handle_i8x16_shuffle:
            case Instructions::i8x16_shuffle.value(): {
                auto& arg = instruction->arguments().get<Instruction::ShuffleArgument>();
                auto b = pop_vector<u8, MakeUnsigned>(configuration);
                auto a = pop_vector<u8, MakeUnsigned>(configuration);
                using VectorType = Native128ByteVectorOf<u8, MakeUnsigned>;
                VectorType result;
                for (size_t i = 0; i < 16; ++i)
                    if (arg.lanes[i] < 16)
                        result[i] = a[arg.lanes[i]];
                    else
                        result[i] = b[arg.lanes[i] - 16];
                configuration.value_stack().unchecked_append(Value(bit_cast<u128>(result)));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_v128_store:
            case Instructions::v128_store.value():
                if (pop_and_store<u128, u128>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_shl:
            case Instructions::i8x16_shl.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<16>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_shr_u:
            case Instructions::i8x16_shr_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<16, MakeUnsigned>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_shr_s:
            case Instructions::i8x16_shr_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<16, MakeSigned>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_shl:
            case Instructions::i16x8_shl.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<8>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_shr_u:
            case Instructions::i16x8_shr_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<8, MakeUnsigned>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_shr_s:
            case Instructions::i16x8_shr_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<8, MakeSigned>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_shl:
            case Instructions::i32x4_shl.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<4>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_shr_u:
            case Instructions::i32x4_shr_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<4, MakeUnsigned>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_shr_s:
            case Instructions::i32x4_shr_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<4, MakeSigned>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_shl:
            case Instructions::i64x2_shl.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<2>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_shr_u:
            case Instructions::i64x2_shr_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<2, MakeUnsigned>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_shr_s:
            case Instructions::i64x2_shr_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<2, MakeSigned>, i32>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_swizzle:
            case Instructions::i8x16_swizzle.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorSwizzle>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_extract_lane_s:
            case Instructions::i8x16_extract_lane_s.value():
                if (unary_operation<u128, i8, Operators::VectorExtractLane<16, MakeSigned>>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_extract_lane_u:
            case Instructions::i8x16_extract_lane_u.value():
                if (unary_operation<u128, u8, Operators::VectorExtractLane<16, MakeUnsigned>>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extract_lane_s:
            case Instructions::i16x8_extract_lane_s.value():
                if (unary_operation<u128, i16, Operators::VectorExtractLane<8, MakeSigned>>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extract_lane_u:
            case Instructions::i16x8_extract_lane_u.value():
                if (unary_operation<u128, u16, Operators::VectorExtractLane<8, MakeUnsigned>>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extract_lane:
            case Instructions::i32x4_extract_lane.value():
                if (unary_operation<u128, i32, Operators::VectorExtractLane<4, MakeSigned>>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_extract_lane:
            case Instructions::i64x2_extract_lane.value():
                if (unary_operation<u128, i64, Operators::VectorExtractLane<2, MakeSigned>>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_extract_lane:
            case Instructions::f32x4_extract_lane.value():
                if (unary_operation<u128, float, Operators::VectorExtractLaneFloat<4>>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_extract_lane:
            case Instructions::f64x2_extract_lane.value():
                if (unary_operation<u128, double, Operators::VectorExtractLaneFloat<2>>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_replace_lane:
            case Instructions::i8x16_replace_lane.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<16, i32>, i32>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_replace_lane:
            case Instructions::i16x8_replace_lane.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<8, i32>, i32>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_replace_lane:
            case Instructions::i32x4_replace_lane.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<4>, i32>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_replace_lane:
            case Instructions::i64x2_replace_lane.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<2>, i64>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_replace_lane:
            case Instructions::f32x4_replace_lane.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<4, float>, float>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_replace_lane:
            case Instructions::f64x2_replace_lane.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<2, double>, double>(configuration, instruction->arguments().get<Instruction::LaneIndex>().lane))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_eq:
            case Instructions::i8x16_eq.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::Equals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_ne:
            case Instructions::i8x16_ne.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::NotEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_lt_s:
            case Instructions::i8x16_lt_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThan, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_lt_u:
            case Instructions::i8x16_lt_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThan, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_gt_s:
            case Instructions::i8x16_gt_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThan, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_gt_u:
            case Instructions::i8x16_gt_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThan, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_le_s:
            case Instructions::i8x16_le_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThanOrEquals, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_le_u:
            case Instructions::i8x16_le_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThanOrEquals, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_ge_s:
            case Instructions::i8x16_ge_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThanOrEquals, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_ge_u:
            case Instructions::i8x16_ge_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThanOrEquals, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_abs:
            case Instructions::i8x16_abs.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::Absolute>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_neg:
            case Instructions::i8x16_neg.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::Negate>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_all_true:
            case Instructions::i8x16_all_true.value():
                if (unary_operation<u128, i32, Operators::VectorAllTrue<16>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_popcnt:
            case Instructions::i8x16_popcnt.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::PopCount>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_add:
            case Instructions::i8x16_add.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Add>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_sub:
            case Instructions::i8x16_sub.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Subtract>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_avgr_u:
            case Instructions::i8x16_avgr_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Average, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_add_sat_s:
            case Instructions::i8x16_add_sat_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<i8, Operators::Add>, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_add_sat_u:
            case Instructions::i8x16_add_sat_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<u8, Operators::Add>, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_sub_sat_s:
            case Instructions::i8x16_sub_sat_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<i8, Operators::Subtract>, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_sub_sat_u:
            case Instructions::i8x16_sub_sat_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<u8, Operators::Subtract>, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_min_s:
            case Instructions::i8x16_min_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Minimum, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_min_u:
            case Instructions::i8x16_min_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Minimum, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_max_s:
            case Instructions::i8x16_max_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Maximum, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_max_u:
            case Instructions::i8x16_max_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Maximum, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_eq:
            case Instructions::i16x8_eq.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::Equals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_ne:
            case Instructions::i16x8_ne.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::NotEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_lt_s:
            case Instructions::i16x8_lt_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThan, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_lt_u:
            case Instructions::i16x8_lt_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThan, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_gt_s:
            case Instructions::i16x8_gt_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThan, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_gt_u:
            case Instructions::i16x8_gt_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThan, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_le_s:
            case Instructions::i16x8_le_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThanOrEquals, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_le_u:
            case Instructions::i16x8_le_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThanOrEquals, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_ge_s:
            case Instructions::i16x8_ge_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThanOrEquals, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_ge_u:
            case Instructions::i16x8_ge_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThanOrEquals, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_abs:
            case Instructions::i16x8_abs.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<8, Operators::Absolute>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_neg:
            case Instructions::i16x8_neg.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<8, Operators::Negate>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_all_true:
            case Instructions::i16x8_all_true.value():
                if (unary_operation<u128, i32, Operators::VectorAllTrue<8>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_add:
            case Instructions::i16x8_add.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Add>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_sub:
            case Instructions::i16x8_sub.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Subtract>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_mul:
            case Instructions::i16x8_mul.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Multiply>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_avgr_u:
            case Instructions::i16x8_avgr_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Average, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_add_sat_s:
            case Instructions::i16x8_add_sat_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Add>, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_add_sat_u:
            case Instructions::i16x8_add_sat_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<u16, Operators::Add>, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_sub_sat_s:
            case Instructions::i16x8_sub_sat_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Subtract>, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_sub_sat_u:
            case Instructions::i16x8_sub_sat_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<u16, Operators::Subtract>, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_min_s:
            case Instructions::i16x8_min_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Minimum, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_min_u:
            case Instructions::i16x8_min_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Minimum, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_max_s:
            case Instructions::i16x8_max_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Maximum, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_max_u:
            case Instructions::i16x8_max_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Maximum, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extend_low_i8x16_s:
            case Instructions::i16x8_extend_low_i8x16_s.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::Low, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extend_high_i8x16_s:
            case Instructions::i16x8_extend_high_i8x16_s.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::High, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extend_low_i8x16_u:
            case Instructions::i16x8_extend_low_i8x16_u.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::Low, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extend_high_i8x16_u:
            case Instructions::i16x8_extend_high_i8x16_u.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::High, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extadd_pairwise_i8x16_s:
            case Instructions::i16x8_extadd_pairwise_i8x16_s.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<8, Operators::Add, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extadd_pairwise_i8x16_u:
            case Instructions::i16x8_extadd_pairwise_i8x16_u.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<8, Operators::Add, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extmul_low_i8x16_s:
            case Instructions::i16x8_extmul_low_i8x16_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extmul_high_i8x16_s:
            case Instructions::i16x8_extmul_high_i8x16_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::High, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extmul_low_i8x16_u:
            case Instructions::i16x8_extmul_low_i8x16_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_extmul_high_i8x16_u:
            case Instructions::i16x8_extmul_high_i8x16_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_eq:
            case Instructions::i32x4_eq.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::Equals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_ne:
            case Instructions::i32x4_ne.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::NotEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_lt_s:
            case Instructions::i32x4_lt_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThan, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_lt_u:
            case Instructions::i32x4_lt_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThan, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_gt_s:
            case Instructions::i32x4_gt_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThan, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_gt_u:
            case Instructions::i32x4_gt_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThan, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_le_s:
            case Instructions::i32x4_le_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThanOrEquals, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_le_u:
            case Instructions::i32x4_le_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThanOrEquals, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_ge_s:
            case Instructions::i32x4_ge_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThanOrEquals, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_ge_u:
            case Instructions::i32x4_ge_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThanOrEquals, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_abs:
            case Instructions::i32x4_abs.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<4, Operators::Absolute>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_neg:
            case Instructions::i32x4_neg.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<4, Operators::Negate, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_all_true:
            case Instructions::i32x4_all_true.value():
                if (unary_operation<u128, i32, Operators::VectorAllTrue<4>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_add:
            case Instructions::i32x4_add.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Add, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_sub:
            case Instructions::i32x4_sub.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Subtract, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_mul:
            case Instructions::i32x4_mul.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Multiply, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_min_s:
            case Instructions::i32x4_min_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Minimum, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_min_u:
            case Instructions::i32x4_min_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Minimum, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_max_s:
            case Instructions::i32x4_max_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Maximum, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_max_u:
            case Instructions::i32x4_max_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Maximum, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extend_low_i16x8_s:
            case Instructions::i32x4_extend_low_i16x8_s.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::Low, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extend_high_i16x8_s:
            case Instructions::i32x4_extend_high_i16x8_s.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::High, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extend_low_i16x8_u:
            case Instructions::i32x4_extend_low_i16x8_u.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::Low, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extend_high_i16x8_u:
            case Instructions::i32x4_extend_high_i16x8_u.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::High, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extadd_pairwise_i16x8_s:
            case Instructions::i32x4_extadd_pairwise_i16x8_s.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<4, Operators::Add, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extadd_pairwise_i16x8_u:
            case Instructions::i32x4_extadd_pairwise_i16x8_u.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<4, Operators::Add, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extmul_low_i16x8_s:
            case Instructions::i32x4_extmul_low_i16x8_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extmul_high_i16x8_s:
            case Instructions::i32x4_extmul_high_i16x8_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::High, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extmul_low_i16x8_u:
            case Instructions::i32x4_extmul_low_i16x8_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_extmul_high_i16x8_u:
            case Instructions::i32x4_extmul_high_i16x8_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_eq:
            case Instructions::i64x2_eq.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::Equals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_ne:
            case Instructions::i64x2_ne.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::NotEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_lt_s:
            case Instructions::i64x2_lt_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::LessThan, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_gt_s:
            case Instructions::i64x2_gt_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::GreaterThan, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_le_s:
            case Instructions::i64x2_le_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::LessThanOrEquals, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_ge_s:
            case Instructions::i64x2_ge_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::GreaterThanOrEquals, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_abs:
            case Instructions::i64x2_abs.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<2, Operators::Absolute>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_neg:
            case Instructions::i64x2_neg.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<2, Operators::Negate, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_all_true:
            case Instructions::i64x2_all_true.value():
                if (unary_operation<u128, i32, Operators::VectorAllTrue<2>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_add:
            case Instructions::i64x2_add.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Add, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_sub:
            case Instructions::i64x2_sub.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Subtract, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_mul:
            case Instructions::i64x2_mul.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Multiply, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_extend_low_i32x4_s:
            case Instructions::i64x2_extend_low_i32x4_s.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::Low, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_extend_high_i32x4_s:
            case Instructions::i64x2_extend_high_i32x4_s.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::High, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_extend_low_i32x4_u:
            case Instructions::i64x2_extend_low_i32x4_u.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::Low, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_extend_high_i32x4_u:
            case Instructions::i64x2_extend_high_i32x4_u.value():
                if (unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::High, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_extmul_low_i32x4_s:
            case Instructions::i64x2_extmul_low_i32x4_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_extmul_high_i32x4_s:
            case Instructions::i64x2_extmul_high_i32x4_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::High, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_extmul_low_i32x4_u:
            case Instructions::i64x2_extmul_low_i32x4_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_extmul_high_i32x4_u:
            case Instructions::i64x2_extmul_high_i32x4_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_eq:
            case Instructions::f32x4_eq.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::Equals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_ne:
            case Instructions::f32x4_ne.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::NotEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_lt:
            case Instructions::f32x4_lt.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::LessThan>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_gt:
            case Instructions::f32x4_gt.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::GreaterThan>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_le:
            case Instructions::f32x4_le.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::LessThanOrEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_ge:
            case Instructions::f32x4_ge.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::GreaterThanOrEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_min:
            case Instructions::f32x4_min.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Minimum>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_max:
            case Instructions::f32x4_max.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Maximum>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_eq:
            case Instructions::f64x2_eq.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::Equals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_ne:
            case Instructions::f64x2_ne.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::NotEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_lt:
            case Instructions::f64x2_lt.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::LessThan>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_gt:
            case Instructions::f64x2_gt.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::GreaterThan>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_le:
            case Instructions::f64x2_le.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::LessThanOrEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_ge:
            case Instructions::f64x2_ge.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::GreaterThanOrEquals>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_min:
            case Instructions::f64x2_min.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Minimum>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_max:
            case Instructions::f64x2_max.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Maximum>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_div:
            case Instructions::f32x4_div.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Divide>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_mul:
            case Instructions::f32x4_mul.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Multiply>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_sub:
            case Instructions::f32x4_sub.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Subtract>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_add:
            case Instructions::f32x4_add.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Add>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_pmin:
            case Instructions::f32x4_pmin.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::PseudoMinimum>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_pmax:
            case Instructions::f32x4_pmax.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::PseudoMaximum>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_div:
            case Instructions::f64x2_div.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Divide>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_mul:
            case Instructions::f64x2_mul.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Multiply>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_sub:
            case Instructions::f64x2_sub.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Subtract>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_add:
            case Instructions::f64x2_add.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Add>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_pmin:
            case Instructions::f64x2_pmin.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::PseudoMinimum>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_pmax:
            case Instructions::f64x2_pmax.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::PseudoMaximum>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_ceil:
            case Instructions::f32x4_ceil.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Ceil>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_floor:
            case Instructions::f32x4_floor.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Floor>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_trunc:
            case Instructions::f32x4_trunc.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Truncate>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_nearest:
            case Instructions::f32x4_nearest.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::NearbyIntegral>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_sqrt:
            case Instructions::f32x4_sqrt.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::SquareRoot>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_neg:
            case Instructions::f32x4_neg.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Negate>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_abs:
            case Instructions::f32x4_abs.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Absolute>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_ceil:
            case Instructions::f64x2_ceil.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Ceil>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_floor:
            case Instructions::f64x2_floor.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Floor>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_trunc:
            case Instructions::f64x2_trunc.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Truncate>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_nearest:
            case Instructions::f64x2_nearest.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::NearbyIntegral>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_sqrt:
            case Instructions::f64x2_sqrt.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::SquareRoot>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_neg:
            case Instructions::f64x2_neg.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Negate>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_abs:
            case Instructions::f64x2_abs.value():
                if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Absolute>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_and:
            case Instructions::v128_and.value():
                if (binary_numeric_operation<u128, u128, Operators::BitAnd>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_or:
            case Instructions::v128_or.value():
                if (binary_numeric_operation<u128, u128, Operators::BitOr>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_xor:
            case Instructions::v128_xor.value():
                if (binary_numeric_operation<u128, u128, Operators::BitXor>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_not:
            case Instructions::v128_not.value():
                if (unary_operation<u128, u128, Operators::BitNot>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_andnot:
            case Instructions::v128_andnot.value():
                if (binary_numeric_operation<u128, u128, Operators::BitAndNot>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_bitselect:
            case Instructions::v128_bitselect.value(): {
                // bounds checked by verifier.
                auto mask = configuration.value_stack().unsafe_take_last().to<u128>();
                auto false_vector = configuration.value_stack().unsafe_take_last().to<u128>();
                auto true_vector = configuration.value_stack().unsafe_take_last().to<u128>();
                u128 result = (true_vector & mask) | (false_vector & ~mask);
                configuration.value_stack().unchecked_append(Value(result));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_v128_any_true:
            case Instructions::v128_any_true.value(): {
                auto vector = configuration.value_stack().unsafe_take_last().to<u128>(); // bounds checked by verifier.
                configuration.value_stack().unchecked_append(Value(static_cast<i32>(vector != 0)));
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::No);
            }
            handle_v128_load8_lane:
            case Instructions::v128_load8_lane.value():
                if (load_and_push_lane_n<8>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load16_lane:
            case Instructions::v128_load16_lane.value():
                if (load_and_push_lane_n<16>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load32_lane:
            case Instructions::v128_load32_lane.value():
                if (load_and_push_lane_n<32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load64_lane:
            case Instructions::v128_load64_lane.value():
                if (load_and_push_lane_n<64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load32_zero:
            case Instructions::v128_load32_zero.value():
                if (load_and_push_zero_n<32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_load64_zero:
            case Instructions::v128_load64_zero.value():
                if (load_and_push_zero_n<64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_store8_lane:
            case Instructions::v128_store8_lane.value():
                if (pop_and_store_lane_n<8>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_store16_lane:
            case Instructions::v128_store16_lane.value():
                if (pop_and_store_lane_n<16>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_store32_lane:
            case Instructions::v128_store32_lane.value():
                if (pop_and_store_lane_n<32>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_v128_store64_lane:
            case Instructions::v128_store64_lane.value():
                if (pop_and_store_lane_n<64>(configuration, *instruction))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_trunc_sat_f32x4_s:
            case Instructions::i32x4_trunc_sat_f32x4_s.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, f32, Operators::SaturatingTruncate<i32>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_trunc_sat_f32x4_u:
            case Instructions::i32x4_trunc_sat_f32x4_u.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, f32, Operators::SaturatingTruncate<u32>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_bitmask:
            case Instructions::i8x16_bitmask.value():
                if (unary_operation<u128, i32, Operators::VectorBitmask<16>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_bitmask:
            case Instructions::i16x8_bitmask.value():
                if (unary_operation<u128, i32, Operators::VectorBitmask<8>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_bitmask:
            case Instructions::i32x4_bitmask.value():
                if (unary_operation<u128, i32, Operators::VectorBitmask<4>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i64x2_bitmask:
            case Instructions::i64x2_bitmask.value():
                if (unary_operation<u128, i32, Operators::VectorBitmask<2>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_dot_i16x8_s:
            case Instructions::i32x4_dot_i16x8_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorDotProduct<4>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_narrow_i16x8_s:
            case Instructions::i8x16_narrow_i16x8_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorNarrow<16, i8>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i8x16_narrow_i16x8_u:
            case Instructions::i8x16_narrow_i16x8_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorNarrow<16, u8>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_narrow_i32x4_s:
            case Instructions::i16x8_narrow_i32x4_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorNarrow<8, i16>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_narrow_i32x4_u:
            case Instructions::i16x8_narrow_i32x4_u.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorNarrow<8, u16>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i16x8_q15mulr_sat_s:
            case Instructions::i16x8_q15mulr_sat_s.value():
                if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Q15Mul>, MakeSigned>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_convert_i32x4_s:
            case Instructions::f32x4_convert_i32x4_s.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, i32, Operators::Convert<f32>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_convert_i32x4_u:
            case Instructions::f32x4_convert_i32x4_u.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, u32, Operators::Convert<f32>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_convert_low_i32x4_s:
            case Instructions::f64x2_convert_low_i32x4_s.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, i32, Operators::Convert<f64>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_convert_low_i32x4_u:
            case Instructions::f64x2_convert_low_i32x4_u.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, u32, Operators::Convert<f64>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f32x4_demote_f64x2_zero:
            case Instructions::f32x4_demote_f64x2_zero.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::Convert<f32>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_f64x2_promote_low_f32x4:
            case Instructions::f64x2_promote_low_f32x4.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, f32, Operators::Convert<f64>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_trunc_sat_f64x2_s_zero:
            case Instructions::i32x4_trunc_sat_f64x2_s_zero.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::SaturatingTruncate<i32>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            handle_i32x4_trunc_sat_f64x2_u_zero:
            case Instructions::i32x4_trunc_sat_f64x2_u_zero.value():
                if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::SaturatingTruncate<u32>>>(configuration))
                    return;
                RUN_NEXT_INSTRUCTION(CouldHaveChangedIP::Yes);
            }

            VERIFY_NOT_REACHED();
        }
    continue_interpretation:;

        if (interpret_instruction_chain(configuration, current_ip_value, old_ip, instructions.span(), max_ip_value, executed_instructions) && did_trap())
            return;
        if (current_ip_value == old_ip) // If no jump occurred
            ++current_ip_value;
    }
}

void BytecodeInterpreter::branch_to_label(Configuration& configuration, LabelIndex index)
{
    dbgln_if(WASM_TRACE_DEBUG, "Branch to label with index {}...", index.value());
    for (size_t i = 0; i < index.value(); ++i)
        configuration.label_stack().take_last();
    auto label = configuration.label_stack().last();
    dbgln_if(WASM_TRACE_DEBUG, "...which is actually IP {}, and has {} result(s)", label.continuation().value(), label.arity());

    configuration.value_stack().remove(label.stack_height(), configuration.value_stack().size() - label.stack_height() - label.arity());
    configuration.ip() = label.continuation();
}

template<typename ReadType, typename PushType>
bool BytecodeInterpreter::load_and_push(Configuration& configuration, Instruction const& instruction)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto& entry = configuration.value_stack().unsafe_last(); // bounds checked by verifier.
    auto base = entry.to<i32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + arg.offset;
    if (instance_address + sizeof(ReadType) > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln_if(WASM_TRACE_DEBUG, "LibWasm: Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + sizeof(ReadType), memory->size());
        return true;
    }
    dbgln_if(WASM_TRACE_DEBUG, "load({} : {}) -> stack", instance_address, sizeof(ReadType));
    auto slice = memory->data().bytes().slice(instance_address, sizeof(ReadType));
    entry = Value(static_cast<PushType>(read_value<ReadType>(slice)));
    return false;
}

template<typename TDst, typename TSrc>
ALWAYS_INLINE static TDst convert_vector(TSrc v)
{
    return __builtin_convertvector(v, TDst);
}

template<size_t M, size_t N, template<typename> typename SetSign>
bool BytecodeInterpreter::load_and_push_mxn(Configuration& configuration, Instruction const& instruction)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto& entry = configuration.value_stack().unsafe_last(); // bounds checked by verifier.
    auto base = entry.to<i32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + arg.offset;
    if (instance_address + M * N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln_if(WASM_TRACE_DEBUG, "LibWasm: Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + M * N / 8, memory->size());
        return true;
    }
    dbgln_if(WASM_TRACE_DEBUG, "vec-load({} : {}) -> stack", instance_address, M * N / 8);
    auto slice = memory->data().bytes().slice(instance_address, M * N / 8);
    using V64 = NativeVectorType<M, N, SetSign>;
    using V128 = NativeVectorType<M * 2, N, SetSign>;

    V64 bytes { 0 };
    if (bit_cast<FlatPtr>(slice.data()) % sizeof(V64) == 0)
        bytes = *bit_cast<V64*>(slice.data());
    else
        ByteReader::load(slice.data(), bytes);

    entry = Value(bit_cast<u128>(convert_vector<V128>(bytes)));
    return false;
}

template<size_t N>
bool BytecodeInterpreter::load_and_push_lane_n(Configuration& configuration, Instruction const& instruction)
{
    auto memarg_and_lane = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    auto& address = configuration.frame().module().memories()[memarg_and_lane.memory.memory_index.value()];
    auto memory = configuration.store().get(address);
    // bounds checked by verifier.
    auto vector = configuration.value_stack().unsafe_take_last().to<u128>();
    auto base = configuration.value_stack().unsafe_take_last().to<u32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + memarg_and_lane.memory.offset;
    if (instance_address + N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        return true;
    }
    auto slice = memory->data().bytes().slice(instance_address, N / 8);
    auto dst = bit_cast<u8*>(&vector) + memarg_and_lane.lane * N / 8;
    memcpy(dst, slice.data(), N / 8);
    configuration.value_stack().unchecked_append(Value(vector));
    return false;
}

template<size_t N>
bool BytecodeInterpreter::load_and_push_zero_n(Configuration& configuration, Instruction const& instruction)
{
    auto memarg_and_lane = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[memarg_and_lane.memory_index.value()];
    auto memory = configuration.store().get(address);
    // bounds checked by verifier.
    auto base = configuration.value_stack().unsafe_take_last().to<u32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + memarg_and_lane.offset;
    if (instance_address + N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        return true;
    }
    auto slice = memory->data().bytes().slice(instance_address, N / 8);
    u128 vector = 0;
    memcpy(&vector, slice.data(), N / 8);
    configuration.value_stack().unchecked_append(Value(vector));
    return false;
}

template<size_t M>
bool BytecodeInterpreter::load_and_push_m_splat(Configuration& configuration, Instruction const& instruction)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto& entry = configuration.value_stack().unsafe_last(); // bounds checked by verifier.
    auto base = entry.to<i32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + arg.offset;
    if (instance_address + M / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln_if(WASM_TRACE_DEBUG, "LibWasm: Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + M / 8, memory->size());
        return true;
    }
    dbgln_if(WASM_TRACE_DEBUG, "vec-splat({} : {}) -> stack", instance_address, M / 8);
    auto slice = memory->data().bytes().slice(instance_address, M / 8);
    auto value = read_value<NativeIntegralType<M>>(slice);
    set_top_m_splat<M, NativeIntegralType>(configuration, value);
    return false;
}

template<size_t M, template<size_t> typename NativeType>
void BytecodeInterpreter::set_top_m_splat(Wasm::Configuration& configuration, NativeType<M> value)
{
    auto push = [&](auto result) {
        configuration.value_stack().last() = Value(bit_cast<u128>(result));
    };

    if constexpr (IsFloatingPoint<NativeType<32>>) {
        if constexpr (M == 32) // 32 -> 32x4
            push(expand4(value));
        else if constexpr (M == 64) // 64 -> 64x2
            push(f64x2 { value, value });
        else
            static_assert(DependentFalse<NativeType<M>>, "Invalid vector size");
    } else {
        if constexpr (M == 8) // 8 -> 8x4 -> 32x4
            push(expand4(bit_cast<u32>(u8x4 { value, value, value, value })));
        else if constexpr (M == 16) // 16 -> 16x2 -> 32x4
            push(expand4(bit_cast<u32>(u16x2 { value, value })));
        else if constexpr (M == 32) // 32 -> 32x4
            push(expand4(value));
        else if constexpr (M == 64) // 64 -> 64x2
            push(u64x2 { value, value });
        else
            static_assert(DependentFalse<NativeType<M>>, "Invalid vector size");
    }
}

template<size_t M, template<size_t> typename NativeType>
void BytecodeInterpreter::pop_and_push_m_splat(Wasm::Configuration& configuration, Instruction const&)
{
    using PopT = Conditional<M <= 32, NativeType<32>, NativeType<64>>;
    using ReadT = NativeType<M>;
    auto entry = configuration.value_stack().last();
    auto value = static_cast<ReadT>(entry.to<PopT>());
    dbgln_if(WASM_TRACE_DEBUG, "stack({}) -> splat({})", value, M);
    set_top_m_splat<M, NativeType>(configuration, value);
}

template<typename M, template<typename> typename SetSign, typename VectorType>
VectorType BytecodeInterpreter::pop_vector(Configuration& configuration)
{
    // bounds checked by verifier.
    return bit_cast<VectorType>(configuration.value_stack().unsafe_take_last().to<u128>());
}

bool BytecodeInterpreter::call_address(Configuration& configuration, FunctionAddress address, CallAddressSource source)
{
    TRAP_IF_NOT(m_stack_info.size_free() >= Constants::minimum_stack_space_to_keep_free, "{}: {}", Constants::stack_exhaustion_message);

    auto instance = configuration.store().get(address);
    FunctionType const* type { nullptr };
    instance->visit([&](auto const& function) { type = &function.type(); });
    if (source == CallAddressSource::IndirectCall) {
        TRAP_IF_NOT(type->parameters().size() <= configuration.value_stack().size());
    }
    Vector<Value> args;
    args.ensure_capacity(type->parameters().size());
    auto span = configuration.value_stack().span().slice_from_end(type->parameters().size());
    for (auto& value : span)
        args.unchecked_append(value);

    configuration.value_stack().remove(configuration.value_stack().size() - span.size(), span.size());

    Result result { Trap::from_string("") };
    if (instance->has<WasmFunction>()) {
        CallFrameHandle handle { *this, configuration };
        result = configuration.call(*this, address, move(args));
    } else {
        result = configuration.call(*this, address, move(args));
    }

    if (result.is_trap()) {
        m_trap = move(result.trap());
        return true;
    }

    configuration.value_stack().ensure_capacity(configuration.value_stack().size() + result.values().size());
    for (auto& entry : result.values().in_reverse())
        configuration.value_stack().unchecked_append(entry);

    return false;
}

template<typename PopTypeLHS, typename PushType, typename Operator, typename PopTypeRHS, typename... Args>
bool BytecodeInterpreter::binary_numeric_operation(Configuration& configuration, Args&&... args)
{
    // bounds checked by verifier.
    auto rhs = configuration.value_stack().unsafe_take_last().to<PopTypeRHS>();
    auto& lhs_entry = configuration.value_stack().unsafe_last(); // bounds checked by verifier.
    auto lhs = lhs_entry.to<PopTypeLHS>();
    PushType result;
    auto call_result = Operator { forward<Args>(args)... }(lhs, rhs);
    if constexpr (IsSpecializationOf<decltype(call_result), AK::ErrorOr>) {
        if (call_result.is_error())
            return trap_if_not(false, call_result.error());
        result = call_result.release_value();
    } else {
        result = call_result;
    }
    dbgln_if(WASM_TRACE_DEBUG, "{} {} {} = {}", lhs, Operator::name(), rhs, result);
    lhs_entry = Value(result);
    return false;
}

template<typename PopType, typename PushType, typename Operator, typename... Args>
bool BytecodeInterpreter::unary_operation(Configuration& configuration, Args&&... args)
{
    auto& entry = configuration.value_stack().unsafe_last(); // bounds checked by verifier.
    auto value = entry.to<PopType>();
    auto call_result = Operator { forward<Args>(args)... }(value);
    PushType result;
    if constexpr (IsSpecializationOf<decltype(call_result), AK::ErrorOr>) {
        if (call_result.is_error())
            return trap_if_not(false, call_result.error());
        result = call_result.release_value();
    } else {
        result = call_result;
    }
    dbgln_if(WASM_TRACE_DEBUG, "map({}) {} = {}", Operator::name(), value, result);
    entry = Value(result);
    return false;
}

template<typename T>
struct ConvertToRaw {
    T operator()(T value)
    {
        return LittleEndian<T>(value);
    }
};

template<>
struct ConvertToRaw<float> {
    u32 operator()(float value)
    {
        ReadonlyBytes bytes { &value, sizeof(float) };
        FixedMemoryStream stream { bytes };
        auto res = stream.read_value<LittleEndian<u32>>().release_value_but_fixme_should_propagate_errors();
        return static_cast<u32>(res);
    }
};

template<>
struct ConvertToRaw<double> {
    u64 operator()(double value)
    {
        ReadonlyBytes bytes { &value, sizeof(double) };
        FixedMemoryStream stream { bytes };
        auto res = stream.read_value<LittleEndian<u64>>().release_value_but_fixme_should_propagate_errors();
        return static_cast<u64>(res);
    }
};

template<typename PopT, typename StoreT>
bool BytecodeInterpreter::pop_and_store(Configuration& configuration, Instruction const& instruction)
{
    auto& memarg = instruction.arguments().get<Instruction::MemoryArgument>();
    // bounds checked by verifier.
    auto entry = configuration.value_stack().unsafe_take_last();
    auto value = ConvertToRaw<StoreT> {}(entry.to<PopT>());
    dbgln_if(WASM_TRACE_DEBUG, "stack({}) -> temporary({}b)", value, sizeof(StoreT));
    auto base = configuration.value_stack().unsafe_take_last().to<i32>();
    return store_to_memory(configuration, memarg, { &value, sizeof(StoreT) }, base);
}

template<size_t N>
bool BytecodeInterpreter::pop_and_store_lane_n(Configuration& configuration, Instruction const& instruction)
{
    auto& memarg_and_lane = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    // bounds checked by verifier.
    auto vector = configuration.value_stack().unsafe_take_last().to<u128>();
    auto src = bit_cast<u8*>(&vector) + memarg_and_lane.lane * N / 8;
    auto base = configuration.value_stack().unsafe_take_last().to<u32>();
    return store_to_memory(configuration, memarg_and_lane.memory, { src, N / 8 }, base);
}

bool BytecodeInterpreter::store_to_memory(Configuration& configuration, Instruction::MemoryArgument const& arg, ReadonlyBytes data, u32 base)
{
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    u64 instance_address = static_cast<u64>(base) + arg.offset;
    Checked addition { instance_address };
    addition += data.size();
    if (addition.has_overflow() || addition.value() > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln_if(WASM_TRACE_DEBUG, "LibWasm: Memory access out of bounds (expected 0 <= {} and {} <= {})", instance_address, instance_address + data.size(), memory->size());
        return true;
    }
    dbgln_if(WASM_TRACE_DEBUG, "temporary({}b) -> store({})", data.size(), instance_address);
    data.copy_to(memory->data().bytes().slice(instance_address, data.size()));
    return false;
}

template<typename T>
T BytecodeInterpreter::read_value(ReadonlyBytes data)
{
    VERIFY(sizeof(T) <= data.size());
    if (bit_cast<FlatPtr>(data.data()) % alignof(T)) {
        alignas(T) u8 buf[sizeof(T)];
        memcpy(buf, data.data(), sizeof(T));
        return bit_cast<LittleEndian<T>>(buf);
    }
    return *bit_cast<LittleEndian<T> const*>(data.data());
}

template<>
float BytecodeInterpreter::read_value<float>(ReadonlyBytes data)
{
    return bit_cast<float>(read_value<u32>(data));
}

template<>
double BytecodeInterpreter::read_value<double>(ReadonlyBytes data)
{
    return bit_cast<double>(read_value<u64>(data));
}

bool BytecodeInterpreter::interpret_instruction_chain(Configuration& configuration, InstructionPointer& ip, InstructionPointer& ip_at_start, Span<Instruction const> instructions, InstructionPointer max_ip_value, u64& executed_instructions)
{
    (void)configuration;
    (void)ip_at_start;
    (void)instructions;
    (void)max_ip_value;
    (void)executed_instructions;
    (void)ip;
    return false;
}

}
