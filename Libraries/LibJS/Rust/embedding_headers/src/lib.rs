/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A crate with no code of its own: its build script writes the C headers of the embedding ABI of the LibJS runtime,
//! which LibJS's public headers include, so that the C++ that includes them waits for the headers alone rather than
//! for the runtime crate (Libraries/LibJS/Rust/CMakeLists.txt).
