/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/CounterStyleDefinition.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom/#remove-a-css-style-sheet
void StyleScope::remove_a_css_style_sheet(CSS::StyleSheetState& sheet, StyleEngineUpdate style_engine_update)
{
    NonnullRefPtr keep_alive { sheet };
    // 1. Remove the CSS style sheet from the list of document or shadow root CSS style sheets.
    remove_sheet(sheet, style_engine_update);

    // 2. Set the CSS style sheet’s parent CSS style sheet, owner node and owner CSS rule to null.
    sheet.set_parent_css_style_sheet(nullptr);
    sheet.set_owner_node(nullptr);
    sheet.set_owner_import(nullptr);
}

// https://www.w3.org/TR/cssom/#add-a-css-style-sheet
void StyleScope::add_a_css_style_sheet(CSS::StyleSheetState& sheet, StyleEngineUpdate style_engine_update)
{
    // 1. Add the CSS style sheet to the list of document or shadow root CSS style sheets at the appropriate location. The remainder of these steps deal with the disabled flag.
    add_sheet(sheet, style_engine_update);

    // 2. If the disabled flag is set, then return.
    if (sheet.disabled())
        return;

    // 3. If the title is not the empty string, the alternate flag is unset, and preferred CSS style sheet set name is the empty string change the preferred CSS style sheet set name to the title.
    if (!sheet.title().is_empty() && !sheet.is_alternate() && m_preferred_css_style_sheet_set_name.is_empty()) {
        m_preferred_css_style_sheet_set_name = sheet.title();
    }

    // 4. If any of the following is true, then unset the disabled flag and return:
    //    - The title is the empty string.
    //    - The last CSS style sheet set name is null and the title is a case-sensitive match for the preferred CSS style sheet set name.
    //    - The title is a case-sensitive match for the last CSS style sheet set name.
    // NOTE: We don't enable alternate sheets with an empty title.  This isn't directly mentioned in the algorithm steps, but the
    // HTML specification says that the title element must be specified with a non-empty value for alternative style sheets.
    // See: https://html.spec.whatwg.org/multipage/links.html#the-link-is-an-alternative-stylesheet
    if ((sheet.title().is_empty() && !sheet.is_alternate())
        || (!sheet.title().is_empty()
            && ((!m_last_css_style_sheet_set_name.has_value() && sheet.title().equals_ignoring_ascii_case(m_preferred_css_style_sheet_set_name))
                || (m_last_css_style_sheet_set_name.has_value() && sheet.title().equals_ignoring_ascii_case(m_last_css_style_sheet_set_name.value()))))) {
        sheet.set_disabled(false);
        return;
    }

    // 5. Set the disabled flag.
    sheet.set_disabled(true);
}

// https://www.w3.org/TR/cssom/#create-a-css-style-sheet
NonnullRefPtr<StyleSheetState> StyleScope::create_a_css_style_sheet(Utf16View css_text, DOM::Element* owner_node, Utf16View media, Utf16String title, Alternate alternate, OriginClean origin_clean, Optional<::URL::URL> location, StyleSheetState* parent_style_sheet, StyleSheetImport* owner_import, StyleEngineUpdate style_engine_update)
{
    // 1. Create a new CSS style sheet object and set its properties as specified.
    // AD-HOC: The spec never tells us when to parse this style sheet, but the most logical place is here.
    auto sheet = parse_css_stylesheet(Parser::ParsingParams { document() }, css_text, location);
    initialize_a_css_style_sheet(*sheet, owner_node, media, move(title), alternate, origin_clean, parent_style_sheet, owner_import, style_engine_update);
    return sheet;
}

void StyleScope::initialize_a_css_style_sheet(StyleSheetState& sheet, DOM::Element* owner_node, Utf16View media, Utf16String title, Alternate alternate, OriginClean origin_clean, StyleSheetState* parent_style_sheet, StyleSheetImport* owner_import, StyleEngineUpdate style_engine_update)
{
    sheet.set_parent_css_style_sheet(parent_style_sheet);
    sheet.set_owner_import(owner_import);
    sheet.set_owner_node(owner_node);
    sheet.set_media(move(media));
    sheet.set_title(move(title));
    sheet.set_alternate(alternate == Alternate::Yes);
    sheet.set_origin_clean(origin_clean == OriginClean::Yes);

    // 2. Then run the add a CSS style sheet steps for the newly created CSS style sheet.
    add_a_css_style_sheet(sheet, style_engine_update);
}

void StyleScope::add_sheet(StyleSheetState& sheet, StyleEngineUpdate style_engine_update)
{
    sheet.add_owning_document_or_shadow_root(node());

    sheet.load_pending_image_resources(document());

    insert_sheet_in_tree_order(sheet);
    document().fonts()->synchronize_css_connected_font_order();

    if (style_engine_update == StyleEngineUpdate::Record)
        record_stylesheet_attached(sheet, node(), following_sheet(sheet));

    // NOTE: We evaluate media queries immediately when adding a new sheet.
    //       This coalesces the full document style invalidations.
    //       If we don't do this, we invalidate now, and then again when Document updates media rules.
    sheet.evaluate_media_queries(document());
    // A sheet whose media does not match contributes nothing, so its rules can reach no element.
    if (style_engine_update == StyleEngineUpdate::Record)
        record_stylesheet_conditions(sheet, node(), !sheet.disabled() && sheet.native_media_list().matches());

    invalidate_rule_cache_after_style_sheet_change(node(), sheet);
}

void StyleScope::insert_sheet_in_tree_order(StyleSheetState& sheet)
{
    if (m_style_sheets.is_empty()) {
        // This is the first sheet, append it to the list.
        m_style_sheets.append(sheet);
    } else {
        // We have sheets from before. Insert the new sheet in the correct position (DOM tree order).
        bool did_insert = false;
        for (ssize_t i = m_style_sheets.size() - 1; i >= 0; --i) {
            auto& existing_sheet = *m_style_sheets[i];
            auto position = existing_sheet.owner_node()->compare_document_position(sheet.owner_node());
            if (position & DOM::Node::DocumentPosition::DOCUMENT_POSITION_FOLLOWING) {
                m_style_sheets.insert(i + 1, sheet);
                did_insert = true;
                break;
            }
        }
        if (!did_insert)
            m_style_sheets.prepend(sheet);
    }
}

