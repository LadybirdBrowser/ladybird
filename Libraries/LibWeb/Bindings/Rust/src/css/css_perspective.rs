/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSPerspective, https://drafts.css-houdini.org/css-typed-om-1/#cssperspective

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, Trace, Vm};
use libweb_rust::typed_om::{BASE_TYPE_LENGTH, RetainedStyleValue, TransformFunction, transformation_style_value};

use super::css_keyword_value::{CSSKeywordValue, CSSKeywordish, rectify_a_keywordish_value};
use super::css_numeric_value::{AnyCSSNumericValue, SerializationParams};
use super::css_style_value::AnyCSSStyleValue;
use super::css_transform_component::{
    AnyCSSTransformComponent, CSSTransformComponent, Is2D, argument_style_value, create_dom_matrix,
};
use super::typed_om;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::geometry::dom_matrix::DOMMatrix;
use crate::geometry::matrix::Matrix4;

// https://drafts.css-houdini.org/css-typed-om-1/#typedefdef-cssperspectivevalue
// NB: CSSKeywordish is flattened here, because our bindings generator flattens nested variants.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum CSSPerspectiveValue {
    CSSNumericValue(AnyCSSNumericValue),
    DOMString(#[gc(untraced)] Utf16String),
    CSSKeywordValue(Ref<CSSKeywordValue>),
}

#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum CSSPerspectiveValueInternal {
    CSSNumericValue(AnyCSSNumericValue),
    CSSKeywordValue(Ref<CSSKeywordValue>),
}

