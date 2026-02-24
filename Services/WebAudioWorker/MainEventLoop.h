/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>

namespace Core {

class WeakEventLoopReference;

}

namespace Web::WebAudio {

void set_main_event_loop_reference(NonnullRefPtr<Core::WeakEventLoopReference>);
RefPtr<Core::WeakEventLoopReference> main_event_loop();

}
