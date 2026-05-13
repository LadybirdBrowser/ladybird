/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibGfx/Forward.h>

namespace Gfx {

struct JPEGEncoderOptions {
    Optional<ReadonlyBytes> icc_data;
    u8 quality { 75 };
};

class JPEGWriter {
public:
    using Options = JPEGEncoderOptions;

    static ErrorOr<void> encode(Stream&, Bitmap const&, Options const& = {});

private:
    JPEGWriter() = delete;
};

}
