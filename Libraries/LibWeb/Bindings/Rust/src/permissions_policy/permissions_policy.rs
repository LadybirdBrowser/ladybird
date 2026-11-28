/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// Web::PermissionsPolicy::PolicyControlledFeature.
#[derive(Clone, Copy)]
#[repr(u8)]
pub(crate) enum PolicyControlledFeature {
    EncryptedMedia = 3,
    Gamepad = 6,
    Geolocation = 7,
    PublickeyCredentialsCreate = 10,
    PublickeyCredentialsGet = 11,
}
