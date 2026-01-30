/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Types.h>

namespace Web::WebAudio::Render {

struct WorkletModule {
    u64 module_id { 0 };
    ByteString url;
    ByteString source_text;
};

}
