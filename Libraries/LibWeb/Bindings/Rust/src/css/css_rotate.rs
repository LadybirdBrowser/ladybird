/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSRotate, https://drafts.css-houdini.org/css-typed-om-1/#cssrotate

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, Trace, Vm};
use libweb_rust::typed_om::{RetainedStyleValue, TransformFunction, transformation_style_value};

use super::css_numeric_value::{AnyCSSNumericValue, CSSNumberish, SerializationParams, rectify_a_numberish_value};
use super::css_transform_component::{
    AnyCSSTransformComponent, CSSTransformComponent, Is2D, argument_style_value, create_dom_matrix, matches_angle,
    matches_number,
};
use super::css_unit_value::CSSUnitValue;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::geometry::dom_matrix::DOMMatrix;
use crate::geometry::dom_matrix_read_only::DOMMatrixReadOnly;
use crate::geometry::matrix::Matrix4;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSRotate {
    transform_component: CSSTransformComponent,
    x: GcRefCell<AnyCSSNumericValue>,
    y: GcRefCell<AnyCSSNumericValue>,
    z: GcRefCell<AnyCSSNumericValue>,
    angle: GcRefCell<AnyCSSNumericValue>,
}

impl Wrappable for CSSRotate {}

impl Inherits<CSSTransformComponent> for CSSRotate {
    fn parent(&self) -> &CSSTransformComponent {
        &self.transform_component
    }
}

