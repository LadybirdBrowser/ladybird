/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ValueParserRustFFI.h>

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

void StyleScope::attach_sheet_to_style_engine(StyleSheetState& sheet)
{
    record_stylesheet_attached(sheet, node(), following_sheet(sheet));
    record_stylesheet_conditions(sheet, node(), !sheet.disabled() && sheet.native_media_list().matches());
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

StyleScope::~StyleScope()
{
    Parser::ValueParserFFI::rust_counter_styles_release(m_counter_styles);
}

void StyleScope::build_rule_cache()
{
    if (!m_rule_cache.has_value()) {
        m_rule_cache = StyleRuleCache {};
        add_rules_to_rule_cache(CascadeOrigin::Author);

        // NB: A shadow root's scope holds only its own sheets. What the user and user-agent sheets define is found
        //     in the document's scope, where every name a shadow root's scope does not define is looked for next.
        if (m_node->is_document()) {
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
            add_rules_to_rule_cache(CascadeOrigin::User);
            add_rules_to_rule_cache(CascadeOrigin::UserAgent);
        }
    }

    if (!m_has_published_rule_cache) {
        publish_cascade_layer_order();
        publish_animation_keyframes();
        m_has_published_rule_cache = true;
    }
}

void StyleScope::invalidate_style_cache()
{
    document().note_style_sheet_set_change();
    invalidate_counter_style_cache();
    m_rule_cache.clear();
    m_has_published_rule_cache = false;
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
    if (m_rule_cache.has_value() && m_has_published_rule_cache)
        return;
    const_cast<StyleScope&>(*this).build_rule_cache();
}

StyleRuleCache const& StyleScope::rule_cache() const
{
    build_rule_cache_if_needed();
    return *m_rule_cache;
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

void StyleScope::add_rules_to_rule_cache(CascadeOrigin cascade_origin)
{
    for_each_stylesheet(cascade_origin, [&](auto& sheet) {
        if (!sheet.native_media_list().matches())
            return;
        // OPTIMIZATION: A constructed sheet may be adopted by many shadow roots. What it defines is collected once,
        //               on the sheet, and every scope that adopts it takes that in.
        if (sheet.constructed())
            m_rule_cache->add_rules_from_cache(sheet.rule_cache());
        else
            m_rule_cache->add_rules_from_sheet(sheet, cascade_origin);
    });
}

void StyleRuleCache::add_rules_from_cache(StyleRuleCache const& other)
{
    for (auto const& [name, keyframe_set] : other.rules_by_animation_keyframes)
        rules_by_animation_keyframes.set(name, keyframe_set);
    function_rules.extend(other.function_rules);
    has_size_container_queries |= other.has_size_container_queries;
}

void StyleRuleCache::add_rules_from_sheet(StyleSheetState& sheet, CascadeOrigin cascade_origin)
{
    auto& rule_sheet = sheet.shared_compiled_style_sheet() ? sheet.shared_compiled_style_sheet()->contents() : sheet;
    rule_sheet.for_each_effective_rule_data(TraversalOrder::Preorder, [&](RustRuleView const& rule, Utf16View layer_prefix) {
        if (rule.type() == RustRule::Type::Container && Parser::ValueParserFFI::rust_container_conditions_contains_size_feature(rule.container()))
            has_size_container_queries = true;
        if (rule.type() == RustRule::Type::Function)
            function_rules.append({ rule.compile_function(), Utf16FlyString::from_utf16(layer_prefix), cascade_origin });
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
                    if (value->tag == StyleValueFFI::StyleValueData::Tag::Keyword && is_css_wide_keyword(static_cast<Keyword>(value->keyword.keyword)))
                        return;
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

        rules_by_animation_keyframes.set(Utf16FlyString { name }, move(keyframe_set));
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
        if (!sheet.native_media_list().matches())
            return;
        if (auto* shared_compiled_style_sheet = sheet.shared_compiled_style_sheet())
            sheets.append(shared_compiled_style_sheet->contents().native_sheet().handle());
        else
            sheets.append(sheet.native_sheet().handle());
    });
    if (pending_attachment && !pending_attachment->disabled() && pending_attachment->native_media_list().matches())
        sheets.append(pending_attachment->native_sheet().handle());

    m_has_published_named_layer_order = Parser::ValueParserFFI::rust_style_sheet_publish_layer_order(
        sheets.data(), sheets.size(), document().style_computer().style_engine().host(),
        style_engine_tree_scope().value(), m_has_published_named_layer_order, &document(),
        [](void* document) { static_cast<DOM::Document*>(document)->flush_deferred_style_change_event(); });
}

// The `@keyframes` this scope defines, as the rule cache just built resolved them. The style computation resolves an
// animation's keyframes from these, so it never builds a rule cache itself.
void StyleScope::publish_animation_keyframes()
{
    auto const& keyframes = m_rule_cache->rules_by_animation_keyframes;
    Vector<u32> name_lengths;
    Vector<u16> name_units;
    Vector<size_t> keyframe_sets;
    Vector<NonnullRefPtr<Animations::KeyframeEffect::KeyFrameSet const>> published;
    name_lengths.ensure_capacity(keyframes.size());
    keyframe_sets.ensure_capacity(keyframes.size());
    published.ensure_capacity(keyframes.size());
    for (auto const& [name, keyframe_set] : keyframes) {
        name_lengths.unchecked_append(name.length_in_code_units());
        for (size_t index = 0; index < name.length_in_code_units(); ++index)
            name_units.append(name.code_unit_at(index));
        keyframe_sets.unchecked_append(bit_cast<size_t>(keyframe_set.ptr()));
        published.unchecked_append(*keyframe_set);
    }
    // A scope that defined nothing before and defines nothing now has no row to replace.
    if (published.is_empty() && m_published_keyframe_sets.is_empty())
        return;
    StyleEngineFFI::style_engine_set_tree_scope_animation_keyframes(
        document().style_computer().style_engine().host(), style_engine_tree_scope().value(),
        bit_cast<FlatPtr>(as_if<DOM::ShadowRoot>(*m_node)), name_lengths.data(), name_units.data(), name_units.size(),
        keyframe_sets.data(), name_lengths.size());
    m_published_keyframe_sets = move(published);
}

Optional<StyleScope::DepartedAnimationKeyframes> StyleScope::take_published_animation_keyframes()
{
    // The scope publishes again in the document it joins, if it joins one, under the tree scope that document gives it.
    m_has_published_rule_cache = false;
    auto* shadow_root = as_if<DOM::ShadowRoot>(*m_node);
    if (!shadow_root || m_published_keyframe_sets.is_empty())
        return {};
    return DepartedAnimationKeyframes {
        .tree_scope = shadow_root->style_engine_tree_scope(),
        .shadow_root_identity = bit_cast<FlatPtr>(shadow_root),
        .keyframe_sets = move(m_published_keyframe_sets),
    };
}

TreeScopeID StyleScope::style_engine_tree_scope() const
{
    return style_engine_tree_scope_for(*m_node);
}

void StyleScope::invalidate_counter_style_cache()
{
    m_needs_counter_style_cache_update = true;

    // Only this style scope and those whose counter style names pass on to it, which may include counter styles that
    // extend the ones defined in this scope, can resolve differently.
    m_node->document().for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) {
        auto& scope = shadow_root.style_scope();
        for (auto const* ancestor = scope.parent_style_scope(); ancestor; ancestor = ancestor->parent_style_scope()) {
            if (ancestor == this) {
                scope.m_needs_counter_style_cache_update = true;
                break;
            }
        }
    });
}