StyleSheetState* StyleScope::following_sheet(StyleSheetState& sheet)
{
    // The list is kept in DOM tree order, so the sheet that now follows this one is its successor
    // in cascade order too.
    auto position = m_style_sheets.find_first_index(sheet);
    StyleSheetState* following = nullptr;
    if (position.has_value() && *position + 1 < m_style_sheets.size())
        following = m_style_sheets[*position + 1].ptr();

    // https://drafts.csswg.org/cssom/#documentorshadowroot-final-css-style-sheets
    // Adopted sheets cascade after every sheet in the list, whenever either arrived, so the last
    // sheet of the list is followed by the first adopted one rather than by nothing.
    if (!following) {
        auto& scope = node();
        auto adopted = is<DOM::Document>(scope)
            ? DOM::AdoptedStyleSheetsAccess::adopted_style_sheets(static_cast<DOM::Document&>(scope))
            : DOM::AdoptedStyleSheetsAccess::adopted_style_sheets(static_cast<DOM::ShadowRoot&>(scope));
        DOM::for_each_adopted_style_sheet(adopted, [&](StyleSheetState& adopted_sheet) {
            if (!following)
                following = &adopted_sheet;
        });
    }
    return following;
}

void StyleScope::remove_sheet(StyleSheetState& sheet, StyleEngineUpdate style_engine_update)
{
    NonnullRefPtr keep_alive { sheet };
    sheet.remove_owning_document_or_shadow_root(node());
    bool did_remove = m_style_sheets.remove_first_matching([&](auto& entry) { return entry.ptr() == &sheet; });
    VERIFY(did_remove);
    if (style_engine_update == StyleEngineUpdate::Record)
        record_stylesheet_detached(sheet, node());

    invalidate_rule_cache_after_style_sheet_change(node(), sheet);
}

void StyleScope::move_sheet(StyleSheetState& sheet, StyleScope& destination)
{
    NonnullRefPtr keep_alive { sheet };
    if (this != &destination) {
        remove_sheet(sheet, StyleEngineUpdate::Record);
        destination.add_sheet(sheet, StyleEngineUpdate::Record);
        return;
    }

    auto position = m_style_sheets.find_first_index(sheet);
    VERIFY(position.has_value());
    m_style_sheets.remove(*position);
    insert_sheet_in_tree_order(sheet);
    document().fonts()->synchronize_css_connected_font_order();
    record_stylesheet_attached(sheet, node(), following_sheet(sheet));
    invalidate_rule_cache_after_style_sheet_change(node(), sheet);
}

NonnullRefPtr<StyleCache> StyleCache::create()
{
    return adopt_ref(*new StyleCache);
}

void StyleScope::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_node);
    visitor.visit(m_user_style_sheet);
    visitor.visit(m_style_sheets);
}

StyleScope::StyleScope(GC::Ref<DOM::Node> node)
    : m_node(node)
{
}

StyleScope::~StyleScope() = default;

bool SheetSetStyleCacheRegistry::entry_is_current(Entry const& entry)
{
    for (size_t i = 0; i < entry.sheets.size(); ++i) {
        if (entry.sheets[i]->shared_style_cache_generation() != entry.sheet_generations[i])
            return false;
    }
    return true;
}

static u32 hash_sheet_set(Vector<NonnullRefPtr<StyleSheetState>> const& sheets)
{
    u32 hash = u64_hash(sheets.size());
    for (auto const& sheet : sheets)
        hash = pair_int_hash(hash, ptr_hash(sheet.ptr()));
    return hash;
}

NonnullRefPtr<StyleCache> SheetSetStyleCacheRegistry::ensure_style_cache_for_sheet_set(Vector<NonnullRefPtr<StyleSheetState>> const& sheets)
{
    auto hash = hash_sheet_set(sheets);
    if (auto entries = m_entries_by_hash.get(hash); entries.has_value()) {
        entries->remove_all_matching([](Entry const& entry) { return !entry_is_current(entry); });
        for (auto& entry : *entries) {
            if (entry.sheets == sheets)
                return entry.style_cache;
        }
    }

    // NB: Entries are only revalidated when their bucket is consulted, so purge entries registry-wide on every
    //     insert: stale ones, and current ones whose cache no longer has any scope using it (the registry holds
    //     the only reference). Otherwise abandoned sheet sets would keep their sheets and built caches alive
    //     through visit_edges for the lifetime of the document. Inserts only happen once per distinct sheet set
    //     and generation, so this walk stays rare.
    m_entries_by_hash.remove_all_matching([](auto&, Vector<Entry>& entries) {
        entries.remove_all_matching([](Entry const& entry) { return !entry_is_current(entry) || entry.style_cache->ref_count() == 1; });
        return entries.is_empty();
    });

    Entry entry {
        .sheets = sheets,
        .sheet_generations = {},
        .style_cache = StyleCache::create(),
    };
    entry.sheet_generations.ensure_capacity(sheets.size());
    for (auto const& sheet : sheets)
        entry.sheet_generations.append(sheet->shared_style_cache_generation());

    auto style_cache = entry.style_cache;
    m_entries_by_hash.ensure(hash).append(move(entry));
    return style_cache;
}

void SheetSetStyleCacheRegistry::visit_edges(GC::Cell::Visitor& visitor)
{
    for (auto& [hash, entries] : m_entries_by_hash) {
        for (auto& entry : entries) {
            visitor.visit(entry.sheets);
        }
    }
}

