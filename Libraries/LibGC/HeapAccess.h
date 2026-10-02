/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Export.h>

namespace GC {

// A thread that runs beside the heap's own thread, but never on its behalf, forbids itself heap access: the collector
// only scans its own thread's stack and never waits for other threads, so a cell another thread allocates, roots or
// collects is one it races with. Allocation and rooting on such a thread are debug assertions, collection is a
// verification failure.
GC_API void forbid_heap_access_on_this_thread();
GC_API bool heap_access_is_forbidden_on_this_thread();

}
