/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Result.h>
#include <AK/SourceLocation.h>
#include <AK/TemporaryChange.h>
#include <AK/Try.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWasm/Printer/Printer.h>

namespace Wasm {

ErrorOr<void, ValidationError> Validator::validate(Module& module)
{
    // Pre-emptively make invalid. The module will be set to `Valid` at the end
    // of validation.
    module.set_validation_status(Module::ValidationStatus::Invalid, {});

    // Note: The spec performs this after populating the context, but there's no real reason to do so,
    //       as this has no dependency.
    HashTable<StringView> seen_export_names;
    for (auto& export_ : module.export_section().entries())
        if (seen_export_names.try_set(export_.name()).release_value_but_fixme_should_propagate_errors() != AK::HashSetResult::InsertedNewEntry)
            return Errors::duplicate_export_name(export_.name());

    m_context = {};

    m_context.types.extend(module.type_section().types());
    m_context.data_count = module.data_count_section().count();

    for (auto& import_ : module.import_section().imports()) {
        TRY(import_.description().visit(
            [&](TypeIndex const& index) -> ErrorOr<void, ValidationError> {
                if (m_context.types.size() > index.value())
                    m_context.functions.append(m_context.types[index.value()]);
                else
                    return Errors::invalid("TypeIndex"sv);
                m_context.imported_function_count++;
                return {};
            },
            [&](FunctionType const& type) -> ErrorOr<void, ValidationError> {
                m_context.functions.append(type);
                m_context.imported_function_count++;
                return {};
            },
            [&](TableType const& type) -> ErrorOr<void, ValidationError> {
                m_context.tables.append(type);
                return {};
            },
            [&](MemoryType const& type) -> ErrorOr<void, ValidationError> {
                m_context.memories.append(type);
                return {};
            },
            [&](GlobalType const& type) -> ErrorOr<void, ValidationError> {
                m_globals_without_internal_globals.append(type);
                m_context.globals.append(type);
                return {};
            }));
    }

    if (module.code_section().functions().size() != module.function_section().types().size())
        return Errors::invalid("FunctionSection"sv);

    m_context.functions.ensure_capacity(module.function_section().types().size() + m_context.functions.size());
    for (auto& index : module.function_section().types())
        if (m_context.types.size() > index.value())
            m_context.functions.append(m_context.types[index.value()]);
        else
            return Errors::invalid("TypeIndex"sv);

    m_context.tables.ensure_capacity(m_context.tables.size() + module.table_section().tables().size());
    for (auto& table : module.table_section().tables())
        m_context.tables.append(table.type());

    m_context.memories.ensure_capacity(m_context.memories.size() + module.memory_section().memories().size());
    for (auto& memory : module.memory_section().memories())
        m_context.memories.append(memory.type());

    m_context.globals.ensure_capacity(m_context.globals.size() + module.global_section().entries().size());
    for (auto& global : module.global_section().entries())
        m_context.globals.append(global.type());

    m_context.elements.ensure_capacity(module.element_section().segments().size());
    for (auto& segment : module.element_section().segments())
        m_context.elements.append(segment.type);

    m_context.datas.resize(module.data_section().data().size());

    // We need to build the set of declared functions to check that `ref.func` uses a specific set of predetermined functions, found in:
    // - Element initializer expressions
    // - Global initializer expressions
    // - Exports
    auto scan_expression_for_function_indices = [&](auto& expression) {
        for (auto& instruction : expression.instructions()) {
            if (instruction.opcode() == Instructions::ref_func) {
                auto index = instruction.arguments().template get<FunctionIndex>();
                m_context.references->tree.insert(index.value(), index);
            }
        }
    };
    for (auto& export_ : module.export_section().entries()) {
        if (!export_.description().has<FunctionIndex>())
            continue;
        auto index = export_.description().get<FunctionIndex>();
        m_context.references->tree.insert(index.value(), index);
    }
    for (auto& segment : module.element_section().segments()) {
        for (auto& expression : segment.init)
            scan_expression_for_function_indices(expression);
    }
    for (auto& segment : module.global_section().entries())
        scan_expression_for_function_indices(segment.expression());

    TRY(validate(module.import_section()));
    TRY(validate(module.export_section()));
    TRY(validate(module.start_section()));
    TRY(validate(module.data_section()));
    TRY(validate(module.element_section()));
    TRY(validate(module.global_section()));
    TRY(validate(module.memory_section()));
    TRY(validate(module.table_section()));
    TRY(validate(module.code_section()));

    module.set_validation_status(Module::ValidationStatus::Valid, {});
    return {};
}

ErrorOr<void, ValidationError> Validator::validate(ImportSection const& section)
{
    for (auto& import_ : section.imports())
        TRY(import_.description().visit([&](auto& entry) { return validate(entry); }));
    return {};
}

ErrorOr<void, ValidationError> Validator::validate(ExportSection const& section)
{
    for (auto& export_ : section.entries())
        TRY(export_.description().visit([&](auto& entry) { return validate(entry); }));
    return {};
}

ErrorOr<void, ValidationError> Validator::validate(StartSection const& section)
{
    if (!section.function().has_value())
        return {};
    TRY(validate(section.function()->index()));
    FunctionType const& type = m_context.functions[section.function()->index().value()];
    if (!type.parameters().is_empty() || !type.results().is_empty())
        return Errors::invalid("start function signature"sv);
    return {};
}

ErrorOr<void, ValidationError> Validator::validate(DataSection const& section)
{
    if (m_context.data_count.has_value() && section.data().size() != m_context.data_count)
        return Errors::invalid("data count does not match segment count"sv);
    for (auto& entry : section.data()) {
        TRY(entry.value().visit(
            [](DataSection::Data::Passive const&) { return ErrorOr<void, ValidationError> {}; },
            [&](DataSection::Data::Active const& active) -> ErrorOr<void, ValidationError> {
                TRY(validate(active.index));

                auto expression_result = TRY(validate(active.offset, { ValueType(ValueType::I32) }));

                if (!expression_result.is_constant)
                    return Errors::invalid("active data initializer"sv);

                if (expression_result.result_types.size() != 1 || !expression_result.result_types.first().is_of_kind(ValueType::I32))
                    return Errors::invalid("active data initializer type"sv, ValueType(ValueType::I32), expression_result.result_types);

                return {};
            }));
    }

    return {};
}

ErrorOr<void, ValidationError> Validator::validate(ElementSection const& section)
{
    for (auto& segment : section.segments()) {
        TRY(segment.mode.visit(
            [](ElementSection::Declarative const&) -> ErrorOr<void, ValidationError> { return {}; },
            [](ElementSection::Passive const&) -> ErrorOr<void, ValidationError> { return {}; },
            [&](ElementSection::Active const& active) -> ErrorOr<void, ValidationError> {
                TRY(validate(active.index));
                auto table = m_context.tables[active.index.value()];
                if (table.element_type() != segment.type)
                    return Errors::invalid("active element reference type"sv);
                auto expression_result = TRY(validate(active.expression, { ValueType(ValueType::I32) }));
                if (!expression_result.is_constant)
                    return Errors::invalid("active element initializer"sv);
                if (expression_result.result_types.size() != 1 || !expression_result.result_types.first().is_of_kind(ValueType::I32))
                    return Errors::invalid("active element initializer type"sv, ValueType(ValueType::I32), expression_result.result_types);
                return {};
            }));

        for (auto& expression : segment.init) {
            if (expression.instructions().is_empty())
                continue;
            auto result = TRY(validate(expression, { segment.type }));
            if (!result.is_constant)
                return Errors::invalid("element initializer"sv);
        }
    }
    return {};
}

ErrorOr<void, ValidationError> Validator::validate(GlobalSection const& section)
{
    TemporaryChange omit_internal_globals { m_context.globals, m_globals_without_internal_globals };

    for (auto& entry : section.entries()) {
        auto& type = entry.type();
        TRY(validate(type));
        auto expression_result = TRY(validate(entry.expression(), { type.type() }));
        if (!expression_result.is_constant)
            return Errors::invalid("global variable initializer"sv);
        if (expression_result.result_types.size() != 1 || !expression_result.result_types.first().is_of_kind(type.type().kind()))
            return Errors::invalid("global variable initializer type"sv, ValueType(ValueType::I32), expression_result.result_types);
    }

    return {};
}

ErrorOr<void, ValidationError> Validator::validate(MemorySection const& section)
{
    for (auto& entry : section.memories())
        TRY(validate(entry.type()));
    return {};
}

ErrorOr<void, ValidationError> Validator::validate(TableSection const& section)
{
    for (auto& entry : section.tables())
        TRY(validate(entry.type()));
    return {};
}

ErrorOr<void, ValidationError> Validator::validate(CodeSection const& section)
{
    size_t index = m_context.imported_function_count;
    for (auto& entry : section.functions()) {
        auto function_index = index++;
        TRY(validate(FunctionIndex { function_index }));
        auto& function_type = m_context.functions[function_index];
        auto& function = entry.func();

        auto function_validator = fork();
        function_validator.m_context.locals = {};
        function_validator.m_context.locals.extend(function_type.parameters());
        for (auto& local : function.locals()) {
            for (size_t i = 0; i < local.n(); ++i)
                function_validator.m_context.locals.append(local.type());
        }

        function_validator.m_frames.empend(function_type, FrameKind::Function, (size_t)0);

        auto results = TRY(function_validator.validate(function.body(), function_type.results()));
        if (results.result_types.size() != function_type.results().size())
            return Errors::invalid("function result"sv, function_type.results(), results.result_types);
    }

    return {};
}

ErrorOr<void, ValidationError> Validator::validate(TableType const& type)
{
    return validate(type.limits(), (1ull << 32) - 1);
}

ErrorOr<void, ValidationError> Validator::validate(MemoryType const& type)
{
    return validate(type.limits(), 1 << 16);
}

ErrorOr<FunctionType, ValidationError> Validator::validate(BlockType const& type)
{
    if (type.kind() == BlockType::Index) {
        TRY(validate(type.type_index()));
        return m_context.types[type.type_index().value()];
    }

    if (type.kind() == BlockType::Type) {
        FunctionType function_type { {}, { type.value_type() } };
        TRY(validate(function_type));
        return function_type;
    }

    if (type.kind() == BlockType::Empty)
        return FunctionType { {}, {} };

    return Errors::invalid("BlockType"sv);
}

ErrorOr<void, ValidationError> Validator::validate(Limits const& limits, u64 bound)
{
    auto check_bound = [bound](auto value) {
        return static_cast<u64>(value) <= bound;
    };

    if (!check_bound(limits.min()))
        return Errors::out_of_bounds("limit minimum"sv, limits.min(), 0, bound);

    if (limits.max().has_value() && (limits.max().value() < limits.min() || !check_bound(*limits.max()))) {
        return Errors::out_of_bounds("limit maximum"sv, limits.max().value(), limits.min(), bound);
    }

    return {};
}

template<u64 opcode>
ErrorOr<void, ValidationError> Validator::validate_instruction(Instruction const& instruction, Stack&, bool&)
{
    return Errors::invalid(ByteString::formatted("instruction opcode ({:#x}) (missing validation!)", instruction.opcode().value()));
}

#define VALIDATE_INSTRUCTION(name) \
    template<>                     \
    ErrorOr<void, ValidationError> Validator::validate_instruction<Instructions::name.value()>([[maybe_unused]] Instruction const& instruction, [[maybe_unused]] Stack& stack, [[maybe_unused]] bool& is_constant)

// https://webassembly.github.io/spec/core/bikeshed/#-tmathsfhrefsyntax-instr-numericmathsfconstc
VALIDATE_INSTRUCTION(i32_const)
{
    is_constant = true;
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_const)
{
    is_constant = true;
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(f32_const)
{
    is_constant = true;
    stack.append(ValueType(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f64_const)
{
    is_constant = true;
    stack.append(ValueType(ValueType::F64));
    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#-tmathsfhrefsyntax-unopmathitunop
VALIDATE_INSTRUCTION(i32_clz)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_ctz)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_popcnt)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_clz)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_ctz)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_popcnt)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(f32_abs)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_neg)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_sqrt)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_ceil)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_floor)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_trunc)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_nearest)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f64_abs)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_neg)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_sqrt)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_ceil)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_floor)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_trunc)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_nearest)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(i32_extend16_s)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_extend8_s)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_extend32_s)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_extend16_s)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_extend8_s)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::I64));
    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#-tmathsfhrefsyntax-binopmathitbinop
