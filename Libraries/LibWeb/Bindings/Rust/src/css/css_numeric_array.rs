/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSNumericArray, https://drafts.css-houdini.org/css-typed-om-1/#cssnumericarray

use libjs_rust::api::{MarkedVec, Trace, Vm};

use super::css_numeric_value::AnyCSSNumericValue;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSNumericArray {
    values: Vec<AnyCSSNumericValue>,
}

impl Wrappable for CSSNumericArray {}

impl CSSNumericArray {
    pub(crate) fn create(vm: &Vm, values: &MarkedVec<'_, AnyCSSNumericValue>) -> Ref<Self> {
        allocate(vm, |_| Self {
            values: values.to_vec(),
        })
    }

    pub(crate) fn values(&self) -> &[AnyCSSNumericValue] {
        &self.values
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericarray-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The length attribute of CSSNumericArray indicates how many CSSNumericValues are contained within the
        // CSSNumericArray.
        Ok(u32::try_from(self.values.len()).expect("the array has fewer items than an index can name"))
    }

    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<AnyCSSNumericValue> {
        Ok(self.values[index as usize])
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
    pub(crate) fn is_equal_numeric_values(&self, other: &Self) -> bool {
        // NB: This is just step 3, moved here to reduce repetition.
        // 3. If value1 and value2 are both CSSMathSums, CSSMathProducts, CSSMathMins, or CSSMathMaxs:
        // 1. If value1’s values and value2s values internal slots have different sizes, return false.
        if self.values.len() != other.values.len() {
            return false;
        }

        // 2. If any item in value1’s values internal slot is not an equal numeric value to the item in value2’s values
        //    internal slot at the same index, return false.
        // 3. Return true.
        self.values
            .iter()
            .zip(&other.values)
            .all(|(first, second)| first.is_equal_numeric_value(second))
    }
}
