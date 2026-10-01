/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod class_blueprint;
pub mod executable;
pub mod operand;
pub mod property_access;

/// Views of the bytecode instructions and of the operand records their slow paths receive.
pub mod op {
    use super::operand::*;
    use crate::layout::property_lookup_cache::EnvironmentCoordinate;
    use crate::layout::value::Value;

    include!(concat!(env!("OUT_DIR"), "/bytecode_ops.rs"));
}
