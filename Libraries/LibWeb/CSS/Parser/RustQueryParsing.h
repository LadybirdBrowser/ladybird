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

namespace Web::CSS::Parser {

class Parser;

class RustQueryParser {
public:
    struct SizesAttributeEntry {
        Utf16String condition;
        Utf16String size;
    };

    struct ContainerCondition {
        Optional<Utf16FlyString> name;
        RefPtr<ContainerQuery> query;
    };

    static Vector<NonnullRefPtr<MediaQuery>> parse_media_query_list(Parser&, Utf16View);
    static OwnPtr<BooleanExpression> parse_media_condition(Parser&, Utf16View);
    static OwnPtr<BooleanExpression> parse_media_feature(Parser&, Utf16View);
    static RefPtr<Supports> parse_supports(Parser&, Utf16View);
    static OwnPtr<BooleanExpression> parse_supports_condition(Parser&, Utf16View);
    static OwnPtr<BooleanExpression> parse_supports_declaration(Parser&, Utf16View);
    static OwnPtr<BooleanExpression> parse_style_query(Parser&, Utf16View);
    static Optional<Vector<ContainerCondition>> parse_container_condition_list(Parser&, Utf16View);
    static RefPtr<StyleValue const> parse_source_size_value(Parser&, Utf16View);
    static Optional<Vector<SizesAttributeEntry>> split_sizes_attribute(Utf16View);
    static Optional<FeatureValue> parse_media_feature_value(Parser&, MediaFeatureID, Utf16View);
    static Optional<FeatureValue> parse_size_feature_value(Parser&, SizeFeatureID, Utf16View);
    static OwnPtr<BooleanExpression> supports_declaration_feature(Parser&, Utf16View);
    static OwnPtr<BooleanExpression> supports_selector_feature(Parser&, Utf16View);
};

}