StyleCache& StyleScope::ensure_style_cache()
{
    if (m_style_cache)
        return *m_style_cache;

    // NB: A quirks-mode scope folds id and class name case into its bucket keys, and neither shared cache
    //     below keys on that, so such a scope keeps its own cache.
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(*m_node); shadow_root && !document().page().user_style().has_value() && !document().in_quirks_mode()) {
        Vector<NonnullRefPtr<StyleSheetState>> sheets;
        bool all_sheets_are_constructed = true;
        shadow_root->for_each_active_css_style_sheet([&](StyleSheetState& style_sheet) {
            if (!style_sheet.constructed())
                all_sheets_are_constructed = false;
            else
                sheets.append(style_sheet);
        });

        if (all_sheets_are_constructed && !sheets.is_empty()) {
            if (sheets.size() == 1) {
                m_style_cache = sheets.first()->shared_single_constructed_sheet_style_cache();
                return *m_style_cache;
            }

            // OPTIMIZATION: Scopes whose active stylesheets are the same ordered set of constructed sheets can
            //               share one cache, for the same reason the single-constructed-sheet cache above is
            //               shareable: the contents only depend on the sheets and document-wide state.
            m_style_cache = document().sheet_set_style_cache_registry().ensure_style_cache_for_sheet_set(sheets);
            return *m_style_cache;
        }
    }

    m_style_cache = StyleCache::create();
    return *m_style_cache;
}

StyleCache& StyleScope::ensure_style_cache() const
{
    return const_cast<StyleScope&>(*this).ensure_style_cache();
}

void StyleScope::build_rule_cache()
{
    auto& style_cache = ensure_style_cache();

    if (!style_cache.rule_cache) {
        ++document().style_invalidation_counters().scope_rule_cache_builds;

        style_cache.rule_cache = make<StyleRuleCache>();
        ++style_cache.rule_cache_generation;
        populate_rule_cache(*style_cache.rule_cache);
    }

    // A constructed-sheet cache can be shared by several shadow scopes. Its keyframes and other
    // reference data are reusable, but each scope owns a distinct engine layer order and publishes
    // this cache generation into that slot once.
    if (m_published_layer_order_generation != style_cache.rule_cache_generation) {
        publish_cascade_layer_order();
        m_published_layer_order_generation = style_cache.rule_cache_generation;
    }
}

void StyleScope::populate_rule_cache(StyleRuleCache& rule_cache)
{
    build_user_style_sheet_if_needed();

    // A user-agent sheet is a process-wide singleton with no owning document, so nothing that walks a
    // document's own sheets ever evaluates its media rules. Its `@media` answers are still per
    // document - `(scripting)` is - so they are evaluated here, where the rule cache that consumes
    // them is built. Without this the cache is built against whatever state some other document
    // happened to leave behind, and `noscript` keeps the UA sheet's `display: none` only by accident.
    for (auto origin : { CascadeOrigin::UserAgent, CascadeOrigin::User }) {
        for_each_stylesheet(origin, [&](StyleSheetState& sheet) {
            sheet.evaluate_media_queries(document());
        });
    }

    make_rule_cache_for_cascade_origin(CascadeOrigin::Author, rule_cache);
    make_rule_cache_for_cascade_origin(CascadeOrigin::User, rule_cache);
    make_rule_cache_for_cascade_origin(CascadeOrigin::UserAgent, rule_cache);
}

void StyleScope::invalidate_style_cache()
{
    invalidate_counter_style_cache();
    m_style_cache = nullptr;
    m_published_layer_order_generation = 0;
    // The registered custom properties cache is built from the document's active stylesheets, so it only needs a
    // rebuild when the document scope's rule set changes.
    if (m_node->is_document())
        document().set_needs_registered_properties_cache_update();
}

void StyleScope::invalidate_user_style_sheet()
{
    m_user_style_sheet = nullptr;
    invalidate_style_cache();
}

void StyleScope::build_user_style_sheet_if_needed()
{
    if (m_user_style_sheet)
        return;

    if (!is<DOM::Document>(*m_node))
        return;

    auto user_style_source = document().page().user_style();
    auto const& content_blocker_style_source = document().content_blocker_style_sheet();
    if (!user_style_source.has_value() && content_blocker_style_source.is_empty())
        return;

    Utf16StringBuilder source;
    if (user_style_source.has_value())
        source.append(user_style_source->utf16_view());
    if (!content_blocker_style_source.is_empty()) {
        if (!source.is_empty())
            source.append_ascii('\n');
        source.append(content_blocker_style_source.utf16_view());
    }

    m_user_style_sheet = parse_css_stylesheet(CSS::Parser::ParsingParams(document()), source.view());
}

void StyleScope::build_rule_cache_if_needed() const
{
    if (has_valid_rule_cache() && m_published_layer_order_generation == m_style_cache->rule_cache_generation)
        return;
    const_cast<StyleScope&>(*this).build_rule_cache();
}

StyleRuleCache const& StyleScope::rule_cache() const
{
    build_rule_cache_if_needed();
    return *m_style_cache->rule_cache;
}

static StyleSheetState& default_stylesheet()
{
    static auto& sheet = *new RefPtr<StyleSheetState>;
    if (!sheet) {
        extern String const& default_stylesheet_source;
        sheet = parse_css_stylesheet(CSS::Parser::ParsingParams(Parser::IsUAStyleSheet::Yes), default_stylesheet_source);
    }
    return *sheet;
}

static StyleSheetState& quirks_mode_stylesheet()
{
    static auto& sheet = *new RefPtr<StyleSheetState>;
    if (!sheet) {
        extern String const& quirks_mode_stylesheet_source;
        sheet = parse_css_stylesheet(CSS::Parser::ParsingParams(Parser::IsUAStyleSheet::Yes), quirks_mode_stylesheet_source);
    }
    return *sheet;
}

static StyleSheetState& mathml_stylesheet()
{
    static auto& sheet = *new RefPtr<StyleSheetState>;
    if (!sheet) {
        extern String const& mathml_stylesheet_source;
        sheet = parse_css_stylesheet(CSS::Parser::ParsingParams(Parser::IsUAStyleSheet::Yes), mathml_stylesheet_source);
    }
    return *sheet;
}

static StyleSheetState& svg_stylesheet()
{
    static auto& sheet = *new RefPtr<StyleSheetState>;
    if (!sheet) {
        extern String const& svg_stylesheet_source;
        sheet = parse_css_stylesheet(CSS::Parser::ParsingParams(Parser::IsUAStyleSheet::Yes), svg_stylesheet_source);
    }
    return *sheet;
}

