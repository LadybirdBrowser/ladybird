/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// Web::Permissions::PermissionName.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum PermissionName {
    Geolocation = 0,
    Microphone = 1,
    Notifications = 2,
}
