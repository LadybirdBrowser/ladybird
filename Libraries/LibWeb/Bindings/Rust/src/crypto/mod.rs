/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Web Cryptography API.

#[cfg_attr(not(web_idl_bindings), allow(dead_code))]
#[allow(clippy::module_inception, reason = "the module mirrors the Crypto interface file")]
pub(crate) mod crypto;
#[cfg_attr(not(web_idl_bindings), allow(dead_code))]
pub(crate) mod crypto_algorithms;
#[cfg_attr(not(web_idl_bindings), allow(dead_code))]
pub(crate) mod crypto_bindings;

#[cfg_attr(not(web_idl_bindings), allow(dead_code))]
pub(crate) mod crypto_key;

#[cfg_attr(not(web_idl_bindings), allow(dead_code))]
pub(crate) mod subtle_crypto;
