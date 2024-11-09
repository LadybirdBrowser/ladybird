/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ImageFormats/AnimationWriter.h>

namespace Gfx {

// Specified at: https://www.w3.org/Graphics/GIF/spec-gif89a.txt

class GIFWriter {
public:
    static ErrorOr<void> encode(Stream&, Bitmap const&);
    static ErrorOr<NonnullOwnPtr<AnimationWriter>> start_encoding_animation(SeekableStream&, IntSize dimensions, u16 loop_count);
};

}
