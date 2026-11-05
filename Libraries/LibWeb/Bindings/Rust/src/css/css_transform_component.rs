/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSTransformComponent, https://drafts.css-houdini.org/css-typed-om-1/#csstransformcomponent

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, ThrowCompletionOr, Trace, Value, Vm};
use libweb_rust::typed_om::{
    BASE_TYPE_ANGLE, Reification, RetainedStyleValue, TransformFunction, reification_of, transform_matrix,
    transformation_parts,
};

use super::css_keyword_value::CSSKeywordValue;
use super::css_matrix_component::CSSMatrixComponent;
use super::css_numeric_value::AnyCSSNumericValue;
use super::css_perspective::{CSSPerspective, CSSPerspectiveValueInternal};
use super::css_rotate::CSSRotate;
use super::css_scale::CSSScale;
use super::css_skew::CSSSkew;
use super::css_skew_x::CSSSkewX;
use super::css_skew_y::CSSSkewY;
use super::css_style_value::{AnyCSSStyleValue, PerformTypeCheck};
use super::css_translate::CSSTranslate;
use super::css_unit_value::CSSUnitValue;
use super::typed_om;
use crate::bindings::conversions::ToJs;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, wrap};
use crate::bindings::{CallContext, WebIdlResult};
use crate::geometry::dom_matrix::DOMMatrix;
use crate::geometry::dom_matrix_read_only::DOMMatrixReadOnly;
use crate::geometry::matrix::Matrix4;

#[derive(Clone, Copy, Eq, PartialEq)]
pub(crate) enum Is2D {
    No,
    Yes,
}

/// An object of CSSTransformComponent or of an interface that inherits from it.
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum AnyCSSTransformComponent {
    CSSTransformComponent(Ref<CSSTransformComponent>),
    CSSMatrixComponent(Ref<CSSMatrixComponent>),
    CSSPerspective(Ref<CSSPerspective>),
    CSSRotate(Ref<CSSRotate>),
    CSSScale(Ref<CSSScale>),
    CSSSkew(Ref<CSSSkew>),
    CSSSkewX(Ref<CSSSkewX>),
    CSSSkewY(Ref<CSSSkewY>),
    CSSTranslate(Ref<CSSTranslate>),
}

impl Deref for AnyCSSTransformComponent {
    type Target = CSSTransformComponent;

    fn deref(&self) -> &CSSTransformComponent {
        match self {
            Self::CSSTransformComponent(object) => object,
            Self::CSSMatrixComponent(object) => <CSSMatrixComponent as Inherits<CSSTransformComponent>>::parent(object),
            Self::CSSPerspective(object) => <CSSPerspective as Inherits<CSSTransformComponent>>::parent(object),
            Self::CSSRotate(object) => <CSSRotate as Inherits<CSSTransformComponent>>::parent(object),
            Self::CSSScale(object) => <CSSScale as Inherits<CSSTransformComponent>>::parent(object),
            Self::CSSSkew(object) => <CSSSkew as Inherits<CSSTransformComponent>>::parent(object),
            Self::CSSSkewX(object) => <CSSSkewX as Inherits<CSSTransformComponent>>::parent(object),
            Self::CSSSkewY(object) => <CSSSkewY as Inherits<CSSTransformComponent>>::parent(object),
            Self::CSSTranslate(object) => <CSSTranslate as Inherits<CSSTransformComponent>>::parent(object),
        }
    }
}

impl ToJs for AnyCSSTransformComponent {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(match self {
            Self::CSSTransformComponent(object) => wrap(cx, object),
            Self::CSSMatrixComponent(object) => wrap(cx, object),
            Self::CSSPerspective(object) => wrap(cx, object),
            Self::CSSRotate(object) => wrap(cx, object),
            Self::CSSScale(object) => wrap(cx, object),
            Self::CSSSkew(object) => wrap(cx, object),
            Self::CSSSkewX(object) => wrap(cx, object),
            Self::CSSSkewY(object) => wrap(cx, object),
            Self::CSSTranslate(object) => wrap(cx, object),
        }))
    }
}

impl AnyCSSTransformComponent {
    /// Tells the part of the object that all CSSTransformComponents have which object it is the part of.
    pub(crate) fn finish(self, vm: &Vm) -> Self {
        self.set_object(vm, self);
        self
    }