VALIDATE_INSTRUCTION(i32_add)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_sub)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_mul)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_divs)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_divu)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_rems)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_remu)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_and)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_or)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_xor)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_shl)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_shrs)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_shru)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_rotl)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_rotr)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_add)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_sub)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_mul)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_divs)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_divu)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_rems)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_remu)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_and)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_or)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_xor)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_shl)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_shrs)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_shru)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_rotl)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_rotr)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(f32_add)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_sub)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_mul)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_div)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_min)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_max)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_copysign)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f64_add)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_sub)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_mul)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_div)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_min)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_max)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_copysign)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::F64));
    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#-tmathsfhrefsyntax-testopmathittestop
VALIDATE_INSTRUCTION(i32_eqz)
{
    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_eqz)
{
    TRY((stack.take<ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#-tmathsfhrefsyntax-relopmathitrelop
VALIDATE_INSTRUCTION(i32_eq)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_ne)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_lts)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_ltu)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_gts)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_gtu)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_les)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_leu)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_ges)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_geu)
{
    TRY((stack.take<ValueType::I32, ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_eq)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_ne)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_lts)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_ltu)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_gts)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_gtu)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_les)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_leu)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_ges)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_geu)
{
    TRY((stack.take<ValueType::I64, ValueType::I64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f32_eq)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f32_ne)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f32_lt)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f32_le)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f32_gt)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f32_ge)
{
    TRY((stack.take<ValueType::F32, ValueType::F32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f64_eq)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f64_ne)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f64_lt)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f64_le)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f64_gt)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(f64_ge)
{
    TRY((stack.take<ValueType::F64, ValueType::F64>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#-t_2mathsfhrefsyntax-cvtopmathitcvtopmathsf_t_1mathsf_hrefsyntax-sxmathitsx
VALIDATE_INSTRUCTION(i32_wrap_i64)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_extend_si32)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_extend_ui32)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i32_trunc_sf32)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_trunc_uf32)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_trunc_sf64)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_trunc_uf64)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_trunc_sf32)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_trunc_uf32)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_trunc_sf64)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_trunc_uf64)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i32_trunc_sat_f32_s)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_trunc_sat_f32_u)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_trunc_sat_f64_s)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_trunc_sat_f64_u)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_trunc_sat_f32_s)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_trunc_sat_f32_u)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_trunc_sat_f64_s)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_trunc_sat_f64_u)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(f32_convert_si32)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_convert_ui32)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_convert_si64)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f32_convert_ui64)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f64_convert_si32)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_convert_ui32)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_convert_si64)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f64_convert_ui64)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f32_demote_f64)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f64_promote_f32)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(f32_reinterpret_i32)
{
    TRY(stack.take_and_put<ValueType::I32>(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f64_reinterpret_i64)
{
    TRY(stack.take_and_put<ValueType::I64>(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(i32_reinterpret_f32)
{
    TRY(stack.take_and_put<ValueType::F32>(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_reinterpret_f64)
{
    TRY(stack.take_and_put<ValueType::F64>(ValueType::I64));
    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#reference-instructions%E2%91%A2

VALIDATE_INSTRUCTION(ref_null)
{
    is_constant = true;
    stack.append(instruction.arguments().get<ValueType>());
    return {};
}

VALIDATE_INSTRUCTION(ref_is_null)
{
    if (stack.is_empty() || !stack.last().is_reference())
        return Errors::invalid_stack_state(stack, Tuple { "reference" });

    TRY(stack.take_last());
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(ref_func)
{
    auto index = instruction.arguments().get<FunctionIndex>();
    TRY(validate(index));

    if (m_context.references->tree.find(index.value()) == nullptr)
        return Errors::invalid("function reference"sv);

    is_constant = true;
    stack.append(ValueType(ValueType::FunctionReference));
    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#parametric-instructions%E2%91%A2
VALIDATE_INSTRUCTION(drop)
{
    TRY(stack.take_last());
    return {};
}

VALIDATE_INSTRUCTION(select)
{
    TRY(stack.take<ValueType::I32>());
    auto arg0_type = TRY(stack.take_last());
    auto arg1_type = TRY(stack.take_last());

    if (arg0_type != arg1_type || arg0_type.concrete_type.is_reference() || arg1_type.concrete_type.is_reference())
        return Errors::invalid("select argument types"sv, Vector { arg0_type, arg0_type }, Vector { arg0_type, arg1_type });

    stack.append(arg0_type.is_known ? arg0_type : arg1_type);

    return {};
}

VALIDATE_INSTRUCTION(select_typed)
{
    auto& required_types = instruction.arguments().get<Vector<ValueType>>();
    if (required_types.size() != 1)
        return Errors::invalid("select types"sv, "exactly one type"sv, required_types);

    TRY(stack.take<ValueType::I32>());
    auto arg0_type = TRY(stack.take_last());
    auto arg1_type = TRY(stack.take_last());

    if (arg0_type != arg1_type || arg0_type != required_types.first())
        return Errors::invalid("select argument types"sv, Vector { required_types.first(), required_types.first() }, Vector { arg0_type, arg1_type });

    stack.append(arg0_type.is_known ? arg0_type : arg1_type);

    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#variable-instructions%E2%91%A2
VALIDATE_INSTRUCTION(local_get)
{
    auto index = instruction.arguments().get<LocalIndex>();
    TRY(validate(index));

    stack.append(m_context.locals[index.value()]);
    return {};
}

VALIDATE_INSTRUCTION(local_set)
{
    auto index = instruction.arguments().get<LocalIndex>();
    TRY(validate(index));

    auto& value_type = m_context.locals[index.value()];
    TRY(stack.take(value_type));

    return {};
}

VALIDATE_INSTRUCTION(local_tee)
{
    auto index = instruction.arguments().get<LocalIndex>();
    TRY(validate(index));

    auto& value_type = m_context.locals[index.value()];
    TRY(stack.take(value_type));
    stack.append(value_type);

    return {};
}

VALIDATE_INSTRUCTION(global_get)
{
    auto index = instruction.arguments().get<GlobalIndex>();
    TRY(validate(index));

    auto& global = m_context.globals[index.value()];

    is_constant = !global.is_mutable();
    stack.append(global.type());
    return {};
}

VALIDATE_INSTRUCTION(global_set)
{
    auto index = instruction.arguments().get<GlobalIndex>();
    TRY(validate(index));

    auto& global = m_context.globals[index.value()];

    if (!global.is_mutable())
        return Errors::invalid("global variable for global.set"sv);

    TRY(stack.take(global.type()));

    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#table-instructions%E2%91%A2
VALIDATE_INSTRUCTION(table_get)
{
    auto index = instruction.arguments().get<TableIndex>();
    TRY(validate(index));

    auto& table = m_context.tables[index.value()];
    TRY(stack.take<ValueType::I32>());
    stack.append(table.element_type());
    return {};
}

VALIDATE_INSTRUCTION(table_set)
{
    auto index = instruction.arguments().get<TableIndex>();
    TRY(validate(index));

    auto& table = m_context.tables[index.value()];
    TRY(stack.take(table.element_type()));

    TRY(stack.take<ValueType::I32>());

    return {};
}

VALIDATE_INSTRUCTION(table_size)
{
    auto index = instruction.arguments().get<TableIndex>();
    TRY(validate(index));

    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(table_grow)
{
    auto index = instruction.arguments().get<TableIndex>();
    TRY(validate(index));

    auto& table = m_context.tables[index.value()];

    TRY(stack.take<ValueType::I32>());
    TRY(stack.take(table.element_type()));

    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(table_fill)
{
    auto index = instruction.arguments().get<TableIndex>();
    TRY(validate(index));

    auto& table = m_context.tables[index.value()];

    TRY(stack.take<ValueType::I32>());
    TRY(stack.take(table.element_type()));
    TRY(stack.take<ValueType::I32>());

    return {};
}

VALIDATE_INSTRUCTION(table_copy)
{
    auto& args = instruction.arguments().get<Instruction::TableTableArgs>();

    TRY(validate(args.lhs));
    TRY(validate(args.rhs));

    auto& lhs_table = m_context.tables[args.lhs.value()];
    auto& rhs_table = m_context.tables[args.rhs.value()];

    if (lhs_table.element_type() != rhs_table.element_type())
        return Errors::non_conforming_types("table.copy"sv, lhs_table.element_type(), rhs_table.element_type());

    if (!lhs_table.element_type().is_reference())
        return Errors::invalid("table.copy element type"sv, "a reference type"sv, lhs_table.element_type());

    TRY((stack.take<ValueType::I32, ValueType::I32, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(table_init)
{
    auto& args = instruction.arguments().get<Instruction::TableElementArgs>();

    TRY(validate(args.table_index));
    TRY(validate(args.element_index));

    auto& table = m_context.tables[args.table_index.value()];
    auto& element_type = m_context.elements[args.element_index.value()];

    if (table.element_type() != element_type)
        return Errors::non_conforming_types("table.init"sv, table.element_type(), element_type);

    TRY((stack.take<ValueType::I32, ValueType::I32, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(elem_drop)
{
    auto index = instruction.arguments().get<ElementIndex>();
    TRY(validate(index));

    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#memory-instructions%E2%91%A2
VALIDATE_INSTRUCTION(i32_load)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(i32))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(i32));

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_load)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(i64))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(i64));

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(f32_load)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(float))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(float));

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::F32));
    return {};
}

VALIDATE_INSTRUCTION(f64_load)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(double))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(double));

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::F64));
    return {};
}

VALIDATE_INSTRUCTION(i32_load16_s)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 16 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 16 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_load16_u)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 16 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 16 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_load8_s)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 8 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 8 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i32_load8_u)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 8 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 8 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(i64_load32_s)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 32 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 32 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_load32_u)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 32 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 32 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_load16_s)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 16 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 16 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_load16_u)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 16 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 16 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_load8_s)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 8 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 8 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i64_load8_u)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 8 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 8 / 8);

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I64));
    return {};
}

