/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/RustQueryHandle.h>

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
    static Optional<RustQueryHandle> parse_media_condition(Parser&, Utf16View);
    static Optional<RustQueryHandle> parse_media_feature(Parser&, Utf16View);
    static Optional<RustQueryHandle> parse_supports_condition(Parser&, Utf16View);
    static Optional<RustQueryHandle> parse_supports_declaration(Parser&, Utf16View);
    static Optional<RustQueryHandle> parse_style_query(Parser&, Utf16View);
    static Optional<Vector<ContainerCondition>> parse_container_condition_list(Parser&, Utf16View);
    static RefPtr<StyleValue const> parse_source_size_value(Parser&, Utf16View);
    static Optional<Vector<SizesAttributeEntry>> split_sizes_attribute(Utf16View);
    static u16 resolve_query_feature(u8, u16 const*, size_t);
    static bool evaluate_supports_feature(void*, ValueParserFFI::FfiSupportsFeatureKind, ValueParserFFI::FfiUtf16View);
    static RustQueryHandle reevaluate_supports_condition(Parser&, RustQueryHandle const&);
};

}
