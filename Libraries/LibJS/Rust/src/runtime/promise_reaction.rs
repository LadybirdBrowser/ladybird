/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::member::NullableMember;
use crate::runtime::job_callback::JobCallback;
use crate::runtime::promise_capability::PromiseCapability;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PromiseReactionType {
    Fulfill,
    Reject,
}

// 27.2.1.2 PromiseReaction Records, https://tc39.es/ecma262/#sec-promisereaction-records
// NB: The fields only take 20 bytes, and cells take at least 24.
#[repr(C, align(8))]
#[derive(Trace)]
pub struct PromiseReaction {
    header: CellHeader,
    #[gc(untraced)]
    reaction_type: PromiseReactionType,
    capability: NullableMember<PromiseCapability>,
    handler: NullableMember<JobCallback>,
}

const _: () = assert!(size_of::<PromiseReaction>() == 24);

define_cell!(PromiseReaction, Other);

impl PromiseReaction {
    pub fn create(
        vm: &Vm,
        reaction_type: PromiseReactionType,
        capability: Option<Gc<PromiseCapability>>,
        handler: Option<Gc<JobCallback>>,
    ) -> Gc<PromiseReaction> {
        vm.heap().allocate(|_| PromiseReaction {
            header: CellHeader::for_class(Self::CLASS),
            reaction_type,
            capability: NullableMember::new(capability),
            handler: NullableMember::new(handler),
        })
    }

    pub fn reaction_type(&self) -> PromiseReactionType {
        self.reaction_type
    }

    pub fn capability(&self) -> Option<Gc<PromiseCapability>> {
        self.capability.get()
    }

    pub fn handler(&self) -> Option<Gc<JobCallback>> {
        self.handler.get()
    }
}
