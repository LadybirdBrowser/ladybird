/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/RustQueryHandle.h>

namespace Web::CSS::Parser {

class RustQueryParser {
public:
    static Vector<NonnullRefPtr<MediaQuery>> parse_media_query_list(Utf16View);
    static Optional<RustQueryHandle> parse_style_query(Utf16View);
};

}