impl CSSRotate {
    pub(crate) fn create(
        vm: &Vm,
        is_2d: Is2D,
        x: AnyCSSNumericValue,
        y: AnyCSSNumericValue,
        z: AnyCSSNumericValue,
        angle: AnyCSSNumericValue,
    ) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            transform_component: CSSTransformComponent::new(is_2d),
            x: GcRefCell::new(x),
            y: GcRefCell::new(y),
            z: GcRefCell::new(z),
            angle: GcRefCell::new(angle),
        });
        AnyCSSTransformComponent::CSSRotate(object).finish(vm);
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-cssrotate
    pub(crate) fn construct_from_angle(cx: &mut CallContext, angle: AnyCSSNumericValue) -> WebIdlResult<Ref<Self>> {
        // The CSSRotate(angle) constructor must, when invoked, perform the following steps:

        // 1. If angle doesn’t match <angle>, throw a TypeError.
        if !matches_angle(&angle) {
            return Err(Exception::type_error("CSSRotate angle component doesn't match <angle>"));
        }

        // 2. Return a new CSSRotate with its angle internal slot set to angle, its x and y internal slots set to new
        //    unit values of (0, "number"), its z internal slot set to a new unit value of (1, "number"), and its is2D
        //    internal slot set to true.
        let vm = cx.vm();
        let unit_value = |value| AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(vm, value, "number"));
        Ok(Self::create(
            vm,
            Is2D::Yes,
            unit_value(0.0),
            unit_value(0.0),
            unit_value(1.0),
            angle,
        ))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-cssrotate-x-y-z-anglec
    pub(crate) fn construct_from_axis(
        cx: &mut CallContext,
        x: CSSNumberish,
        y: CSSNumberish,
        z: CSSNumberish,
        angle: AnyCSSNumericValue,
    ) -> WebIdlResult<Ref<Self>> {
        // The CSSRotate(x, y, z, angle) constructor must, when invoked, perform the following steps:

        // 1. If angle doesn’t match <angle>, throw a TypeError.
        if !matches_angle(&angle) {
            return Err(Exception::type_error("CSSRotate angle component doesn't match <angle>"));
        }

        // 2. Let x, y, and z be replaced by the result of rectifying a numberish value.
        let vm = cx.vm();
        let rectified_x = rectify_a_numberish_value(vm, &x, None);
        let rectified_y = rectify_a_numberish_value(vm, &y, None);
        let rectified_z = rectify_a_numberish_value(vm, &z, None);

        // 3. If x, y, or z don’t match <number>, throw a TypeError.
        if !matches_number(&rectified_x) {
            return Err(Exception::type_error("CSSRotate x component doesn't match <number>"));
        }
        if !matches_number(&rectified_y) {
            return Err(Exception::type_error("CSSRotate y component doesn't match <number>"));
        }
        if !matches_number(&rectified_z) {
            return Err(Exception::type_error("CSSRotate z component doesn't match <number>"));
        }

        // 4. Return a new CSSRotate with its angle internal slot set to angle, its x, y, z internal slots set to x, y,
        //    and z, and its is2D internal slot set to false.
        Ok(Self::create(vm, Is2D::No, rectified_x, rectified_y, rectified_z, angle))
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

    pub(crate) fn angle(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        Ok(*self.angle.borrow())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-x
    pub(crate) fn set_x(&self, cx: &mut CallContext, value: CSSNumberish) -> WebIdlResult<()> {
        // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set
        // the corresponding internal slot to the result of that.
        // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
        let vm = cx.vm();
        let rectified_x = rectify_a_numberish_value(vm, &value, None);
        if !matches_number(&rectified_x) {
            return Err(Exception::type_error("CSSRotate x component doesn't match <number>"));
        }
        *self.x.borrow_mut(vm) = rectified_x;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-y
    pub(crate) fn set_y(&self, cx: &mut CallContext, value: CSSNumberish) -> WebIdlResult<()> {
        // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set
        // the corresponding internal slot to the result of that.
        // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
        let vm = cx.vm();
        let rectified_y = rectify_a_numberish_value(vm, &value, None);
        if !matches_number(&rectified_y) {
            return Err(Exception::type_error("CSSRotate y component doesn't match <number>"));
        }
        *self.y.borrow_mut(vm) = rectified_y;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-z
    pub(crate) fn set_z(&self, cx: &mut CallContext, value: CSSNumberish) -> WebIdlResult<()> {
        // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set
        // the corresponding internal slot to the result of that.
        // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
        let vm = cx.vm();
        let rectified_z = rectify_a_numberish_value(vm, &value, None);
        if !matches_number(&rectified_z) {
            return Err(Exception::type_error("CSSRotate z component doesn't match <number>"));
        }
        *self.z.borrow_mut(vm) = rectified_z;
        Ok(())
    }

    pub(crate) fn set_angle(&self, cx: &mut CallContext, value: AnyCSSNumericValue) -> WebIdlResult<()> {
        // AD-HOC: Not specced. WPT expects this to throw for invalid values.
        //         https://github.com/w3c/css-houdini-drafts/issues/1153
        if !matches_angle(&value) {
            return Err(Exception::type_error("CSSRotate angle component doesn't match <angle>"));
        }
        *self.angle.borrow_mut(cx.vm()) = value;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssrotate
    pub(crate) fn serialize(&self) -> Utf16String {
        let params = SerializationParams::default();

        // 1. Let s initially be the empty string.
        let mut s = String::new();

        // 2. If this’s is2D internal slot is false:
        if !self.transform_component.raw_is_2d() {
            // 1. Append "rotate3d(" to s.
            s.push_str("rotate3d(");

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

            // 7. Append "," to s.
            s.push_str(", ");

            // 8. Serialize this’s angle internal slot, and append it to s.
            self.angle.borrow().serialize(&mut s, params);

            // 9. Append ")" to s, and return s.
            s.push(')');
        }
        // 2. Otherwise:
        else {
            // 1. Append "rotate(" to s.
            s.push_str("rotate(");

            // 2. Serialize this’s angle internal slot, and append it to s.
            self.angle.borrow().serialize(&mut s, params);

            // 3. Append ")" to s, and return s.
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

        let is_2d = self.transform_component.raw_is_2d();
        let matrix = create_dom_matrix(vm, Matrix4::identity(), is_2d);
        let read_only_matrix = <DOMMatrix as Inherits<DOMMatrixReadOnly>>::parent(&matrix);

        // NB: to_unit() throws a TypeError if the conversion can't be done.
        let angle = self.angle.borrow().to_unit(vm, "deg")?.raw_value();

        if is_2d {
            read_only_matrix.rotate_axis_angle_self_steps(0.0, 0.0, 1.0, angle);
            return Ok(matrix);
        }

        let x = self.x.borrow().to_unit(vm, "number")?.raw_value();
        let y = self.y.borrow().to_unit(vm, "number")?.raw_value();
        let z = self.z.borrow().to_unit(vm, "number")?.raw_value();

        read_only_matrix.rotate_axis_angle_self_steps(x, y, z, angle);
        Ok(matrix)
    }

    pub(crate) fn create_style_value(&self, vm: &Vm, property_id: u16) -> WebIdlResult<RetainedStyleValue> {
        if self.transform_component.raw_is_2d() {
            let angle = argument_style_value(vm, property_id, *self.angle.borrow())?;
            return Ok(transformation_style_value(
                property_id,
                TransformFunction::Rotate,
                vec![angle],
            ));
        }

        let x = argument_style_value(vm, property_id, *self.x.borrow())?;
        let y = argument_style_value(vm, property_id, *self.y.borrow())?;
        let z = argument_style_value(vm, property_id, *self.z.borrow())?;
        let angle = argument_style_value(vm, property_id, *self.angle.borrow())?;
        Ok(transformation_style_value(
            property_id,
            TransformFunction::Rotate3d,
            vec![x, y, z, angle],
        ))
    }
}
