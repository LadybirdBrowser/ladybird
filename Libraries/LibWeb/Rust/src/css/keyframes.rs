/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::declaration_block::{DeclarationBlock, DeclarationBlockData};
use std::sync::Arc;

pub struct KeyframeData {
    pub(crate) keys: Box<[f64]>,
    pub(crate) declarations: Arc<DeclarationBlockData>,
}

pub struct FfiKeyframe {
    data: Arc<KeyframeData>,
    declarations: DeclarationBlock,
}

impl FfiKeyframe {
    pub(crate) fn declaration_block(&self) -> &DeclarationBlock {
        &self.declarations
    }

    pub(crate) fn keys(&self) -> &[f64] {
        &self.data.keys
    }

    pub(crate) fn declarations(&self) -> Arc<DeclarationBlockData> {
        self.declarations.data()
    }

    pub(crate) fn new(data: Arc<KeyframeData>) -> Self {
        Self {
            declarations: DeclarationBlock::new(data.declarations.clone()),
            data,
        }
    }
}

#[repr(C)]
pub struct FfiKeyframeKeys {
    pub values: *const f64,
    pub count: usize,
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_keyframe_keys(frame: &FfiKeyframe) -> FfiKeyframeKeys {
    FfiKeyframeKeys {
        values: frame.keys().as_ptr(),
        count: frame.keys().len(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_keyframe_declarations(frame: &FfiKeyframe) -> *mut DeclarationBlock {
    Box::into_raw(Box::new(frame.declaration_block().clone()))
}
