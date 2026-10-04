/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Constructing the VM in storage the embedder provides, and the VM's state.

use crate::interpreter::vm::Vm;
use crate::layout::vm::{VM_ALIGN, VM_SIZE};

// LibJS/Embedding/Layout.h tells an embedder to reserve this much storage for the VM.
const _: () = assert!(size_of::<Vm>() <= VM_SIZE, "the Vm outgrew VM_SIZE in src/layout/vm.rs");
const _: () = assert!(
    align_of::<Vm>() <= VM_ALIGN,
    "the Vm outgrew VM_ALIGN in src/layout/vm.rs"
);