VALIDATE_INSTRUCTION(i32_store)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(i32))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(i32));

    TRY((stack.take<ValueType::I32, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(i64_store)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(i64))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(i64));

    TRY((stack.take<ValueType::I64, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(f32_store)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(float))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(float));

    TRY((stack.take<ValueType::F32, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(f64_store)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(double))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(double));

    TRY((stack.take<ValueType::F64, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(i32_store16)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 16 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 16 / 8);

    TRY((stack.take<ValueType::I32, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(i32_store8)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 8 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 8 / 8);

    TRY((stack.take<ValueType::I32, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(i64_store32)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 32 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 32 / 8);

    TRY((stack.take<ValueType::I64, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(i64_store16)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 16 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 16 / 8);

    TRY((stack.take<ValueType::I64, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(i64_store8)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > 8 / 8)
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, 8 / 8);

    TRY((stack.take<ValueType::I64, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(memory_size)
{
    TRY(validate(instruction.arguments().get<Instruction::MemoryIndexArgument>().memory_index));

    stack.append(ValueType(ValueType::I32));
    return {};
}

VALIDATE_INSTRUCTION(memory_grow)
{
    TRY(validate(instruction.arguments().get<Instruction::MemoryIndexArgument>().memory_index));

    TRY((stack.take<ValueType::I32>()));
    stack.append(ValueType(ValueType::I32));

    return {};
}

VALIDATE_INSTRUCTION(memory_fill)
{
    TRY(validate(instruction.arguments().get<Instruction::MemoryIndexArgument>().memory_index));

    TRY((stack.take<ValueType::I32, ValueType::I32, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(memory_copy)
{
    auto& args = instruction.arguments().get<Instruction::MemoryCopyArgs>();
    TRY(validate(args.src_index));
    TRY(validate(args.dst_index));

    TRY((stack.take<ValueType::I32, ValueType::I32, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(memory_init)
{
    if (!m_context.data_count.has_value())
        return Errors::invalid("memory.init, requires data count section"sv);

    auto& args = instruction.arguments().get<Instruction::MemoryInitArgs>();

    TRY(validate(args.memory_index));
    TRY(validate(args.data_index));

    TRY((stack.take<ValueType::I32, ValueType::I32, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(data_drop)
{
    if (!m_context.data_count.has_value())
        return Errors::invalid("data.drop, requires data count section"sv);

    auto index = instruction.arguments().get<DataIndex>();
    TRY(validate(index));

    return {};
}

// https://webassembly.github.io/spec/core/bikeshed/#control-instructions%E2%91%A2
VALIDATE_INSTRUCTION(nop)
{
    return {};
}

VALIDATE_INSTRUCTION(unreachable)
{
    // https://webassembly.github.io/spec/core/bikeshed/#polymorphism
    m_frames.last().unreachable = true;
    stack.resize(m_frames.last().initial_size);

    return {};
}

// Note: This is responsible for _all_ structured instructions, and is *not* from the spec.
VALIDATE_INSTRUCTION(structured_end)
{
    if (m_frames.is_empty())
        return Errors::invalid("usage of structured end"sv);

    auto& last_frame = m_frames.last();

    // If this is true, then the `if` had no else. In that case, validate that the
    // empty else block produces the correct type.
    if (last_frame.kind == FrameKind::If) {
        bool is_constant = false;
        TRY(validate(Instruction(Instructions::structured_else), stack, is_constant));
    }

    auto& results = last_frame.type.results();
    for (size_t i = 1; i <= results.size(); ++i)
        TRY(stack.take(results[results.size() - i]));

    if (stack.size() != last_frame.initial_size)
        return Errors::stack_height_mismatch(stack, last_frame.initial_size);

    for (auto& result : results)
        stack.append(result);
    m_frames.take_last();

    return {};
}

// Note: This is *not* from the spec.
VALIDATE_INSTRUCTION(structured_else)
{
    if (m_frames.is_empty())
        return Errors::invalid("usage of structured else"sv);

    if (m_frames.last().kind != FrameKind::If)
        return Errors::invalid("usage of structured else"sv);

    auto& frame = m_frames.last();
    auto& block_type = frame.type;
    auto& results = block_type.results();

    for (size_t i = 1; i <= results.size(); ++i)
        TRY(stack.take(results[results.size() - i]));

    if (stack.size() != frame.initial_size)
        return Errors::stack_height_mismatch(stack, frame.initial_size);

    frame.kind = FrameKind::Else;
    frame.unreachable = false;
    for (auto& parameter : block_type.parameters())
        stack.append(parameter);

    return {};
}

VALIDATE_INSTRUCTION(block)
{
    auto& args = instruction.arguments().get<Instruction::StructuredInstructionArgs>();
    auto block_type = TRY(validate(args.block_type));

    auto& parameters = block_type.parameters();
    for (size_t i = 1; i <= parameters.size(); ++i)
        TRY(stack.take(parameters[parameters.size() - i]));

    m_frames.empend(block_type, FrameKind::Block, stack.size());
    for (auto& parameter : parameters)
        stack.append(parameter);

    return {};
}

VALIDATE_INSTRUCTION(loop)
{
    auto& args = instruction.arguments().get<Instruction::StructuredInstructionArgs>();
    auto block_type = TRY(validate(args.block_type));

    auto& parameters = block_type.parameters();
    for (size_t i = 1; i <= parameters.size(); ++i)
        TRY(stack.take(parameters[parameters.size() - i]));

    m_frames.empend(block_type, FrameKind::Loop, stack.size());
    for (auto& parameter : parameters)
        stack.append(parameter);

    return {};
}

VALIDATE_INSTRUCTION(if_)
{
    auto& args = instruction.arguments().get<Instruction::StructuredInstructionArgs>();
    auto block_type = TRY(validate(args.block_type));

    TRY(stack.take<ValueType::I32>());

    auto stack_snapshot = stack;

    auto& parameters = block_type.parameters();
    for (size_t i = 1; i <= parameters.size(); ++i)
        TRY(stack.take(parameters[parameters.size() - i]));

    m_frames.empend(block_type, FrameKind::If, stack.size());
    for (auto& parameter : parameters)
        stack.append(parameter);

    return {};
}

VALIDATE_INSTRUCTION(br)
{
    auto label = instruction.arguments().get<LabelIndex>();
    TRY(validate(label));

    auto& type = m_frames[(m_frames.size() - 1) - label.value()].labels();
    for (size_t i = 1; i <= type.size(); ++i)
        TRY(stack.take(type[type.size() - i]));

    m_frames.last().unreachable = true;
    stack.resize(m_frames.last().initial_size);
    return {};
}

VALIDATE_INSTRUCTION(br_if)
{
    auto label = instruction.arguments().get<LabelIndex>();
    TRY(validate(label));

    TRY(stack.take<ValueType::I32>());

    auto& type = m_frames[(m_frames.size() - 1) - label.value()].labels();

    Vector<StackEntry> entries;
    entries.ensure_capacity(type.size());

    for (size_t i = 0; i < type.size(); ++i) {
        auto& entry = type[type.size() - i - 1];
        TRY(stack.take(entry));
        entries.append(entry);
    }

    for (size_t i = 0; i < entries.size(); ++i)
        stack.append(entries[entries.size() - i - 1]);

    return {};
}

VALIDATE_INSTRUCTION(br_table)
{
    auto& args = instruction.arguments().get<Instruction::TableBranchArgs>();
    TRY(validate(args.default_));

    for (auto& label : args.labels)
        TRY(validate(label));

    TRY(stack.take<ValueType::I32>());

    auto& default_types = m_frames[(m_frames.size() - 1) - args.default_.value()].labels();
    auto arity = default_types.size();

    for (auto& label : args.labels) {
        auto& label_types = m_frames[(m_frames.size() - 1) - label.value()].labels();
        if (label_types.size() != arity)
            return Errors::invalid("br_table label arity mismatch"sv);
        Vector<StackEntry> popped {};
        for (size_t i = 0; i < arity; ++i) {
            auto stack_entry = TRY(stack.take(label_types[label_types.size() - i - 1]));
            popped.append(stack_entry);
        }
        for (auto popped_type : popped.in_reverse())
            stack.append(popped_type);
    }

    for (size_t i = 0; i < arity; ++i) {
        auto expected = default_types[default_types.size() - i - 1];
        TRY((stack.take(expected)));
    }

    m_frames.last().unreachable = true;
    stack.resize(m_frames.last().initial_size);

    return {};
}

VALIDATE_INSTRUCTION(return_)
{
    auto& return_types = m_frames.first().type.results();
    for (size_t i = 0; i < return_types.size(); ++i)
        TRY((stack.take(return_types[return_types.size() - i - 1])));

    m_frames.last().unreachable = true;
    stack.resize(m_frames.last().initial_size);

    return {};
}

VALIDATE_INSTRUCTION(call)
{
    auto index = instruction.arguments().get<FunctionIndex>();
    TRY(validate(index));

    auto& function_type = m_context.functions[index.value()];
    for (size_t i = 0; i < function_type.parameters().size(); ++i)
        TRY(stack.take(function_type.parameters()[function_type.parameters().size() - i - 1]));

    for (auto& type : function_type.results())
        stack.append(type);

    return {};
}

VALIDATE_INSTRUCTION(call_indirect)
{
    auto& args = instruction.arguments().get<Instruction::IndirectCallArgs>();
    TRY(validate(args.table));
    TRY(validate(args.type));

    auto& table = m_context.tables[args.table.value()];
    if (table.element_type().kind() != ValueType::FunctionReference)
        return Errors::invalid("table element type for call.indirect"sv, "a function reference"sv, table.element_type());

    auto& type = m_context.types[args.type.value()];

    TRY(stack.take<ValueType::I32>());

    for (size_t i = 0; i < type.parameters().size(); ++i)
        TRY(stack.take(type.parameters()[type.parameters().size() - i - 1]));

    for (auto& type : type.results())
        stack.append(type);

    return {};
}

VALIDATE_INSTRUCTION(v128_load)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(u128))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(u128));

    TRY((stack.take_and_put<ValueType::I32>(ValueType::V128)));

    return {};
}

VALIDATE_INSTRUCTION(v128_load8x8_s)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 8;
    constexpr auto M = 8;
    constexpr auto max_alignment = N * M / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load8x8_u)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 8;
    constexpr auto M = 8;
    constexpr auto max_alignment = N * M / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load16x4_s)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 16;
    constexpr auto M = 4;
    constexpr auto max_alignment = N * M / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load16x4_u)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 16;
    constexpr auto M = 4;
    constexpr auto max_alignment = N * M / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load32x2_s)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 32;
    constexpr auto M = 2;
    constexpr auto max_alignment = N * M / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load32x2_u)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 32;
    constexpr auto M = 2;
    constexpr auto max_alignment = N * M / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load8_splat)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 8;
    constexpr auto max_alignment = N / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load16_splat)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 16;
    constexpr auto max_alignment = N / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load32_splat)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 32;
    constexpr auto max_alignment = N / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load64_splat)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 64;
    constexpr auto max_alignment = N / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_store)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();

    TRY(validate(arg.memory_index));

    if ((1ull << arg.align) > sizeof(u128))
        return Errors::out_of_bounds("memory op alignment"sv, 1ull << arg.align, 0, sizeof(u128));

    TRY((stack.take<ValueType::V128, ValueType::I32>()));

    return {};
}

VALIDATE_INSTRUCTION(v128_const)
{
    is_constant = true;
    stack.append(ValueType(ValueType::V128));
    return {};
}

VALIDATE_INSTRUCTION(i8x16_shuffle)
{
    auto const& arg = instruction.arguments().get<Instruction::ShuffleArgument>();
    for (auto lane : arg.lanes) {
        if (lane >= 32)
            return Errors::out_of_bounds("shuffle lane"sv, lane, 0, 32);
    }
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_swizzle)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

enum class Shape {
    I8x16,
    I16x8,
    I32x4,
    I64x2,
    F32x4,
    F64x2,
};

template<Shape shape>
static constexpr Wasm::ValueType::Kind unpacked()
{
    switch (shape) {
    case Shape::I8x16:
    case Shape::I16x8:
    case Shape::I32x4:
        return Wasm::ValueType::I32;
    case Shape::I64x2:
        return Wasm::ValueType::I64;
    case Shape::F32x4:
        return Wasm::ValueType::F32;
    case Shape::F64x2:
        return Wasm::ValueType::F64;
    }
}

template<Shape shape>
static constexpr size_t dimensions()
{
    switch (shape) {
    case Shape::I8x16:
        return 16;
    case Shape::I16x8:
        return 8;
    case Shape::I32x4:
        return 4;
    case Shape::I64x2:
        return 2;
    case Shape::F32x4:
        return 4;
    case Shape::F64x2:
        return 2;
    }
}

VALIDATE_INSTRUCTION(i8x16_splat)
{
    constexpr auto unpacked_shape = unpacked<Shape::I8x16>();
    return stack.take_and_put<unpacked_shape>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_splat)
{
    constexpr auto unpacked_shape = unpacked<Shape::I16x8>();
    return stack.take_and_put<unpacked_shape>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_splat)
{
    constexpr auto unpacked_shape = unpacked<Shape::I32x4>();
    return stack.take_and_put<unpacked_shape>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_splat)
{
    constexpr auto unpacked_shape = unpacked<Shape::I64x2>();
    return stack.take_and_put<unpacked_shape>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_splat)
{
    constexpr auto unpacked_shape = unpacked<Shape::F32x4>();
    return stack.take_and_put<unpacked_shape>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_splat)
{
    constexpr auto unpacked_shape = unpacked<Shape::F64x2>();
    return stack.take_and_put<unpacked_shape>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_extract_lane_s)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I8x16;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<ValueType::V128>(unpacked<shape>());
}