void StyleScope::for_each_user_agent_stylesheet(bool include_quirks_mode_stylesheet, bool include_mathml_and_svg_stylesheets, Function<void(CSS::StyleSheetState&, StyleSheetIdentifier const&)> const& callback)
{
    auto callback_with_identifier = [&](StyleSheetState& sheet, Utf16String url) {
        StyleSheetIdentifier identifier {
            .type = StyleSheetIdentifier::Type::UserAgent,
            .url = move(url),
        };
        callback(sheet, identifier);
    };

    callback_with_identifier(default_stylesheet(), "CSS/Default.css"_utf16);
    if (include_quirks_mode_stylesheet)
        callback_with_identifier(quirks_mode_stylesheet(), "CSS/QuirksMode.css"_utf16);
    // Both sheets declare a default namespace, so every selector in them - the universal one
    // included - is restricted to the namespace it belongs to. Neither decides anything for a
    // document with no element in it, and every element of every page was evaluating them.
    if (include_mathml_and_svg_stylesheets) {
        callback_with_identifier(mathml_stylesheet(), "MathML/Default.css"_utf16);
        callback_with_identifier(svg_stylesheet(), "SVG/Default.css"_utf16);
    }
}

Optional<StyleSheetIdentifier> StyleScope::user_agent_style_sheet_identifier(CSS::StyleSheetState const& style_sheet)
{
    Optional<StyleSheetIdentifier> identifier;
    for_each_user_agent_stylesheet(true, true, [&](auto& user_agent_style_sheet, auto const& user_agent_style_sheet_identifier) {
        if (&style_sheet == &user_agent_style_sheet)
            identifier = user_agent_style_sheet_identifier;
    });
    return identifier;
}

void StyleScope::for_each_stylesheet(CascadeOrigin cascade_origin, Function<void(CSS::StyleSheetState&)> const& callback) const
{
    if (cascade_origin == CascadeOrigin::UserAgent) {
        for_each_user_agent_stylesheet(document().in_quirks_mode(), document().needs_mathml_and_svg_user_agent_style_sheets(), [&](auto& sheet, auto const&) {
            callback(sheet);
        });
    }
    if (cascade_origin == CascadeOrigin::User) {
        auto& style_scope = const_cast<StyleScope&>(*this);
        style_scope.build_user_style_sheet_if_needed();
        if (style_scope.m_user_style_sheet)
            callback(*style_scope.m_user_style_sheet);
    }
    if (cascade_origin == CascadeOrigin::Author) {
        for_each_active_css_style_sheet(move(callback));
    }
}

void StyleScope::make_rule_cache_for_cascade_origin(CascadeOrigin cascade_origin, StyleRuleCache& rule_cache)
{
    for_each_stylesheet(cascade_origin, [&](auto& sheet) {
        sheet.for_each_effective_rule_data(TraversalOrder::Preorder, [&](RustRuleView const& rule, Utf16View layer_prefix) {
            if (rule.type() == RustRule::Type::Container && Parser::ValueParserFFI::rust_container_conditions_contains_size_feature(rule.container()))
                rule_cache.has_size_container_queries = true;
            if (rule.type() == RustRule::Type::Function) {
                auto function = rule.compile_function();
                auto name = Parser::ValueParserFFI::rust_function_signature_view(function.signature()).name;
                rule_cache.function_rules_by_name.ensure(Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(name.utf16), name.length })).append({ move(function), Utf16FlyString::from_utf16(layer_prefix), cascade_origin });
            }
            if (rule.type() != RustRule::Type::Keyframes)
                return;

            // Loosely based on https://drafts.csswg.org/css-animations-2/#keyframe-processing
            auto name = rule.name();
            auto keyframe_set = adopt_ref(*new Animations::KeyframeEffect::KeyFrameSet);
            auto base_url = sheet.style_resource_base_url();
            keyframe_set->style_sheet_resource_context = {
                .base_url = base_url.has_value() ? base_url->to_string() : String {},
                .origin_clean = sheet.is_origin_clean(),
            };
            HashTable<PropertyNameAndID> animated_properties;

            // Forwards pass, resolve all the user-specified keyframe properties.
            Function<void(ReadonlySpan<double>, RustDeclarationBlockSnapshot const&)> append_keyframe = [&](ReadonlySpan<double> keys, RustDeclarationBlockSnapshot const& keyframe_style) {
                Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame resolved_keyframe;

                auto append_property = [&](Parser::ValueParserFFI::FfiDeclaredProperty const& declaration) {
                    auto* value = static_cast<StyleValueFFI::StyleValueData const*>(declaration.value);
                    if (declaration.name.length != 0) {
                        auto name = Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(declaration.name.utf16), declaration.name.length });
                        auto property = PropertyNameAndID::from_name(name);
                        if (!property.has_value())
                            return;
                        animated_properties.set(*property);
                        resolved_keyframe.properties.set(*property, RustStyleValueHandle::retained(value));
                        return;
                    }
                    auto property_id = static_cast<PropertyID>(declaration.property_id);
                    if (property_id == PropertyID::AnimationTimingFunction) {
                        // animation-timing-function is a list property, but inside @keyframes only
                        // a single value is meaningful.
                        if (value->tag == StyleValueFFI::StyleValueData::Tag::ValueList) {
                            auto const& list = value->value_list.values;
                            if (list.length == 0)
                                return;
                            value = static_cast<StyleValueFFI::StyleValueData const*>(list.pointer[0].pointer);
                        }
                        if (value->tag == StyleValueFFI::StyleValueData::Tag::Easing || value->tag == StyleValueFFI::StyleValueData::Tag::Keyword) {
                            auto easing_value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value));
                            resolved_keyframe.easing = EasingFunction::from_style_value(*easing_value);
                        } else {
                            resolved_keyframe.easing = RustStyleValueHandle::retained(value);
                        }
                        return;
                    }
                    if (property_id == PropertyID::AnimationComposition) {
                        AnimationComposition composition = AnimationComposition::Replace;
                        if (value->tag == StyleValueFFI::StyleValueData::Tag::ValueList && value->value_list.values.length == 1)
                            value = static_cast<StyleValueFFI::StyleValueData const*>(value->value_list.values.pointer[0].pointer);
                        if (value->tag == StyleValueFFI::StyleValueData::Tag::Keyword) {
                            if (static_cast<Keyword>(value->keyword.keyword) == Keyword::Add)
                                composition = AnimationComposition::Add;
                            else if (static_cast<Keyword>(value->keyword.keyword) == Keyword::Accumulate)
                                composition = AnimationComposition::Accumulate;
                        }
                        resolved_keyframe.composite = Animations::css_animation_composition_to_composite_operation_or_auto(composition);
                        return;
                    }
                    if (!is_animatable_property(property_id))
                        return;

                    // Unresolved properties will be resolved in collect_animation_into()
                    auto expansion = ComputedValuesFFI::rust_expand_property_shorthands(
                        to_underlying(property_id), value);
                    for (size_t i = 0; i < expansion.count; ++i) {
                        auto const& property = expansion.properties[i];
                        auto longhand_property = PropertyNameAndID::from_id(static_cast<PropertyID>(property.property_id));
                        animated_properties.set(longhand_property);
                        resolved_keyframe.properties.set(longhand_property,
                            RustStyleValueHandle::retained(static_cast<StyleValueFFI::StyleValueData const*>(property.data)));
                    }
                    ComputedValuesFFI::rust_shorthand_expansion_destroy(expansion.storage);
                };
                Parser::ValueParserFFI::rust_declaration_data_visit(keyframe_style.data(), &append_property, [](void* context, Parser::ValueParserFFI::FfiDeclaredProperty const* property) {
                    (*static_cast<decltype(append_property)*>(context))(*property);
                });

                for (auto key : keys) {
                    auto resolved_key = static_cast<u64>(key * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor);

                    if (auto* existing_keyframe = keyframe_set->keyframes_by_key.find(resolved_key)) {
                        for (auto& [property, value] : resolved_keyframe.properties)
                            existing_keyframe->properties.set(property, value);
                        if (resolved_keyframe.composite != Bindings::CompositeOperationOrAuto::Auto)
                            existing_keyframe->composite = resolved_keyframe.composite;
                        if (!resolved_keyframe.easing.has<Empty>())
                            existing_keyframe->easing = resolved_keyframe.easing;
                    } else {
                        keyframe_set->keyframes_by_key.insert(resolved_key, resolved_keyframe);
                    }
                }
            };
            rule.for_each_keyframe(append_keyframe);

            Animations::KeyframeEffect::generate_initial_and_final_frames(keyframe_set, animated_properties);

            if constexpr (LIBWEB_CSS_DEBUG) {
                dbgln("Resolved keyframe set '{}' into {} keyframes:", name, keyframe_set->keyframes_by_key.size());
                for (auto it = keyframe_set->keyframes_by_key.begin(); it != keyframe_set->keyframes_by_key.end(); ++it)
                    dbgln("    - keyframe {}: {} properties", it.key(), it->properties.size());
            }

            rule_cache.rules_by_animation_keyframes.set(Utf16FlyString { name }, move(keyframe_set));
        });
    });
}

