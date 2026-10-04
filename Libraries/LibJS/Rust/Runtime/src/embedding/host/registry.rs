/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime class of each host class table.

use std::collections::HashMap;

use crate::gc::class::Class;

/// The class derived at run time for each host class table, by the address of its JSHostClass.
pub type HostClassRegistry = HashMap<usize, &'static Class, foldhash::fast::RandomState>;
