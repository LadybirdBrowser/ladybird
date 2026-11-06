/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Member access, super properties and delete.

use libjs_abi::PutKind;

use crate::ast::Name;
use crate::utf16::Utf16StringBuilder;
use crate::utf16::Utf16View;

use crate::bytecode::instruction::typed as op;
use crate::bytecode::operand::*;

use super::context::Generator;
use super::registers::ScopedOperand;

/// Emit a property access by key, using GetLength for the "length" property.
pub(super) fn emit_get_by_key(
    generator: &mut Generator<'_>,
    dst: &ScopedOperand,
    base: &ScopedOperand,
    key: PropertyKeyTableIndex,
    base_identifier: Option<IdentifierTableIndex>,
) {
    if generator.context.names.is_length_property_key(key) {
        generator.length_identifier = Some(key);
        let cache = generator.next_property_lookup_cache();
        let value_feedback = generator.next_value_feedback();
        let keyed_feedback = generator.next_keyed_feedback();
        generator.emit(op::GetLength {
            value_feedback,
            keyed_feedback,
            dst: dst.operand(),
            base: base.operand(),
            base_identifier,
            cache,
        });
    } else {
        let cache = generator.next_property_lookup_cache();
        let value_feedback = generator.next_value_feedback();
        generator.emit(op::GetById {
            value_feedback,
            dst: dst.operand(),
            base: base.operand(),
            property: key,
            base_identifier,
            cache,
        });
    }
}

/// Emit a property access by name with a this value, using GetLengthWithThis
/// for the "length" property.
pub(super) fn emit_get_by_id_with_this(
    generator: &mut Generator<'_>,
    dst: &ScopedOperand,
    base: &ScopedOperand,
    property_name: Name,
    this_value: &ScopedOperand,
) {
    let key = generator.intern_property_key(property_name);
    if generator.context.names.is_length_property_key(key) {
        generator.length_identifier = Some(key);
        let cache = generator.next_property_lookup_cache();
        let value_feedback = generator.next_value_feedback();
        generator.emit(op::GetLengthWithThis {
            value_feedback,
            dst: dst.operand(),
            base: base.operand(),
            this_value: this_value.operand(),
            cache,
        });
    } else {
        let cache = generator.next_property_lookup_cache();
        let value_feedback = generator.next_value_feedback();
        generator.emit(op::GetByIdWithThis {
            value_feedback,
            dst: dst.operand(),
            base: base.operand(),
            property: key,
            this_value: this_value.operand(),
            cache,
        });
    }
}

/// Emit a property read by value, optimizing constant string properties to GetById.
pub(super) fn emit_get_by_value(
    generator: &mut Generator<'_>,
    dst: &ScopedOperand,
    base: &ScopedOperand,
    property: &ScopedOperand,
    base_identifier: Option<IdentifierTableIndex>,
) {
    if let Some(key) = generator.try_constant_string_to_property_key(property) {
        if generator.context.names.is_length_property_key(key) {
            generator.length_identifier = Some(key);
            let cache = generator.next_property_lookup_cache();
            let value_feedback = generator.next_value_feedback();
            let keyed_feedback = generator.next_keyed_feedback();
            generator.emit(op::GetLength {
                value_feedback,
                keyed_feedback,
                dst: dst.operand(),
                base: base.operand(),
                base_identifier,
                cache,
            });
        } else {
            let cache = generator.next_property_lookup_cache();
            let value_feedback = generator.next_value_feedback();
            generator.emit(op::GetById {
                value_feedback,
                dst: dst.operand(),
                base: base.operand(),
                property: key,
                base_identifier,
                cache,
            });
        }
        return;
    }
    let value_feedback = generator.next_value_feedback();
    let keyed_feedback = generator.next_keyed_feedback();
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::GetByValue {
        value_feedback,
        keyed_feedback,
        dst: dst.operand(),
        base: base.operand(),
        property: property.operand(),
        base_identifier,
        cache,
    });
}