VALIDATE_INSTRUCTION(i8x16_extract_lane_u)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I8x16;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<ValueType::V128>(unpacked<shape>());
}

VALIDATE_INSTRUCTION(i8x16_replace_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I8x16;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<unpacked<shape>(), ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extract_lane_s)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I16x8;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<ValueType::V128>(unpacked<shape>());
}

VALIDATE_INSTRUCTION(i16x8_extract_lane_u)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I16x8;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<ValueType::V128>(unpacked<shape>());
}

VALIDATE_INSTRUCTION(i16x8_replace_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I16x8;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<unpacked<shape>(), ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extract_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I32x4;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<ValueType::V128>(unpacked<shape>());
}

VALIDATE_INSTRUCTION(i32x4_replace_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I32x4;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<unpacked<shape>(), ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_extract_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I64x2;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<ValueType::V128>(unpacked<shape>());
}

VALIDATE_INSTRUCTION(i64x2_replace_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::I64x2;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<unpacked<shape>(), ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_extract_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::F32x4;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<ValueType::V128>(unpacked<shape>());
}

VALIDATE_INSTRUCTION(f32x4_replace_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::F32x4;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<unpacked<shape>(), ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_extract_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::F64x2;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<ValueType::V128>(unpacked<shape>());
}

VALIDATE_INSTRUCTION(f64x2_replace_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::LaneIndex>();
    constexpr auto shape = Shape::F64x2;
    constexpr auto max_lane = dimensions<shape>();

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("extract lane"sv, arg.lane, 0, max_lane);
    return stack.take_and_put<unpacked<shape>(), ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_eq)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_ne)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_lt_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_lt_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_gt_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_gt_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_le_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_le_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_ge_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_ge_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_eq)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_ne)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_lt_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_lt_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_gt_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_gt_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_le_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_le_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_ge_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_ge_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_eq)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_ne)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_lt_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_lt_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_gt_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_gt_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_le_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_le_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_ge_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_ge_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_eq)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_ne)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_lt)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_gt)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_le)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_ge)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_eq)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_ne)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_lt)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_gt)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_le)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_ge)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_not)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_and)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_andnot)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_or)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_xor)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_bitselect)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_any_true)
{
    return stack.take_and_put<ValueType::V128>(ValueType::I32);
}

