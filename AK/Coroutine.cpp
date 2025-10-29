/*
 * Copyright (c) 2025, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Coroutine.h>

TracingAllocator AK::Detail::g_coroutine_state_allocator;