    pub(crate) fn serialize(&self) -> WebIdlResult<Utf16String> {
        match self {
            Self::CSSTransformComponent(_) => unreachable!("no object is only of an abstract interface"),
            Self::CSSMatrixComponent(object) => object.serialize(),
            Self::CSSPerspective(object) => Ok(object.serialize()),
            Self::CSSRotate(object) => Ok(object.serialize()),
            Self::CSSScale(object) => Ok(object.serialize()),
            Self::CSSSkew(object) => Ok(object.serialize()),
            Self::CSSSkewX(object) => Ok(object.serialize()),
            Self::CSSSkewY(object) => Ok(object.serialize()),
            Self::CSSTranslate(object) => Ok(object.serialize()),
        }
    }

    pub(crate) fn to_matrix(self, vm: &Vm) -> WebIdlResult<Ref<DOMMatrix>> {
        match self {
            Self::CSSTransformComponent(_) => unreachable!("no object is only of an abstract interface"),
            Self::CSSMatrixComponent(object) => Ok(object.to_matrix(vm)),
            Self::CSSPerspective(object) => object.to_matrix(vm),
            Self::CSSRotate(object) => object.to_matrix(vm),
            Self::CSSScale(object) => object.to_matrix(vm),
            Self::CSSSkew(object) => object.to_matrix(vm),
            Self::CSSSkewX(object) => object.to_matrix(vm),
            Self::CSSSkewY(object) => object.to_matrix(vm),
            Self::CSSTranslate(object) => object.to_matrix(vm),
        }
    }

    pub(crate) fn create_style_value(&self, vm: &Vm, property_id: u16) -> WebIdlResult<RetainedStyleValue> {
        match self {
            Self::CSSTransformComponent(_) => unreachable!("no object is only of an abstract interface"),
            Self::CSSMatrixComponent(object) => Ok(object.create_style_value(property_id)),
            Self::CSSPerspective(object) => object.create_style_value(vm, property_id),
            Self::CSSRotate(object) => object.create_style_value(vm, property_id),
            Self::CSSScale(object) => object.create_style_value(vm, property_id),
            Self::CSSSkew(object) => object.create_style_value(vm, property_id),
            Self::CSSSkewX(object) => object.create_style_value(vm, property_id),
            Self::CSSSkewY(object) => object.create_style_value(vm, property_id),
            Self::CSSTranslate(object) => object.create_style_value(vm, property_id),
        }
    }

    fn set_is_2d_steps(&self, value: bool) {
        match self {
            Self::CSSPerspective(object) => object.set_is_2d(),
            Self::CSSSkew(object) => object.set_is_2d(),
            Self::CSSSkewX(object) => object.set_is_2d(),
            Self::CSSSkewY(object) => object.set_is_2d(),
            _ => self.is_2d.set(value),
        }
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#csstransformcomponent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSTransformComponent {
    #[gc(untraced)]
    is_2d: Cell<bool>,
    /// The object that this is the CSSTransformComponent part of, by which the steps of CSSTransformComponent reach the
    /// steps of the interface of the object.
    object: GcRefCell<Option<AnyCSSTransformComponent>>,
}

impl Wrappable for CSSTransformComponent {}

impl CSSTransformComponent {
    pub(crate) fn new(is_2d: Is2D) -> Self {
        Self {
            is_2d: Cell::new(is_2d == Is2D::Yes),
            object: GcRefCell::new(None),
        }
    }

    /// Tells the part which object it belongs to, which an object does as soon as it exists.
    pub(crate) fn set_object(&self, vm: &Vm, object: AnyCSSTransformComponent) {
        *self.object.borrow_mut(vm) = Some(object);
    }

    pub(crate) fn object(&self) -> AnyCSSTransformComponent {
        self.object.borrow().expect("an object exists from when it is created")
    }

    pub(crate) fn raw_is_2d(&self) -> bool {
        self.is_2d.get()
    }

    pub(crate) fn to_string(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        self.object().serialize()
    }

    pub(crate) fn is_2d(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.is_2d.get())
    }

    pub(crate) fn set_is_2d(&self, _: &mut CallContext, value: bool) -> WebIdlResult<()> {
        self.object().set_is_2d_steps(value);
        Ok(())
    }

    pub(crate) fn to_matrix(&self, cx: &mut CallContext) -> WebIdlResult<Ref<DOMMatrix>> {
        self.object().to_matrix(cx.vm())
    }
}

/// A new DOMMatrix with the elements of the matrix, whose is 2D is the value.
pub(crate) fn create_dom_matrix(vm: &Vm, matrix: Matrix4, is_2d: bool) -> Ref<DOMMatrix> {
    allocate(vm, |_| DOMMatrix::from_read_only(DOMMatrixReadOnly::new(matrix, is_2d)))
}

