/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebGL command stream that WebContent records and the Compositor replays, as Libraries/LibCompositing/WebGL/
//! GLFunctions.json defines it.

pub mod vocabulary;
pub mod wire;

/// The `u64` that the replayer stores the sequence number of the last executed flush in is at the start of the shared
/// command buffer, before the region of records.
pub const SHARED_DATA_REGION_OFFSET: usize = 128;
pub const DEFAULT_SHARED_DATA_REGION_CAPACITY: usize = 32 * 1024 * 1024;
