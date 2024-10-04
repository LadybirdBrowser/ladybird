/*
 * Copyright (c) 2019-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/StyleSheet.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

class CSSImportRule;
class FontLoader;

struct CSSStyleSheetInit {
    Optional<String> base_url {};
    Variant<JS::Handle<MediaList>, String> media { String {} };
    bool disabled { false };
};

class CSSStyleSheet final : public StyleSheet {
    WEB_PLATFORM_OBJECT(CSSStyleSheet, StyleSheet);
    JS_DECLARE_ALLOCATOR(CSSStyleSheet);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CSSStyleSheet> create(JS::Realm&, CSSRuleList&, MediaList&, Optional<URL::URL> location);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<CSSStyleSheet>> construct_impl(JS::Realm&, Optional<CSSStyleSheetInit> const& options = {});

    virtual ~CSSStyleSheet() override = default;

    JS::GCPtr<CSSRule const> owner_rule() const { return m_owner_css_rule; }
    JS::GCPtr<CSSRule> owner_rule() { return m_owner_css_rule; }
    void set_owner_css_rule(CSSRule* rule) { m_owner_css_rule = rule; }

    virtual String type() const override { return "text/css"_string; }

    CSSRuleList const& rules() const { return *m_rules; }
    CSSRuleList& rules() { return *m_rules; }

    CSSRuleList* css_rules() { return m_rules; }
    CSSRuleList const* css_rules() const { return m_rules; }

    WebIDL::ExceptionOr<unsigned> insert_rule(StringView rule, unsigned index);
    WebIDL::ExceptionOr<WebIDL::Long> add_rule(Optional<String> selector, Optional<String> style, Optional<WebIDL::UnsignedLong> index);
    WebIDL::ExceptionOr<void> remove_rule(Optional<WebIDL::UnsignedLong> index);
    WebIDL::ExceptionOr<void> delete_rule(unsigned index);

    JS::NonnullGCPtr<JS::Promise> replace(String text);
    WebIDL::ExceptionOr<void> replace_sync(StringView text);

    void for_each_effective_rule(TraversalOrder, Function<void(CSSRule const&)> const& callback) const;
    void for_each_effective_style_rule(Function<void(CSSStyleRule const&)> const& callback) const;
    // Returns whether the match state of any media queries changed after evaluation.
    bool evaluate_media_queries(HTML::Window const&);
    void for_each_effective_keyframes_at_rule(Function<void(CSSKeyframesRule const&)> const& callback) const;

    JS::GCPtr<StyleSheetList> style_sheet_list() const { return m_style_sheet_list; }
    void set_style_sheet_list(Badge<StyleSheetList>, StyleSheetList*);

    Optional<FlyString> default_namespace() const;
    JS::GCPtr<CSSNamespaceRule> default_namespace_rule() const { return m_default_namespace_rule; }

    Optional<FlyString> namespace_uri(StringView namespace_prefix) const;

    Vector<JS::NonnullGCPtr<CSSImportRule>> const& import_rules() const { return m_import_rules; }

    Optional<URL::URL> base_url() const { return m_base_url; }
    void set_base_url(Optional<URL::URL> base_url) { m_base_url = move(base_url); }

    bool constructed() const { return m_constructed; }

    JS::GCPtr<DOM::Document const> constructor_document() const { return m_constructor_document; }
    void set_constructor_document(JS::GCPtr<DOM::Document const> constructor_document) { m_constructor_document = constructor_document; }

    bool disallow_modification() const { return m_disallow_modification; }

    void set_source_text(String);
    Optional<String> source_text(Badge<DOM::Document>) const;

    void add_associated_font_loader(WeakPtr<FontLoader const> font_loader)
    {
        m_associated_font_loaders.append(font_loader);
    }
    bool has_associated_font_loader(FontLoader& font_loader) const;

private:
    CSSStyleSheet(JS::Realm&, CSSRuleList&, MediaList&, Optional<URL::URL> location);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void recalculate_rule_caches();

    void set_constructed(bool constructed) { m_constructed = constructed; }
    void set_disallow_modification(bool disallow_modification) { m_disallow_modification = disallow_modification; }

    Optional<String> m_source_text;

    JS::GCPtr<CSSRuleList> m_rules;
    JS::GCPtr<CSSNamespaceRule> m_default_namespace_rule;
    HashMap<FlyString, JS::GCPtr<CSSNamespaceRule>> m_namespace_rules;
    Vector<JS::NonnullGCPtr<CSSImportRule>> m_import_rules;

    JS::GCPtr<StyleSheetList> m_style_sheet_list;
    JS::GCPtr<CSSRule> m_owner_css_rule;

    Optional<URL::URL> m_base_url;
    JS::GCPtr<DOM::Document const> m_constructor_document;
    bool m_constructed { false };
    bool m_disallow_modification { false };
    Optional<bool> m_did_match;

    Vector<WeakPtr<FontLoader const>> m_associated_font_loaders;
};

}