VALIDATE_INSTRUCTION(v128_load8_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    constexpr auto N = 8;
    constexpr auto max_lane = 128 / N;
    constexpr auto max_alignment = N / 8;

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("lane index"sv, arg.lane, 0u, max_lane);

    TRY(validate(arg.memory.memory_index));

    if ((1 << arg.memory.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.memory.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::V128, ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load16_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    constexpr auto N = 16;
    constexpr auto max_lane = 128 / N;
    constexpr auto max_alignment = N / 8;

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("lane index"sv, arg.lane, 0u, max_lane);

    TRY(validate(arg.memory.memory_index));

    if ((1 << arg.memory.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.memory.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::V128, ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load32_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    constexpr auto N = 32;
    constexpr auto max_lane = 128 / N;
    constexpr auto max_alignment = N / 8;

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("lane index"sv, arg.lane, 0u, max_lane);

    TRY(validate(arg.memory.memory_index));

    if ((1 << arg.memory.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.memory.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::V128, ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load64_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    constexpr auto N = 64;
    constexpr auto max_lane = 128 / N;
    constexpr auto max_alignment = N / 8;

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("lane index"sv, arg.lane, 0u, max_lane);

    TRY(validate(arg.memory.memory_index));

    if ((1 << arg.memory.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.memory.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::V128, ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_store8_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    constexpr auto N = 8;
    constexpr auto max_lane = 128 / N;
    constexpr auto max_alignment = N / 8;

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("lane index"sv, arg.lane, 0u, max_lane);

    TRY(validate(arg.memory.memory_index));

    if ((1 << arg.memory.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.memory.align, 0u, max_alignment);

    return stack.take<ValueType::V128, ValueType::I32>();
}

VALIDATE_INSTRUCTION(v128_store16_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    constexpr auto N = 16;
    constexpr auto max_lane = 128 / N;
    constexpr auto max_alignment = N / 8;

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("lane index"sv, arg.lane, 0u, max_lane);

    TRY(validate(arg.memory.memory_index));

    if ((1 << arg.memory.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.memory.align, 0u, max_alignment);

    return stack.take<ValueType::V128, ValueType::I32>();
}

VALIDATE_INSTRUCTION(v128_store32_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    constexpr auto N = 32;
    constexpr auto max_lane = 128 / N;
    constexpr auto max_alignment = N / 8;

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("lane index"sv, arg.lane, 0u, max_lane);

    TRY(validate(arg.memory.memory_index));

    if ((1 << arg.memory.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.memory.align, 0u, max_alignment);

    return stack.take<ValueType::V128, ValueType::I32>();
}

VALIDATE_INSTRUCTION(v128_store64_lane)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    constexpr auto N = 64;
    constexpr auto max_lane = 128 / N;
    constexpr auto max_alignment = N / 8;

    if (arg.lane >= max_lane)
        return Errors::out_of_bounds("lane index"sv, arg.lane, 0u, max_lane);

    TRY(validate(arg.memory.memory_index));

    if ((1 << arg.memory.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.memory.align, 0u, max_alignment);

    return stack.take<ValueType::V128, ValueType::I32>();
}

VALIDATE_INSTRUCTION(v128_load32_zero)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 32;
    constexpr auto max_alignment = N / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(v128_load64_zero)
{
    auto const& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    constexpr auto N = 64;
    constexpr auto max_alignment = N / 8;

    TRY(validate(arg.memory_index));

    if ((1 << arg.align) > max_alignment)
        return Errors::out_of_bounds("memory op alignment"sv, 1 << arg.align, 0u, max_alignment);

    return stack.take_and_put<ValueType::I32>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_demote_f64x2_zero)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_promote_low_f32x4)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_abs)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_neg)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_popcnt)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_all_true)
{
    return stack.take_and_put<ValueType::V128>(ValueType::I32);
}

VALIDATE_INSTRUCTION(i8x16_bitmask)
{
    return stack.take_and_put<ValueType::V128>(ValueType::I32);
}

VALIDATE_INSTRUCTION(i8x16_narrow_i16x8_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_narrow_i16x8_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_ceil)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_floor)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_trunc)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_nearest)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_shl)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_shr_s)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_shr_u)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_add)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_add_sat_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_add_sat_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_sub)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_sub_sat_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_sub_sat_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_ceil)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_floor)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_min_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_min_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_max_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_max_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_trunc)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i8x16_avgr_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extadd_pairwise_i8x16_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extadd_pairwise_i8x16_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extadd_pairwise_i16x8_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extadd_pairwise_i16x8_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_abs)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_neg)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_q15mulr_sat_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_all_true)
{
    return stack.take_and_put<ValueType::V128>(ValueType::I32);
}

