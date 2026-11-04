/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSSkewX, https://drafts.css-houdini.org/css-typed-om-1/#cssskewx

use ak::{Utf16String, math};
use libjs_rust::api::{GcRefCell, Trace, Vm};
use libweb_rust::typed_om::{RetainedStyleValue, TransformFunction, transformation_style_value};

use super::css_numeric_value::{AnyCSSNumericValue, SerializationParams};
use super::css_transform_component::{
    AnyCSSTransformComponent, CSSTransformComponent, Is2D, argument_style_value, create_dom_matrix, matches_angle,
};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::geometry::dom_matrix::DOMMatrix;
use crate::geometry::matrix::Matrix4;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSSkewX {
    transform_component: CSSTransformComponent,
    ax: GcRefCell<AnyCSSNumericValue>,
}

impl Wrappable for CSSSkewX {}

impl Inherits<CSSTransformComponent> for CSSSkewX {
    fn parent(&self) -> &CSSTransformComponent {
        &self.transform_component
    }
}

impl CSSSkewX {
    pub(crate) fn create(vm: &Vm, ax: AnyCSSNumericValue) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            transform_component: CSSTransformComponent::new(Is2D::Yes),
            ax: GcRefCell::new(ax),
        });
        AnyCSSTransformComponent::CSSSkewX(object).finish(vm);
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskewx-cssskewx
    pub(crate) fn construct(cx: &mut CallContext, ax: AnyCSSNumericValue) -> WebIdlResult<Ref<Self>> {
        // The CSSSkewX(ax) constructor must, when invoked, perform the following steps:

        // 1. If ax does not match <angle>, throw a TypeError.
        if !matches_angle(&ax) {
            return Err(Exception::type_error("CSSSkewX ax component doesn't match <angle>"));
        }

        // 2. Return a new CSSSkewX object with its ax internal slot set to ax, and its is2D internal slot set to true.
        Ok(Self::create(cx.vm(), ax))
    }

    pub(crate) fn ax(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        Ok(*self.ax.borrow())
    }

    pub(crate) fn set_ax(&self, cx: &mut CallContext, ax: AnyCSSNumericValue) -> WebIdlResult<()> {
        // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
        //         WPT expects this to throw for invalid values.
        if !matches_angle(&ax) {
            return Err(Exception::type_error("CSSSkewX ax component doesn't match <angle>"));
        }
        *self.ax.borrow_mut(cx.vm()) = ax;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssskewx
    pub(crate) fn serialize(&self) -> Utf16String {
        // 1. Let s initially be "skewX(".
        let mut s = String::from("skewX(");

        // 2. Serialize this’s ax internal slot, and append it to s.
        self.ax.borrow().serialize(&mut s, SerializationParams::default());

        // 3. Append ")" to s, and return s.
        s.push(')');
        Utf16String::from_utf8(&s)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformcomponent-tomatrix
    pub(crate) fn to_matrix(&self, vm: &Vm) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let matrix be a new DOMMatrix object, initialized to this’s equivalent 4x4 transform matrix, as defined in
        //    CSS Transforms 1 § 12. Mathematical Description of Transform Functions, and with its is2D internal slot
        //    set to the same value as this’s is2D internal slot.
        //    NOTE: Recall that the is2D flag affects what transform, and thus what equivalent matrix, a
        //          CSSTransformComponent represents.
        //    As the entries of such a matrix are defined relative to the px unit, if any <length>s in this involved in
        //    generating the matrix are not compatible units with px (such as relative lengths or percentages), throw a
        //    TypeError.
        let ax_rad = self.ax.borrow().to_unit(vm, "rad")?.raw_value();

        // NB: Matrix4 is row by row, so the element m21 is in the first row.
        let mut matrix = Matrix4::identity();
        matrix.elements[0][1] = f64::from(math::tan(ax_rad as f32));

        Ok(create_dom_matrix(vm, matrix, self.transform_component.raw_is_2d()))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskew-is2d
    pub(crate) fn set_is_2d(&self) {
        // The is2D attribute of a CSSSkew, CSSSkewX, or CSSSkewY object must, on setting, do nothing.
    }

    pub(crate) fn create_style_value(&self, vm: &Vm, property_id: u16) -> WebIdlResult<RetainedStyleValue> {
        let ax = argument_style_value(vm, property_id, *self.ax.borrow())?;
        Ok(transformation_style_value(
            property_id,
            TransformFunction::SkewX,
            vec![ax],
        ))
    }
}
