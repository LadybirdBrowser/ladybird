/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/NumericLimits.h>
#include <AK/StringView.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/PutKind.h>
#include <LibJS/Bytecode/Validator.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/RustFFI.h>

namespace JS::Bytecode {

static StringView validation_error_kind_to_string(JS::FFI::ValidationErrorKind kind)
{
    switch (kind) {
    case JS::FFI::ValidationErrorKind::Ok:
        return "Ok"sv;
    case JS::FFI::ValidationErrorKind::BufferNotAligned:
        return "BufferNotAligned"sv;
    case JS::FFI::ValidationErrorKind::InstructionMisaligned:
        return "InstructionMisaligned"sv;
    case JS::FFI::ValidationErrorKind::UnknownOpcode:
        return "UnknownOpcode"sv;
    case JS::FFI::ValidationErrorKind::TruncatedInstruction:
        return "TruncatedInstruction"sv;
    case JS::FFI::ValidationErrorKind::InvalidLength:
        return "InvalidLength"sv;
    case JS::FFI::ValidationErrorKind::OperandOutOfRange:
        return "OperandOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::OperandInvalid:
        return "OperandInvalid"sv;
    case JS::FFI::ValidationErrorKind::LabelNotAtInstructionBoundary:
        return "LabelNotAtInstructionBoundary"sv;
    case JS::FFI::ValidationErrorKind::IdentifierIndexOutOfRange:
        return "IdentifierIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::StringIndexOutOfRange:
        return "StringIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::PropertyKeyIndexOutOfRange:
        return "PropertyKeyIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::RegexIndexOutOfRange:
        return "RegexIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::PropertyLookupCacheIndexOutOfRange:
        return "PropertyLookupCacheIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::GlobalVariableCacheIndexOutOfRange:
        return "GlobalVariableCacheIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::TemplateObjectCacheIndexOutOfRange:
        return "TemplateObjectCacheIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::ObjectShapeCacheIndexOutOfRange:
        return "ObjectShapeCacheIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::ObjectPropertyIteratorCacheIndexOutOfRange:
        return "ObjectPropertyIteratorCacheIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::SharedFunctionDataIndexOutOfRange:
        return "SharedFunctionDataIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::ClassBlueprintIndexOutOfRange:
        return "ClassBlueprintIndexOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::EnumOutOfRange:
        return "EnumOutOfRange"sv;
    case JS::FFI::ValidationErrorKind::BasicBlockOffsetInvalid:
        return "BasicBlockOffsetInvalid"sv;
    case JS::FFI::ValidationErrorKind::ExceptionHandlerStartInvalid:
        return "ExceptionHandlerStartInvalid"sv;
    case JS::FFI::ValidationErrorKind::ExceptionHandlerEndInvalid:
        return "ExceptionHandlerEndInvalid"sv;
    case JS::FFI::ValidationErrorKind::ExceptionHandlerHandlerInvalid:
        return "ExceptionHandlerHandlerInvalid"sv;
    case JS::FFI::ValidationErrorKind::ExceptionHandlerRangeInvalid:
        return "ExceptionHandlerRangeInvalid"sv;
    case JS::FFI::ValidationErrorKind::SourceMapOffsetInvalid:
        return "SourceMapOffsetInvalid"sv;
    }
    VERIFY_NOT_REACHED();
}

// Variant counts for the C++ enums referenced by Bytecode.def fields. The
// static_asserts pin the last variant so adding a new one without bumping
// the count here breaks the build instead of silently outdating the
// validator.
static constexpr u32 completion_type_variant_count = to_underlying(Completion::Type::Throw) + 1;
static_assert(completion_type_variant_count == 6);
static constexpr u32 iterator_hint_variant_count = to_underlying(IteratorHint::Async) + 1;
static_assert(iterator_hint_variant_count == 2);
static constexpr u32 environment_mode_variant_count = to_underlying(Op::EnvironmentMode::Var) + 1;
static_assert(environment_mode_variant_count == 2);
static constexpr u32 put_kind_variant_count = to_underlying(PutKind::Own) + 1;
static_assert(put_kind_variant_count == 5);
static constexpr u32 arguments_kind_variant_count = to_underlying(Op::ArgumentsKind::Unmapped) + 1;
static_assert(arguments_kind_variant_count == 2);

ErrorOr<void> validate_bytecode(Executable const& executable, CacheState cache_state)
{
    JS::FFI::FFIValidatorBounds bounds {
        .number_of_registers = executable.number_of_registers,
        .number_of_locals = static_cast<u32>(executable.local_variable_names.size()),
        .number_of_constants = static_cast<u32>(executable.constants.size()),
        .number_of_arguments = executable.number_of_arguments,
        .identifier_table_size = static_cast<u32>(executable.identifier_table->identifiers().size()),
        .string_table_size = static_cast<u32>(executable.string_table->size()),
        .property_key_table_size = static_cast<u32>(executable.property_key_table->property_keys().size()),
        // The regex table is not consulted at runtime; m_regex_index fields
        // are skipped during validation.
        .regex_table_size = 0,
        .property_lookup_cache_count = static_cast<u32>(executable.property_lookup_caches.size()),
        .global_variable_cache_count = static_cast<u32>(executable.global_variable_caches.size()),
        .template_object_cache_count = static_cast<u32>(executable.template_object_caches.size()),
        .object_shape_cache_count = static_cast<u32>(executable.object_shape_caches.size()),
        .object_property_iterator_cache_count = static_cast<u32>(executable.object_property_iterator_caches.size()),
        .class_blueprint_count = static_cast<u32>(executable.class_blueprints.size()),
        .shared_function_data_count = static_cast<u32>(executable.shared_function_data.size()),
        .completion_type_variant_count = completion_type_variant_count,
        .iterator_hint_variant_count = iterator_hint_variant_count,
        .environment_mode_variant_count = environment_mode_variant_count,
        .put_kind_variant_count = put_kind_variant_count,
        .arguments_kind_variant_count = arguments_kind_variant_count,
        .before_cache_fixup = cache_state == CacheState::BeforeFixup,
    };

    // Project Executable's exception handlers down to plain offsets; the
    // structural metadata's source-position parts aren't validated here.
    Vector<JS::FFI::FFIExceptionHandlerOffsets> handler_offsets;
    handler_offsets.ensure_capacity(executable.exception_handlers.size());
    for (auto const& h : executable.exception_handlers) {
        handler_offsets.append({
            .start = static_cast<u32>(h.start_offset),
            .end = static_cast<u32>(h.end_offset),
            .handler = static_cast<u32>(h.handler_offset),
        });
    }

    Vector<u32> basic_block_offsets;
    basic_block_offsets.ensure_capacity(executable.basic_block_start_offsets.size());
    for (auto offset : executable.basic_block_start_offsets)
        basic_block_offsets.append(static_cast<u32>(offset));

    Vector<u32> source_map_offsets;
    source_map_offsets.ensure_capacity(executable.source_map.size());
    for (auto const& entry : executable.source_map)
        source_map_offsets.append(entry.bytecode_offset);

    JS::FFI::FFIValidatorExtras extras {
        .basic_block_offsets = basic_block_offsets.data(),
        .basic_block_count = basic_block_offsets.size(),
        .exception_handlers = handler_offsets.data(),
        .exception_handler_count = handler_offsets.size(),
        .source_map_offsets = source_map_offsets.data(),
        .source_map_count = source_map_offsets.size(),
    };

    JS::FFI::FFIValidationError error {};
    auto ok = rust_validate_bytecode(
        executable.bytecode.data(),
        executable.bytecode.size(),
        &bounds,
        &extras,
        &error);
    if (ok)
        return {};

    auto kind = validation_error_kind_to_string(error.kind);
    dbgln("Bytecode validation failed at offset {} (opcode {}): {}",
        error.offset, error.opcode, kind);
    return AK::Error::from_string_view(kind);
}

}
