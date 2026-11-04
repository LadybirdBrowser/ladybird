/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSMatrixComponent, https://drafts.css-houdini.org/css-typed-om-1/#cssmatrixcomponent

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, Trace, Vm};
use libweb_rust::typed_om::{NumericValue, RetainedStyleValue, TransformFunction, transformation_style_value};

use super::css_transform_component::{AnyCSSTransformComponent, CSSTransformComponent, Is2D, create_dom_matrix};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::geometry::dom_matrix::DOMMatrix;
use crate::geometry::dom_matrix_read_only::{AnyDOMMatrixReadOnly, DOMMatrixReadOnly};
use crate::geometry::matrix::{ColumnMajor, Matrix4};

// https://drafts.css-houdini.org/css-typed-om-1/#dictdef-cssmatrixcomponentoptions
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct CSSMatrixComponentOptions {
    pub(crate) is2d: Option<bool>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSMatrixComponent {
    transform_component: CSSTransformComponent,
    matrix: GcRefCell<Ref<DOMMatrix>>,
}

impl Wrappable for CSSMatrixComponent {}

impl Inherits<CSSTransformComponent> for CSSMatrixComponent {
    fn parent(&self) -> &CSSTransformComponent {
        &self.transform_component
    }
}

impl CSSMatrixComponent {
    pub(crate) fn create(vm: &Vm, is_2d: Is2D, matrix: Ref<DOMMatrix>) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            transform_component: CSSTransformComponent::new(is_2d),
            matrix: GcRefCell::new(matrix),
        });
        AnyCSSTransformComponent::CSSMatrixComponent(object).finish(vm);
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmatrixcomponent-cssmatrixcomponent
    pub(crate) fn construct(
        cx: &mut CallContext,
        matrix: AnyDOMMatrixReadOnly,
        options: CSSMatrixComponentOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // The CSSMatrixComponent(matrix, options) constructor must, when invoked, perform the following steps:

        // 1. Let this be a new CSSMatrixComponent object with its matrix internal slot set to matrix.
        // NB: Done below.

        // 2. If options was passed and has a is2D field, set this’s is2D internal slot to the value of that field.
        // 3. Otherwise, set this’s is2D internal slot to the value of matrix’s is2D internal slot.
        let is_2d = if options.is2d.unwrap_or(matrix.is_2d()) {
            Is2D::Yes
        } else {
            Is2D::No
        };

        let vm = cx.vm();
        let this = Self::create(vm, is_2d, create_dom_matrix(vm, matrix.matrix(), matrix.is_2d()));

        // 4. Return this.
        Ok(this)
    }

    /// The elements m11, m12, m13, m14, m21, ..., m44 of the matrix internal slot.
    fn elements(&self) -> [f64; 16] {
        let matrix = *self.matrix.borrow();
        <DOMMatrix as Inherits<DOMMatrixReadOnly>>::parent(&matrix)
            .matrix()
            .to_column_major()
    }

    pub(crate) fn matrix(&self, _: &mut CallContext) -> WebIdlResult<Ref<DOMMatrix>> {
        Ok(*self.matrix.borrow())
    }

    pub(crate) fn set_matrix(&self, cx: &mut CallContext, matrix: Ref<DOMMatrix>) -> WebIdlResult<()> {
        // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
        // FIXME: Should this modify is_2d? Or should we modify the matrix's is_2d?
        *self.matrix.borrow_mut(cx.vm()) = matrix;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmatrixcomponent
    pub(crate) fn serialize(&self) -> WebIdlResult<Utf16String> {
        // 1. Return the serialization of this’s matrix internal slot.
        // FIXME: This means we ignore our is_2d state. https://github.com/w3c/css-houdini-drafts/issues/1155
        let matrix = *self.matrix.borrow();
        <DOMMatrix as Inherits<DOMMatrixReadOnly>>::parent(&matrix)
            .stringify()
            .ok_or_else(|| {
                Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_ascii(b"Cannot stringify non-finite matrix values"),
                )
            })
    }

    pub(crate) fn to_matrix(&self, vm: &Vm) -> Ref<DOMMatrix> {
        // https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformcomponent-tomatrix
        // 1. Let matrix be a new DOMMatrix object, initialized to this’s equivalent 4x4 transform matrix, as defined in
        //    CSS Transforms 1 § 12. Mathematical Description of Transform Functions, and with its is2D internal slot
        //    set to the same value as this’s is2D internal slot.
        // NB: The elements are m11, m12, m13, m14, m21, ..., m44. A 2D component only uses m11, m12, m21, m22, m41 and
        //     m42.
        let is_2d = self.transform_component.raw_is_2d();
        let mut elements = self.elements();
        if is_2d {
            elements = [
                elements[0],
                elements[1],
                0.0,
                0.0,
                elements[4],
                elements[5],
                0.0,
                0.0,
                0.0,
                0.0,
                1.0,
                0.0,
                elements[12],
                elements[13],
                0.0,
                1.0,
            ];
        }

        // 2. Return matrix.
        create_dom_matrix(vm, Matrix4::from_column_major(elements), is_2d)
    }

    pub(crate) fn create_style_value(&self, property_id: u16) -> RetainedStyleValue {
        // NB: The elements are m11, m12, m13, m14, m21, ..., m44, and the elements a to f of a 2D matrix are m11, m12,
        //     m21, m22, m41 and m42.
        let elements = self.elements();
        if self.transform_component.raw_is_2d() {
            let values = [0, 1, 4, 5, 12, 13]
                .into_iter()
                .map(|index| NumericValue::Number(elements[index]).style_value())
                .collect();
            return transformation_style_value(property_id, TransformFunction::Matrix, values);
        }

        let values = elements
            .into_iter()
            .map(|element| NumericValue::Number(element).style_value())
            .collect();
        transformation_style_value(property_id, TransformFunction::Matrix3d, values)
    }
}
