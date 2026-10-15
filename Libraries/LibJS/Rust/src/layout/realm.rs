/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::{CellHeader, Gc};
use super::environment::{DeclarativeEnvironment, GlobalEnvironment};
use super::object::Object;
use super::shape::WatchpointSet;
use crate::layout_forward::{ForeignCellSlot, Intrinsics, RealmStorage};

#[repr(C)]
pub struct Realm {
    pub header: CellHeader,
    pub global_object: Cell<Option<Gc<Object>>>,
    /// Cached from the global environment.
    pub global_declarative_environment: Cell<Option<Gc<DeclarativeEnvironment>>>,
    pub global_environment: Cell<Option<Gc<GlobalEnvironment>>>,
    pub intrinsics: Cell<Option<Gc<Intrinsics>>>,
    /// [[HostDefined]]: a cell of the embedder, such as the settings object of a web realm.
    pub host_defined: ForeignCellSlot,
    /// Valid while %ArrayIteratorPrototype% has its original `next` (see `runtime::realm_watchpoints`).
    pub array_iterator_next_watchpoint_set: Cell<Option<Gc<WatchpointSet>>>,
    /// Valid while array iterators of the realm have no `return` method (see `runtime::realm_watchpoints`).
    pub array_iterator_return_watchpoint_set: Cell<Option<Gc<WatchpointSet>>>,
    pub storage: RealmStorage,
}