fn to_internal(vm: &Vm, value: CSSPerspectiveValue) -> WebIdlResult<CSSPerspectiveValueInternal> {
    // Steps 1 and 2 of The CSSPerspective(length) constructor:
    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssperspective-cssperspective
    let keywordish = match value {
        // 1. If length is a CSSNumericValue:
        CSSPerspectiveValue::CSSNumericValue(numeric_value) => {
            // 1. If length does not match <length>, throw a TypeError.
            if !numeric_value.numeric_type().matches_dimension(BASE_TYPE_LENGTH, None) {
                return Err(Exception::type_error(
                    "CSSPerspective length component doesn't match <length>",
                ));
            }
            return Ok(CSSPerspectiveValueInternal::CSSNumericValue(numeric_value));
        }
        // 2. Otherwise (that is, if length is not a CSSNumericValue):
        CSSPerspectiveValue::DOMString(value) => CSSKeywordish::DOMString(value),
        CSSPerspectiveValue::CSSKeywordValue(value) => CSSKeywordish::CSSKeywordValue(value),
    };

    // 1. Rectify a keywordish value from length, then set length to the result’s value.
    let rectified_length = rectify_a_keywordish_value(vm, &keywordish);

    // 2. If length does not represent a value that is an ASCII case-insensitive match for the keyword none,
    //    throw a TypeError.
    if !typed_om::to_rust_string(&rectified_length.raw_value()).eq_ignore_ascii_case("none") {
        return Err(Exception::type_error(
            "CSSPerspective length component is a keyword other than `none`",
        ));
    }

    Ok(CSSPerspectiveValueInternal::CSSKeywordValue(rectified_length))
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSPerspective {
    transform_component: CSSTransformComponent,
    length: GcRefCell<CSSPerspectiveValueInternal>,
}

impl Wrappable for CSSPerspective {}

impl Inherits<CSSTransformComponent> for CSSPerspective {
    fn parent(&self) -> &CSSTransformComponent {
        &self.transform_component
    }
}

impl CSSPerspective {
    pub(crate) fn create(vm: &Vm, length: CSSPerspectiveValueInternal) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            transform_component: CSSTransformComponent::new(Is2D::No),
            length: GcRefCell::new(length),
        });
        AnyCSSTransformComponent::CSSPerspective(object).finish(vm);
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssperspective-cssperspective
    pub(crate) fn construct(cx: &mut CallContext, length: CSSPerspectiveValue) -> WebIdlResult<Ref<Self>> {
        // The CSSPerspective(length) constructor must, when invoked, perform the following steps:
        // NB: Steps 1 and 2 are implemented in to_internal().
        let vm = cx.vm();
        let internal_length = to_internal(vm, length)?;

        // 3. Return a new CSSPerspective object with its length internal slot set to length, and its is2D internal slot
        //    set to false.
        Ok(Self::create(vm, internal_length))
    }

    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<CSSPerspectiveValue> {
        Ok(match *self.length.borrow() {
            CSSPerspectiveValueInternal::CSSNumericValue(value) => CSSPerspectiveValue::CSSNumericValue(value),
            CSSPerspectiveValueInternal::CSSKeywordValue(value) => CSSPerspectiveValue::CSSKeywordValue(value),
        })
    }

    pub(crate) fn set_length(&self, cx: &mut CallContext, value: CSSPerspectiveValue) -> WebIdlResult<()> {
        // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
        //         WPT expects this to throw for invalid values, so just reuse the constructor code.
        let vm = cx.vm();
        let length = to_internal(vm, value)?;
        *self.length.borrow_mut(vm) = length;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssperspective
    pub(crate) fn serialize(&self) -> Utf16String {
        // 1. Let s initially be "perspective(".
        let mut s = String::from("perspective(");

        // 2. Serialize this’s length internal slot, with a minimum of 0px, and append it to s.
        match *self.length.borrow() {
            CSSPerspectiveValueInternal::CSSNumericValue(numeric_value) => numeric_value.serialize(
                &mut s,
                SerializationParams {
                    minimum: Some(0.0),
                    ..SerializationParams::default()
                },
            ),
            CSSPerspectiveValueInternal::CSSKeywordValue(keyword_value) => {
                s.push_str(&typed_om::to_rust_string(&keyword_value.serialize()));
            }
        }

        // 3. Append ")" to s, and return s.
        s.push(')');
        Utf16String::from_utf8(&s)
    }

    pub(crate) fn to_matrix(&self, vm: &Vm) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let matrix be a new DOMMatrix object, initialized to this’s equivalent 4x4 transform matrix, as defined in
        //    CSS Transforms 1 § 12. Mathematical Description of Transform Functions, and with its is2D internal slot
        //    set to the same value as this’s is2D internal slot.
        //    NOTE: Recall that the is2D flag affects what transform, and thus what equivalent matrix, a
        //          CSSTransformComponent represents.
        //    As the entries of such a matrix are defined relative to the px unit, if any <length>s in this involved in
        //    generating the matrix are not compatible units with px (such as relative lengths or percentages), throw a
        //    TypeError.
        let mut matrix = Matrix4::identity();

        match *self.length.borrow() {
            CSSPerspectiveValueInternal::CSSNumericValue(numeric_value) => {
                // NB: to_unit() throws a TypeError if the conversion can't be done.
                let distance = numeric_value.to_unit(vm, "px")?.raw_value();
                // NB: Matrix4 is row by row, so the element m34 is in the fourth row and the third column.
                //     Unlike f64::max(), this keeps a distance that is NaN.
                matrix.elements[3][2] = -1.0 / if distance < 1.0 { 1.0 } else { distance };
            }
            CSSPerspectiveValueInternal::CSSKeywordValue(_) => {
                // NB: This is `none`, so do nothing.
            }
        }

        Ok(create_dom_matrix(vm, matrix, self.transform_component.raw_is_2d()))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssperspective-is2d
    pub(crate) fn set_is_2d(&self) {
        // The is2D attribute of a CSSPerspective object must, on setting, do nothing.
    }

    pub(crate) fn create_style_value(&self, vm: &Vm, property_id: u16) -> WebIdlResult<RetainedStyleValue> {
        let length = match *self.length.borrow() {
            CSSPerspectiveValueInternal::CSSNumericValue(value) => argument_style_value(vm, property_id, value)?,
            CSSPerspectiveValueInternal::CSSKeywordValue(value) => {
                argument_style_value(vm, property_id, AnyCSSStyleValue::CSSKeywordValue(value))?
            }
        };
        Ok(transformation_style_value(
            property_id,
            TransformFunction::Perspective,
            vec![length],
        ))
    }
}
