/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSTranslate, https://drafts.css-houdini.org/css-typed-om-1/#csstranslate

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, Trace, Vm};
use libweb_rust::typed_om::{BASE_TYPE_LENGTH, RetainedStyleValue, TransformFunction, transformation_style_value};

use super::css_numeric_value::{AnyCSSNumericValue, SerializationParams};
use super::css_transform_component::{
    AnyCSSTransformComponent, CSSTransformComponent, Is2D, argument_style_value, create_dom_matrix,
};
use super::css_unit_value::CSSUnitValue;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::geometry::dom_matrix::DOMMatrix;
use crate::geometry::matrix::Matrix4;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSTranslate {
    transform_component: CSSTransformComponent,
    x: GcRefCell<AnyCSSNumericValue>,
    y: GcRefCell<AnyCSSNumericValue>,
    z: GcRefCell<AnyCSSNumericValue>,
}

impl Wrappable for CSSTranslate {}

impl Inherits<CSSTransformComponent> for CSSTranslate {
    fn parent(&self) -> &CSSTransformComponent {
        &self.transform_component
    }
}

fn matches_length_percentage(value: &AnyCSSNumericValue) -> bool {
    value
        .numeric_type()
        .matches_dimension_percentage(BASE_TYPE_LENGTH, None)
}

fn matches_length(value: &AnyCSSNumericValue) -> bool {
    value.numeric_type().matches_dimension(BASE_TYPE_LENGTH, None)
}

impl CSSTranslate {
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
        AnyCSSTransformComponent::CSSTranslate(object).finish(vm);
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csstranslate-csstranslate
    pub(crate) fn construct(
        cx: &mut CallContext,
        x: AnyCSSNumericValue,
        y: AnyCSSNumericValue,
        z: Option<AnyCSSNumericValue>,
    ) -> WebIdlResult<Ref<Self>> {
        // The CSSTranslate(x, y, z) constructor must, when invoked, perform the following steps:

        // 1. If x or y don’t match <length-percentage>, throw a TypeError.
        if !matches_length_percentage(&x) {
            return Err(Exception::type_error(
                "CSSTranslate x component doesn't match <length-percentage>",
            ));
        }

        if !matches_length_percentage(&y) {
            return Err(Exception::type_error(
                "CSSTranslate y component doesn't match <length-percentage>",
            ));
        }

        // 2. If z was passed, but doesn’t match <length>, throw a TypeError.
        if z.as_ref().is_some_and(|z| !matches_length(z)) {
            return Err(Exception::type_error("CSSTranslate z component doesn't match <length>"));
        }

        // 3. Let this be a new CSSTranslate object, with its x and y internal slots set to x and y.
        // 4. If z was passed, set this’s z internal slot to z, and set this’s is2D internal slot to false.
        // 5. If z was not passed, set this’s z internal slot to a new unit value of (0, "px"), and set this’s is2D
        //    internal slot to true.
        let vm = cx.vm();
        let (is_2d, z) = match z {
            Some(z) => (Is2D::No, z),
            None => (
                Is2D::Yes,
                AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(vm, 0.0, "px")),
            ),
        };
        let this = Self::create(vm, is_2d, x, y, z);

        // 6. Return this.
        Ok(this)
    }

    pub(crate) fn x(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        Ok(*self.x.borrow())
    }

    pub(crate) fn y(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        Ok(*self.y.borrow())
    }

    pub(crate) fn z(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        Ok(*self.z.borrow())
    }

    pub(crate) fn set_x(&self, cx: &mut CallContext, x: AnyCSSNumericValue) -> WebIdlResult<()> {
        // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
        //         WPT expects this to throw for invalid values.
        if !matches_length_percentage(&x) {
            return Err(Exception::type_error(
                "CSSTranslate x component doesn't match <length-percentage>",
            ));
        }
        *self.x.borrow_mut(cx.vm()) = x;
        Ok(())
    }

    pub(crate) fn set_y(&self, cx: &mut CallContext, y: AnyCSSNumericValue) -> WebIdlResult<()> {
        // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
        //         WPT expects this to throw for invalid values.
        if !matches_length_percentage(&y) {
            return Err(Exception::type_error(
                "CSSTranslate y component doesn't match <length-percentage>",
            ));
        }
        *self.y.borrow_mut(cx.vm()) = y;
        Ok(())
    }

    pub(crate) fn set_z(&self, cx: &mut CallContext, z: AnyCSSNumericValue) -> WebIdlResult<()> {
        // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
        //         WPT expects this to throw for invalid values.
        if !matches_length(&z) {
            return Err(Exception::type_error("CSSTranslate z component doesn't match <length>"));
        }
        *self.z.borrow_mut(cx.vm()) = z;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-csstranslate
    pub(crate) fn serialize(&self) -> Utf16String {
        let params = SerializationParams::default();

        // 1. Let s initially be the empty string.
        let mut s = String::new();

        // 2. If this’s is2D internal slot is false:
        if !self.transform_component.raw_is_2d() {
            // 1. Append "translate3d(" to s.
            s.push_str("translate3d(");

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
            // 1. Append "translate(" to s.
            s.push_str("translate(");

            // 2. Serialize this’s x internal slot, and append it to s.
            self.x.borrow().serialize(&mut s, params);

            // 3. Append ", " to s.
            s.push_str(", ");

            // 4. Serialize this’s y internal slot, and append it to s.
            self.y.borrow().serialize(&mut s, params);

            // 5. Append ")" to s, and return s.
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
        let is_2d = self.transform_component.raw_is_2d();
        let x = self.x.borrow().to_unit(vm, "px")?.raw_value();
        let y = self.y.borrow().to_unit(vm, "px")?.raw_value();

        let mut z = 0.0;
        if !is_2d {
            z = self.z.borrow().to_unit(vm, "px")?.raw_value();
        }

        Ok(create_dom_matrix(vm, Matrix4::translation(x, y, z), is_2d))
    }

    pub(crate) fn create_style_value(&self, vm: &Vm, property_id: u16) -> WebIdlResult<RetainedStyleValue> {
        let x = argument_style_value(vm, property_id, *self.x.borrow())?;
        let y = argument_style_value(vm, property_id, *self.y.borrow())?;
        if self.transform_component.raw_is_2d() {
            return Ok(transformation_style_value(
                property_id,
                TransformFunction::Translate,
                vec![x, y],
            ));
        }

        let z = argument_style_value(vm, property_id, *self.z.borrow())?;
        Ok(transformation_style_value(
            property_id,
            TransformFunction::Translate3d,
            vec![x, y, z],
        ))
    }
}
