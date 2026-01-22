/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>

namespace AK {

void initialize_main_thread();
bool is_main_thread();

}

#define ASSERT_ON_MAIN_THREAD() ASSERT(AK::is_main_thread())

#define VERIFY_ON_MAIN_THREAD() VERIFY(AK::is_main_thread())
