/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Encrypted Media Extensions, https://w3c.github.io/encrypted-media/

pub(crate) mod algorithms;
#[allow(clippy::module_inception, reason = "the module mirrors EncryptedMediaExtensions.h")]
pub(crate) mod encrypted_media_extensions;
pub(crate) mod key_system;
pub(crate) mod media_key_system_access;
