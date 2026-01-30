/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Debug.h>

namespace Web::WebAudio {

WebAudioThreadRole& current_thread_role()
{
    thread_local WebAudioThreadRole s_role = WebAudioThreadRole::Unset;
    return s_role;
}

}