VALIDATE_INSTRUCTION(i16x8_bitmask)
{
    return stack.take_and_put<ValueType::V128>(ValueType::I32);
}

VALIDATE_INSTRUCTION(i16x8_narrow_i32x4_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_narrow_i32x4_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extend_low_i8x16_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extend_high_i8x16_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extend_low_i8x16_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extend_high_i8x16_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_shl)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_shr_s)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_shr_u)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_add)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_add_sat_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_add_sat_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_sub)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_sub_sat_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_sub_sat_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_nearest)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_mul)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_min_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_min_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_max_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_max_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_avgr_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extmul_low_i8x16_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extmul_high_i8x16_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extmul_low_i8x16_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i16x8_extmul_high_i8x16_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_abs)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_neg)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_all_true)
{
    return stack.take_and_put<ValueType::V128>(ValueType::I32);
}

VALIDATE_INSTRUCTION(i32x4_bitmask)
{
    return stack.take_and_put<ValueType::V128>(ValueType::I32);
}

VALIDATE_INSTRUCTION(i32x4_extend_low_i16x8_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extend_high_i16x8_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extend_low_i16x8_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extend_high_i16x8_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_shl)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_shr_s)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_shr_u)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_add)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_sub)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_mul)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_min_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_min_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_max_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_max_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_dot_i16x8_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extmul_low_i16x8_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extmul_high_i16x8_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extmul_low_i16x8_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_extmul_high_i16x8_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_abs)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_neg)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_all_true)
{
    return stack.take_and_put<ValueType::V128>(ValueType::I32);
}

