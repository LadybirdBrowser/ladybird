/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[allow(
    clippy::module_inception,
    reason = "the module mirrors the Geolocation interface file"
)]
pub(crate) mod geolocation;
pub(crate) mod geolocation_coordinates;
pub(crate) mod geolocation_position;
pub(crate) mod geolocation_position_error;
pub(crate) mod geolocation_position_error_code;
