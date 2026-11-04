/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSSkew, https://drafts.css-houdini.org/css-typed-om-1/#cssskew

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
pub(crate) struct CSSSkew {
    transform_component: CSSTransformComponent,
    ax: GcRefCell<AnyCSSNumericValue>,
    ay: GcRefCell<AnyCSSNumericValue>,
}

impl Wrappable for CSSSkew {}

impl Inherits<CSSTransformComponent> for CSSSkew {
    fn parent(&self) -> &CSSTransformComponent {
        &self.transform_component
    }
}

impl CSSSkew {
    pub(crate) fn create(vm: &Vm, ax: AnyCSSNumericValue, ay: AnyCSSNumericValue) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            transform_component: CSSTransformComponent::new(Is2D::Yes),
            ax: GcRefCell::new(ax),
            ay: GcRefCell::new(ay),
        });
        AnyCSSTransformComponent::CSSSkew(object).finish(vm);
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskew-cssskew
    pub(crate) fn construct(
        cx: &mut CallContext,
        ax: AnyCSSNumericValue,
        ay: AnyCSSNumericValue,
    ) -> WebIdlResult<Ref<Self>> {
        // The CSSSkew(ax, ay) constructor must, when invoked, perform the following steps:

        // 1. If ax or ay do not match <angle>, throw a TypeError.
        if !matches_angle(&ax) {
            return Err(Exception::type_error("CSSSkew ax component doesn't match <angle>"));
        }
        if !matches_angle(&ay) {
            return Err(Exception::type_error("CSSSkew ay component doesn't match <angle>"));
        }

        // 2. Return a new CSSSkew object with its ax and ay internal slots set to ax and ay, and its is2D internal slot
        //    set to true.
        Ok(Self::create(cx.vm(), ax, ay))
    }

    pub(crate) fn ax(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        Ok(*self.ax.borrow())
    }

    pub(crate) fn ay(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        Ok(*self.ay.borrow())
    }

    pub(crate) fn set_ax(&self, cx: &mut CallContext, ax: AnyCSSNumericValue) -> WebIdlResult<()> {
        // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
        //         WPT expects this to throw for invalid values.
        if !matches_angle(&ax) {
            return Err(Exception::type_error("CSSSkew ax component doesn't match <angle>"));
        }
        *self.ax.borrow_mut(cx.vm()) = ax;
        Ok(())
    }

    pub(crate) fn set_ay(&self, cx: &mut CallContext, ay: AnyCSSNumericValue) -> WebIdlResult<()> {
        // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
        //         WPT expects this to throw for invalid values.
        if !matches_angle(&ay) {
            return Err(Exception::type_error("CSSSkew ay component doesn't match <angle>"));
        }
        *self.ay.borrow_mut(cx.vm()) = ay;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssskew
    pub(crate) fn serialize(&self) -> Utf16String {
        let params = SerializationParams::default();

        // 1. Let s initially be "skew(".
        let mut s = String::from("skew(");

        // 2. Serialize this’s ax internal slot, and append it to s.
        self.ax.borrow().serialize(&mut s, params);

        // 3. If this’s ay internal slot is a CSSUnitValue with a value of 0, then append ")" to s and return s.
        let ay = *self.ay.borrow();
        if ay.as_unit_value().is_some_and(|ay| ay.raw_value() == 0.0) {
            s.push(')');
            return Utf16String::from_utf8(&s);
        }

        // 4. Otherwise, append ", " to s.
        s.push_str(", ");

        // 5. Serialize this’s ay internal slot, and append it to s.
        ay.serialize(&mut s, params);

        // 6. Append ")" to s, and return s.
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
        let ay_rad = self.ay.borrow().to_unit(vm, "rad")?.raw_value();

        // NB: Matrix4 is row by row, so the element m21 is in the first row and m12 in the second.
        let mut matrix = Matrix4::identity();
        matrix.elements[0][1] = f64::from(math::tan(ax_rad as f32));
        matrix.elements[1][0] = f64::from(math::tan(ay_rad as f32));

        Ok(create_dom_matrix(vm, matrix, self.transform_component.raw_is_2d()))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskew-is2d
    pub(crate) fn set_is_2d(&self) {
        // The is2D attribute of a CSSSkew, CSSSkewX, or CSSSkewY object must, on setting, do nothing.
    }

    pub(crate) fn create_style_value(&self, vm: &Vm, property_id: u16) -> WebIdlResult<RetainedStyleValue> {
        let ax = argument_style_value(vm, property_id, *self.ax.borrow())?;
        let ay = argument_style_value(vm, property_id, *self.ay.borrow())?;
        Ok(transformation_style_value(
            property_id,
            TransformFunction::Skew,
            vec![ax, ay],
        ))
    }
}
