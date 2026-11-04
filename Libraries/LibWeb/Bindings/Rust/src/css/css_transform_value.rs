/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSTransformValue, https://drafts.css-houdini.org/css-typed-om-1/#csstransformvalue

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, MarkedVec, Trace, Vm};
use libweb_rust::typed_om::RetainedStyleValue;

use super::css_style_value::{AnyCSSStyleValue, CSSStyleValue, PerformTypeCheck};
use super::css_transform_component::{AnyCSSTransformComponent, create_dom_matrix};
use super::typed_om;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::geometry::dom_matrix::DOMMatrix;
use crate::geometry::dom_matrix_read_only::DOMMatrixReadOnly;
use crate::geometry::matrix::Matrix4;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSTransformValue {
    style_value: CSSStyleValue,
    transforms: GcRefCell<Vec<AnyCSSTransformComponent>>,
}

impl Wrappable for CSSTransformValue {}

impl Inherits<CSSStyleValue> for CSSTransformValue {
    fn parent(&self) -> &CSSStyleValue {
        &self.style_value
    }
}

impl CSSTransformValue {
    pub(crate) fn create(vm: &Vm, transforms: &MarkedVec<'_, AnyCSSTransformComponent>) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            style_value: CSSStyleValue::empty(),
            transforms: GcRefCell::new(transforms.to_vec()),
        });
        object
            .style_value
            .set_object(vm, AnyCSSStyleValue::CSSTransformValue(object));
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformvalue-csstransformvalue
    pub(crate) fn construct(
        cx: &mut CallContext,
        transforms: MarkedVec<'_, AnyCSSTransformComponent>,
    ) -> WebIdlResult<Ref<Self>> {
        // The CSSTransformValue(transforms) constructor must, when called, perform the following steps:

        // 1. If transforms is empty, throw a TypeError.
        if transforms.is_empty() {
            return Err(Exception::type_error(
                "CSSTransformValue's transforms list cannot be empty.",
            ));
        }

        // 2. Return a new CSSTransformValue whose values to iterate over is transforms.
        Ok(Self::create(cx.vm(), &transforms))
    }

    pub(crate) fn style_value(&self) -> &CSSStyleValue {
        &self.style_value
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformvalue-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The length attribute indicates how many transform components are contained within the CSSTransformValue.
        Ok(u32::try_from(self.transforms.borrow().len()).expect("the list has fewer items than an index can name"))
    }

    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<AnyCSSTransformComponent> {
        Ok(self.transforms.borrow()[index as usize])
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-set-the-value-of-an-existing-indexed-property%E2%91%A0
    pub(crate) fn set_value_of_existing_indexed_property(
        &self,
        cx: &mut CallContext,
        index: u32,
        value: AnyCSSTransformComponent,
    ) -> WebIdlResult<()> {
        // To set the value of an existing indexed property of a CSSTransformValue this, an index n, and a value new
        // value, let values be this’s [[values]] internal slot, and set values[n] to new value.
        self.transforms.borrow_mut(cx.vm())[index as usize] = value;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-set-the-value-of-a-new-indexed-property①
    pub(crate) fn set_value_of_new_indexed_property(
        &self,
        cx: &mut CallContext,
        index: u32,
        value: AnyCSSTransformComponent,
    ) -> WebIdlResult<()> {
        // To set the value of a new indexed property of a CSSTransformValue this, an index n, and a value new value,
        // let values be this’s [[values]] internal slot. If n is not equal to the size of values, throw a RangeError.
        // Otherwise, append new value to values.
        if index as usize != self.transforms.borrow().len() {
            return Err(Exception::range_error("Index out of range"));
        }

        self.transforms.borrow_mut(cx.vm()).push(value);
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformvalue-is2d
    pub(crate) fn is_2d(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The is2D attribute of a CSSTransformValue this must, on getting, return true if, for each func in this’s
        // values to iterate over, the func’s is2D attribute would return true; otherwise, the attribute returns false.
        Ok(self.transforms.borrow().iter().all(|transform| transform.raw_is_2d()))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformvalue-tomatrix
    pub(crate) fn to_matrix(&self, cx: &mut CallContext) -> WebIdlResult<Ref<DOMMatrix>> {
        // The toMatrix() method of a CSSTransformValue this must, when called, perform the following steps:
        let vm = cx.vm();

        // 1. Let matrix be a new DOMMatrix, initialized to the identity matrix, with its is2D internal slot set to
        //    true.
        // NB: The elements and the slot stay in locals until the matrix is returned.
        let mut matrix = Matrix4::identity();
        let mut is_2d = true;

        // 2. For each func in this’s values to iterate over:
        for function in self.transforms.borrow().iter() {
            // 1. Let funcMatrix be the DOMMatrix returned by calling toMatrix() on func.
            // AD-HOC: This can throw exceptions.
            let function_matrix = function.to_matrix(vm)?;
            let function_matrix = <DOMMatrix as Inherits<DOMMatrixReadOnly>>::parent(&function_matrix);

            // 2. Set matrix to the result of multiplying matrix and the matrix represented by funcMatrix.
            matrix = matrix.multiplied(function_matrix.matrix());
            if !function_matrix.is_2d() {
                is_2d = false;
            }
        }

        // 3. Return matrix.
        Ok(create_dom_matrix(vm, matrix, is_2d))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-csstransformvalue
    pub(crate) fn serialize(&self, _: &Vm) -> WebIdlResult<Utf16String> {
        // 1. Return the result of serializing each item in this’s values to iterate over, then concatenating them
        //    separated by " ".
        let mut s = String::new();
        for (index, transform) in self.transforms.borrow().iter().enumerate() {
            if index != 0 {
                s.push(' ');
            }
            s.push_str(&typed_om::to_rust_string(&transform.serialize()?));
        }
        Ok(Utf16String::from_utf8(&s))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
    pub(crate) fn create_an_internal_representation(
        &self,
        vm: &Vm,
        property_id: u16,
        perform_type_check: PerformTypeCheck,
    ) -> WebIdlResult<RetainedStyleValue> {
        // NB: This can become <transform-function> or <transform-list>, and we don't know which is wanted without
        //     performing the type checking.
        //     We can worry about that if and when we ever do have a CSSTransformValue that isn't top-level.
        assert!(perform_type_check == PerformTypeCheck::Yes);

        // If value is a CSSStyleValue subclass,
        //     If value does not match the grammar of a list-valued property iteration of property, throw a TypeError.
        //
        //     If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of
        //     value is a CSSUnitValue that is outside of that range, replace that value with the result of wrapping it
        //     in a fresh CSSMathSum whose values internal slot contains only that part of value.
        //
        //     Return the value.

        // NB: We match <transform-function> if we have 1 transform. We match <transform-list> always.
        let transforms = self.transforms.borrow();
        if transforms.len() == 1 && typed_om::property_accepts_transform_function(property_id) {
            return transforms[0].create_style_value(vm, property_id);
        }

        if typed_om::property_accepts_transform_list(property_id) {
            let values = transforms
                .iter()
                .map(|transform| transform.create_style_value(vm, property_id))
                .collect::<WebIdlResult<Vec<_>>>()?;
            return Ok(typed_om::style_value_list(values));
        }

        Err(Exception::type_error("Property does not accept values of this type."))
    }
}
