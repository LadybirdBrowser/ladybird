/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::runtime::job_callback::JobCallback;
use crate::runtime::promise_capability::PromiseCapability;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PromiseReactionType {
    Fulfill,
    Reject,
}

// 27.2.1.2 PromiseReaction Records, https://tc39.es/ecma262/#sec-promisereaction-records
#[repr(C)]
#[derive(Trace)]
pub struct PromiseReaction {
    header: CellHeader,
    #[gc(untraced)]
    reaction_type: PromiseReactionType,
    capability: Cell<Option<Gc<PromiseCapability>>>,
    handler: Cell<Option<Gc<JobCallback>>>,
}

define_cell!(PromiseReaction, Other);

impl PromiseReaction {
    pub fn create(
        vm: &Vm,
        reaction_type: PromiseReactionType,
        capability: Option<Gc<PromiseCapability>>,
        handler: Option<Gc<JobCallback>>,
    ) -> Gc<PromiseReaction> {
        vm.heap().allocate(PromiseReaction {
            header: CellHeader::for_class(Self::CLASS),
            reaction_type,
            capability: Cell::new(capability),
            handler: Cell::new(handler),
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
