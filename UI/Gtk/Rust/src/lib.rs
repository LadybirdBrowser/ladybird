/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(all(unix, not(target_os = "macos")))]
pub mod browser_window;
