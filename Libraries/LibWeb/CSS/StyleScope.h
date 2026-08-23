/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class StyleScope;

// What a StyleEngine rule identity has to be turned back into before it can decide anything: the
// declaration it carries and where that declaration sits in the cascade.
struct StyleEngineRuleTarget {
    GC::Ptr<CSSRule const> rule;
    GC::Ptr<CSSContainerRule const> container_rule;
    Utf16FlyString qualified_layer_name;
    CascadeOrigin cascade_origin { CascadeOrigin::Author };

    void visit_edges(GC::Cell::Visitor&) const;
};

// What one style scope holds that is about the program rather than about any element: the keyframes
// each `@keyframes` name resolves to and whether a size container query is in play. Which rules
// match and how cascade layers are ordered are StyleEngine's answers and are not filed here.
struct StyleRuleCache {
    HashMap<Utf16FlyString, NonnullRefPtr<Animations::KeyframeEffect::KeyFrameSet>> rules_by_animation_keyframes;
    bool has_size_container_queries { false };

    void visit_edges(GC::Cell::Visitor&);
};

struct StyleCache : public RefCounted<StyleCache> {
    static NonnullRefPtr<StyleCache> create();

    OwnPtr<StyleRuleCache> rule_cache;
    u64 rule_cache_generation { 0 };

    void visit_edges(GC::Cell::Visitor&);
};

// Shared style caches for shadow-root scopes whose active stylesheets are the same ordered set of constructed
// sheets. The cache contents only depend on the sheets and document-wide state, so all such scopes can use one
// cache. Entries are validated against each sheet's shared-style-cache generation at lookup, so rule mutations and
// media match-state flips (which bump the generation) make stale entries fall away lazily. Entries whose cache no
// scope uses anymore get purged on insert, so abandoned sheet sets don't pin their caches for the document's
// lifetime.
class SheetSetStyleCacheRegistry {
public:
    NonnullRefPtr<StyleCache> ensure_style_cache_for_sheet_set(Vector<GC::Ref<CSSStyleSheet>> const& sheets);

    void visit_edges(GC::Cell::Visitor&);

private:
    struct Entry {
        Vector<GC::Ref<CSSStyleSheet>> sheets;
        Vector<u64> sheet_generations;
        NonnullRefPtr<StyleCache> style_cache;
    };

    static bool entry_is_current(Entry const&);

    HashMap<u32, Vector<Entry>> m_entries_by_hash;
};

class StyleScope {
public:
    explicit StyleScope(GC::Ref<DOM::Node>);

    DOM::Node& node() const { return m_node; }
    DOM::Document& document() const;

    [[nodiscard]] StyleRuleCache const& rule_cache() const;
    [[nodiscard]] bool has_valid_rule_cache() const { return m_style_cache && m_style_cache->rule_cache; }
    void invalidate_style_cache();
    void publish_cascade_layer_order(CSSStyleSheet* pending_attachment = nullptr);
    void invalidate_user_style_sheet();

    void for_each_stylesheet(CascadeOrigin, Function<void(CSS::CSSStyleSheet&)> const&) const;
    static WEB_API void for_each_user_agent_stylesheet(bool include_quirks_mode_stylesheet, bool include_mathml_and_svg_stylesheets, Function<void(CSS::CSSStyleSheet&, StyleSheetIdentifier const&)> const&);
    static Optional<StyleSheetIdentifier> user_agent_style_sheet_identifier(CSS::CSSStyleSheet const&);
    void build_user_style_sheet_if_needed();

    void make_rule_cache_for_cascade_origin(CascadeOrigin, StyleRuleCache&);

    void build_rule_cache();
    void build_rule_cache_if_needed() const;
    void populate_rule_cache(StyleRuleCache&);

    [[nodiscard]] TreeScopeID style_engine_tree_scope() const;

    void for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)> const& callback) const;

    void invalidate_counter_style_cache();
    void build_counter_style_cache();
    u64 counter_style_environment_identity() const;
    RefPtr<CSS::CounterStyle const> get_registered_counter_style(Utf16FlyString const& name) const;

    struct FunctionDefinitionAndScope {
        GC::Ref<CSSFunctionRule const> function;
        StyleScope const& scope;
    };
    Optional<FunctionDefinitionAndScope> get_function_definition(Utf16FlyString const& name) const;
    void for_each_visible_function_definition(Function<void(FunctionDefinitionAndScope const&)> const&) const;

    template<typename T>
    Optional<T> dereference_global_tree_scoped_reference(Function<Optional<T>(StyleScope const&)> const& callback) const;

    void visit_edges(GC::Cell::Visitor&);

    StyleCache& ensure_style_cache();
    StyleCache& ensure_style_cache() const;

    RefPtr<StyleCache> m_style_cache;

    GC::Ptr<CSSStyleSheet> m_user_style_sheet;

    bool m_needs_counter_style_cache_update : 1 { true };
    bool m_is_doing_counter_style_cache_update : 1 { false };
    bool m_has_published_named_layer_order : 1 { false };
    u64 m_published_layer_order_generation { 0 };
    u64 m_counter_style_environment_identity { 0 };
    HashMap<Utf16FlyString, NonnullRefPtr<CSS::CounterStyle const>> m_registered_counter_styles;

    GC::Ref<DOM::Node> m_node;
};

}