/// Emit a property read by value with explicit this, optimizing constant string properties.
pub(super) fn emit_get_by_value_with_this(
    generator: &mut Generator<'_>,
    dst: &ScopedOperand,
    base: &ScopedOperand,
    property: &ScopedOperand,
    this_value: &ScopedOperand,
) {
    if let Some(key) = generator.try_constant_string_to_property_key(property) {
        if generator.context.names.is_length_property_key(key) {
            generator.length_identifier = Some(key);
            let cache = generator.next_property_lookup_cache();
            let value_feedback = generator.next_value_feedback();
            generator.emit(op::GetLengthWithThis {
                value_feedback,
                dst: dst.operand(),
                base: base.operand(),
                this_value: this_value.operand(),
                cache,
            });
        } else {
            let cache = generator.next_property_lookup_cache();
            let value_feedback = generator.next_value_feedback();
            generator.emit(op::GetByIdWithThis {
                value_feedback,
                dst: dst.operand(),
                base: base.operand(),
                property: key,
                this_value: this_value.operand(),
                cache,
            });
        }
        return;
    }
    let value_feedback = generator.next_value_feedback();
    generator.emit(op::GetByValueWithThis {
        value_feedback,
        dst: dst.operand(),
        base: base.operand(),
        property: property.operand(),
        this_value: this_value.operand(),
    });
}

/// Emit a normal property write by value, optimizing constant string properties to PutNormalById.
pub(super) fn emit_put_normal_by_value(
    generator: &mut Generator<'_>,
    base: &ScopedOperand,
    property: &ScopedOperand,
    src: &ScopedOperand,
    base_identifier: Option<IdentifierTableIndex>,
) {
    if let Some(key) = generator.try_constant_string_to_property_key(property) {
        let cache = generator.next_property_lookup_cache();
        generator.emit(op::PutById {
            base: base.operand(),
            property: key,
            src: src.operand(),
            cache,
            base_identifier,
            kind: PutKind::Normal as u32,
        });
        return;
    }
    let keyed_feedback = generator.next_keyed_feedback();
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::PutByValue {
        keyed_feedback,
        base: base.operand(),
        property: property.operand(),
        src: src.operand(),
        base_identifier,
        kind: PutKind::Normal as u32,
        cache,
    });
}

/// Emit a normal property write by value with explicit this, optimizing constant string properties.
pub(super) fn emit_put_normal_by_value_with_this(
    generator: &mut Generator<'_>,
    base: &ScopedOperand,
    property: &ScopedOperand,
    this_value: &ScopedOperand,
    src: &ScopedOperand,
) {
    if let Some(key) = generator.try_constant_string_to_property_key(property) {
        let cache = generator.next_property_lookup_cache();
        generator.emit(op::PutByIdWithThis {
            base: base.operand(),
            this_value: this_value.operand(),
            property: key,
            src: src.operand(),
            cache,
            kind: PutKind::Normal as u32,
        });
        return;
    }
    generator.emit(op::PutByValueWithThis {
        base: base.operand(),
        property: property.operand(),
        this_value: this_value.operand(),
        src: src.operand(),
        kind: PutKind::Normal as u32,
    });
}

/// Emit a property write by value, optimizing constant string properties to the ById variant.
pub(super) fn emit_put_by_value(
    generator: &mut Generator<'_>,
    base: &ScopedOperand,
    property: &ScopedOperand,
    src: &ScopedOperand,
    kind: PutKind,
) {
    if let Some(key) = generator.try_constant_string_to_property_key(property) {
        let cache = generator.next_property_lookup_cache();
        generator.emit(op::PutById {
            base: base.operand(),
            property: key,
            src: src.operand(),
            cache,
            base_identifier: None,
            kind: kind as u32,
        });
        return;
    }
    let keyed_feedback = generator.next_keyed_feedback();
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::PutByValue {
        keyed_feedback,
        base: base.operand(),
        property: property.operand(),
        src: src.operand(),
        base_identifier: None,
        kind: kind as u32,
        cache,
    });
}

/// A private name as the tables hold it, with its `#`.
pub(super) fn intern_private_name(generator: &mut Generator<'_>, name: Name) -> IdentifierTableIndex {
    let mut text = Utf16StringBuilder::new();
    text.append_ascii("#");
    text.append(generator.tree.name_text(name));
    let text = text.to_utf16_string();
    generator.intern_identifier_text(Utf16View::of_string(&text))
}