void StyleScope::publish_cascade_layer_order(StyleSheetState* pending_attachment)
{
    Vector<Parser::ValueParserFFI::NativeStyleSheet const*> sheets;
    // An adopted sheet is announced before ObservableArray stores it. Include that pending
    // attachment so its layer order crosses the same transaction boundary as its rules.
    for_each_stylesheet(CascadeOrigin::Author, [&](auto& sheet) {
        if (&sheet == pending_attachment)
            pending_attachment = nullptr;
        sheets.append(sheet.native_sheet().handle());
    });
    if (pending_attachment && !pending_attachment->disabled() && pending_attachment->native_media_list().matches())
        sheets.append(pending_attachment->native_sheet().handle());

    m_has_published_named_layer_order = Parser::ValueParserFFI::rust_style_sheet_publish_layer_order(
        sheets.data(), sheets.size(), document().style_computer().style_engine().rust_handle(),
        style_engine_tree_scope().value(), m_has_published_named_layer_order, &document(),
        [](void* document) { static_cast<DOM::Document*>(document)->flush_deferred_style_change_event(); });
}

TreeScopeID StyleScope::style_engine_tree_scope() const
{
    return style_engine_tree_scope_for(*m_node);
}

void StyleScope::invalidate_counter_style_cache()
{
    m_needs_counter_style_cache_update = true;

    // FIXME: We only need to invalidate this style scope and those belonging to descendant shadow roots (since they may
    //        include counter styles which extend the ones defined in this scope), not all style scopes in the document.
    m_node->document().style_scope().m_needs_counter_style_cache_update = true;
    m_node->document().for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) {
        shadow_root.style_scope().m_needs_counter_style_cache_update = true;
    });
}