VALIDATE_INSTRUCTION(i64x2_bitmask)
{
    return stack.take_and_put<ValueType::V128>(ValueType::I32);
}

VALIDATE_INSTRUCTION(i64x2_extend_low_i32x4_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_extend_high_i32x4_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_extend_low_i32x4_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_extend_high_i32x4_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_shl)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_shr_s)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_shr_u)
{
    return stack.take_and_put<ValueType::I32, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_add)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_sub)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_mul)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_eq)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_ne)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_lt_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_gt_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_le_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_ge_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_extmul_low_i32x4_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_extmul_high_i32x4_s)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_extmul_low_i32x4_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i64x2_extmul_high_i32x4_u)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_abs)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_neg)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_sqrt)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_add)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_sub)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_mul)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_div)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_min)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_max)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_pmin)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_pmax)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_abs)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_neg)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_sqrt)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_add)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_sub)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_mul)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_div)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_min)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_max)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_pmin)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_pmax)
{
    return stack.take_and_put<ValueType::V128, ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_trunc_sat_f32x4_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_trunc_sat_f32x4_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_convert_i32x4_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f32x4_convert_i32x4_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_trunc_sat_f64x2_s_zero)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(i32x4_trunc_sat_f64x2_u_zero)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_convert_low_i32x4_s)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

