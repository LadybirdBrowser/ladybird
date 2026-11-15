/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// https://w3c.github.io/geolocation/#dom-geolocationpositionerror-code
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub(crate) enum GeolocationPositionErrorCode {
    PermissionDenied = 1,
    PositionUnavailable = 2,
    Timeout = 3,
}

impl GeolocationPositionErrorCode {
    pub(crate) fn from_u16(code: u16) -> Self {
        match code {
            1 => Self::PermissionDenied,
            2 => Self::PositionUnavailable,
            3 => Self::Timeout,
            _ => unreachable!("LibWeb passes a GeolocationPositionErrorCode"),
        }
    }
}
