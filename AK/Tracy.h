/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if defined(TRACY_ENABLE)
#    include <tracy/Tracy.hpp>
#    define TRACY_ZONE_SCOPED() ZoneScoped
#    define TRACY_ZONE_SCOPED_NAMED(name) ZoneScopedN(name)
#    define TRACY_SET_ZONE_NAME(name, length) ZoneName(name, length)
#    define TRACY_FRAME_MARK() FrameMark
#    define TRACY_FRAME_MARK_NAMED(name) FrameMarkNamed(name)
#    define TRACY_SET_PROGRAM_NAME(name) TracySetProgramName(name)
#    define TRACY_PLOT(name, value) TracyPlot(name, value)
#    if defined(TRACY_ENABLE_MEMORY)
#        define TRACY_ALLOCATED_MEMORY(ptr, size) TracyAlloc(ptr, size)
#        define TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, name) TracyAllocN(ptr, size, name)
#        define TRACY_FREED_MEMORY(ptr) TracyFree(ptr)
#        define TRACY_FREED_MEMORY_NAMED(ptr, name) TracyFreeN(ptr, name)
#    else
#        define TRACY_ALLOCATED_MEMORY(ptr, size)
#        define TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, name)
#        define TRACY_FREED_MEMORY(ptr)
#        define TRACY_FREED_MEMORY_NAMED(ptr, name)
#    endif
#else
#    define TRACY_ZONE_SCOPED()
#    define TRACY_ZONE_SCOPED_NAMED(name)
#    define TRACY_SET_ZONE_NAME(name, length)
#    define TRACY_FRAME_MARK()
#    define TRACY_FRAME_MARK_NAMED(name)
#    define TRACY_SET_PROGRAM_NAME(name)
#    define TRACY_PLOT(name, value)
#    define TRACY_ALLOCATED_MEMORY(ptr, size)
#    define TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, name)
#    define TRACY_FREED_MEMORY(ptr)
#    define TRACY_FREED_MEMORY_NAMED(ptr, name)
#endif
