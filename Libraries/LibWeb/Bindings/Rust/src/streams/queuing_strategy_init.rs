/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! QueuingStrategyInit, https://streams.spec.whatwg.org/#dictdef-queuingstrategyinit

/// The dictionary QueuingStrategyInit of Streams/QueuingStrategyInit.idl, whose members the bindings convert.
pub(crate) struct QueuingStrategyInit {
    pub(crate) high_water_mark: f64,
}