void StyleScope::build_counter_style_cache()
{
    m_is_doing_counter_style_cache_update = true;

    // Counter styles can be resolved before any keyframe or function lookup builds the rule cache.
    // Publish this scope's layer order before comparing definitions from different layers.
    build_rule_cache_if_needed();

    // A rebuild is triggered by any sheet arriving, and almost every rebuild produces the same
    // counter styles it produced last time. A style names the one it resolved to by identity, so
    // minting a fresh object for an unchanged style makes every element's inherited list group new,
    // which is what a child reads of its parent.
    auto previously_registered_counter_styles = move(m_registered_counter_styles);
    m_registered_counter_styles.clear_with_capacity();

    auto register_counter_style = [&](Utf16FlyString const& name, NonnullRefPtr<CSS::CounterStyle const> counter_style) {
        if (auto previous = previously_registered_counter_styles.get(name); previous.has_value() && previous.value()->equals(*counter_style)) {
            m_registered_counter_styles.set(name, *previous.value());
            return;
        }
        m_registered_counter_styles.set(name, move(counter_style));
    };

    HashMap<Utf16FlyString, CSS::CounterStyleDefinition> counter_style_definitions;
    struct CounterStylePriority {
        u8 origin;
        u32 layer;
    };
    HashMap<Utf16FlyString, CounterStylePriority> counter_style_priorities;

    auto const define_complex_predefined_counter_styles = [&]() {
        // https://drafts.csswg.org/css-counter-styles-3/#complex-predefined-counters
        // While authors may define their own counter styles using the @counter-style rule or rely on the set of
        // predefined counter styles, a few counter styles are described by rules that are too complex to be captured by
        // the predefined algorithms.

        // FIXME: All of the counter styles defined in this section have a spoken form of numbers

        // https://drafts.csswg.org/css-counter-styles-3/#ethiopic-numeric-counter-style
        // For this system, the name is "ethiopic-numeric", the range is 1 infinite, the suffix is "/ " (U+002F SOLIDUS
        // followed by a U+0020 SPACE), and the rest of the descriptors have their initial value.
        counter_style_definitions.set(
            "ethiopic-numeric"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "ethiopic-numeric"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::EthiopicNumericCounterStyleAlgorithm {} },
                {},
                {},
                "/ "_utf16_fly_string,
                Vector<CSS::CounterStyleRangeEntry> { { 1, AK::NumericLimits<i32>::max() } },
                {},
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#extended-range-optional
        // For all of these counter styles, the descriptors are the same as for the limited range variants, except for
        // the range, which is calc(-1 * pow(10, 16) + 1) calc(pow(10, 16) - 1).
        // AD-HOC: Ranges (as with all other CSS <integer>s are limited to i32 range)
        Vector<CSS::CounterStyleRangeEntry> extended_cjk_range { { AK::clamp_to<i32>(-9999999999999999), AK::clamp_to<i32>(9999999999999999) } };

        // https://drafts.csswg.org/css-counter-styles-3/#limited-chinese
        // For all of these counter styles, the suffix is "、" U+3001, the fallback is cjk-decimal, the range is -9999
        // 9999, and the negative value is given in the table of symbols for each style.

        //                  simp-chinese-informal simp-chinese-formal trad-chinese-informal trad-chinese-formal
        // Negative Sign    负 U+8D1F             负 U+8D1F           負 U+8CA0              負 U+8CA0

        // https://drafts.csswg.org/css-counter-styles-3/#simp-chinese-informal
        // simp-chinese-informal
        counter_style_definitions.set(
            "simp-chinese-informal"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "simp-chinese-informal"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseInformal } },
                CSS::CounterStyleNegativeSign { "\U00008D1F"_utf16_fly_string, ""_utf16_fly_string },
                {},
                "\U00003001"_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#simp-chinese-formal
        // simp-chinese-formal
        counter_style_definitions.set(
            "simp-chinese-formal"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "simp-chinese-formal"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseFormal } },
                CSS::CounterStyleNegativeSign { "\U00008D1F"_utf16_fly_string, ""_utf16_fly_string },
                {},
                "\U00003001"_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#trad-chinese-informal
        // trad-chinese-informal
        counter_style_definitions.set(
            "trad-chinese-informal"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "trad-chinese-informal"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::TradChineseInformal } },
                CSS::CounterStyleNegativeSign { "\U00008CA0"_utf16_fly_string, ""_utf16_fly_string },
                {},
                "\U00003001"_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#trad-chinese-formal
        // trad-chinese-formal
        counter_style_definitions.set(
            "trad-chinese-formal"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "trad-chinese-formal"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::TradChineseFormal } },
                CSS::CounterStyleNegativeSign { "\U00008CA0"_utf16_fly_string, ""_utf16_fly_string },
                {},
                "\U00003001"_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#cjk-ideographic
        // cjk-ideographic
        // This counter style is identical to trad-chinese-informal. (It exists for legacy reasons.)
        counter_style_definitions.set(
            "cjk-ideographic"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "cjk-ideographic"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::TradChineseInformal } },
                CSS::CounterStyleNegativeSign { "\U00008CA0"_utf16_fly_string, ""_utf16_fly_string },
                {},
                "\U00003001"_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#japanese-informal
        // japanese-informal
        counter_style_definitions.set(
            "japanese-informal"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "japanese-informal"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::JapaneseInformal } },
                CSS::CounterStyleNegativeSign { "\U000030DE\U000030A4\U000030CA\U000030B9"_utf16_fly_string, ""_utf16_fly_string },
                {},
                "\U00003001"_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#japanese-formal
        // japanese-formal
        counter_style_definitions.set(
            "japanese-formal"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "japanese-formal"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::JapaneseFormal } },
                CSS::CounterStyleNegativeSign { "\U000030DE\U000030A4\U000030CA\U000030B9"_utf16_fly_string, ""_utf16_fly_string },
                {},
                "\U00003001"_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#korean-hangul-formal
        // korean-hangul-formal
        counter_style_definitions.set(
            "korean-hangul-formal"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "korean-hangul-formal"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::KoreanHangulFormal } },
                CSS::CounterStyleNegativeSign { "\U0000B9C8\U0000C774\U0000B108\U0000C2A4 "_utf16_fly_string, ""_utf16_fly_string },
                {},
                ", "_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#korean-hanja-informal
        // korean-hanja-informal
        counter_style_definitions.set(
            "korean-hanja-informal"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "korean-hanja-informal"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::KoreanHanjaInformal } },
                CSS::CounterStyleNegativeSign { "\U0000B9C8\U0000C774\U0000B108\U0000C2A4 "_utf16_fly_string, ""_utf16_fly_string },
                {},
                ", "_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#korean-hanja-formal
        // korean-hanja-formal
        counter_style_definitions.set(
            "korean-hanja-formal"_utf16_fly_string,
            CSS::CounterStyleDefinition::create(
                "korean-hanja-formal"_utf16_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::KoreanHanjaFormal } },
                CSS::CounterStyleNegativeSign { "\U0000B9C8\U0000C774\U0000B108\U0000C2A4 "_utf16_fly_string, ""_utf16_fly_string },
                {},
                ", "_utf16_fly_string,
                extended_cjk_range,
                "cjk-decimal"_utf16_fly_string,
                {}));
    };

    CSS::ComputationContext computation_context {
        .length_resolution_context = CSS::Length::ResolutionContext::for_document(document())
    };

    auto collect_counter_style_definitions = [&](CSS::CascadeOrigin cascade_origin, CSS::StyleSheetState const& style_sheet) {
        auto& style_engine = document().style_computer().style_engine();
        auto const tree_scope = style_engine_tree_scope();
        auto const origin_priority = [&]() -> u8 {
            switch (cascade_origin) {
            case CSS::CascadeOrigin::UserAgent:
                return 0;
            case CSS::CascadeOrigin::User:
                return 1;
            case CSS::CascadeOrigin::Author:
                return 2;
            default:
                VERIFY_NOT_REACHED();
            }
        }();
        style_sheet.for_each_effective_rule_data(TraversalOrder::Preorder, [&](RustRuleView const& rule, Utf16View layer_prefix) {
            if (rule.type() != RustRule::Type::CounterStyle)
                return;
            auto name = Utf16FlyString { rule.name() };
            auto qualified_layer_name = Utf16FlyString::from_utf16(layer_prefix);
            auto const layer = qualified_layer_name.is_empty() ? 0 : style_engine.intern_atom(qualified_layer_name).value();
            CounterStylePriority priority {
                .origin = origin_priority,
                .layer = style_engine.layer_index(tree_scope, layer),
            };
            if (auto existing = counter_style_priorities.get(name); existing.has_value()) {
                if (existing->origin > priority.origin || (existing->origin == priority.origin && existing->layer > priority.layer))
                    return;
            }
            if (auto const& definition = CSS::CounterStyleDefinition::from_descriptors(name.view(), rule.descriptors(), computation_context); definition.has_value()) {
                counter_style_definitions.set(definition->name(), *definition);
                counter_style_priorities.set(definition->name(), priority);
            }
        });
    };

    // NB: We should only register predefined counter styles in the document's style scope, this ensures overrides are
    //     correctly inherited by shadow roots.
    if (m_node->is_document()) {
        for_each_stylesheet(CSS::CascadeOrigin::UserAgent, [&](auto& sheet) { collect_counter_style_definitions(CSS::CascadeOrigin::UserAgent, sheet); });
        define_complex_predefined_counter_styles();
        for_each_stylesheet(CSS::CascadeOrigin::User, [&](auto& sheet) { collect_counter_style_definitions(CSS::CascadeOrigin::User, sheet); });
    }

    for_each_stylesheet(CSS::CascadeOrigin::Author, [&](auto& sheet) { collect_counter_style_definitions(CSS::CascadeOrigin::Author, sheet); });

    VERIFY(!m_node->is_document() || counter_style_definitions.contains("decimal"_utf16_fly_string));

    auto const is_part_of_extends_cycle = [&](Utf16FlyString const& counter_style_name) {
        HashTable<Utf16FlyString> visited;
        auto current_counter_style_name = counter_style_name;

        while (true) {
            if (visited.contains(current_counter_style_name))
                return true;

            visited.set(current_counter_style_name);

            auto const& current_definition = counter_style_definitions.get(current_counter_style_name);

            // NB: If we don't have a definition for this counter style it means it's either undefined in this scope
            //     (and will the counter style extending it will instead default to extending "decimal" instead) or it's
            //     defined in an outer style scope (and thus can't extend a counter style in the current scope), neither
            //     of which can lead to a cycle.
            if (!current_definition.has_value())
                return false;

            if (current_definition->algorithm().has<CSS::CounterStyleAlgorithm>())
                return false;

            current_counter_style_name = current_definition->algorithm().get<CSS::CounterStyleSystemStyleValue::Extends>().name;
        }

        VERIFY_NOT_REACHED();
    };

    // NB: We register non-extending counter styles immediately and then extending counter styles after we have
    //     registered their corresponding extended counter style.
    Vector<CSS::CounterStyleDefinition> extending_counter_styles;

    for (auto const& [name, definition] : counter_style_definitions) {
        // NB: We don't need to wait for this counter style's extended counter style to be registered since it doesn't
        //     have one - register it immediately.
        if (definition.algorithm().has<CSS::CounterStyleAlgorithm>()) {
            register_counter_style(name, CSS::CounterStyle::from_counter_style_definition(definition, *this));
            continue;
        }

        auto extends = definition.algorithm().get<CSS::CounterStyleSystemStyleValue::Extends>();

        if (is_part_of_extends_cycle(name)) {
            auto copied = definition;
            copied.set_algorithm(CSS::CounterStyleSystemStyleValue::Extends { "decimal"_utf16_fly_string });
            extending_counter_styles.append(copied);
        } else {
            extending_counter_styles.append(definition);
        }
    }

    // FIXME: This is O(n^2) in the worst case but we usually don't see many counter styles so it should be fine in practice.
    while (!extending_counter_styles.is_empty()) {
        for (size_t i = 0; i < extending_counter_styles.size(); ++i) {
            auto const& definition = extending_counter_styles.at(i);
            auto extends = definition.algorithm().get<CSS::CounterStyleSystemStyleValue::Extends>();

            auto const& extends_name = extends.name;
            if (!m_registered_counter_styles.contains(extends_name) && counter_style_definitions.contains(extends_name))
                continue;

            register_counter_style(definition.name(), CSS::CounterStyle::from_counter_style_definition(definition, *this));
            extending_counter_styles.remove(i);
            --i;
        }
    }

    bool counter_style_environment_changed = previously_registered_counter_styles.size() != m_registered_counter_styles.size();
    if (!counter_style_environment_changed) {
        for (auto const& [name, counter_style] : m_registered_counter_styles) {
            auto previous = previously_registered_counter_styles.get(name);
            if (!previous.has_value() || previous.value() != counter_style.ptr()) {
                counter_style_environment_changed = true;
                break;
            }
        }
    }
    if (counter_style_environment_changed)
        m_counter_style_environment_identity = document().next_counter_style_environment_identity();

    m_is_doing_counter_style_cache_update = false;
    m_needs_counter_style_cache_update = false;
}