// The precedence of a counter style rule's cascade origin: user agent, then user, then author.
u8 cascade_origin_precedence(CascadeOrigin origin)
{
    switch (origin) {
    case CascadeOrigin::UserAgent:
        return 0;
    case CascadeOrigin::User:
        return 1;
    default:
        return 2;
    }
}

void StyleScope::build_counter_style_cache(Layout::BegunRead const& read)
{
    m_is_doing_counter_style_cache_update = true;

    // Counter styles can be resolved before any keyframe or function lookup builds the rule cache.
    // Publish this scope's layer order before comparing definitions from different layers.
    build_rule_cache_if_needed();

    struct CounterStyleRule {
        Utf16FlyString name;
        RustDescriptorBlock descriptors;
        u8 origin;
        u32 layer;
    };
    Vector<CounterStyleRule> rules;
    auto& style_engine = document().style_computer().style_engine();
    auto const tree_scope = style_engine_tree_scope();
    auto collect_counter_style_rules = [&](CSS::CascadeOrigin cascade_origin, CSS::StyleSheetState const& style_sheet) {
        if (!style_sheet.native_media_list().matches())
            return;
        auto const& rule_sheet = style_sheet.shared_compiled_style_sheet() ? style_sheet.shared_compiled_style_sheet()->contents() : style_sheet;
        rule_sheet.for_each_effective_rule_data(TraversalOrder::Preorder, [&](RustRuleView const& rule, Utf16View layer_prefix) {
            if (rule.type() != RustRule::Type::CounterStyle)
                return;
            auto const layer = layer_prefix.is_empty() ? 0 : style_engine.intern_atom(Utf16FlyString::from_utf16(layer_prefix)).value();
            rules.append({
                .name = Utf16FlyString { rule.name() },
                .descriptors = rule.descriptors(),
                .origin = cascade_origin_precedence(cascade_origin),
                .layer = StyleEngineFFI::style_engine_layer_index(style_engine.host(), &read, tree_scope, layer),
            });
        });
    };

    bool const is_document = m_node->is_document();
    if (is_document)
        for_each_stylesheet(CSS::CascadeOrigin::User, [&](auto& sheet) { collect_counter_style_rules(CSS::CascadeOrigin::User, sheet); });
    for_each_stylesheet(CSS::CascadeOrigin::Author, [&](auto& sheet) { collect_counter_style_rules(CSS::CascadeOrigin::Author, sheet); });

    // OPTIMIZATION: The predefined counter styles are the same for every document (they all come from Default.css),
    //               and so is what a document that defines no counter style of its own registers. Every new iframe
    //               and SVG image registers them in its first style update, so they are made once.
    static Parser::ValueParserFFI::RegisteredCounterStyles const* user_agent_counter_styles = nullptr;
    bool const registers_only_user_agent_counter_styles = is_document && rules.is_empty();
    Parser::ValueParserFFI::RegisteredCounterStyles const* counter_styles = nullptr;
    if (registers_only_user_agent_counter_styles && user_agent_counter_styles) {
        counter_styles = Parser::ValueParserFFI::rust_counter_styles_retain(user_agent_counter_styles);
    } else {
        // NB: Only the document's style scope registers the predefined counter styles, so that overrides of them are
        //     inherited by shadow roots.
        if (is_document)
            for_each_stylesheet(CSS::CascadeOrigin::UserAgent, [&](auto& sheet) { collect_counter_style_rules(CSS::CascadeOrigin::UserAgent, sheet); });
        Vector<Parser::ValueParserFFI::FfiCounterStyleRule> ffi_rules;
        ffi_rules.ensure_capacity(rules.size());
        for (auto const& rule : rules)
            ffi_rules.unchecked_append({ Parser::ffi_utf16_view(rule.name), rule.descriptors.handle(), rule.origin, rule.layer });
        auto const* parent = parent_style_scope();
        auto outer_scopes = parent ? parent->counter_style_lookup_chain(read) : CounterStyleLookupChain {};
        auto length_resolution_context = to_ffi_length_resolution_context(CSS::Length::ResolutionContext::for_document(document()));
        counter_styles = Parser::ValueParserFFI::rust_counter_styles_resolve(ffi_rules.data(), ffi_rules.size(), is_document, outer_scopes.data(), outer_scopes.size(), m_counter_styles, &length_resolution_context);
        if (registers_only_user_agent_counter_styles)
            user_agent_counter_styles = Parser::ValueParserFFI::rust_counter_styles_retain(counter_styles);
    }

    if (counter_styles != m_counter_styles) {
        m_counter_style_environment_identity = document().next_counter_style_environment_identity();
        // The style engine names the same registry on every record it computes against it.
        StyleEngineFFI::style_engine_set_counter_style_environment_identity(style_engine.host(), tree_scope, m_counter_style_environment_identity);
    }
    Parser::ValueParserFFI::rust_counter_styles_release(m_counter_styles);
    m_counter_styles = counter_styles;

    m_is_doing_counter_style_cache_update = false;
    m_needs_counter_style_cache_update = false;
}

