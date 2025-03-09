/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>

namespace Core {

void deferred_invoke_block(EventLoop& event_loop, void (^invokee)(void));

}