VALIDATE_INSTRUCTION(f64x2_convert_low_i32x4_u)
{
    return stack.take_and_put<ValueType::V128>(ValueType::V128);
}

ErrorOr<void, ValidationError> Validator::validate(Instruction const& instruction, Stack& stack, bool& is_constant)
{
    switch (instruction.opcode().value()) {
#define M(name, integer_value)                                                   \
    case Instructions::name.value():                                             \
        dbgln_if(WASM_VALIDATOR_DEBUG, "checking {}, stack = {}", #name, stack); \
        return validate_instruction<integer_value>(instruction, stack, is_constant);

        ENUMERATE_WASM_OPCODES(M)

#undef M
    default:
        is_constant = false;
        return Errors::invalid(ByteString::formatted("instruction opcode ({:#x})", instruction.opcode().value()));
    }
}

ErrorOr<Validator::ExpressionTypeResult, ValidationError> Validator::validate(Expression const& expression, Vector<ValueType> const& result_types)
{
    if (m_frames.is_empty())
        m_frames.empend(FunctionType { {}, result_types }, FrameKind::Function, (size_t)0);
    auto stack = Stack(m_frames);
    bool is_constant_expression = true;

    for (auto& instruction : expression.instructions()) {
        bool is_constant = false;
        TRY(validate(instruction, stack, is_constant));

        is_constant_expression &= is_constant;
    }

    auto expected_result_types = result_types;
    while (!expected_result_types.is_empty())
        TRY(stack.take(expected_result_types.take_last()));

    for (auto& type : result_types)
        stack.append(type);
    m_frames.take_last();
    VERIFY(m_frames.is_empty());

    return ExpressionTypeResult { stack.release_vector(), is_constant_expression };
}

ByteString Validator::Errors::find_instruction_name(SourceLocation const& location)
{
    auto index = location.function_name().find('<');
    auto end_index = location.function_name().find('>');
    if (!index.has_value() || !end_index.has_value())
        return ByteString::formatted("{}", location);

    auto opcode = location.function_name().substring_view(index.value() + 1, end_index.value() - index.value() - 1).to_number<unsigned>();
    if (!opcode.has_value())
        return ByteString::formatted("{}", location);

    return instruction_name(OpCode { *opcode });
}
}
