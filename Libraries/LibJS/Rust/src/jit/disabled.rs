/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime side of the optimizing JIT in builds without it (the crate's "jit" feature off): the items of
//! `jit/mod.rs` that the rest of the runtime uses, with nothing behind them. Every executable runs with the plain
//! handlers of the interpreter, which is the only variant of it these builds have, and nothing collects feedback.

use core::ffi::c_void;

use crate::interpreter::dispatch_tables::plain_dispatch_table;

/// Which interpreter handlers an executable's frames run with: the plain ones.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum InterpreterTier {
    Plain = 0,
}

/// The JIT's state on a VM, which has none.
pub struct JitState;

impl JitState {
    pub fn new(_options: options::Options) -> Self {
        Self
    }

    pub fn collects_feedback(&self) -> bool {
        false
    }

    pub fn initial_tier(&self) -> InterpreterTier {
        InterpreterTier::Plain
    }

    pub fn dispatch_tables(&self) -> impl Iterator<Item = (InterpreterTier, *const c_void)> {
        [(InterpreterTier::Plain, plain_dispatch_table())].into_iter()
    }
}

pub mod options {
    /// The options of the JIT, which these builds ignore.
    pub struct Options;

    impl Options {
        /// Warns once if LIBJS_JIT asks for the JIT, which this build does not have.
        pub fn from_environment() -> Self {
            static WARNING: std::sync::Once = std::sync::Once::new();
            if let Ok(value) = std::env::var("LIBJS_JIT")
                && value.trim() != "off"
            {
                WARNING.call_once(|| eprintln!("LIBJS_JIT: This build has no JIT, ignoring LIBJS_JIT={value}"));
            }
            Self
        }
    }
}

pub mod testing {
    use crate::interpreter::vm::Vm;
    use crate::layout::cell::Gc;
    use crate::layout::object::Object;
    use crate::runtime::realm::Realm;

    /// Without the JIT, there is no jit object.
    pub fn define_jit_testing_object(_vm: &Vm, _realm: Gc<Realm>, _global: &Object) {}
}
