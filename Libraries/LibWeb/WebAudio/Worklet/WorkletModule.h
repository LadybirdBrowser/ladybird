/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>

namespace Web::WebAudio::Render {

struct WorkletModule {
    ByteString url;
    ByteString source_text;
};

}
