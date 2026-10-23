/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The File API, https://w3c.github.io/FileAPI/

#[cfg(not(test))]
pub(crate) mod blob;
#[cfg(not(test))]
pub(crate) mod exports;
#[cfg(not(test))]
pub(crate) mod file;