u64 StyleScope::counter_style_environment_identity() const
{
    if (m_needs_counter_style_cache_update && !m_is_doing_counter_style_cache_update)
        const_cast<StyleScope*>(this)->build_counter_style_cache();
    return m_counter_style_environment_identity;
}

DOM::Document& StyleScope::document() const
{
    return m_node->document();
}

void StyleScope::for_each_active_css_style_sheet(Function<void(CSS::StyleSheetState&)> const& callback) const
{
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(*m_node)) {
        shadow_root->for_each_active_css_style_sheet(callback);
    } else {
        m_node->document().for_each_active_css_style_sheet(callback);
    }
}

RefPtr<CSS::CounterStyle const> StyleScope::get_registered_counter_style(Utf16FlyString const& name) const
{
    if (m_needs_counter_style_cache_update && !m_is_doing_counter_style_cache_update)
        const_cast<StyleScope*>(this)->build_counter_style_cache();

    return dereference_global_tree_scoped_reference<CSS::CounterStyle const*>([&](StyleScope const& scope) { return scope.m_registered_counter_styles.get(name); })
        .value_or(nullptr);
}

Optional<StyleScope::FunctionDefinitionAndScope> StyleScope::get_function_definition(Utf16FlyString const& name) const
{
    return dereference_global_tree_scoped_reference<FunctionDefinitionAndScope>([&](StyleScope const& scope) -> Optional<FunctionDefinitionAndScope> {
        auto const get_function_definition_for_cascade_origin = [&](CSS::CascadeOrigin cascade_origin) {
            RustCompiledFunction const* cascade_origin_result = nullptr;
            u32 existing_layer_index = 0;
            auto& style_engine = scope.document().style_computer().style_engine();
            auto const tree_scope = scope.style_engine_tree_scope();
            auto layer_index_of = [&](Utf16FlyString const& qualified_layer_name) {
                auto const layer = qualified_layer_name.is_empty() ? 0 : style_engine.intern_atom(qualified_layer_name);
                return style_engine.layer_index(tree_scope, layer.value());
            };

            auto cached_rules = scope.rule_cache().function_rules_by_name.get(name);
            if (!cached_rules.has_value())
                return cascade_origin_result;

            for (auto const& cached_rule : *cached_rules) {
                if (cached_rule.cascade_origin != cascade_origin)
                    continue;

                auto layer_index = layer_index_of(cached_rule.qualified_layer_name);
                if (!cascade_origin_result || layer_index >= existing_layer_index) {
                    cascade_origin_result = &cached_rule.rule;
                    existing_layer_index = layer_index;
                }
            }

            return cascade_origin_result;
        };

        RustCompiledFunction const* result = nullptr;

        if (scope.m_node->is_document()) {
            if (auto const* user_agent_result = get_function_definition_for_cascade_origin(CSS::CascadeOrigin::UserAgent))
                result = user_agent_result;

            if (auto const* user_result = get_function_definition_for_cascade_origin(CSS::CascadeOrigin::User))
                result = user_result;
        }

        if (auto const* author_result = get_function_definition_for_cascade_origin(CSS::CascadeOrigin::Author))
            result = author_result;

        if (!result)
            return OptionalNone {};

        return FunctionDefinitionAndScope { .function = *result, .scope = scope };
    });
}

