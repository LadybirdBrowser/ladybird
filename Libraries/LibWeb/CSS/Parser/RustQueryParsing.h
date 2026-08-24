/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/FeatureQuery.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/Parser/TokenStream.h>

namespace Web::CSS::Parser {

class Parser;

class RustQueryParser {
public:
    static Vector<NonnullRefPtr<MediaQuery>> parse_media_query_list(Parser&, Utf16View);
    static Optional<FeatureValue> parse_media_feature_value(Parser&, MediaFeatureID, TokenStream<ComponentValue>&);
};

}
