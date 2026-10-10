/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Types.h>

namespace TestWeb {

void maybe_attach_on_fail_fast_timeout(pid_t);
void capture_web_content_state_on_pre_navigation_timeout(pid_t);

}
