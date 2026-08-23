/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/FeatureQuery.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/Parser/TokenStream.h>

namespace Web::CSS::Parser {

class Parser;

class RustQueryParser {
public:
    struct ContainerCondition {
        Optional<Utf16FlyString> name;
        RefPtr<ContainerQuery> query;
    };

    static Vector<NonnullRefPtr<MediaQuery>> parse_media_query_list(Parser&, Utf16View);
    static RefPtr<Supports> parse_supports(Parser&, Utf16View);
    static Optional<Vector<ContainerCondition>> parse_container_condition_list(Parser&, Utf16View);
    static Optional<FeatureValue> parse_media_feature_value(Parser&, MediaFeatureID, TokenStream<ComponentValue>&);
    static Optional<FeatureValue> parse_size_feature_value(Parser&, SizeFeatureID, TokenStream<ComponentValue>&);
    static OwnPtr<BooleanExpression> parse_supports_declaration(Parser&, Vector<ComponentValue>);
    static OwnPtr<BooleanExpression> parse_supports_selector(Parser&, Vector<ComponentValue>);
};

}
