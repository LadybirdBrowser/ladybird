/*
 * Copyright (c) 2019-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
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
#include <LibWeb/DOM/StyleInvalidationReason.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

class CSSImportRule;
class FontLoader;

struct CSSStyleSheetInit {
    Optional<String> base_url {};
    Variant<GC::Root<MediaList>, String> media { String {} };
    bool disabled { false };
};

// https://drafts.csswg.org/cssom-1/#cssstylesheet
class WEB_API CSSStyleSheet final : public StyleSheet {
    WEB_PLATFORM_OBJECT(CSSStyleSheet, StyleSheet);
    GC_DECLARE_ALLOCATOR(CSSStyleSheet);

public:
    enum class LoadingState : u8 {
        Unloaded,
        Loading,
        Loaded,
        Error,
    };
    static StringView loading_state_name(LoadingState);

    class Subresource {
    public:
        virtual ~Subresource() = default;

        virtual GC::Ptr<CSSStyleSheet> parent_style_sheet_for_subresource() = 0;
        LoadingState loading_state() const { return m_loading_state; }
        virtual void visit_edges(Cell::Visitor&) = 0;

        void set_loading_state(LoadingState);

    private:
        LoadingState m_loading_state { LoadingState::Unloaded };
    };

    [[nodiscard]] static GC::Ref<CSSStyleSheet> create(JS::Realm&, CSSRuleList&, MediaList&, Optional<::URL::URL> location);
    static WebIDL::ExceptionOr<GC::Ref<CSSStyleSheet>> construct_impl(JS::Realm&, Optional<CSSStyleSheetInit> const& options = {});

    virtual ~CSSStyleSheet() override = default;

    GC::Ptr<CSSRule const> owner_rule() const { return m_owner_css_rule; }
    GC::Ptr<CSSRule> owner_rule() { return m_owner_css_rule; }
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

    GC::Ref<WebIDL::Promise> replace(String text);
    WebIDL::ExceptionOr<void> replace_sync(StringView text);

    void for_each_effective_rule(TraversalOrder, Function<void(CSSRule const&)> const& callback) const;
    void for_each_effective_style_producing_rule(Function<void(CSSRule const&)> const& callback) const;
    // Returns whether the match state of any media queries changed after evaluation.
    bool evaluate_media_queries(DOM::Document const&);
    void for_each_effective_keyframes_at_rule(Function<void(CSSKeyframesRule const&)> const& callback) const;
    void for_each_counter_style_at_rule(Function<void(CSSCounterStyleRule const&)> const& callback) const;

    HashTable<GC::Ptr<DOM::Node>> owning_documents_or_shadow_roots() const { return m_owning_documents_or_shadow_roots; }
    void add_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root);
    void remove_owning_document_or_shadow_root(DOM::Node& document_or_shadow_root);
    void invalidate_owners(DOM::StyleInvalidationReason);
    GC::Ptr<DOM::Document> owning_document() const;

    Optional<FlyString> default_namespace() const;
    GC::Ptr<CSSNamespaceRule> default_namespace_rule() const { return m_default_namespace_rule; }
    HashTable<FlyString> declared_namespaces() const;

    Optional<FlyString> namespace_uri(StringView namespace_prefix) const;

    Vector<GC::Ref<CSSImportRule>> const& import_rules() const { return m_import_rules; }

    Optional<::URL::URL> base_url() const { return m_base_url; }
    void set_base_url(Optional<::URL::URL> base_url) { m_base_url = move(base_url); }

    bool constructed() const { return m_constructed; }

    GC::Ptr<DOM::Document const> constructor_document() const { return m_constructor_document; }
    void set_constructor_document(GC::Ptr<DOM::Document const> constructor_document) { m_constructor_document = constructor_document; }

    bool disallow_modification() const { return m_disallow_modification; }

    void set_source_text(String);
    Optional<String> source_text(Badge<DOM::Document>) const;

    void add_associated_font_loader(GC::Ref<FontLoader const> font_loader)
    {
        m_associated_font_loaders.append(font_loader);
    }
    bool has_associated_font_loader(FontLoader& font_loader) const;

    void add_critical_subresource(Subresource&);
    void remove_critical_subresource(Subresource&);
    LoadingState loading_state() const;
    void check_if_loading_completed();

private:
    CSSStyleSheet(JS::Realm&, CSSRuleList&, MediaList&, Optional<::URL::URL> location);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void recalculate_rule_caches();

    void set_constructed(bool constructed) { m_constructed = constructed; }
    void set_disallow_modification(bool disallow_modification) { m_disallow_modification = disallow_modification; }

    Parser::ParsingParams make_parsing_params() const;

    Optional<String> m_source_text;

    GC::Ptr<CSSRuleList> m_rules;
    GC::Ptr<CSSNamespaceRule> m_default_namespace_rule;
    HashMap<FlyString, GC::Ptr<CSSNamespaceRule>> m_namespace_rules;
    Vector<GC::Ref<CSSImportRule>> m_import_rules;

    GC::Ptr<CSSRule> m_owner_css_rule;

    Optional<::URL::URL> m_base_url;
    GC::Ptr<DOM::Document const> m_constructor_document;
    HashTable<GC::Ptr<DOM::Node>> m_owning_documents_or_shadow_roots;
    bool m_constructed { false };
    bool m_disallow_modification { false };
    Optional<bool> m_did_match;

    Vector<GC::Ptr<FontLoader const>> m_associated_font_loaders;

    Vector<Subresource&> m_critical_subresources;
};

}