/// The internal representation of an argument of a transform function, for the property.
pub(crate) fn argument_style_value(
    vm: &Vm,
    property_id: u16,
    argument: impl Into<AnyCSSStyleValue>,
) -> WebIdlResult<RetainedStyleValue> {
    // NB: The name of the property is only for the errors of the type check, which does not happen here.
    argument
        .into()
        .create_an_internal_representation(vm, property_id, &Utf16String::default(), PerformTypeCheck::No)
}

pub(crate) fn matches_angle(value: &AnyCSSNumericValue) -> bool {
    value.numeric_type().matches_dimension(BASE_TYPE_ANGLE, None)
}

pub(crate) fn matches_number(value: &AnyCSSNumericValue) -> bool {
    value.numeric_type().matches_number(None)
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-transform-function
pub(crate) fn reify_a_transform_function(vm: &Vm, value: &RetainedStyleValue) -> Option<AnyCSSTransformComponent> {
    let (transform_function, values) = transformation_parts(value);

    // NB: An argument that does not reify to a numeric value gives up, like a function that is not reifiable.
    let reify_numeric_argument = |index: usize| typed_om::reify_a_numeric_value(vm, &values[index]);
    let reify_unit_value =
        |value: f64, unit: &str| AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(vm, value, unit));
    let reify_0 = || reify_unit_value(0.0, "number");
    let reify_1 = || reify_unit_value(1.0, "number");
    let reify_0px = || reify_unit_value(0.0, "px");
    let reify_0deg = || reify_unit_value(0.0, "deg");

    let translate = |is_2d, x, y, z| AnyCSSTransformComponent::CSSTranslate(CSSTranslate::create(vm, is_2d, x, y, z));
    let scale = |is_2d, x, y, z| AnyCSSTransformComponent::CSSScale(CSSScale::create(vm, is_2d, x, y, z));
    let rotate =
        |is_2d, x, y, z, angle| AnyCSSTransformComponent::CSSRotate(CSSRotate::create(vm, is_2d, x, y, z, angle));

    // To reify a <transform-function> func, perform the appropriate set of steps below, based on func:
    Some(match transform_function {
        // -> matrix()
        // -> matrix3d()
        //    1. Return a new CSSMatrixComponent object, whose matrix internal slot is set to a 4x4 matrix representing
        //       the same information as func, and whose is2D internal slot is true if func is matrix(), and false
        //       otherwise.
        TransformFunction::Matrix | TransformFunction::Matrix3d => {
            let transform_as_matrix = transform_matrix(value)?;

            // NB: The matrix is row by row, as Matrix4 stores it.
            let mut matrix = Matrix4::identity();
            for (index, element) in transform_as_matrix.into_iter().enumerate() {
                matrix.elements[index / 4][index % 4] = f64::from(element);
            }

            let is_2d = if transform_function == TransformFunction::Matrix {
                Is2D::Yes
            } else {
                Is2D::No
            };
            let dom_matrix = create_dom_matrix(vm, matrix, matrix.is_2d_affine());
            AnyCSSTransformComponent::CSSMatrixComponent(CSSMatrixComponent::create(vm, is_2d, dom_matrix))
        }

        // -> translate()
        // -> translateX()
        // -> translateY()
        // -> translate3d()
        // -> translateZ()
        //    1. Return a new CSSTranslate object, whose x, y, and z internal slots are set to the reification of the
        //       specified x/y/z offsets, or the reification of 0px if not specified in func, and whose is2D internal
        //       slot is true if func is translate(), translateX(), or translateY(), and false otherwise.
        TransformFunction::Translate => {
            // NB: Default y to 0px if it's not specified.
            let y = if values.len() > 1 {
                reify_numeric_argument(1)?
            } else {
                reify_0px()
            };
            translate(Is2D::Yes, reify_numeric_argument(0)?, y, reify_0px())
        }
        TransformFunction::TranslateX => translate(Is2D::Yes, reify_numeric_argument(0)?, reify_0px(), reify_0px()),
        TransformFunction::TranslateY => translate(Is2D::Yes, reify_0px(), reify_numeric_argument(0)?, reify_0px()),
        TransformFunction::Translate3d => translate(
            Is2D::No,
            reify_numeric_argument(0)?,
            reify_numeric_argument(1)?,
            reify_numeric_argument(2)?,
        ),
        TransformFunction::TranslateZ => translate(Is2D::No, reify_0px(), reify_0px(), reify_numeric_argument(0)?),

        // -> scale()
        // -> scaleX()
        // -> scaleY()
        // -> scale3d()
        // -> scaleZ()
        //    1. Return a new CSSScale object, whose x, y, and z internal slots are set to the specified x/y/z scales,
        //       or to 1 if not specified in func and whose is2D internal slot is true if func is scale(), scaleX(), or
        //       scaleY(), and false otherwise.
        TransformFunction::Scale => {
            // NB: Default y to a copy of x if it's not specified.
            let y = if values.len() > 1 {
                reify_numeric_argument(1)?
            } else {
                reify_numeric_argument(0)?
            };
            scale(Is2D::Yes, reify_numeric_argument(0)?, y, reify_1())
        }
        TransformFunction::ScaleX => scale(Is2D::Yes, reify_numeric_argument(0)?, reify_1(), reify_1()),
        TransformFunction::ScaleY => scale(Is2D::Yes, reify_1(), reify_numeric_argument(0)?, reify_1()),
        TransformFunction::Scale3d => scale(
            Is2D::No,
            reify_numeric_argument(0)?,
            reify_numeric_argument(1)?,
            reify_numeric_argument(2)?,
        ),
        TransformFunction::ScaleZ => scale(Is2D::No, reify_1(), reify_1(), reify_numeric_argument(0)?),

        // -> rotate()
        // -> rotate3d()
        // -> rotateX()
        // -> rotateY()
        // -> rotateZ()
        //    1. Return a new CSSRotate object, whose angle internal slot is set to the reification of the specified
        //       angle, and whose x, y, and z internal slots are set to the specified rotation axis coordinates, or the
        //       implicit axis coordinates if not specified in func and whose is2D internal slot is true if func is
        //       rotate(), and false otherwise.
        TransformFunction::Rotate => rotate(Is2D::Yes, reify_0(), reify_0(), reify_1(), reify_numeric_argument(0)?),
        TransformFunction::Rotate3d => rotate(
            Is2D::No,
            reify_numeric_argument(0)?,
            reify_numeric_argument(1)?,
            reify_numeric_argument(2)?,
            reify_numeric_argument(3)?,
        ),
        TransformFunction::RotateX => rotate(Is2D::No, reify_1(), reify_0(), reify_0(), reify_numeric_argument(0)?),
        TransformFunction::RotateY => rotate(Is2D::No, reify_0(), reify_1(), reify_0(), reify_numeric_argument(0)?),
        TransformFunction::RotateZ => rotate(Is2D::No, reify_0(), reify_0(), reify_1(), reify_numeric_argument(0)?),

        // -> skew()
        //    1. Return a new CSSSkew object, whose ax and ay internal slots are set to the reification of the specified
        //       x and y angles, or the reification of 0deg if not specified in func, and whose is2D internal slot is
        //       true.
        TransformFunction::Skew => {
            // NB: Default y to 0deg if it's not specified.
            let y = if values.len() > 1 {
                reify_numeric_argument(1)?
            } else {
                reify_0deg()
            };
            AnyCSSTransformComponent::CSSSkew(CSSSkew::create(vm, reify_numeric_argument(0)?, y))
        }

        // -> skewX()
        //    1. Return a new CSSSkewX object, whose ax internal slot is set to the reification of the specified x
        //       angle, or the reification of 0deg if not specified in func, and whose is2D internal slot is true.
        TransformFunction::SkewX => {
            AnyCSSTransformComponent::CSSSkewX(CSSSkewX::create(vm, reify_numeric_argument(0)?))
        }

        // -> skewY()
        //    1. Return a new CSSSkewY object, whose ay internal slot is set to the reification of the specified y
        //       angle, or the reification of 0deg if not specified in func, and whose is2D internal slot is true.
        TransformFunction::SkewY => {
            AnyCSSTransformComponent::CSSSkewY(CSSSkewY::create(vm, reify_numeric_argument(0)?))
        }

        // -> perspective()
        //    1. Return a new CSSPerspective object, whose length internal slot is set to the reification of the
        //       specified length (see reify a numeric value if it is a length, and reify an identifier if it is the
        //       keyword none) and whose is2D internal slot is false.
        TransformFunction::Perspective => {
            let length = match reification_of(&values[0]) {
                Reification::Keyword(keyword) => {
                    CSSPerspectiveValueInternal::CSSKeywordValue(CSSKeywordValue::create(vm, keyword))
                }
                _ => CSSPerspectiveValueInternal::CSSNumericValue(reify_numeric_argument(0)?),
            };
            AnyCSSTransformComponent::CSSPerspective(CSSPerspective::create(vm, length))
        }
    })
}
