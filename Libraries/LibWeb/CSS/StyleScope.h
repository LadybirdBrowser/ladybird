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
#include <AK/RefPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/RustRule.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser::ValueParserFFI {

struct RegisteredCounterStyles;

}

namespace Web::CSS {

class StyleScope;

// What a StyleEngine rule identity has to be turned back into before it can decide anything: the
// declaration it carries and where that declaration sits in the cascade.
struct StyleEngineRuleTarget {
    u64 rule_identity;
    u32 declaration_version;
    RustDeclarationBlockSnapshot declaration;
    RefPtr<StyleSheetState const> source_style_sheet;
    bool has_container_conditions { false };
    Utf16FlyString qualified_layer_name;
    CascadeOrigin cascade_origin { CascadeOrigin::Author };
};

struct CachedFunctionRule {
    RustCompiledFunction rule;
    Utf16FlyString qualified_layer_name;
    CascadeOrigin cascade_origin { CascadeOrigin::Author };
};

// What one style scope holds that is about the program rather than about any element: the keyframes
// each `@keyframes` name resolves to, the visible `@function` rules, and whether a size container query
// is in play. Which rules match and how cascade layers are ordered are StyleEngine's answers and are
// not filed here. A scope's cache is what each of its sheets defines, taken in sheet order.
struct StyleRuleCache {
    AK_ALLOC_WITH_KMALLOC;

    // Take in what the effective rules of a later sheet define: its keyframes replace those of the same name.
    void add_rules_from_sheet(StyleSheetState&, CascadeOrigin);
    // Take in what another cache collected, as if from a later sheet.
    void add_rules_from_cache(StyleRuleCache const&);

    HashMap<Utf16FlyString, NonnullRefPtr<Animations::KeyframeEffect::KeyFrameSet>> rules_by_animation_keyframes;
    HashMap<Utf16FlyString, Vector<CachedFunctionRule>> function_rules_by_name;
    bool has_size_container_queries { false };
};

class StyleScope {
    AK_MAKE_NONCOPYABLE(StyleScope);
    AK_MAKE_NONMOVABLE(StyleScope);

public:
    explicit StyleScope(GC::Ref<DOM::Node>);
    ~StyleScope();

    DOM::Node& node() const { return m_node; }
    DOM::Document& document() const;

    Vector<NonnullRefPtr<StyleSheetState>> const& style_sheets() const { return m_style_sheets; }

    enum class StyleEngineUpdate : u8 {
        Record,
        Defer,
    };
    void add_a_css_style_sheet(StyleSheetState&, StyleEngineUpdate = StyleEngineUpdate::Record);
    void remove_a_css_style_sheet(StyleSheetState&, StyleEngineUpdate = StyleEngineUpdate::Record);
    void move_sheet(StyleSheetState&, StyleScope& destination);
    void attach_sheet_to_style_engine(StyleSheetState&);
    enum class Alternate : u8 {
        No,
        Yes,
    };
    enum class OriginClean : u8 {
        No,
        Yes,
    };
    NonnullRefPtr<StyleSheetState> create_a_css_style_sheet(Utf16View css_text, DOM::Element* owner_node, Utf16View media, Utf16String title, Alternate, OriginClean, Optional<::URL::URL> location, StyleSheetState* parent_style_sheet, StyleSheetImport* owner_import, StyleEngineUpdate = StyleEngineUpdate::Record);
    void initialize_a_css_style_sheet(StyleSheetState&, DOM::Element* owner_node, Utf16View media, Utf16String title, Alternate, OriginClean, StyleSheetState* parent_style_sheet, StyleSheetImport* owner_import, StyleEngineUpdate = StyleEngineUpdate::Record);

    [[nodiscard]] StyleRuleCache const& rule_cache() const;
    void invalidate_style_cache();
    void publish_cascade_layer_order(StyleSheetState* pending_attachment = nullptr);
    void publish_animation_keyframes();
    void invalidate_user_style_sheet();

    // The `@keyframes` row a shadow root's scope published to the style engine, taken from the scope as the root
    // leaves its document, with the keyframe sets the row names: they stay alive until the row is given up.
    struct DepartedAnimationKeyframes {
        TreeScopeID tree_scope;
        FlatPtr shadow_root_identity { 0 };
        Vector<NonnullRefPtr<Animations::KeyframeEffect::KeyFrameSet const>> keyframe_sets;
    };
    [[nodiscard]] Optional<DepartedAnimationKeyframes> take_published_animation_keyframes();

