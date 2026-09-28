/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace JS {

// https://console.spec.whatwg.org/#logger
// These are not really levels, but that's the term used in the spec.
enum class ConsoleLogLevel {
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
};

}
