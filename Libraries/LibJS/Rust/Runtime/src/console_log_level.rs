/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://console.spec.whatwg.org/#logger
// These are not really levels, but that's the term used in the spec.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConsoleLogLevel {
    Assert,
    Count,
    CountReset,
    Debug,
    Dir,
    DirXML,
    Error,
    Group,
    GroupCollapsed,
    Info,
    Log,
    TimeEnd,
    TimeLog,
    Table,
    Trace,
    Warn,
}
