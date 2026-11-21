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

/// Emit a property access by key, using GetLength for the "length" property.
pub(super) fn emit_get_by_key(generator: &mut Generator<'_>, dst: Operand, base: Operand, key: PropertyKeyTableIndex) {
    if generator.context.names.is_length_property_key(key) {
        generator.length_identifier = Some(key);
        let cache = generator.next_property_lookup_cache();
        let keyed_feedback = generator.next_keyed_feedback();
        generator.put_with_destination(
            op::GetLength {
                keyed_feedback,
                dst,
                base,
                cache,
            },
            dst,
        );
    } else {
        let cache = generator.next_property_lookup_cache();
        generator.put_with_destination(
            op::GetById {
                dst,
                base,
                property: key,
                cache,
            },
            dst,
        );
    }
}

/// Emit a property access by name with a this value.
pub(super) fn emit_get_by_id_with_this(
    generator: &mut Generator<'_>,
    dst: Operand,
    base: Operand,
    property_name: Name,
    this_value: Operand,
) {
    let key = generator.intern_property_key(property_name);
    emit_get_by_key_with_this(generator, dst, base, key, this_value);
}

/// Emit a property access by key with a this value, using GetLengthWithThis for the "length" property.
fn emit_get_by_key_with_this(
    generator: &mut Generator<'_>,
    dst: Operand,
    base: Operand,
    key: PropertyKeyTableIndex,
    this_value: Operand,
) {
    if generator.context.names.is_length_property_key(key) {
        generator.length_identifier = Some(key);
        let cache = generator.next_property_lookup_cache();
        generator.put_with_destination(
            op::GetLengthWithThis {
                dst,
                base,
                this_value,
                cache,
            },
            dst,
        );
    } else {
        let cache = generator.next_property_lookup_cache();
        generator.put_with_destination(
            op::GetByIdWithThis {
                dst,
                base,
                property: key,
                this_value,
                cache,
            },
            dst,
        );
    }
}

/// Emit a property read by value, optimizing constant string properties to GetById.
pub(super) fn emit_get_by_value(generator: &mut Generator<'_>, dst: Operand, base: Operand, property: Operand) {
    if let Some(key) = generator.try_constant_string_to_property_key(property) {
        emit_get_by_key(generator, dst, base, key);
        return;
    }
    let keyed_feedback = generator.next_keyed_feedback();
    let cache = generator.next_property_lookup_cache();
    generator.put_with_destination(
        op::GetByValue {
            keyed_feedback,
            dst,
            base,
            property,
            cache,
        },
        dst,
    );
}

/// Emit a property read by value with explicit this, optimizing constant string properties.
pub(super) fn emit_get_by_value_with_this(
    generator: &mut Generator<'_>,
    dst: Operand,
    base: Operand,
    property: Operand,
    this_value: Operand,
) {
    if let Some(key) = generator.try_constant_string_to_property_key(property) {
        emit_get_by_key_with_this(generator, dst, base, key, this_value);
        return;
    }
    let value_feedback = generator.next_value_feedback();
    generator.put_with_destination(
        op::GetByValueWithThis {
            value_feedback,
            dst,
            base,
            property,
            this_value,
        },
        dst,
    );
}

/// Emit a normal property write by value with explicit this, optimizing constant string properties.
pub(super) fn emit_put_normal_by_value_with_this(
    generator: &mut Generator<'_>,
    base: Operand,
    property: Operand,
    this_value: Operand,
    src: Operand,
) {
    if let Some(key) = generator.try_constant_string_to_property_key(property) {
        let cache = generator.next_property_lookup_cache();
        generator.put(op::PutByIdWithThis {
            base,
            this_value,
            property: key,
            src,
            cache,
            kind: PutKind::Normal as u8,
        });
        return;
    }
    generator.put(op::PutByValueWithThis {
        base,
        property,
        this_value,
        src,
        kind: PutKind::Normal as u8,
    });
}

/// Emit a property write by value, optimizing constant string properties to the ById variant.
pub(super) fn emit_put_by_value(
    generator: &mut Generator<'_>,
    base: Operand,
    property: Operand,
    src: Operand,
    kind: PutKind,
) {
    if let Some(key) = generator.try_constant_string_to_property_key(property) {
        let cache = generator.next_property_lookup_cache();
        generator.put(op::PutById {
            base,
            property: key,
            src,
            cache,
            kind: kind as u8,
        });
        return;
    }
    let keyed_feedback = generator.next_keyed_feedback();
    let cache = generator.next_property_lookup_cache();
    generator.put(op::PutByValue {
        keyed_feedback,
        base,
        property,
        src,
        kind: kind as u8,
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
