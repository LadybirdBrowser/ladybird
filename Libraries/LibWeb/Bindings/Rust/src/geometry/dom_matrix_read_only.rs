/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The 2D matrix dictionary of Geometry/DOMMatrixReadOnly.idl, https://drafts.fxtf.org/geometry/#dictdef-dommatrix2dinit

use libgfx_rust::AffineTransform;

/// The dictionary DOMMatrix2DInit of Geometry/DOMMatrixReadOnly.idl, whose members the bindings convert.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct DOMMatrix2DInit {
    pub(crate) a: Option<f64>,
    pub(crate) b: Option<f64>,
    pub(crate) c: Option<f64>,
    pub(crate) d: Option<f64>,
    pub(crate) e: Option<f64>,
    pub(crate) f: Option<f64>,
    pub(crate) m11: Option<f64>,
    pub(crate) m12: Option<f64>,
    pub(crate) m21: Option<f64>,
    pub(crate) m22: Option<f64>,
    pub(crate) m41: Option<f64>,
    pub(crate) m42: Option<f64>,
}

/// The six elements of a 2D matrix, which validating and fixing up a DOMMatrix2DInit gives.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct Matrix2D {
    pub(crate) m11: f64,
    pub(crate) m12: f64,
    pub(crate) m21: f64,
    pub(crate) m22: f64,
    pub(crate) m41: f64,
    pub(crate) m42: f64,
}

impl Matrix2D {
    /// Whether none of the elements is infinite or NaN.
    pub(crate) fn is_finite(&self) -> bool {
        [self.m11, self.m12, self.m21, self.m22, self.m41, self.m42]
            .iter()
            .all(|element| element.is_finite())
    }

    /// The transform of the matrix in floats, as Gfx::AffineTransform takes the elements of a DOMMatrix.
    #[allow(clippy::cast_possible_truncation, reason = "Gfx::AffineTransform is in floats")]
    pub(crate) fn to_affine_transform(self) -> AffineTransform {
        AffineTransform::new(
            self.m11 as f32,
            self.m12 as f32,
            self.m21 as f32,
            self.m22 as f32,
            self.m41 as f32,
            self.m42 as f32,
        )
    }
}

/// The TypeError of validating a dictionary whose members disagree, with its message.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct InconsistentMembers(pub(crate) &'static str);

#[cfg(not(test))]
impl From<InconsistentMembers> for crate::bindings::Exception {
    fn from(InconsistentMembers(message): InconsistentMembers) -> Self {
        Self::TypeError(ak::Utf16String::from_ascii(message.as_bytes()))
    }
}

/// SameValueZero() of two numbers, https://tc39.es/ecma262/#sec-samevaluezero
fn same_value_zero(x: f64, y: f64) -> bool {
    (x.is_nan() && y.is_nan()) || x == y
}