u64 StyleScope::counter_style_environment_identity(Layout::BegunRead const& read) const
{
    if (m_needs_counter_style_cache_update && !m_is_doing_counter_style_cache_update)
        const_cast<StyleScope*>(this)->build_counter_style_cache(read);
    // NB: This is asked for whenever a style that depends on the counter style environment is published, which is
    //     what the layout tree build and the generated content counter style comparison resolve counter styles
    //     for, against the published registry.
    if (!m_is_doing_counter_style_cache_update)
        publish_counter_style_lookup_chain(read);
    return m_counter_style_environment_identity;
}

StyleScope* StyleScope::parent_style_scope() const
{
    auto* shadow_root = as_if<DOM::ShadowRoot>(*m_node);
    if (!shadow_root)
        return nullptr;
    auto* host = shadow_root->host();
    if (!host)
        return nullptr;
    auto& root = host->root();
    if (auto* host_shadow_root = as_if<DOM::ShadowRoot>(root)) {
        if (host_shadow_root->uses_document_style_sheets())
            return &root.document().style_scope();
        return &host_shadow_root->style_scope();
    }
    if (auto* document = as_if<DOM::Document>(root))
        return &document->style_scope();
    // A detached host's node tree is rooted at an ordinary element, which carries no tree-scoped names of its own.
    return nullptr;
}

