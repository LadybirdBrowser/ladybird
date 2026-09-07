/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Lorenz Ackermann <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSImportRule);

GC::Ref<CSSImportRule> CSSImportRule::create(StyleSheetImport& import)
{
    return GC::Heap::the().allocate<CSSImportRule>(import);
}

CSSImportRule::CSSImportRule(StyleSheetImport& import)
    : CSSRule(import.native_rule())
    , m_rule(native_rule().payload().import_rule)
    , m_import(import)
    , m_scope(native_rule().payload().scope ? RefPtr<RustScopeSelectors> { RustScopeSelectors::create(native_rule().payload().scope) } : nullptr)
    , m_supports(m_rule.supports())
{
    m_import->set_cssom_rule(*this);
    CSSRule::set_parent_style_sheet(import.parent_style_sheet());
}

Optional<Utf16FlyString> CSSImportRule::internal_layer_name() const
{
    if (!m_rule.layer().has_value())
        return {};
    return m_layer_internal.ensure([&] {
        auto name = native_rule().internal_layer_name().release_value();
        return Utf16FlyString::from_utf16(name.utf16_view());
    });
}

CSSImportRule::~CSSImportRule() = default;

URL const& CSSImportRule::url() const
{
    return m_import->url();
}

GC::Ref<MediaList> CSSImportRule::media() const
{
    return m_import->media();
}

void CSSImportRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_import->visit_edges(visitor);
}

void CSSImportRule::set_parent_style_sheet(StyleSheetState* parent_style_sheet)
{
    Base::set_parent_style_sheet(parent_style_sheet);
    if (m_import->parent_style_sheet() != parent_style_sheet)
        m_import->set_parent_style_sheet(parent_style_sheet);
}

// https://www.w3.org/TR/cssom/#serialize-a-css-rule
Utf16String CSSImportRule::serialized() const
{
    Utf16StringBuilder builder;
    // The result of concatenating the following:

    // 1. The string "@import" followed by a single SPACE (U+0020).
    builder.append_ascii("@import "sv);

    // 2. The result of performing serialize a URL on the rule’s location.
    builder.append(url().to_utf16_string());

    // AD-HOC: Serialize the rule's layer if it exists.
    if (auto layer = m_rule.layer(); layer.has_value()) {
        if (layer->is_empty()) {
            builder.append_ascii(" layer"sv);
        } else {
            builder.append_ascii(" layer("sv);
            builder.append(*layer);
            builder.append_ascii(')');
        }
    }

    // AD-HOC: Serialize the rule's import scope if it exists.
    if (has_scope()) {
        builder.append_ascii(" scope"sv);
        if (m_scope->start().has_value() || m_scope->end().has_value()) {
            builder.append_ascii('(');
            if (m_scope->start().has_value()) {
                if (m_scope->end().has_value())
                    builder.appendff("({})", serialize_a_group_of_selectors(*m_scope->start()));
                else
                    builder.append(serialize_a_group_of_selectors(*m_scope->start()));
            }
            if (m_scope->end().has_value()) {
                if (m_scope->start().has_value())
                    builder.append_ascii(' ');
                builder.appendff("to ({})", serialize_a_group_of_selectors(*m_scope->end()));
            }
            builder.append_ascii(')');
        }
    }

    // AD-HOC: Serialize the rule's supports condition if it exists.
    //         This isn't currently specified, but major browsers include this in their serialization of import rules
    if (m_supports.has_value()) {
        builder.append_ascii(" supports("sv);
        builder.append(serialize_supports_condition(*m_supports));
        builder.append_ascii(')');
    }

    // 3. If the rule’s associated media list is not empty, a single SPACE (U+0020) followed by the result of performing serialize a media query list on the media list.
    if (m_import->native_media_list().length() != 0) {
        builder.append_ascii(' ');
        builder.append(m_import->native_media_list().media_text());
    }

    // 4. The string ";", i.e., SEMICOLON (U+003B).
    builder.append_ascii(';');

    return builder.to_string();
}

// https://drafts.csswg.org/cssom/#dom-cssimportrule-layername
Optional<Utf16FlyString> CSSImportRule::layer_name() const
{
    // The layerName attribute must return the layer name declared in the at-rule itself, or an empty string if the
    // layer is anonymous, or null if the at-rule does not declare a layer.
    auto layer = m_rule.layer();
    if (!layer.has_value())
        return {};
    return Utf16FlyString::from_utf16(*layer);
}

// https://drafts.csswg.org/cssom/#dom-cssimportrule-supportstext
Optional<Utf16String> CSSImportRule::supports_text() const
{
    // The supportsText attribute must return the <supports-condition> declared in the at-rule itself, or null if the
    // at-rule does not declare a supports condition.
    if (!m_supports.has_value())
        return {};
    return serialize_supports_condition(*m_supports);
}

bool CSSImportRule::matches() const
{
    if (m_supports.has_value() && !supports_condition_matches(*m_supports))
        return false;
    return m_import->native_media_list().matches();
}

void CSSImportRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Document URL: {}\n", url().to_string());

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Loading state: {}\n", StyleSheetState::loading_state_name(m_import->loading_state()));

    if (auto layer = layer_name(); layer.has_value()) {
        dump_indent(builder, indent_levels + 1);
        builder.appendff("Layer: `{}` (internal: `{}`)\n", *layer, *internal_layer_name());
    }

    if (m_import->native_media_list().length() != 0)
        m_import->native_media_list().dump(builder, indent_levels + 1);

    if (m_supports.has_value())
        dump_supports_condition(builder, *m_supports, indent_levels + 1);

    if (has_scope()) {
        dump_indent(builder, indent_levels + 1);
        builder.append("Scope:\n"sv);

        dump_indent(builder, indent_levels + 2);
        if (m_scope->start().has_value())
            builder.appendff("Start selectors: {}\n", serialize_a_group_of_selectors(*m_scope->start()));
        else
            builder.append("Start selectors: <none>\n"sv);

        dump_indent(builder, indent_levels + 2);
        if (m_scope->end().has_value())
            builder.appendff("End selectors: {}\n", serialize_a_group_of_selectors(*m_scope->end()));
        else
            builder.append("End selectors: <none>\n"sv);
    }

    if (auto* sheet = loaded_style_sheet()) {
        dump_sheet(builder, *sheet, indent_levels + 1);
    } else {
        dump_indent(builder, indent_levels + 1);
        builder.append("Style sheet not loaded\n"sv);
    }
}

}