/// https://drafts.fxtf.org/geometry/#matrix-validate-and-fixup-2d
pub(crate) fn validate_and_fixup_2d(init: &DOMMatrix2DInit) -> Result<Matrix2D, InconsistentMembers> {
    // 1. If at least one of the following conditions are true for dict, then throw a TypeError exception and abort
    //    these steps.
    let disagree = |member: Option<f64>, matrix_member: Option<f64>| matches!((member, matrix_member), (Some(member), Some(matrix_member)) if !same_value_zero(member, matrix_member));
    // - a and m11 are both present and SameValueZero(a, m11) is false.
    if disagree(init.a, init.m11) {
        return Err(InconsistentMembers(
            "DOMMatrix2DInit.a and DOMMatrix2DInit.m11 must have the same value if they are both present",
        ));
    }
    // - b and m12 are both present and SameValueZero(b, m12) is false.
    if disagree(init.b, init.m12) {
        return Err(InconsistentMembers(
            "DOMMatrix2DInit.b and DOMMatrix2DInit.m12 must have the same value if they are both present",
        ));
    }
    // - c and m21 are both present and SameValueZero(c, m21) is false.
    if disagree(init.c, init.m21) {
        return Err(InconsistentMembers(
            "DOMMatrix2DInit.c and DOMMatrix2DInit.m21 must have the same value if they are both present",
        ));
    }
    // - d and m22 are both present and SameValueZero(d, m22) is false.
    if disagree(init.d, init.m22) {
        return Err(InconsistentMembers(
            "DOMMatrix2DInit.d and DOMMatrix2DInit.m22 must have the same value if they are both present",
        ));
    }
    // - e and m41 are both present and SameValueZero(e, m41) is false.
    if disagree(init.e, init.m41) {
        return Err(InconsistentMembers(
            "DOMMatrix2DInit.e and DOMMatrix2DInit.m41 must have the same value if they are both present",
        ));
    }
    // - f and m42 are both present and SameValueZero(f, m42) is false.
    if disagree(init.f, init.m42) {
        return Err(InconsistentMembers(
            "DOMMatrix2DInit.f and DOMMatrix2DInit.m42 must have the same value if they are both present",
        ));
    }

    // 2. If m11 is not present then set it to the value of member a, or value 1 if a is also not present.
    // 3. If m12 is not present then set it to the value of member b, or value 0 if b is also not present.
    // 4. If m21 is not present then set it to the value of member c, or value 0 if c is also not present.
    // 5. If m22 is not present then set it to the value of member d, or value 1 if d is also not present.
    // 6. If m41 is not present then set it to the value of member e, or value 0 if e is also not present.
    // 7. If m42 is not present then set it to the value of member f, or value 0 if f is also not present.
    Ok(Matrix2D {
        m11: init.m11.or(init.a).unwrap_or(1.0),
        m12: init.m12.or(init.b).unwrap_or(0.0),
        m21: init.m21.or(init.c).unwrap_or(0.0),
        m22: init.m22.or(init.d).unwrap_or(1.0),
        m41: init.m41.or(init.e).unwrap_or(0.0),
        m42: init.m42.or(init.f).unwrap_or(0.0),
    })
}

#[cfg(test)]
mod tests {
    use super::{DOMMatrix2DInit, InconsistentMembers, Matrix2D, validate_and_fixup_2d};

    #[test]
    fn an_empty_dictionary_is_the_identity() {
        assert_eq!(
            validate_and_fixup_2d(&DOMMatrix2DInit::default()),
            Ok(Matrix2D {
                m11: 1.0,
                m12: 0.0,
                m21: 0.0,
                m22: 1.0,
                m41: 0.0,
                m42: 0.0,
            })
        );
    }

    #[test]
    fn matrix_members_win_over_their_aliases() {
        let init = DOMMatrix2DInit {
            a: Some(2.0),
            m22: Some(3.0),
            e: Some(4.0),
            m41: Some(4.0),
            f: Some(5.0),
            ..DOMMatrix2DInit::default()
        };
        let matrix = validate_and_fixup_2d(&init).expect("the members agree");
        assert_eq!((matrix.m11, matrix.m22, matrix.m41, matrix.m42), (2.0, 3.0, 4.0, 5.0));
    }

    #[test]
    fn members_that_disagree_are_a_type_error() {
        let init = DOMMatrix2DInit {
            b: Some(1.0),
            m12: Some(2.0),
            ..DOMMatrix2DInit::default()
        };
        assert_eq!(
            validate_and_fixup_2d(&init),
            Err(InconsistentMembers(
                "DOMMatrix2DInit.b and DOMMatrix2DInit.m12 must have the same value if they are both present"
            ))
        );
    }

    #[test]
    fn members_are_compared_with_same_value_zero() {
        let init = DOMMatrix2DInit {
            a: Some(f64::NAN),
            m11: Some(f64::NAN),
            c: Some(0.0),
            m21: Some(-0.0),
            ..DOMMatrix2DInit::default()
        };
        let matrix = validate_and_fixup_2d(&init).expect("the members agree");
        assert!(matrix.m11.is_nan());
        assert!(!matrix.is_finite());
        assert!(matrix.m21.is_sign_negative());
    }
}