void StyleScope::for_each_visible_function_definition(Function<void(FunctionDefinitionAndScope const&)> const& callback) const
{
    HashTable<Utf16FlyString> names;
    Function<void(StyleScope const&)> collect_names = [&](StyleScope const& scope) {
        for (auto const& [name, rules] : scope.rule_cache().function_rules_by_name) {
            (void)rules;
            names.set(name);
        }

        if (auto* shadow_root = as_if<DOM::ShadowRoot>(*scope.m_node)) {
            if (auto* host = shadow_root->host()) {
                auto const& root = host->root();
                if (root.is_shadow_root()) {
                    auto const& parent_shadow_root = as<DOM::ShadowRoot>(root);
                    if (parent_shadow_root.uses_document_style_sheets())
                        collect_names(root.document().style_scope());
                    else
                        collect_names(parent_shadow_root.style_scope());
                } else if (auto const* document = as_if<DOM::Document>(root)) {
                    collect_names(document->style_scope());
                }
            }
        }
    };
    collect_names(*this);

    for (auto const& name : names) {
        if (auto definition = get_function_definition(name); definition.has_value())
            callback(*definition);
    }
}

template<typename T>
Optional<T> StyleScope::dereference_global_tree_scoped_reference(Function<Optional<T>(StyleScope const&)> const& callback) const
{
    // https://drafts.csswg.org/css-shadow-1/#tree-scoped-name-global
    // If a tree-scoped name is global (such as @font-face names), then when a tree-scoped reference is dereferenced to
    // find it, first search only the tree-scoped names associated with the same root as the tree-scoped reference. If
    // no relevant tree-scoped name is found, and the root is a shadow root, then repeat this search in the root’s
    // host’s node tree (recursively). (In other words, global tree-scoped names “inherit” into descendant shadow trees,
    // so long as they don’t define the same name themselves.)
    if (auto result = callback(*this); result.has_value())
        return result;

    if (auto* shadow_root = as_if<DOM::ShadowRoot>(*m_node)) {
        if (auto* host = shadow_root->host()) {
            auto const& root = host->root();

            if (root.is_shadow_root()) {
                auto const& shadow_root = as<DOM::ShadowRoot>(root);
                if (shadow_root.uses_document_style_sheets())
                    return root.document().style_scope().dereference_global_tree_scoped_reference(callback);

                return shadow_root.style_scope().dereference_global_tree_scoped_reference(callback);
            }

            if (auto const* document = as_if<DOM::Document>(root))
                return document->style_scope().dereference_global_tree_scoped_reference(callback);

            // A detached host's node tree is rooted at an ordinary element. Such a tree carries no
            // tree-scoped names of its own, so the inheritance chain ends here.
            return OptionalNone {};
        }
    }

    return {};
}

}
