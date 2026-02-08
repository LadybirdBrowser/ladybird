/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibGfx/Forward.h>

namespace Web::HTML {

struct SerializeBitmapResult {
    ByteBuffer buffer;
    StringView mime_type;
};

// https://html.spec.whatwg.org/multipage/canvas.html#a-serialisation-of-the-bitmap-as-a-file
ErrorOr<SerializeBitmapResult> serialize_bitmap(Gfx::Bitmap const& bitmap, StringView type, Optional<double> quality);

}
