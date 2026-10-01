/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The classes an executable declares, as in Libraries/LibJS/Bytecode/ClassBlueprint.h.

use std::rc::Rc;

use ak::{Utf16FlyString, Utf16String};
use libjs_abi::ClassElementKind;
use libjs_runtime_macros::Trace;

use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::value::Value;
use crate::runtime::primitive_string::PrimitiveString;
use crate::source_code::SourceCode;
use libjs_rust::bytecode::generator::{PendingClassBlueprint, PendingClassElement, PendingLiteralValueKind};

#[derive(Clone, Trace)]
pub struct ClassElementDescriptor {
    #[gc(untraced)]
    pub kind: ClassElementKind,
    pub is_static: bool,
    pub is_private: bool,
    pub private_identifier: Option<Utf16FlyString>,
    pub shared_function_data_index: Option<u32>,
    pub has_initializer: bool,
    pub literal_value: Option<Value>,
}

#[derive(Trace)]
pub struct ClassBlueprint {
    pub constructor_shared_function_data_index: u32,
    pub has_super_class: bool,
    pub has_name: bool,
    pub name: Utf16FlyString,
    #[gc(untraced)]
    pub source_code: Option<Rc<SourceCode>>,
    pub source_text_offset: usize,
    pub source_text_length: usize,
    pub elements: Vec<ClassElementDescriptor>,
}

fn class_element_kind(kind: u8) -> ClassElementKind {
    match kind {
        0 => ClassElementKind::Method,
        1 => ClassElementKind::Getter,
        2 => ClassElementKind::Setter,
        3 => ClassElementKind::Field,
        4 => ClassElementKind::StaticInitializer,
        _ => unreachable!("{kind} is not a class element kind"),
    }
}

impl ClassBlueprint {
    /// Creates the blueprint of a class the frontend compiled, as rust_create_class_blueprint does. The literal values
    /// of its elements are also pushed to `rooted_literal_values`, which keeps them alive until something that traces
    /// the blueprint holds it.
    pub fn create(
        vm: &Vm,
        blueprint: &PendingClassBlueprint,
        source_code: Option<&Rc<SourceCode>>,
        rooted_literal_values: &MarkedVec<'_, Value>,
    ) -> ClassBlueprint {
        let name = match &blueprint.name {
            Some(name) if !name.is_empty() => Utf16FlyString::from_utf16(name),
            _ => Utf16FlyString::default(),
        };
        let elements = blueprint
            .elements
            .iter()
            .map(|element| Self::create_element(vm, element, rooted_literal_values))
            .collect();
        ClassBlueprint {
            constructor_shared_function_data_index: blueprint.constructor_sfd_index,
            has_super_class: blueprint.has_super_class,
            has_name: blueprint.has_name,
            name,
            source_code: source_code.cloned(),
            source_text_offset: blueprint.source_text_offset,
            source_text_length: blueprint.source_text_length,
            elements,
        }
    }

    fn create_element(
        vm: &Vm,
        element: &PendingClassElement,
        rooted_literal_values: &MarkedVec<'_, Value>,
    ) -> ClassElementDescriptor {
        let private_identifier = element
            .private_identifier
            .as_ref()
            .filter(|identifier| !identifier.is_empty())
            .map(|identifier| Utf16FlyString::from_utf16(identifier));
        let literal_value = match element.literal_value_kind {
            PendingLiteralValueKind::None => None,
            PendingLiteralValueKind::Number => Some(Value::from_f64(element.literal_value_number)),
            PendingLiteralValueKind::BooleanTrue => Some(Value::TRUE),
            PendingLiteralValueKind::BooleanFalse => Some(Value::FALSE),
            PendingLiteralValueKind::Null => Some(Value::NULL),
            PendingLiteralValueKind::String => {
                let string = element
                    .literal_value_string
                    .as_ref()
                    .map_or_else(Utf16String::default, |string| Utf16String::from_utf16(string));
                let value = Value::from_string(PrimitiveString::create(vm, string));
                rooted_literal_values.push(value);
                Some(value)
            }
        };
        ClassElementDescriptor {
            kind: class_element_kind(element.kind),
            is_static: element.is_static,
            is_private: element.is_private,
            private_identifier,
            shared_function_data_index: element.shared_function_data_index,
            has_initializer: element.has_initializer,
            literal_value,
        }
    }
}
