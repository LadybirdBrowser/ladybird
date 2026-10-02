/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::layout::cell::Gc;
use crate::layout::realm::Realm;
use crate::runtime::cyclic_module::CyclicModule;
use crate::runtime::module::GraphLoadingState;
use crate::runtime::promise_capability::PromiseCapability;
use crate::script::Script;

/// The referrer of HostLoadImportedModule: a Script Record, a Cyclic Module Record or a Realm Record.
#[derive(Clone, Copy, Trace)]
pub enum ImportedModuleReferrer {
    Script(Gc<Script>),
    CyclicModule(Gc<CyclicModule>),
    Realm(Gc<Realm>),
}

/// The payload of HostLoadImportedModule: a GraphLoadingState Record or a PromiseCapability Record.
#[derive(Clone, Copy, Trace)]
pub enum ImportedModulePayload {
    GraphLoadingState(Gc<GraphLoadingState>),
    PromiseCapability(Gc<PromiseCapability>),
}
