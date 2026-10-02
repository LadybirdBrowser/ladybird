/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::gc::visitor::{Trace, Visitor};
use crate::layout::cell::Gc;
use crate::runtime::completion::Completion;
use crate::runtime::promise_capability::PromiseCapability;

// 27.6.3.1 AsyncGeneratorRequest Records, https://tc39.es/ecma262/#sec-asyncgeneratorrequest-records
#[derive(Clone, Copy)]
pub struct AsyncGeneratorRequest {
    pub completion: Completion,            // [[Completion]]
    pub capability: Gc<PromiseCapability>, // [[Capability]]
}

// SAFETY: Visits the completion's value and the capability, the only cells a request reaches.
unsafe impl Trace for AsyncGeneratorRequest {
    fn trace(&self, visitor: &mut Visitor) {
        self.completion.value().trace(visitor);
        self.capability.trace(visitor);
    }
}
