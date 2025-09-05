/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/RedBlackTree.h>
#include <AK/SIMDExtras.h>
#include <AK/Time.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Operators.h>
#include <LibWasm/Opcode.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Types.h>

using namespace AK::SIMD;

namespace Wasm {

template<typename T>
struct ConvertToRaw {
    T operator()(T value)
    {
        return LittleEndian<T>(value);
    }
};

template<>
struct ConvertToRaw<float> {
    u32 operator()(float value) const { return bit_cast<LittleEndian<u32>>(value); }
};

template<>
struct ConvertToRaw<double> {
    u64 operator()(double value) const { return bit_cast<LittleEndian<u64>>(value); }
};

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
    auto& expression = configuration.frame().expression();
    auto const should_limit_instruction_count = configuration.should_limit_instruction_count();
    if (!expression.compiled_instructions.dispatches.is_empty()) {
        if (should_limit_instruction_count)
            return interpret_impl<true, true>(configuration, expression);
        return interpret_impl<true, false>(configuration, expression);
    }
    if (should_limit_instruction_count)
        return interpret_impl<false, true>(configuration, expression);
    return interpret_impl<false, false>(configuration, expression);
}

template<bool HasCompiledList, bool HasDynamicInsnLimit>
void BytecodeInterpreter::interpret_impl(Configuration& configuration, Expression const& expression)
{
    auto& instructions = expression.instructions();
    u64 max_ip_value = (HasCompiledList ? expression.compiled_instructions.dispatches.size() : instructions.size()) - 1;
    auto current_ip_value = configuration.ip();
    u64 executed_instructions = 0;

    constexpr static u32 default_sources_and_destination = (to_underlying(Dispatch::RegisterOrStack::Stack) | (to_underlying(Dispatch::RegisterOrStack::Stack) << 2) | (to_underlying(Dispatch::RegisterOrStack::Stack) << 4));
    SourcesAndDestination addresses { .sources_and_destination = default_sources_and_destination };

    enum class CouldHaveChangedIP {
        No,
        Yes
    };

    auto const cc = expression.compiled_instructions.dispatches.data();

    while (true) {
        if constexpr (HasDynamicInsnLimit) {
            if (executed_instructions++ >= Constants::max_allowed_executed_instructions_per_call) [[unlikely]] {
                m_trap = Trap::from_string("Exceeded maximum allowed number of instructions");
                return;
            }
        }
        // bounds checked by loop condition.
        addresses.sources_and_destination = HasCompiledList
            ? cc[current_ip_value].sources_and_destination
            : default_sources_and_destination;
        auto const instruction = HasCompiledList
            ? cc[current_ip_value].instruction
            : &instructions.data()[current_ip_value];
        auto const opcode = (HasCompiledList
                ? cc[current_ip_value].instruction_opcode
                : instruction->opcode())
                                .value();

#define RUN_NEXT_INSTRUCTION() \
    {                          \
        ++current_ip_value;    \
        break;                 \
    }

        dbgln_if(WASM_TRACE_DEBUG, "Executing instruction {} at current_ip_value {}", instruction_name(instruction->opcode()), current_ip_value);
        if ((opcode & Instructions::SyntheticInstructionBase.value()) != Instructions::SyntheticInstructionBase.value())
            __builtin_prefetch(&instruction->arguments(), /* read */ 0, /* low temporal locality */ 1);

        switch (opcode) {
        case Instructions::local_get.value():
            configuration.push_to_destination(configuration.local(instruction->local_index()), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_const.value():
            configuration.push_to_destination(Value(instruction->arguments().unsafe_get<i32>()), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        case Instructions::synthetic_i32_add2local.value():
            configuration.push_to_destination(Value(static_cast<i32>(Operators::Add {}(configuration.local(instruction->local_index()).to<u32>(), configuration.local(instruction->arguments().get<LocalIndex>()).to<u32>()))), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        case Instructions::synthetic_i32_addconstlocal.value():
            configuration.push_to_destination(Value(static_cast<i32>(Operators::Add {}(configuration.local(instruction->local_index()).to<u32>(), instruction->arguments().unsafe_get<i32>()))), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        case Instructions::synthetic_i32_andconstlocal.value():
            configuration.push_to_destination(Value(Operators::BitAnd {}(configuration.local(instruction->local_index()).to<i32>(), instruction->arguments().unsafe_get<i32>())), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        case Instructions::synthetic_i32_storelocal.value():
            if (store_value(configuration, *instruction, ConvertToRaw<i32> {}(configuration.local(instruction->local_index()).to<i32>()), 0, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::synthetic_i64_storelocal.value():
            if (store_value(configuration, *instruction, ConvertToRaw<i64> {}(configuration.local(instruction->local_index()).to<i64>()), 0, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::synthetic_local_seti32_const.value():
            configuration.local(instruction->local_index()) = Value(instruction->arguments().unsafe_get<i32>());
            RUN_NEXT_INSTRUCTION();
        case Instructions::synthetic_call_00.value():
        case Instructions::synthetic_call_01.value():
        case Instructions::synthetic_call_10.value():
        case Instructions::synthetic_call_11.value():
        case Instructions::synthetic_call_20.value():
        case Instructions::synthetic_call_21.value():
        case Instructions::synthetic_call_30.value():
        case Instructions::synthetic_call_31.value(): {
            auto regs_copy = configuration.regs;
            auto index = instruction->arguments().get<FunctionIndex>();
            auto address = configuration.frame().module().functions()[index.value()];
            dbgln_if(WASM_TRACE_DEBUG, "[{}] call(#{} -> {})", current_ip_value, index.value(), address.value());
            if (call_address(configuration, address))
                return;
            configuration.regs = regs_copy;
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::unreachable.value():
            m_trap = Trap::from_string("Unreachable");
            return;
        case Instructions::nop.value():
            RUN_NEXT_INSTRUCTION();
        case Instructions::local_set.value(): {
            // bounds checked by verifier.
            configuration.local(instruction->local_index()) = configuration.take_source(0, addresses.sources);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::i64_const.value():
            configuration.push_to_destination(Value(instruction->arguments().unsafe_get<i64>()), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_const.value():
            configuration.push_to_destination(Value(instruction->arguments().unsafe_get<float>()), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_const.value():
            configuration.push_to_destination(Value(instruction->arguments().unsafe_get<double>()), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        case Instructions::block.value(): {
            size_t arity = 0;
            size_t param_arity = 0;
            auto& args = instruction->arguments().unsafe_get<Instruction::StructuredInstructionArgs>();
            if (args.block_type.kind() != BlockType::Empty) [[unlikely]] {
                switch (args.block_type.kind()) {
                case BlockType::Type:
                    arity = 1;
                    break;
                case BlockType::Index: {
                    auto& type = configuration.frame().module().types()[args.block_type.type_index().value()];
                    arity = type.results().size();
                    param_arity = type.parameters().size();
                    break;
                }
                case BlockType::Empty:
                    VERIFY_NOT_REACHED();
                }
            }

            configuration.label_stack().append(Label(arity, args.end_ip, configuration.value_stack().size() - param_arity));
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::loop.value(): {
            auto& args = instruction->arguments().get<Instruction::StructuredInstructionArgs>();
            size_t arity = 0;
            if (args.block_type.kind() == BlockType::Index) {
                auto& type = configuration.frame().module().types()[args.block_type.type_index().value()];
                arity = type.parameters().size();
            }
            configuration.label_stack().append(Label(arity, current_ip_value + 1, configuration.value_stack().size() - arity));
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::if_.value(): {
            size_t arity = 0;
            size_t param_arity = 0;
            auto& args = instruction->arguments().unsafe_get<Instruction::StructuredInstructionArgs>();
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

            auto value = configuration.take_source(0, addresses.sources).to<i32>();
            auto end_label = Label(arity, args.end_ip.value(), configuration.value_stack().size() - param_arity);
            if (value == 0) {
                if (args.else_ip.has_value()) {
                    current_ip_value = args.else_ip->value() - 1;
                    configuration.label_stack().append(end_label);
                } else {
                    current_ip_value = args.end_ip.value();
                }
            } else {
                configuration.label_stack().append(end_label);
            }
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::structured_end.value():
            configuration.label_stack().take_last();
            RUN_NEXT_INSTRUCTION();
        case Instructions::structured_else.value(): {
            auto label = configuration.label_stack().take_last();
            // Jump to the end label
            current_ip_value = label.continuation().value() - 1;
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::return_.value(): {
            configuration.label_stack().shrink(configuration.frame().label_index() + 1, true);
            current_ip_value = max_ip_value - 1;
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::br.value():
            current_ip_value = branch_to_label(configuration, instruction->arguments().get<LabelIndex>()).value();
            RUN_NEXT_INSTRUCTION();
        case Instructions::br_if.value(): {
            // bounds checked by verifier.
            auto cond = configuration.take_source(0, addresses.sources).to<i32>();
            if (cond == 0)
                RUN_NEXT_INSTRUCTION();
            current_ip_value = branch_to_label(configuration, instruction->arguments().get<LabelIndex>()).value();
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::br_table.value(): {
            auto& arguments = instruction->arguments().get<Instruction::TableBranchArgs>();
            auto i = configuration.take_source(0, addresses.sources).to<u32>();

            if (i >= arguments.labels.size()) {
                current_ip_value = branch_to_label(configuration, arguments.default_).value();
                RUN_NEXT_INSTRUCTION();
            }
            current_ip_value = branch_to_label(configuration, arguments.labels[i]).value();
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::call.value(): {
            auto index = instruction->arguments().get<FunctionIndex>();
            auto address = configuration.frame().module().functions()[index.value()];
            dbgln_if(WASM_TRACE_DEBUG, "call({})", address.value());
            if (call_address(configuration, address))
                return;
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::call_indirect.value(): {
            auto& args = instruction->arguments().get<Instruction::IndirectCallArgs>();
            auto table_address = configuration.frame().module().tables()[args.table.value()];
            auto table_instance = configuration.store().get(table_address);
            // bounds checked by verifier.
            auto index = configuration.take_source(0, addresses.sources).to<i32>();
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
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::i32_load.value():
            if (load_and_push<i32, i32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_load.value():
            if (load_and_push<i64, i64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_load.value():
            if (load_and_push<float, float>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_load.value():
            if (load_and_push<double, double>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_load8_s.value():
            if (load_and_push<i8, i32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_load8_u.value():
            if (load_and_push<u8, i32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_load16_s.value():
            if (load_and_push<i16, i32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_load16_u.value():
            if (load_and_push<u16, i32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_load8_s.value():
            if (load_and_push<i8, i64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_load8_u.value():
            if (load_and_push<u8, i64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_load16_s.value():
            if (load_and_push<i16, i64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_load16_u.value():
            if (load_and_push<u16, i64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_load32_s.value():
            if (load_and_push<i32, i64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_load32_u.value():
            if (load_and_push<u32, i64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_store.value():
            if (pop_and_store<i32, i32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_store.value():
            if (pop_and_store<i64, i64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_store.value():
            if (pop_and_store<float, float>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_store.value():
            if (pop_and_store<double, double>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_store8.value():
            if (pop_and_store<i32, i8>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_store16.value():
            if (pop_and_store<i32, i16>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_store8.value():
            if (pop_and_store<i64, i8>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_store16.value():
            if (pop_and_store<i64, i16>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_store32.value():
            if (pop_and_store<i64, i32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::local_tee.value(): {
            auto value = configuration.source_value(0, addresses.sources); // bounds checked by verifier.
            auto local_index = instruction->local_index();
            dbgln_if(WASM_TRACE_DEBUG, "stack:peek -> locals({})", local_index.value());
            configuration.frame().locals()[local_index.value()] = value;
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::global_get.value(): {
            auto global_index = instruction->arguments().get<GlobalIndex>();
            // This check here is for const expressions. In non-const expressions,
            // a validation error would have been thrown.
            TRAP_IN_LOOP_IF_NOT(global_index < configuration.frame().module().globals().size());
            auto address = configuration.frame().module().globals()[global_index.value()];
            dbgln_if(WASM_TRACE_DEBUG, "global({}) -> stack", address.value());
            auto global = configuration.store().get(address);
            configuration.push_to_destination(global->value(), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::global_set.value(): {
            auto global_index = instruction->arguments().get<GlobalIndex>();
            auto address = configuration.frame().module().globals()[global_index.value()];
            // bounds checked by verifier.
            auto value = configuration.take_source(0, addresses.sources);
            dbgln_if(WASM_TRACE_DEBUG, "stack -> global({})", address.value());
            auto global = configuration.store().get(address);
            global->set_value(value);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::memory_size.value(): {
            auto& args = instruction->arguments().get<Instruction::MemoryIndexArgument>();
            auto address = configuration.frame().module().memories().data()[args.memory_index.value()];
            auto instance = configuration.store().get(address);
            auto pages = instance->size() / Constants::page_size;
            dbgln_if(WASM_TRACE_DEBUG, "memory.size -> stack({})", pages);
            configuration.push_to_destination(Value(static_cast<i32>(pages)), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::memory_grow.value(): {
            auto& args = instruction->arguments().get<Instruction::MemoryIndexArgument>();
            auto address = configuration.frame().module().memories().data()[args.memory_index.value()];
            auto instance = configuration.store().get(address);
            i32 old_pages = instance->size() / Constants::page_size;
            auto& entry = configuration.source_value(0, addresses.sources); // bounds checked by verifier.
            auto new_pages = entry.to<i32>();
            dbgln_if(WASM_TRACE_DEBUG, "memory.grow({}), previously {} pages...", new_pages, old_pages);
            if (instance->grow(new_pages * Constants::page_size))
                entry = Value(old_pages);
            else
                entry = Value(-1);
            RUN_NEXT_INSTRUCTION();
        }
        // https://webassembly.github.io/spec/core/bikeshed/#exec-memory-fill
        case Instructions::memory_fill.value(): {
            auto& args = instruction->arguments().get<Instruction::MemoryIndexArgument>();
            auto address = configuration.frame().module().memories().data()[args.memory_index.value()];
            auto instance = configuration.store().get(address);
            // bounds checked by verifier.
            auto count = configuration.take_source(0, addresses.sources).to<u32>();
            u8 value = static_cast<u8>(configuration.take_source(1, addresses.sources).to<u32>());
            auto destination_offset = configuration.take_source(2, addresses.sources).to<u32>();

            Checked<u32> checked_end = destination_offset;
            checked_end += count;
            TRAP_IN_LOOP_IF_NOT(!checked_end.has_overflow() && static_cast<size_t>(checked_end.value()) <= instance->data().size());

            if (count == 0)
                RUN_NEXT_INSTRUCTION();

            Instruction::MemoryArgument memarg { 0, 0, args.memory_index };
            for (u32 i = 0; i < count; ++i) {
                if (store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i))
                    return;
            }

            RUN_NEXT_INSTRUCTION();
        }
        // https://webassembly.github.io/spec/core/bikeshed/#exec-memory-copy
        case Instructions::memory_copy.value(): {
            auto& args = instruction->arguments().get<Instruction::MemoryCopyArgs>();
            auto source_address = configuration.frame().module().memories().data()[args.src_index.value()];
            auto destination_address = configuration.frame().module().memories().data()[args.dst_index.value()];
            auto source_instance = configuration.store().get(source_address);
            auto destination_instance = configuration.store().get(destination_address);

            // bounds checked by verifier.
            auto count = configuration.take_source(0, addresses.sources).to<i32>();
            auto source_offset = configuration.take_source(1, addresses.sources).to<i32>();
            auto destination_offset = configuration.take_source(2, addresses.sources).to<i32>();

            Checked<size_t> source_position = source_offset;
            source_position.saturating_add(count);
            Checked<size_t> destination_position = destination_offset;
            destination_position.saturating_add(count);
            TRAP_IN_LOOP_IF_NOT(source_position <= source_instance->data().size());
            TRAP_IN_LOOP_IF_NOT(destination_position <= destination_instance->data().size());

            if (count == 0)
                RUN_NEXT_INSTRUCTION();

            Instruction::MemoryArgument memarg { 0, 0, args.dst_index };
            if (destination_offset <= source_offset) {
                for (auto i = 0; i < count; ++i) {
                    auto value = source_instance->data()[source_offset + i];
                    if (store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i))
                        return;
                }
            } else {
                for (auto i = count - 1; i >= 0; --i) {
                    auto value = source_instance->data()[source_offset + i];
                    if (store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i))
                        return;
                }
            }

            RUN_NEXT_INSTRUCTION();
        }
        // https://webassembly.github.io/spec/core/bikeshed/#exec-memory-init
        case Instructions::memory_init.value(): {
            auto& args = instruction->arguments().get<Instruction::MemoryInitArgs>();
            auto& data_address = configuration.frame().module().datas()[args.data_index.value()];
            auto& data = *configuration.store().get(data_address);
            auto memory_address = configuration.frame().module().memories().data()[args.memory_index.value()];
            auto memory = configuration.store().unsafe_get(memory_address);
            // bounds checked by verifier.
            auto count = configuration.take_source(0, addresses.sources).to<u32>();
            auto source_offset = configuration.take_source(1, addresses.sources).to<u32>();
            auto destination_offset = configuration.take_source(2, addresses.sources).to<u32>();

            Checked<size_t> source_position = source_offset;
            source_position.saturating_add(count);
            Checked<size_t> destination_position = destination_offset;
            destination_position.saturating_add(count);
            TRAP_IN_LOOP_IF_NOT(source_position <= data.data().size());
            TRAP_IN_LOOP_IF_NOT(destination_position <= memory->data().size());

            if (count == 0)
                RUN_NEXT_INSTRUCTION();

            Instruction::MemoryArgument memarg { 0, 0, args.memory_index };
            for (size_t i = 0; i < (size_t)count; ++i) {
                auto value = data.data()[source_offset + i];
                if (store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i))
                    return;
            }
            RUN_NEXT_INSTRUCTION();
        }
        // https://webassembly.github.io/spec/core/bikeshed/#exec-data-drop
        case Instructions::data_drop.value(): {
            auto data_index = instruction->arguments().get<DataIndex>();
            auto data_address = configuration.frame().module().datas()[data_index.value()];
            *configuration.store().get(data_address) = DataInstance({});
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::elem_drop.value(): {
            auto elem_index = instruction->arguments().get<ElementIndex>();
            auto address = configuration.frame().module().elements()[elem_index.value()];
            auto elem = configuration.store().get(address);
            *configuration.store().get(address) = ElementInstance(elem->type(), {});
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::table_init.value(): {
            auto& args = instruction->arguments().get<Instruction::TableElementArgs>();
            auto table_address = configuration.frame().module().tables()[args.table_index.value()];
            auto table = configuration.store().get(table_address);
            auto element_address = configuration.frame().module().elements()[args.element_index.value()];
            auto element = configuration.store().get(element_address);
            // bounds checked by verifier.
            auto count = configuration.take_source(0, addresses.sources).to<u32>();
            auto source_offset = configuration.take_source(1, addresses.sources).to<u32>();
            auto destination_offset = configuration.take_source(2, addresses.sources).to<u32>();

            Checked<u32> checked_source_offset = source_offset;
            Checked<u32> checked_destination_offset = destination_offset;
            checked_source_offset += count;
            checked_destination_offset += count;
            TRAP_IN_LOOP_IF_NOT(!checked_source_offset.has_overflow() && checked_source_offset <= (u32)element->references().size());
            TRAP_IN_LOOP_IF_NOT(!checked_destination_offset.has_overflow() && checked_destination_offset <= (u32)table->elements().size());

            for (u32 i = 0; i < count; ++i)
                table->elements()[destination_offset + i] = element->references()[source_offset + i];
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::table_copy.value(): {
            auto& args = instruction->arguments().get<Instruction::TableTableArgs>();
            auto source_address = configuration.frame().module().tables()[args.rhs.value()];
            auto destination_address = configuration.frame().module().tables()[args.lhs.value()];
            auto source_instance = configuration.store().get(source_address);
            auto destination_instance = configuration.store().get(destination_address);

            // bounds checked by verifier.
            auto count = configuration.take_source(0, addresses.sources).to<u32>();
            auto source_offset = configuration.take_source(1, addresses.sources).to<u32>();
            auto destination_offset = configuration.take_source(2, addresses.sources).to<u32>();

            Checked<size_t> source_position = source_offset;
            source_position.saturating_add(count);
            Checked<size_t> destination_position = destination_offset;
            destination_position.saturating_add(count);
            TRAP_IN_LOOP_IF_NOT(source_position <= source_instance->elements().size());
            TRAP_IN_LOOP_IF_NOT(destination_position <= destination_instance->elements().size());

            if (count == 0)
                RUN_NEXT_INSTRUCTION();

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

            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::table_fill.value(): {
            auto table_index = instruction->arguments().get<TableIndex>();
            auto address = configuration.frame().module().tables()[table_index.value()];
            auto table = configuration.store().get(address);
            // bounds checked by verifier.
            auto count = configuration.take_source(0, addresses.sources).to<u32>();
            auto value = configuration.take_source(1, addresses.sources);
            auto start = configuration.take_source(2, addresses.sources).to<u32>();

            Checked<u32> checked_offset = start;
            checked_offset += count;
            TRAP_IN_LOOP_IF_NOT(!checked_offset.has_overflow() && checked_offset <= (u32)table->elements().size());

            for (u32 i = 0; i < count; ++i)
                table->elements()[start + i] = value.to<Reference>();
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::table_set.value(): {
            // bounds checked by verifier.
            auto ref = configuration.take_source(0, addresses.sources);
            auto index = (size_t)(configuration.take_source(1, addresses.sources).to<i32>());
            auto table_index = instruction->arguments().get<TableIndex>();
            auto address = configuration.frame().module().tables()[table_index.value()];
            auto table = configuration.store().get(address);
            TRAP_IN_LOOP_IF_NOT(index < table->elements().size());
            table->elements()[index] = ref.to<Reference>();
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::table_get.value(): {
            // bounds checked by verifier.
            auto& index_value = configuration.source_value(0, addresses.sources);
            auto index = static_cast<size_t>(index_value.to<i32>());
            auto table_index = instruction->arguments().get<TableIndex>();
            auto address = configuration.frame().module().tables()[table_index.value()];
            auto table = configuration.store().get(address);
            TRAP_IN_LOOP_IF_NOT(index < table->elements().size());
            index_value = Value(table->elements()[index]);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::table_grow.value(): {
            // bounds checked by verifier.
            auto size = configuration.take_source(0, addresses.sources).to<u32>();
            auto fill_value = configuration.take_source(1, addresses.sources);
            auto table_index = instruction->arguments().get<TableIndex>();
            auto address = configuration.frame().module().tables()[table_index.value()];
            auto table = configuration.store().get(address);
            auto previous_size = table->elements().size();
            auto did_grow = table->grow(size, fill_value.to<Reference>());
            if (!did_grow) {
                configuration.push_to_destination(Value(-1), addresses.destination);
            } else {
                configuration.push_to_destination(Value(static_cast<i32>(previous_size)), addresses.destination);
            }
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::table_size.value(): {
            auto table_index = instruction->arguments().get<TableIndex>();
            auto address = configuration.frame().module().tables()[table_index.value()];
            auto table = configuration.store().get(address);
            configuration.push_to_destination(Value(static_cast<i32>(table->elements().size())), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::ref_null.value(): {
            auto type = instruction->arguments().get<ValueType>();
            configuration.push_to_destination(Value(Reference(Reference::Null { type })), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        };
        case Instructions::ref_func.value(): {
            auto index = instruction->arguments().get<FunctionIndex>().value();
            auto& functions = configuration.frame().module().functions();
            auto address = functions[index];
            configuration.push_to_destination(Value(Reference { Reference::Func { address, configuration.store().get_module_for(address) } }), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::ref_is_null.value(): {
            // bounds checked by verifier.
            auto ref = configuration.take_source(0, addresses.sources);
            configuration.push_to_destination(Value(static_cast<i32>(ref.to<Reference>().ref().has<Reference::Null>() ? 1 : 0)), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::drop.value():
            // bounds checked by verifier.
            configuration.take_source(0, addresses.sources);
            RUN_NEXT_INSTRUCTION();
        case Instructions::select.value():
        case Instructions::select_typed.value(): {
            // Note: The type seems to only be used for validation.
            auto value = configuration.take_source(0, addresses.sources).to<i32>(); // bounds checked by verifier.
            dbgln_if(WASM_TRACE_DEBUG, "select({})", value);
            auto rhs = configuration.take_source(1, addresses.sources);
            auto& lhs = configuration.source_value(2, addresses.sources); // bounds checked by verifier.
            lhs = value != 0 ? lhs : rhs;
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::i32_eqz.value():
            if (unary_operation<i32, i32, Operators::EqualsZero>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_eq.value():
            if (binary_numeric_operation<i32, i32, Operators::Equals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_ne.value():
            if (binary_numeric_operation<i32, i32, Operators::NotEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_lts.value():
            if (binary_numeric_operation<i32, i32, Operators::LessThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_ltu.value():
            if (binary_numeric_operation<u32, i32, Operators::LessThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_gts.value():
            if (binary_numeric_operation<i32, i32, Operators::GreaterThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_gtu.value():
            if (binary_numeric_operation<u32, i32, Operators::GreaterThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_les.value():
            if (binary_numeric_operation<i32, i32, Operators::LessThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_leu.value():
            if (binary_numeric_operation<u32, i32, Operators::LessThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_ges.value():
            if (binary_numeric_operation<i32, i32, Operators::GreaterThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_geu.value():
            if (binary_numeric_operation<u32, i32, Operators::GreaterThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_eqz.value():
            if (unary_operation<i64, i32, Operators::EqualsZero>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_eq.value():
            if (binary_numeric_operation<i64, i32, Operators::Equals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_ne.value():
            if (binary_numeric_operation<i64, i32, Operators::NotEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_lts.value():
            if (binary_numeric_operation<i64, i32, Operators::LessThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_ltu.value():
            if (binary_numeric_operation<u64, i32, Operators::LessThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_gts.value():
            if (binary_numeric_operation<i64, i32, Operators::GreaterThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_gtu.value():
            if (binary_numeric_operation<u64, i32, Operators::GreaterThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_les.value():
            if (binary_numeric_operation<i64, i32, Operators::LessThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_leu.value():
            if (binary_numeric_operation<u64, i32, Operators::LessThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_ges.value():
            if (binary_numeric_operation<i64, i32, Operators::GreaterThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_geu.value():
            if (binary_numeric_operation<u64, i32, Operators::GreaterThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_eq.value():
            if (binary_numeric_operation<float, i32, Operators::Equals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_ne.value():
            if (binary_numeric_operation<float, i32, Operators::NotEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_lt.value():
            if (binary_numeric_operation<float, i32, Operators::LessThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_gt.value():
            if (binary_numeric_operation<float, i32, Operators::GreaterThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_le.value():
            if (binary_numeric_operation<float, i32, Operators::LessThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_ge.value():
            if (binary_numeric_operation<float, i32, Operators::GreaterThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_eq.value():
            if (binary_numeric_operation<double, i32, Operators::Equals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_ne.value():
            if (binary_numeric_operation<double, i32, Operators::NotEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_lt.value():
            if (binary_numeric_operation<double, i32, Operators::LessThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_gt.value():
            if (binary_numeric_operation<double, i32, Operators::GreaterThan>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_le.value():
            if (binary_numeric_operation<double, i32, Operators::LessThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_ge.value():
            if (binary_numeric_operation<double, i32, Operators::GreaterThanOrEquals>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_clz.value():
            if (unary_operation<i32, i32, Operators::CountLeadingZeros>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_ctz.value():
            if (unary_operation<i32, i32, Operators::CountTrailingZeros>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_popcnt.value():
            if (unary_operation<i32, i32, Operators::PopCount>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_add.value():
            if (binary_numeric_operation<u32, i32, Operators::Add>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_sub.value():
            if (binary_numeric_operation<u32, i32, Operators::Subtract>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_mul.value():
            if (binary_numeric_operation<u32, i32, Operators::Multiply>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_divs.value():
            if (binary_numeric_operation<i32, i32, Operators::Divide>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_divu.value():
            if (binary_numeric_operation<u32, i32, Operators::Divide>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_rems.value():
            if (binary_numeric_operation<i32, i32, Operators::Modulo>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_remu.value():
            if (binary_numeric_operation<u32, i32, Operators::Modulo>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_and.value():
            if (binary_numeric_operation<i32, i32, Operators::BitAnd>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_or.value():
            if (binary_numeric_operation<i32, i32, Operators::BitOr>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_xor.value():
            if (binary_numeric_operation<i32, i32, Operators::BitXor>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_shl.value():
            if (binary_numeric_operation<u32, i32, Operators::BitShiftLeft>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_shrs.value():
            if (binary_numeric_operation<i32, i32, Operators::BitShiftRight>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_shru.value():
            if (binary_numeric_operation<u32, i32, Operators::BitShiftRight>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_rotl.value():
            if (binary_numeric_operation<u32, i32, Operators::BitRotateLeft>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_rotr.value():
            if (binary_numeric_operation<u32, i32, Operators::BitRotateRight>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_clz.value():
            if (unary_operation<i64, i64, Operators::CountLeadingZeros>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_ctz.value():
            if (unary_operation<i64, i64, Operators::CountTrailingZeros>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_popcnt.value():
            if (unary_operation<i64, i64, Operators::PopCount>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_add.value():
            if (binary_numeric_operation<u64, i64, Operators::Add>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_sub.value():
            if (binary_numeric_operation<u64, i64, Operators::Subtract>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_mul.value():
            if (binary_numeric_operation<u64, i64, Operators::Multiply>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_divs.value():
            if (binary_numeric_operation<i64, i64, Operators::Divide>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_divu.value():
            if (binary_numeric_operation<u64, i64, Operators::Divide>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_rems.value():
            if (binary_numeric_operation<i64, i64, Operators::Modulo>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_remu.value():
            if (binary_numeric_operation<u64, i64, Operators::Modulo>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_and.value():
            if (binary_numeric_operation<i64, i64, Operators::BitAnd>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_or.value():
            if (binary_numeric_operation<i64, i64, Operators::BitOr>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_xor.value():
            if (binary_numeric_operation<i64, i64, Operators::BitXor>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_shl.value():
            if (binary_numeric_operation<u64, i64, Operators::BitShiftLeft>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_shrs.value():
            if (binary_numeric_operation<i64, i64, Operators::BitShiftRight>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_shru.value():
            if (binary_numeric_operation<u64, i64, Operators::BitShiftRight>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_rotl.value():
            if (binary_numeric_operation<u64, i64, Operators::BitRotateLeft>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_rotr.value():
            if (binary_numeric_operation<u64, i64, Operators::BitRotateRight>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_abs.value():
            if (unary_operation<float, float, Operators::Absolute>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_neg.value():
            if (unary_operation<float, float, Operators::Negate>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_ceil.value():
            if (unary_operation<float, float, Operators::Ceil>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_floor.value():
            if (unary_operation<float, float, Operators::Floor>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_trunc.value():
            if (unary_operation<float, float, Operators::Truncate>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_nearest.value():
            if (unary_operation<float, float, Operators::NearbyIntegral>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_sqrt.value():
            if (unary_operation<float, float, Operators::SquareRoot>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_add.value():
            if (binary_numeric_operation<float, float, Operators::Add>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_sub.value():
            if (binary_numeric_operation<float, float, Operators::Subtract>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_mul.value():
            if (binary_numeric_operation<float, float, Operators::Multiply>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_div.value():
            if (binary_numeric_operation<float, float, Operators::Divide>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_min.value():
            if (binary_numeric_operation<float, float, Operators::Minimum>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_max.value():
            if (binary_numeric_operation<float, float, Operators::Maximum>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_copysign.value():
            if (binary_numeric_operation<float, float, Operators::CopySign>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_abs.value():
            if (unary_operation<double, double, Operators::Absolute>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_neg.value():
            if (unary_operation<double, double, Operators::Negate>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_ceil.value():
            if (unary_operation<double, double, Operators::Ceil>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_floor.value():
            if (unary_operation<double, double, Operators::Floor>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_trunc.value():
            if (unary_operation<double, double, Operators::Truncate>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_nearest.value():
            if (unary_operation<double, double, Operators::NearbyIntegral>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_sqrt.value():
            if (unary_operation<double, double, Operators::SquareRoot>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_add.value():
            if (binary_numeric_operation<double, double, Operators::Add>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_sub.value():
            if (binary_numeric_operation<double, double, Operators::Subtract>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_mul.value():
            if (binary_numeric_operation<double, double, Operators::Multiply>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_div.value():
            if (binary_numeric_operation<double, double, Operators::Divide>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_min.value():
            if (binary_numeric_operation<double, double, Operators::Minimum>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_max.value():
            if (binary_numeric_operation<double, double, Operators::Maximum>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_copysign.value():
            if (binary_numeric_operation<double, double, Operators::CopySign>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_wrap_i64.value():
            if (unary_operation<i64, i32, Operators::Wrap<i32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_trunc_sf32.value():
            if (unary_operation<float, i32, Operators::CheckedTruncate<i32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_trunc_uf32.value():
            if (unary_operation<float, i32, Operators::CheckedTruncate<u32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_trunc_sf64.value():
            if (unary_operation<double, i32, Operators::CheckedTruncate<i32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_trunc_uf64.value():
            if (unary_operation<double, i32, Operators::CheckedTruncate<u32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_trunc_sf32.value():
            if (unary_operation<float, i64, Operators::CheckedTruncate<i64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_trunc_uf32.value():
            if (unary_operation<float, i64, Operators::CheckedTruncate<u64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_trunc_sf64.value():
            if (unary_operation<double, i64, Operators::CheckedTruncate<i64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_trunc_uf64.value():
            if (unary_operation<double, i64, Operators::CheckedTruncate<u64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_extend_si32.value():
            if (unary_operation<i32, i64, Operators::Extend<i64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_extend_ui32.value():
            if (unary_operation<u32, i64, Operators::Extend<i64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_convert_si32.value():
            if (unary_operation<i32, float, Operators::Convert<float>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_convert_ui32.value():
            if (unary_operation<u32, float, Operators::Convert<float>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_convert_si64.value():
            if (unary_operation<i64, float, Operators::Convert<float>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_convert_ui64.value():
            if (unary_operation<u64, float, Operators::Convert<float>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_demote_f64.value():
            if (unary_operation<double, float, Operators::Demote>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_convert_si32.value():
            if (unary_operation<i32, double, Operators::Convert<double>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_convert_ui32.value():
            if (unary_operation<u32, double, Operators::Convert<double>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_convert_si64.value():
            if (unary_operation<i64, double, Operators::Convert<double>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_convert_ui64.value():
            if (unary_operation<u64, double, Operators::Convert<double>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_promote_f32.value():
            if (unary_operation<float, double, Operators::Promote>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_reinterpret_f32.value():
            if (unary_operation<float, i32, Operators::Reinterpret<i32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_reinterpret_f64.value():
            if (unary_operation<double, i64, Operators::Reinterpret<i64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32_reinterpret_i32.value():
            if (unary_operation<i32, float, Operators::Reinterpret<float>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64_reinterpret_i64.value():
            if (unary_operation<i64, double, Operators::Reinterpret<double>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_extend8_s.value():
            if (unary_operation<i32, i32, Operators::SignExtend<i8>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_extend16_s.value():
            if (unary_operation<i32, i32, Operators::SignExtend<i16>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_extend8_s.value():
            if (unary_operation<i64, i64, Operators::SignExtend<i8>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_extend16_s.value():
            if (unary_operation<i64, i64, Operators::SignExtend<i16>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_extend32_s.value():
            if (unary_operation<i64, i64, Operators::SignExtend<i32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_trunc_sat_f32_s.value():
            if (unary_operation<float, i32, Operators::SaturatingTruncate<i32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_trunc_sat_f32_u.value():
            if (unary_operation<float, i32, Operators::SaturatingTruncate<u32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_trunc_sat_f64_s.value():
            if (unary_operation<double, i32, Operators::SaturatingTruncate<i32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32_trunc_sat_f64_u.value():
            if (unary_operation<double, i32, Operators::SaturatingTruncate<u32>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_trunc_sat_f32_s.value():
            if (unary_operation<float, i64, Operators::SaturatingTruncate<i64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_trunc_sat_f32_u.value():
            if (unary_operation<float, i64, Operators::SaturatingTruncate<u64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_trunc_sat_f64_s.value():
            if (unary_operation<double, i64, Operators::SaturatingTruncate<i64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64_trunc_sat_f64_u.value():
            if (unary_operation<double, i64, Operators::SaturatingTruncate<u64>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_const.value():
            configuration.push_to_destination(Value(instruction->arguments().get<u128>()), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load.value():
            if (load_and_push<u128, u128>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load8x8_s.value():
            if (load_and_push_mxn<8, 8, MakeSigned>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load8x8_u.value():
            if (load_and_push_mxn<8, 8, MakeUnsigned>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load16x4_s.value():
            if (load_and_push_mxn<16, 4, MakeSigned>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load16x4_u.value():
            if (load_and_push_mxn<16, 4, MakeUnsigned>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load32x2_s.value():
            if (load_and_push_mxn<32, 2, MakeSigned>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load32x2_u.value():
            if (load_and_push_mxn<32, 2, MakeUnsigned>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load8_splat.value():
            if (load_and_push_m_splat<8>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load16_splat.value():
            if (load_and_push_m_splat<16>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load32_splat.value():
            if (load_and_push_m_splat<32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load64_splat.value():
            if (load_and_push_m_splat<64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_splat.value():
            pop_and_push_m_splat<8, NativeIntegralType>(configuration, *instruction, addresses);
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_splat.value():
            pop_and_push_m_splat<16, NativeIntegralType>(configuration, *instruction, addresses);
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_splat.value():
            pop_and_push_m_splat<32, NativeIntegralType>(configuration, *instruction, addresses);
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_splat.value():
            pop_and_push_m_splat<64, NativeIntegralType>(configuration, *instruction, addresses);
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_splat.value():
            pop_and_push_m_splat<32, NativeFloatingType>(configuration, *instruction, addresses);
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_splat.value():
            pop_and_push_m_splat<64, NativeFloatingType>(configuration, *instruction, addresses);
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_shuffle.value(): {
            auto& arg = instruction->arguments().get<Instruction::ShuffleArgument>();
            auto b = pop_vector<u8, MakeUnsigned>(configuration, 0, addresses);
            auto a = pop_vector<u8, MakeUnsigned>(configuration, 1, addresses);
            using VectorType = Native128ByteVectorOf<u8, MakeUnsigned>;
            VectorType result;
            for (size_t i = 0; i < 16; ++i)
                if (arg.lanes[i] < 16)
                    result[i] = a[arg.lanes[i]];
                else
                    result[i] = b[arg.lanes[i] - 16];
            configuration.push_to_destination(Value(bit_cast<u128>(result)), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::v128_store.value():
            if (pop_and_store<u128, u128>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_shl.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<16>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_shr_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<16, MakeUnsigned>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_shr_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<16, MakeSigned>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_shl.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<8>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_shr_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<8, MakeUnsigned>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_shr_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<8, MakeSigned>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_shl.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<4>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_shr_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<4, MakeUnsigned>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_shr_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<4, MakeSigned>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_shl.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<2>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_shr_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<2, MakeUnsigned>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_shr_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorShiftRight<2, MakeSigned>, i32>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_swizzle.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorSwizzle>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_extract_lane_s.value():
            if (unary_operation<u128, i8, Operators::VectorExtractLane<16, MakeSigned>>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_extract_lane_u.value():
            if (unary_operation<u128, u8, Operators::VectorExtractLane<16, MakeUnsigned>>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extract_lane_s.value():
            if (unary_operation<u128, i16, Operators::VectorExtractLane<8, MakeSigned>>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extract_lane_u.value():
            if (unary_operation<u128, u16, Operators::VectorExtractLane<8, MakeUnsigned>>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extract_lane.value():
            if (unary_operation<u128, i32, Operators::VectorExtractLane<4, MakeSigned>>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_extract_lane.value():
            if (unary_operation<u128, i64, Operators::VectorExtractLane<2, MakeSigned>>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_extract_lane.value():
            if (unary_operation<u128, float, Operators::VectorExtractLaneFloat<4>>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_extract_lane.value():
            if (unary_operation<u128, double, Operators::VectorExtractLaneFloat<2>>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_replace_lane.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<16, i32>, i32>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_replace_lane.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<8, i32>, i32>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_replace_lane.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<4>, i32>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_replace_lane.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<2>, i64>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_replace_lane.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<4, float>, float>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_replace_lane.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<2, double>, double>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_eq.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::Equals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_ne.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::NotEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_lt_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThan, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_lt_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThan, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_gt_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThan, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_gt_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThan, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_le_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThanOrEquals, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_le_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThanOrEquals, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_ge_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThanOrEquals, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_ge_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThanOrEquals, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_abs.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::Absolute>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_neg.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::Negate>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_all_true.value():
            if (unary_operation<u128, i32, Operators::VectorAllTrue<16>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_popcnt.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::PopCount>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_add.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Add>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_sub.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Subtract>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_avgr_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Average, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_add_sat_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<i8, Operators::Add>, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_add_sat_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<u8, Operators::Add>, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_sub_sat_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<i8, Operators::Subtract>, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_sub_sat_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<u8, Operators::Subtract>, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_min_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Minimum, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_min_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Minimum, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_max_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Maximum, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_max_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Maximum, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_eq.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::Equals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_ne.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::NotEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_lt_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThan, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_lt_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThan, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_gt_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThan, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_gt_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThan, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_le_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThanOrEquals, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_le_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThanOrEquals, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_ge_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThanOrEquals, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_ge_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThanOrEquals, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_abs.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<8, Operators::Absolute>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_neg.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<8, Operators::Negate>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_all_true.value():
            if (unary_operation<u128, i32, Operators::VectorAllTrue<8>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_add.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Add>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_sub.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Subtract>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_mul.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Multiply>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_avgr_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Average, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_add_sat_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Add>, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_add_sat_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<u16, Operators::Add>, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_sub_sat_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Subtract>, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_sub_sat_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<u16, Operators::Subtract>, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_min_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Minimum, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_min_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Minimum, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_max_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Maximum, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_max_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Maximum, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extend_low_i8x16_s.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::Low, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extend_high_i8x16_s.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::High, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extend_low_i8x16_u.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::Low, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extend_high_i8x16_u.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::High, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extadd_pairwise_i8x16_s.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<8, Operators::Add, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extadd_pairwise_i8x16_u.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<8, Operators::Add, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extmul_low_i8x16_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extmul_high_i8x16_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::High, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extmul_low_i8x16_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_extmul_high_i8x16_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_eq.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::Equals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_ne.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::NotEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_lt_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThan, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_lt_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThan, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_gt_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThan, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_gt_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThan, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_le_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThanOrEquals, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_le_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThanOrEquals, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_ge_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThanOrEquals, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_ge_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThanOrEquals, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_abs.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<4, Operators::Absolute>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_neg.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<4, Operators::Negate, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_all_true.value():
            if (unary_operation<u128, i32, Operators::VectorAllTrue<4>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_add.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Add, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_sub.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Subtract, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_mul.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Multiply, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_min_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Minimum, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_min_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Minimum, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_max_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Maximum, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_max_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Maximum, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extend_low_i16x8_s.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::Low, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extend_high_i16x8_s.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::High, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extend_low_i16x8_u.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::Low, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extend_high_i16x8_u.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::High, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extadd_pairwise_i16x8_s.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<4, Operators::Add, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extadd_pairwise_i16x8_u.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<4, Operators::Add, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extmul_low_i16x8_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extmul_high_i16x8_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::High, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extmul_low_i16x8_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_extmul_high_i16x8_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_eq.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::Equals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_ne.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::NotEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_lt_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::LessThan, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_gt_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::GreaterThan, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_le_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::LessThanOrEquals, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_ge_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::GreaterThanOrEquals, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_abs.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<2, Operators::Absolute>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_neg.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<2, Operators::Negate, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_all_true.value():
            if (unary_operation<u128, i32, Operators::VectorAllTrue<2>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_add.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Add, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_sub.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Subtract, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_mul.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Multiply, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_extend_low_i32x4_s.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::Low, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_extend_high_i32x4_s.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::High, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_extend_low_i32x4_u.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::Low, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_extend_high_i32x4_u.value():
            if (unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::High, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_extmul_low_i32x4_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_extmul_high_i32x4_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::High, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_extmul_low_i32x4_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_extmul_high_i32x4_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_eq.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::Equals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_ne.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::NotEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_lt.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::LessThan>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_gt.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::GreaterThan>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_le.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::LessThanOrEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_ge.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::GreaterThanOrEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_min.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Minimum>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_max.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Maximum>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_eq.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::Equals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_ne.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::NotEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_lt.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::LessThan>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_gt.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::GreaterThan>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_le.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::LessThanOrEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_ge.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::GreaterThanOrEquals>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_min.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Minimum>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_max.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Maximum>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_div.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Divide>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_mul.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Multiply>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_sub.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Subtract>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_add.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Add>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_pmin.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::PseudoMinimum>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_pmax.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::PseudoMaximum>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_div.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Divide>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_mul.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Multiply>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_sub.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Subtract>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_add.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Add>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_pmin.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::PseudoMinimum>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_pmax.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::PseudoMaximum>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_ceil.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Ceil>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_floor.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Floor>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_trunc.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Truncate>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_nearest.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::NearbyIntegral>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_sqrt.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::SquareRoot>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_neg.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Negate>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_abs.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Absolute>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_ceil.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Ceil>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_floor.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Floor>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_trunc.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Truncate>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_nearest.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::NearbyIntegral>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_sqrt.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::SquareRoot>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_neg.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Negate>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_abs.value():
            if (unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Absolute>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_and.value():
            if (binary_numeric_operation<u128, u128, Operators::BitAnd>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_or.value():
            if (binary_numeric_operation<u128, u128, Operators::BitOr>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_xor.value():
            if (binary_numeric_operation<u128, u128, Operators::BitXor>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_not.value():
            if (unary_operation<u128, u128, Operators::BitNot>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_andnot.value():
            if (binary_numeric_operation<u128, u128, Operators::BitAndNot>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_bitselect.value(): {
            // bounds checked by verifier.
            auto mask = configuration.take_source(0, addresses.sources).to<u128>();
            auto false_vector = configuration.take_source(1, addresses.sources).to<u128>();
            auto true_vector = configuration.take_source(2, addresses.sources).to<u128>();
            u128 result = (true_vector & mask) | (false_vector & ~mask);
            configuration.push_to_destination(Value(result), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::v128_any_true.value(): {
            auto vector = configuration.take_source(0, addresses.sources).to<u128>(); // bounds checked by verifier.
            configuration.push_to_destination(Value(static_cast<i32>(vector != 0)), addresses.destination);
            RUN_NEXT_INSTRUCTION();
        }
        case Instructions::v128_load8_lane.value():
            if (load_and_push_lane_n<8>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load16_lane.value():
            if (load_and_push_lane_n<16>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load32_lane.value():
            if (load_and_push_lane_n<32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load64_lane.value():
            if (load_and_push_lane_n<64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load32_zero.value():
            if (load_and_push_zero_n<32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_load64_zero.value():
            if (load_and_push_zero_n<64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_store8_lane.value():
            if (pop_and_store_lane_n<8>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_store16_lane.value():
            if (pop_and_store_lane_n<16>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_store32_lane.value():
            if (pop_and_store_lane_n<32>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::v128_store64_lane.value():
            if (pop_and_store_lane_n<64>(configuration, *instruction, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_trunc_sat_f32x4_s.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, f32, Operators::SaturatingTruncate<i32>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_trunc_sat_f32x4_u.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, f32, Operators::SaturatingTruncate<u32>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_bitmask.value():
            if (unary_operation<u128, i32, Operators::VectorBitmask<16>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_bitmask.value():
            if (unary_operation<u128, i32, Operators::VectorBitmask<8>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_bitmask.value():
            if (unary_operation<u128, i32, Operators::VectorBitmask<4>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i64x2_bitmask.value():
            if (unary_operation<u128, i32, Operators::VectorBitmask<2>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_dot_i16x8_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorDotProduct<4>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_narrow_i16x8_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorNarrow<16, i8>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i8x16_narrow_i16x8_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorNarrow<16, u8>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_narrow_i32x4_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorNarrow<8, i16>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_narrow_i32x4_u.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorNarrow<8, u16>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i16x8_q15mulr_sat_s.value():
            if (binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Q15Mul>, MakeSigned>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_convert_i32x4_s.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, i32, Operators::Convert<f32>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_convert_i32x4_u.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, u32, Operators::Convert<f32>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_convert_low_i32x4_s.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, i32, Operators::Convert<f64>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_convert_low_i32x4_u.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, u32, Operators::Convert<f64>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f32x4_demote_f64x2_zero.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::Convert<f32>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::f64x2_promote_low_f32x4.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, f32, Operators::Convert<f64>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_trunc_sat_f64x2_s_zero.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::SaturatingTruncate<i32>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::i32x4_trunc_sat_f64x2_u_zero.value():
            if (unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::SaturatingTruncate<u32>>>(configuration, addresses))
                return;
            RUN_NEXT_INSTRUCTION();
        case Instructions::synthetic_end_expression.value():
            return;
        default:
            VERIFY_NOT_REACHED();
        }
    }
}

InstructionPointer BytecodeInterpreter::branch_to_label(Configuration& configuration, LabelIndex index)
{
    dbgln_if(WASM_TRACE_DEBUG, "Branch to label with index {}...", index.value());
    auto& label_stack = configuration.label_stack();
    label_stack.shrink(label_stack.size() - index.value(), true);
    auto label = configuration.label_stack().last();
    dbgln_if(WASM_TRACE_DEBUG, "...which is actually IP {}, and has {} result(s)", label.continuation().value(), label.arity());

    configuration.value_stack().remove(label.stack_height(), configuration.value_stack().size() - label.stack_height() - label.arity());
    return label.continuation().value() - 1;
}

template<typename ReadType, typename PushType>
bool BytecodeInterpreter::load_and_push(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto& entry = configuration.source_value(0, addresses.sources); // bounds checked by verifier.
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
bool BytecodeInterpreter::load_and_push_mxn(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto& entry = configuration.source_value(0, addresses.sources); // bounds checked by verifier.
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
bool BytecodeInterpreter::load_and_push_lane_n(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto memarg_and_lane = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    auto& address = configuration.frame().module().memories()[memarg_and_lane.memory.memory_index.value()];
    auto memory = configuration.store().get(address);
    // bounds checked by verifier.
    auto vector = configuration.take_source(0, addresses.sources).to<u128>();
    auto base = configuration.take_source(1, addresses.sources).to<u32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + memarg_and_lane.memory.offset;
    if (instance_address + N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        return true;
    }
    auto slice = memory->data().bytes().slice(instance_address, N / 8);
    auto dst = bit_cast<u8*>(&vector) + memarg_and_lane.lane * N / 8;
    memcpy(dst, slice.data(), N / 8);
    configuration.push_to_destination(Value(vector), addresses.destination);
    return false;
}

template<size_t N>
bool BytecodeInterpreter::load_and_push_zero_n(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto memarg_and_lane = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[memarg_and_lane.memory_index.value()];
    auto memory = configuration.store().get(address);
    // bounds checked by verifier.
    auto base = configuration.take_source(0, addresses.sources).to<u32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + memarg_and_lane.offset;
    if (instance_address + N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        return true;
    }
    auto slice = memory->data().bytes().slice(instance_address, N / 8);
    u128 vector = 0;
    memcpy(&vector, slice.data(), N / 8);
    configuration.push_to_destination(Value(vector), addresses.destination);
    return false;
}

template<size_t M>
bool BytecodeInterpreter::load_and_push_m_splat(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto& entry = configuration.source_value(0, addresses.sources); // bounds checked by verifier.
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
    set_top_m_splat<M, NativeIntegralType>(configuration, value, addresses);
    return false;
}

template<size_t M, template<size_t> typename NativeType>
void BytecodeInterpreter::set_top_m_splat(Wasm::Configuration& configuration, NativeType<M> value, SourcesAndDestination const& addresses)
{
    auto push = [&](auto result) {
        configuration.source_value(0, addresses.sources) = Value(bit_cast<u128>(result));
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
void BytecodeInterpreter::pop_and_push_m_splat(Wasm::Configuration& configuration, Instruction const&, SourcesAndDestination const& addresses)
{
    using PopT = Conditional<M <= 32, NativeType<32>, NativeType<64>>;
    using ReadT = NativeType<M>;
    auto entry = configuration.source_value(0, addresses.sources);
    auto value = static_cast<ReadT>(entry.to<PopT>());
    dbgln_if(WASM_TRACE_DEBUG, "stack({}) -> splat({})", value, M);
    set_top_m_splat<M, NativeType>(configuration, value, addresses);
}

template<typename M, template<typename> typename SetSign, typename VectorType>
VectorType BytecodeInterpreter::pop_vector(Configuration& configuration, size_t source, SourcesAndDestination const& addresses)
{
    // bounds checked by verifier.
    return bit_cast<VectorType>(configuration.take_source(source, addresses.sources).to<u128>());
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
    if (!type->parameters().is_empty()) {
        args.ensure_capacity(type->parameters().size());
        auto span = configuration.value_stack().span().slice_from_end(type->parameters().size());
        for (auto& value : span)
            args.unchecked_append(value);

        configuration.value_stack().remove(configuration.value_stack().size() - span.size(), span.size());
    }

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

    if (!result.values().is_empty()) {
        configuration.value_stack().ensure_capacity(configuration.value_stack().size() + result.values().size());
        for (auto& entry : result.values().in_reverse())
            configuration.value_stack().unchecked_append(entry);
    }

    return false;
}

template<typename PopTypeLHS, typename PushType, typename Operator, typename PopTypeRHS, typename... Args>
bool BytecodeInterpreter::binary_numeric_operation(Configuration& configuration, SourcesAndDestination const& addresses, Args&&... args)
{
    // bounds checked by Nor.
    auto rhs = configuration.take_source(0, addresses.sources).to<PopTypeRHS>();
    auto lhs = configuration.take_source(1, addresses.sources).to<PopTypeLHS>(); // bounds checked by verifier.
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
    configuration.push_to_destination(Value(result), addresses.destination);
    return false;
}

template<typename PopType, typename PushType, typename Operator, typename... Args>
bool BytecodeInterpreter::unary_operation(Configuration& configuration, SourcesAndDestination const& addresses, Args&&... args)
{
    auto& entry = configuration.source_value(0, addresses.sources); // bounds checked by verifier.
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

template<typename PopT, typename StoreT>
bool BytecodeInterpreter::pop_and_store(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    // bounds checked by verifier.
    auto entry = configuration.take_source(0, addresses.sources);
    auto value = ConvertToRaw<StoreT> {}(entry.to<PopT>());
    return store_value(configuration, instruction, value, 1, addresses);
}

template<typename StoreT>
bool BytecodeInterpreter::store_value(Configuration& configuration, Instruction const& instruction, StoreT value, size_t address_source, SourcesAndDestination const& addresses)
{
    auto& memarg = instruction.arguments().unsafe_get<Instruction::MemoryArgument>();
    dbgln_if(WASM_TRACE_DEBUG, "stack({}) -> temporary({}b)", value, sizeof(StoreT));
    auto base = configuration.take_source(address_source, addresses.sources).to<i32>();
    return store_to_memory(configuration, memarg, { &value, sizeof(StoreT) }, base);
}

template<size_t N>
bool BytecodeInterpreter::pop_and_store_lane_n(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto& memarg_and_lane = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    // bounds checked by verifier.
    auto vector = configuration.take_source(0, addresses.sources).to<u128>();
    auto src = bit_cast<u8*>(&vector) + memarg_and_lane.lane * N / 8;
    auto base = configuration.take_source(1, addresses.sources).to<u32>();
    return store_to_memory(configuration, memarg_and_lane.memory, { src, N / 8 }, base);
}

bool BytecodeInterpreter::store_to_memory(Configuration& configuration, Instruction::MemoryArgument const& arg, ReadonlyBytes data, u32 base)
{
    auto& address = configuration.frame().module().memories().data()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    u64 instance_address = static_cast<u64>(base) + arg.offset;
    Checked addition { instance_address };
    addition += data.size();
    if (addition.has_overflow() || addition.value() > memory->size()) [[unlikely]] {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln_if(WASM_TRACE_DEBUG, "LibWasm: Memory access out of bounds (expected 0 <= {} and {} <= {})", instance_address, instance_address + data.size(), memory->size());
        return true;
    }
    dbgln_if(WASM_TRACE_DEBUG, "temporary({}b) -> store({})", data.size(), instance_address);
    (void)data.copy_to(memory->data().bytes().slice(instance_address, data.size()));
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

CompiledInstructions try_compile_instructions(Expression const& expression, Span<FunctionType const> functions)
{
    CompiledInstructions result;
    result.dispatches.ensure_capacity(expression.instructions().size());
    result.extra_instruction_storage.ensure_capacity(expression.instructions().size());
    i32 i32_const_value { 0 };
    LocalIndex local_index_0 { 0 };
    LocalIndex local_index_1 { 0 };
    enum class InsnPatternState {
        Nothing,
        GetLocal,
        GetLocalI32Const,
        GetLocalx2,
        I32Const,
        I32ConstGetLocal,
    } pattern_state { InsnPatternState::Nothing };
    static Instruction nop { Instructions::nop };
    constexpr auto default_dispatch = [](Instruction const& instruction) {
        return Dispatch {
            instruction.opcode(),
            &instruction,
            { .sources = { Dispatch::Stack, Dispatch::Stack, Dispatch::Stack }, .destination = Dispatch::Stack }
        };
    };

    for (auto& instruction : expression.instructions()) {
        if (instruction.opcode() == Instructions::call) {
            auto& function = functions[instruction.arguments().get<FunctionIndex>().value()];
            if (function.results().size() <= 1 && function.parameters().size() < 4) {
                pattern_state = InsnPatternState::Nothing;
                OpCode op { Instructions::synthetic_call_00.value() + function.parameters().size() * 2 + function.results().size() };
                result.extra_instruction_storage.unchecked_append(Instruction(
                    op,
                    instruction.arguments()));
                result.dispatches.unchecked_append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                continue;
            }
        }

        switch (pattern_state) {
        case InsnPatternState::Nothing:
            if (instruction.opcode() == Instructions::local_get) {
                local_index_0 = instruction.local_index();
                pattern_state = InsnPatternState::GetLocal;
            } else if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::I32Const;
            }
            break;
        case InsnPatternState::GetLocal:
            if (instruction.opcode() == Instructions::local_get) {
                local_index_1 = instruction.local_index();
                pattern_state = InsnPatternState::GetLocalx2;
            } else if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::GetLocalI32Const;
            } else if (instruction.opcode() == Instructions::i32_store) {
                // `local.get a; i32.store m` -> `i32.storelocal a m`.
                result.dispatches[result.dispatches.size() - 1] = default_dispatch(nop);
                result.extra_instruction_storage.append(Instruction(
                    Instructions::synthetic_i32_storelocal,
                    local_index_0,
                    instruction.arguments()));

                result.dispatches.append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else if (instruction.opcode() == Instructions::i64_store) {
                // `local.get a; i64.store m` -> `i64.storelocal a m`.
                result.dispatches[result.dispatches.size() - 1] = default_dispatch(nop);
                result.extra_instruction_storage.append(Instruction(
                    Instructions::synthetic_i64_storelocal,
                    local_index_0,
                    instruction.arguments()));

                result.dispatches.append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else {
                pattern_state = InsnPatternState::Nothing;
            }
            break;
        case InsnPatternState::GetLocalx2:
            if (instruction.opcode() == Instructions::i32_add) {
                // `local.get a; local.get b; i32.add` -> `i32.add_2local a b`.
                // Replace the previous two ops with noops, and add i32.add_2local.
                result.dispatches[result.dispatches.size() - 1] = default_dispatch(nop);
                result.dispatches[result.dispatches.size() - 2] = default_dispatch(nop);
                result.extra_instruction_storage.append(Instruction {
                    Instructions::synthetic_i32_add2local,
                    local_index_0,
                    local_index_1,
                });
                result.dispatches.append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i32_store) {
                // `local.get a; i32.store m` -> `i32.storelocal a m`.
                result.dispatches[result.dispatches.size() - 1] = default_dispatch(nop);
                result.extra_instruction_storage.append(Instruction(
                    Instructions::synthetic_i32_storelocal,
                    local_index_1,
                    instruction.arguments()));

                result.dispatches.append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i64_store) {
                // `local.get a; i64.store m` -> `i64.storelocal a m`.
                result.dispatches[result.dispatches.size() - 1] = default_dispatch(nop);
                result.extra_instruction_storage.append(Instruction(
                    Instructions::synthetic_i64_storelocal,
                    local_index_1,
                    instruction.arguments()));

                result.dispatches.append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i32_const) {
                swap(local_index_0, local_index_1);
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::GetLocalI32Const;
            } else {
                pattern_state = InsnPatternState::Nothing;
            }
            break;
        case InsnPatternState::I32Const:
            if (instruction.opcode() == Instructions::local_get) {
                local_index_0 = instruction.local_index();
                pattern_state = InsnPatternState::I32ConstGetLocal;
            } else if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
            } else if (instruction.opcode() == Instructions::local_set) {
                // `i32.const a; local.set b` -> `local.seti32_const b a`.
                result.dispatches[result.dispatches.size() - 1] = default_dispatch(nop);
                result.extra_instruction_storage.append(Instruction(
                    Instructions::synthetic_local_seti32_const,
                    instruction.local_index(),
                    i32_const_value));
                result.dispatches.append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else {
                pattern_state = InsnPatternState::Nothing;
            }
            break;
        case InsnPatternState::GetLocalI32Const:
            if (instruction.opcode() == Instructions::local_set) {
                // `i32.const a; local.set b` -> `local.seti32_const b a`.
                result.dispatches[result.dispatches.size() - 1] = default_dispatch(nop);
                result.extra_instruction_storage.append(Instruction(
                    Instructions::synthetic_local_seti32_const,
                    instruction.local_index(),
                    i32_const_value));
                result.dispatches.append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::I32Const;
                break;
            }
            if (instruction.opcode() == Instructions::local_get) {
                local_index_0 = instruction.local_index();
                pattern_state = InsnPatternState::I32ConstGetLocal;
                break;
            }
            [[fallthrough]];
        case InsnPatternState::I32ConstGetLocal:
            if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::GetLocalI32Const;
            } else if (instruction.opcode() == Instructions::local_get) {
                swap(local_index_0, local_index_1);
                local_index_1 = instruction.local_index();
                pattern_state = InsnPatternState::GetLocalx2;
            } else if (instruction.opcode() == Instructions::i32_add) {
                // `i32.const a; local.get b; i32.add` -> `i32.add_constlocal b a`.
                // Replace the previous two ops with noops, and add i32.add_constlocal.
                result.dispatches[result.dispatches.size() - 1] = default_dispatch(nop);
                result.dispatches[result.dispatches.size() - 2] = default_dispatch(nop);
                result.extra_instruction_storage.append(Instruction(
                    Instructions::synthetic_i32_addconstlocal,
                    local_index_0,
                    i32_const_value));

                result.dispatches.append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i32_and) {
                // `i32.const a; local.get b; i32.add` -> `i32.and_constlocal b a`.
                // Replace the previous two ops with noops, and add i32.and_constlocal.
                result.dispatches[result.dispatches.size() - 1] = default_dispatch(nop);
                result.dispatches[result.dispatches.size() - 2] = default_dispatch(nop);
                result.extra_instruction_storage.append(Instruction(
                    Instructions::synthetic_i32_andconstlocal,
                    local_index_0,
                    i32_const_value));

                result.dispatches.append(default_dispatch(result.extra_instruction_storage.unsafe_last()));
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            pattern_state = InsnPatternState::Nothing;
            break;
        }
        result.dispatches.unchecked_append(default_dispatch(instruction));
    }

    // Remove all nops (that were either added by the above patterns or were already present in the original instructions),
    // and adjust jumps accordingly.
    Vector<size_t> nops_to_remove;
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        if (result.dispatches[i].instruction->opcode() == Instructions::nop)
            nops_to_remove.append(i);
    }

    auto nops_to_remove_span = nops_to_remove.span();
    size_t offset_accumulated = 0;
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        if (result.dispatches[i].instruction->opcode() == Instructions::nop) {
            offset_accumulated++;
            nops_to_remove_span = nops_to_remove_span.slice(1);
            continue;
        }

        auto& args = result.dispatches[i].instruction->arguments();
        if (auto ptr = args.get_pointer<Instruction::StructuredInstructionArgs>()) {
            auto offset_to = [&](InstructionPointer ip) {
                size_t offset = 0;
                for (auto nop_ip : nops_to_remove_span) {
                    if (nop_ip < ip.value())
                        ++offset;
                    else
                        break;
                }
                return offset;
            };

            InstructionPointer end_ip = ptr->end_ip.value() - offset_accumulated - offset_to(ptr->end_ip - ptr->else_ip.has_value());
            auto else_ip = ptr->else_ip.map([&](InstructionPointer const& ip) -> InstructionPointer { return ip.value() - offset_accumulated - offset_to(ip - 1); });
            auto instruction = *result.dispatches[i].instruction;
            instruction.arguments() = Instruction::StructuredInstructionArgs {
                .block_type = ptr->block_type,
                .end_ip = end_ip,
                .else_ip = else_ip,
            };
            result.extra_instruction_storage.append(move(instruction));
            result.dispatches[i].instruction = &result.extra_instruction_storage.unsafe_last();
            result.dispatches[i].instruction_opcode = result.dispatches[i].instruction->opcode();
        }
    }
    for (auto index : nops_to_remove.in_reverse())
        result.dispatches.remove(index);

    // Allocate registers for instructions, meeting the following constraints:
    // - Any instruction that produces polymorphic stack, or requires its inputs on the stack must sink all active values to the stack.
    // - All instructions must have the same location for their last input and their destination value (if any).
    // - Any value left at the end of the expression must be on the stack.
    // - All inputs and outputs of call instructions with <4 inputs and <=1 output must be on the stack.

    using ValueID = DistinctNumeric<size_t, struct ValueIDTag, AK::DistinctNumericFeature::Comparison, AK::DistinctNumericFeature::Arithmetic, AK::DistinctNumericFeature::Increment>;
    using IP = DistinctNumeric<size_t, struct IPTag, AK::DistinctNumericFeature::Comparison>;

    struct Value {
        ValueID id;
        IP definition_index;
        Vector<IP> uses;
        IP last_use = 0;
    };

    struct ActiveReg {
        ValueID value_id;
        IP end;
        Dispatch::RegisterOrStack reg;
    };

    HashMap<ValueID, Value> values;
    Vector<ValueID> value_stack;
    ValueID next_value_id = 0;
    HashMap<IP, ValueID> instr_to_output_value;
    HashMap<IP, Vector<ValueID>> instr_to_input_values;
    HashMap<IP, Vector<ValueID>> instr_to_dependent_values;

    Vector<ValueID> forced_stack_values;

    Vector<ValueID> parent;      // parent[id] -> parent ValueID of id in the alias tree
    Vector<ValueID> rank;        // rank[id] -> rank of the tree rooted at id
    Vector<ValueID> final_roots; // final_roots[id] -> the final root parent of id

    auto ensure_id_space = [&](ValueID id) {
        if (id >= parent.size()) {
            size_t old_size = parent.size();
            parent.resize(id.value() + 1);
            rank.resize(id.value() + 1);
            final_roots.resize(id.value() + 1);
            for (size_t i = old_size; i <= id; ++i) {
                parent[i] = i;
                rank[i] = 0;
                final_roots[i] = i;
            }
        }
    };

    auto find_root = [&parent](this auto& self, ValueID x) -> ValueID {
        if (parent[x.value()] != x)
            parent[x.value()] = self(parent[x.value()]);
        return parent[x.value()];
    };

    auto union_alias = [&](ValueID a, ValueID b) {
        ensure_id_space(max(a, b));

        auto const root_a = find_root(a);
        auto const root_b = find_root(b);

        if (root_a == root_b)
            return;

        if (rank[root_a.value()] < rank[root_b.value()]) {
            parent[root_a.value()] = root_b;
        } else if (rank[root_a.value()] > rank[root_b.value()]) {
            parent[root_b.value()] = root_a;
        } else {
            parent[root_b.value()] = root_a;
            ++rank[root_a.value()];
        }
    };

    HashTable<ValueID> stack_forced_roots;

    Vector<Vector<ValueID>> live_at_instr;
    live_at_instr.resize(result.dispatches.size());

    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto& dispatch = result.dispatches[i];
        auto opcode = dispatch.instruction->opcode();
        size_t inputs = 0;
        size_t outputs = 0;
        Vector<ValueID> dependent_ids;

        bool variadic_or_unknown = false;
        auto const is_known_call = opcode == Instructions::synthetic_call_00 || opcode == Instructions::synthetic_call_01
            || opcode == Instructions::synthetic_call_10 || opcode == Instructions::synthetic_call_11
            || opcode == Instructions::synthetic_call_20 || opcode == Instructions::synthetic_call_21
            || opcode == Instructions::synthetic_call_30 || opcode == Instructions::synthetic_call_31;

        switch (opcode.value()) {
#define M(name, _, ins, outs)                    \
    case Instructions::name.value():             \
        if constexpr (ins == -1 || outs == -1) { \
            variadic_or_unknown = true;          \
        } else {                                 \
            inputs = ins;                        \
            outputs = outs;                      \
        }                                        \
        break;
            ENUMERATE_WASM_OPCODES(M)
#undef M
        }

        if (variadic_or_unknown) {
            for (auto val : value_stack) {
                auto& value = values.get(val).value();
                value.uses.append(i);
                value.last_use = max(value.last_use, i);
                dependent_ids.append(val);
                forced_stack_values.append(val);
                live_at_instr[i].append(val);
            }
            value_stack.clear_with_capacity();
        }

        Vector<ValueID> input_ids;
        if (!variadic_or_unknown && value_stack.size() < inputs) {
            size_t j = 0;
            for (; j < inputs && !value_stack.is_empty(); ++j) {
                auto input_value = value_stack.take_last();
                input_ids.append(input_value);
                dependent_ids.append(input_value);
                auto& value = values.get(input_value).value();
                value.uses.append(i);
                value.last_use = max(value.last_use, i);
            }

            for (; j < inputs; ++j) {
                auto val_id = next_value_id++;
                values.set(val_id, Value { val_id, i, {}, i });
                input_ids.append(val_id);
                forced_stack_values.append(val_id);
                ensure_id_space(val_id);
            }

            inputs = 0;
        }

        for (size_t j = 0; j < inputs; ++j) {
            auto input_value = value_stack.take_last();
            input_ids.append(input_value);
            dependent_ids.append(input_value);
            auto& value = values.get(input_value).value();
            value.uses.append(i);
            value.last_use = max(value.last_use, i);

            if (is_known_call)
                forced_stack_values.append(input_value);
        }
        instr_to_input_values.set(i, input_ids);
        instr_to_dependent_values.set(i, dependent_ids);

        ValueID output_id = NumericLimits<size_t>::max();
        for (size_t j = 0; j < outputs; ++j) {
            auto id = next_value_id++;
            values.set(id, Value { id, i, {}, i });
            value_stack.append(id);
            instr_to_output_value.set(i, id);
            output_id = id;
            ensure_id_space(id);

            if (is_known_call)
                forced_stack_values.append(id);
        }

        // Alias the output with the last input, if one exists.
        if (outputs > 0) {
            auto maybe_input_ids = instr_to_input_values.get(i);
            if (maybe_input_ids.has_value() && !maybe_input_ids->is_empty()) {
                auto last_input_id = maybe_input_ids->last();
                union_alias(output_id, last_input_id);

                auto alias_root = find_root(last_input_id);

                // If any *other* input is forced to alias the output, we have no choice but to place all three on the stack.
                for (size_t j = 0; j < maybe_input_ids->size() - 1; ++j) {
                    auto input_root = find_root((*maybe_input_ids)[j]);
                    if (input_root == alias_root) {
                        stack_forced_roots.set(alias_root);
                        break;
                    }
                }
            }
        }
    }

    forced_stack_values.extend(value_stack);

    for (size_t i = 0; i < final_roots.size(); ++i)
        final_roots[i] = find_root(i);

    // One more pass to ensure that all inputs and outputs of known calls are forced to the stack after aliases are resolved.
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto const opcode = result.dispatches[i].instruction->opcode();
        auto const is_known_call = opcode == Instructions::synthetic_call_00 || opcode == Instructions::synthetic_call_01
            || opcode == Instructions::synthetic_call_10 || opcode == Instructions::synthetic_call_11
            || opcode == Instructions::synthetic_call_20 || opcode == Instructions::synthetic_call_21
            || opcode == Instructions::synthetic_call_30 || opcode == Instructions::synthetic_call_31;

        if (is_known_call) {
            if (auto input_ids = instr_to_input_values.get(i); input_ids.has_value()) {
                for (auto input_id : *input_ids) {
                    if (input_id.value() < final_roots.size()) {
                        stack_forced_roots.set(final_roots[input_id.value()]);
                    }
                }
            }

            if (auto output_id = instr_to_output_value.get(i); output_id.has_value()) {
                if (output_id->value() < final_roots.size()) {
                    stack_forced_roots.set(final_roots[output_id->value()]);
                }
            }
        }
    }

    struct LiveInterval {
        ValueID value_id;
        IP start;
        IP end;
        bool forced_to_stack { false };
    };

    Vector<LiveInterval> intervals;
    intervals.ensure_capacity(values.size());

    for (auto const& [_, value] : values) {
        auto start = value.definition_index;
        auto end = max(start, value.last_use);
        intervals.append({ value.id, start, end });
    }

    for (auto id : forced_stack_values)
        stack_forced_roots.set(final_roots[id.value()]);
    for (auto& interval : intervals)
        interval.forced_to_stack = stack_forced_roots.contains(final_roots[interval.value_id.value()]);

    quick_sort(intervals, [](auto const& a, auto const& b) {
        return a.start < b.start;
    });

    HashMap<ValueID, Dispatch::RegisterOrStack> value_alloc;
    RedBlackTree<size_t, ActiveReg> active_by_end;

    auto expire_old_intervals = [&](IP current_start) {
        while (true) {
            auto it = active_by_end.find_smallest_not_below_iterator(current_start.value());
            if (it.is_end())
                break;
            active_by_end.remove(it.key());
        }
    };

    HashMap<ValueID, Vector<LiveInterval*>> alias_groups;
    for (auto& interval : intervals) {
        auto root = final_roots[interval.value_id.value()];
        alias_groups.ensure(root).append(&interval);
    }

    Array<Vector<LiveInterval const*>, Dispatch::CountRegisters> reg_intervals;
    reg_intervals.fill({});

    for (auto& [key, group] : alias_groups) {
        IP group_start = NumericLimits<size_t>::max();
        IP group_end = 0;
        auto group_forced_to_stack = false;

        for (auto* interval : group) {
            group_start = min(group_start, interval->start);
            group_end = max(group_end, interval->end);
            if (interval->forced_to_stack)
                group_forced_to_stack = true;
        }

        expire_old_intervals(group_start);

        Dispatch::RegisterOrStack reg = Dispatch::RegisterOrStack::Stack;
        if (!group_forced_to_stack) {
            Array<bool, Dispatch::CountRegisters> used_regs;
            used_regs.fill(false);

            for (auto const& active_entry : active_by_end) {
                if (active_entry.reg != Dispatch::RegisterOrStack::Stack)
                    used_regs[to_underlying(active_entry.reg)] = true;
            }

            for (u8 r = 0; r < Dispatch::CountRegisters; ++r) {
                if (used_regs[r]) // There's no hope of using this register, it was already used earlier.
                    continue;

                // We can assign to "live" registers, but only if we know there will be no overlap, or that they're aliasing values anyway.
                auto can_assign = true;
                for (auto* interval : group) {
                    auto interval_root = final_roots[interval->value_id.value()];

                    for (auto const* other_interval : reg_intervals[r]) {
                        if (interval_root == final_roots[other_interval->value_id.value()])
                            continue;
                        if (interval->end >= other_interval->start && other_interval->end >= interval->start) {
                            can_assign = false;
                            break;
                        }
                    }
                    if (!can_assign)
                        break;
                }

                if (can_assign) {
                    reg = static_cast<Dispatch::RegisterOrStack>(r);
                    active_by_end.insert(group_end.value(), { key, group_end, reg });

                    for (auto* interval : group)
                        reg_intervals[r].append(interval);
                    break;
                }
            }
        }

        for (auto* interval : group)
            value_alloc.set(interval->value_id, reg);
    }

    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto& dispatch = result.dispatches[i];
        auto input_ids = instr_to_input_values.get(i).value_or({});

        for (size_t j = 0; j < input_ids.size(); ++j) {
            auto reg = value_alloc.get(input_ids[j]).value_or(Dispatch::RegisterOrStack::Stack);
            dispatch.sources[j] = reg;
        }

        if (auto output_id = instr_to_output_value.get(i); output_id.has_value())
            dispatch.destination = value_alloc.get(*output_id).value_or(Dispatch::RegisterOrStack::Stack);
    }

    return result;
}

}
