/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::query_parser::{
    CONTAINER_QUERY_REQUIRES_BLOCK_SIZE, CONTAINER_QUERY_REQUIRES_HEIGHT, CONTAINER_QUERY_REQUIRES_INLINE_SIZE,
    CONTAINER_QUERY_REQUIRES_WIDTH, FfiQueryHandle,
};
use std::sync::Arc;

pub(crate) struct ContainerConditionData {
    pub name: Option<CssString>,
    pub query: Option<Arc<FfiQueryHandle>>,
}

// Names remain independent of the document's atom table, including when a
// parsed stylesheet is shared between documents or published by a worker.
pub struct ContainerConditionsData {
    pub(crate) conditions: Box<[ContainerConditionData]>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ContainerConditionsData>();
};

#[unsafe(no_mangle)]
pub extern "C" fn rust_container_conditions_contains_size_feature(conditions: &ContainerConditionsData) -> bool {
    conditions.contains_size_feature()
}

impl ContainerConditionsData {
    pub(crate) fn contains_size_feature(&self) -> bool {
        self.conditions.iter().any(|condition| {
            condition.query.as_ref().is_some_and(|query| {
                let requirements = query.container_requirements();
                requirements
                    & (CONTAINER_QUERY_REQUIRES_WIDTH
                        | CONTAINER_QUERY_REQUIRES_HEIGHT
                        | CONTAINER_QUERY_REQUIRES_INLINE_SIZE
                        | CONTAINER_QUERY_REQUIRES_BLOCK_SIZE)
                    != 0
            })
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_container_conditions_retain(
    conditions: *const ContainerConditionsData,
) -> *const ContainerConditionsData {
    if !conditions.is_null() {
        unsafe { Arc::increment_strong_count(conditions) };
    }
    conditions
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_container_conditions_release(conditions: *const ContainerConditionsData) {
    if !conditions.is_null() {
        unsafe { Arc::decrement_strong_count(conditions) };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_container_conditions_count(conditions: *const ContainerConditionsData) -> usize {
    unsafe { &*conditions }.conditions.len()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_container_conditions_name(
    conditions: *const ContainerConditionsData,
    index: usize,
) -> FfiUtf16View {
    let name = unsafe { &*conditions }.conditions[index].name.as_ref();
    let units = name.map_or(&[][..], CssString::units);
    FfiUtf16View {
        ascii: std::ptr::null(),
        utf16: units.as_ptr(),
        length: units.len(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_container_conditions_query(
    conditions: *const ContainerConditionsData,
    index: usize,
) -> *const FfiQueryHandle {
    unsafe { &*conditions }.conditions[index]
        .query
        .as_ref()
        .map_or(std::ptr::null(), Arc::as_ptr)
}
