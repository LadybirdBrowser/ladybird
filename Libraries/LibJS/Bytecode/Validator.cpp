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
#include <LibJS/Bytecode/Validator.h>
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
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<void> validate_bytecode(Executable const& executable, CacheState cache_state)
{
    JS::FFI::FFIValidatorBounds bounds {
        .number_of_registers = executable.number_of_registers,
        .number_of_locals = static_cast<u32>(executable.local_variable_names.size()),
        .number_of_constants = static_cast<u32>(executable.constants.size()),
        // Argument count isn't tracked on Executable yet; per-instruction
        // argument-index checks land in a follow-up commit.
        .number_of_arguments = NumericLimits<u32>::max(),
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
        .before_cache_fixup = cache_state == CacheState::BeforeFixup,
    };

    JS::FFI::FFIValidationError error {};
    auto ok = rust_validate_bytecode(
        executable.bytecode.data(),
        executable.bytecode.size(),
        &bounds,
        &error);
    if (ok)
        return {};

    auto kind = validation_error_kind_to_string(error.kind);
    dbgln("Bytecode validation failed at offset {} (opcode {}): {}",
        error.offset, error.opcode, kind);
    return AK::Error::from_string_view(kind);
}

}