// The counter styles of every scope a counter style name used in this scope is looked up in, nearest first.
StyleScope::CounterStyleLookupChain StyleScope::counter_style_lookup_chain(Layout::BegunRead const& read) const
{
    CounterStyleLookupChain chain;
    for (auto const* scope = this; scope; scope = scope->parent_style_scope()) {
        if (scope->m_needs_counter_style_cache_update && !scope->m_is_doing_counter_style_cache_update)
            const_cast<StyleScope*>(scope)->build_counter_style_cache(read);
        if (scope->m_counter_styles)
            chain.append(scope->m_counter_styles);
    }
    return chain;
}

bool StyleScope::list_style_type_depends_on_counter_value(void const* list_style_type) const
{
    Layout::ForcedReadScope read { document() };
    auto scopes = counter_style_lookup_chain(read);
    return Parser::ValueParserFFI::rust_list_style_type_depends_on_counter_value(list_style_type, scopes.data(), scopes.size());
}

// Settles every scope a counter style name used in this scope may be looked up in, and publishes what each registers
// to the layout node arena, so that the arena answers every lookup the way counter_style_lookup_chain() does.
void StyleScope::publish_counter_style_lookup_chain(Layout::BegunRead const& read) const
{
    for (auto const* scope = this; scope; scope = scope->parent_style_scope()) {
        if (scope->m_needs_counter_style_cache_update && !scope->m_is_doing_counter_style_cache_update)
            const_cast<StyleScope*>(scope)->build_counter_style_cache(read);
        scope->publish_counter_styles_if_changed();
    }
}

void StyleScope::publish_counter_styles_if_changed() const
{
    auto const* parent = parent_style_scope();
    auto parent_tree_scope = parent ? parent->style_engine_tree_scope() : Optional<TreeScopeID> {};
    if (m_published_counter_style_environment_identity == m_counter_style_environment_identity && m_published_parent_counter_style_scope == parent_tree_scope)
        return;

    Parser::ValueParserFFI::render_state_publish_counter_styles(
        document().layout_node_arena().host(),
        style_engine_tree_scope().value(),
        parent_tree_scope.has_value() ? parent_tree_scope->value() : 0,
        parent_tree_scope.has_value(),
        m_counter_styles);
    m_published_counter_style_environment_identity = m_counter_style_environment_identity;
    m_published_parent_counter_style_scope = parent_tree_scope;
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

}
