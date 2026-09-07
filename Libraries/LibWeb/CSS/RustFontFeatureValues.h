/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Span.h>
#include <AK/Utf16View.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

using FontFeatureValuesRuleKind = Parser::ValueParserFFI::FontFeatureValuesRuleKind;

class RustFontFeatureValuesSnapshot {
    AK_MAKE_NONCOPYABLE(RustFontFeatureValuesSnapshot);

public:
    explicit RustFontFeatureValuesSnapshot(Parser::ValueParserFFI::FontFeatureValuesData const* data)
        : m_data(data)
    {
    }
    RustFontFeatureValuesSnapshot(RustFontFeatureValuesSnapshot&& other)
        : m_data(exchange(other.m_data, nullptr))
    {
    }
    ~RustFontFeatureValuesSnapshot() { Parser::ValueParserFFI::rust_font_feature_values_data_release(m_data); }
    size_t family_count() const { return Parser::ValueParserFFI::rust_font_feature_values_data_family_count(m_data); }
    Utf16View family_at(size_t index) const
    {
        auto view = Parser::ValueParserFFI::rust_font_feature_values_data_family_at(m_data, index);
        return { reinterpret_cast<char16_t const*>(view.utf16), view.length };
    }
    void for_each_entry(FontFeatureValuesRuleKind kind, Function<void(Utf16View, ReadonlySpan<u32>)> const& callback) const
    {
        auto count = Parser::ValueParserFFI::rust_font_feature_values_data_count(m_data, kind);
        for (size_t index = 0; index < count; ++index) {
            auto entry = Parser::ValueParserFFI::rust_font_feature_values_data_at(m_data, kind, index);
            callback({ reinterpret_cast<char16_t const*>(entry.name.utf16), entry.name.length }, { entry.values, entry.count });
        }
    }

private:
    Parser::ValueParserFFI::FontFeatureValuesData const* m_data;
};

}
