/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSScale, https://drafts.css-houdini.org/css-typed-om-1/#cssscale

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, Trace, Vm};
use libweb_rust::typed_om::{RetainedStyleValue, TransformFunction, transformation_style_value};

use super::css_numeric_value::{AnyCSSNumericValue, CSSNumberish, SerializationParams, rectify_a_numberish_value};
use super::css_transform_component::{
    AnyCSSTransformComponent, CSSTransformComponent, Is2D, argument_style_value, create_dom_matrix, matches_number,
};
use super::css_unit_value::CSSUnitValue;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::geometry::dom_matrix::DOMMatrix;
use crate::geometry::matrix::Matrix4;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSScale {
    transform_component: CSSTransformComponent,
    x: GcRefCell<AnyCSSNumericValue>,
    y: GcRefCell<AnyCSSNumericValue>,
    z: GcRefCell<AnyCSSNumericValue>,
}

impl Wrappable for CSSScale {}

impl Inherits<CSSTransformComponent> for CSSScale {
    fn parent(&self) -> &CSSTransformComponent {
        &self.transform_component
    }
}

impl CSSScale {
    pub(crate) fn create(
        vm: &Vm,
        is_2d: Is2D,
        x: AnyCSSNumericValue,
        y: AnyCSSNumericValue,
        z: AnyCSSNumericValue,
    ) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            transform_component: CSSTransformComponent::new(is_2d),
            x: GcRefCell::new(x),
            y: GcRefCell::new(y),
            z: GcRefCell::new(z),
        });
        AnyCSSTransformComponent::CSSScale(object).finish(vm);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        x: CSSNumberish,
        y: CSSNumberish,
        z: Option<CSSNumberish>,
    ) -> WebIdlResult<Ref<Self>> {
        // The CSSScale(x, y, z) constructor must, when invoked, perform the following steps:

        // 1. Let x, y, and z (if passed) be replaced by the result of rectifying a numberish value.
        let vm = cx.vm();
        let rectified_x = rectify_a_numberish_value(vm, &x, None);
        let rectified_y = rectify_a_numberish_value(vm, &y, None);
        let rectified_z = z.map(|z| rectify_a_numberish_value(vm, &z, None));

        // 2. If x, y, or z (if passed) don’t match <number>, throw a TypeError.
        if !matches_number(&rectified_x) {
            return Err(Exception::type_error("CSSScale x component doesn't match <number>"));
        }
        if !matches_number(&rectified_y) {
            return Err(Exception::type_error("CSSScale y component doesn't match <number>"));
        }
        if rectified_z.as_ref().is_some_and(|z| !matches_number(z)) {
            return Err(Exception::type_error("CSSScale z component doesn't match <number>"));
        }

        // 3. Let this be a new CSSScale object, with its x and y internal slots set to x and y.
        // 4. If z was passed, set this’s z internal slot to z, and set this’s is2D internal slot to false.
        // 5. If z was not passed, set this’s z internal slot to a new unit value of (1, "number"), and set this’s is2D
        //    internal slot to true.
        let (is_2d, rectified_z) = match rectified_z {
            Some(z) => (Is2D::No, z),
            None => (
                Is2D::Yes,
                AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(vm, 1.0, "number")),
            ),
        };
        let this = Self::create(vm, is_2d, rectified_x, rectified_y, rectified_z);

        // 6. Return this.
        Ok(this)
    }

    pub(crate) fn x(&self, _: &mut CallContext) -> WebIdlResult<CSSNumberish> {
        Ok(CSSNumberish::CSSNumericValue(*self.x.borrow()))
    }

    pub(crate) fn y(&self, _: &mut CallContext) -> WebIdlResult<CSSNumberish> {
        Ok(CSSNumberish::CSSNumericValue(*self.y.borrow()))
    }

    pub(crate) fn z(&self, _: &mut CallContext) -> WebIdlResult<CSSNumberish> {
        Ok(CSSNumberish::CSSNumericValue(*self.z.borrow()))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssscale-x
    pub(crate) fn set_x(&self, cx: &mut CallContext, value: CSSNumberish) -> WebIdlResult<()> {
        // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set
        // the corresponding internal slot to the result of that.
        // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
        let vm = cx.vm();
        let rectified_x = rectify_a_numberish_value(vm, &value, None);
        if !matches_number(&rectified_x) {
            return Err(Exception::type_error("CSSScale x component doesn't match <number>"));
        }
        *self.x.borrow_mut(vm) = rectified_x;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssscale-y
    pub(crate) fn set_y(&self, cx: &mut CallContext, value: CSSNumberish) -> WebIdlResult<()> {
        // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set
        // the corresponding internal slot to the result of that.
        // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
        let vm = cx.vm();
        let rectified_y = rectify_a_numberish_value(vm, &value, None);
        if !matches_number(&rectified_y) {
            return Err(Exception::type_error("CSSScale y component doesn't match <number>"));
        }
        *self.y.borrow_mut(vm) = rectified_y;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssscale-z
    pub(crate) fn set_z(&self, cx: &mut CallContext, value: CSSNumberish) -> WebIdlResult<()> {
        // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set
        // the corresponding internal slot to the result of that.
        // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
        let vm = cx.vm();
        let rectified_z = rectify_a_numberish_value(vm, &value, None);
        if !matches_number(&rectified_z) {
            return Err(Exception::type_error("CSSScale z component doesn't match <number>"));
        }
        *self.z.borrow_mut(vm) = rectified_z;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssscale
    pub(crate) fn serialize(&self) -> Utf16String {
        let params = SerializationParams::default();

        // 1. Let s initially be the empty string.
        let mut s = String::new();

        // 2. If this’s is2D internal slot is false:
        if !self.transform_component.raw_is_2d() {
            // 1. Append "scale3d(" to s.
            s.push_str("scale3d(");

            // 2. Serialize this’s x internal slot, and append it to s.
            self.x.borrow().serialize(&mut s, params);

            // 3. Append ", " to s.
            s.push_str(", ");

            // 4. Serialize this’s y internal slot, and append it to s.
            self.y.borrow().serialize(&mut s, params);

            // 5. Append ", " to s.
            s.push_str(", ");

            // 6. Serialize this’s z internal slot, and append it to s.
            self.z.borrow().serialize(&mut s, params);

            // 7. Append ")" to s, and return s.
            s.push(')');
        }
        // 3. Otherwise:
        else {
            // 1. Append "scale(" to s.
            s.push_str("scale(");

            // 2. Serialize this’s x internal slot, and append it to s.
            self.x.borrow().serialize(&mut s, params);

            // 3. If this’s x and y internal slots are equal numeric values, append ")" to s and return s.
            // AD-HOC: Don't do this - neither Chrome nor Safari show this behavior.
            //         Upstream issue: https://github.com/w3c/css-houdini-drafts/issues/1161

            // 4. Otherwise, append ", " to s.
            s.push_str(", ");

            // 5. Serialize this’s y internal slot, and append it to s.
            self.y.borrow().serialize(&mut s, params);

            // 6. Append ")" to s, and return s.
            s.push(')');
        }
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
        // 2. Return matrix.

        // NB: to_unit() throws a TypeError if the conversion can't be done.
        let is_2d = self.transform_component.raw_is_2d();
        let x = self.x.borrow().to_unit(vm, "number")?.raw_value();
        let y = self.y.borrow().to_unit(vm, "number")?.raw_value();

        let mut z = 1.0;
        if !is_2d {
            z = self.z.borrow().to_unit(vm, "number")?.raw_value();
        }

        Ok(create_dom_matrix(vm, Matrix4::scale(x, y, z), is_2d))
    }

    pub(crate) fn create_style_value(&self, vm: &Vm, property_id: u16) -> WebIdlResult<RetainedStyleValue> {
        let x = argument_style_value(vm, property_id, *self.x.borrow())?;
        let y = argument_style_value(vm, property_id, *self.y.borrow())?;
        if self.transform_component.raw_is_2d() {
            return Ok(transformation_style_value(
                property_id,
                TransformFunction::Scale,
                vec![x, y],
            ));
        }

        let z = argument_style_value(vm, property_id, *self.z.borrow())?;
        Ok(transformation_style_value(
            property_id,
            TransformFunction::Scale3d,
            vec![x, y, z],
        ))
    }
}