    void for_each_stylesheet(CascadeOrigin, Function<void(CSS::StyleSheetState&)> const&) const;
    static WEB_API void for_each_user_agent_stylesheet(bool include_quirks_mode_stylesheet, bool include_mathml_and_svg_stylesheets, Function<void(CSS::StyleSheetState&, StyleSheetIdentifier const&)> const&);
    void build_user_style_sheet_if_needed();

    void build_rule_cache_if_needed() const;

    [[nodiscard]] TreeScopeID style_engine_tree_scope() const;

    void for_each_active_css_style_sheet(Function<void(CSS::StyleSheetState&)> const& callback) const;

    void invalidate_counter_style_cache();
    void build_counter_style_cache(Layout::BegunRead const& read);
    u64 counter_style_environment_identity(Layout::BegunRead const& read) const;
    void publish_counter_style_lookup_chain(Layout::BegunRead const& read) const;
    // Whether the marker text of a list item with this `list-style-type` value differs between counter values.
    bool list_style_type_depends_on_counter_value(void const* list_style_type) const;

    struct FunctionDefinitionAndScope {
        RustCompiledFunction function;
        StyleScope const& scope;
    };
    Optional<FunctionDefinitionAndScope> get_function_definition(Layout::BegunRead const& read, Utf16FlyString const& name) const;
    void for_each_visible_function_definition(Layout::BegunRead const& read, Function<void(FunctionDefinitionAndScope const&)> const&) const;

    template<typename T>
    Optional<T> dereference_global_tree_scoped_reference(Function<Optional<T>(StyleScope const&)> const& callback) const;

    void visit_edges(GC::Cell::Visitor&);

    // The keyframe sets this scope last published. The style engine names them by pointer, so they stay alive after
    // the rule cache they came from is invalidated, until the scope publishes again or gives its row up.
    Vector<NonnullRefPtr<Animations::KeyframeEffect::KeyFrameSet const>> m_published_keyframe_sets;

    RefPtr<StyleSheetState> m_user_style_sheet;

    bool m_needs_counter_style_cache_update : 1 { true };
    bool m_is_doing_counter_style_cache_update : 1 { false };
    bool m_has_published_named_layer_order : 1 { false };
    // Whether the style engine holds this scope's layer order and keyframes as the rule cache resolved them.
    bool m_has_published_rule_cache : 1 { false };
    u64 m_counter_style_environment_identity { 0 };
    // What the layout node arena last received from this scope: the counter style environment it registered, and the
    // scope a name it does not register is looked for in next.
    mutable Optional<u64> m_published_counter_style_environment_identity;
    mutable Optional<TreeScopeID> m_published_parent_counter_style_scope;
    // The counter styles this scope registers, which Rust holds; null when it registers none.
    Parser::ValueParserFFI::RegisteredCounterStyles const* m_counter_styles { nullptr };

    GC::Ref<DOM::Node> m_node;

private:
    void build_rule_cache();
    void add_rules_to_rule_cache(CascadeOrigin);

    Optional<StyleRuleCache> m_rule_cache;

    [[nodiscard]] StyleScope* parent_counter_style_scope() const;
    using CounterStyleLookupChain = Vector<Parser::ValueParserFFI::RegisteredCounterStyles const*, 4>;
    [[nodiscard]] CounterStyleLookupChain counter_style_lookup_chain(Layout::BegunRead const& read) const;
    void publish_counter_styles_if_changed() const;

    void add_sheet(StyleSheetState&, StyleEngineUpdate);
    void remove_sheet(StyleSheetState&, StyleEngineUpdate);
    void insert_sheet_in_tree_order(StyleSheetState&);
    StyleSheetState* following_sheet(StyleSheetState&);

    Vector<NonnullRefPtr<StyleSheetState>> m_style_sheets;
    // https://www.w3.org/TR/cssom/#preferred-css-style-sheet-set-name
    Utf16String m_preferred_css_style_sheet_set_name;
    // https://www.w3.org/TR/cssom/#last-css-style-sheet-set-name
    Optional<Utf16String> m_last_css_style_sheet_set_name;
};

}
