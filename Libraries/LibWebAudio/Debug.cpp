/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebAudio/Debug.h>

namespace Web::WebAudio {

static thread_local WebAudioThreadRole s_role = WebAudioThreadRole::Unset;

WebAudioThreadRole& current_thread_role() { return s_role; }

} // namespace Web::WebAudio
