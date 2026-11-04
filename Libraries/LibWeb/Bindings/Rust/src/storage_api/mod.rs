/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Storage Standard, https://storage.spec.whatwg.org/

#[cfg(not(test))]
pub(crate) mod exports;
pub(crate) mod storage_area_mirror;
#[cfg(not(test))]
pub(crate) mod storage_bottle;
#[cfg(not(test))]
pub(crate) mod storage_endpoint;
pub(crate) mod storage_key;
#[cfg(not(test))]
pub(crate) mod storage_manager;
#[cfg(not(test))]
pub(crate) mod storage_shelf;
