/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/RustImportRule.h>
#include <LibWeb/CSS/RustScopeSelectors.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class WEB_API CSSImportRule final : public CSSRule {
    WEB_WRAPPABLE(CSSImportRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSImportRule);

public:
    [[nodiscard]] static GC::Ref<CSSImportRule> create(StyleSheetImport&);

    virtual ~CSSImportRule() override;

    URL const& url() const;
    Utf16String const& href() const { return url().url(); }
    Utf16String href_for_bindings() const { return href(); }

    StyleSheetState* loaded_style_sheet() { return m_import->loaded_style_sheet(); }
    StyleSheetState const* loaded_style_sheet() const { return m_import->loaded_style_sheet(); }
    GC::Ref<MediaList> media() const;
    CSSStyleSheet* style_sheet_for_bindings()
    {
        auto* sheet = loaded_style_sheet();
        return sheet ? &sheet->cssom_sheet() : nullptr;
    }
    StyleSheetImport& import() const { return *m_import; }

    Optional<Utf16FlyString> layer_name() const;
    Optional<Utf16String> supports_text() const;

    bool matches() const;
    bool has_scope() const { return m_scope; }

    Optional<Utf16FlyString> internal_layer_name() const;

private:
    explicit CSSImportRule(StyleSheetImport&);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    virtual void set_parent_style_sheet(StyleSheetState*) override;

    virtual Utf16String serialized() const override;

    RustImportRule m_rule;
    NonnullRefPtr<StyleSheetImport> m_import;
    mutable Optional<Utf16FlyString> m_layer_internal;
    RefPtr<RustScopeSelectors> m_scope;
    Optional<RustQueryHandle> m_supports;
};

template<>
inline bool CSSRule::fast_is<CSSImportRule>() const { return type() == CSSRule::Type::Import; }

}
