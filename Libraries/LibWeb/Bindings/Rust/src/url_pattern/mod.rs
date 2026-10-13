/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The URL Pattern Standard, https://urlpattern.spec.whatwg.org/

#[cfg(not(test))]
#[expect(
    clippy::module_inception,
    reason = "the module of an interface has the name of its IDL file, which here is also that of its directory"
)]
pub(crate) mod url_pattern;
