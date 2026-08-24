/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <AK/Bitmap.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/Find.h>
#include <AK/FixedBitmap.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <AK/Utf8View.h>
#include <LibGC/Heap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSContainerRule.h>
#include <LibWeb/CSS/CSSFunctionRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSScopeRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheet.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RandomValueSharingStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>
#include <math.h>

namespace Web::CSS {

static ComputedValuesFFI::FfiUtf16View ffi_utf16_view(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

static Utf16View utf16_view(ComputedValuesFFI::FfiUtf16View view)
{
    if (view.length == 0)
        return {};
    VERIFY((view.ascii == nullptr) != (view.utf16 == nullptr));
    if (view.ascii)
        return StringView { reinterpret_cast<char const*>(view.ascii), view.length };
    return { reinterpret_cast<char16_t const*>(view.utf16), view.length };
}

static size_t retain_utf16_fly_string_for_substitution(u16 const* code_units, size_t length)
{
    return Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length }).to_raw_leaked();
}

static size_t normalize_svg_path_data_for_substitution(u16 const* code_units, size_t length)
{
    auto path = SVG::AttributeParser::parse_path_data(Utf16View { reinterpret_cast<char16_t const*>(code_units), length });
    if (path.instructions().is_empty())
        return 0;
    return Utf16String::from_utf8(path.serialize()).to_raw_leaked();
}

struct SubstitutionData {
    struct Attribute {
        Utf16String name;
        Utf16String value;
    };
    struct FunctionParameter {
        Utf16String name;
        Utf16String syntax;
        void const* default_data { nullptr };
    };
    struct FunctionDeclaration {
        Utf16String name;
        void const* data { nullptr };
    };
    struct FunctionDefinition {
        GC::Ptr<CSSFunctionRule const> function;
        StyleScope const* scope { nullptr };
        Utf16String name;
        Vector<FunctionParameter> parameters;
        Utf16String return_syntax;
        Vector<FunctionDeclaration> declarations;
        Vector<ComputedValuesFFI::FfiSubstitutionFunctionParameter> ffi_parameters;
        Vector<ComputedValuesFFI::FfiSubstitutionFunctionDeclaration> ffi_declarations;
    };

    SubstitutionData(AbstractOrHypotheticalElement element, bool collect_attributes, bool collect_functions)
    {
        auto& document = element.document();
        if (collect_attributes) {
            document_url = document.url().serialize();
            document_base_url = document.base_url().serialize();
        }
        parse_context = {
            .in_quirks_mode = document.in_quirks_mode(),
            .is_svg_presentation_attribute = false,
            .is_substituted_value = true,
            .contains_attr_tainted_values = false,
            .value_contexts = nullptr,
            .value_context_count = 0,
            .document_url = document_url.bytes().data(),
            .document_url_length = document_url.bytes().size(),
            .document_base_url = document_base_url.bytes().data(),
            .document_base_url_length = document_base_url.bytes().size(),
            .intern_utf16_fly_string = retain_utf16_fly_string_for_substitution,
            .normalize_svg_path_data = normalize_svg_path_data_for_substitution,
            .precomputed_svg_paths = nullptr,
            .precomputed_svg_path_count = 0,
            .font_format_is_supported = nullptr,
            .font_tech_is_supported = nullptr,
            .descriptor_integer_resolution_context = nullptr,
            .resolve_descriptor_integer = nullptr,
            .random_function_index = nullptr,
        };

        if (collect_attributes) {
            element.abstract_element().element().for_each_attribute([&](DOM::QualifiedName const& name, Utf16View value) {
                if (name.namespace_().has_value())
                    return;
                attributes.append({ name.local_name().to_utf16_string(), Utf16String::from_utf16(value) });
            });
        }
        ffi_attributes.ensure_capacity(attributes.size());
        for (auto const& attribute : attributes)
            ffi_attributes.unchecked_append({ .name = ffi_utf16_view(attribute.name), .value = ffi_utf16_view(attribute.value) });

        if (collect_functions) {
            Function<void(StyleScope::FunctionDefinitionAndScope const&)> append_function = [&](StyleScope::FunctionDefinitionAndScope const& definition) {
                for (auto const& existing : functions) {
                    if (existing.function == definition.function)
                        return;
                }
                FunctionDefinition snapshot {
                    .function = definition.function,
                    .scope = &definition.scope,
                    .name = definition.function->name(),
                    .parameters = {},
                    .return_syntax = definition.function->return_type_internal().to_string(),
                    .declarations = {},
                    .ffi_parameters = {},
                    .ffi_declarations = {},
                };
                snapshot.parameters.ensure_capacity(definition.function->parameters_internal().size());
                for (auto const& parameter : definition.function->parameters_internal()) {
                    snapshot.parameters.unchecked_append({
                        .name = parameter.name.to_utf16_string(),
                        .syntax = parameter.type->to_string(),
                        .default_data = parameter.default_value ? parameter.default_value->rust_style_value_data() : nullptr,
                    });
                }
                definition.function->for_each_effective_declaration(element.abstract_element(), [&](Utf16FlyString const& name, NonnullRefPtr<StyleValue const> const& value) {
                    snapshot.declarations.append({ .name = name.to_utf16_string(), .data = value->rust_style_value_data() });
                });
                functions.append(move(snapshot));
            };
            element.style_scope().for_each_visible_function_definition(append_function);
            for (size_t index = 0; index < functions.size(); ++index)
                functions[index].scope->for_each_visible_function_definition(append_function);
        }
        ffi_functions.ensure_capacity(functions.size());
        for (auto& definition : functions) {
            definition.ffi_parameters.ensure_capacity(definition.parameters.size());
            for (auto const& parameter : definition.parameters) {
                definition.ffi_parameters.unchecked_append({
                    .name = ffi_utf16_view(parameter.name),
                    .syntax = ffi_utf16_view(parameter.syntax),
                    .default_data = parameter.default_data,
                });
            }
            definition.ffi_declarations.ensure_capacity(definition.declarations.size());
            for (auto const& declaration : definition.declarations) {
                definition.ffi_declarations.unchecked_append({
                    .name = ffi_utf16_view(declaration.name),
                    .data = declaration.data,
                });
            }
            ffi_functions.unchecked_append({
                .identity = bit_cast<FlatPtr>(definition.function),
                .scope_identity = bit_cast<FlatPtr>(definition.scope),
                .name = ffi_utf16_view(definition.name),
                .parameters = definition.ffi_parameters.data(),
                .parameter_count = definition.ffi_parameters.size(),
                .return_syntax = ffi_utf16_view(definition.return_syntax),
                .declarations = definition.ffi_declarations.data(),
                .declaration_count = definition.ffi_declarations.size(),
            });
        }
    }

    String document_url;
    String document_base_url;
    Parser::ValueParserFFI::ParseContext parse_context {};
    Vector<Attribute> attributes;
    Vector<ComputedValuesFFI::FfiSubstitutionAttribute> ffi_attributes;
    Vector<FunctionDefinition> functions;
    Vector<ComputedValuesFFI::FfiSubstitutionFunctionDefinition> ffi_functions;
};

static size_t resolve_custom_function_for_substitution(size_t scope_identity, ComputedValuesFFI::FfiUtf16View name)
{
    auto& scope = *bit_cast<StyleScope const*>(scope_identity);
    auto definition = scope.get_function_definition(Utf16FlyString::from_utf16(utf16_view(name)));
    return definition.has_value() ? bit_cast<FlatPtr>(definition->function) : 0;
}

static u8 evaluate_condition_for_substitution(AbstractOrHypotheticalElement element, u8 kind, ComputedValuesFFI::FfiUtf16View source)
{
    auto source_view = utf16_view(source);
    auto parser = Parser::Parser::create(Parser::ParsingParams { element.document() }, source_view);
    OwnPtr<BooleanExpression> expression;
    if (kind == 0) {
        expression = Parser::RustQueryParser::parse_media_feature(parser, source_view);
        if (!expression)
            expression = Parser::RustQueryParser::parse_media_condition(parser, source_view);
    } else if (kind == 1) {
        expression = Parser::RustQueryParser::parse_supports_declaration(parser, source_view);
        if (!expression)
            expression = Parser::RustQueryParser::parse_supports_condition(parser, source_view);
    } else {
        VERIFY(kind == 2);
        expression = Parser::RustQueryParser::parse_style_query(parser, source_view);
    }
    if (!expression)
        return 2;
    prepare_for_style_query_evaluation();
    auto matches = expression->evaluate_to_boolean({
        .document = &element.document(),
        .style_query_element = element,
    });
    return style_query_cycle_detected() ? 3 : matches;
}

class Fnv1a64 {
public:
    void add(u64 value)
    {
        m_hash ^= value;
        m_hash *= 0x100000001b3ull;
    }

    u64 value() const { return m_hash; }

private:
    u64 m_hash { 0xcbf29ce484222325ull };
};

GC_DEFINE_ALLOCATOR(StyleComputer);

static bool property_affects_font_metrics(PropertyID property_id)
{
    return property_id == PropertyID::FontSize || property_id == PropertyID::LineHeight;
}

// What a rule contributes, for the two rule types that carry a declaration block.
static CSSStyleProperties const& declaration_of_rule(CSSRule const& rule)
{
    if (rule.type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(rule).declaration();
    if (rule.type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(rule).declaration();
    VERIFY_NOT_REACHED();
}

StyleComputer::StyleComputer(DOM::Document& document)
    : m_document(document)
    , m_default_font_metrics(16, Platform::FontPlugin::the().default_font(16)->pixel_metrics(), InitialValues::line_height())
    , m_root_element_font_metrics(m_default_font_metrics)
    , m_style_engine(StyleEngine::DeviceClass::ForegroundDesktop, this)
{
}

void StyleComputer::finalize()
{
    Base::finalize();
    clear_style_sharing_cache();
}

void StyleComputer::clear_style_sharing_cache() const
{
    for (auto const& bucket : m_style_sharing_cache) {
        for (auto const& entry : bucket.value) {
            if (entry.explicitly_inherited_non_inherited_property && !!entry.parent_style_record_identity)
                unpin_style_record(entry.parent_style_record_identity);
            if (entry.style_record_identity.has_value())
                unpin_style_record(*entry.style_record_identity);
        }
    }
    m_style_sharing_cache.clear();
    m_style_sharing_cache_entry_count = 0;
}

void StyleComputer::prepare_for_style_engine_transaction() const
{
    ++m_style_sharing_transaction_generation;
    if (m_style_sharing_cache_entry_count > maximum_persistent_style_sharing_entries)
        clear_style_sharing_cache();
    m_computed_style_invalidation_cache.clear();
    m_style_engine_cascade_input_cache.clear();
    m_inherited_style_group_swaps.clear();
    sweep_custom_property_environments();
}

void StyleComputer::drop_style_sharing_cache() const
{
    clear_style_sharing_cache();
    m_computed_style_invalidation_cache.clear();
    m_style_engine_cascade_input_cache.clear();
    m_inherited_style_group_swaps.clear();
    sweep_custom_property_environments();
}

ComputedStyleRecordView StyleComputer::computed_style_record_view(StyleRecordID style_record_identity) const
{
    if (!style_record_identity)
        return {};
    auto view = m_style_engine.style_record_view(style_record_identity);
    if (!view.present)
        return {};
    pin_style_record(style_record_identity);
    ++m_computed_style_record_view_pin_count;
    return ComputedStyleRecordView { view, *this, style_record_identity };
}

void const* StyleComputer::style_record_payloads(StyleRecordID style_record_identity) const
{
    if (!style_record_identity)
        return nullptr;
    return m_style_engine.style_record_payloads(style_record_identity);
}

void StyleComputer::pin_style_record(StyleRecordID style_record_identity) const
{
    VERIFY(style_record_identity);
    const_cast<StyleComputer&>(*this).m_style_engine.pin_style_record(style_record_identity);
}

void StyleComputer::unpin_style_record(StyleRecordID style_record_identity) const
{
    VERIFY(style_record_identity);
    const_cast<StyleComputer&>(*this).m_style_engine.unpin_style_record(style_record_identity);
}

void StyleComputer::register_style_node(StyleNodeID style_node_id, DOM::Element& element)
{
    if (style_node_id == 0)
        return;
    ensure_style_node_slot(style_node_id);
    m_style_nodes[style_node_id.value()] = element;
}

void StyleComputer::ensure_style_node_slot(StyleNodeID style_node_id)
{
    if (style_node_id != 0 && style_node_id.value() >= m_style_nodes.size()) {
        m_style_nodes.grow_capacity(style_node_id.value() + 1);
        m_style_nodes.resize(style_node_id.value() + 1);
    }
}

void StyleComputer::unregister_style_node(StyleNodeID style_node_id)
{
    if (style_node_id != 0 && style_node_id.value() < m_style_nodes.size()) {
        m_style_nodes[style_node_id.value()] = nullptr;
        m_style_engine.cancel_preallocated_style_node(style_node_id);
        m_style_engine.consume_recorded_element_style_input_change(style_node_id);
    }
}

GC::Ptr<DOM::Element> StyleComputer::element_for_style_node(StyleNodeID style_node_id) const
{
    if (style_node_id == 0 || style_node_id.value() >= m_style_nodes.size())
        return nullptr;
    return m_style_nodes[style_node_id.value()];
}

void StyleComputer::prepare_elements_for_style_computation()
{
    for (;;) {
        auto elements = m_style_engine.take_elements_awaiting_first_style_computation();
        if (elements.is_empty())
            break;
        for (auto style_node : elements) {
            auto element = element_for_style_node(style_node);
            if (element && element->is_connected())
                element->prepare_for_style_computation({});
        }
    }
}

void StyleComputer::for_each_style_node(Function<void(DOM::Element&)> callback) const
{
    for (auto element : m_style_nodes) {
        if (element)
            callback(*element);
    }
}

void StyleComputer::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    m_style_engine.visit_edges(visitor);
    visitor.visit(m_style_nodes);
    for (auto const& entry : m_non_author_style_sheets)
        visitor.visit(entry.sheet);
    for (auto const& entry : m_non_author_rule_ids)
        visitor.visit(entry.key);
    for (auto const& entry : m_constructed_sheet_ids)
        visitor.visit(entry.key);
    for (auto const& entry : m_constructed_rule_ids)
        visitor.visit(entry.key);

    if (m_active_custom_property_resolution.has_value())
        m_active_custom_property_resolution->visit_edges(visitor);

    if (m_cached_font_computation_context.has_value())
        m_cached_font_computation_context->visit_edges(visitor);
    if (m_cached_line_height_computation_context.has_value())
        m_cached_line_height_computation_context->visit_edges(visitor);
    for (auto const& state : m_provisional_transition_states) {
        visitor.visit(state.element);
        visitor.visit(state.committed_transition);
        visitor.visit(state.proposed_transition);
    }
    if (m_cached_generic_computation_context.has_value())
        m_cached_generic_computation_context->visit_edges(visitor);

    for (auto const& entry : m_style_engine_rule_targets) {
        visitor.visit(entry.key);
        entry.value.visit_edges(visitor);
    }
    for (auto const& entry : m_style_engine_rules_by_id)
        visitor.visit(entry.value);
    for (auto const& entry : m_style_engine_cascade_input_cache) {
        for (auto const& contribution : entry.value->contributions) {
            visitor.visit(contribution.declaration);
            visitor.visit(contribution.source_shadow_root);
        }
    }
}

void StyleComputer::begin_transition_stabilization_epoch()
{
    VERIFY(m_provisional_transition_states.is_empty());
    VERIFY(m_provisional_transition_state_indices.is_empty());
    VERIFY(m_provisional_transition_state_indices_by_target.is_empty());
    VERIFY(m_transition_stabilization_baselines.is_empty());
}

void StyleComputer::record_transition_stabilization_baseline(DOM::AbstractElement abstract_element) const
{
    auto style_node_id = abstract_element.element().style_node_id();
    if (style_node_id == 0)
        return;

    auto transition_target_key = (static_cast<u64>(style_node_id.value()) << 8) | pseudo_element_to_ffi(abstract_element.pseudo_element());
    if (m_transition_stabilization_baselines.contains(transition_target_key))
        return;

    auto style_record_identity = abstract_element.style_record_identity();
    if (!style_record_identity)
        return;
    pin_style_record(style_record_identity);
    m_transition_stabilization_baselines.set(transition_target_key, style_record_identity);
}

// A provisionally started transition already contributed to the style published by the pass that
// started it, but it is not associated with its target until the stabilization epoch commits. An
// animated style update running before that commit has to collect it anyway, or it rebuilds the
// target's style without the transition's value and clobbers it.
void StyleComputer::for_each_provisional_transition_effect(DOM::AbstractElement const& abstract_element, Function<void(Animations::KeyframeEffect&)> const& callback) const
{
    for (auto const& state : m_provisional_transition_states) {
        if (state.element.ptr() != &abstract_element.element() || state.pseudo_element != abstract_element.pseudo_element())
            continue;
        if (!state.proposed_transition)
            continue;
        if (auto effect = state.proposed_transition->effect(); effect && effect->is_keyframe_effect())
            callback(static_cast<Animations::KeyframeEffect&>(*effect));
    }
}

void StyleComputer::commit_transition_stabilization_epoch()
{
    for (auto const& state : m_provisional_transition_states) {
        VERIFY(state.element);
        auto& element = *state.element;
        auto remove_committed_transition = [&] {
            if (element.property_transition(state.pseudo_element, state.property_id) == state.committed_transition)
                element.remove_transition(state.pseudo_element, state.property_id);
        };
        auto cancel_and_remove_committed_transition = [&] {
            VERIFY(state.committed_transition);
            state.committed_transition->cancel();
            remove_committed_transition();
        };
        auto commit_proposed_transition = [&] {
            VERIFY(state.proposed_transition);
            state.proposed_transition->commit_provisional_transition();
            ++document().style_invalidation_counters().committed_transitions_started;
        };

        switch (state.action) {
        case ProvisionalTransitionAction::None:
            continue;
        case ProvisionalTransitionAction::Remove:
            remove_committed_transition();
            break;
        case ProvisionalTransitionAction::Cancel:
            VERIFY(state.committed_transition);
            state.committed_transition->cancel();
            break;
        case ProvisionalTransitionAction::Start:
            commit_proposed_transition();
            break;
        case ProvisionalTransitionAction::RemoveAndStart:
            remove_committed_transition();
            commit_proposed_transition();
            break;
        case ProvisionalTransitionAction::CancelRemoveAndStart:
            cancel_and_remove_committed_transition();
            commit_proposed_transition();
            break;
        }
        ++document().style_invalidation_counters().committed_transition_actions;
    }
    m_provisional_transition_states.clear();
    m_provisional_transition_state_indices.clear();
    m_provisional_transition_state_indices_by_target.clear();
    for (auto const& baseline : m_transition_stabilization_baselines)
        unpin_style_record(baseline.value);
    m_transition_stabilization_baselines.clear();
}

template<size_t length>
static constexpr Utf16View utf16_view(char16_t const (&string)[length])
{
    return { string, length - 1 };
}

Optional<Utf16String> StyleComputer::user_agent_style_sheet_source(Utf16View name)
{
    extern String const& default_stylesheet_source;
    extern String const& quirks_mode_stylesheet_source;
    extern String const& mathml_stylesheet_source;
    extern String const& svg_stylesheet_source;

    if (name == utf16_view(u"CSS/Default.css"))
        return Utf16String::from_utf8(default_stylesheet_source);
    if (name == utf16_view(u"CSS/QuirksMode.css"))
        return Utf16String::from_utf8(quirks_mode_stylesheet_source);
    if (name == utf16_view(u"MathML/Default.css"))
        return Utf16String::from_utf8(mathml_stylesheet_source);
    if (name == utf16_view(u"SVG/Default.css"))
        return Utf16String::from_utf8(svg_stylesheet_source);
    return {};
}

struct ResolvedScope {
    GC::Ptr<DOM::Element const> root;
    size_t proximity { NumericLimits<size_t>::max() };
};

static u64 pseudo_element_style_bit(PseudoElement pseudo_element)
{
    VERIFY(to_underlying(pseudo_element) < to_underlying(PseudoElement::KnownPseudoElementCount));
    return 1ull << to_underlying(pseudo_element);
}

void StyleComputer::for_each_property_expanding_shorthands(PropertyID property_id, StyleValue const& value, Function<void(PropertyID, StyleValue const&)> const& set_longhand_property)
{
    // The expansion recursion and pending-substitution values live in the Rust style value graph.
    // This wrapper creates C++ facades only for the returned longhand roots.
    auto expansion = ComputedValuesFFI::rust_expand_property_shorthands(
        to_underlying(property_id), value.rust_style_value_data());
    ScopeGuard destroy_expansion = [&] {
        ComputedValuesFFI::rust_shorthand_expansion_destroy(expansion.storage);
    };
    HashMap<void const*, NonnullRefPtr<StyleValue const>> wrapper_cache;
    for (size_t i = 0; i < expansion.count; ++i) {
        auto const& property = expansion.properties[i];
        auto& expanded_value = wrapper_cache.ensure(property.data, [&] {
            return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                static_cast<StyleValueFFI::StyleValueData const*>(property.data)));
        });
        set_longhand_property(static_cast<PropertyID>(property.property_id), *expanded_value);
    }
}

static RefPtr<CustomPropertyData const> inheritable_custom_property_data(DOM::AbstractElement abstract_element)
{
    auto data = abstract_element.custom_property_data();
    if (!data)
        return nullptr;
    return data->inheritable(abstract_element.document());
}

static Optional<CSS::EasingFunction> resolve_keyframe_easing(CSS::StyleValue const& style_value, DOM::AbstractElement abstract_element)
{
    RefPtr<CSS::StyleValue const> resolved = style_value;
    if (resolved->is_unresolved())
        resolved = abstract_element.document().style_computer().resolve_unresolved_style_value(abstract_element, CSS::PropertyNameAndID::from_id(CSS::PropertyID::AnimationTimingFunction), resolved->as_unresolved());
    if (!resolved || resolved->is_guaranteed_invalid())
        return {};
    if (resolved->is_value_list()) {
        auto const& list = resolved->as_value_list();
        if (list.size() > 0)
            resolved = list.value_at(0, false);
        else
            return {};
    }
    if (resolved->is_easing() || resolved->is_keyword())
        return CSS::EasingFunction::from_style_value(*resolved);
    return {};
}

void StyleComputer::collect_animations_into(DOM::AbstractElement abstract_element, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects, ComputedStyleWorkingSet& computed_properties, AnimationRefresh refresh) const
{
    if (refresh == AnimationRefresh::No) {
        collect_animation_effects_into(abstract_element, effects, computed_properties);
        return;
    }
    m_keyframes_inherited_non_inherited_property = false;
    collect_animation_effects_into(abstract_element, effects, computed_properties);
    // An animation-only overlay update resolves keyframe values just like a full style computation does, so a
    // keyframe-borne `inherit` on a non-inherited property discovered here must leave the same invalidation
    // mark behind, or a later change to the parent's value never reaches this element's animated style.
    if (m_keyframes_inherited_non_inherited_property) {
        if (auto* parent = abstract_element.element().parent())
            parent->set_children_may_depend_on_non_inherited_property_inheritance();
        m_keyframes_inherited_non_inherited_property = false;
    }
    adjust_animated_element_style_if_needed(computed_properties, abstract_element);
}

void StyleComputer::collect_animation_effects_into(DOM::AbstractElement abstract_element, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects, ComputedStyleWorkingSet& computed_properties) const
{
    struct PreparedKeyframeValue {
        i64 key { 0 };
        RefPtr<StyleValue const> value;
        CSS::EasingFunction easing;
        Bindings::CompositeOperation composite_operation;
    };
    struct PreparedAnimationValue {
        GC::Ref<Animations::KeyframeEffect> effect;
        PropertyID property_id;
        AnimatedPropertyResultOfTransition is_result_of_transition;
        NonnullRefPtr<StyleValue const> underlying;
        NonnullRefPtr<StyleValue const> initial;
        double current_key { 0 };
        Vector<PreparedKeyframeValue> keyframes;
    };
    Vector<PreparedAnimationValue> prepared_values;

    struct KeyframeDeclaration {
        size_t keyframe_index { 0 };
        PropertyID property_id;
        RustStyleValueHandle value;
        bool use_initial { false };
        bool is_transition { false };
    };
    Vector<KeyframeDeclaration> keyframe_declarations;
    Vector<Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame const*> keyframes_by_index;
    for (auto effect : effects) {
        auto animation = effect->associated_animation();
        if (!animation || !effect->transformed_progress().has_value() || !effect->key_frame_set())
            continue;
        auto& keyframes = effect->key_frame_set()->keyframes_by_key;
        if (keyframes.size() < 2)
            continue;
        for (auto it = keyframes.begin(); it != keyframes.end(); ++it) {
            auto keyframe_index = keyframes_by_index.size();
            keyframes_by_index.append(&*it);
            for (auto const& [property_id, value] : it->properties) {
                bool is_use_initial = false;
                auto style_value = value.visit(
                    [&](Animations::KeyframeEffect::KeyFrameSet::UseInitial) -> RustStyleValueHandle {
                        if (property_is_shorthand(property_id))
                            return {};
                        is_use_initial = true;
                        return RustStyleValueHandle::retained(computed_properties.property(property_id, ComputedStyleWorkingSet::WithAnimationsApplied::No).rust_style_value_data());
                    },
                    [](RustStyleValueHandle const& value) -> RustStyleValueHandle { return value; });
                if (!style_value || style_value->tag == StyleValueFFI::StyleValueData::Tag::PendingSubstitution)
                    continue;
                if (style_value->tag == StyleValueFFI::StyleValueData::Tag::Unresolved) {
                    // Substitution needs the typed facade, but only var()-bearing keyframes take this path,
                    // and those re-parse the value every frame anyway.
                    auto unresolved = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(style_value.data()));
                    auto resolved = abstract_element.document().style_computer().resolve_unresolved_style_value(abstract_element, PropertyNameAndID::from_id(property_id), unresolved->as_unresolved());
                    style_value = RustStyleValueHandle::retained(resolved->rust_style_value_data());
                }
                // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
                // When substitution results in a guaranteed-invalid value, treat it as unset
                // (i.e. inherit for inherited properties, initial for non-inherited properties).
                if (style_value->tag == StyleValueFFI::StyleValueData::Tag::GuaranteedInvalid)
                    continue;
                keyframe_declarations.append({
                    .keyframe_index = keyframe_index,
                    .property_id = property_id,
                    .value = move(style_value),
                    .use_initial = is_use_initial,
                    .is_transition = animation->is_css_transition(),
                });
            }
        }
    }

    struct SelectedKeyframeValue {
        PropertyID source_longhand_id;
        NonnullRefPtr<StyleValue const> value;
        StyleValueFFI::FfiAnimationSpecifiedValueSource value_source;
    };
    if (keyframe_declarations.is_empty())
        return;

    Vector<StyleValueFFI::FfiAnimationDeclaration> ffi_declarations;
    ffi_declarations.ensure_capacity(keyframe_declarations.size());
    for (auto const& declaration : keyframe_declarations) {
        ffi_declarations.unchecked_append({
            .keyframe_index = declaration.keyframe_index,
            .property_id = to_underlying(declaration.property_id),
            .value = declaration.value.data(),
            .use_initial = declaration.use_initial,
            .is_transition = declaration.is_transition,
        });
    }
    // The table's importance bitmap already uses the byte layout the animation core expects.
    Vector<u8> important_property_bitmap;
    important_property_bitmap.append(computed_properties.property_importance_bitmap().data(), computed_properties.property_importance_bitmap().size());

    Vector<StyleValueFFI::FfiAnimationValueInput> ffi_values;
    Vector<StyleValueFFI::FfiAnimatedProperty> ffi_results;
    Vector<Vector<StyleValueFFI::FfiAnimationKeyframeValue>> ffi_keyframes;
    Vector<Vector<Vector<StyleValueFFI::FfiLinearEasingPoint>>> linear_easing_points;
    auto compute_animation_values = [&](ReadonlySpan<StyleValueFFI::FfiResolvedAnimationProperty> resolved_properties) -> StyleValueFFI::FfiComputedAnimationBatch {
        HashMap<Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame const*, HashMap<PropertyID, SelectedKeyframeValue>> selected_keyframe_values;
        for (auto const& property : resolved_properties) {
            VERIFY(property.keyframe_index < keyframes_by_index.size());
            auto* keyframe = keyframes_by_index[property.keyframe_index];
            auto& keyframe_values = selected_keyframe_values.ensure(keyframe);
            keyframe_values.set(static_cast<PropertyID>(property.physical_property_id), {
                                                                                            .source_longhand_id = static_cast<PropertyID>(property.source_longhand_id),
                                                                                            .value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(property.value)),
                                                                                            .value_source = property.value_source,
                                                                                        });
        }

        VERIFY(computation_context_cache_is_empty());
        for (auto effect : effects) {
            auto animation = effect->associated_animation();
            if (!animation)
                continue;

            auto output_progress = effect->transformed_progress();
            if (!output_progress.has_value())
                continue;

            if (!effect->key_frame_set())
                continue;

            auto& keyframes = effect->key_frame_set()->keyframes_by_key;
            if (keyframes.size() < 2) {
                if constexpr (LIBWEB_CSS_ANIMATION_DEBUG) {
                    dbgln("    Did not find enough keyframes ({} keyframes)", keyframes.size());
                    for (auto it = keyframes.begin(); it != keyframes.end(); ++it)
                        dbgln("        - {}", it.key());
                }
                continue;
            }

            double current_key = output_progress.value() * 100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor;
            current_key = clamp(current_key, static_cast<double>(NumericLimits<i64>::min()), static_cast<double>(NumericLimits<i64>::max()));

            // Each property is animated using its property-specific keyframes, so two properties in the same animation may be
            // interpolated across different intervals.
            // Collect the keyframes in ascending offset order, and index for each physical longhand the keyframes that specify
            // it, so that the interval endpoints can be found separately for every property.
            struct KeyframeInfo {
                i64 key { 0 };
                Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame const* frame { nullptr };
            };
            Vector<KeyframeInfo> ordered_keyframes;
            ordered_keyframes.ensure_capacity(keyframes.size());
            HashMap<PropertyID, Vector<size_t>> keyframes_specifying_property;
            for (auto it = keyframes.begin(); it != keyframes.end(); ++it) {
                auto keyframe_index = ordered_keyframes.size();
                if (auto selected_values = selected_keyframe_values.get(&*it); selected_values.has_value()) {
                    for (auto const& [physical_property_id, _] : *selected_values) {
                        auto& specifying_keyframes = keyframes_specifying_property.ensure(physical_property_id);
                        specifying_keyframes.append(keyframe_index);
                    }
                }
                ordered_keyframes.append({ static_cast<i64>(it.key()), &*it });
            }

            // https://drafts.csswg.org/css-animations-1/#animation-timing-function
            // Apply the per-keyframe easing to the interval progress. The easing on a keyframe applies to the
            // interval from that keyframe to the next. If the keyframe doesn't specify an easing, use the
            // animation's default easing (from the animation-timing-function property).
            auto resolve_interval_easing = [&](auto const& keyframe_easing) {
                auto resolved_easing = keyframe_easing.visit(
                    [](Empty) -> Optional<CSS::EasingFunction> { return {}; },
                    [](CSS::EasingFunction const& easing) -> Optional<CSS::EasingFunction> { return easing; },
                    [&](RustStyleValueHandle const& value) -> Optional<CSS::EasingFunction> {
                        // Only an easing kept as an unresolved value reaches here, and it re-parses
                        // every frame anyway, so the on-demand facade is not the expensive part.
                        auto style_value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value.data()));
                        return resolve_keyframe_easing(*style_value, abstract_element);
                    });
                if (resolved_easing.has_value())
                    return resolved_easing.release_value();
                if (animation->is_css_animation())
                    return static_cast<CSSAnimation const&>(*animation).default_easing();
                return CSS::EasingFunction::linear();
            };

            auto compute_keyframe_values = [&computed_properties, &abstract_element, &selected_keyframe_values, this](auto const& keyframe_values) {
                HashMap<PropertyID, RefPtr<StyleValue const>> result;
                HashMap<PropertyID, RefPtr<StyleValue const>> specified_values;
                if (auto selected_values = selected_keyframe_values.get(&keyframe_values); selected_values.has_value()) {
                    for (auto const& [physical_property_id, selected_value] : *selected_values) {
                        auto longhand_id = selected_value.source_longhand_id;
                        auto specified_value = [&]() -> NonnullRefPtr<StyleValue const> {
                            switch (selected_value.value_source) {
                            case StyleValueFFI::FfiAnimationSpecifiedValueSource::Inherited:
                                // A keyframe-borne `inherit` on a non-inherited property reads the
                                // half of the parent's style ordinary inheritance does not carry,
                                // exactly like an explicitly inherited declaration.
                                if (!is_inherited_property(longhand_id))
                                    m_keyframes_inherited_non_inherited_property = true;
                                if (auto inherited_animated_value = get_animated_inherit_value(longhand_id, abstract_element); inherited_animated_value.has_value())
                                    return inherited_animated_value->value;
                                return get_non_animated_inherit_value(longhand_id, abstract_element);
                            case StyleValueFFI::FfiAnimationSpecifiedValueSource::Initial:
                                return property_initial_value(longhand_id);
                            case StyleValueFFI::FfiAnimationSpecifiedValueSource::Underlying:
                                return computed_properties.property(longhand_id);
                            case StyleValueFFI::FfiAnimationSpecifiedValueSource::Value:
                                return selected_value.value;
                            }
                            VERIFY_NOT_REACHED();
                        }();
                        specified_values.set(physical_property_id, specified_value);
                    }
                }

                // NOTE: This doesn't necessarily return the specified value if we reach into computed_properties but that
                //       doesn't matter as a computed value is always valid as a specified value.
                Function<NonnullRefPtr<StyleValue const>(PropertyID)> get_property_specified_value = [&](PropertyID property_id) -> NonnullRefPtr<StyleValue const> {
                    if (auto keyframe_value = specified_values.get(property_id); keyframe_value.has_value() && keyframe_value.value())
                        return *keyframe_value.value();

                    return computed_properties.property(property_id);
                };

                for (auto const& [property_id, style_value] : specified_values) {
                    if (!style_value)
                        continue;

                    auto const& computation_context = get_computation_context_for_property(property_id, computed_properties, abstract_element);

                    computation_context.reset_viewport_metric_dependency_tracking();
                    result.set(property_id, compute_value_of_property(property_id, *style_value, get_property_specified_value, computation_context, m_document->page().client().device_pixels_per_css_pixel()));
                    if (computation_context.depends_on_viewport_metrics()) {
                        computed_properties.set_depends_on_viewport_metrics();
                        if (property_affects_font_metrics(property_id))
                            computed_properties.set_font_metrics_depend_on_viewport_metrics();
                    }
                }

                return result;
            };

            auto to_composite_operation = [&](Bindings::CompositeOperationOrAuto composite_operation_or_auto) {
                switch (composite_operation_or_auto) {
                case Bindings::CompositeOperationOrAuto::Accumulate:
                    return Bindings::CompositeOperation::Accumulate;
                case Bindings::CompositeOperationOrAuto::Add:
                    return Bindings::CompositeOperation::Add;
                case Bindings::CompositeOperationOrAuto::Replace:
                    return Bindings::CompositeOperation::Replace;
                case Bindings::CompositeOperationOrAuto::Auto:
                    return effect->composite();
                }
                VERIFY_NOT_REACHED();
            };

            auto is_result_of_transition = animation->is_css_transition() ? AnimatedPropertyResultOfTransition::Yes : AnimatedPropertyResultOfTransition::No;

            Vector<HashMap<PropertyID, RefPtr<StyleValue const>>> keyframe_computed_values;
            keyframe_computed_values.resize(ordered_keyframes.size());
            auto computed_values_for_keyframe = [&](size_t index) -> HashMap<PropertyID, RefPtr<StyleValue const>> const& {
                if (keyframe_computed_values[index].is_empty())
                    keyframe_computed_values[index] = compute_keyframe_values(*ordered_keyframes[index].frame);
                return keyframe_computed_values[index];
            };

            for (auto const& [property_id, specifying_keyframes] : keyframes_specifying_property) {
                // A property is usually specified by at least the initial and final keyframes, but a value that stays
                // unresolved may leave a property with only one specifying keyframe. Such a property cannot be interpolated, so skip it.
                if (specifying_keyframes.size() < 2)
                    continue;

                // An unresolved shorthand cannot be expanded while building the property index. Computing its keyframe
                // values either resolves it into physical longhands or leaves no value, so it is never itself animatable.
                if (property_id < first_longhand_property_id || property_id > last_longhand_property_id)
                    continue;

                PreparedAnimationValue prepared_value {
                    .effect = effect,
                    .property_id = property_id,
                    .is_result_of_transition = is_result_of_transition,
                    .underlying = computed_properties.property(property_id),
                    .initial = property_initial_value(property_id),
                    .current_key = current_key,
                    .keyframes = {},
                };
                prepared_value.keyframes.ensure_capacity(specifying_keyframes.size());
                for (auto keyframe_index : specifying_keyframes) {
                    prepared_value.keyframes.unchecked_append({
                        .key = ordered_keyframes[keyframe_index].key,
                        .value = computed_values_for_keyframe(keyframe_index).get(property_id).value_or(nullptr),
                        .easing = resolve_interval_easing(ordered_keyframes[keyframe_index].frame->easing),
                        .composite_operation = to_composite_operation(ordered_keyframes[keyframe_index].frame->composite),
                    });
                }
                prepared_values.append(move(prepared_value));
            }
        }

        if (prepared_values.is_empty()) {
            return {};
        }

        auto const& color_computation_context = get_computation_context_for_property(PropertyID::Color, computed_properties, abstract_element);
        ColorResolutionContext color_resolution_context {
            .color_scheme = color_computation_context.color_scheme,
            .current_color = InitialValues::color(),
            .current_color_style_value = &computed_properties.property(PropertyID::Color),
            .calculation_resolution_context = { .length_resolution_context = color_computation_context.length_resolution_context },
        };
        color_resolution_context.current_color = computed_properties.color(PropertyID::Color, color_resolution_context);

        auto ffi_composite_operation = [](Bindings::CompositeOperation operation) {
            switch (operation) {
            case Bindings::CompositeOperation::Replace:
                return StyleValueFFI::FfiCompositeOperation::Replace;
            case Bindings::CompositeOperation::Add:
                return StyleValueFFI::FfiCompositeOperation::Add;
            case Bindings::CompositeOperation::Accumulate:
                return StyleValueFFI::FfiCompositeOperation::Accumulate;
            }
            VERIFY_NOT_REACHED();
        };
        ffi_keyframes.resize(prepared_values.size());
        linear_easing_points.resize(prepared_values.size());
        ffi_values.ensure_capacity(prepared_values.size());
        for (size_t index = 0; index < prepared_values.size(); ++index) {
            auto const& value = prepared_values[index];
            auto& keyframes = ffi_keyframes[index];
            auto& property_linear_easing_points = linear_easing_points[index];
            keyframes.ensure_capacity(value.keyframes.size());
            property_linear_easing_points.resize(value.keyframes.size());
            for (size_t keyframe_index = 0; keyframe_index < value.keyframes.size(); ++keyframe_index) {
                auto const& keyframe = value.keyframes[keyframe_index];
                auto easing = keyframe.easing.visit(
                    [&](LinearEasingFunction const& linear) {
                        auto& points = property_linear_easing_points[keyframe_index];
                        points.ensure_capacity(linear.control_points.size());
                        for (auto const& point : linear.control_points) {
                            VERIFY(point.input.has_value());
                            points.unchecked_append({ .input = *point.input, .output = point.output });
                        }
                        return StyleValueFFI::FfiEasingDescriptor {
                            .kind = StyleValueFFI::FfiEasingKind::Linear,
                            .linear_points = points.data(),
                            .linear_point_count = points.size(),
                            .x1 = 0,
                            .y1 = 0,
                            .x2 = 0,
                            .y2 = 0,
                            .interval_count = 0,
                            .step_position = 0,
                        };
                    },
                    [](CubicBezierEasingFunction const& cubic_bezier) {
                        return StyleValueFFI::FfiEasingDescriptor {
                            .kind = StyleValueFFI::FfiEasingKind::CubicBezier,
                            .linear_points = nullptr,
                            .linear_point_count = 0,
                            .x1 = cubic_bezier.x1,
                            .y1 = cubic_bezier.y1,
                            .x2 = cubic_bezier.x2,
                            .y2 = cubic_bezier.y2,
                            .interval_count = 0,
                            .step_position = 0,
                        };
                    },
                    [](StepsEasingFunction const& steps) {
                        return StyleValueFFI::FfiEasingDescriptor {
                            .kind = StyleValueFFI::FfiEasingKind::Steps,
                            .linear_points = nullptr,
                            .linear_point_count = 0,
                            .x1 = 0,
                            .y1 = 0,
                            .x2 = 0,
                            .y2 = 0,
                            .interval_count = steps.interval_count,
                            .step_position = to_underlying(steps.position),
                        };
                    });
                keyframes.unchecked_append({
                    .key = keyframe.key,
                    .value = keyframe.value ? keyframe.value->rust_style_value_data() : nullptr,
                    .easing = easing,
                    .composite = ffi_composite_operation(keyframe.composite_operation),
                });
            }
            ffi_values.unchecked_append({
                .property_id = to_underlying(value.property_id),
                .underlying = value.underlying->rust_style_value_data(),
                .initial = value.initial->rust_style_value_data(),
                .current_key = value.current_key,
                .keyframes = keyframes.data(),
                .keyframe_count = keyframes.size(),
            });
        }

        auto animation_font_metrics = [](Length::FontMetrics const& metrics) {
            return StyleValueFFI::FfiAnimationFontMetrics {
                .font_size = metrics.font_size.to_double(),
                .x_height = metrics.x_height.to_double(),
                .cap_height = metrics.cap_height.to_double(),
                .zero_advance = metrics.zero_advance.to_double(),
                .line_height = metrics.line_height.to_double(),
            };
        };
        auto const& resolution_context = color_computation_context.length_resolution_context;
        StyleValueFFI::FfiAnimationContext animation_context {
            .allow_discrete = true,
            .current_color = computed_properties.property(PropertyID::Color).rust_style_value_data(),
            .has_length_resolution_context = true,
            .length_resolution_context = {
                .viewport_width = resolution_context.viewport_rect.width().to_double(),
                .viewport_height = resolution_context.viewport_rect.height().to_double(),
                .font_metrics = animation_font_metrics(resolution_context.font_metrics),
                .root_font_metrics = animation_font_metrics(resolution_context.root_font_metrics),
                .font_metrics_depend_on_viewport_metrics = resolution_context.font_metrics_depend_on_viewport_metrics,
                .root_font_metrics_depend_on_viewport_metrics = resolution_context.root_font_metrics_depend_on_viewport_metrics,
            },
            .has_transform_reference_box = false,
            .transform_reference_box_width = 0,
            .transform_reference_box_height = 0,
        };
        if (auto const* layout_node = prepared_values.first().effect->target()->unsafe_layout_node(); layout_node && Painting::has_committed_box(*layout_node)) {
            auto reference_box = Painting::transform_reference_box(*layout_node);
            animation_context.has_transform_reference_box = true;
            animation_context.transform_reference_box_width = reference_box.width().to_double();
            animation_context.transform_reference_box_height = reference_box.height().to_double();
        }
        ffi_results.resize(ffi_values.size());
        return StyleValueFFI::FfiComputedAnimationBatch {
            .context = animation_context,
            .values = ffi_values.data(),
            .value_count = ffi_values.size(),
            .results = ffi_results.data(),
            .result_capacity = ffi_results.size(),
        };
    };

    StyleValueFFI::FfiAnimationBatch batch {
        .declarations = ffi_declarations.data(),
        .declaration_count = ffi_declarations.size(),
        .writing_mode = to_underlying(computed_properties.writing_mode()),
        .direction = to_underlying(computed_properties.direction()),
        .important_property_bitmap = important_property_bitmap.data(),
        .important_property_bitmap_length = important_property_bitmap.size(),
    };
    auto resolved_properties = StyleValueFFI::rust_resolve_animation_declarations(&batch);
    ScopeGuard destroy_resolved_properties = [&] {
        StyleValueFFI::rust_resolved_animation_properties_destroy(resolved_properties.storage);
    };
    StyleValueFFI::FfiComputedAnimationBatch computed_batch {};
    if (resolved_properties.count > 0) {
        computed_batch = compute_animation_values(ReadonlySpan<StyleValueFFI::FfiResolvedAnimationProperty> {
            resolved_properties.properties, resolved_properties.count });
    }
    auto result_count = StyleValueFFI::rust_evaluate_animations(&computed_batch);
    VERIFY(result_count == prepared_values.size());
    VERIFY(result_count == ffi_results.size());
    for (size_t index = 0; index < result_count; ++index) {
        auto const& value = ffi_results[index];
        auto& prepared_value = prepared_values[index];
        VERIFY(value.property_id == to_underlying(prepared_value.property_id));
        VERIFY(value.handled);
        if (!value.apply)
            continue;
        if (value.value) {
            auto style_value = StyleValue::adopt_rust_style_value_data(value.value);
            computed_properties.set_animated_property(Badge<StyleComputer> {}, prepared_value.property_id, style_value, prepared_value.is_result_of_transition);
        } else {
            // NB: If interpolation fails, the element should not be rendered.
            computed_properties.set_animated_property(Badge<StyleComputer> {}, PropertyID::Visibility, KeywordStyleValue::create(Keyword::Hidden), prepared_value.is_result_of_transition);
        }
    }

    clear_computation_context_caches();
}

void StyleComputer::process_animation_definitions(ComputedStyleWorkingSet const& computed_properties, CascadedProperties const& cascaded_properties, DOM::AbstractElement& abstract_element) const
{
    auto const& animation_definitions = computed_properties.animations(abstract_element);

    auto& document = abstract_element.document();

    auto const* element_animations = abstract_element.css_defined_animations();

    // If we have a nullptr for element_animations it means that the pseudo element was invalid and thus we shouldn't apply animations
    if (!element_animations)
        return;

    // https://drafts.csswg.org/css-animations-1/#animations
    // Setting the 'display' property to 'none' will terminate any running animation applied to the element and its
    // descendants. If an element has a 'display' of 'none', updating 'display' to a value other than 'none' will
    // start all animations applied to the element by the 'animation-name' property, as well as all animations
    // applied to descendants with 'display' other than 'none'.
    // NB: We must not start animations on elements that are not rendered due to display:none. Once display becomes
    //     something other than none, the resulting style recomputation re-enters this function and starts them.
    //     Termination of running animations when display becomes none is handled by
    //     Element::play_or_cancel_animations_after_display_property_change().
    // OPTIMIZATION: This involves an ancestor walk, so it's computed lazily since it's only needed on the path that
    //               starts a brand new animation, not for the common case of an element without animations.
    Optional<bool> in_display_none_subtree;
    auto is_in_display_none_subtree = [&] {
        if (!in_display_none_subtree.has_value()) {
            bool result = computed_properties.display().is_none();
            if (!result) {
                if (abstract_element.pseudo_element().has_value())
                    result = abstract_element.element().has_inclusive_ancestor_with_display_none_ignoring_animations();
                else if (auto* parent = abstract_element.element().parent_or_shadow_host())
                    result = parent->has_inclusive_ancestor_with_display_none_ignoring_animations();
            }
            in_display_none_subtree = result;
        }
        return in_display_none_subtree.value();
    };

    // The same @keyframes rule name may be repeated within an animation-name. Changes to the animation-name update
    // existing animations by iterating over the new list of animations from last to first, and, for each animation,
    // finding the last matching animation in the list of existing animations. If a match is found, the existing
    // animation is updated using the animation properties corresponding to its position in the new list of animations,
    // whilst maintaining its current playback time as described above. The matching animation is removed from the
    // existing list of animations such that it will not match twice. If a match is not found, a new animation is
    // created. As a result, updating animation-name from ‘a’ to ‘a, a’ will cause the existing animation for ‘a’ to
    // become the second animation in the list and a new animation will be created for the first item in the list.

    auto existing_animations = *element_animations;
    Vector<GC::Ref<CSSAnimation>> new_animations;

    for (size_t i = animation_definitions.size(); i-- > 0;) {
        auto const& animation_properties = animation_definitions[i];
        auto const& animation_name = animation_properties.name;

        auto find_keyframes = [&](GC::Ptr<DOM::ShadowRoot const> shadow_root) -> RefPtr<Animations::KeyframeEffect::KeyFrameSet const> {
            auto& style_scope = shadow_root ? shadow_root->style_scope() : m_document->style_scope();
            style_scope.build_rule_cache_if_needed();
            if (auto keyframe_set = style_scope.rule_cache().rules_by_animation_keyframes.get(animation_name); keyframe_set.has_value())
                return keyframe_set.value();
            return {};
        };

        auto resolve_keyframes = [&]() -> RefPtr<Animations::KeyframeEffect::KeyFrameSet const> {
            if (auto animation_name_source_shadow_root = cascaded_properties.property_source_shadow_root(PropertyID::AnimationName)) {
                // The winning animation-name declaration can come from a shadow-root rule even when the animated
                // element itself is outside that subtree, most notably for :host(...) and ::slotted(...). Resolve
                // @keyframes in the declaration's scope first so same-named document rules do not win.
                if (auto keyframe_set = find_keyframes(animation_name_source_shadow_root))
                    return keyframe_set;
            }

            if (auto shadow_root = as_if<DOM::ShadowRoot>(abstract_element.element().root())) {
                if (auto keyframe_set = find_keyframes(shadow_root))
                    return keyframe_set;
            }

            return find_keyframes(nullptr);
        };

        Optional<size_t> existing_animation_index;

        for (size_t i = existing_animations.size(); i-- > 0;) {
            if (existing_animations[i]->animation_name() == animation_name) {
                existing_animation_index = i;
                break;
            }
        }

        if (existing_animation_index.has_value()) {
            auto existing_animation = existing_animations.take(*existing_animation_index);

            if (auto effect = existing_animation->effect()) {
                as<Animations::KeyframeEffect>(*effect).set_key_frame_set(resolve_keyframes());
                existing_animation->apply_css_properties(animation_properties);
            }
            existing_animation->set_animation_name_index(i);
            new_animations.append(existing_animation);
            continue;
        }

        if (is_in_display_none_subtree())
            continue;

        // An animation applies to an element if its name appears as one of the identifiers in the computed value of the
        // animation-name property and the animation uses a valid @keyframes rule
        auto animation = CSSAnimation::create(document.relevant_settings_object());
        animation->set_animation_name(animation_properties.name);
        animation->set_owning_element(abstract_element);

        auto effect = Animations::KeyframeEffect::create();
        animation->set_effect(effect);

        animation->apply_css_properties(animation_properties);
        animation->set_animation_name_index(i);

        effect->set_key_frame_set(resolve_keyframes());

        effect->set_target(abstract_element);
        new_animations.append(animation);
    }

    // Once an animation has started it continues until it ends or the animation-name is removed
    // NB: All animations that are matched by the new set of animations have been removed from `existing_animations` by
    //     this point so any still in the Vector have had their animation-name entries removed.
    for (auto const& existing_animation : existing_animations)
        existing_animation->cancel(Animations::Animation::ShouldInvalidate::No);

    // NB: We create animations in reverse definition order so flip it back.
    new_animations.reverse();

    abstract_element.set_css_defined_animations(move(new_animations));
}

static void collect_dimension_attribute(Vector<StyleProperty>& properties, DOM::Element const& element, Utf16FlyString const& attribute_name, CSS::PropertyID property_id)
{
    auto attribute = element.attribute(attribute_name);
    if (!attribute.has_value())
        return;

    auto parsed_value = HTML::parse_dimension_value(*attribute);
    if (!parsed_value)
        return;

    properties.append({ .property_id = property_id, .value = parsed_value.release_nonnull() });
}

static void compute_transitioned_properties(ComputedStyleWorkingSet const& style, DOM::AbstractElement abstract_element)
{
    // FIXME: For now we don't bother registering transitions on the first computation since they can't run (because
    //        there is nothing to transition from) but this will change once we implement @starting-style
    if (!abstract_element.has_style())
        return;
    // FIXME: Add transition helpers on AbstractElement.
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();

    element.clear_registered_transitions(pseudo_element);

    auto const& delay = style.property(PropertyID::TransitionDelay);
    auto const& duration = style.property(PropertyID::TransitionDuration);

    auto const value_is_list_containing_a_single_time_of_zero_seconds = [](StyleValue const& value) -> bool {
        if (!value.is_value_list())
            return false;

        auto const& value_list = value.as_value_list().values();

        if (value_list.size() != 1)
            return false;

        if (!value_list[0]->is_time())
            return false;

        return value_list[0]->as_time().time().to_seconds() == 0;
    };

    // OPTIMIZATION: Registered transitions with a "combined duration" of less than or equal to 0s are equivalent to not
    //               having a transition registered at all, except in the case that we already have an associated
    //               transition for that property, so we can skip registering them. This implementation intentionally
    //               ignores some of those cases (e.g. transitions being registered but for other properties, multiple
    //               transitions, negative delays, etc) since it covers the common (initial property values) case and
    //               the other cases are rare enough that the cost of identifying them would likely more than offset any
    //               gains.
    if (
        element.property_ids_with_existing_transitions(pseudo_element).is_empty()
        && value_is_list_containing_a_single_time_of_zero_seconds(delay)
        && value_is_list_containing_a_single_time_of_zero_seconds(duration)) {
        return;
    }

    element.add_transitioned_properties(pseudo_element, style.transitions());
}

static void compute_transitioned_properties(ComputedValues const& style, DOM::AbstractElement abstract_element)
{
    if (!abstract_element.has_style())
        return;

    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();
    element.clear_registered_transitions(pseudo_element);

    if (element.property_ids_with_existing_transitions(pseudo_element).is_empty()
        && style.transition_delay_and_duration_are_single_zero()) {
        return;
    }

    auto const& delays = style.transition_delays();
    auto const& durations = style.transition_durations();
    auto const& properties = style.transition_properties();
    auto const& timing_functions = style.transition_timing_functions();
    auto const& behaviors = style.transition_behaviors();
    VERIFY(!delays.is_empty());
    VERIFY(!durations.is_empty());
    VERIFY(!timing_functions.is_empty());
    VERIFY(!behaviors.is_empty());

    Vector<TransitionProperties> transitions;
    transitions.ensure_capacity(properties.size());
    for (size_t i = 0; i < properties.size(); ++i) {
        Vector<PropertyID> transition_properties;
        if (properties[i].has_value()) {
            auto maybe_property = property_id_from_string(*properties[i]);
            if (maybe_property.has_value()) {
                auto append_property_mapping_logical_aliases = [&](PropertyID property_id) {
                    if (property_is_logical_alias(property_id))
                        transition_properties.append(map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { style.writing_mode(), style.direction() }));
                    else if (property_id != PropertyID::Custom)
                        transition_properties.append(property_id);
                };
                auto transition_property = maybe_property.release_value();
                if (property_is_shorthand(transition_property)) {
                    for (auto property_id : expanded_longhands_for_shorthand(transition_property))
                        append_property_mapping_logical_aliases(property_id);
                } else {
                    append_property_mapping_logical_aliases(transition_property);
                }
            }
        }
        transitions.append({
            .properties = move(transition_properties),
            .duration = durations[i % durations.size()].to_milliseconds(),
            .timing_function = timing_functions[i % timing_functions.size()],
            .delay = delays[i % delays.size()].to_milliseconds(),
            .transition_behavior = behaviors[i % behaviors.size()],
        });
    }
    element.add_transitioned_properties(pseudo_element, transitions);
}

// https://drafts.csswg.org/css-transitions/#starting
Vector<GC::Ref<Animations::KeyframeEffect>> StyleComputer::start_needed_transitions(ComputedValues const& previous_style, ComputedStyleWorkingSet& new_style, DOM::AbstractElement abstract_element) const
{
    auto had_pending_animated_style_update = m_document->needs_animated_style_update();

    // FIXME: Add some transition helpers to AbstractElement.
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();
    auto style_node_id = element.style_node_id();
    Optional<u64> transition_target_key;
    if (style_node_id != 0)
        transition_target_key = (static_cast<u64>(style_node_id.value()) << 8) | pseudo_element_to_ffi(pseudo_element);
    auto const* transition_baseline = &previous_style;
    StyleRecordID transition_baseline_style_record;
    if (transition_target_key.has_value()
        && (abstract_element.style_scope().rule_cache().has_size_container_queries
            || document().is_in_style_stabilization_feedback_epoch())) {
        record_transition_stabilization_baseline(abstract_element);
    }
    if (transition_target_key.has_value()) {
        if (auto existing_baseline = m_transition_stabilization_baselines.get(*transition_target_key); existing_baseline.has_value())
            transition_baseline_style_record = *existing_baseline;
    }
    auto transition_baseline_view = computed_style_record_view(transition_baseline_style_record);
    if (transition_baseline_view)
        transition_baseline = &*transition_baseline_view;
    Vector<size_t> existing_stabilization_state_indices;
    if (transition_target_key.has_value()) {
        if (auto indices = m_provisional_transition_state_indices_by_target.get(*transition_target_key); indices.has_value())
            existing_stabilization_state_indices = *indices;
    } else {
        for (size_t index = 0; index < m_provisional_transition_states.size(); ++index) {
            auto const& state = m_provisional_transition_states[index];
            if (state.element == GC::Ptr { element } && state.pseudo_element == pseudo_element)
                existing_stabilization_state_indices.append(index);
        }
    }

    // NB: We know that a DocumentTimeline's current time is always in milliseconds
    auto current_time = m_document->timeline()->current_time();
    if (!current_time.has_value())
        return {};
    VERIFY(current_time->type == Animations::TimeValue::Type::Milliseconds);
    auto style_change_event_time = current_time->value;

    // OPTIMIZATION: The two lists below are what this decides over, and an element with neither
    //               starts nothing. Answering that first is worth doing because the after-change
    //               style is a whole computed style built for the comparison, and every recompute
    //               of every element that has a style at all reaches here.
    if (abstract_element.element().property_ids_with_matching_transition_property_entry(abstract_element.pseudo_element()).is_empty()
        && abstract_element.element().property_ids_with_existing_transitions(abstract_element.pseudo_element()).is_empty()
        && existing_stabilization_state_indices.is_empty())
        return {};

    u32 transition_groups_to_build = 0;
    auto include_transition_property_group = [&](PropertyID property_id) {
        auto group = ComputedValues::style_group_of_property(property_id);
        if (!group.has_value()) {
            transition_groups_to_build = ComputedValues::all_style_groups;
            return;
        }
        transition_groups_to_build |= 1u << to_underlying(*group);
    };
    for (auto property_id : element.property_ids_with_matching_transition_property_entry(pseudo_element))
        include_transition_property_group(property_id);
    for (auto property_id : element.property_ids_with_existing_transitions(pseudo_element))
        include_transition_property_group(property_id);
    for (auto stabilization_state_index : existing_stabilization_state_indices)
        include_transition_property_group(m_provisional_transition_states[stabilization_state_index].property_id);

    auto after_change_style = build_computed_values(new_style, abstract_element, abstract_element.style_scope(), &previous_style.base_values(), transition_groups_to_build);

    auto transition_font_metrics = [](Length::FontMetrics const& metrics) {
        return StyleValueFFI::FfiAnimationFontMetrics {
            .font_size = metrics.font_size.to_double(),
            .x_height = metrics.x_height.to_double(),
            .cap_height = metrics.cap_height.to_double(),
            .zero_advance = metrics.zero_advance.to_double(),
            .line_height = metrics.line_height.to_double(),
        };
    };
    auto const& transition_computation_context = get_computation_context_for_property(PropertyID::Color, new_style, abstract_element);
    auto const& transition_length_context = transition_computation_context.length_resolution_context;
    StyleValueFFI::FfiAnimationContext transition_animation_context {
        .allow_discrete = false,
        .current_color = new_style.property(PropertyID::Color).rust_style_value_data(),
        .has_length_resolution_context = true,
        .length_resolution_context = {
            .viewport_width = transition_length_context.viewport_rect.width().to_double(),
            .viewport_height = transition_length_context.viewport_rect.height().to_double(),
            .font_metrics = transition_font_metrics(transition_length_context.font_metrics),
            .root_font_metrics = transition_font_metrics(transition_length_context.root_font_metrics),
            .font_metrics_depend_on_viewport_metrics = transition_length_context.font_metrics_depend_on_viewport_metrics,
            .root_font_metrics_depend_on_viewport_metrics = transition_length_context.root_font_metrics_depend_on_viewport_metrics,
        },
        .has_transform_reference_box = false,
        .transform_reference_box_width = 0,
        .transform_reference_box_height = 0,
    };
    if (auto const* layout_node = abstract_element.element().unsafe_layout_node(); layout_node && Painting::has_committed_box(*layout_node)) {
        auto reference_box = Painting::transform_reference_box(*layout_node);
        transition_animation_context.has_transform_reference_box = true;
        transition_animation_context.transform_reference_box_width = reference_box.width().to_double();
        transition_animation_context.transform_reference_box_height = reference_box.height().to_double();
    }
    clear_computation_context_caches();

    struct PreparedTransition {
        size_t stabilization_state_index;
        PropertyID property_id;
        RefPtr<StyleValue const> before_change_value;
        RefPtr<StyleValue const> after_change_value;
        RefPtr<StyleValue const> current_value;
        GC::Ptr<CSSTransition> existing_transition;
    };
    Vector<PreparedTransition> prepared_transitions;
    Vector<StyleValueFFI::FfiTransitionPropertyInput> ffi_properties;

    enum class HasMatchingTransition {
        No,
        Yes,
    };
    auto originates_from_current_color = [](ComputedValues const& style, PropertyID property_id) {
        auto value = style.computed_style_value_for_inheritance(property_id, ComputedValues::WithAnimationsApplied::No);
        return value && value->to_keyword() == Keyword::Currentcolor;
    };
    auto ensure_stabilization_state = [&](PropertyID property_id) -> size_t {
        Optional<u64> state_key;
        if (transition_target_key.has_value()) {
            auto property = to_underlying(property_id);
            VERIFY(property <= NumericLimits<u16>::max());
            state_key = (*transition_target_key << 16) | property;
            if (auto index = m_provisional_transition_state_indices.get(*state_key); index.has_value())
                return *index;
        } else {
            for (size_t index = 0; index < m_provisional_transition_states.size(); ++index) {
                auto const& state = m_provisional_transition_states[index];
                if (state.element == GC::Ptr { element } && state.pseudo_element == pseudo_element && state.property_id == property_id)
                    return index;
            }
        }
        auto existing_transition = element.property_transition(pseudo_element, property_id);
        m_provisional_transition_states.append({
            .element = element,
            .pseudo_element = pseudo_element,
            .property_id = property_id,
            .before_change_value = transition_baseline->computed_style_value(property_id, ComputedValues::WithAnimationsApplied::Yes),
            .before_change_value_originates_from_current_color = originates_from_current_color(*transition_baseline, property_id),
            .committed_transition = existing_transition,
            .proposed_transition = nullptr,
            .action = ProvisionalTransitionAction::None,
            .has_decision = false,
        });
        auto index = m_provisional_transition_states.size() - 1;
        if (state_key.has_value()) {
            m_provisional_transition_state_indices.set(*state_key, index);
            m_provisional_transition_state_indices_by_target.ensure(*transition_target_key).append(index);
        }
        return index;
    };
    auto append_transition_input = [&](PropertyID property_id, HasMatchingTransition has_matching_transition) {
        auto stabilization_state_index = ensure_stabilization_state(property_id);
        auto const& stabilization_state = m_provisional_transition_states[stabilization_state_index];
        auto existing_transition = stabilization_state.committed_transition;
        bool has_running_transition = existing_transition && !existing_transition->is_finished() && !existing_transition->is_idle();
        bool has_completed_transition = existing_transition && !has_running_transition;
        RefPtr<StyleValue const> before_change_value;
        RefPtr<StyleValue const> after_change_value;
        RefPtr<StyleValue const> current_value;
        bool before_change_value_originates_from_current_color = false;
        bool after_change_value_originates_from_current_color = false;
        bool allow_discrete = false;
        double delay = 0;
        double duration = 0;
        double old_timing_function_output = 0;
        double old_reversing_shortening_factor = 1;

        if (has_matching_transition == HasMatchingTransition::Yes) {
            auto transition_attributes = element.property_transition_attributes(pseudo_element, property_id).value();
            delay = transition_attributes.delay;
            duration = transition_attributes.duration;
            allow_discrete = transition_attributes.transition_behavior == TransitionBehavior::AllowDiscrete;
            // https://drafts.csswg.org/css-transitions-1/#starting
            // The after-change style excludes styles from CSS Transitions but keeps animation-derived values, and the
            // before-change style has declarative animations updated to the current time. A property that a
            // non-transition animation currently applies to therefore carries the same animated value on both sides of
            // the comparison, so a change to the underlying value cannot start a transition beneath a running
            // animation.
            RefPtr<StyleValue const> value_covered_by_animation;
            if (auto const* animated_properties = after_change_style->animated_properties();
                animated_properties && animated_properties->has_property(property_id)
                && !animated_properties->is_property_result_of_transition(property_id))
                value_covered_by_animation = animated_properties->property(property_id);

            if (value_covered_by_animation) {
                before_change_value = value_covered_by_animation;
                after_change_value = value_covered_by_animation;
                before_change_value_originates_from_current_color = value_covered_by_animation->to_keyword() == Keyword::Currentcolor;
                after_change_value_originates_from_current_color = before_change_value_originates_from_current_color;
            } else {
                before_change_value = stabilization_state.before_change_value;
                after_change_value = after_change_style->computed_style_value(property_id, ComputedValues::WithAnimationsApplied::No);
                before_change_value_originates_from_current_color = stabilization_state.before_change_value_originates_from_current_color;
                after_change_value_originates_from_current_color = originates_from_current_color(*after_change_style, property_id);
            }
            VERIFY(before_change_value);
            VERIFY(after_change_value);
            if (existing_transition) {
                old_reversing_shortening_factor = existing_transition->reversing_shortening_factor();
                if (has_running_transition)
                    old_timing_function_output = existing_transition->timing_function_output_at_time(style_change_event_time);
            }
            if (has_running_transition) {
                current_value = after_change_style->computed_style_value(property_id, ComputedValues::WithAnimationsApplied::Yes);
                VERIFY(current_value);
            }
        }

        ffi_properties.append({
            .property_id = to_underlying(property_id),
            .before_change_value = before_change_value ? before_change_value->rust_style_value_data() : nullptr,
            .after_change_value = after_change_value ? after_change_value->rust_style_value_data() : nullptr,
            .current_value = current_value ? current_value->rust_style_value_data() : nullptr,
            .existing_end_value = existing_transition ? existing_transition->transition_end_value()->rust_style_value_data() : nullptr,
            .reversing_adjusted_start_value = existing_transition ? existing_transition->reversing_adjusted_start_value()->rust_style_value_data() : nullptr,
            .has_matching_transition = has_matching_transition == HasMatchingTransition::Yes,
            .allow_discrete = allow_discrete,
            .before_change_value_originates_from_current_color = before_change_value_originates_from_current_color,
            .after_change_value_originates_from_current_color = after_change_value_originates_from_current_color,
            .has_running_transition = has_running_transition,
            .has_completed_transition = has_completed_transition,
            .delay = delay,
            .duration = duration,
            .old_timing_function_output = old_timing_function_output,
            .old_reversing_shortening_factor = old_reversing_shortening_factor,
        });
        prepared_transitions.append({
            .stabilization_state_index = stabilization_state_index,
            .property_id = property_id,
            .before_change_value = move(before_change_value),
            .after_change_value = move(after_change_value),
            .current_value = move(current_value),
            .existing_transition = existing_transition,
        });
    };

    // OPTIMIZATION: Instead of iterating over all properties we collect properties which appear in
    //               transition-property, followed by existing transitions without a matching entry.
    for (auto property_id : element.property_ids_with_matching_transition_property_entry(pseudo_element))
        append_transition_input(property_id, HasMatchingTransition::Yes);
    for (auto property_id : element.property_ids_with_existing_transitions(pseudo_element)) {
        if (!element.property_transition_attributes(pseudo_element, property_id).has_value())
            append_transition_input(property_id, HasMatchingTransition::No);
    }

    for (auto stabilization_state_index : existing_stabilization_state_indices) {
        auto& stabilization_state = m_provisional_transition_states[stabilization_state_index];
        bool has_prepared_transition = false;
        for (auto const& prepared_transition : prepared_transitions) {
            if (prepared_transition.stabilization_state_index == stabilization_state_index) {
                has_prepared_transition = true;
                break;
            }
        }
        if (has_prepared_transition)
            continue;
        ++document().style_invalidation_counters().provisional_transition_decisions;
        if (stabilization_state.has_decision)
            ++document().style_invalidation_counters().superseded_provisional_transition_decisions;
        stabilization_state.has_decision = true;
        if (stabilization_state.proposed_transition)
            stabilization_state.proposed_transition->discard_provisional_transition();
        stabilization_state.proposed_transition = nullptr;
        stabilization_state.action = ProvisionalTransitionAction::None;
    }

    StyleValueFFI::FfiTransitionInput input {
        .context = transition_animation_context,
        .properties = ffi_properties.data(),
        .property_count = ffi_properties.size(),
    };
    Vector<StyleValueFFI::FfiTransitionAction> actions;
    actions.resize(prepared_transitions.size());
    StyleValueFFI::rust_decide_transitions(&input, actions.data());

    Vector<GC::Ref<Animations::KeyframeEffect>> newly_started_transition_effects;
    HashTable<Animations::KeyframeEffect*> replaced_transition_effects;
    for (size_t index = 0; index < prepared_transitions.size(); ++index) {
        auto const& prepared_transition = prepared_transitions[index];
        auto& stabilization_state = m_provisional_transition_states[prepared_transition.stabilization_state_index];
        auto property_id = prepared_transition.property_id;
        auto const& action = actions[index];
        VERIFY(action.property_id == to_underlying(property_id));
        auto existing_transition = prepared_transition.existing_transition;
        ++document().style_invalidation_counters().provisional_transition_decisions;
        if (stabilization_state.has_decision)
            ++document().style_invalidation_counters().superseded_provisional_transition_decisions;
        stabilization_state.has_decision = true;
        if (stabilization_state.proposed_transition)
            stabilization_state.proposed_transition->discard_provisional_transition();
        stabilization_state.proposed_transition = nullptr;
        auto start_a_transition = [&](StyleValue const& start_value, StyleValue const& end_value, StyleValue const& reversing_adjusted_start_value) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Proposing a transition of {} from {} to {}", string_from_property_id(property_id), start_value.to_string(SerializationMode::Normal), end_value.to_string(SerializationMode::Normal));
            auto start_time = style_change_event_time;
            auto end_time = start_time + action.active_duration;
            auto transition = CSSTransition::start_a_transition(abstract_element, property_id,
                document().transition_generation(), action.delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, action.reversing_shortening_factor, CSSTransition::Publication::Provisional);
            stabilization_state.proposed_transition = transition;
            newly_started_transition_effects.append(as<Animations::KeyframeEffect>(*transition->effect()));
        };
        auto replace_existing_transition = [&] {
            VERIFY(existing_transition);
            auto effect = existing_transition->effect();
            if (effect && effect->is_keyframe_effect())
                replaced_transition_effects.set(static_cast<Animations::KeyframeEffect*>(effect.ptr()));
        };

        switch (action.kind) {
        case StyleValueFFI::FfiTransitionActionKind::None:
            stabilization_state.action = ProvisionalTransitionAction::None;
            break;
        case StyleValueFFI::FfiTransitionActionKind::Remove:
            stabilization_state.action = ProvisionalTransitionAction::Remove;
            replace_existing_transition();
            break;
        case StyleValueFFI::FfiTransitionActionKind::Cancel:
            VERIFY(existing_transition);
            stabilization_state.action = ProvisionalTransitionAction::Cancel;
            replace_existing_transition();
            break;
        case StyleValueFFI::FfiTransitionActionKind::Start:
            stabilization_state.action = ProvisionalTransitionAction::Start;
            start_a_transition(*prepared_transition.before_change_value, *prepared_transition.after_change_value, *prepared_transition.before_change_value);
            break;
        case StyleValueFFI::FfiTransitionActionKind::RemoveAndStart:
            stabilization_state.action = ProvisionalTransitionAction::RemoveAndStart;
            replace_existing_transition();
            start_a_transition(*prepared_transition.before_change_value, *prepared_transition.after_change_value, *prepared_transition.before_change_value);
            break;
        case StyleValueFFI::FfiTransitionActionKind::CancelRemoveAndStartReversing: {
            VERIFY(existing_transition);
            auto reversing_adjusted_start_value = existing_transition->transition_end_value();
            stabilization_state.action = ProvisionalTransitionAction::CancelRemoveAndStart;
            replace_existing_transition();
            start_a_transition(*prepared_transition.current_value, *prepared_transition.after_change_value, *reversing_adjusted_start_value);
            break;
        }
        case StyleValueFFI::FfiTransitionActionKind::CancelRemoveAndStartInterrupted:
            stabilization_state.action = ProvisionalTransitionAction::CancelRemoveAndStart;
            replace_existing_transition();
            start_a_transition(*prepared_transition.current_value, *prepared_transition.after_change_value, *prepared_transition.current_value);
            break;
        }
    }

    // A transition action is provisional until the stabilization epoch commits, but the style
    // published by this pass must already reflect that decision. Rebuild the effect stack without
    // transitions which are being removed, then layer any proposed replacements on top.
    if (!replaced_transition_effects.is_empty()) {
        new_style.clear_animated_properties(Badge<StyleComputer> {});
        auto animations = abstract_element.element().get_animations_internal(
            Animations::Animatable::GetAnimationsSorted::Yes,
            Animations::Animatable::GetAnimationsOptions { .subtree = false, .pseudo_element = {} });
        if (animations.is_exception()) {
            dbgln("Error getting animations for element {}", abstract_element.debug_description());
        } else {
            GC::RootVector<GC::Ref<Animations::KeyframeEffect>> remaining_effects;
            for (auto& animation : animations.value()) {
                auto effect = animation->effect();
                if (!effect || !effect->is_keyframe_effect())
                    continue;
                auto& keyframe_effect = static_cast<Animations::KeyframeEffect&>(*effect);
                if (keyframe_effect.pseudo_element_type() != abstract_element.pseudo_element())
                    continue;
                if (replaced_transition_effects.contains(&keyframe_effect))
                    continue;
                remaining_effects.append(keyframe_effect);
            }
            if (!remaining_effects.is_empty())
                collect_animations_into(abstract_element, remaining_effects.span(), new_style, AnimationRefresh::No);
        }
    }

    // Immediately set the properties to the transitions' current values, to prevent single-frame jumps.
    if (!newly_started_transition_effects.is_empty()) {
        collect_animations_into(abstract_element, newly_started_transition_effects.span(), new_style, AnimationRefresh::No);
        // NB: Construction does not invalidate animated style because the effects were just evaluated. Request the
        //     first animation frame directly so timeline updates can schedule subsequent animated style updates.
        m_document->page().client().request_frame();
        if (!had_pending_animated_style_update)
            m_document->clear_needs_animated_style_update();
    }

    return newly_started_transition_effects;
}

// The encapsulation contexts that decide for an element, outermost first.
//
// https://drafts.csswg.org/css-cascade-5/#cascade-context
// The order is what the cascade applies them in without re-sorting individual declarations: for a
// normal declaration the outer context wins. Most elements only have the document context, and the
// small vector avoids heap storage for the common shadow-depth cases.
Vector<GC::Ptr<DOM::ShadowRoot const>, 4> StyleComputer::author_context_shadow_roots(DOM::AbstractElement abstract_element)
{
    Vector<GC::Ptr<DOM::ShadowRoot const>, 4> context_shadow_roots;
    auto append_context = [&](GC::Ptr<DOM::ShadowRoot const> shadow_root) {
        if (context_shadow_roots.contains_slow(shadow_root))
            return;
        context_shadow_roots.append(shadow_root);
    };

    append_context(nullptr);

    if (auto const* shadow_root = as_if<DOM::ShadowRoot>(abstract_element.element().root())) {
        Vector<GC::Ref<DOM::ShadowRoot const>, 4> ancestor_shadow_roots;
        for (auto const* current_shadow_root = shadow_root; current_shadow_root;) {
            ancestor_shadow_roots.append(*current_shadow_root);
            auto const* host = current_shadow_root->host();
            if (!host)
                break;
            current_shadow_root = as_if<DOM::ShadowRoot>(host->root());
        }
        for (auto& ancestor_shadow_root : ancestor_shadow_roots.in_reverse())
            append_context(ancestor_shadow_root);
    }

    if (!is<HTML::HTMLSlotElement>(abstract_element.element()) || !abstract_element.element().root().is_shadow_root()) {
        for (GC::Ptr<HTML::HTMLSlotElement const> slot = abstract_element.element().assigned_slot_internal(); slot; slot = slot->assigned_slot_internal()) {
            if (auto const* slot_shadow_root = as_if<DOM::ShadowRoot>(slot->root()))
                append_context(slot_shadow_root);
        }
    }

    if (auto element_shadow_root = abstract_element.element().shadow_root())
        append_context(element_shadow_root);

    return context_shadow_roots;
}

void StyleComputer::register_style_engine_rule_target(CSSRule const& rule, StyleEngineRuleTarget target)
{
    m_style_engine_rule_targets.set(&rule, move(target));
}

void StyleComputer::register_style_engine_rule_identity(StyleEngineRuleID rule_id, CSSRule const& rule)
{
    m_style_engine_rules_by_id.set(rule_id, &rule);
}

Optional<StyleEngineRuleTarget> StyleComputer::style_engine_rule_target(StyleEngineRuleID rule_id) const
{
    auto rule = m_style_engine_rules_by_id.get(rule_id);
    if (!rule.has_value() || !*rule)
        return {};
    auto it = m_style_engine_rule_targets.find(rule->ptr());
    if (it == m_style_engine_rule_targets.end())
        return {};
    // NB: Building a rule cache can register more targets and rehash this map. Return a copy so the
    //     caller can safely keep the target while asking a style scope for its rule cache.
    return it->value;
}

StyleEngineRuleID StyleComputer::style_engine_rule_id_for(CSSRule const& rule) const
{
    // A non-author rule's identity is held per document; an author rule's lives on the rule itself.
    if (auto id = m_non_author_rule_ids.get(&rule); id.has_value())
        return *id;
    if (auto id = m_constructed_rule_ids.get(&rule); id.has_value())
        return *id;
    return rule.style_engine_rule_id();
}

SheetID StyleComputer::style_engine_sheet_id_for(CSSStyleSheet const& sheet) const
{
    if (sheet.constructed())
        return m_constructed_sheet_ids.get(&sheet).value_or(0);
    return sheet.style_engine_sheet_id();
}

void StyleComputer::set_style_engine_sheet_id_for(CSSStyleSheet& sheet, SheetID sheet_id)
{
    if (sheet.constructed())
        m_constructed_sheet_ids.set(&sheet, sheet_id);
    else
        sheet.set_style_engine_sheet_id(sheet_id);
}

static bool custom_property_inherits(DOM::Document const& document, Utf16FlyString const& name)
{
    // A custom property inherits unless it has been registered with an explicit `inherits: false`.
    auto registration = document.get_registered_custom_property(name);
    return !registration.has_value() || registration->inherit;
}

enum class IsCustomProperty : u8 {
    No,
    Yes,
};

enum class Inherits : u8 {
    No,
    Yes,
};

enum class NameIsValid : u8 {
    No,
    Yes,
};

enum class IsValid : u8 {
    No,
    Yes,
};

static JsonObject serialize_devtools_style_declaration(
    String name,
    String value,
    Important important,
    IsCustomProperty is_custom_property,
    Inherits inherits,
    NameIsValid is_name_valid,
    IsValid is_valid)
{
    JsonObject serialized_property;
    serialized_property.set("name"sv, move(name));
    serialized_property.set("value"sv, move(value));
    serialized_property.set("priority"sv, important == Important::Yes ? "important"sv : ""sv);
    serialized_property.set("isCustomProperty"sv, is_custom_property == IsCustomProperty::Yes);
    serialized_property.set("inherits"sv, inherits == Inherits::Yes);
    serialized_property.set("isNameValid"sv, is_name_valid == NameIsValid::Yes);
    serialized_property.set("isValid"sv, is_valid == IsValid::Yes);
    return serialized_property;
}

static JsonArray serialize_devtools_style_declarations(DOM::Document const& document, CSSStyleProperties const& declaration)
{
    JsonArray declarations;

    auto serialize_property = [&](Utf16FlyString const& name, StyleProperty const& property, IsCustomProperty is_custom_property, Inherits inherits) {
        declarations.must_append(serialize_devtools_style_declaration(
            name.to_utf16_string().to_utf8_but_should_be_ported_to_utf16(),
            property.value->to_string(SerializationMode::Normal),
            property.important,
            is_custom_property,
            inherits,
            NameIsValid::Yes,
            IsValid::Yes));
    };

    for (auto const& property : declaration.properties()) {
        serialize_property(
            string_from_property_id(property.property_id),
            property,
            IsCustomProperty::No,
            is_inherited_property(property.property_id) ? Inherits::Yes : Inherits::No);
    }

    for (auto const& custom_property : declaration.custom_properties())
        serialize_property(
            custom_property.key,
            custom_property.value,
            IsCustomProperty::Yes,
            custom_property_inherits(document, custom_property.key) ? Inherits::Yes : Inherits::No);

    return declarations;
}

static JsonArray serialize_devtools_style_declarations(DOM::Document const& document, Vector<Parser::DevToolsStyleDeclaration> const& declarations)
{
    JsonArray serialized_declarations;

    for (auto const& declaration : declarations) {
        bool inherits = declaration.is_custom_property
            ? custom_property_inherits(document, declaration.name)
            : PropertyNameAndID::from_name(declaration.name)
                  .map([](auto const& property) { return !property.is_custom_property() && is_inherited_property(property.id()); })
                  .value_or(false);

        serialized_declarations.must_append(serialize_devtools_style_declaration(
            MUST(declaration.name.view().to_utf8()),
            declaration.value.to_utf8(),
            declaration.important,
            declaration.is_custom_property ? IsCustomProperty::Yes : IsCustomProperty::No,
            inherits ? Inherits::Yes : Inherits::No,
            declaration.is_name_valid ? NameIsValid::Yes : NameIsValid::No,
            declaration.is_valid ? IsValid::Yes : IsValid::No));
    }

    return serialized_declarations;
}

static Vector<Parser::DevToolsStyleDeclaration> parse_devtools_style_declarations(DOM::Document const& document, StringView declaration_block)
{
    return Parser::parse_css_declaration_block_for_devtools(Parser::ParsingParams(document), declaration_block);
}

static Vector<Parser::DevToolsStyleDeclaration> parse_devtools_style_declarations(DOM::Document const& document, Utf16View declaration_block)
{
    return Parser::parse_css_declaration_block_for_devtools(Parser::ParsingParams(document), declaration_block);
}

static Optional<size_t> source_offset_for_line_and_column(StringView source, SourcePosition const& position)
{
    size_t line = 0;
    size_t column = 0;

    Utf8View source_code_points { source };
    for (auto it = source_code_points.begin(); it != source_code_points.end();) {
        auto offset = source_code_points.byte_offset_of(it);
        if (line == position.line && column == position.column)
            return offset;

        auto code_point = *it;
        ++it;

        if (code_point == '\r') {
            if (offset + 1 < source.length() && source[offset + 1] == '\n')
                ++it;
            ++line;
            column = 0;
        } else if (code_point == '\n' || code_point == '\f') {
            ++line;
            column = 0;
        } else {
            ++column;
        }
    }

    if (line == position.line && column == position.column)
        return source.length();

    return {};
}

static Optional<String> extract_css_declaration_block_from_source(CSSRule const& rule)
{
    if (rule.type() != CSSRule::Type::Style)
        return {};

    auto const* style_sheet = rule.parent_style_sheet();
    if (!style_sheet)
        return {};

    auto const source_text = style_sheet->source_text();
    if (!source_text.has_value())
        return {};

    auto source = source_text->to_utf8();
    auto source_view = source.bytes_as_string_view();
    auto const& source_location = rule.source_location();
    if (!source_location.has_value())
        return {};

    auto maybe_offset = source_offset_for_line_and_column(source_view, *source_location);
    if (!maybe_offset.has_value())
        return {};

    Optional<u8> string_quote;
    bool in_comment = false;
    bool escaped = false;
    Optional<size_t> block_start;
    size_t block_depth = 0;

    for (size_t offset = *maybe_offset; offset < source_view.length(); ++offset) {
        auto ch = source_view[offset];
        auto next_ch = offset + 1 < source_view.length() ? source_view[offset + 1] : '\0';

        if (in_comment) {
            if (ch == '*' && next_ch == '/') {
                in_comment = false;
                ++offset;
            }
            continue;
        }

        if (string_quote.has_value()) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == *string_quote)
                string_quote = {};
            continue;
        }

        if (ch == '/' && next_ch == '*') {
            in_comment = true;
            ++offset;
            continue;
        }

        if (ch == '"' || ch == '\'') {
            string_quote = ch;
            continue;
        }

        if (ch == '{') {
            if (!block_start.has_value())
                block_start = offset + 1;
            ++block_depth;
            continue;
        }

        if (ch == '}' && block_start.has_value()) {
            VERIFY(block_depth > 0);
            --block_depth;
            if (block_depth == 0)
                return MUST(String::from_utf8(source_view.substring_view(*block_start, offset - *block_start)));
        }
    }

    return {};
}

static bool has_inherited_declaration(DOM::Document const& document, CSSStyleProperties const& declaration)
{
    if (any_of(declaration.properties(), [](auto const& property) {
            return CSS::is_inherited_property(property.property_id);
        })) {
        return true;
    }

    return any_of(declaration.custom_properties(), [&](auto const& custom_property) {
        return custom_property_inherits(document, custom_property.key);
    });
}

static JsonObject serialize_devtools_style_sheet_identifier(StyleSheetIdentifier const& identifier)
{
    JsonObject serialized_identifier;
    serialized_identifier.set("type"sv, style_sheet_identifier_type_to_string(identifier.type));
    if (identifier.dom_element_unique_id.has_value())
        serialized_identifier.set("domElementUniqueId"sv, identifier.dom_element_unique_id->value());
    if (identifier.url.has_value())
        serialized_identifier.set("url"sv, identifier.url->to_utf8());
    serialized_identifier.set("ruleCount"sv, identifier.rule_count);
    return serialized_identifier;
}

// What DevTools shows for one rule the engine says decides for this element. Which of the rule's
// selectors matched is asked through the ad-hoc engine query path because the engine reports the
// rule rather than the entry, and a panel can afford to ask a question a style pass cannot.
static JsonObject serialize_devtools_applied_rule(DOM::Document& document, CSSRule const& rule, DOM::AbstractElement const& element)
{
    auto const& declaration = declaration_of_rule(rule);
    auto authored_text = extract_css_declaration_block_from_source(rule);
    SelectorList const* selector_list = nullptr;
    if (auto const* style_rule = as_if<CSSStyleRule>(rule))
        selector_list = &style_rule->absolutized_selectors();
    else if (auto const* nested = as_if<CSSNestedDeclarations>(rule))
        selector_list = &nested->parent_style_rule().absolutized_selectors();
    SelectorList const empty_selectors;
    auto const& selectors = selector_list ? *selector_list : empty_selectors;

    JsonArray serialized_selectors;
    JsonArray specificities;
    JsonArray matched_selector_indexes;
    for (size_t index = 0; index < selectors.size(); ++index) {
        auto const& selector = selectors[index];
        serialized_selectors.must_append(selector->serialize().to_utf8());
        specificities.must_append(selector->specificity());
        SelectorList selector_query_list;
        selector_query_list.append(selector);
        auto selector_query = DOM::SelectorQuery::create(document, move(selector_query_list));
        if (selector_query->matches(element.element(), document))
            matched_selector_indexes.must_append(index);
    }

    JsonObject serialized_rule;
    serialized_rule.set("type"sv, to_underlying(rule.type()));
    serialized_rule.set("className"sv, rule.type() == CSSRule::Type::Style ? "CSSStyleRule"sv : "CSSNestedDeclarations"sv);
    serialized_rule.set("selectors"sv, move(serialized_selectors));
    serialized_rule.set("selectorsSpecificity"sv, move(specificities));
    serialized_rule.set("matchedSelectorIndexes"sv, move(matched_selector_indexes));
    serialized_rule.set("cssText"sv, rule.serialized().to_utf8());
    if (authored_text.has_value()) {
        serialized_rule.set("authoredText"sv, *authored_text);
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, parse_devtools_style_declarations(document, authored_text->bytes_as_string_view())));
    } else {
        serialized_rule.set("authoredText"sv, declaration.serialized().to_utf8());
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, declaration));
    }
    if (auto* sheet = rule.parent_style_sheet()) {
        if (auto identifier = style_sheet_identifier_for(*sheet); identifier.has_value())
            serialized_rule.set("ruleId"sv, serialize_devtools_style_sheet_identifier(*identifier));
    }
    return serialized_rule;
}

static JsonObject serialize_devtools_inline_style(DOM::Document const& document, DOM::AbstractElement abstract_element, CSSStyleProperties const& declaration)
{
    auto authored_text = abstract_element.element().get_attribute(HTML::AttributeNames::style);

    JsonObject serialized_rule;
    serialized_rule.set("type"sv, 100);
    serialized_rule.set("className"sv, 100);
    serialized_rule.set("cssText"sv, declaration.serialized().to_utf8());
    if (authored_text.has_value()) {
        auto authored_text_utf8 = authored_text->to_utf8();
        serialized_rule.set("authoredText"sv, authored_text_utf8);
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, parse_devtools_style_declarations(document, authored_text->utf16_view())));
    } else {
        serialized_rule.set("authoredText"sv, declaration.serialized().to_utf8());
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, declaration));
    }
    serialized_rule.set("isSystem"sv, false);
    serialized_rule.set("nodeId"sv, abstract_element.element().unique_id().value());
    return serialized_rule;
}

static void append_devtools_applied_style_entry(JsonArray& entries, JsonObject rule, Optional<UniqueNodeID> inherited_node_id = {})
{
    JsonObject entry;

    JsonValue matched_selector_indexes { JsonArray {} };
    if (auto value = rule.get("matchedSelectorIndexes"sv); value.has_value())
        matched_selector_indexes = *value;
    rule.remove("matchedSelectorIndexes"sv);
    auto is_system = rule.get_bool("isSystem"sv).value_or(false);

    entry.set("rule"sv, move(rule));
    entry.set("isSystem"sv, is_system);
    entry.set("matchedSelectorIndexes"sv, move(matched_selector_indexes));
    if (inherited_node_id.has_value())
        entry.set("inheritedNodeId"sv, inherited_node_id->value());
    else
        entry.set("inherited"sv, JsonValue {});

    entries.must_append(move(entry));
}

JsonArray StyleComputer::collect_devtools_applied_style_rules(DOM::AbstractElement abstract_element, bool include_inherited, bool include_user_agent_styles)
{
    JsonArray entries;

    auto append_rules_for_abstract_element = [&](DOM::AbstractElement current_element, Optional<UniqueNodeID> inherited_node_id) {
        if (auto inline_style = current_element.inline_style()) {
            if (!inherited_node_id.has_value() || has_inherited_declaration(m_document, *inline_style))
                append_devtools_applied_style_entry(entries, serialize_devtools_inline_style(m_document, current_element, *inline_style), inherited_node_id);
        }

        auto node = current_element.element().style_node_id();
        if (node == 0)
            return;
        Vector<StyleEngine::RuleMatch> matches;
        if (!style_engine().match_element(node, matches, StyleEngine::MatchPurpose::Exact))
            return;

        // The engine reports the rules in the order the cascade applies them, and the panel lists
        // the winning one first.
        for (auto const& match : matches.in_reverse()) {
            if (match.pseudo_element != NumericLimits<u32>::max())
                continue;
            auto target = style_engine_rule_target(StyleEngineRuleID { match.rule });
            if (!target.has_value() || !target->rule)
                continue;
            if (target->cascade_origin == CascadeOrigin::UserAgent && !include_user_agent_styles)
                continue;
            if (inherited_node_id.has_value() && !has_inherited_declaration(m_document, declaration_of_rule(*target->rule)))
                continue;
            append_devtools_applied_style_entry(entries, serialize_devtools_applied_rule(m_document, *target->rule, current_element), inherited_node_id);
        }
    };

    append_rules_for_abstract_element(abstract_element, {});

    if (!include_inherited)
        return entries;

    for (auto current_element = abstract_element.element_to_inherit_style_from(); current_element.has_value(); current_element = current_element->element_to_inherit_style_from())
        append_rules_for_abstract_element(*current_element, current_element->element().unique_id());

    return entries;
}

static bool block_declares_custom_properties(OrderedHashMap<Utf16FlyString, StyleProperty> const* custom_properties)
{
    return custom_properties && !custom_properties->is_empty();
}

// Whether a declaration block holds anything that reads the inherited custom property environment:
// a declaration of a custom property, or a value with a substitution still to make. This is what
// decides whether the style sharing key has to name that environment. The same serializer that
// names each block answers this for every reuse path.
static bool block_reads_custom_properties(ReadonlySpan<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> const* custom_properties)
{
    if (block_declares_custom_properties(custom_properties))
        return true;
    return any_of(properties, [](auto const& property) { return property.value->is_unresolved(); });
}

static bool block_reads_style_scope(ReadonlySpan<StyleProperty> properties)
{
    return any_of(properties, [](auto const& property) {
        return property.property_id == PropertyID::Content
            || property.property_id == PropertyID::ListStyleType
            || property.value->is_unresolved();
    });
}

enum class CascadeBlockKeyValueComparison : u8 {
    ByIdentity,
    ByValue,
};

struct CascadeBlockKey {
    ReadonlySpan<StyleProperty> properties;
    OrderedHashMap<Utf16FlyString, StyleProperty> const* custom_properties { nullptr };
    CascadeOrigin origin { CascadeOrigin::Author };
    u32 author_context_index { 0 };
    u32 layer_index { 0 };
    bool is_inline_style { false };
    bool bypass_pseudo_element_property_whitelist { false };
    bool is_layered { false };
    GC::Ptr<CSSStyleProperties const> source {};
    GC::Ptr<DOM::ShadowRoot const> source_shadow_root {};
    u32 semantic_declaration_id { 0 };
};

// Serialize every declaration block the computation is allowed to read. A sharing key names a
// freshly mapped presentational-hint value by identity and pins it for the transaction; a persistent
// input record pins the same value but compares it by value, since mapping the same element's hint
// again is allowed to produce a fresh object.
struct CascadeBlockKeyDependencies {
    bool reads_custom_properties { false };
    bool declares_custom_properties { false };
    bool reads_style_scope { false };
};

static CascadeBlockKeyDependencies append_cascade_blocks_to_key(Vector<u64>& key, Vector<NonnullRefPtr<StyleValue const>>& pinned_values, StyleComputer::CascadeInput const& cascade_input, ReadonlySpan<StyleProperty> presentational_hint_properties, GC::Ptr<CSSStyleProperties const> inline_style, CascadeBlockKeyValueComparison value_comparison)
{
    auto append_block = [&](CascadeBlockKey const& block) {
        key.append(to_underlying(block.origin) | (static_cast<u64>(block.author_context_index) << 8) | (static_cast<u64>(block.layer_index) << 40));
        key.append(static_cast<u64>(block.is_inline_style)
            | (static_cast<u64>(block.bypass_pseudo_element_property_whitelist) << 1)
            | (static_cast<u64>(block.is_layered) << 2)
            | (static_cast<u64>(block.custom_properties != nullptr) << 3));
        auto const reads_custom_properties = block_reads_custom_properties(block.properties, block.custom_properties);
        // The engine collision-checks semantic declaration IDs before exposing them. Incomplete
        // inventories keep using their CSSOM identity, since their C++ declarations may differ.
        auto const use_semantic_source_identity = value_comparison == CascadeBlockKeyValueComparison::ByIdentity
            && block.source && block.semantic_declaration_id != 0;
        key.append(use_semantic_source_identity ? block.semantic_declaration_id : 0);
        key.append(block.source && !use_semantic_source_identity ? block.source->identity() : 0);
        key.append(block.source && !use_semantic_source_identity ? block.source->revision() : 0);
        auto const declares_animation_name = any_of(block.properties, [](auto const& property) { return property.property_id == PropertyID::AnimationName; });
        key.append(declares_animation_name ? bit_cast<FlatPtr>(block.source_shadow_root.ptr()) : 0);
        if (!block.source) {
            key.append(block.properties.size());
            for (auto const& property : block.properties) {
                key.append(to_underlying(property.property_id) | (static_cast<u64>(property.important == Important::Yes) << 32));
                if (value_comparison == CascadeBlockKeyValueComparison::ByIdentity)
                    key.append(bit_cast<FlatPtr>(property.value->rust_style_value_data()));
                pinned_values.append(property.value);
            }
        }
        return CascadeBlockKeyDependencies {
            .reads_custom_properties = reads_custom_properties,
            .declares_custom_properties = block_declares_custom_properties(block.custom_properties),
            .reads_style_scope = block_reads_style_scope(block.properties),
        };
    };

    key.append(NumericLimits<u64>::max());
    key.append(cascade_input.author_context_count);
    key.append(cascade_input.inline_style_context_index.value_or(NumericLimits<u32>::max()));
    key.append(cascade_input.contributions.size());

    CascadeBlockKeyDependencies dependencies;
    for (auto const& contribution : cascade_input.contributions) {
        auto const& declaration = *contribution.declaration;
        auto const* custom_properties = contribution.cascade_origin == CascadeOrigin::Author
            ? &declaration.custom_properties()
            : nullptr;
        auto block_dependencies = append_block({
            .properties = declaration.properties(),
            .custom_properties = custom_properties,
            .origin = contribution.cascade_origin,
            .author_context_index = contribution.author_context_index,
            .layer_index = contribution.layer_index,
            .is_layered = contribution.layer_name.has_value(),
            .source = contribution.declaration,
            .source_shadow_root = contribution.source_shadow_root,
            .semantic_declaration_id = contribution.semantic_declaration_id,
        });
        dependencies.reads_custom_properties |= block_dependencies.reads_custom_properties;
        dependencies.declares_custom_properties |= block_dependencies.declares_custom_properties;
        dependencies.reads_style_scope |= block_dependencies.reads_style_scope;
    }

    if (!presentational_hint_properties.is_empty()) {
        auto block_dependencies = append_block({
            .properties = presentational_hint_properties,
            .origin = CascadeOrigin::AuthorPresentationalHint,
        });
        dependencies.reads_custom_properties |= block_dependencies.reads_custom_properties;
        dependencies.declares_custom_properties |= block_dependencies.declares_custom_properties;
        dependencies.reads_style_scope |= block_dependencies.reads_style_scope;
    }

    if (inline_style && cascade_input.inline_style_context_index.has_value()) {
        auto block_dependencies = append_block({
            .properties = inline_style->properties(),
            .custom_properties = &inline_style->custom_properties(),
            .origin = CascadeOrigin::Author,
            .author_context_index = *cascade_input.inline_style_context_index,
            .is_inline_style = true,
            .bypass_pseudo_element_property_whitelist = true,
            .source = inline_style,
        });
        dependencies.reads_custom_properties |= block_dependencies.reads_custom_properties;
        dependencies.declares_custom_properties |= block_dependencies.declares_custom_properties;
        dependencies.reads_style_scope |= block_dependencies.reads_style_scope;
    }

    return dependencies;
}

// The cascade input, built from StyleEngine's matching rather than from the rule caches.
//
// The engine reports the rules that decide for the element already ordered by specificity and
// source order, and carries the cascade context it used. Nothing here sorts: each match is turned
// back into what its rule contributes, with its layer rank queried from that context's engine-owned
// topology.
RefPtr<StyleComputer::CascadeInput const> StyleComputer::style_engine_cascade_input(DOM::AbstractElement abstract_element, StyleEngineMatchResult* reusable_matches) const
{
    // Matching is a read that grows the engine's own scratch, so it is not const on the engine even
    // though it decides nothing about it.
    auto& style_engine = const_cast<StyleComputer&>(*this).style_engine();

    // What is being styled: the element itself, or one of its pseudo-elements. A pseudo-element is
    // decided by the rules that match its originating element and name it as their target, so both
    // are answered from the originating element's matches.
    Optional<u32> queried_pseudo_element;
    if (auto pseudo_element = abstract_element.pseudo_element(); pseudo_element.has_value())
        queried_pseudo_element = to_underlying(*pseudo_element);

    // An element with no identity is one the engine has never been told about, which is one that is
    // not in a tree: an identity is taken when an element connects, and mutating a disconnected tree
    // deliberately costs nothing. Nothing decides for such an element, which is the answer the other
    // engines give for one as well - its own declarations still apply, and those are not part of
    // this input.
    auto node = abstract_element.element().style_node_id();
    // A published signature names the current transaction answer and can reach a shared cascade
    // input without copying its matched-rule payload first. Outside a published transaction, the
    // last-ask signature is read below only after matching has made it current for this element.
    Optional<u32> match_signature;
    auto cache_key_for = [&](u32 signature) {
        auto target = queried_pseudo_element.has_value() ? *queried_pseudo_element + 1 : 0;
        return (static_cast<u64>(signature) << 32) | target;
    };
    if (node != 0) {
        match_signature = style_engine.published_match_answer_signature(node);
        if (match_signature.has_value()) {
            if (auto cached = m_style_engine_cascade_input_cache.get(cache_key_for(*match_signature)); cached.has_value())
                return *cached;
        }
    }

    Vector<StyleEngine::RuleMatch> local_matches;
    Vector<StyleEngine::RuleMatch> const* matches = &local_matches;
    auto read_matches = [&](Vector<StyleEngine::RuleMatch>& matches) {
        if (style_engine.consume_published_match_answer(node, matches))
            return true;
        return style_engine.match_element(node, matches, StyleEngine::MatchPurpose::Cascade);
    };
    if (node == 0) {
        // Nothing decides for an element that has no StyleEngine identity.
    } else if (reusable_matches && (reusable_matches->node == 0 || reusable_matches->node == node)) {
        if (reusable_matches->node == 0) {
            reusable_matches->node = node;
            Vector<StyleEngine::RuleMatch> matched_rules;
            if (read_matches(matched_rules)) {
                reusable_matches->matches = move(matched_rules);
                reusable_matches->signature = style_engine.match_element_signature(node);
            }
        }
        if (!reusable_matches->matches.has_value())
            return {};
        matches = &*reusable_matches->matches;
        if (!match_signature.has_value())
            match_signature = reusable_matches->signature;
    } else {
        if (!read_matches(local_matches))
            return {};
        if (!match_signature.has_value())
            match_signature = style_engine.match_element_signature(node);
        matches = &local_matches;
    }

    if (match_signature.has_value()) {
        if (auto cached = m_style_engine_cascade_input_cache.get(cache_key_for(*match_signature)); cached.has_value())
            return *cached;
    }

    auto context_shadow_roots = author_context_shadow_roots(abstract_element);

    auto input = adopt_ref(*new CascadeInput);
    bool input_is_cacheable = match_signature.has_value();
    input->author_context_count = static_cast<u32>(context_shadow_roots.size());
    GC::Ptr<DOM::ShadowRoot const> element_context_shadow_root = as_if<DOM::ShadowRoot>(abstract_element.element().root());
    for (u32 index = 0; index < context_shadow_roots.size(); ++index) {
        if (context_shadow_roots[index] == element_context_shadow_root)
            input->inline_style_context_index = index;
    }

    for (auto const& match : *matches) {
        Optional<u32> match_pseudo_element;
        if (match.pseudo_element != NumericLimits<u32>::max())
            match_pseudo_element = match.pseudo_element;

        // Only a rule aimed at what is being styled contributes to it.
        if (match_pseudo_element != queried_pseudo_element) {
            // A rule that decides for a pseudo-element of the element being styled says that element
            // has one to materialize. Only the synthetic ones are recorded, which is what the bit
            // means.
            if (!queried_pseudo_element.has_value() && match_pseudo_element.has_value()
                && *match_pseudo_element < to_underlying(PseudoElement::KnownPseudoElementCount)) {
                auto pseudo_element = static_cast<PseudoElement>(*match_pseudo_element);
                if (is_synthetic_pseudo_element(pseudo_element))
                    input->matching_pseudo_element_styles |= pseudo_element_style_bit(pseudo_element);
            }
            continue;
        }

        auto target = style_engine_rule_target(StyleEngineRuleID { match.rule });
        if (!target.has_value())
            return {};

        // A container query asks about the element being styled, so matching cannot answer it once
        // for the rule and the consumer applies it here. This is the same division the matcher uses.
        // A size or style query also records that this element's style depends on its container,
        // which is what bounds the scan that re-styles it when the container moves.
        if (target->container_rule) {
            input_is_cacheable = false;
            if (target->container_rule->contains_size_feature() || target->container_rule->contains_style_feature())
                target->container_rule->mark_element_style_dependencies(abstract_element);
            if (!target->container_rule->matches(abstract_element))
                continue;
        }

        // The user-agent and user origins have no encapsulation context of their own: they decide in
        // every scope and are weighed where they are attached, which is the document.
        u32 author_context_index = 0;
        u32 layer_index = 0;
        GC::Ptr<DOM::ShadowRoot const> context_shadow_root;
        if (target->cascade_origin == CascadeOrigin::Author) {
            if (match.scope_host != 0) {
                auto host = element_for_style_node(StyleNodeID { match.scope_host });
                if (!host)
                    return {};
                context_shadow_root = host->shadow_root();
            }
            auto found = context_shadow_roots.find_first_index(context_shadow_root);
            if (!found.has_value())
                return {};
            author_context_index = static_cast<u32>(*found);
            auto& context_style_scope = context_shadow_root ? context_shadow_root->style_scope() : document().style_scope();
            auto const layer = target->qualified_layer_name.is_empty() ? 0 : style_engine.intern_atom(target->qualified_layer_name);
            layer_index = style_engine.layer_index(context_style_scope.style_engine_tree_scope(), layer.value());
        }

        Optional<Utf16FlyString> layer_name;
        if (target->cascade_origin == CascadeOrigin::Author && !target->qualified_layer_name.is_empty())
            layer_name = target->qualified_layer_name;

        input->contributions.append({
            .declaration = &declaration_of_rule(*target->rule),
            .style_engine_rule_id = StyleEngineRuleID { match.rule },
            .semantic_declaration_id = match.semantic_declaration,
            .source_shadow_root = context_shadow_root,
            .layer_name = layer_name,
            .cascade_origin = target->cascade_origin,
            .author_context_index = author_context_index,
            .layer_index = layer_index,
        });
    }

    if (input_is_cacheable) {
        input->match_signature = match_signature;
        m_style_engine_cascade_input_cache.set(cache_key_for(*match_signature), input);
    }
    return input;
}

static Vector<StyleProperty> collect_presentational_hint_properties(DOM::AbstractElement abstract_element)
{
    Vector<StyleProperty> properties;
    if (abstract_element.pseudo_element().has_value())
        return properties;

    auto& element = abstract_element.element();
    element.apply_presentational_hints(properties);
    if (element.supports_dimension_attributes()) {
        auto const& dimension_source = is<HTML::HTMLImageElement>(element)
            ? static_cast<HTML::HTMLImageElement const&>(element).dimension_attribute_source()
            : element;
        collect_dimension_attribute(properties, dimension_source, HTML::AttributeNames::width, CSS::PropertyID::Width);
        collect_dimension_attribute(properties, dimension_source, HTML::AttributeNames::height, CSS::PropertyID::Height);
    }
    HashTable<CSS::PropertyID> seen_properties;
    for (size_t i = properties.size(); i > 0; --i) {
        if (seen_properties.set(properties[i - 1].property_id) != AK::HashSetResult::InsertedNewEntry)
            properties.remove(i - 1);
    }
    // Which properties a hint decides is a fact about the element, and this is the one place that
    // knows it: mapping the attributes needs the element fully built, and for a table cell it needs
    // the table's computed style, so it cannot be done when the element arrives.
    if (element.presentational_hint_properties_need_publication(properties)
        && record_element_presentational_hint_properties(element, properties))
        element.did_publish_presentational_hint_properties(properties);
    return properties;
}

// The same values resolved against the same environment are the same environment, so an element
// keeps the object it already has rather than taking an equal one. Custom property data chains by
// parent pointer, so a fresh object anywhere retires the identity of everything below it, and a
// child is then told that something it reads has moved when nothing has.
static RefPtr<CustomPropertyData const> custom_property_data_keeping_identity(DOM::Document const& document, RefPtr<CustomPropertyData const> existing, RefPtr<CustomPropertyData const> computed)
{
    if (!existing || !computed || existing == computed)
        return computed;
    if (existing->parent() != computed->parent())
        return computed;
    if (existing->own_values().size() != computed->own_values().size())
        return computed;
    // A custom property's value is a token stream, and what it computes to is what it serializes to.
    // Two streams that compare equal can still serialize differently - `if( style( --x : 3 ) : v )`
    // keeps the whitespace its result was written with - so the text is what decides here.
    for (auto const& [name, property] : existing->own_values()) {
        auto other = computed->own_values().get(name);
        if (!other.has_value() || other->important != property.important)
            return computed;
        if (other->value->rust_style_value_data() == property.value->rust_style_value_data())
            continue;
        // A registration decides what the name's value computes to, so two streams that read alike
        // are only alike while the name has none.
        if (document.get_registered_custom_property(name).has_value())
            return computed;
        // A value computed under an earlier registration can serialize like the raw tokens while
        // still being typed. Only values of the same kind are interchangeable by their text.
        if (other->value->type() != property.value->type())
            return computed;
        if (other->value->to_utf16_string(SerializationMode::Normal) != property.value->to_utf16_string(SerializationMode::Normal))
            return computed;
    }
    return existing;
}

// What decides an environment: the one it inherits from, and every name and value it declares. A
// value is named by its identity, since a cascade hands out the very value objects a declaration
// holds and two declarations of the same text share one.
static u64 hash_custom_property_data(CustomPropertyData const& data)
{
    Fnv1a64 hash;
    hash.add(bit_cast<FlatPtr>(data.parent().ptr()));
    for (auto const& [name, property] : data.own_values()) {
        hash.add(name.hash());
        hash.add(bit_cast<FlatPtr>(property.value->rust_style_value_data()));
        hash.add(property.important == Important::Yes ? 1 : 0);
    }
    return hash.value();
}

static bool custom_property_data_are_equal(CustomPropertyData const& a, CustomPropertyData const& b)
{
    if (a.parent() != b.parent() || a.own_values().size() != b.own_values().size())
        return false;
    auto a_iterator = a.own_values().begin();
    auto b_iterator = b.own_values().begin();
    for (; a_iterator != a.own_values().end(); ++a_iterator, ++b_iterator) {
        if (a_iterator->key != b_iterator->key)
            return false;
        if (a_iterator->value.important != b_iterator->value.important)
            return false;
        if (a_iterator->value.value->rust_style_value_data() != b_iterator->value.value->rust_style_value_data())
            return false;
    }
    return true;
}

NonnullRefPtr<CustomPropertyData const> StyleComputer::intern_custom_property_data(NonnullRefPtr<CustomPropertyData const> data) const
{
    auto& bucket = m_custom_property_environments.ensure(hash_custom_property_data(*data));
    for (auto const& existing : bucket) {
        if (custom_property_data_are_equal(*existing, *data))
            return existing;
    }
    bucket.append(data);
    return data;
}

// An environment nothing but the table holds is one no element is in, and the table is the only
// thing keeping it - and its parent chain - alive.
void StyleComputer::sweep_custom_property_environments() const
{
    // The memo of what a declaration list resolves to holds environments too, so it goes first or
    // nothing below it is ever the last reference.
    m_cascaded_custom_property_environments.clear();
    m_custom_property_reference_scans.clear();
    m_registered_custom_property_parses.clear();
    m_custom_property_environments.remove_all_matching([](auto&, Vector<NonnullRefPtr<CustomPropertyData const>>& bucket) {
        bucket.remove_all_matching([](auto const& data) { return data->ref_count() == 1; });
        return bucket.is_empty();
    });
    HashTable<u64> live_environment_identities;
    for (auto const& bucket : m_custom_property_environments) {
        for (auto const& data : bucket.value)
            live_environment_identities.set(data->identity());
    }
    m_parsed_substitutions.remove_all_matching([&](auto&, Vector<ParsedSubstitution>& bucket) {
        bucket.remove_all_matching([&](auto const& entry) {
            return entry.custom_property_environment_identity != 0
                && !live_environment_identities.contains(entry.custom_property_environment_identity);
        });
        return bucket.is_empty();
    });
    settle_parsed_substitution_cache();
}

u64 StyleComputer::parsed_substitution_cache_bytes() const
{
    u64 bytes = m_parsed_substitutions.capacity() * (sizeof(StyleEngineRuleID) + sizeof(Vector<ParsedSubstitution>));
    for (auto const& bucket : m_parsed_substitutions)
        bytes += bucket.value.capacity() * sizeof(ParsedSubstitution);
    return bytes;
}

void StyleComputer::settle_parsed_substitution_cache() const
{
    if (m_style_engine.resize_parsed_substitution_cache(parsed_substitution_cache_bytes()))
        return;
    m_parsed_substitutions.clear();
    VERIFY(m_style_engine.resize_parsed_substitution_cache(0));
}

void StyleComputer::invalidate_parsed_substitutions_for_rule(StyleEngineRuleID rule_id) const
{
    m_parsed_substitutions.remove(rule_id);
    settle_parsed_substitution_cache();
}

NonnullRefPtr<CascadedProperties> StyleComputer::compute_cascaded_values(DOM::AbstractElement abstract_element, CascadeInput const& cascade_input, IncludeInlineStyle include_inline_style, StyleSharingCandidate* sharing, Vector<StyleProperty> const* precomputed_presentational_hints) const
{
    auto cascaded_properties = CascadedProperties::create();

    // The whole css-cascade-5 origin sequence runs in the Rust style computation core over
    // one bulk description of the matched declaration blocks. Declarations, layer names and
    // sources are collected and pinned here; the core derives the application order from
    // the origin and the author context and layer indices.
    struct BlockSource {
        GC::Ptr<CSSStyleDeclaration const> source;
        GC::Ptr<DOM::ShadowRoot const> source_shadow_root;
    };
    Vector<ComputedValuesFFI::FfiCascadeDeclaration> all_declarations;
    Vector<ComputedValuesFFI::FfiCustomPropertyDeclaration> all_custom_property_declarations;
    bool has_unresolved_declarations = false;
    bool has_custom_function_declarations = false;
    struct PendingBlock {
        ComputedValuesFFI::FfiCascadeBlock block;
        size_t declarations_offset { 0 };
        size_t custom_property_declarations_offset { 0 };
    };
    Vector<PendingBlock> pending_blocks;
    Vector<BlockSource> block_sources;
    Vector<FlatPtr> leaked_custom_property_names;

    auto add_block = [&](ReadonlySpan<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> const* custom_properties, CascadeOrigin origin, u32 author_context_index, u32 layer_index, bool is_inline_style, bool bypass_pseudo_element_property_whitelist, Optional<Utf16FlyString> const& layer_name, GC::Ptr<CSSStyleDeclaration const> source, GC::Ptr<DOM::ShadowRoot const> source_shadow_root, StyleEngineRuleID style_engine_rule_id = {}) {
        auto declarations_offset = all_declarations.size();
        all_declarations.ensure_capacity(all_declarations.size() + properties.size());
        for (auto const& property : properties) {
            has_unresolved_declarations |= property.value->is_unresolved();
            has_custom_function_declarations |= property.value->is_unresolved() && property.value->as_unresolved().includes_dashed_function();
            all_declarations.unchecked_append({
                .property_id = to_underlying(property.property_id),
                .important = property.important == Important::Yes,
                .has_style_sheet_context = property.value->has_style_sheet_context(),
                .data = property.value->rust_style_value_data(),
            });
        }
        auto custom_property_declarations_offset = all_custom_property_declarations.size();
        if (custom_properties) {
            all_custom_property_declarations.ensure_capacity(all_custom_property_declarations.size() + custom_properties->size());
            for (auto const& [name, property] : *custom_properties) {
                auto name_raw = name.to_raw_leaked();
                leaked_custom_property_names.append(name_raw);
                all_custom_property_declarations.unchecked_append({
                    .name_raw = name_raw,
                    .important = property.important == Important::Yes,
                    .is_revert_layer = property.value->is_revert_layer(),
                    .data = property.value->rust_style_value_data(),
                });
            }
        }
        FlatPtr layer_name_raw = 0;
        if (layer_name.has_value()) {
            layer_name_raw = layer_name->to_raw_leaked();
        }
        pending_blocks.append({
            .block = {
                .origin = static_cast<ComputedValuesFFI::CascadeOrigin>(to_underlying(origin)),
                .author_context_index = author_context_index,
                .layer_index = layer_index,
                .is_inline_style = is_inline_style,
                .bypass_pseudo_element_property_whitelist = bypass_pseudo_element_property_whitelist,
                .has_layer_name = layer_name.has_value(),
                .layer_name_raw = layer_name_raw,
                .source_shadow_root_identity = bit_cast<FlatPtr>(source_shadow_root.ptr()),
                .source_id = static_cast<u32>(block_sources.size()),
                .style_engine_rule_id = style_engine_rule_id.value(),
                .declarations = nullptr,
                .declaration_count = properties.size(),
                .custom_property_declarations = nullptr,
                .custom_property_declaration_count = custom_properties ? custom_properties->size() : 0,
            },
            .declarations_offset = declarations_offset,
            .custom_property_declarations_offset = custom_property_declarations_offset,
        });
        block_sources.append({ source, source_shadow_root });
    };

    // The user-agent and user origins contribute no custom properties, which is why only an author
    // block carries them.
    for (auto const& contribution : cascade_input.contributions) {
        auto const& declaration = *contribution.declaration;
        auto const* custom_properties = contribution.cascade_origin == CascadeOrigin::Author ? &declaration.custom_properties() : nullptr;
        add_block(declaration.properties(), custom_properties, contribution.cascade_origin, contribution.author_context_index, contribution.layer_index, false, false, contribution.layer_name, &declaration, contribution.source_shadow_root, contribution.style_engine_rule_id);
    }

    // Author presentational hints
    // The spec calls this a special "Author presentational hint origin":
    // "For the purpose of cascading this author presentational hint origin is treated as an independent origin;
    // however for the purpose of the revert keyword (but not for the revert-layer keyword) it is considered
    // part of the author origin."
    // https://drafts.csswg.org/css-cascade-5/#author-presentational-hint-origin
    auto local_presentational_hint_properties = precomputed_presentational_hints
        ? Vector<StyleProperty> {}
        : collect_presentational_hint_properties(abstract_element);
    auto const& presentational_hint_properties = precomputed_presentational_hints
        ? *precomputed_presentational_hints
        : local_presentational_hint_properties;
    auto const inline_style = include_inline_style == IncludeInlineStyle::Yes && cascade_input.inline_style_context_index.has_value()
        ? abstract_element.inline_style()
        : GC::Ptr<CSSStyleProperties const> {};

    if (sharing) {
        auto dependencies = append_cascade_blocks_to_key(sharing->key, sharing->pinned_key_values, cascade_input, presentational_hint_properties, inline_style, CascadeBlockKeyValueComparison::ByIdentity);
        sharing->cascade_reads_custom_properties = dependencies.reads_custom_properties;
    }

    if (!presentational_hint_properties.is_empty())
        add_block(presentational_hint_properties, nullptr, CascadeOrigin::AuthorPresentationalHint, 0, 0, false, false, {}, nullptr, nullptr);

    if (inline_style) {
        // NB: Inline style bypasses the pseudo-element property whitelist since inline style is used
        //     internally to style element-reference pseudo-elements and sometimes contains disallowed
        //     properties (e.g. input::placeholder has height set); authors can't set inline style on
        //     pseudo-elements so this doesn't cause any spec compliance issues.
        add_block(inline_style->properties(), &inline_style->custom_properties(), CascadeOrigin::Author, *cascade_input.inline_style_context_index, 0, true, true, {}, inline_style, nullptr);
    }

    Vector<ComputedValuesFFI::FfiCascadeBlock> blocks;
    blocks.ensure_capacity(pending_blocks.size());
    for (auto& pending : pending_blocks) {
        pending.block.declarations = all_declarations.data() + pending.declarations_offset;
        if (pending.block.custom_property_declaration_count > 0)
            pending.block.custom_property_declarations = all_custom_property_declarations.data() + pending.custom_property_declarations_offset;
        blocks.unchecked_append(pending.block);
    }

    struct BulkCascadeContext {
        CascadedProperties& cascaded_properties;
        DOM::AbstractElement& abstract_element;
        Vector<BlockSource> const& block_sources;
        u64 custom_property_environment_identity { 0 };
        void const* inheritance_custom_property_store { nullptr };
    } bulk_context {
        .cascaded_properties = *cascaded_properties,
        .abstract_element = abstract_element,
        .block_sources = block_sources,
    };

    // The cascade only reads this value's data pointer, so mint a bare Rust handle instead of a wrapper.
    RustStyleValueHandle const unset_value { StyleValueFFI::rust_style_value_create_keyword(to_underlying(Keyword::Unset)) };

    auto install_custom_properties = [&](ComputedValuesFFI::FfiCascadedCustomProperty const* properties, size_t count) -> void const* {
        auto& document = bulk_context.abstract_element.element().document();
        auto& style_computer = document.style_computer();

        RefPtr<CustomPropertyData const> parent_data;
        auto inherit_from = bulk_context.abstract_element.element_to_inherit_style_from();
        if (inherit_from.has_value()) {
            parent_data = inheritable_custom_property_data(*inherit_from);
            auto inheritance_data = inherit_from->custom_property_data();
            bulk_context.inheritance_custom_property_store = inheritance_data ? inheritance_data->rust_store() : nullptr;
        }

        // OPTIMIZATION: The declarations below name the whole answer, together with what the
        //               element inherits and which names are registered, so an element handed the
        //               same list against the same environment gets the same one back.
        auto& key = style_computer.m_cascaded_custom_property_key_scratch;
        key.clear_with_capacity();
        key.append(bit_cast<FlatPtr>(parent_data.ptr()));
        key.append(document.custom_property_registration_generation());
        for (size_t i = 0; i < count; ++i) {
            key.append(properties[i].name_raw);
            key.append(bit_cast<FlatPtr>(properties[i].data));
            key.append(properties[i].important ? 1 : 0);
        }
        Fnv1a64 key_hash;
        for (auto word : key)
            key_hash.add(word);
        auto apply_environment = [&](RefPtr<CustomPropertyData const> const& result) -> void const* {
            if (!result || result == parent_data) {
                bulk_context.abstract_element.set_custom_property_data(result);
            } else {
                bulk_context.abstract_element.set_custom_property_data(custom_property_data_keeping_identity(
                    document, bulk_context.abstract_element.custom_property_data(), result));
            }
            auto custom_property_data = bulk_context.abstract_element.custom_property_data();
            bulk_context.custom_property_environment_identity = custom_property_data ? custom_property_data->identity() : 0;
            return custom_property_data ? custom_property_data->rust_store() : nullptr;
        };
        auto& memo_bucket = style_computer.m_cascaded_custom_property_environments.ensure(key_hash.value());
        for (auto const& entry : memo_bucket) {
            if (entry.key == key)
                return apply_environment(entry.result);
        }

        OrderedHashMap<Utf16FlyString, StyleProperty> cascaded_all;
        cascaded_all.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i) {
            auto const& property = properties[i];
            auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                static_cast<StyleValueFFI::StyleValueData const*>(property.data)));
            cascaded_all.set(
                Utf16FlyString::from_raw(property.name_raw),
                StyleProperty {
                    .important = property.important ? Important::Yes : Important::No,
                    .property_id = PropertyID::Custom,
                    .value = move(value),
                });
        }

        OrderedHashMap<Utf16FlyString, StyleProperty> cascaded_own;
        for (auto& [name, property] : cascaded_all) {
            if (parent_data) {
                auto const* parent_property = parent_data->get(name);
                if (parent_property && parent_property->value->rust_style_value_data() == property.value->rust_style_value_data())
                    continue;
            }
            cascaded_own.set(name, move(property));
        }

        RefPtr<CustomPropertyData const> resolved;
        if (cascaded_own.is_empty())
            resolved = parent_data;
        else
            resolved = style_computer.intern_custom_property_data(CustomPropertyData::create(move(cascaded_own), parent_data));
        style_computer.m_cascaded_custom_property_environments.ensure(key_hash.value()).append({ key, parent_data, resolved });
        return apply_environment(resolved);
    };

    auto cascaded_custom_properties = ComputedValuesFFI::rust_cascade_custom_properties(
        blocks.data(),
        blocks.size(),
        cascade_input.author_context_count,
        pseudo_element_to_ffi(abstract_element.pseudo_element()));
    ScopeGuard destroy_cascaded_custom_properties = [&] {
        ComputedValuesFFI::rust_cascaded_custom_properties_destroy(
            cascaded_custom_properties.storage, cascaded_custom_properties.count);
    };
    void const* custom_property_store = nullptr;
    if (cascaded_custom_properties.applies)
        custom_property_store = install_custom_properties(cascaded_custom_properties.properties, cascaded_custom_properties.count);

    auto& document = bulk_context.abstract_element.document();
    auto& style_computer = document.style_computer();
    if (has_unresolved_declarations) {
        auto registration_generation = document.custom_property_registration_generation();
        if (registration_generation != style_computer.m_parsed_substitution_registration_generation) {
            style_computer.m_parsed_substitutions.clear();
            style_computer.m_parsed_substitution_registration_generation = registration_generation;
            style_computer.settle_parsed_substitution_cache();
        }
    }
    SubstitutionData substitution_data { abstract_element, has_unresolved_declarations, has_custom_function_declarations };
    ComputedValuesFFI::FfiCascadeResolutionContext resolution_context {
        .parse_context = &substitution_data.parse_context,
        .custom_property_store = custom_property_store,
        .inheritance_custom_property_store = bulk_context.inheritance_custom_property_store,
        .custom_property_registry = document.rust_custom_property_registry(),
        .root_custom_property_name = {},
        .attributes = substitution_data.ffi_attributes.data(),
        .attribute_count = substitution_data.ffi_attributes.size(),
        .attribute_names_are_ascii_case_insensitive = abstract_element.element().namespace_uri() == Namespace::HTML && document.is_html_document(),
        .custom_functions = substitution_data.ffi_functions.data(),
        .custom_function_count = substitution_data.ffi_functions.size(),
        .custom_function_scope_identity = bit_cast<FlatPtr>(&abstract_element.style_scope()),
        .callback_context = &bulk_context,
        .resolve_custom_function = resolve_custom_function_for_substitution,
        .evaluate_condition = [](void* context, u8 kind, ComputedValuesFFI::FfiUtf16View source) -> u8 {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            return evaluate_condition_for_substitution(bulk_context.abstract_element, kind, source);
        },
        .lookup_final_custom_property = [](void* context, ComputedValuesFFI::FfiUtf16View name) -> void const* {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            auto& document = bulk_context.abstract_element.document();
            auto& style_computer = document.style_computer();
            auto name_view = utf16_view(name);
            if (style_computer.m_active_custom_property_resolution.has_value()
                && style_computer.m_active_custom_property_resolution->element == bulk_context.abstract_element) {
                if (auto finalized = style_computer.m_active_custom_property_resolution->finalized.get(Utf16FlyString::from_utf16(name_view)); finalized.has_value()) {
                    document.style_invalidation_counters().custom_property_overlay_hits++;
                    return finalized.value()->rust_style_value_data();
                }
                document.style_invalidation_counters().custom_property_value_computations++;
            }
            return nullptr;
        },
        .note_substitution = [](void* context, void const* unresolved_data) {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            auto unresolved = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                static_cast<StyleValueFFI::StyleValueData const*>(unresolved_data)));
            if (unresolved->as_unresolved().includes_var_function())
                bulk_context.abstract_element.element().set_style_uses_var_css_function();
            if (unresolved->as_unresolved().includes_attr_function())
                bulk_context.abstract_element.element().set_style_uses_attr_css_function();
            if (unresolved->as_unresolved().includes_if_function())
                bulk_context.abstract_element.element().set_style_uses_if_css_function();
            if (unresolved->as_unresolved().includes_inherit_function())
                bulk_context.abstract_element.element().set_style_uses_inherit_css_function();
            if (unresolved->as_unresolved().includes_dashed_function())
                bulk_context.abstract_element.element().set_style_uses_custom_function();
            auto const& scan = bulk_context.abstract_element.document().style_computer().custom_property_reference_scan(unresolved);
            for (auto const& name : scan.references)
                bulk_context.abstract_element.element().record_style_custom_property_reference(name); },
        .lookup_cached_substitution = [](void* context, u32 style_engine_rule_id, u16 property_id) -> void const* {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            auto& document = bulk_context.abstract_element.document();
            auto& style_computer = document.style_computer();
            auto cache = style_computer.m_parsed_substitutions.get(StyleEngineRuleID { style_engine_rule_id });
            if (!cache.has_value())
                return nullptr;
            for (auto const& entry : *cache) {
                if (entry.custom_property_environment_identity == bulk_context.custom_property_environment_identity
                    && entry.registration_generation == document.custom_property_registration_generation()
                    && entry.property_id == static_cast<PropertyID>(property_id))
                    return entry.parsed->rust_style_value_data();
            }
            return nullptr;
        },
        .cache_parsed_substitution = [](void* context, u32 style_engine_rule_id, u16 property_id, void const* data) {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            auto& document = bulk_context.abstract_element.document();
            ++document.style_invalidation_counters().substitution_value_parses;
            if (style_engine_rule_id == 0)
                return;
            auto& style_computer = document.style_computer();
            auto parsed = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                static_cast<StyleValueFFI::StyleValueData const*>(data)));
            style_computer.m_parsed_substitutions.ensure(StyleEngineRuleID { style_engine_rule_id }).append({
                .custom_property_environment_identity = bulk_context.custom_property_environment_identity,
                .registration_generation = document.custom_property_registration_generation(),
                .property_id = static_cast<PropertyID>(property_id),
                .parsed = move(parsed),
            });
            style_computer.settle_parsed_substitution_cache(); },
    };

    auto assign_source_slots = [&](ComputedValuesFFI::FfiSourceSlotAssignment const* assignments, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            auto const& source = bulk_context.block_sources[assignments[i].source_id];
            bulk_context.cascaded_properties.assign_source_slot(assignments[i].slot, source.source, source.source_shadow_root);
        }
    };

    if (auto node = abstract_element.element().style_node_id(); node != 0) {
        auto& style_engine = const_cast<StyleComputer&>(*this).style_engine();
        auto retained_assignments = style_engine.materialize_retained_cascade_state(
            node,
            pseudo_element_to_ffi(abstract_element.pseudo_element()),
            cascaded_properties->rust_store(),
            blocks);
        if (!retained_assignments.is_empty())
            assign_source_slots(retained_assignments.data(), retained_assignments.size());
        style_engine.discard_retained_cascade_assignments();
    }

    auto cascade_result = ComputedValuesFFI::rust_cascade_matched_blocks(
        cascaded_properties->rust_store(),
        blocks.data(),
        blocks.size(),
        cascade_input.author_context_count,
        pseudo_element_to_ffi(abstract_element.pseudo_element()),
        unset_value.data(),
        has_unresolved_declarations ? &resolution_context : nullptr);
    ScopeGuard destroy_cascade_result = [&] {
        ComputedValuesFFI::rust_cascade_result_destroy(cascade_result.storage, cascade_result.source_slot_assignment_count);
    };
    assign_source_slots(cascade_result.source_slot_assignments, cascade_result.source_slot_assignment_count);

    for (auto custom_property_name_raw : leaked_custom_property_names)
        Utf16FlyString::unref_raw(custom_property_name_raw);

    // Transition declarations [css-transitions-1]
    // Note that we have to do these after finishing computing the style,
    // so they're not done here, but as the final step in compute_properties()

    return cascaded_properties;
}

NonnullRefPtr<StyleValue const> StyleComputer::get_non_animated_inherit_value(PropertyID property_id, DOM::AbstractElement abstract_element)
{
    auto parent_element = abstract_element.element_to_inherit_style_from();

    if (!parent_element.has_value() || !parent_element->has_style())
        return property_initial_value(property_id);

    auto parent_style = parent_element->computed_style();
    auto value = parent_style->computed_style_value_for_inheritance(property_id, ComputedValues::WithAnimationsApplied::No);
    VERIFY(value);

    return value.release_nonnull();
}

Optional<StyleComputer::AnimatedInheritValue> StyleComputer::get_animated_inherit_value(PropertyID property_id, DOM::AbstractElement abstract_element)
{
    auto parent_element = abstract_element.element_to_inherit_style_from();

    if (!parent_element.has_value() || !parent_element->has_style())
        return {};

    auto parent_style = parent_element->computed_style();
    auto const* animated_properties = parent_style->animated_properties();
    if (!animated_properties || !animated_properties->has_property(property_id))
        return {};

    if (auto animated_value = animated_properties->values().get(property_id); animated_value.has_value()) {
        return AnimatedInheritValue {
            .value = *animated_value.value(),
            .is_result_of_transition = animated_properties->is_property_result_of_transition(property_id)
                ? AnimatedPropertyResultOfTransition::Yes
                : AnimatedPropertyResultOfTransition::No
        };
    }

    return {};
}

Length::FontMetrics StyleComputer::calculate_root_element_font_metrics(ComputedStyleWorkingSet const& style) const
{
    auto const& root_value = style.property(CSS::PropertyID::FontSize);

    auto font_pixel_metrics = style.first_available_computed_font(document().font_computer())->pixel_metrics();
    Length::FontMetrics font_metrics { m_default_font_metrics.font_size, font_pixel_metrics, InitialValues::line_height() };
    font_metrics.font_size = root_value.as_length().length().to_px(viewport_rect(), font_metrics, font_metrics);
    font_metrics.line_height = style.line_height(document().font_computer());

    return font_metrics;
}

CSSPixels StyleComputer::default_user_font_size()
{
    // FIXME: This value should be configurable by the user.
    return 16;
}

// https://w3c.github.io/csswg-drafts/css-fonts/#absolute-size-mapping
CSSPixels StyleComputer::absolute_size_mapping(AbsoluteSize absolute_size, CSSPixels default_font_size)
{
    // An <absolute-size> keyword refers to an entry in a table of font sizes computed and kept by the user agent. See
    // § 2.5.1 Absolute Size Keyword Mapping Table.
    switch (absolute_size) {
    case AbsoluteSize::XxSmall:
        return default_font_size * CSSPixels(3) / 5;
    case AbsoluteSize::XSmall:
        return default_font_size * CSSPixels(3) / 4;
    case AbsoluteSize::Small:
        return default_font_size * CSSPixels(8) / 9;
    case AbsoluteSize::Medium:
        return default_font_size;
    case AbsoluteSize::Large:
        return default_font_size * CSSPixels(6) / 5;
    case AbsoluteSize::XLarge:
        return default_font_size * CSSPixels(3) / 2;
    case AbsoluteSize::XxLarge:
        return default_font_size * 2;
    case AbsoluteSize::XxxLarge:
        return default_font_size * 3;
    }

    VERIFY_NOT_REACHED();
}

void StyleComputer::compute_property_values(ComputedStyleWorkingSet& style, Optional<DOM::AbstractElement> abstract_element) const
{
    VERIFY(computation_context_cache_is_empty());
    // NOTE: This doesn't necessarily return the specified value if we have already computed this property but that
    //       doesn't matter as a computed value is always valid as a specified value.
    Function<NonnullRefPtr<StyleValue const>(PropertyID)> const get_property_specified_value = [&](auto property_id) -> NonnullRefPtr<StyleValue const> {
        return style.property(property_id);
    };

    auto device_pixels_per_css_pixel = m_document->page().client().device_pixels_per_css_pixel();
    for (auto const& property_id : property_computation_order()) {
        auto const& computation_context = get_computation_context_for_property(property_id, style, abstract_element);

        auto const& specified_value = style.property(property_id, ComputedStyleWorkingSet::WithAnimationsApplied::No);

        computation_context.reset_viewport_metric_dependency_tracking();
        auto const& computed_value = compute_value_of_property(property_id, specified_value, get_property_specified_value, computation_context, device_pixels_per_css_pixel);
        if (computation_context.depends_on_viewport_metrics()) {
            style.set_depends_on_viewport_metrics();
            if (property_affects_font_metrics(property_id))
                style.set_font_metrics_depend_on_viewport_metrics();
        }

        style.set_property_without_modifying_flags(property_id, computed_value);
    }

    clear_computation_context_caches();

    if (abstract_element.has_value() && is<HTML::HTMLHtmlElement>(abstract_element->element())) {
        m_root_element_font_metrics = calculate_root_element_font_metrics(style);
        m_root_element_font_metrics_depend_on_viewport_metrics = style.font_metrics_depend_on_viewport_metrics();
    }
}

ComputationContext StyleComputer::make_computation_context_for_property(PropertyID property_id, ComputedStyleWorkingSet const& style, Optional<DOM::AbstractElement> abstract_element) const
{
    auto subject_inline_axis_is_horizontal = [&]() {
        if (!abstract_element.has_value())
            return true;
        if (auto computed_values = abstract_element->computed_style(); computed_values)
            return computed_values->writing_mode() == WritingMode::HorizontalTb;
        if (auto inheritance_parent = abstract_element->element_to_inherit_style_from(); inheritance_parent.has_value() && inheritance_parent->has_style())
            return inheritance_parent->computed_style()->writing_mode() == WritingMode::HorizontalTb;
        return true;
    }();

    switch (property_id) {
    // FIXME: While `color-scheme` doesn't actually require a computation context (since it only takes keyword values)
    //        we still try to generate one in `compute_property_values()` and since we need `color-scheme` to be
    //        computed before creating a generic computation context we use the font one instead.
    case PropertyID::ColorScheme:
    case PropertyID::FontFamily:
    case PropertyID::FontFeatureSettings:
    case PropertyID::FontKerning:
    case PropertyID::FontOpticalSizing:
    case PropertyID::FontSize:
    case PropertyID::FontStyle:
    case PropertyID::FontVariantAlternates:
    case PropertyID::FontVariantCaps:
    case PropertyID::FontVariantEastAsian:
    case PropertyID::FontVariantEmoji:
    case PropertyID::FontVariantLigatures:
    case PropertyID::FontVariantNumeric:
    case PropertyID::FontVariantPosition:
    case PropertyID::FontVariationSettings:
    case PropertyID::FontWeight:
    case PropertyID::FontWidth:
    case PropertyID::MathDepth:
    case PropertyID::TextRendering: {
        auto inheritance_parent = abstract_element.map([](auto& element) { return element.element_to_inherit_style_from(); }).value_or(OptionalNone {});
        auto length_resolution_context = inheritance_parent.has_value() && inheritance_parent->has_style() && inheritance_parent->element().navigable()
            ? Length::ResolutionContext::for_element(inheritance_parent.value())
            : Length::ResolutionContext::for_document(m_document);
        length_resolution_context.subject_inline_axis_is_horizontal = subject_inline_axis_is_horizontal;
        length_resolution_context.subject_element = abstract_element.has_value() ? &abstract_element->element() : nullptr;

        return {
            .length_resolution_context = length_resolution_context,
            .abstract_element = abstract_element
        };
    }
    case PropertyID::LineHeight: {
        auto inheritance_parent = abstract_element.map([](auto& element) { return element.element_to_inherit_style_from(); }).value_or(OptionalNone {});

        auto line_height_font_metrics = Length::FontMetrics {
            style.font_size(),
            style.first_available_computed_font(document().font_computer())->pixel_metrics(),
            inheritance_parent.has_value() && inheritance_parent->has_style() ? inheritance_parent->computed_style()->line_height() : InitialValues::line_height()
        };

        return {
            .length_resolution_context = {
                .viewport_rect = viewport_rect(),
                .font_metrics = line_height_font_metrics,
                .root_font_metrics = abstract_element.has_value() && abstract_element->element().is_html_html_element()
                    ? line_height_font_metrics
                    : m_root_element_font_metrics,
                .font_metrics_depend_on_viewport_metrics = style.font_metrics_depend_on_viewport_metrics(),
                .root_font_metrics_depend_on_viewport_metrics = abstract_element.has_value() && abstract_element->element().is_html_html_element()
                    ? style.font_metrics_depend_on_viewport_metrics()
                    : m_root_element_font_metrics_depend_on_viewport_metrics,
                .subject_inline_axis_is_horizontal = subject_inline_axis_is_horizontal,
                .subject_element = abstract_element.has_value() ? &abstract_element->element() : nullptr,
            },
            .abstract_element = abstract_element
        };
    }
    default: {
        return {
            .length_resolution_context = {
                .viewport_rect = viewport_rect(),
                .font_metrics = {
                    style.font_size(),
                    style.first_available_computed_font(document().font_computer())->pixel_metrics(),
                    style.line_height(document().font_computer()) },
                .root_font_metrics = m_root_element_font_metrics,
                .font_metrics_depend_on_viewport_metrics = style.font_metrics_depend_on_viewport_metrics(),
                .root_font_metrics_depend_on_viewport_metrics = abstract_element.has_value() && abstract_element->element().is_html_html_element() ? style.font_metrics_depend_on_viewport_metrics() : m_root_element_font_metrics_depend_on_viewport_metrics,
                .subject_inline_axis_is_horizontal = subject_inline_axis_is_horizontal,
                .subject_element = abstract_element.has_value() ? &abstract_element->element() : nullptr,
            },
            .abstract_element = abstract_element,
            .color_scheme = style.color_scheme(document().page().preferred_color_scheme(), document().supported_color_schemes())
        };
    }
    }

    VERIFY_NOT_REACHED();
}

ComputationContext const& StyleComputer::get_computation_context_for_property(PropertyID property_id, ComputedStyleWorkingSet const& style, Optional<DOM::AbstractElement> abstract_element) const
{
    switch (property_id) {
    case PropertyID::ColorScheme:
    case PropertyID::FontFamily:
    case PropertyID::FontFeatureSettings:
    case PropertyID::FontKerning:
    case PropertyID::FontOpticalSizing:
    case PropertyID::FontSize:
    case PropertyID::FontStyle:
    case PropertyID::FontVariantAlternates:
    case PropertyID::FontVariantCaps:
    case PropertyID::FontVariantEastAsian:
    case PropertyID::FontVariantEmoji:
    case PropertyID::FontVariantLigatures:
    case PropertyID::FontVariantNumeric:
    case PropertyID::FontVariantPosition:
    case PropertyID::FontVariationSettings:
    case PropertyID::FontWeight:
    case PropertyID::FontWidth:
    case PropertyID::MathDepth:
    case PropertyID::TextRendering:
        if (!m_cached_font_computation_context.has_value())
            m_cached_font_computation_context = make_computation_context_for_property(property_id, style, abstract_element);
        return m_cached_font_computation_context.value();
    case PropertyID::LineHeight:
        if (!m_cached_line_height_computation_context.has_value())
            m_cached_line_height_computation_context = make_computation_context_for_property(property_id, style, abstract_element);
        return m_cached_line_height_computation_context.value();
    default:
        if (!m_cached_generic_computation_context.has_value())
            m_cached_generic_computation_context = make_computation_context_for_property(property_id, style, abstract_element);
        return m_cached_generic_computation_context.value();
    }
}

void StyleComputer::resolve_effective_overflow_values(ComputedStyleWorkingSet& style) const
{
    // The css-overflow-3 rule pairing the two axes lives in the Rust style computation core.
    auto effective_overflow = ComputedValuesFFI::rust_resolve_effective_overflow_keywords(
        to_underlying(style.property(PropertyID::OverflowX).to_keyword()),
        to_underlying(style.property(PropertyID::OverflowY).to_keyword()));
    if (effective_overflow.changed_x)
        style.set_property(PropertyID::OverflowX, KeywordStyleValue::create(static_cast<Keyword>(effective_overflow.x_keyword)));
    if (effective_overflow.changed_y)
        style.set_property(PropertyID::OverflowY, KeywordStyleValue::create(static_cast<Keyword>(effective_overflow.y_keyword)));
}

static void compute_text_align(ComputedStyleWorkingSet& style, DOM::AbstractElement abstract_element)
{
    auto text_align_keyword = style.property(PropertyID::TextAlign).to_keyword();

    // NB: Only these two keywords trigger an adjustment in the Rust decision below; the early
    //     return avoids fetching the parent's computed values for every element.
    if (text_align_keyword != Keyword::MatchParent && text_align_keyword != Keyword::LibwebInheritOrCenter)
        return;

    // The resolved value does not remember that it read the parent's direction and text alignment.
    // Keep the specified keyword so inherited-style refresh can resolve it again when either moves.
    style.add_inheritance_dependent_specified_value(PropertyID::TextAlign, style.property(PropertyID::TextAlign));

    auto const parent = abstract_element.element_to_inherit_style_from();
    bool has_parent_with_computed_values = parent.has_value() && parent->has_style();
    u16 parent_text_align = 0;
    bool parent_direction_is_ltr = true;
    if (has_parent_with_computed_values) {
        auto parent_style = parent->computed_style();
        auto const& parent_values = *parent_style;
        parent_text_align = to_underlying(to_keyword(parent_values.text_align()));
        parent_direction_is_ltr = parent_values.direction() == Direction::Ltr;
    }

    // The adjustment decision lives in the Rust style computation core.
    auto adjustment = ComputedValuesFFI::rust_compute_text_align(
        to_underlying(text_align_keyword),
        abstract_element.element().local_name() == HTML::TagNames::th,
        has_parent_with_computed_values,
        parent_text_align,
        parent_direction_is_ltr);
    if (adjustment.changed) {
        style.set_property(PropertyID::TextAlign, KeywordStyleValue::create(static_cast<Keyword>(adjustment.keyword)),
            adjustment.inherited ? ComputedStyleWorkingSet::Inherited::Yes : ComputedStyleWorkingSet::Inherited::No);
    }
}

static ComputedValuesFFI::FfiBoxTypeTransformationInput make_box_type_transformation_input(
    DOM::AbstractElement abstract_element, Display display, Keyword position, Keyword float_value,
    Optional<Display> known_parent_display = {})
{
    auto& element = abstract_element.element();

    // NOTE: If we're computing style for a pseudo-element, the effective parent will be the originating element itself, not its parent.
    auto parent = abstract_element.element_to_inherit_style_from();

    // Climb out of `display: contents` context.
    Optional<Display> parent_display;
    while (parent.has_value() && parent->has_style()) {
        auto display = [&] {
            if (known_parent_display.has_value())
                return *known_parent_display;
            return parent->computed_style()->display();
        }();
        known_parent_display.clear();
        if (!display.is_contents()) {
            parent_display = display;
            break;
        }
        parent = parent->element_to_inherit_style_from();
    }

    bool has_parent_display = parent_display.has_value();
    bool is_html_element = element.namespace_uri() == Namespace::HTML;
    bool should_adjust_element = !abstract_element.pseudo_element().has_value();
    auto local_name = element.local_name();

    bool input_allows_adjustment = false;
    bool input_is_single_line = false;
    if (is<HTML::HTMLInputElement>(element)) {
        auto const& input = static_cast<HTML::HTMLInputElement const&>(element);
        input_allows_adjustment = !first_is_one_of(
            input.type_state(),
            HTML::HTMLInputElement::TypeAttributeState::Hidden,
            HTML::HTMLInputElement::TypeAttributeState::SubmitButton,
            HTML::HTMLInputElement::TypeAttributeState::Button,
            HTML::HTMLInputElement::TypeAttributeState::ResetButton,
            HTML::HTMLInputElement::TypeAttributeState::ImageButton,
            HTML::HTMLInputElement::TypeAttributeState::Checkbox,
            HTML::HTMLInputElement::TypeAttributeState::RadioButton);
        input_is_single_line = input_allows_adjustment && input.is_single_line();
    }

    bool force_position_static = false;
    if (element.namespace_uri() == Namespace::SVG) {
        force_position_static = true;
        if (local_name == "svg"sv) {
            force_position_static = false;
            for (auto ancestor = element.parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "foreignObject"sv)
                    break;
                if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "svg"sv) {
                    force_position_static = true;
                    break;
                }
            }
        }
    }

    bool force_symbol_display_inline = false;
    if (element.namespace_uri() == Namespace::SVG && local_name == "symbol"sv) {
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(element.parent())) {
            auto* host = shadow_root->host();
            force_symbol_display_inline = host->namespace_uri() == Namespace::SVG && host->local_name() == "use"sv;
        }
    }

    return {
        .display = to_ffi_display(display),
        .position = to_underlying(position),
        .float_value = to_underlying(float_value),
        .is_br_element = !abstract_element.pseudo_element().has_value() && is<HTML::HTMLBRElement>(element),
        .is_document_element = element.is_document_element(),
        .is_mathml_element = element.namespace_uri() == Namespace::MathML,
        .is_mathml_mtable = element.tag_name().equals_ignoring_ascii_case("mtable"sv),
        .is_mathml_mtr = element.tag_name().equals_ignoring_ascii_case("mtr"sv),
        .is_mathml_mtd = element.tag_name().equals_ignoring_ascii_case("mtd"sv),
        .has_parent_display = has_parent_display,
        .parent_display = has_parent_display ? to_ffi_display(*parent_display) : ComputedValuesFFI::FfiDisplay {},
        .is_wbr_element = should_adjust_element && is_html_element && local_name == HTML::TagNames::wbr,
        .disallow_display_contents = should_adjust_element && (input_allows_adjustment || (is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::canvas, HTML::TagNames::object, HTML::TagNames::iframe, HTML::TagNames::progress, HTML::TagNames::embed, HTML::TagNames::frame, HTML::TagNames::meter, HTML::TagNames::frameset, HTML::TagNames::img))),
        .rewrite_inline_flow = should_adjust_element && (input_allows_adjustment || (is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::select))),
        .is_button_element = should_adjust_element && is_html_element && local_name == HTML::TagNames::button,
        .force_line_height_normal = should_adjust_element && is_html_element && local_name == HTML::TagNames::select,
        .check_input_line_height = should_adjust_element && input_is_single_line,
        .hide_audio_without_controls = should_adjust_element && is_html_element && local_name == HTML::TagNames::audio && !element.has_attribute(HTML::AttributeNames::controls),
        .is_table_element = should_adjust_element && is_html_element && local_name == HTML::TagNames::table,
        .force_position_static = should_adjust_element && force_position_static,
        .force_symbol_display_inline = should_adjust_element && force_symbol_display_inline,
    };
}

static ComputedValuesFFI::FfiInputLineHeightMetrics input_line_height_metrics(ComputedStyleWorkingSet const& style, DOM::AbstractElement abstract_element, bool should_measure)
{
    ComputedValuesFFI::FfiInputLineHeightMetrics line_height_metrics {};
    if (should_measure) {
        line_height_metrics.current_line_height = style.line_height(abstract_element.element().document().font_computer()).to_double();
        line_height_metrics.minimum_line_height = normal_line_height(style.first_available_computed_font(abstract_element.element().document().font_computer())->pixel_metrics()).to_double();
    }
    return line_height_metrics;
}

template<typename SetProperty>
static void apply_element_style_adjustments(ComputedStyleWorkingSet const& style, DOM::AbstractElement abstract_element, ComputedValuesFFI::FfiElementStyleAdjustments const& adjustments, SetProperty set_property)
{
    if (adjustments.box_type.set_float_none)
        set_property(PropertyID::Float, KeywordStyleValue::create(Keyword::None));
    if (adjustments.box_type.changed_display)
        set_property(PropertyID::Display, DisplayStyleValue::create(display_from_ffi_display(adjustments.box_type.display)));

    auto const& element_style = adjustments.element_style;
    if (element_style.changed_display)
        set_property(PropertyID::Display, DisplayStyleValue::create(display_from_ffi_display(element_style.display)));
    if (element_style.set_position_static)
        set_property(PropertyID::Position, KeywordStyleValue::create(Keyword::Static));
    if (element_style.changed_text_align)
        set_property(PropertyID::TextAlign, KeywordStyleValue::create(static_cast<Keyword>(element_style.text_align)));

    auto line_height_metrics = input_line_height_metrics(style, abstract_element, element_style.check_input_line_height);
    if (element_style.set_line_height_normal || (element_style.check_input_line_height && line_height_metrics.current_line_height < line_height_metrics.minimum_line_height))
        set_property(PropertyID::LineHeight, KeywordStyleValue::create(Keyword::Normal));
}

// https://drafts.csswg.org/css-display/#transformations
void StyleComputer::adjust_element_style_if_needed(ComputedStyleWorkingSet& style, DOM::AbstractElement abstract_element) const
{
    auto input = make_box_type_transformation_input(
        abstract_element,
        style.display(),
        style.property(PropertyID::Position).to_keyword(),
        style.property(PropertyID::Float).to_keyword());
    auto adjustments = ComputedValuesFFI::rust_adjust_element_style(
        &input, to_underlying(style.property(PropertyID::TextAlign).to_keyword()));

    auto set_adjusted_property = [&](PropertyID property_id, NonnullRefPtr<StyleValue const> value) {
        // Animated values are stored separately from the builder's base values, so post-compute
        // adjustments must replace the sampled value as well.
        if (style.has_animated_property(property_id)) {
            auto is_result_of_transition = style.is_animated_property_result_of_transition(property_id)
                ? AnimatedPropertyResultOfTransition::Yes
                : AnimatedPropertyResultOfTransition::No;
            auto inherited = style.is_animated_property_inherited(property_id)
                ? ComputedStyleWorkingSet::Inherited::Yes
                : ComputedStyleWorkingSet::Inherited::No;
            style.set_animated_property(Badge<StyleComputer> {}, property_id, value, is_result_of_transition, inherited);
        }
        style.set_property(property_id, move(value));
    };

    style.set_display_before_box_type_transformation(display_from_ffi_display(input.display));
    apply_element_style_adjustments(style, abstract_element, adjustments, set_adjusted_property);
}

void StyleComputer::adjust_animated_element_style_if_needed(ComputedStyleWorkingSet& style, DOM::AbstractElement abstract_element) const
{
    auto display = style.has_animated_property(PropertyID::Display)
        ? style.display()
        : style.display_before_box_type_transformation();
    auto input = make_box_type_transformation_input(
        abstract_element,
        display,
        style.property(PropertyID::Position).to_keyword(),
        style.property(PropertyID::Float).to_keyword());
    auto adjustments = ComputedValuesFFI::rust_adjust_element_style(
        &input, to_underlying(style.property(PropertyID::TextAlign).to_keyword()));

    auto set_adjusted_property = [&](PropertyID property_id, NonnullRefPtr<StyleValue const> value) {
        if (style.property(property_id).equals(*value))
            return;
        auto is_result_of_transition = style.has_animated_property(property_id) && style.is_animated_property_result_of_transition(property_id)
            ? AnimatedPropertyResultOfTransition::Yes
            : AnimatedPropertyResultOfTransition::No;
        auto inherited = style.has_animated_property(property_id) && style.is_animated_property_inherited(property_id)
            ? ComputedStyleWorkingSet::Inherited::Yes
            : ComputedStyleWorkingSet::Inherited::No;
        style.set_animated_property(Badge<StyleComputer> {}, property_id, move(value), is_result_of_transition, inherited);
    };
    if (!style.has_animated_property(PropertyID::Display))
        set_adjusted_property(PropertyID::Display, DisplayStyleValue::create(display));
    apply_element_style_adjustments(style, abstract_element, adjustments, set_adjusted_property);
}

void StyleComputer::apply_post_compute_adjustments(ComputedStyleWorkingSet& style, DOM::AbstractElement abstract_element) const
{
    adjust_element_style_if_needed(style, abstract_element);
    resolve_effective_overflow_values(style);
    compute_text_align(style, abstract_element);
}

NonnullRefPtr<ComputedValues const> StyleComputer::create_document_style() const
{
    auto computed_properties = CSS::ComputedStyleWorkingSet::create();
    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        computed_properties->set_property(property_id, property_initial_value(property_id));
    }

    compute_property_values(*computed_properties, {});
    computed_properties->set_property(CSS::PropertyID::Width, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().width())));
    computed_properties->set_property(CSS::PropertyID::Height, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().height())));
    computed_properties->set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::Block)));
    computed_properties->freeze_computed_longhand_table();
    CSS::ColorResolutionContext color_resolution_context {
        .color_scheme = document().page().preferred_color_scheme(),
        .current_color = CSS::InitialValues::color(),
        .current_color_style_value = &computed_properties->property(PropertyID::Color),
        .calculation_resolution_context = { .length_resolution_context = CSS::Length::ResolutionContext::for_document(document()) },
    };
    auto computed_values = CSS::ComputedValues::create(*computed_properties, document(), document().style_scope(), move(color_resolution_context));
    return computed_values;
}

static u64 compute_style_sharing_key_hash(Vector<u64> const& key)
{
    VERIFY(key.size() >= ComputedValues::inherited_style_group_count);
    Fnv1a64 hash;
    for (size_t i = ComputedValues::inherited_style_group_count; i < key.size(); ++i)
        hash.add(key[i]);
    return hash.value();
}

static bool style_sharing_keys_are_equal(Vector<u64> const& first, Vector<u64> const& second)
{
    if (first.size() != second.size())
        return false;
    VERIFY(first.size() >= ComputedValues::inherited_style_group_count);
    for (size_t i = ComputedValues::inherited_style_group_count; i < first.size(); ++i) {
        if (first[i] != second[i])
            return false;
    }
    for (size_t i = 0; i < ComputedValues::inherited_style_group_count; ++i) {
        if (first[i] == second[i])
            continue;
        if (!ComputedValuesFFI::rust_style_group_payloads_equal(
                i,
                bit_cast<void const*>(static_cast<FlatPtr>(first[i])),
                bit_cast<void const*>(static_cast<FlatPtr>(second[i]))))
            return false;
    }
    return true;
}

static Optional<u32> style_record_transition_key(StyleEngine::StyleRecordDelta const& delta)
{
    if (!delta.old_style_record || !delta.new_style_record)
        return {};
    return pair_int_hash(Traits<CSS::StyleRecordID>::hash(delta.old_style_record), Traits<CSS::StyleRecordID>::hash(delta.new_style_record));
}

Optional<StyleComputer::ComputedStyleInvalidation> StyleComputer::cached_computed_style_invalidation(StyleEngine::StyleRecordDelta const& delta, bool element_folds_transform_into_layout) const
{
    auto key = style_record_transition_key(delta);
    if (!key.has_value())
        return {};
    auto entries = m_computed_style_invalidation_cache.get(*key);
    if (!entries.has_value())
        return {};
    for (auto const& entry : entries.value()) {
        if (entry.old_style_record == delta.old_style_record
            && entry.new_style_record == delta.new_style_record
            && entry.element_folds_transform_into_layout == element_folds_transform_into_layout)
            return entry.result;
    }
    return {};
}

void StyleComputer::cache_computed_style_invalidation(StyleEngine::StyleRecordDelta const& delta, bool element_folds_transform_into_layout, ComputedStyleInvalidation result) const
{
    auto key = style_record_transition_key(delta);
    if (!key.has_value())
        return;
    m_computed_style_invalidation_cache.ensure(*key).append({
        .old_style_record = delta.old_style_record,
        .new_style_record = delta.new_style_record,
        .element_folds_transform_into_layout = element_folds_transform_into_layout,
        .result = move(result),
    });
}

// Re-resolve the specified values that read the computed color into a swapped style. Values in
// inherited groups arrive resolved with the swap itself. Only fields one setter can write are
// handled; a property outside the switch sends the element down the full path instead.
static bool rebake_current_color_dependent_values(ComputedValues::Builder& builder, ComputedValues const& old_values, ComputedValues const& parent_values, HashMap<PropertyID, NonnullRefPtr<StyleValue const>> const& entries)
{
    ColorResolutionContext color_resolution_context {
        .color_scheme = old_values.color_scheme(),
        .current_color = parent_values.color(),
        .current_color_style_value = parent_values.color_style_value(),
        .calculation_resolution_context = {},
    };
    for (auto const& [property_id, value] : entries) {
        auto physical_property_id = property_id;
        if (property_is_logical_alias(property_id))
            physical_property_id = map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { old_values.writing_mode(), old_values.direction() });
        if (is_inherited_property(physical_property_id))
            continue;
        auto color = value->to_color(color_resolution_context);
        if (!color.has_value())
            return false;
        switch (physical_property_id) {
        case PropertyID::BorderTopColor:
            builder->set_border_top_color(color.value());
            break;
        case PropertyID::BorderRightColor:
            builder->set_border_right_color(color.value());
            break;
        case PropertyID::BorderBottomColor:
            builder->set_border_bottom_color(color.value());
            break;
        case PropertyID::BorderLeftColor:
            builder->set_border_left_color(color.value());
            break;
        case PropertyID::OutlineColor:
            builder->set_outline_color(color.value());
            break;
        case PropertyID::TextDecorationColor:
            builder->set_text_decoration_color(color.value());
            break;
        case PropertyID::BackgroundColor:
            builder->set_background_color(color.value());
            break;
        default:
            return false;
        }
    }
    return true;
}

RefPtr<ComputedValues const> StyleComputer::inherited_style_group_swap(DOM::Element& element, ComputedValues const& old_values, ComputedValues const& new_parent_values) const
{
    auto* input_record = element.style_input_record();
    if (!input_record || !old_values.property_inheritance_is_standard()
        || old_values.has_animated_values() || old_values.animated_properties()
        || new_parent_values.has_animated_values() || new_parent_values.animated_properties()
        || old_values.display().is_list_item()
        || old_values.color_scheme() != new_parent_values.color_scheme())
        return nullptr;

    auto new_parent_groups = new_parent_values.inherited_style_group_identities();
    auto update_input_record = [&] {
        VERIFY(input_record->words.size() >= new_parent_groups.size());
        input_record->pinned_parent_groups.set(new_parent_groups.span());
        for (size_t index = 0; index < new_parent_groups.size(); ++index)
            input_record->words[index] = bit_cast<FlatPtr>(new_parent_groups[index]);
    };

    auto old_style_record = element.style_record_identity();
    for (auto const& entry : m_inherited_style_group_swaps) {
        if (entry.old_style_record == old_style_record && entry.new_parent_groups == new_parent_groups) {
            update_input_record();
            return entry.result;
        }
    }

    auto inheritance_dependent_values = old_values.inheritance_dependent_specified_values_snapshot();
    for (auto const& [_, value] : inheritance_dependent_values) {
        if (!value->depends_on_current_color())
            return nullptr;
    }

    auto builder = ComputedValues::Builder::create_with_inherited_style_replaced(old_values, new_parent_values);
    if (old_values.color() != new_parent_values.color()
        && !rebake_current_color_dependent_values(builder, old_values, new_parent_values, inheritance_dependent_values))
        return nullptr;

    auto result = move(builder).build();
    m_inherited_style_group_swaps.append({ old_style_record, new_parent_groups, result });
    update_input_record();
    return result;
}

// Whether an element's own custom properties changed is a question about what it now holds against
// what it held, and it is answered the same way whether the values were computed or taken from
// another element.
static void report_custom_property_change(DOM::AbstractElement abstract_element, RefPtr<CustomPropertyData const> const& old_custom_property_data, Optional<bool&> did_change_custom_properties)
{
    if (!did_change_custom_properties.has_value())
        return;
    auto new_custom_property_data = abstract_element.custom_property_data();
    if (old_custom_property_data.ptr() == new_custom_property_data.ptr())
        return;
    static NeverDestroyed<OrderedHashMap<Utf16FlyString, StyleProperty>> empty_own_values;
    auto const& old_own = old_custom_property_data ? old_custom_property_data->own_values() : *empty_own_values;
    auto const& new_own = new_custom_property_data ? new_custom_property_data->own_values() : *empty_own_values;
    if (old_own != new_own)
        *did_change_custom_properties = true;
}

// A shared style installs the current inheritance parent's environment directly. Unlike a normal
// cascade, that can change the inherited environment during an otherwise ordinary recomputation,
// so descendants need the same propagation signal as they would receive from an explicit inherited
// custom-property reaction.
static void report_shared_custom_property_environment_change(DOM::AbstractElement abstract_element, RefPtr<CustomPropertyData const> const& old_custom_property_data, Optional<bool&> did_change_custom_properties)
{
    if (!did_change_custom_properties.has_value())
        return;
    if (old_custom_property_data.ptr() != abstract_element.custom_property_data().ptr())
        *did_change_custom_properties = true;
}

static bool computed_content_depends_on_counter_style_environment(ComputedContentData const& content)
{
    auto item_depends_on_counter_style_environment = [](ComputedContentItem const& item) {
        return item.has<ComputedContentCounter>() && item.get<ComputedContentCounter>().style.has<Utf16FlyString>();
    };
    return any_of(content.items, item_depends_on_counter_style_environment)
        || any_of(content.alt_text, item_depends_on_counter_style_environment);
}

StyleEngine::StyleRecordDelta StyleComputer::publish_computed_style_inputs(DOM::AbstractElement abstract_element, ComputedValues const& values) const
{
    auto publication = record_computed_style_inputs(Optional<DOM::AbstractElement> { abstract_element }, values, abstract_element.element().style_node_id());
    if (!abstract_element.pseudo_element().has_value()) {
        if (auto* record = abstract_element.element().style_input_record()) {
            record->computed_style_record = record->bind_next_published_style ? publication.new_style_record : StyleRecordID {};
            record->bind_next_published_style = false;
        }
    }
    return publication;
}

StyleRecordID StyleComputer::intern_computed_style_inputs(DOM::AbstractElement abstract_element, ComputedValues const& values) const
{
    return record_computed_style_inputs(Optional<DOM::AbstractElement> { abstract_element }, values, 0).new_style_record;
}

StyleRecordID StyleComputer::intern_anonymous_layout_style(ComputedValues const& values) const
{
    return record_computed_style_inputs({}, values, 0).new_style_record;
}

StyleEngine::StyleRecordDelta StyleComputer::record_computed_style_inputs(Optional<DOM::AbstractElement> abstract_element, ComputedValues const& values, StyleNodeID style_node_id) const
{
    auto const& base = values.base_values();
    // An unassigned record cannot own an animation overlay, so a layout-derived copy of an
    // animated style interns its final merged payloads directly. Splitting off the base there
    // would intern (and paint) the un-animated values.
    bool const unassigned_with_animations = style_node_id == 0 && (values.has_animated_values() || values.animated_properties());
    auto const& payload_source = unassigned_with_animations ? values : base;
    Array<void const*, to_underlying(StyleGroupIndex::Count)> payloads;
    for (size_t index = 0; index < payloads.size(); ++index)
        payloads[index] = payload_source.style_group_payload(static_cast<StyleGroupIndex>(index));
    auto custom_property_environment = abstract_element.has_value() ? abstract_element->custom_property_data() : nullptr;
    u8 dependency_flags = static_cast<u8>(base.depends_on_viewport_metrics())
        | (static_cast<u8>(base.font_metrics_depend_on_viewport_metrics()) << 1)
        | (static_cast<u8>(base.in_display_none_subtree()) << 2);
    if (abstract_element.has_value() && !abstract_element->pseudo_element().has_value()) {
        auto& element = abstract_element->element();
        bool const inherited_group_swap_eligible = element.style_input_record()
            && base.property_inheritance_is_standard()
            && !values.has_animated_values() && !values.animated_properties()
            && !element.has_relevant_animations() && !element.has_css_defined_animations()
            && element.property_ids_with_existing_transitions({}).is_empty()
            && element.property_ids_with_matching_transition_property_entry({}).is_empty()
            && !base.display().is_list_item();
        // Rust strips this node-local production capability before interning the semantic flags.
        constexpr u8 inherited_group_swap_eligible_flag = 1 << 3;
        if (inherited_group_swap_eligible)
            dependency_flags |= inherited_group_swap_eligible_flag;
    }
    u64 counter_style_environment_identity = 0;
    if (abstract_element.has_value()
        && (computed_content_depends_on_counter_style_environment(base.computed_content())
            || base.list_style_type_depends_on_counter_style_environment()))
        counter_style_environment_identity = abstract_element->style_scope().counter_style_environment_identity();
    auto animated_properties = style_node_id != 0 ? values.animated_properties() : nullptr;
    u64 animation_overlay_identity = animated_properties ? animated_properties->identity() : 0;
    Array<void const*, to_underlying(StyleGroupIndex::Count)> animation_overlay_payloads;
    if (animated_properties) {
        for (size_t index = 0; index < animation_overlay_payloads.size(); ++index)
            animation_overlay_payloads[index] = values.style_group_payload(static_cast<StyleGroupIndex>(index));
    }
    // A drive-built style borrows its recorded inheritance-dependent values from its table's
    // span, while a builder-copied style carries them as owned wrappers; publish both, with the
    // borrowed span winning like the snapshot's merge order.
    Vector<u16> inheritance_dependent_properties;
    Vector<void const*> inheritance_dependent_values;
    auto borrowed_inheritance_dependent = base.borrowed_inheritance_dependent_values();
    inheritance_dependent_properties.ensure_capacity(base.inheritance_dependent_specified_values().size() + borrowed_inheritance_dependent.size());
    inheritance_dependent_values.ensure_capacity(base.inheritance_dependent_specified_values().size() + borrowed_inheritance_dependent.size());
    for (auto const& entry : borrowed_inheritance_dependent) {
        inheritance_dependent_properties.unchecked_append(entry.property);
        inheritance_dependent_values.unchecked_append(entry.value);
    }
    for (auto const& [property_id, value] : base.inheritance_dependent_specified_values()) {
        if (inheritance_dependent_properties.contains_slow(to_underlying(property_id)))
            continue;
        inheritance_dependent_properties.unchecked_append(to_underlying(property_id));
        inheritance_dependent_values.unchecked_append(value->rust_style_value_data());
    }
    auto raw_cascaded_font_size = base.raw_cascaded_font_size();
    auto pseudo_kind = pseudo_element_to_ffi(abstract_element.has_value() ? abstract_element->pseudo_element() : Optional<CSS::PseudoElement> {});
    auto publication = const_cast<StyleComputer&>(*this).style_engine().publish_computed_groups(style_node_id, pseudo_kind, payloads, ComputedValues::inherited_style_group_count, custom_property_environment ? custom_property_environment->identity() : 0, base.pseudo_element_style_mask(), dependency_flags, counter_style_environment_identity, animation_overlay_identity, animated_properties, animated_properties ? animation_overlay_payloads.span() : ReadonlySpan<void const*> {}, base.property_importance_bitmap(), base.property_inheritance_bitmap(), inheritance_dependent_properties, inheritance_dependent_values, raw_cascaded_font_size ? raw_cascaded_font_size->rust_style_value_data() : nullptr, base.computed_longhand_table());
    return publication;
}

NonnullRefPtr<ComputedValues const> StyleComputer::materialize_style_record(DOM::AbstractElement abstract_element, Optional<bool&> did_change_custom_properties, StyleEngineMatchResult* reusable_matches, Optional<StyleEngine::StyleRecordDelta&> style_record_delta, u8 inherited_style_groups) const
{
    auto was_materializing_for_targeted_style_update = m_materializing_for_targeted_style_update;
    m_materializing_for_targeted_style_update = true;
    ScopeGuard restore_materialization_mode = [&] {
        m_materializing_for_targeted_style_update = was_materializing_for_targeted_style_update;
    };
    StyleSharingCandidate sharing;
    sharing.inherited_style_groups = inherited_style_groups;
    auto publish_computed_groups = [&](NonnullRefPtr<ComputedValues const> values) {
        auto publication = publish_computed_style_inputs(abstract_element, *values);
        if (sharing.new_style_sharing_entry_hash.has_value()) {
            auto bucket = m_style_sharing_cache.get(*sharing.new_style_sharing_entry_hash);
            VERIFY(bucket.has_value());
            auto& entry = bucket->last();
            VERIFY(entry.values.ptr() == values.ptr());
            VERIFY(entry.custom_property_data.ptr() == abstract_element.custom_property_data().ptr());
            VERIFY(!entry.style_record_identity.has_value());
            pin_style_record(publication.new_style_record);
            entry.style_record_identity = publication.new_style_record;
        }
        if (style_record_delta.has_value())
            *style_record_delta = publication;
        return values;
    };

    auto& style_scope = abstract_element.style_scope();
    auto computed_properties = compute_style_impl(abstract_element, ComputeStyleMode::Normal, did_change_custom_properties, style_scope, IncludeInlineStyle::Yes, reusable_matches, &sharing);
    if (sharing.reused_values)
        return publish_computed_groups(sharing.reused_values.release_nonnull());
    if (sharing.shared_values && sharing.shared_style_record_identity.has_value()) {
        auto& values = *sharing.shared_values;
        bool const inherited_group_swap_eligible = !abstract_element.pseudo_element().has_value()
            && abstract_element.element().style_input_record()
            && values.property_inheritance_is_standard()
            && !values.display().is_list_item();
        auto publication = const_cast<StyleComputer&>(*this).style_engine().assign_shared_style_record(
            abstract_element.element().style_node_id(),
            pseudo_element_to_ffi(abstract_element.pseudo_element()),
            *sharing.shared_style_record_identity,
            inherited_group_swap_eligible);
        if (!!publication.new_style_record) {
            if (!abstract_element.pseudo_element().has_value()) {
                if (auto* record = abstract_element.element().style_input_record()) {
                    record->computed_style_record = record->bind_next_published_style ? publication.new_style_record : StyleRecordID {};
                    record->bind_next_published_style = false;
                }
            }
            if (style_record_delta.has_value())
                *style_record_delta = publication;
            return sharing.shared_values.release_nonnull();
        }
    }
    if (sharing.shared_values)
        return publish_computed_groups(sharing.shared_values.release_nonnull());
    VERIFY(computed_properties);
    return publish_computed_groups(build_and_share_computed_values(computed_properties.release_nonnull(), abstract_element, style_scope, sharing));
}

// Where the halves of a style input record meet. Everything before the blocks is a fixed number of
// words, so the index of the first differing one says which half moved.
static constexpr size_t style_input_record_parent_custom_properties_index = ComputedValues::inherited_style_group_count;
static constexpr size_t style_sharing_style_scope_index = ComputedValues::inherited_style_group_count + 2;
static constexpr size_t style_input_record_element_index = style_input_record_parent_custom_properties_index + 1;
static constexpr size_t style_input_record_block_index = style_input_record_element_index + 6;

NonnullRefPtr<ComputedValues const> StyleComputer::build_and_share_computed_values(NonnullRefPtr<ComputedStyleWorkingSet> computed_properties, DOM::AbstractElement abstract_element, StyleScope const& style_scope, StyleSharingCandidate& sharing) const
{
    auto values_started_at = MonotonicTime::now();
    auto previous_style = abstract_element.computed_style();
    auto const* previous_values = previous_style ? &*previous_style : nullptr;
    auto const* previous_base = previous_values ? &previous_values->base_values() : nullptr;
    auto groups_to_rebuild = sharing.computed_groups_to_rebuild.value_or(ComputedValues::all_style_groups);
    auto& element = abstract_element.element();
    bool has_monospace_font_size_recascade = ComputedValuesFFI::rust_font_family_is_monospace(computed_properties->effective_property_data(PropertyID::FontFamily));
    if (groups_to_rebuild != ComputedValues::all_style_groups) {
        if (element.has_relevant_animations()
            || element.has_css_defined_animations())
            groups_to_rebuild = ComputedValues::all_style_groups;
    }
    auto computed_values = build_computed_values(
        *computed_properties,
        abstract_element,
        style_scope,
        previous_base,
        groups_to_rebuild);
    document().style_invalidation_counters().style_values_microseconds += (MonotonicTime::now() - values_started_at).to_microseconds();

    // The result answers for another element only if this computation read nothing about this one
    // beyond what the key holds. Everything that can read more says so on the element: a container
    // unit or query asks about its container, `attr()` about its attributes, a tree-counting
    // function about its place among its siblings, and `if()` about the environment. An animation
    // or transition carries state on the element itself, and the monospace font-size recascade
    // reads the whole ancestor chain rather than the inherited context. `var()` is not among them:
    // what it resolves against is the cascaded custom properties, which the blocks in the key
    // decide, and the inherited environment, which the parent in the key decides.
    if (sharing.is_candidate) {
        bool const computation_read_only_the_key = !element.style_uses_attr_css_function()
            && !element.style_uses_if_css_function()
            && !element.style_uses_custom_function()
            && !element.style_uses_tree_counting_function()
            && !element.style_depends_on_size_container_query()
            && !element.style_depends_on_style_container_query()
            && !has_monospace_font_size_recascade
            && !computed_values->animated_properties()
            && !computed_values->has_animated_values();
        // The same question decides whether this element's own next computation can be skipped, and
        // its record is what has to answer it once the flags are cleared for that computation.
        if (!abstract_element.pseudo_element().has_value()) {
            auto* record = element.style_input_record();
            VERIFY(record);
            record->read_beyond_the_record = !computation_read_only_the_key;
            record->style_uses_var_css_function = element.style_uses_var_css_function();
            record->style_uses_inherit_css_function = element.style_uses_inherit_css_function();
            record->explicitly_inherited_non_inherited_property = sharing.explicitly_inherited_non_inherited_property;
        }
        if (computation_read_only_the_key) {
            auto key_hash = compute_style_sharing_key_hash(sharing.key);
            Vector<u64> style_input_declaration_words;
            Vector<NonnullRefPtr<StyleValue const>> pinned_style_input_values;
            bool cascade_declares_custom_properties = false;
            if (!abstract_element.pseudo_element().has_value()) {
                auto declaration_words = element.style_input_record()->words.span().slice(style_input_record_block_index);
                style_input_declaration_words.append(declaration_words.data(), declaration_words.size());
                pinned_style_input_values = element.style_input_record()->pinned_values;
                cascade_declares_custom_properties = element.style_input_record()->cascade_declares_custom_properties;
            }
            if (sharing.explicitly_inherited_non_inherited_property && !!sharing.parent_style_record_identity)
                pin_style_record(sharing.parent_style_record_identity);
            m_style_sharing_cache.ensure(key_hash).append({
                .key = move(sharing.key),
                .pinned_parent_groups = move(sharing.pinned_parent_groups),
                .pinned_key_values = move(sharing.pinned_key_values),
                .parent_style_record_identity = sharing.parent_style_record_identity,
                .explicitly_inherited_non_inherited_property = sharing.explicitly_inherited_non_inherited_property,
                .values = computed_values,
                .custom_property_data = abstract_element.custom_property_data(),
                .style_record_identity = {},
                .style_input_declaration_words = move(style_input_declaration_words),
                .pinned_style_input_values = move(pinned_style_input_values),
                .cascade_declares_custom_properties = cascade_declares_custom_properties,
                .custom_property_references = abstract_element.pseudo_element().has_value() ? Vector<Utf16FlyString> {} : element.style_input_record()->custom_property_references,
                .style_uses_var_css_function = element.style_uses_var_css_function(),
                .style_uses_inherit_css_function = element.style_uses_inherit_css_function(),
            });
            sharing.new_style_sharing_entry_hash = key_hash;
            ++m_style_sharing_cache_entry_count;
        }
    }

    return computed_values;
}

NonnullRefPtr<ComputedStyleWorkingSet> StyleComputer::compute_properties_without_inline_style(DOM::AbstractElement abstract_element) const
{
    // Computing custom properties normally caches them on the element. Preserve the real cache while asking the
    // cascade what this element would look like without its inline declaration.
    auto custom_property_data = abstract_element.custom_property_data();
    ScopeGuard restore_custom_property_data = [&] {
        abstract_element.set_custom_property_data(move(custom_property_data));
    };

    auto& style_scope = abstract_element.style_scope();
    auto computed_properties = compute_style_impl(abstract_element, ComputeStyleMode::Normal, {}, style_scope, IncludeInlineStyle::No);
    VERIFY(computed_properties);
    return computed_properties.release_nonnull();
}

RefPtr<ComputedValues const> StyleComputer::compute_pseudo_element_style_if_needed(DOM::AbstractElement abstract_element, Optional<bool&> did_change_custom_properties, StyleEngineMatchResult* reusable_matches, Optional<StyleEngine::StyleRecordDelta&> style_record_delta) const
{
    StyleSharingCandidate sharing;
    auto publish_computed_groups = [&](NonnullRefPtr<ComputedValues const> values) {
        auto publication = publish_computed_style_inputs(abstract_element, *values);
        if (sharing.new_style_sharing_entry_hash.has_value()) {
            auto bucket = m_style_sharing_cache.get(*sharing.new_style_sharing_entry_hash);
            VERIFY(bucket.has_value());
            auto& entry = bucket->last();
            VERIFY(entry.values.ptr() == values.ptr());
            VERIFY(entry.custom_property_data.ptr() == abstract_element.custom_property_data().ptr());
            VERIFY(!entry.style_record_identity.has_value());
            pin_style_record(publication.new_style_record);
            entry.style_record_identity = publication.new_style_record;
        }
        if (style_record_delta.has_value())
            *style_record_delta = publication;
        return values;
    };

    auto& style_scope = abstract_element.style_scope();
    auto computed_properties = compute_style_impl(abstract_element, ComputeStyleMode::CreatePseudoElementStyleIfNeeded, did_change_custom_properties, style_scope, IncludeInlineStyle::Yes, reusable_matches, &sharing);
    if (sharing.reused_values)
        return publish_computed_groups(sharing.reused_values.release_nonnull());
    if (sharing.shared_values && sharing.shared_style_record_identity.has_value()) {
        auto publication = const_cast<StyleComputer&>(*this).style_engine().assign_shared_style_record(
            abstract_element.element().style_node_id(),
            pseudo_element_to_ffi(abstract_element.pseudo_element()),
            *sharing.shared_style_record_identity,
            false);
        if (!!publication.new_style_record) {
            if (style_record_delta.has_value())
                *style_record_delta = publication;
            return sharing.shared_values.release_nonnull();
        }
    }
    if (sharing.shared_values)
        return publish_computed_groups(sharing.shared_values.release_nonnull());
    if (!computed_properties) {
        auto node = abstract_element.element().style_node_id();
        VERIFY(abstract_element.pseudo_element().has_value());
        if (node != 0) {
            auto publication = const_cast<StyleComputer&>(*this).style_engine().remove_computed_pseudo(node, pseudo_element_to_ffi(abstract_element.pseudo_element()));
            if (style_record_delta.has_value())
                *style_record_delta = publication;
        }
        return {};
    }
    return publish_computed_groups(build_and_share_computed_values(computed_properties.release_nonnull(), abstract_element, style_scope, sharing));
}

NonnullRefPtr<ComputedValues const> StyleComputer::build_computed_values(ComputedStyleWorkingSet& computed_properties, DOM::AbstractElement abstract_element, StyleScope const& style_scope, ComputedValues const* previous_base, u32 groups_to_apply) const
{
    VERIFY(computation_context_cache_is_empty());
    ScopeGuard clear_computation_context_cache = [&] { clear_computation_context_caches(); };

    auto const& computation_context = get_computation_context_for_property(PropertyID::Color, computed_properties, abstract_element);
    ColorResolutionContext color_resolution_context {
        .color_scheme = computation_context.color_scheme,
        .current_color = InitialValues::color(),
        .current_color_style_value_data = computed_properties.effective_property_data(PropertyID::Color),
        .calculation_resolution_context = { .length_resolution_context = computation_context.length_resolution_context },
    };
    // NB: Sharing group payloads with the parent costs almost nothing for groups that already
    //     share the leaked defaults (a pointer compare each) and lets children reference their
    //     parent's payloads for everything they inherit unchanged, including values that can
    //     never match the process-wide defaults, like scope-resolved counter styles.
    auto adopt_group_payloads = [&](ComputedValues const& style) {
        if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value()) {
            if (auto parent_values = parent->computed_style())
                style.adopt_identical_group_payloads(*parent_values);
        }
        // NB: Siblings computing the same style never see each other's payloads through the parent:
        //     each one's non-default groups are fresh allocations that agree on every value. The last
        //     style built is offered as a second donor, so a run of alike elements collapses onto one
        //     set of payloads - and one style record - instead of minting per element.
        if (m_last_built_computed_values && m_last_built_computed_values != &style)
            style.adopt_identical_group_payloads(*m_last_built_computed_values);
        m_last_built_computed_values = &style;
    };

    auto const inherit_parent = abstract_element.element_to_inherit_style_from();
    auto inherit_parent_style = inherit_parent.has_value() ? inherit_parent->computed_style() : ComputedStyleRecordView {};
    auto const* inherit_parent_values = inherit_parent_style ? &*inherit_parent_style : nullptr;

    auto animated_properties = computed_properties.animated_properties_snapshot();
    RefPtr<ComputedStyleWorkingSet> unanimated_properties;
    auto* base_properties = &computed_properties;
    if (animated_properties && !animated_properties->is_empty()) {
        unanimated_properties = computed_properties.copy_without_animations();
        base_properties = unanimated_properties.ptr();
    }
    bool can_rebuild_selected_groups = previous_base
        && groups_to_apply != ComputedValues::all_style_groups;
    auto base_values = can_rebuild_selected_groups
        ? ComputedValues::create_over_base(*base_properties, document(), style_scope, color_resolution_context, *previous_base, groups_to_apply)
        : ComputedValues::create(*base_properties, document(), style_scope, color_resolution_context, inherit_parent_values);
    auto& counters = document().style_invalidation_counters();
    if (can_rebuild_selected_groups)
        counters.base_style_partial_builds++;
    else
        counters.base_style_full_builds++;
    if (!animated_properties || animated_properties->is_empty()) {
        adopt_group_payloads(*base_values);
        return base_values;
    }

    auto animated_values = can_rebuild_selected_groups
        ? ComputedValues::create_over_base(computed_properties, document(), style_scope, move(color_resolution_context), *base_values, groups_to_apply)
        : ComputedValues::create(computed_properties, document(), style_scope, move(color_resolution_context), inherit_parent_values);
    ComputedValues::Builder builder(*animated_values);
    builder->set_base_values(move(base_values));
    builder->set_animated_properties(animated_properties.ptr());
    auto style = move(builder).build();
    adopt_group_payloads(*style);
    return style;
}

NonnullRefPtr<ComputedValues const> StyleComputer::build_animated_computed_values(ComputedStyleWorkingSet& computed_properties, DOM::AbstractElement abstract_element, StyleScope const& style_scope, ComputedValues const& previous_values) const
{
    // The base half of an animated style does not move between frames, and everything the overlay
    // touches is named by the animated property set, so a frame keeps the previous base and
    // rebuilds only the groups the animation writes. A property whose group is unknown takes the
    // full build, and so does the color, because currentcolor consumers in other groups bake their
    // resolved colors against it.
    auto& counters = document().style_invalidation_counters();
    auto animated_properties = computed_properties.animated_properties_snapshot();
    u32 groups_to_apply = 0;
    bool touched_groups_known = animated_properties && !animated_properties->is_empty();
    if (touched_groups_known) {
        for (auto const& entry : animated_properties->values()) {
            auto group = ComputedValues::style_group_of_property(entry.key);
            if (!group.has_value() || entry.key == PropertyID::Color) {
                touched_groups_known = false;
                break;
            }
            groups_to_apply |= 1u << to_underlying(group.value());
        }
    }
    if (!touched_groups_known) {
        counters.animated_style_full_builds++;
        return build_computed_values(computed_properties, abstract_element, style_scope);
    }
    counters.animated_style_overlay_builds++;

    VERIFY(computation_context_cache_is_empty());
    ScopeGuard clear_computation_context_cache = [&] { clear_computation_context_caches(); };
    auto const& computation_context = get_computation_context_for_property(PropertyID::Color, computed_properties, abstract_element);
    ColorResolutionContext color_resolution_context {
        .color_scheme = computation_context.color_scheme,
        .current_color = InitialValues::color(),
        .current_color_style_value_data = computed_properties.effective_property_data(PropertyID::Color),
        .calculation_resolution_context = { .length_resolution_context = computation_context.length_resolution_context },
    };

    auto base_values = ComputedValues::Builder { previous_values.base_values() }.build();
    auto animated_values = ComputedValues::create_over_base(computed_properties, document(), style_scope, move(color_resolution_context), *base_values, groups_to_apply);
    ComputedValues::Builder builder { *animated_values };
    auto effective_overflow = ComputedValuesFFI::rust_resolve_effective_overflow_keywords(
        to_underlying(to_keyword(animated_values->overflow_x())),
        to_underlying(to_keyword(animated_values->overflow_y())));
    if (effective_overflow.changed_x)
        builder->set_overflow_x(keyword_to_overflow(static_cast<Keyword>(effective_overflow.x_keyword)).value());
    if (effective_overflow.changed_y)
        builder->set_overflow_y(keyword_to_overflow(static_cast<Keyword>(effective_overflow.y_keyword)).value());
    builder->set_base_values(move(base_values));
    builder->set_animated_properties(animated_properties.ptr());
    return move(builder).build();
}

NonnullRefPtr<ComputedStyleWorkingSet> StyleComputer::reconstruct_computed_properties(ComputedValues const& computed_values) const
{
    auto style = ComputedStyleWorkingSet::create_with_base_values_from(computed_values);
    // The recorded pre-box-type-transformation display tracks the animated display while one is applied, on both
    // the animated style and its base. When the animation stops covering `display`, re-adjustment must start over
    // from the base style's display, or the sampled value the finished animation left behind is resurrected as
    // the element's display. Box-type transformations are idempotent, so the adjusted base display is a sound
    // transformation input.
    if (auto const* animated_properties = computed_values.animated_properties(); animated_properties && animated_properties->has_property(PropertyID::Display))
        style->set_display_before_box_type_transformation(computed_values.base_values().display());
    style->freeze_computed_longhand_table();
    apply_animated_properties_to_reconstruction(*style, computed_values);
    return style;
}

void StyleComputer::apply_animated_properties_to_reconstruction(ComputedStyleWorkingSet& style, ComputedValues const& computed_values) const
{
    auto const* animated_properties = computed_values.animated_properties();
    if (!animated_properties)
        return;
    for (auto const& [property_id, value] : animated_properties->values()) {
        style.set_animated_property(
            Badge<StyleComputer> {}, property_id, value,
            animated_properties->is_property_result_of_transition(property_id) ? AnimatedPropertyResultOfTransition::Yes : AnimatedPropertyResultOfTransition::No,
            animated_properties->is_property_inherited(property_id) ? ComputedStyleWorkingSet::Inherited::Yes : ComputedStyleWorkingSet::Inherited::No);
    }
}

// Whether the element's own shape can be read off a fixed set of questions, so that two elements
// answering them the same way are interchangeable to the computation. Anything the computation
// discovers about the element outside these is caught afterwards, when the result is offered for
// sharing; this is only what has to be asked before the computation runs.
static Array<u64, 3> element_shape_style_sharing_key(DOM::AbstractElement abstract_element, Optional<Display> known_parent_display)
{
    auto const shape = make_box_type_transformation_input(
        abstract_element, InitialValues::display(), Keyword::Static, Keyword::None, known_parent_display);
    auto& element = abstract_element.element();
    auto const shape_bits = static_cast<u64>(shape.is_br_element)
        | (static_cast<u64>(shape.is_document_element) << 1)
        | (static_cast<u64>(shape.is_mathml_element) << 2)
        | (static_cast<u64>(shape.is_mathml_mtable) << 3)
        | (static_cast<u64>(shape.is_mathml_mtr) << 4)
        | (static_cast<u64>(shape.is_mathml_mtd) << 5)
        | (static_cast<u64>(shape.has_parent_display) << 6)
        | (static_cast<u64>(shape.is_wbr_element) << 7)
        | (static_cast<u64>(shape.disallow_display_contents) << 8)
        | (static_cast<u64>(shape.rewrite_inline_flow) << 9)
        | (static_cast<u64>(shape.is_button_element) << 10)
        | (static_cast<u64>(shape.force_line_height_normal) << 11)
        | (static_cast<u64>(shape.check_input_line_height) << 12)
        | (static_cast<u64>(shape.hide_audio_without_controls) << 13)
        | (static_cast<u64>(shape.is_table_element) << 14)
        | (static_cast<u64>(shape.force_position_static) << 15)
        | (static_cast<u64>(shape.force_symbol_display_inline) << 16);
    // The parent's display is read to decide the box type, and a parent whose display differs is a
    // different question even when the inherited half of its style is the same.
    auto const parent_display_bits = static_cast<u64>(shape.parent_display.tag)
        | (static_cast<u64>(shape.parent_display.outside) << 8)
        | (static_cast<u64>(shape.parent_display.inside) << 16)
        | (static_cast<u64>(shape.parent_display.list_item) << 24)
        | (static_cast<u64>(shape.parent_display.internal) << 32)
        | (static_cast<u64>(shape.parent_display.box_value) << 40);
    // The one question the box type transformation does not ask, and the one the driver does.
    return { shape_bits, parent_display_bits, static_cast<u64>(element.local_name() == HTML::TagNames::th) };
}

void StyleComputer::record_style_custom_property_reference(DOM::Element& element, Utf16FlyString const& name) const
{
    auto* record = element.style_input_record();
    if (!record)
        return;
    record->custom_property_references.append(name);
    if (record->pinned_parent_custom_property_data)
        return;
    if (auto parent = DOM::AbstractElement { element }.element_to_inherit_style_from(); parent.has_value())
        record->pinned_parent_custom_property_data = inheritable_custom_property_data(*parent);
    record->words[style_input_record_parent_custom_properties_index] = bit_cast<FlatPtr>(record->pinned_parent_custom_property_data.ptr());
}

static bool custom_property_references_are_unchanged(StyleInputRecord const& record, CustomPropertyData const* current)
{
    auto const* previous = record.pinned_parent_custom_property_data.ptr();
    for (auto const& name : record.custom_property_references) {
        auto const* previous_property = previous ? previous->get(name) : nullptr;
        auto const* current_property = current ? current->get(name) : nullptr;
        if (previous_property == current_property)
            continue;
        if (!previous_property || !current_property || !previous_property->value->equals(*current_property->value))
            return false;
    }
    return true;
}

static StyleInputRecord::Difference compare_style_input_records(StyleInputRecord const& previous, StyleInputRecord const& current)
{
    auto const differing_index = [&]() -> Optional<size_t> {
        auto const common = min(previous.words.size(), current.words.size());
        for (size_t index = 0; index < common; ++index) {
            if (previous.words[index] != current.words[index])
                return index;
        }
        if (previous.words.size() != current.words.size())
            return common;
        return {};
    }();
    if (!differing_index.has_value()) {
        // A presentational hint is mapped afresh for each computation, so what it says is compared
        // by value: naming it by the identity of the value object would report every element
        // carrying one as changed on every pass.
        if (previous.pinned_values.size() != current.pinned_values.size())
            return StyleInputRecord::Difference::Declarations;
        for (size_t index = 0; index < previous.pinned_values.size(); ++index) {
            if (!previous.pinned_values[index]->equals(current.pinned_values[index]))
                return StyleInputRecord::Difference::Declarations;
        }
        return StyleInputRecord::Difference::None;
    }
    if (*differing_index < style_input_record_parent_custom_properties_index)
        return StyleInputRecord::Difference::ParentStyle;
    if (*differing_index == style_input_record_parent_custom_properties_index)
        return StyleInputRecord::Difference::ParentCustomProperties;
    if (*differing_index < style_input_record_block_index)
        return StyleInputRecord::Difference::Element;
    return StyleInputRecord::Difference::Declarations;
}

bool StyleComputer::can_reuse_style_after_inherited_custom_property_change(DOM::Element& element) const
{
    auto* record = element.style_input_record();
    auto values = element.computed_style();
    if (!record || record->read_beyond_the_record || !values || values->animated_properties() || values->has_animated_values())
        return false;
    if (element.has_synthetic_pseudo_elements())
        return false;

    // An element with declarations of its own has to rebuild the environment against the new
    // parent. The fast path is for an element that inherited the old environment unchanged.
    if (record->cascade_declares_custom_properties)
        return false;
    if (element.custom_property_data({}).ptr() != record->pinned_parent_custom_property_data.ptr())
        return false;

    RefPtr<CustomPropertyData const> current_parent_data;
    if (auto parent = DOM::AbstractElement { element }.element_to_inherit_style_from(); parent.has_value())
        current_parent_data = inheritable_custom_property_data(*parent);
    if (!custom_property_references_are_unchanged(*record, current_parent_data.ptr()))
        return false;

    element.set_custom_property_data({}, current_parent_data);
    record->pinned_parent_custom_property_data = move(current_parent_data);
    record->words[style_input_record_parent_custom_properties_index] = bit_cast<FlatPtr>(record->pinned_parent_custom_property_data.ptr());
    return true;
}

static bool logical_property_group_has_computed_closure(LogicalPropertyGroup group)
{
    switch (group) {
    case LogicalPropertyGroup::BorderColor:
    case LogicalPropertyGroup::BorderRadius:
    case LogicalPropertyGroup::BorderStyle:
    case LogicalPropertyGroup::BorderWidth:
    case LogicalPropertyGroup::CornerShape:
    case LogicalPropertyGroup::Inset:
    case LogicalPropertyGroup::Margin:
    case LogicalPropertyGroup::MaxSize:
    case LogicalPropertyGroup::MinSize:
    case LogicalPropertyGroup::Overflow:
    case LogicalPropertyGroup::OverflowClipMargin:
    case LogicalPropertyGroup::Padding:
    case LogicalPropertyGroup::ScrollMargin:
    case LogicalPropertyGroup::ScrollPadding:
    case LogicalPropertyGroup::Size:
        return true;
    }
    VERIFY_NOT_REACHED();
}

static Vector<PropertyID> active_transition_properties_from_computed_values(ComputedValues const& values)
{
    auto const& transition_properties = values.transition_properties();
    auto const& transition_durations = values.transition_durations();
    auto const& transition_delays = values.transition_delays();
    VERIFY(!transition_durations.is_empty());
    VERIFY(!transition_delays.is_empty());

    Vector<PropertyID> properties;
    for (size_t index = 0; index < transition_properties.size(); ++index) {
        auto duration = transition_durations[index % transition_durations.size()].to_milliseconds();
        auto delay = transition_delays[index % transition_delays.size()].to_milliseconds();
        if (max(duration, 0.0) + delay <= 0 || !transition_properties[index].has_value())
            continue;
        auto maybe_property = property_id_from_string(*transition_properties[index]);
        if (!maybe_property.has_value())
            continue;
        auto append_property_mapping_logical_aliases = [&](PropertyID property_id) {
            if (property_is_logical_alias(property_id))
                properties.append(map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { values.writing_mode(), values.direction() }));
            else if (property_id != PropertyID::Custom)
                properties.append(property_id);
        };
        auto transition_property = maybe_property.release_value();
        if (property_is_shorthand(transition_property)) {
            for (auto property_id : expanded_longhands_for_shorthand(transition_property))
                append_property_mapping_logical_aliases(property_id);
        } else {
            append_property_mapping_logical_aliases(transition_property);
        }
    }
    return properties;
}

RefPtr<ComputedStyleWorkingSet> StyleComputer::compute_style_impl(DOM::AbstractElement abstract_element, ComputeStyleMode mode, Optional<bool&> did_change_custom_properties, StyleScope const& style_scope, IncludeInlineStyle include_inline_style, StyleEngineMatchResult* reusable_matches, StyleSharingCandidate* sharing) const
{
    // Special path for elements that represent a pseudo-element in some element's internal shadow tree.
    // FirstLetter is excluded so that ::first-letter rules can match against such elements normally.
    if (abstract_element.element().associated_shadow_host_pseudo_element().has_value() && abstract_element.pseudo_element() != CSS::PseudoElement::FirstLetter) {
        auto& element = abstract_element.element();
        auto& host_element = *element.root().parent_or_shadow_host_element();

        // We have to decide where to inherit from. If the pseudo-element has a parent element,
        // we inherit from that. Otherwise, we inherit from the host element in the light DOM.
        DOM::AbstractElement abstract_element_for_pseudo_element { host_element, element.associated_shadow_host_pseudo_element() };
        if (auto parent_element = element.parent_element())
            abstract_element_for_pseudo_element.set_inheritance_override(*parent_element);
        else
            abstract_element_for_pseudo_element.set_inheritance_override(host_element);

        auto& inherited_style_scope = abstract_element_for_pseudo_element.style_scope();
        auto inherited_pseudo_element_style = compute_style_impl(abstract_element_for_pseudo_element, ComputeStyleMode::Normal, {}, inherited_style_scope, include_inline_style);
        VERIFY(inherited_pseudo_element_style);
        auto style = ComputedStyleWorkingSet::create_with_base_values_from(*inherited_pseudo_element_style);

        adjust_element_style_if_needed(*style, abstract_element);
        style->freeze_computed_longhand_table();
        return style;
    }

    // 1. Perform the cascade. This produces the "specified style"
    bool did_match_any_pseudo_element_rules = false;
    auto style_engine_input = style_engine_cascade_input(abstract_element, reusable_matches);
    // A pseudo-element is materialized because some rule decides for it, so the rules that decide
    // for it are exactly what answers that question.
    if (style_engine_input && mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded)
        did_match_any_pseudo_element_rules = !style_engine_input->contributions.is_empty();
    // StyleEngine is the only thing that decides what matches. An element it cannot answer for is an
    // invariant breach rather than a request for a second opinion: a disconnected element is not
    // that case, since it has no identity and nothing decides for it at all (see the note on
    // disconnected elements in `style_engine_cascade_input`), and every other case is a bug that a
    // second answer would hide rather than fix.
    VERIFY(style_engine_input);
    auto const& cascade_input = *style_engine_input;

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        // NOTE: If we're computing style for a pseudo-element, we look for a number of reasons to bail early.

        // Some pseudo-elements are generated regardless of CSS rules, so we need to compute their styles even when no
        // rules matched.
        auto has_implicit_style = ComputedValuesFFI::rust_pseudo_element_has_implicit_style(to_underlying(*abstract_element.pseudo_element()));

        // Bail if no pseudo-element rules matched. Clear any stale custom property data so
        // getComputedStyle() doesn't return values from a previous match.
        if (!did_match_any_pseudo_element_rules && !has_implicit_style) {
            abstract_element.set_custom_property_data(nullptr);
            return {};
        }
    }

    auto old_custom_property_data = abstract_element.custom_property_data();

    // An element is offered the style another element already computed only when its whole input is
    // the same. What "whole input" means is the cascade's blocks - collected below - the inherited
    // context, the element's own shape, and what it reads of the style it is replacing. Ordinary
    // property computation reads that style's writing mode. Custom property computation can use
    // the whole style as its fallback context, so an input that needs it is bound to that identity
    // below. Transitions read the before-change style too, but their per-element state excludes
    // them from sharing before a key is built.
    auto const inheritance_parent = abstract_element.element_to_inherit_style_from();
    auto const inheritance_parent_style_record_identity = inheritance_parent.has_value() ? inheritance_parent->style_record_identity() : StyleRecordID {};
    auto const inheritance_parent_style_record = m_style_engine.style_record_view(inheritance_parent_style_record_identity);
    auto previous_style_record_identity = abstract_element.style_record_identity();
    auto const previous_style_record = m_style_engine.style_record_view(previous_style_record_identity);
    OwnPtr<ComputedStyleRecordView> inheritance_parent_style;
    OwnPtr<ComputedStyleRecordView> previous_style;
    ComputedValues const* inheritance_parent_values = nullptr;
    ComputedValues const* previous_values = nullptr;
    if (sharing) {
        auto& element = abstract_element.element();
        // A registered or running transition is decided per element from the style it is replacing,
        // and starting one is not something the values carry.
        sharing->is_candidate = inheritance_parent_style_record.present && !element.is_document_element()
            && !inheritance_parent_style_record.animated_properties
            && !element.has_relevant_animations()
            && !element.has_css_defined_animations()
            && element.property_ids_with_existing_transitions(abstract_element.pseudo_element()).is_empty()
            && element.property_ids_with_matching_transition_property_entry(abstract_element.pseudo_element()).is_empty();
    }
    Optional<Array<void const*, ComputedValues::inherited_style_group_count>> inherited_style_group_identities;
    auto get_inherited_style_group_identities = [&]() -> auto const& {
        if (!inherited_style_group_identities.has_value()) {
            VERIFY(inheritance_parent_style_record.payload_count >= ComputedValues::inherited_style_group_count);
            Array<void const*, ComputedValues::inherited_style_group_count> identities;
            for (size_t index = 0; index < identities.size(); ++index)
                identities[index] = inheritance_parent_style_record.payloads[index];
            inherited_style_group_identities = identities;
        }
        return *inherited_style_group_identities;
    };
    Optional<Array<u64, 3>> element_shape_key;
    auto append_element_shape_key = [&](Vector<u64>& key) {
        if (!element_shape_key.has_value()) {
            Optional<Display> parent_display;
            if (inheritance_parent_style_record.present) {
                auto const* box = static_cast<ComputedValuesFFI::BoxValues const*>(inheritance_parent_style_record.payloads[to_underlying(StyleGroupIndex::BoxValues)]);
                parent_display = display_from_ffi_display(box->display);
            }
            element_shape_key = element_shape_style_sharing_key(abstract_element, parent_display);
        }
        for (auto word : *element_shape_key)
            key.append(word);
    };
    if (sharing && sharing->is_candidate) {
        // What a child reads of the style it inherits from is that style's inherited half, so the
        // parent is named by the values of those groups rather than by the whole style: two parents
        // that differ only in what they say about themselves ask their children the same question.
        // A computation that reads more than that is bound to the style it read.
        auto const& inherited_group_identities = get_inherited_style_group_identities();
        for (auto const* group : inherited_group_identities)
            sharing->key.append(bit_cast<FlatPtr>(group));
        sharing->pinned_parent_groups.set(inherited_group_identities);
        sharing->pinned_parent_custom_property_data = inheritable_custom_property_data(*inheritance_parent);
        sharing->key.append(0);
        if (previous_style_record.present) {
            auto const* inherited_box = static_cast<ComputedValuesFFI::InheritedBoxValues const*>(previous_style_record.payloads[to_underlying(StyleGroupIndex::InheritedBoxValues)]);
            sharing->key.append(inherited_box->writing_mode + 1);
        } else {
            sharing->key.append(0);
        }
        sharing->key.append(0);
        sharing->key.append(document().style_environment_version());
        sharing->key.append(abstract_element.pseudo_element().has_value() ? to_underlying(*abstract_element.pseudo_element()) + 1 : 0);
        sharing->key.append(cascade_input.matching_pseudo_element_styles);
        sharing->parent_style_record_identity = inheritance_parent->style_record_identity();
        append_element_shape_key(sharing->key);
    }

    // What this computation is allowed to read, recorded so the next one on this element can ask
    // whether any of it moved. Nothing is answered from it yet: it says how often a recomputation
    // could have been, and which half of its input moved when it could not.
    Vector<StyleProperty> presentational_hint_properties;
    bool collected_presentational_hints = false;
    StyleInputRecord* new_style_input_record = nullptr;
    bool style_input_is_unchanged = false;
    bool only_declarations_changed = false;
    bool only_inherited_style_changed = false;
    // What the last computation decided and left behind, kept when this one differs from it in
    // nothing but which declarations it was handed.
    struct PreviousComputation {
        bool read_beyond_the_record { true };
        bool style_uses_attr_css_function { false };
        bool style_uses_var_css_function { false };
        bool style_uses_if_css_function { false };
        bool style_uses_custom_function { false };
        bool style_uses_inherit_css_function { false };
        bool style_uses_tree_counting_function { false };
        bool style_depends_on_size_container_query { false };
        bool style_depends_on_style_container_query { false };
        bool explicitly_inherited_non_inherited_property { false };
        Vector<Utf16FlyString> custom_property_references;
    };
    Optional<PreviousComputation> previous_computation;
    auto record_style_input = [&](StyleSharingEntry const* shared_entry = nullptr) {
        if (!sharing || abstract_element.pseudo_element().has_value() || !inheritance_parent_style_record.present)
            return;
        auto& element = abstract_element.element();
        auto& counters = document().style_invalidation_counters();

        if (!shared_entry && !collected_presentational_hints) {
            presentational_hint_properties = collect_presentational_hint_properties(abstract_element);
            collected_presentational_hints = true;
        }

        auto record = move(m_style_input_record_scratch);
        if (!record)
            record = make<StyleInputRecord>();
        record->words.clear_with_capacity();
        record->pinned_values.clear_with_capacity();
        record->custom_property_references.clear_with_capacity();
        record->read_beyond_the_record = true;
        record->style_uses_attr_css_function = false;
        record->style_uses_var_css_function = false;
        record->style_uses_if_css_function = false;
        record->style_uses_custom_function = false;
        record->style_uses_inherit_css_function = false;
        record->style_uses_tree_counting_function = false;
        record->style_depends_on_size_container_query = false;
        record->style_depends_on_style_container_query = false;
        record->explicitly_inherited_non_inherited_property = false;
        record->cascade_reads_custom_properties = false;
        record->cascade_declares_custom_properties = false;
        record->pinned_parent_custom_property_data = nullptr;
        record->computed_style_record = {};
        record->bind_next_published_style = false;
        auto const& inherited_group_identities = get_inherited_style_group_identities();
        record->pinned_parent_groups.set(inherited_group_identities.span());
        for (auto const* group : inherited_group_identities)
            record->words.append(bit_cast<FlatPtr>(group));
        record->words.append(0);
        record->words.append(style_scope.style_engine_tree_scope().value());
        record->words.append(cascade_input.matching_pseudo_element_styles);
        append_element_shape_key(record->words);
        // The environment names what reaches an element by no route the blocks describe: a font
        // arriving, the viewport moving, a registration. It sits with the element's own words rather
        // than with the blocks, so that a version that moved is never reported as a change of
        // declarations - a reuse admitted on the declarations alone must not be admitted by it.
        record->words.append(document().style_environment_version());
        VERIFY(record->words.size() == style_input_record_block_index);

        if (shared_entry) {
            record->words.append(shared_entry->style_input_declaration_words.data(), shared_entry->style_input_declaration_words.size());
            record->pinned_values = shared_entry->pinned_style_input_values;
            record->cascade_reads_custom_properties = sharing->cascade_reads_custom_properties;
            record->cascade_declares_custom_properties = shared_entry->cascade_declares_custom_properties;
        } else {
            auto const inline_style = include_inline_style == IncludeInlineStyle::Yes && cascade_input.inline_style_context_index.has_value()
                ? abstract_element.inline_style()
                : GC::Ptr<CSSStyleProperties const> {};
            auto const dependencies = append_cascade_blocks_to_key(record->words, record->pinned_values, cascade_input, presentational_hint_properties, inline_style, CascadeBlockKeyValueComparison::ByValue);
            record->cascade_reads_custom_properties = dependencies.reads_custom_properties;
            record->cascade_declares_custom_properties = dependencies.declares_custom_properties;
        }
        if (record->cascade_reads_custom_properties && inheritance_parent.has_value()) {
            record->pinned_parent_custom_property_data = inheritance_parent->custom_property_data();
            record->words[style_input_record_parent_custom_properties_index] = bit_cast<FlatPtr>(record->pinned_parent_custom_property_data.ptr());
        }

        if (auto const* previous = element.style_input_record()) {
            record->computed_style_record = previous->computed_style_record;
            switch (compare_style_input_records(*previous, *record)) {
            case StyleInputRecord::Difference::None:
                style_input_is_unchanged = true;
                // A computation that is skipped leaves no marks, so the record keeps the ones the
                // computation it stands in for left.
                record->read_beyond_the_record = previous->read_beyond_the_record;
                record->style_uses_attr_css_function = previous->style_uses_attr_css_function;
                record->style_uses_var_css_function = previous->style_uses_var_css_function;
                record->style_uses_if_css_function = previous->style_uses_if_css_function;
                record->style_uses_custom_function = previous->style_uses_custom_function;
                record->style_uses_inherit_css_function = previous->style_uses_inherit_css_function;
                record->style_uses_tree_counting_function = previous->style_uses_tree_counting_function;
                record->style_depends_on_size_container_query = previous->style_depends_on_size_container_query;
                record->style_depends_on_style_container_query = previous->style_depends_on_style_container_query;
                record->explicitly_inherited_non_inherited_property = previous->explicitly_inherited_non_inherited_property;
                record->custom_property_references = previous->custom_property_references;
                break;
            case StyleInputRecord::Difference::ParentStyle:
                counters.element_style_input_changed_by_parent_style++;
                if (sharing->inherited_style_groups != 0) {
                    only_inherited_style_changed = true;
                    previous_computation = PreviousComputation {
                        .read_beyond_the_record = previous->read_beyond_the_record,
                        .style_uses_attr_css_function = previous->style_uses_attr_css_function,
                        .style_uses_var_css_function = previous->style_uses_var_css_function,
                        .style_uses_if_css_function = previous->style_uses_if_css_function,
                        .style_uses_custom_function = previous->style_uses_custom_function,
                        .style_uses_inherit_css_function = previous->style_uses_inherit_css_function,
                        .style_uses_tree_counting_function = previous->style_uses_tree_counting_function,
                        .style_depends_on_size_container_query = previous->style_depends_on_size_container_query,
                        .style_depends_on_style_container_query = previous->style_depends_on_style_container_query,
                        .explicitly_inherited_non_inherited_property = previous->explicitly_inherited_non_inherited_property,
                        .custom_property_references = previous->custom_property_references,
                    };
                }
                break;
            case StyleInputRecord::Difference::ParentCustomProperties:
                counters.element_style_input_changed_by_parent_custom_properties++;
                break;
            case StyleInputRecord::Difference::Element:
                break;
            case StyleInputRecord::Difference::Declarations:
                only_declarations_changed = true;
                previous_computation = PreviousComputation {
                    .read_beyond_the_record = previous->read_beyond_the_record,
                    .style_uses_attr_css_function = previous->style_uses_attr_css_function,
                    .style_uses_var_css_function = previous->style_uses_var_css_function,
                    .style_uses_if_css_function = previous->style_uses_if_css_function,
                    .style_uses_custom_function = previous->style_uses_custom_function,
                    .style_uses_inherit_css_function = previous->style_uses_inherit_css_function,
                    .style_uses_tree_counting_function = previous->style_uses_tree_counting_function,
                    .style_depends_on_size_container_query = previous->style_depends_on_size_container_query,
                    .style_depends_on_style_container_query = previous->style_depends_on_style_container_query,
                    .explicitly_inherited_non_inherited_property = previous->explicitly_inherited_non_inherited_property,
                    .custom_property_references = previous->custom_property_references,
                };
                break;
            }
        }
        // The buffer the element gives up becomes the next element's, so a pass over a document
        // allocates one record's worth of words and reuses it.
        m_style_input_record_scratch = element.take_style_input_record();
        element.set_style_input_record(move(record));
        new_style_input_record = element.style_input_record();
    };

    // Nothing this element's last computation read has moved, so the style it produced is still the
    // answer and deriving it again would produce the same one. What the skipped computation would
    // have decided besides the values - the marks it leaves on the element and on its parent - the
    // record carries, because nothing else leaves them.
    auto last_style_still_stands = [&]() -> bool {
        if (!sharing || !sharing->is_candidate || !new_style_input_record)
            return false;
        auto existing = abstract_element.computed_style();
        if (!existing || existing->animated_properties() || existing->has_animated_values())
            return false;
        // What the record does not name is everything that reaches the element some other way: a
        // font finishing loading, the viewport moving, or a registration arriving. An environment
        // or targeted update may include one of those inputs, so only descendant propagation can
        // reuse the record's parent half here.
        if (m_materializing_for_targeted_style_update)
            return false;
        return true;
    };

    // Hands the element back the style it already has, preserving the bookkeeping the skipped
    // computation would have produced.
    auto reuse_last_computed_style = [&]() {
        auto& element = abstract_element.element();
        auto const& record = *new_style_input_record;
        if (record.style_uses_attr_css_function)
            element.set_style_uses_attr_css_function();
        if (record.style_uses_var_css_function)
            element.set_style_uses_var_css_function();
        if (record.style_uses_if_css_function)
            element.set_style_uses_if_css_function();
        if (record.style_uses_custom_function)
            element.set_style_uses_custom_function();
        if (record.style_uses_inherit_css_function)
            element.set_style_uses_inherit_css_function();
        if (record.style_uses_tree_counting_function)
            element.set_style_uses_tree_counting_function();
        if (record.style_depends_on_size_container_query)
            element.set_style_depends_on_size_container_query();
        if (record.style_depends_on_style_container_query)
            element.set_style_depends_on_style_container_query();
        if (record.explicitly_inherited_non_inherited_property) {
            if (auto* parent = element.parent())
                parent->set_children_may_depend_on_non_inherited_property_inheritance();
        }
        auto existing = abstract_element.computed_style();
        VERIFY(existing);
        sharing->reused_values = ComputedValues::Builder { *existing }.build();
    };

    auto reuse_computed_style = [&]() -> bool {
        if (!style_input_is_unchanged || new_style_input_record->read_beyond_the_record || !last_style_still_stands())
            return false;
        reuse_last_computed_style();
        return true;
    };

    bool has_complete_sharing_key = false;
    auto find_shared_style = [&]() {
        auto key_hash = compute_style_sharing_key_hash(sharing->key);
        auto bucket = m_style_sharing_cache.get(key_hash);
        if (!bucket.has_value())
            return false;
        for (auto const& entry : *bucket) {
            if (!style_sharing_keys_are_equal(entry.key, sharing->key))
                continue;
            // An entry that read the half of its inherited style the key does not name answers only
            // for an element inheriting from that very style.
            if (entry.explicitly_inherited_non_inherited_property && entry.parent_style_record_identity != inheritance_parent->style_record_identity())
                continue;
            if (!new_style_input_record)
                record_style_input(&entry);
            sharing->shared_values = entry.values;
            // The custom properties the computation resolved are the other element's only when the
            // key named the environment they resolved against. An element that reads none keeps the
            // data its own cascade gave it, which is its own parent's - and an answer taken before
            // the cascade has run has to say that itself, since nothing else will.
            if (sharing->cascade_reads_custom_properties)
                abstract_element.set_custom_property_data(custom_property_data_keeping_identity(document(), abstract_element.custom_property_data(), entry.custom_property_data));
            else if (has_complete_sharing_key)
                abstract_element.set_custom_property_data(inheritance_parent.has_value() ? inheritable_custom_property_data(*inheritance_parent) : nullptr);
            if (entry.style_record_identity.has_value()
                && abstract_element.custom_property_data().ptr() == entry.custom_property_data.ptr())
                sharing->shared_style_record_identity = entry.style_record_identity;
            if (entry.style_uses_var_css_function)
                abstract_element.element().set_style_uses_var_css_function();
            if (entry.style_uses_inherit_css_function)
                abstract_element.element().set_style_uses_inherit_css_function();
            if (entry.explicitly_inherited_non_inherited_property) {
                if (auto* parent = abstract_element.element().parent())
                    parent->set_children_may_depend_on_non_inherited_property_inheritance();
            }
            compute_transitioned_properties(*sharing->shared_values, abstract_element);
            // An entry is only published by a computation that read nothing beyond the key, so an
            // element answered from one is one whose own next computation can be skipped.
            if (!abstract_element.pseudo_element().has_value()) {
                auto* record = abstract_element.element().style_input_record();
                VERIFY(record);
                record->read_beyond_the_record = false;
                record->style_uses_var_css_function = entry.style_uses_var_css_function;
                record->style_uses_inherit_css_function = entry.style_uses_inherit_css_function;
                record->explicitly_inherited_non_inherited_property = entry.explicitly_inherited_non_inherited_property;
                record->custom_property_references = entry.custom_property_references;
            }
            report_shared_custom_property_environment_change(abstract_element, old_custom_property_data, did_change_custom_properties);
            return true;
        }
        return false;
    };

    if (!m_materializing_for_targeted_style_update)
        record_style_input();

    // The element's own last answer comes before another element's: it needs no lookup, and it is
    // the one whose marks are already on the element.
    if (reuse_computed_style()) {
        auto& counters = document().style_invalidation_counters();
        counters.element_style_input_reused++;
        // The mechanism is exactly the kind that can be wrong for a year: right everywhere the
        // record is complete, and silently wrong where it is not. Under verification every reuse
        // derives the style anyway and every longhand of the two is compared.
        static bool const verify_reuse = getenv("LIBWEB_VERIFY_STYLE_INPUT_REUSE") != nullptr;
        if (verify_reuse) {
            auto reused = sharing->reused_values.release_nonnull();
            auto custom_property_data = abstract_element.custom_property_data();
            auto cascaded = compute_cascaded_values(abstract_element, cascade_input, include_inline_style, nullptr,
                collected_presentational_hints ? &presentational_hint_properties : nullptr);
            auto derived = compute_properties(abstract_element, cascaded, cascade_input.matching_pseudo_element_styles, nullptr);
            abstract_element.set_custom_property_data(move(custom_property_data));
            auto reused_properties = reconstruct_computed_properties(*reused);
            for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
                auto property_id = static_cast<PropertyID>(i);
                // A logical alias is answered through the writing mode rather than stored, so the
                // two sides spell it differently without disagreeing about anything.
                if (property_is_logical_alias(property_id))
                    continue;
                auto const& reused_value = reused_properties->property(property_id);
                auto const& derived_value = derived->property(property_id);
                if (reused_value.equals(derived_value))
                    continue;
                // The reused side is reconstructed from computed values and the derived side comes
                // straight out of the cascade, so the two can spell the same value differently.
                if (reused_value.to_string(SerializationMode::ResolvedValue) == derived_value.to_string(SerializationMode::ResolvedValue))
                    continue;
                dbgln("StyleEngine: a reused style differs on {}: reused {}, derived {}",
                    string_from_property_id(property_id), reused_value.to_string(SerializationMode::Normal), derived_value.to_string(SerializationMode::Normal));
                // A reconstructed colour is spelled in the legacy form and a cascaded one in the
                // space it was written in, so the two disagree about the text and not the colour.
                // The line above still reports it; only the two colours are not made fatal.
                if (reused_value.is_color() && derived_value.is_color())
                    continue;
                VERIFY_NOT_REACHED();
            }
            sharing->reused_values = move(reused);
        }
        report_custom_property_change(abstract_element, old_custom_property_data, did_change_custom_properties);
        return {};
    }

    if (sharing && sharing->is_candidate && cascade_input.match_signature.has_value()) {
        // StyleEngine has reduced the matched rules to the blocks that can supply a winning
        // declaration. Name those blocks directly rather than the complete match signature: two
        // elements whose losing selectors differ still have the same cascade input. The only
        // remaining blocks belong to the element itself, so append their identities now and let
        // style sharing answer before their declarations are applied.
        auto const inline_style = include_inline_style == IncludeInlineStyle::Yes && cascade_input.inline_style_context_index.has_value()
            ? abstract_element.inline_style()
            : GC::Ptr<CSSStyleProperties const> {};
        if (!collected_presentational_hints) {
            presentational_hint_properties = collect_presentational_hint_properties(abstract_element);
            collected_presentational_hints = true;
        }
        auto const dependencies = append_cascade_blocks_to_key(sharing->key, sharing->pinned_key_values, cascade_input, presentational_hint_properties, inline_style, CascadeBlockKeyValueComparison::ByIdentity);
        sharing->cascade_reads_custom_properties = dependencies.reads_custom_properties;
        if (dependencies.reads_style_scope)
            sharing->key[style_sharing_style_scope_index] = style_scope.style_engine_tree_scope().value();

        // The key names every block the cascade will apply, so what those blocks read of the
        // inherited custom property environment is settled here rather than after the cascade.
        if (sharing->cascade_reads_custom_properties && inheritance_parent.has_value()) {
            sharing->pinned_parent_custom_property_data = inheritance_parent->custom_property_data();
            sharing->key.append(bit_cast<FlatPtr>(sharing->pinned_parent_custom_property_data.ptr()));
        }
        sharing->key.append(sharing->cascade_reads_custom_properties ? static_cast<FlatPtr>(previous_style_record_identity.value()) : 0);
        sharing->key.append(sharing->cascade_reads_custom_properties ? m_style_sharing_transaction_generation : 0);

        has_complete_sharing_key = true;
        if (find_shared_style()) {
            if (auto node = abstract_element.element().style_node_id(); node != 0 && !abstract_element.pseudo_element().has_value())
                const_cast<StyleComputer&>(*this).style_engine().prepare_shared_exact_cascade_state(node);
            document().style_invalidation_counters().element_style_shared_computations++;
            return {};
        }
    }

    if (!new_style_input_record)
        record_style_input();

    auto materialize_style_record_view = [&](StyleEngine::StyleRecordView const& view, StyleRecordID identity) -> OwnPtr<ComputedStyleRecordView> {
        if (!view.present)
            return {};
        pin_style_record(identity);
        return make<ComputedStyleRecordView>(view, *this, identity);
    };
    inheritance_parent_style = materialize_style_record_view(inheritance_parent_style_record, inheritance_parent_style_record_identity);
    inheritance_parent_values = inheritance_parent_style ? &**inheritance_parent_style : nullptr;
    previous_style = materialize_style_record_view(previous_style_record, previous_style_record_identity);
    previous_values = previous_style ? &**previous_style : nullptr;

    auto cascade_started_at = MonotonicTime::now();
    auto cascaded_properties = compute_cascaded_values(
        abstract_element,
        cascade_input,
        include_inline_style,
        sharing && sharing->is_candidate && !has_complete_sharing_key ? sharing : nullptr,
        collected_presentational_hints ? &presentational_hint_properties : nullptr);
    document().style_invalidation_counters().style_cascade_microseconds += (MonotonicTime::now() - cascade_started_at).to_microseconds();

    // What the cascade decided is what the rest of the computation reads, so an element whose
    // cascade came out exactly as it did last time computes the style it already has. A stylesheet
    // arriving mid-load changes which declarations most elements match and which of them win for
    // very few, and this is the case the record's declaration half cannot tell apart on its own.
    bool exact_cascade_is_unchanged = false;
    if (auto node = abstract_element.element().style_node_id(); sharing && node != 0) {
        auto publication = const_cast<StyleComputer&>(*this).style_engine().publish_exact_cascade_state(
            node,
            pseudo_element_to_ffi(abstract_element.pseudo_element()),
            cascaded_properties->rust_store(),
            sharing->inherited_style_groups);
        exact_cascade_is_unchanged = publication.unchanged;
        if (previous_values
            && (only_declarations_changed || only_inherited_style_changed)
            && previous_computation.has_value()
            && !previous_computation->read_beyond_the_record
            && !previous_computation->style_uses_var_css_function
            && !previous_computation->style_uses_inherit_css_function
            && !previous_computation->explicitly_inherited_non_inherited_property) {
            sharing->computed_groups_to_rebuild = publication.computed_group_mask & ComputedValues::all_style_groups;
            sharing->computed_properties_to_evaluate = Array<u64, (number_of_longhand_properties + 63) / 64> {
                publication.computed_property_word_0,
                publication.computed_property_word_1,
                publication.computed_property_word_2,
                publication.computed_property_word_3,
                publication.computed_property_word_4,
                publication.computed_property_word_5,
            };
            sharing->computed_property_closure_is_exact = publication.computed_property_closure_is_exact;
        }
    }
    if (previous_computation.has_value()
        && !previous_computation->read_beyond_the_record
        && exact_cascade_is_unchanged
        && !only_inherited_style_changed
        // A computation that read the other half of its inherited style, through `inherit` on a
        // non-inherited property, read what the record does not name, so an unchanged cascade does
        // not mean an unchanged answer.
        && !previous_computation->explicitly_inherited_non_inherited_property
        // A transition starts by comparing the style that was to the style that is, and a
        // computation skipped here never makes that comparison. The element that starts none is the
        // one this asks about, which is the same question the transition step itself asks first.
        && abstract_element.element().property_ids_with_matching_transition_property_entry(abstract_element.pseudo_element()).is_empty()
        && abstract_element.element().property_ids_with_existing_transitions(abstract_element.pseudo_element()).is_empty()
        // The steps this skips after the cascade are about the element's custom properties: they are
        // resolved against the style, compared against the environment this computation replaced,
        // and any change reported to the caller. An element whose cascade declared none keeps the
        // environment it already had, and all three have nothing to say.
        && abstract_element.custom_property_data().ptr() == old_custom_property_data.ptr()
        && last_style_still_stands()) {
        // The computation being skipped is the one that would have left these, so the record that
        // stands for it carries them instead.
        new_style_input_record->read_beyond_the_record = false;
        new_style_input_record->style_uses_attr_css_function = previous_computation->style_uses_attr_css_function;
        new_style_input_record->style_uses_var_css_function = previous_computation->style_uses_var_css_function;
        new_style_input_record->style_uses_if_css_function = previous_computation->style_uses_if_css_function;
        new_style_input_record->style_uses_custom_function = previous_computation->style_uses_custom_function;
        new_style_input_record->style_uses_inherit_css_function = previous_computation->style_uses_inherit_css_function;
        new_style_input_record->style_uses_tree_counting_function = previous_computation->style_uses_tree_counting_function;
        new_style_input_record->style_depends_on_size_container_query = previous_computation->style_depends_on_size_container_query;
        new_style_input_record->style_depends_on_style_container_query = previous_computation->style_depends_on_style_container_query;
        new_style_input_record->explicitly_inherited_non_inherited_property = previous_computation->explicitly_inherited_non_inherited_property;
        new_style_input_record->custom_property_references = move(previous_computation->custom_property_references);
        reuse_last_computed_style();
        return {};
    }

    // The inherited custom property environment is named only now, because only the collection above
    // can say whether anything in the cascade reads it. A key already complete before the cascade
    // has named it there.
    if (sharing && sharing->is_candidate && !has_complete_sharing_key && sharing->cascade_reads_custom_properties && inheritance_parent.has_value()) {
        sharing->pinned_parent_custom_property_data = inheritance_parent->custom_property_data();
        sharing->key.append(bit_cast<FlatPtr>(sharing->pinned_parent_custom_property_data.ptr()));
    }
    if (sharing && sharing->is_candidate && !has_complete_sharing_key)
        sharing->key.append(sharing->cascade_reads_custom_properties ? static_cast<FlatPtr>(previous_style_record_identity.value()) : 0);
    if (sharing && sharing->is_candidate && !has_complete_sharing_key)
        sharing->key.append(sharing->cascade_reads_custom_properties ? m_style_sharing_transaction_generation : 0);

    // A declared animation registers an animation of its own on the element, which is something the
    // element owns rather than something the values carry, so an element declaring one derives its
    // own style. A declared transition is not like that. What it registers is decided by the
    // computed values, and it is registered again by every computation before anything reads it, so
    // an element that takes another's answer this pass registers the same thing the pass it needs
    // it - which is the pass a transitioned property changes, and that pass changes the blocks the
    // key names, so it cannot be answered from here. An element that does start a transition holds
    // animated values and is refused at the publish.
    if (sharing && sharing->is_candidate) {
        auto animation_name = cascaded_properties->property(PropertyID::AnimationName);
        auto declares_animation = animation_name
            && !(animation_name->is_keyword() && animation_name->to_keyword() == Keyword::None)
            && !(animation_name->is_value_list() && all_of(animation_name->as_value_list().values(), [](auto const& value) {
                   return value->is_keyword() && value->to_keyword() == Keyword::None;
               }));
        if (declares_animation)
            sharing->is_candidate = false;
    }

    if (sharing && sharing->is_candidate) {
        if (!has_complete_sharing_key && find_shared_style()) {
            // Equal values borrowed from another element do not yet certify that every selector
            // reaction continuing below this element has settled. Keep the next comparison
            // conservative until that continuation is represented in the dependency closure.
            if (auto node = abstract_element.element().style_node_id(); node != 0) {
                const_cast<StyleComputer&>(*this).style_engine().discard_pending_exact_cascade_state(
                    node,
                    pseudo_element_to_ffi(abstract_element.pseudo_element()));
            }
            document().style_invalidation_counters().element_style_shared_computations++;
            return {};
        }
    }

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        // Bail if no pseudo-element would be generated due to...
        // - content: none
        // - content: normal (for ::before and ::after)
        auto content_value = cascaded_properties->property(CSS::PropertyID::Content);
        if (ComputedValuesFFI::rust_pseudo_element_content_bails(content_value ? content_value->rust_style_value_data() : nullptr, to_underlying(*abstract_element.pseudo_element())))
            return {};
    }

    auto computed_group_mask = sharing && sharing->is_candidate ? sharing->computed_groups_to_rebuild.value_or(ComputedValues::all_style_groups) : ComputedValues::all_style_groups;
    if (computed_group_mask == 0)
        computed_group_mask = ComputedValues::all_style_groups;
    auto const* previous_computed_values = new_style_input_record
            && new_style_input_record->computed_style_record == previous_style_record_identity
        ? previous_values
        : nullptr;
    auto& element = abstract_element.element();
    Vector<PropertyID> retained_transition_candidates;
    if (previous_computed_values)
        retained_transition_candidates = active_transition_properties_from_computed_values(*previous_computed_values);
    bool transition_definition_changed = false;
    auto transition_property_differs_from_initial = [&](PropertyID property_id) {
        auto value = cascaded_properties->property(property_id);
        return value && !value->equals(property_initial_value(property_id));
    };
    bool has_transition_definition = !retained_transition_candidates.is_empty()
        || transition_property_differs_from_initial(PropertyID::TransitionProperty)
        || transition_property_differs_from_initial(PropertyID::TransitionDuration)
        || transition_property_differs_from_initial(PropertyID::TransitionTimingFunction)
        || transition_property_differs_from_initial(PropertyID::TransitionDelay)
        || transition_property_differs_from_initial(PropertyID::TransitionBehavior);
    if (previous_computed_values && has_transition_definition) {
        auto transition_property_changed = [&](PropertyID property_id) {
            auto cascaded_value = cascaded_properties->property(property_id);
            auto previous_value = previous_computed_values->computed_style_value(property_id, ComputedValues::WithAnimationsApplied::No);
            VERIFY(previous_value);
            if (cascaded_value)
                return !cascaded_value->equals(*previous_value);
            return !property_initial_value(property_id)->equals(*previous_value);
        };
        transition_definition_changed = transition_property_changed(PropertyID::TransitionProperty)
            || transition_property_changed(PropertyID::TransitionDuration)
            || transition_property_changed(PropertyID::TransitionTimingFunction)
            || transition_property_changed(PropertyID::TransitionDelay)
            || transition_property_changed(PropertyID::TransitionBehavior);
    }
    auto font_family = cascaded_properties->property(PropertyID::FontFamily);
    bool has_monospace_font_size_recascade = font_family && ComputedValuesFFI::rust_font_family_is_monospace(font_family->rust_style_value_data());
    bool must_compute_all_properties = !previous_computed_values
        || has_monospace_font_size_recascade
        || element.has_relevant_animations()
        || element.has_css_defined_animations()
        || transition_definition_changed;
    if (must_compute_all_properties)
        computed_group_mask = ComputedValues::all_style_groups;
    else if (!retained_transition_candidates.is_empty())
        computed_group_mask = ComputedValues::all_style_groups;
    u64 const* computed_properties_to_evaluate = nullptr;
    if (!must_compute_all_properties
        && sharing->computed_properties_to_evaluate.has_value()
        && (computed_group_mask != ComputedValues::all_style_groups || !retained_transition_candidates.is_empty())) {
        auto property_is_selected = [&](PropertyID property_id) {
            auto index = to_underlying(property_id) - to_underlying(first_longhand_property_id);
            return ((*sharing->computed_properties_to_evaluate)[index / 64] & (1ull << (index % 64))) != 0;
        };
        auto select_property = [&](PropertyID property_id) {
            auto index = to_underlying(property_id) - to_underlying(first_longhand_property_id);
            (*sharing->computed_properties_to_evaluate)[index / 64] |= 1ull << (index % 64);
        };
        for (auto property_id : element.property_ids_with_matching_transition_property_entry(abstract_element.pseudo_element()))
            select_property(property_id);
        for (auto property_id : element.property_ids_with_existing_transitions(abstract_element.pseudo_element()))
            select_property(property_id);
        for (auto property_id : retained_transition_candidates)
            select_property(property_id);
        Array<bool, to_underlying(LogicalPropertyGroup::Size) + 1> selected_logical_groups {};
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto property_id = static_cast<PropertyID>(i);
            auto group = logical_property_group_for_property(property_id);
            if (group.has_value() && logical_property_group_has_computed_closure(*group) && property_is_selected(property_id))
                selected_logical_groups[to_underlying(*group)] = true;
        }
        if (selected_logical_groups[to_underlying(LogicalPropertyGroup::BorderStyle)]
            || selected_logical_groups[to_underlying(LogicalPropertyGroup::BorderWidth)]) {
            selected_logical_groups[to_underlying(LogicalPropertyGroup::BorderStyle)] = true;
            selected_logical_groups[to_underlying(LogicalPropertyGroup::BorderWidth)] = true;
        }
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto property_id = static_cast<PropertyID>(i);
            auto group = logical_property_group_for_property(property_id);
            if (group.has_value() && selected_logical_groups[to_underlying(*group)])
                select_property(property_id);
        }
        static auto const independent_property_closure = [] {
            Array<u64, (number_of_longhand_properties + 63) / 64> properties {};
            auto add = [&](PropertyID property_id) {
                auto index = to_underlying(property_id) - to_underlying(first_longhand_property_id);
                properties[index / 64] |= 1ull << (index % 64);
            };
            add(PropertyID::AspectRatio);
            add(PropertyID::BackdropFilter);
            add(PropertyID::BackgroundColor);
            add(PropertyID::BoxShadow);
            add(PropertyID::ClipPath);
            add(PropertyID::Filter);
            add(PropertyID::Isolation);
            add(PropertyID::MixBlendMode);
            for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
                auto property_id = static_cast<PropertyID>(i);
                auto group = logical_property_group_for_property(property_id);
                if (group.has_value() && logical_property_group_has_computed_closure(*group))
                    add(property_id);
            }
            add(PropertyID::ObjectFit);
            add(PropertyID::ObjectPosition);
            add(PropertyID::Opacity);
            add(PropertyID::Perspective);
            add(PropertyID::PerspectiveOrigin);
            add(PropertyID::Rotate);
            add(PropertyID::Scale);
            add(PropertyID::Transform);
            add(PropertyID::TransformOrigin);
            add(PropertyID::Translate);
            add(PropertyID::Visibility);
            add(PropertyID::WillChange);
            add(PropertyID::ZIndex);
            return properties;
        }();
        bool only_independent_properties_changed = true;
        for (size_t i = 0; i < independent_property_closure.size(); ++i) {
            if ((*sharing->computed_properties_to_evaluate)[i] & ~independent_property_closure[i]) {
                only_independent_properties_changed = false;
                break;
            }
        }
        if (sharing->computed_property_closure_is_exact || only_independent_properties_changed)
            computed_properties_to_evaluate = sharing->computed_properties_to_evaluate->data();
    }
    auto computed_properties = compute_properties(abstract_element, cascaded_properties, cascade_input.matching_pseudo_element_styles,
        sharing ? &sharing->explicitly_inherited_non_inherited_property : nullptr, previous_computed_values, computed_group_mask, computed_properties_to_evaluate, inheritance_parent_values);
    if (new_style_input_record)
        new_style_input_record->bind_next_published_style = true;
    static bool const verify_computed_closure = getenv("LIBWEB_VERIFY_COMPUTED_CLOSURE") != nullptr;
    if (verify_computed_closure && computed_group_mask != ComputedValues::all_style_groups) {
        auto custom_property_data = abstract_element.custom_property_data();
        auto counters = document().style_invalidation_counters();
        auto fully_computed_properties = compute_properties(
            abstract_element, cascaded_properties, cascade_input.matching_pseudo_element_styles,
            nullptr, nullptr, ComputedValues::all_style_groups, nullptr, inheritance_parent_values, true);
        auto partially_computed_values = build_computed_values(*computed_properties, abstract_element, style_scope);
        auto fully_computed_values = build_computed_values(*fully_computed_properties, abstract_element, style_scope);
        document().style_invalidation_counters() = counters;
        abstract_element.set_custom_property_data(move(custom_property_data));
        auto partially_computed_longhands = partially_computed_values->computed_longhand_values();
        auto fully_computed_longhands = fully_computed_values->computed_longhand_values();
        VERIFY(partially_computed_longhands.size() == number_of_longhand_properties);
        VERIFY(fully_computed_longhands.size() == number_of_longhand_properties);
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto property_id = static_cast<PropertyID>(i);
            auto index = i - to_underlying(first_longhand_property_id);
            auto const* partial_value = static_cast<StyleValueFFI::StyleValueData const*>(partially_computed_longhands[index]);
            auto const* full_value = static_cast<StyleValueFFI::StyleValueData const*>(fully_computed_longhands[index]);
            VERIFY(partial_value);
            VERIFY(full_value);
            bool values_are_equal = partial_value == full_value || StyleValueFFI::rust_style_value_equals(partial_value, full_value);
            if (!values_are_equal) {
                auto partial_wrapper = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(partial_value));
                auto full_wrapper = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(full_value));
                dbgln("computed closure differs on {}: partial {}, full {} (element {} id={})", string_from_property_id(property_id), partial_wrapper->to_string(SerializationMode::Normal), full_wrapper->to_string(SerializationMode::Normal), abstract_element.element().tag_name(), abstract_element.element().id().value_or(Utf16FlyString {}));
            }
            VERIFY(values_are_equal);
            if (partially_computed_values->is_property_important(property_id) != fully_computed_values->is_property_important(property_id))
                dbgln("computed closure importance differs on {}: partial {}, full {}", string_from_property_id(property_id), partially_computed_values->is_property_important(property_id), fully_computed_values->is_property_important(property_id));
            VERIFY(partially_computed_values->is_property_important(property_id) == fully_computed_values->is_property_important(property_id));
            if (partially_computed_values->is_property_inherited(property_id) != fully_computed_values->is_property_inherited(property_id))
                dbgln("computed closure inheritance differs on {}: partial {}, full {}", string_from_property_id(property_id), partially_computed_values->is_property_inherited(property_id), fully_computed_values->is_property_inherited(property_id));
            VERIFY(partially_computed_values->is_property_inherited(property_id) == fully_computed_values->is_property_inherited(property_id));
        }
    }
    // The environment is compared against the one this computation replaced, which is the resolved
    // one: the cascade's own comparison sees the values before substitution, so a computation whose
    // `var()`s resolve to what they resolved to last time is only recognisable here.
    abstract_element.set_custom_property_data(custom_property_data_keeping_identity(document(), old_custom_property_data, abstract_element.custom_property_data()));

    report_custom_property_change(abstract_element, old_custom_property_data, did_change_custom_properties);

    return computed_properties;
}

static bool is_monospace(StyleValue const& value)
{
    return ComputedValuesFFI::rust_font_family_is_monospace(value.rust_style_value_data());
}

// HACK: This function implements time-travelling inheritance for the font-size property
//       in situations where the cascade ended up with `font-family: monospace`.
//       In such cases, other browsers will magically change the meaning of keyword font sizes
//       *even in earlier stages of the cascade!!* to be relative to the default monospace font size (13px)
//       instead of the default font size (16px).
//       See this blog post for a lot more details about this weirdness:
//       https://manishearth.github.io/blog/2017/08/10/font-size-an-unexpectedly-complex-css-property/
RefPtr<StyleValue const> StyleComputer::recascade_font_size_if_needed(DOM::AbstractElement abstract_element, CascadedProperties& cascaded_properties, bool& depends_on_viewport_metrics) const
{
    // Check for `font-family: monospace`. Note that `font-family: monospace, AnythingElse` does not trigger this path.
    // Some CSS frameworks use `font-family: monospace, monospace` to work around this behavior.
    auto font_family_value = cascaded_properties.property(CSS::PropertyID::FontFamily);
    if (!font_family_value || !is_monospace(*font_family_value))
        return nullptr;

    // FIXME: This should be configurable.
    constexpr CSSPixels default_monospace_font_size_in_px = 13;
    static auto const& monospace_font_family_name = *new String(Platform::FontPlugin::the().generic_font_name(Platform::GenericFont::Monospace, 400, 0));
    static auto const& monospace_font = Gfx::FontDatabase::the().get(monospace_font_family_name, default_monospace_font_size_in_px * 0.75f, 400, Gfx::FontWidth::Normal, 0).release_nonnull().leak_ref();

    // Reconstruct the line of ancestor elements we need to inherit style from, and then do the cascade again
    // but only for the font-size property.
    GC::ConservativeVector<DOM::AbstractElement> ancestors;
    for (auto ancestor = abstract_element.element_to_inherit_style_from(); ancestor.has_value(); ancestor = ancestor->element_to_inherit_style_from())
        ancestors.append(*ancestor);

    NonnullRefPtr<StyleValue const> new_font_size = CSS::LengthStyleValue::create(CSS::Length::make_px(default_monospace_font_size_in_px));
    CSSPixels current_size_in_px = default_monospace_font_size_in_px;
    bool current_size_depends_on_viewport_metrics = false;

    for (auto& ancestor : ancestors.in_reverse()) {
        auto ancestor_computed_values = ancestor.computed_style();
        if (!ancestor_computed_values)
            continue;
        auto font_size_value = ancestor_computed_values->raw_cascaded_font_size();

        if (!font_size_value)
            continue;

        // The per-value step lives in the Rust style computation core. A length value needs a
        // resolution context, which is built lazily on request since it involves font work the
        // other value types never need.
        auto step = ComputedValuesFFI::rust_recascade_font_size_step(
            font_size_value->rust_style_value_data(),
            current_size_in_px.raw_value(),
            current_size_depends_on_viewport_metrics,
            default_monospace_font_size_in_px.raw_value(),
            nullptr);

        if (step.action == ComputedValuesFFI::FontSizeRecascadeAction::NeedsLengthResolution) {
            bool inherited_font_metrics_depend_on_viewport_metrics = false;
            auto inherited_line_height = ancestor.element_to_inherit_style_from()
                                             .map([&](auto&& parent_element) {
                                                 auto parent_style = parent_element.computed_style();
                                                 inherited_font_metrics_depend_on_viewport_metrics = parent_style->font_metrics_depend_on_viewport_metrics();
                                                 return parent_style->line_height();
                                             })
                                             .value_or(InitialValues::line_height());

            bool did_resolve_viewport_relative_length = false;
            Length::ResolutionContext resolution_context {
                .viewport_rect = viewport_rect(),
                .font_metrics = { current_size_in_px, monospace_font.with_size(current_size_in_px * 0.75f)->pixel_metrics(), inherited_line_height },
                .root_font_metrics = m_root_element_font_metrics,
                .font_metrics_depend_on_viewport_metrics = current_size_depends_on_viewport_metrics || inherited_font_metrics_depend_on_viewport_metrics,
                .root_font_metrics_depend_on_viewport_metrics = m_root_element_font_metrics_depend_on_viewport_metrics,
                .subject_inline_axis_is_horizontal = ancestor.computed_style()->writing_mode() == WritingMode::HorizontalTb,
                .subject_element = &ancestor.element(),
            };
            auto ffi_resolution_context = to_ffi_length_resolution_context(resolution_context);
            step = ComputedValuesFFI::rust_recascade_font_size_step(
                font_size_value->rust_style_value_data(),
                current_size_in_px.raw_value(),
                current_size_depends_on_viewport_metrics,
                default_monospace_font_size_in_px.raw_value(),
                &ffi_resolution_context);

            if (step.action == ComputedValuesFFI::FontSizeRecascadeAction::NeedsLengthResolution) {
                // A length unit the core cannot resolve; resolve it here instead.
                VERIFY(font_size_value->is_length());
                resolution_context.set_did_resolve_viewport_relative_length(did_resolve_viewport_relative_length);
                current_size_in_px = font_size_value->as_length().length().to_px(resolution_context);
                current_size_depends_on_viewport_metrics = did_resolve_viewport_relative_length;
                continue;
            }
        }

        switch (step.action) {
        case ComputedValuesFFI::FontSizeRecascadeAction::Unchanged:
            break;
        case ComputedValuesFFI::FontSizeRecascadeAction::Set:
            current_size_in_px = CSSPixels::from_raw(step.new_size_raw);
            current_size_depends_on_viewport_metrics = step.depends_on_viewport_metrics;
            break;
        case ComputedValuesFFI::FontSizeRecascadeAction::CalcSkipped:
            dbgln("FIXME: Support calc() when time-traveling for monospace font-size");
            break;
        case ComputedValuesFFI::FontSizeRecascadeAction::NeedsLengthResolution:
            VERIFY_NOT_REACHED();
        }
    };

    depends_on_viewport_metrics = current_size_depends_on_viewport_metrics;
    return CSS::LengthStyleValue::create(CSS::Length::make_px(current_size_in_px));
}

void StyleComputer::ensure_style_metadata_tables_installed()
{
    // Marshal the generated logical-alias-to-physical mapping into the Rust style
    // computation core as a flat table, so the core uses exactly the C++ mapping.
    static bool const installed = [] {
        constexpr size_t writing_mode_count = 5;
        constexpr size_t direction_count = 2;
        Vector<u16> table;
        table.resize(number_of_longhand_properties * writing_mode_count * direction_count);
        size_t index = 0;
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto property_id = static_cast<PropertyID>(i);
            for (size_t writing_mode = 0; writing_mode < writing_mode_count; ++writing_mode) {
                for (size_t direction = 0; direction < direction_count; ++direction) {
                    u16 physical = 0;
                    if (property_is_logical_alias(property_id))
                        physical = to_underlying(map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { static_cast<WritingMode>(writing_mode), static_cast<Direction>(direction) }));
                    table[index++] = physical;
                }
            }
        }
        ComputedValuesFFI::rust_style_metadata_set_logical_alias_table(table.data(), table.size());

        Vector<u16> reverse_table;
        reverse_table.resize(number_of_longhand_properties * writing_mode_count * direction_count);
        index = 0;
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto property_id = static_cast<PropertyID>(i);
            for (size_t writing_mode = 0; writing_mode < writing_mode_count; ++writing_mode) {
                for (size_t direction = 0; direction < direction_count; ++direction) {
                    u16 logical = 0;
                    if (!property_is_logical_alias(property_id)) {
                        auto mapped = map_physical_property_to_logical_alias(property_id, LogicalAliasMappingContext { static_cast<WritingMode>(writing_mode), static_cast<Direction>(direction) });
                        if (mapped != property_id)
                            logical = to_underlying(mapped);
                    }
                    reverse_table[index++] = logical;
                }
            }
        }
        ComputedValuesFFI::rust_style_metadata_set_physical_to_logical_table(reverse_table.data(), reverse_table.size());

        // Transfer one shared Rust reference for every longhand initial value, so
        // initial-value selection never crosses the FFI.
        Vector<void const*> initial_value_entries;
        initial_value_entries.ensure_capacity(number_of_longhand_properties);
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto initial_value = property_initial_value(static_cast<PropertyID>(i));
            initial_value_entries.unchecked_append(StyleValueFFI::rust_style_value_retain(initial_value->rust_style_value_data()));
        }
        ComputedValuesFFI::rust_style_metadata_set_initial_value_table(initial_value_entries.data(), initial_value_entries.size());

        // Mark the color keywords, so the core knows which keyword values resolve to
        // something other than themselves at computed-value time.
        Vector<u64> color_keyword_words;
        color_keyword_words.resize((number_of_keywords + 63) / 64);
        for (size_t keyword = 0; keyword < number_of_keywords; ++keyword) {
            if (KeywordStyleValue::is_color(static_cast<Keyword>(keyword)))
                color_keyword_words[keyword / 64] |= 1ull << (keyword % 64);
        }
        ComputedValuesFFI::rust_style_metadata_set_color_keyword_bitmap(color_keyword_words.data(), color_keyword_words.size());
        return true;
    }();
    (void)installed;
}

NonnullRefPtr<ComputedStyleWorkingSet> StyleComputer::compute_properties(DOM::AbstractElement abstract_element, CascadedProperties& cascaded_properties, u64 matching_pseudo_element_styles, bool* explicitly_inherited_non_inherited_property, ComputedValues const* previous_values, u32 computed_group_mask, u64 const* computed_properties_to_evaluate, ComputedValues const* inheritance_parent_values, bool stop_after_longhand_drive) const
{
    ensure_style_metadata_tables_installed();
    VERIFY(computation_context_cache_is_empty());

    bool rebuilds_over_previous_properties = computed_group_mask != ComputedValues::all_style_groups || computed_properties_to_evaluate;
    auto working_set = rebuilds_over_previous_properties
        ? CSS::ComputedStyleWorkingSet::create_with_base_values_from(*previous_values)
        : CSS::ComputedStyleWorkingSet::create();
    if (rebuilds_over_previous_properties)
        working_set->clear_in_display_none_subtree();
    auto& computed_style = *working_set;
    computed_style.set_has_pseudo_element_styles(matching_pseudo_element_styles);

    bool recascaded_font_size_depends_on_viewport_metrics = false;
    auto new_font_size = recascade_font_size_if_needed(abstract_element, cascaded_properties, recascaded_font_size_depends_on_viewport_metrics);
    if (new_font_size) {
        computed_style.set_property(PropertyID::FontSize, *new_font_size, ComputedStyleWorkingSet::Inherited::No, Important::No);
        if (recascaded_font_size_depends_on_viewport_metrics) {
            computed_style.set_depends_on_viewport_metrics();
            computed_style.set_font_metrics_depend_on_viewport_metrics();
        }
    }

    auto inheritance_parent = abstract_element.element_to_inherit_style_from();
    auto computed_values_to_inherit_from_view = !inheritance_parent_values && inheritance_parent.has_value() ? inheritance_parent->computed_style() : ComputedStyleRecordView {};
    auto const* computed_values_to_inherit_from = inheritance_parent_values ? inheritance_parent_values : computed_values_to_inherit_from_view ? &*computed_values_to_inherit_from_view
                                                                                                                                               : nullptr;

    auto const device_pixels_per_css_pixel = m_document->page().client().device_pixels_per_css_pixel();

    Vector<u8> document_supported_color_scheme_codes;
    auto document_supported_color_schemes = document().supported_color_schemes();
    if (document_supported_color_schemes.has_value()) {
        document_supported_color_scheme_codes.ensure_capacity(document_supported_color_schemes->size());
        for (auto const& scheme : *document_supported_color_schemes)
            document_supported_color_scheme_codes.unchecked_append(to_underlying(preferred_color_scheme_from_string(scheme)));
    }
    ComputedValuesFFI::FfiEffectiveColorSchemeInput const effective_color_scheme_input {
        .preferred_color_scheme = static_cast<u8>(to_underlying(document().page().preferred_color_scheme())),
        .has_document_supported_schemes = document_supported_color_schemes.has_value(),
        .document_supported_scheme_codes = document_supported_color_scheme_codes.data(),
        .document_supported_scheme_count = document_supported_color_scheme_codes.size(),
    };
    computed_style.clear_effective_color_scheme();

    // The parent's computed values, handed to the driver as the parent style's own longhand
    // table span, so the inherit path never crosses the FFI and pins nothing: the span and
    // the sparse currentcolor-dependent specified-value overrides are owned by the parent's
    // style (or the engine's interned record data behind a borrowed view), which outlives
    // the drive. Inheritance reads base values (WithAnimationsApplied::No).
    Vector<u16, 8> parent_override_properties;
    Vector<void const*, 8> parent_override_values;
    Optional<ComputedValuesFFI::FfiParentSnapshot> parent_snapshot;
    if (computed_values_to_inherit_from) {
        auto const& parent_base = computed_values_to_inherit_from->base_values();
        auto parent_longhand_values = parent_base.computed_longhand_values();
        VERIFY(!parent_longhand_values.is_empty());
        // computed_style_value_for_inheritance's preference, replicated on raw handles: a
        // recorded specified value that depends on currentColor wins over the stored computed
        // value. The owned map is consulted before the borrowed record entries, so it comes
        // first in the override list the driver scans in order.
        for (auto const& entry : parent_base.inheritance_dependent_specified_values()) {
            if (!entry.value->depends_on_current_color())
                continue;
            parent_override_properties.append(to_underlying(entry.key));
            parent_override_values.append(entry.value->rust_style_value_data());
        }
        for (auto const& entry : parent_base.borrowed_inheritance_dependent_values()) {
            if (!StyleValueFFI::rust_style_value_depends_on_current_color(static_cast<StyleValueFFI::StyleValueData const*>(entry.value)))
                continue;
            parent_override_properties.append(entry.property);
            parent_override_values.append(entry.value);
        }
        parent_snapshot = ComputedValuesFFI::FfiParentSnapshot {
            .table_values = parent_longhand_values.data(),
            .table_value_count = parent_longhand_values.size(),
            .override_properties = parent_override_properties.data(),
            .override_values = parent_override_values.data(),
            .override_count = parent_override_properties.size(),
            .font_metrics_depend_on_viewport_metrics = computed_values_to_inherit_from->font_metrics_depend_on_viewport_metrics(),
        };
    }

    bool animation_values_applied = false;
    Vector<GC::Ref<Animations::KeyframeEffect>> newly_started_transition_effects;

    // Copies the parent's animated value when a longhand inherits, as its store is
    // applied, so later properties' computation contexts observe the animated value.
    // FIXME: Do we need to recompute animated inherited values?
    auto copy_animated_inherited_value = [&](PropertyID property_id, PropertyID inherited_property_id) {
        if (auto const* animated_properties = computed_values_to_inherit_from->animated_properties(); animated_properties && animated_properties->has_property(inherited_property_id)) {
            auto animated_value = animated_properties->values().get(inherited_property_id);
            VERIFY(animated_value.has_value());
            computed_style.set_animated_property(
                Badge<StyleComputer> {},
                property_id,
                *animated_value.value(),
                animated_properties->is_property_result_of_transition(inherited_property_id)
                    ? AnimatedPropertyResultOfTransition::Yes
                    : AnimatedPropertyResultOfTransition::No,
                ComputedStyleWorkingSet::Inherited::Yes);
            animation_values_applied = true;
        }
    };

    Optional<ComputedValuesFFI::FfiDisplay> display_before_adjustments;
    Optional<Keyword> float_before_adjustments;
    Optional<Keyword> overflow_x_before_adjustments;
    Optional<Keyword> overflow_y_before_adjustments;
    Optional<Keyword> text_align_before_adjustments;
    Optional<Keyword> position_before_adjustments;
    RefPtr<StyleValue const> line_height_before_adjustments;

    auto execute_computation_batch = [&](ComputedValuesFFI::FfiComputedStoreEntry const* entries, size_t count, i16 effective_color_scheme) {
        for (size_t i = 0; i < count; ++i) {
            auto const& entry = entries[i];
            auto property_id = static_cast<PropertyID>(entry.property_id);
            auto inherited_property_id = static_cast<PropertyID>(entry.inherited_property_id);
            i64 const style_sheet_source_slot = entry.source_slot >= 0 && entry.has_style_sheet_context ? entry.source_slot : -1;
            GC::Ptr<CSSStyleSheet> style_sheet;
            if (style_sheet_source_slot >= 0) {
                if (auto source = cascaded_properties.source_for_slot(static_cast<u32>(entry.source_slot)); source && source->parent_rule())
                    style_sheet = source->parent_rule()->parent_style_sheet();
            }
            if (entry.inherited)
                copy_animated_inherited_value(property_id, inherited_property_id);
            switch (entry.computed_kind) {
            case ComputedValuesFFI::COMPUTED_KIND_UNCHANGED:
                computed_style.did_store_property_data_from_drive(property_id, style_sheet);
                break;
            case ComputedValuesFFI::COMPUTED_KIND_PX_LENGTH:
            case ComputedValuesFFI::COMPUTED_KIND_INTEGER:
            case ComputedValuesFFI::COMPUTED_KIND_NUMBER:
            case ComputedValuesFFI::COMPUTED_KIND_PERCENTAGE:
            case ComputedValuesFFI::COMPUTED_KIND_FONT_STYLE:
            case ComputedValuesFFI::COMPUTED_KIND_KEYWORD:
            case ComputedValuesFFI::COMPUTED_KIND_DISPLAY:
            case ComputedValuesFFI::COMPUTED_KIND_SUPERELLIPSE:
            case ComputedValuesFFI::COMPUTED_KIND_STYLE_VALUE:
                computed_style.did_store_property_data_from_drive(property_id, nullptr);
                break;
            }
            if (property_id == PropertyID::ColorScheme && !computed_style.has_effective_color_scheme()) {
                auto effective_color_scheme = ComputedValuesFFI::rust_resolve_effective_color_scheme(computed_style.effective_property_data(PropertyID::ColorScheme), &effective_color_scheme_input);
                computed_style.set_effective_color_scheme(static_cast<PreferredColorScheme>(effective_color_scheme));
            }
        }
        if (effective_color_scheme >= 0)
            computed_style.set_effective_color_scheme(static_cast<PreferredColorScheme>(effective_color_scheme));
    };

    ComputedValuesFFI::FfiLonghandDriverResults driver_results {
        .longhand_evaluations = 0,
        .raw_cascaded_font_size_data = nullptr,
        .depends_on_viewport_metrics = false,
        .font_metrics_depend_on_viewport_metrics = false,
        .explicitly_inherited_non_inherited_property = false,
        .uses_tree_counting_function = false,
        .effective_color_scheme = -1,
        .post_compute_adjustment = {},
    };
    auto box_type_input = make_box_type_transformation_input(
        abstract_element, InitialValues::display(), Keyword::Static, Keyword::None);
    Optional<DOM::AbstractElement::TreeCountingFunctionResolutionContext> tree_counting_context;
    if (ComputedValuesFFI::rust_cascaded_properties_uses_tree_counting_function(cascaded_properties.rust_store()))
        tree_counting_context = abstract_element.tree_counting_function_resolution_context();
    auto const container_relative_length_unit_mask = ComputedValuesFFI::rust_cascaded_properties_container_relative_length_unit_mask(cascaded_properties.rust_store());
    auto unfixed_random_sharings = ComputedValuesFFI::rust_cascaded_properties_unfixed_random_sharings(cascaded_properties.rust_store());
    ScopeGuard release_unfixed_random_sharings = [&] {
        ComputedValuesFFI::rust_cascaded_properties_unfixed_random_sharings_release(unfixed_random_sharings.storage, unfixed_random_sharings.entry_count);
    };
    Vector<ComputedValuesFFI::FfiRandomBaseValue> random_base_values;
    random_base_values.ensure_capacity(unfixed_random_sharings.entry_count);
    for (auto const& sharing : ReadonlySpan<ComputedValuesFFI::FfiUnfixedRandomSharing> { unfixed_random_sharings.entries, unfixed_random_sharings.entry_count }) {
        VERIFY(sharing.name != 0);
        RandomCachingKey random_caching_key {
            .name = Utf16FlyString::from_raw(sharing.name),
            .element_id = sharing.element_shared
                ? Optional<UniqueNodeID> { OptionalNone {} }
                : Optional<UniqueNodeID> { abstract_element.element().unique_id() },
        };
        random_base_values.empend(sharing.source, const_cast<DOM::Element&>(abstract_element.element()).ensure_css_random_base_value(random_caching_key));
    }
    auto const environment_requirements = ComputedValuesFFI::rust_cascaded_properties_environment_requirements(cascaded_properties.rust_store());
    Vector<String> style_sheet_base_urls;
    Vector<ComputedValuesFFI::FfiStyleSheetResourceContext> style_sheet_resource_contexts;
    if (environment_requirements & ComputedValuesFFI::CASCADED_ENVIRONMENT_NEEDS_STYLE_SHEET_CONTEXT) {
        style_sheet_base_urls.resize(cascaded_properties.source_slot_count());
        style_sheet_resource_contexts.resize(cascaded_properties.source_slot_count());
        for (size_t slot = 0; slot < cascaded_properties.source_slot_count(); ++slot) {
            auto& resource_context = style_sheet_resource_contexts[slot];
            auto source = cascaded_properties.source_for_slot(static_cast<u32>(slot));
            if (!source || !source->parent_rule())
                continue;
            auto style_sheet = source->parent_rule()->parent_style_sheet();
            if (!style_sheet)
                continue;
            auto base_url = style_sheet->base_url()
                                .value_or_lazy_evaluated_optional([&]() { return style_sheet->location(); })
                                .value_or_lazy_evaluated_optional([&]() -> Optional<::URL::URL> {
                                    if (auto document = style_sheet->owning_document())
                                        return HTML::relevant_settings_object(*document).api_base_url();
                                    return {};
                                });
            if (base_url.has_value())
                style_sheet_base_urls[slot] = base_url->to_string();
            resource_context.has_value = true;
            resource_context.origin_clean = style_sheet->is_origin_clean();
        }
        for (size_t slot = 0; slot < style_sheet_resource_contexts.size(); ++slot) {
            auto bytes = style_sheet_base_urls[slot].bytes();
            style_sheet_resource_contexts[slot].base_url = bytes.data();
            style_sheet_resource_contexts[slot].base_url_length = bytes.size();
        }
    }
    String document_base_url;
    if (environment_requirements & ComputedValuesFFI::CASCADED_ENVIRONMENT_NEEDS_DOCUMENT_BASE_URL)
        document_base_url = abstract_element.document().base_url().to_string();
    auto document_base_url_bytes = document_base_url.bytes();
    ComputedValuesFFI::FfiStyleComputationEnvironment const computation_environment {
        .box_type_input = box_type_input,
        .color_scheme_input = effective_color_scheme_input,
        .is_th_element = abstract_element.element().local_name() == HTML::TagNames::th,
        .has_new_font_size = new_font_size != nullptr,
        .has_animated_inheritance_parent = computed_values_to_inherit_from && computed_values_to_inherit_from->animated_properties(),
        .has_tree_counting_context = tree_counting_context.has_value(),
        .sibling_count = tree_counting_context.has_value() ? static_cast<u64>(tree_counting_context->sibling_count) : 0,
        .sibling_index = tree_counting_context.has_value() ? static_cast<u64>(tree_counting_context->sibling_index) : 0,
        .random_base_values = random_base_values.data(),
        .random_base_value_count = random_base_values.size(),
        .document_base_url = document_base_url_bytes.data(),
        .document_base_url_length = document_base_url_bytes.size(),
        .style_sheet_resource_contexts = style_sheet_resource_contexts.data(),
        .style_sheet_resource_context_count = style_sheet_resource_contexts.size(),
        .device_pixels_per_css_pixel = device_pixels_per_css_pixel,
        .initial_font_size_raw = InitialValues::font_size().raw_value(),
        .default_font_size_raw = default_user_font_size().raw_value(),
    };
    auto drive_longhand_phase = [&](u8 phase, Optional<PropertyID> context_property) {
        Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_resolution_context;
        if (context_property.has_value()) {
            auto const& computation_context = get_computation_context_for_property(*context_property, computed_style, abstract_element);
            length_resolution_context = to_ffi_length_resolution_context_with_container_bases(computation_context.length_resolution_context, container_relative_length_unit_mask);
        }
        auto store_batch = ComputedValuesFFI::rust_drive_property_computation(computed_style.mutable_computed_longhand_table(), cascaded_properties.rust_store(), parent_snapshot.has_value() ? &*parent_snapshot : nullptr, &computation_environment, computed_group_mask, computed_properties_to_evaluate, phase, length_resolution_context.has_value() ? &*length_resolution_context : nullptr, &driver_results);
        ScopeGuard destroy_store_batch = [&] {
            ComputedValuesFFI::rust_longhand_store_batch_destroy(store_batch.storage);
        };
        execute_computation_batch(store_batch.entries, store_batch.count, store_batch.effective_color_scheme);
    };
    auto apply_font_metric_dependencies = [&] {
        if (driver_results.depends_on_viewport_metrics)
            computed_style.set_depends_on_viewport_metrics();
        if (driver_results.font_metrics_depend_on_viewport_metrics)
            computed_style.set_font_metrics_depend_on_viewport_metrics();
    };
    drive_longhand_phase(ComputedValuesFFI::LONGHAND_DRIVE_PHASE_FONT, PropertyID::FontFamily);
    apply_font_metric_dependencies();
    drive_longhand_phase(ComputedValuesFFI::LONGHAND_DRIVE_PHASE_LINE_HEIGHT, PropertyID::LineHeight);
    apply_font_metric_dependencies();
    drive_longhand_phase(ComputedValuesFFI::LONGHAND_DRIVE_PHASE_COLOR_SCHEME, {});
    drive_longhand_phase(ComputedValuesFFI::LONGHAND_DRIVE_PHASE_REMAINING, PropertyID::Color);
    auto const& post_compute_adjustment = driver_results.post_compute_adjustment;
    display_before_adjustments = post_compute_adjustment.display_before;
    float_before_adjustments = static_cast<Keyword>(post_compute_adjustment.float_before);
    overflow_x_before_adjustments = static_cast<Keyword>(post_compute_adjustment.overflow_x_before);
    overflow_y_before_adjustments = static_cast<Keyword>(post_compute_adjustment.overflow_y_before);
    text_align_before_adjustments = static_cast<Keyword>(post_compute_adjustment.text_align_before);
    position_before_adjustments = static_cast<Keyword>(post_compute_adjustment.position_before);
    line_height_before_adjustments = computed_style.property(PropertyID::LineHeight);
    computed_style.set_display_before_box_type_transformation(display_from_ffi_display(post_compute_adjustment.display_before));
    auto line_height_metrics = input_line_height_metrics(computed_style, abstract_element, post_compute_adjustment.element_style_adjustment.check_input_line_height);
    auto post_adjusted_longhands = ComputedValuesFFI::rust_apply_post_compute_adjustments(computed_style.mutable_computed_longhand_table(), &post_compute_adjustment, &line_height_metrics);
    document().style_invalidation_counters().computed_longhand_evaluations += driver_results.longhand_evaluations;
    if (driver_results.uses_tree_counting_function)
        abstract_element.element().set_style_uses_tree_counting_function();

    auto invalidate_post_adjusted_longhand = [&](u8 flag, PropertyID property_id) {
        if (post_adjusted_longhands & flag)
            computed_style.did_store_property_data_from_drive(property_id, nullptr);
    };
    invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_FLOAT, PropertyID::Float);
    invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_DISPLAY, PropertyID::Display);
    invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_LINE_HEIGHT, PropertyID::LineHeight);
    invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_POSITION, PropertyID::Position);
    invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_TEXT_ALIGN, PropertyID::TextAlign);

    // Store the raw winning cascaded font-size. This is needed to implement the time-traveling inheritance for
    // font-size when font-family is monospace.
    // See the recascade_font_size_if_needed() function for further details.
    if (driver_results.raw_cascaded_font_size_data)
        computed_style.set_raw_cascaded_font_size(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
            static_cast<StyleValueFFI::StyleValueData const*>(driver_results.raw_cascaded_font_size_data))));
    if (driver_results.depends_on_viewport_metrics)
        computed_style.set_depends_on_viewport_metrics();
    if (driver_results.font_metrics_depend_on_viewport_metrics)
        computed_style.set_font_metrics_depend_on_viewport_metrics();
    if (stop_after_longhand_drive) {
        clear_computation_context_caches();
        computed_style.freeze_computed_longhand_table();
        return working_set;
    }
    // A child normally reads nothing but the inherited half of what it inherits from, which is what
    // lets a change confined to non-inherited properties stop above it. `inherit` on a non-inherited
    // property is the exception, so the node it was read from remembers that its children are not
    // bounded that way.
    if (driver_results.explicitly_inherited_non_inherited_property) {
        if (auto* parent = abstract_element.element().parent())
            parent->set_children_may_depend_on_non_inherited_property_inheritance();
        if (explicitly_inherited_non_inherited_property)
            *explicitly_inherited_non_inherited_property = true;
    }

    if (is<HTML::HTMLHtmlElement>(abstract_element.element())) {
        m_root_element_font_metrics = calculate_root_element_font_metrics(computed_style);
        m_root_element_font_metrics_depend_on_viewport_metrics = computed_style.font_metrics_depend_on_viewport_metrics();
    }

    // Compute the value of custom properties
    compute_custom_properties(computed_style, abstract_element);

    clear_computation_context_caches();

    // Add or modify CSS-defined animations
    process_animation_definitions(computed_style, cascaded_properties, abstract_element);

    auto restore_values_before_post_compute_adjustments = [&] {
        VERIFY(display_before_adjustments.has_value());
        VERIFY(float_before_adjustments.has_value());
        VERIFY(overflow_x_before_adjustments.has_value());
        VERIFY(overflow_y_before_adjustments.has_value());
        VERIFY(text_align_before_adjustments.has_value());
        VERIFY(position_before_adjustments.has_value());
        VERIFY(line_height_before_adjustments);
        computed_style.set_property_without_modifying_flags(PropertyID::Display, DisplayStyleValue::create(display_from_ffi_display(*display_before_adjustments)));
        computed_style.set_property_without_modifying_flags(PropertyID::Float, KeywordStyleValue::create(*float_before_adjustments));
        computed_style.set_property_without_modifying_flags(PropertyID::OverflowX, KeywordStyleValue::create(*overflow_x_before_adjustments));
        computed_style.set_property_without_modifying_flags(PropertyID::OverflowY, KeywordStyleValue::create(*overflow_y_before_adjustments));
        computed_style.set_property_without_modifying_flags(PropertyID::TextAlign, KeywordStyleValue::create(*text_align_before_adjustments));
        computed_style.set_property_without_modifying_flags(PropertyID::Position, KeywordStyleValue::create(*position_before_adjustments));
        computed_style.set_property_without_modifying_flags(PropertyID::LineHeight, *line_height_before_adjustments);
    };
    if (animation_values_applied)
        restore_values_before_post_compute_adjustments();

    m_keyframes_inherited_non_inherited_property = false;
    auto animations = abstract_element.element().get_animations_internal(
        Animations::Animatable::GetAnimationsSorted::Yes,
        Animations::Animatable::GetAnimationsOptions { .subtree = false, .pseudo_element = {} });
    if (animations.is_exception()) {
        dbgln("Error getting animations for element {}", abstract_element.debug_description());
    } else {
        GC::RootVector<GC::Ref<Animations::KeyframeEffect>> effects;
        for (auto& animation : animations.value()) {
            if (auto effect = animation->effect(); effect && effect->is_keyframe_effect()) {
                auto& keyframe_effect = *static_cast<Animations::KeyframeEffect*>(effect.ptr());
                auto was_just_started = newly_started_transition_effects.contains_slow(GC::Ref { keyframe_effect });
                if (!was_just_started && keyframe_effect.pseudo_element_type() == abstract_element.pseudo_element())
                    effects.append(keyframe_effect);
            }
        }
        if (!effects.is_empty()) {
            if (!animation_values_applied)
                restore_values_before_post_compute_adjustments();
            animation_values_applied = true;
            collect_animations_into(abstract_element, effects.span(), computed_style, AnimationRefresh::No);
        }
    }

    bool parent_text_align_input_is_animated = false;
    if (computed_values_to_inherit_from) {
        if (auto const* animated_properties = computed_values_to_inherit_from->animated_properties()) {
            parent_text_align_input_is_animated = animated_properties->has_property(PropertyID::TextAlign)
                || animated_properties->has_property(PropertyID::Direction);
        }
    }
    if (parent_text_align_input_is_animated && !animation_values_applied) {
        VERIFY(text_align_before_adjustments.has_value());
        computed_style.set_property_without_modifying_flags(PropertyID::TextAlign, KeywordStyleValue::create(*text_align_before_adjustments));
    }

    // Run automatic box type transformations again after animations have been applied.
    if (animation_values_applied)
        apply_post_compute_adjustments(computed_style, abstract_element);
    else if (parent_text_align_input_is_animated)
        compute_text_align(computed_style, abstract_element);

    bool parent_style_in_display_none_subtree = false;
    if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value()) {
        if (auto parent_style = parent->computed_style())
            parent_style_in_display_none_subtree = parent_style->in_display_none_subtree();
    }

    // Transition declarations [css-transitions-1]
    // Theoretically this should be part of the cascade, but it works with computed values, which we don't have until now.
    compute_transitioned_properties(computed_style, abstract_element);
    if (auto previous_style = abstract_element.computed_style()) {
        // https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
        // In Level 1 of this specification, transitions can only start during a style change event for elements which
        // have a defined before-change style established by the previous style change event. That means a transition
        // could not be started on an element that was not being rendered for the previous style change event.
        // FIXME: If an element does not have a before-change style for a given style change event, the starting style
        //        is used instead of the before-change style to compare with the after-change style to start
        //        transitions.
        if (!previous_style->in_display_none_subtree() && !parent_style_in_display_none_subtree)
            newly_started_transition_effects = start_needed_transitions(*previous_style, computed_style, abstract_element);
    }

    // Newly-created transitions were evaluated while they were started. Keep them
    // out of the general animation pass below so that a style change crosses the
    // Rust animation boundary only once per effect.
    animation_values_applied |= !newly_started_transition_effects.is_empty();

    if (m_keyframes_inherited_non_inherited_property) {
        if (auto* parent = abstract_element.element().parent())
            parent->set_children_may_depend_on_non_inherited_property_inheritance();
        if (explicitly_inherited_non_inherited_property)
            *explicitly_inherited_non_inherited_property = true;
        m_keyframes_inherited_non_inherited_property = false;
    }

    if (parent_style_in_display_none_subtree || computed_style.display().is_none())
        computed_style.set_in_display_none_subtree();

    computed_style.freeze_computed_longhand_table();
    return working_set;
}

static NonnullRefPtr<StyleValue const> resolve_css_wide_keyword_for_custom_property(Optional<CustomPropertyRegistration const&> registration, AbstractOrHypotheticalElement const& element, Utf16FlyString const& name, NonnullRefPtr<StyleValue const> keyword_value, ComputedStyleWorkingSet const* computed_style_for_custom_property_resolution)
{
    VERIFY(keyword_value->is_css_wide_keyword());

    // https://drafts.csswg.org/css-mixins/#resolve-function-styles
    // On result, all CSS-wide keywords are left unresolved.
    if (name == "result"_utf16_fly_string)
        return keyword_value;

    if (keyword_value->is_initial())
        return initial_custom_property_value(registration, element.document());

    if (keyword_value->is_inherit())
        return inherited_custom_property_value(registration, element, name, computed_style_for_custom_property_resolution);

    // https://drafts.csswg.org/css-mixins/#resolve-function-styles
    // NB: When resolving function styles (i.e. when we have a hypothetical element), all CSS-wide keywords other than
    //     inherit and initial resolve to the guaranteed-invalid value.
    if (element.has<HypotheticalElement*>())
        return GuaranteedInvalidStyleValue::create();

    // Unset is the same as inherit for inherited properties, and by default all unregistered custom properties inherit.
    if (keyword_value->is_unset())
        return registration.has_value() && !registration->inherit
            ? initial_custom_property_value(registration, element.document())
            : inherited_custom_property_value(registration, element, name, computed_style_for_custom_property_resolution);

    if (keyword_value->is_revert()) {
        // FIXME: Implement reverting custom properties.
        return keyword_value;
    }
    if (keyword_value->is_revert_layer()) {
        // FIXME: Implement reverting custom properties.
        return keyword_value;
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> StyleComputer::resolve_unresolved_style_value(AbstractOrHypotheticalElement element, PropertyNameAndID const& property, UnresolvedStyleValue const& unresolved) const
{
    auto& dom_element = element.abstract_element().element();
    if (unresolved.includes_var_function())
        dom_element.set_style_uses_var_css_function();
    if (unresolved.includes_attr_function())
        dom_element.set_style_uses_attr_css_function();
    if (unresolved.includes_if_function())
        dom_element.set_style_uses_if_css_function();
    if (unresolved.includes_inherit_function())
        dom_element.set_style_uses_inherit_css_function();
    if (unresolved.includes_dashed_function())
        dom_element.set_style_uses_custom_function();
    auto const& reference_scan = custom_property_reference_scan(NonnullRefPtr<StyleValue const> { unresolved });
    for (auto const& name : reference_scan.references)
        dom_element.record_style_custom_property_reference(name);

    auto& document = element.document();
    SubstitutionData substitution_data { element, true, true };

    auto custom_property_data = element.custom_property_data();
    auto inheritance_data = element.element_to_inherit_style_from().map([](auto const& parent) {
                                                                       return parent.custom_property_data();
                                                                   })
                                .value_or(nullptr);
    ComputedValuesFFI::FfiCascadeResolutionContext resolution_context {
        .parse_context = &substitution_data.parse_context,
        .custom_property_store = custom_property_data ? custom_property_data->rust_store() : nullptr,
        .inheritance_custom_property_store = inheritance_data ? inheritance_data->rust_store() : nullptr,
        .custom_property_registry = document.rust_custom_property_registry(),
        .root_custom_property_name = property.is_custom_property() ? ffi_utf16_view(property.name()) : ComputedValuesFFI::FfiUtf16View {},
        .attributes = substitution_data.ffi_attributes.data(),
        .attribute_count = substitution_data.ffi_attributes.size(),
        .attribute_names_are_ascii_case_insensitive = element.abstract_element().element().namespace_uri() == Namespace::HTML && document.is_html_document(),
        .custom_functions = substitution_data.ffi_functions.data(),
        .custom_function_count = substitution_data.ffi_functions.size(),
        .custom_function_scope_identity = bit_cast<FlatPtr>(&element.style_scope()),
        .callback_context = &element,
        .resolve_custom_function = resolve_custom_function_for_substitution,
        .evaluate_condition = [](void* context, u8 kind, ComputedValuesFFI::FfiUtf16View source) -> u8 {
            auto& element = *static_cast<AbstractOrHypotheticalElement*>(context);
            return evaluate_condition_for_substitution(element, kind, source);
        },
        .lookup_final_custom_property = [](void* context, ComputedValuesFFI::FfiUtf16View name) -> void const* {
            auto& element = *static_cast<AbstractOrHypotheticalElement*>(context);
            auto& document = element.document();
            auto& style_computer = document.style_computer();
            auto name_view = utf16_view(name);
            if (style_computer.m_active_custom_property_resolution.has_value()
                && element.has<DOM::AbstractElement>()
                && style_computer.m_active_custom_property_resolution->element == element.get<DOM::AbstractElement>()) {
                if (auto finalized = style_computer.m_active_custom_property_resolution->finalized.get(Utf16FlyString::from_utf16(name_view)); finalized.has_value()) {
                    document.style_invalidation_counters().custom_property_overlay_hits++;
                    return finalized.value()->rust_style_value_data();
                }
                document.style_invalidation_counters().custom_property_value_computations++;
            }
            return nullptr;
        },
        .note_substitution = nullptr,
        .lookup_cached_substitution = nullptr,
        .cache_parsed_substitution = nullptr,
    };
    auto* result = ComputedValuesFFI::rust_resolve_unresolved_style_value(
        &resolution_context, to_underlying(property.id()), unresolved.rust_style_value_data());
    VERIFY(result);
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(result));
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_value_of_custom_property(ComputedStyleWorkingSet const* computed_style_for_custom_property_resolution, AbstractOrHypotheticalElement const& element, Utf16FlyString const& name) const
{
    // https://drafts.csswg.org/css-variables/#propdef-
    // The computed value of a custom property is its specified value with any arbitrary-substitution functions replaced.
    // FIXME: These should probably be part of the computed style itself.
    auto& document = element.document();

    // While this element's own properties resolve in dependency order, everything a value can name is final before
    // the value resolves, so a nested lookup reads the finished answer instead of resolving its value again.
    if (m_active_custom_property_resolution.has_value() && element.has<DOM::AbstractElement>()
        && element.get<DOM::AbstractElement>() == m_active_custom_property_resolution->element) {
        if (auto finalized = m_active_custom_property_resolution->finalized.get(name); finalized.has_value()) {
            document.style_invalidation_counters().custom_property_overlay_hits++;
            return *finalized.value();
        }
    }

    document.style_invalidation_counters().custom_property_value_computations++;
    auto registration = element.get_registered_custom_property(name);

    auto value = element.get_custom_property(name);
    auto resolved_value = value ? value.release_nonnull() : initial_custom_property_value(registration, document);

    if (resolved_value->is_css_wide_keyword())
        resolved_value = resolve_css_wide_keyword_for_custom_property(registration, element, name, move(resolved_value), computed_style_for_custom_property_resolution);

    if (resolved_value->is_unresolved() && resolved_value->as_unresolved().contains_arbitrary_substitution_function()) {
        auto& unresolved = resolved_value->as_unresolved();
        resolved_value = resolve_unresolved_style_value(element, PropertyNameAndID { {}, PropertyID::Custom, name }, unresolved);

        // A CSS-wide keyword produced by substitution takes on that keyword's meaning for the custom property,
        // exactly as a literally-specified one would (handled above before substitution).
        if (resolved_value->is_css_wide_keyword())
            resolved_value = resolve_css_wide_keyword_for_custom_property(registration, element, name, move(resolved_value), computed_style_for_custom_property_resolution);
    }

    auto invalid_custom_property_fallback_value = [&](NonnullRefPtr<StyleValue const> invalid_value) -> NonnullRefPtr<StyleValue const> {
        // https://drafts.csswg.org/css-values-5/#invalid-substitution
        // When property replacement results in a property’s value containing the guaranteed-invalid value, this makes
        // the declaration invalid at computed-value time. When this happens, the computed value is one of the
        // following depending on the property’s type:

        // -> The property is a non-registered custom property
        // -> The property is a registered custom property with universal syntax
        if (!registration.has_value() || registration->syntax->type() == Parser::SyntaxNode::NodeType::Universal) {
            // The computed value is the guaranteed-invalid value.
            return invalid_value;
        }

        // -> Otherwise
        {
            // Either the property’s inherited value or its initial value depending on whether the property is
            // inherited or not, respectively, as if the property’s value had been specified as the unset keyword.

            // https://drafts.csswg.org/css-mixins/#resolve-function-styles
            // NB: When resolving function styles (i.e. when we have a hypothetical element), all CSS-wide keywords other than
            //     inherit and initial (including unset) resolve to the guaranteed-invalid value.
            if (element.has<HypotheticalElement*>())
                return invalid_value;

            if (registration->inherit)
                return inherited_custom_property_value(registration, element, name, computed_style_for_custom_property_resolution);
            return initial_custom_property_value(registration, element.document());
        }
    };

    if (resolved_value->is_guaranteed_invalid())
        return invalid_custom_property_fallback_value(move(resolved_value));

    if (!registration.has_value() || registration->syntax->type() == Parser::SyntaxNode::NodeType::Universal)
        return resolved_value;

    auto resolved_value_contains_attr_tainted_values = resolved_value->is_unresolved() && resolved_value->as_unresolved().contains_attr_tainted_values();
    auto parsed_value = [&]() -> NonnullRefPtr<StyleValue const> {
        auto registration_generation = document.custom_property_registration_generation();
        auto& parses = m_registered_custom_property_parses.ensure(resolved_value->rust_style_value_data());
        for (auto const& parse : parses) {
            if (parse.syntax_identity == registration->syntax.ptr() && parse.registration_generation == registration_generation)
                return parse.parsed;
        }
        auto parsing_params = Parser::ParsingParams { document };
        parsing_params.value_context.append(PropertyID::Custom);
        auto source = resolved_value->is_unresolved()
            ? resolved_value->as_unresolved().token_source()
            : resolved_value->to_utf16_string(SerializationMode::ResolvedValueForReparse);
        auto parsed = Parser::parse_with_a_syntax(parsing_params, source, registration->syntax);
        parses.append({ resolved_value, registration->syntax.ptr(), registration_generation, parsed });
        return parsed;
    }();
    if (parsed_value->is_guaranteed_invalid())
        return invalid_custom_property_fallback_value(move(parsed_value));

    auto computed_value = [&] {
        // FIXME: At the moment we incorrectly apply ASF replacement at cascade time when we should instead be applying
        //        it at computed-value time. This means we may not yet have a computed style for us to absolutize
        //        against. For now we just return the parsed value as-is and rely on the consuming property to
        //        absolutize it later.
        if (!computed_style_for_custom_property_resolution)
            return parsed_value;

        return compute_registered_custom_property_value(registration.value(), move(parsed_value), get_computation_context_for_property(PropertyID::Custom, *computed_style_for_custom_property_resolution, element.abstract_element()));
    }();

    if (resolved_value_contains_attr_tainted_values) {
        VERIFY(!computed_value->is_unresolved());
        return UnresolvedStyleValue::create_attr_tainted_with_parsed_value(computed_value->is_unresolved()
                ? computed_value->as_unresolved().token_source()
                : computed_value->to_utf16_string(SerializationMode::ResolvedValueForReparse),
            {}, {}, UnresolvedStyleValue::SourceTextMode::Trim, computed_value);
    }

    return computed_value;
}

ComputationContext StyleComputer::fallback_computation_context_for_custom_property(AbstractOrHypotheticalElement const& element) const
{
    auto abstract_element = element.abstract_element();

    auto context_from_computed_values = [&](DOM::AbstractElement const& styled_element) -> ComputationContext {
        auto length_resolution_context = Length::ResolutionContext::for_element(styled_element);
        length_resolution_context.subject_element = &abstract_element.element();
        return {
            .length_resolution_context = move(length_resolution_context),
            .abstract_element = abstract_element,
            .color_scheme = styled_element.computed_style()->color_scheme(),
        };
    };

    if (abstract_element.has_style())
        return context_from_computed_values(abstract_element);

    if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value() && parent->has_style())
        return context_from_computed_values(*parent);

    auto length_resolution_context = Length::ResolutionContext::for_document(document());
    length_resolution_context.subject_element = &abstract_element.element();
    return {
        .length_resolution_context = length_resolution_context,
        .abstract_element = abstract_element,
    };
}

StyleComputer::CustomPropertyReferenceScan const& StyleComputer::custom_property_reference_scan(NonnullRefPtr<StyleValue const> const& value) const
{
    return m_custom_property_reference_scans.ensure(value->rust_style_value_data(), [&] {
        CustomPropertyReferenceScan scan { .value = value, .references = {}, .all_references_visible = true };
        auto visit = [](void* context, u16 const* name, size_t name_length) {
            auto& references = *static_cast<Vector<Utf16FlyString>*>(context);
            references.append(Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(name), name_length }));
        };
        scan.all_references_visible = StyleValueFFI::rust_unresolved_style_value_visit_custom_property_references(
            value->rust_style_value_data(), &scan.references, visit);
        return scan;
    });
}

// Tarjan's strongly-connected-components algorithm, iterative because reference chains are as deep as a stylesheet
// wants. Components come out in dependency order: every component a member references outside its own is emitted
// before it.
static Vector<Vector<u32>> strongly_connected_components_in_dependency_order(Vector<Vector<u32>> const& edges)
{
    auto const node_count = edges.size();
    constexpr u32 unvisited = NumericLimits<u32>::max();
    Vector<u32> discovery_index;
    discovery_index.resize(node_count);
    discovery_index.fill(unvisited);
    Vector<u32> lowlink;
    lowlink.resize(node_count);
    Vector<bool> on_stack;
    on_stack.resize(node_count);
    Vector<u32> component_stack;
    Vector<Vector<u32>> components;
    u32 next_discovery_index = 0;

    struct Frame {
        u32 node;
        u32 next_edge;
    };
    Vector<Frame> walk_stack;

    for (u32 root = 0; root < node_count; ++root) {
        if (discovery_index[root] != unvisited)
            continue;
        walk_stack.append({ root, 0 });
        while (!walk_stack.is_empty()) {
            auto& frame = walk_stack.last();
            auto node = frame.node;
            if (frame.next_edge == 0) {
                discovery_index[node] = lowlink[node] = next_discovery_index++;
                component_stack.append(node);
                on_stack[node] = true;
            }
            bool descended = false;
            while (frame.next_edge < edges[node].size()) {
                auto target = edges[node][frame.next_edge++];
                if (discovery_index[target] == unvisited) {
                    walk_stack.append({ target, 0 });
                    descended = true;
                    break;
                }
                if (on_stack[target])
                    lowlink[node] = min(lowlink[node], discovery_index[target]);
            }
            if (descended)
                continue;
            if (lowlink[node] == discovery_index[node]) {
                Vector<u32> component;
                u32 popped = 0;
                do {
                    popped = component_stack.take_last();
                    on_stack[popped] = false;
                    component.append(popped);
                } while (popped != node);
                components.append(move(component));
            }
            walk_stack.take_last();
            if (!walk_stack.is_empty())
                lowlink[walk_stack.last().node] = min(lowlink[walk_stack.last().node], lowlink[node]);
        }
    }
    return components;
}

void StyleComputer::compute_custom_properties(ComputedStyleWorkingSet& computed_style, DOM::AbstractElement abstract_element) const
{
    // https://drafts.csswg.org/css-variables/#propdef-
    // The computed value of a custom property is its specified value with any arbitrary-substitution functions replaced.
    // FIXME: These should probably be part of the computed style itself.
    auto data = abstract_element.custom_property_data();
    if (!data)
        return;

    // If this element is sharing its parent's data (no own custom properties),
    // the parent has already resolved its values, so there's nothing to do.
    auto inherit_from = abstract_element.element_to_inherit_style_from();
    if (inherit_from.has_value() && inherit_from->custom_property_data().ptr() == data.ptr())
        return;

    if (data->declared_count() == 0)
        return;

    // What this environment resolves to is decided by the values it holds and by the environment it
    // inherits from, and those two are its identity. So the answer is kept on the environment
    // rather than worked out again for each element handed the same one, which on a page whose
    // components declare a theme is most of them.
    auto document_identity = bit_cast<FlatPtr>(&document());
    auto registration_generation = document().custom_property_registration_generation();
    auto color_scheme = computed_style.color_scheme(document().page().preferred_color_scheme(), document().supported_color_schemes());
    if (auto cached = data->cached_resolution(document_identity, registration_generation, color_scheme)) {
        abstract_element.set_custom_property_data(move(cached));
        return;
    }

    // Resolve var() references and only keep values that differ from parent.
    // This avoids growing the hashmap to full size and then shrinking it,
    // which would leave an oversized bucket array.
    RefPtr<CustomPropertyData const> parent_data;
    if (inherit_from.has_value())
        parent_data = inheritable_custom_property_data(*inherit_from);

    document().style_invalidation_counters().custom_property_elements++;
    document().style_invalidation_counters().custom_property_resolutions += data->declared_count();

    // Resolving a value that names another of this element's own properties walks into that value and resolves it,
    // and the walk repeats for every value naming it. Deciding an order first makes each own value resolve once:
    // Tarjan's algorithm over the scanned references emits every component after the components it depends on, so by
    // the time a value resolves, everything it can name is final and a nested lookup reads the finished answer.
    //
    // A reference cycle is the one thing that must not read finished answers: a member handed one would take a var()
    // fallback where the specification makes the whole cycle invalid. So a component's members resolve the
    // independent way, with the substitution guards deciding what is cyclic, and their answers are published only
    // once the whole component is done. A value whose references are not in its tokens - attr() substitutes
    // attribute text that can name custom properties, if() reads them through style() queries, a custom function's
    // body reads what it likes, and a var() name slot can itself be substituted - is given an edge to every own
    // name, which puts it after everything it could read and in a component with everything that names it back.
    // The order only matters when some own value names another own value that itself needs resolving; a value naming
    // only inherited or plain own properties reads answers that are final before it resolves. That is most elements
    // on most pages, so decide first, from the cached scans alone, whether there is anything here to order.
    auto const& own_values = data->own_values();
    auto for_each_declared_value = [&](auto callback) {
        size_t declared = 0;
        for (auto const& own : own_values) {
            if (declared++ >= data->declared_count())
                break;
            callback(own.key, own.value);
        }
    };
    auto needs_resolution = [](StyleValue const& value) {
        return value.is_unresolved() && value.as_unresolved().contains_arbitrary_substitution_function();
    };
    bool has_own_reference = false;
    for_each_declared_value([&](auto const&, auto const& style_property) {
        auto const& value = *style_property.value;
        if (!needs_resolution(value))
            return;
        auto const& unresolved = value.as_unresolved();
        if (unresolved.includes_attr_function() || unresolved.includes_if_function() || unresolved.includes_dashed_function())
            return;
        auto const& scan = custom_property_reference_scan(style_property.value);
        if (!scan.all_references_visible)
            return;
        for (auto const& referenced_name : scan.references) {
            if (auto referenced = own_values.find(referenced_name); referenced != own_values.end() && needs_resolution(*referenced->value.value)) {
                has_own_reference = true;
                break;
            }
        }
    });

    OrderedHashMap<Utf16FlyString, StyleProperty> resolved_own;
    auto keep_resolved_value = [&](Utf16FlyString const& name, StyleProperty const& style_property, NonnullRefPtr<StyleValue const> resolved_value) {
        if (parent_data) {
            auto const* parent_property = parent_data->get(name);
            if (parent_property && resolved_value->equals(*parent_property->value))
                return;
        }
        resolved_own.set(name,
            StyleProperty {
                .important = style_property.important,
                .property_id = style_property.property_id,
                .value = move(resolved_value),
            });
    };

    if (!has_own_reference) {
        for_each_declared_value([&](auto const& name, auto const& style_property) {
            keep_resolved_value(name, style_property, compute_value_of_custom_property(&computed_style, abstract_element, name));
        });
    } else {
        struct OwnCustomProperty {
            Utf16FlyString name;
            StyleProperty const* property;
        };
        Vector<OwnCustomProperty> own;
        own.ensure_capacity(data->declared_count());
        HashMap<Utf16FlyString, u32> own_index;
        for_each_declared_value([&](auto const& name, auto const& style_property) {
            own_index.set(name, static_cast<u32>(own.size()));
            own.append({ name, &style_property });
        });

        Vector<Vector<u32>> references;
        references.resize(own.size());
        for (size_t i = 0; i < own.size(); ++i) {
            auto const& value = *own[i].property->value;
            if (!value.is_unresolved())
                continue;
            auto const& unresolved = value.as_unresolved();
            if (!unresolved.contains_arbitrary_substitution_function())
                continue;
            bool references_visible = !unresolved.includes_attr_function() && !unresolved.includes_if_function() && !unresolved.includes_dashed_function();
            if (references_visible) {
                auto const& scan = custom_property_reference_scan(own[i].property->value);
                references_visible = scan.all_references_visible;
                if (references_visible) {
                    for (auto const& referenced_name : scan.references) {
                        if (auto index = own_index.get(referenced_name); index.has_value())
                            references[i].append(*index);
                    }
                }
            }
            if (!references_visible) {
                references[i].clear_with_capacity();
                references[i].ensure_capacity(own.size());
                for (u32 target = 0; target < own.size(); ++target)
                    references[i].append(target);
            }
        }

        auto previous_active_resolution = move(m_active_custom_property_resolution);
        m_active_custom_property_resolution = ActiveCustomPropertyResolution { abstract_element, {} };
        ScopeGuard restore_active_resolution { [&] { m_active_custom_property_resolution = move(previous_active_resolution); } };

        Vector<RefPtr<StyleValue const>> resolved_values;
        resolved_values.resize(own.size());
        for (auto& component : strongly_connected_components_in_dependency_order(references)) {
            if (component.size() > 1 || references[component[0]].contains_slow(component[0]))
                document().style_invalidation_counters().custom_property_cycle_participants += component.size();
            quick_sort(component);
            for (auto member : component)
                resolved_values[member] = compute_value_of_custom_property(&computed_style, abstract_element, own[member].name);
            for (auto member : component)
                m_active_custom_property_resolution->finalized.set(own[member].name, *resolved_values[member]);
        }

        for (size_t i = 0; i < own.size(); ++i)
            keep_resolved_value(own[i].name, *own[i].property, resolved_values[i].release_nonnull());
    }

    // What a resolution is allowed to read beyond the environment says whether the answer belongs to
    // the environment or to this element: `attr()` reads the element's attributes, `if()` its
    // surroundings, a custom function whatever it likes. Each says so on the element, and an
    // element that read one keeps its answer to itself.
    auto& element = abstract_element.element();
    auto resolution_read_only_the_environment = !element.style_uses_attr_css_function()
        && !element.style_uses_if_css_function()
        && !element.style_uses_custom_function()
        && !element.style_uses_tree_counting_function();

    if (resolved_own.is_empty() && parent_data) {
        if (resolution_read_only_the_environment)
            data->set_cached_resolution(document_identity, registration_generation, color_scheme, parent_data);
        abstract_element.set_custom_property_data(parent_data);
        return;
    }

    // FIXME: We should update in place so that non-recomputed children aren't left pointing at stale data
    auto resolved = intern_custom_property_data(
        CustomPropertyData::create(move(resolved_own), parent_data ? move(parent_data) : data->parent()));
    if (resolution_read_only_the_environment)
        data->set_cached_resolution(document_identity, registration_generation, color_scheme, resolved);
    abstract_element.set_custom_property_data(move(resolved));
}

// https://www.w3.org/TR/css-values-4/#snap-a-length-as-a-border-width
static CSSPixels snap_a_length_as_a_border_width(double device_pixels_per_css_pixel, CSSPixels length)
{
    // 1. Assert: len is non-negative.
    VERIFY(length >= 0);

    // 2. If len is an integer number of device pixels, do nothing.
    auto device_pixels = length.to_double() * device_pixels_per_css_pixel;
    if (device_pixels == trunc(device_pixels))
        return length;

    // 3. If len is greater than zero, but less than 1 device pixel, round len up to 1 device pixel.
    if (device_pixels > 0 && device_pixels < 1)
        return CSSPixels::nearest_value_for(1 / device_pixels_per_css_pixel);

    // 4. If len is greater than 1 device pixel, round it down to the nearest integer number of device pixels.
    if (device_pixels > 1)
        return CSSPixels::nearest_value_for(floor(device_pixels) / device_pixels_per_css_pixel);

    return length;
}

static NonnullRefPtr<StyleValue const> compute_style_value_list(NonnullRefPtr<StyleValue const> const& style_value, Function<NonnullRefPtr<StyleValue const>(NonnullRefPtr<StyleValue const> const&)> const& compute_entry)
{
    StyleValueVector computed_entries;

    for (auto const& entry : style_value->as_value_list().values())
        computed_entries.append(compute_entry(entry));

    return StyleValueList::create(move(computed_entries), StyleValueList::Separator::Comma);
}

static NonnullRefPtr<StyleValue const> compute_svg_number_as_length(NonnullRefPtr<StyleValue const> const& style_value)
{
    if (!style_value->is_number())
        return style_value;
    return LengthStyleValue::create(Length::make_px(style_value->as_number().number()));
}

// https://drafts.csswg.org/css-contain-2/#contain-property
static NonnullRefPtr<StyleValue const> collapse_containment_list(NonnullRefPtr<StyleValue const> const& style_value)
{
    auto const* containment_list = as_if<StyleValueList>(*style_value);
    if (!containment_list)
        return style_value;

    bool contains_size = false;
    bool contains_layout = false;
    bool contains_style = false;
    bool contains_paint = false;

    for (auto const& containment : containment_list->values()) {
        switch (containment->to_keyword()) {
        case Keyword::Size:
            contains_size = true;
            break;
        case Keyword::Layout:
            contains_layout = true;
            break;
        case Keyword::Style:
            contains_style = true;
            break;
        case Keyword::Paint:
            contains_paint = true;
            break;
        default:
            return style_value;
        }
    }

    if (!contains_layout || !contains_style || !contains_paint)
        return style_value;

    return KeywordStyleValue::create(contains_size ? Keyword::Strict : Keyword::Content);
}

static NonnullRefPtr<StyleValue const> repeat_style_value_list_to_n_elements(NonnullRefPtr<StyleValue const> const& style_value, size_t n)
{
    auto const& value_list = style_value->as_value_list();

    if (value_list.size() == n)
        return style_value;

    StyleValueVector repeated_values;
    repeated_values.ensure_capacity(n);

    for (size_t i = 0; i < n; ++i)
        repeated_values.unchecked_append(value_list.value_at(i, true));

    return StyleValueList::create(move(repeated_values), value_list.separator());
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_value_of_property(
    PropertyID property_id,
    NonnullRefPtr<StyleValue const> const& specified_value,
    Function<NonnullRefPtr<StyleValue const>(PropertyID)> const& get_property_specified_value,
    ComputationContext const& computation_context,
    double device_pixels_per_css_pixel)
{
    auto const& absolutized_value = specified_value->absolutized(computation_context);

    auto inheritance_parent = [&]() {
        return computation_context.abstract_element
            .map([](auto const& abstract_element) { return abstract_element.element_to_inherit_style_from(); })
            .value_or(OptionalNone {});
    };

    switch (property_id) {
    case PropertyID::AnimationName:
        return compute_animation_name(absolutized_value);
    // NB: The background properties are coordinated at compute time rather than use time, unlike other coordinating list property groups
    case PropertyID::BackgroundAttachment:
    case PropertyID::BackgroundClip:
    case PropertyID::BackgroundOrigin:
    case PropertyID::BackgroundPositionX:
    case PropertyID::BackgroundPositionY:
    case PropertyID::BackgroundRepeat:
    case PropertyID::BackgroundSize:
        return repeat_style_value_list_to_n_elements(absolutized_value, get_property_specified_value(PropertyID::BackgroundImage)->as_value_list().size());
    case PropertyID::BorderBottomWidth:
    case PropertyID::BorderLeftWidth:
    case PropertyID::BorderRightWidth:
    case PropertyID::BorderTopWidth:
    case PropertyID::OutlineWidth:
        return compute_border_or_outline_width(absolutized_value, device_pixels_per_css_pixel);
    case PropertyID::BorderSpacing: {
        if (absolutized_value->is_value_list())
            return absolutized_value;
        return StyleValueList::create(StyleValueVector { absolutized_value, absolutized_value }, StyleValueList::Separator::Space);
    }
    case PropertyID::Contain:
        return collapse_containment_list(absolutized_value);
    case PropertyID::CornerBottomLeftShape:
    case PropertyID::CornerBottomRightShape:
    case PropertyID::CornerTopLeftShape:
    case PropertyID::CornerTopRightShape:
        return compute_corner_shape(absolutized_value);
    case PropertyID::FontSize: {
        auto parent = inheritance_parent();
        if (ComputedValuesFFI::rust_value_depends_on_inherited_info_for_property(absolutized_value->rust_style_value_data(), to_underlying(PropertyID::FontSize)) && parent.has_value()) {
            auto parent_values = parent->computed_style();
            if (parent_values && parent_values->font_metrics_depend_on_viewport_metrics())
                computation_context.length_resolution_context.record_viewport_relative_length_resolution();
        }
        return compute_font_size(absolutized_value, get_property_specified_value(PropertyID::MathDepth)->as_integer().integer(), parent);
    }
    case PropertyID::FontStyle:
        return compute_font_style(absolutized_value);
    case PropertyID::FontWeight:
        return compute_font_weight(absolutized_value, inheritance_parent());
    case PropertyID::FontWidth:
        return compute_font_width(absolutized_value);
    case PropertyID::FontFeatureSettings:
    case PropertyID::FontVariationSettings:
        return compute_font_feature_tag_value_list(absolutized_value);
    case PropertyID::LetterSpacing:
    case PropertyID::WordSpacing: {
        // The normal-keyword-to-zero-length rule lives in the Rust style computation core.
        auto result = ComputedValuesFFI::rust_compute_letter_or_word_spacing(absolutized_value->rust_style_value_data());
        if (result.unchanged)
            return absolutized_value;
        return LengthStyleValue::create(Length::make_px(result.value));
    }
    case PropertyID::LineHeight:
        return compute_line_height(absolutized_value, computation_context.length_resolution_context.font_metrics.font_size);
    case PropertyID::MathDepth:
        return compute_math_depth(absolutized_value, inheritance_parent());
    case PropertyID::StrokeDasharray:
        // https://svgwg.org/svg2-draft/painting.html#StrokeDasharrayProperty
        // as comma separated list of absolute lengths or percentages, numbers converted to
        // absolute lengths first, or keyword specified
        if (!absolutized_value->is_value_list())
            return absolutized_value;
        return compute_style_value_list(absolutized_value, [](NonnullRefPtr<StyleValue const> const& dash) {
            return compute_svg_number_as_length(dash);
        });
    case PropertyID::StrokeDashoffset:
    case PropertyID::StrokeWidth:
        // https://svgwg.org/svg2-draft/painting.html#StrokeWidth
        // an absolute length or percentage, numbers converted to absolute lengths first
        return compute_svg_number_as_length(absolutized_value);
    case PropertyID::TransformOrigin:
        return compute_transform_origin(absolutized_value);
    default:
        return absolutized_value;
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_animation_name(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // https://drafts.csswg.org/css-animations-1/#animation-name
    // list, each item either a case-sensitive css identifier or the keyword none

    return compute_style_value_list(absolutized_value, [](NonnullRefPtr<StyleValue const> const& entry) -> NonnullRefPtr<StyleValue const> {
        // none | <custom-ident>
        if (entry->to_keyword() == Keyword::None || entry->is_custom_ident())
            return entry;

        // <string>
        if (entry->is_string()) {
            auto const& string_value = entry->as_string().string_value();

            // AD-HOC: We shouldn't convert strings that aren't valid <custom-ident>s
            if (!is_valid_custom_ident(string_value, { { "none"sv } }))
                return entry;

            return CustomIdentStyleValue::create(entry->as_string().string_value());
        }

        VERIFY_NOT_REACHED();
    });
}

// https://drafts.csswg.org/css-fonts-4/#font-variation-settings-def
// https://drafts.csswg.org/css-fonts/#font-feature-settings-prop
NonnullRefPtr<StyleValue const> StyleComputer::compute_font_feature_tag_value_list(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // NB: The computation logic is the same for both font-feature-settings and font-variation-settings, first we
    //     deduplicate feature tags (with latter taking precedence), then we sort them in ascending order by code unit
    if (absolutized_value->is_keyword())
        return absolutized_value;

    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(
        ComputedValuesFFI::rust_compute_font_feature_settings(absolutized_value->rust_style_value_data())));
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_border_or_outline_width(NonnullRefPtr<StyleValue const> const& absolutized_value, double device_pixels_per_css_pixel)
{
    // The line-width keywords and the border-width snapping live in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_border_or_outline_width(absolutized_value->rust_style_value_data(), device_pixels_per_css_pixel);
    if (result.handled)
        return LengthStyleValue::create(Length::make_px(result.value));

    auto const absolute_length = Length::from_style_value(absolutized_value, {}).absolute_length_to_px();
    return LengthStyleValue::create(Length::make_px(snap_a_length_as_a_border_width(device_pixels_per_css_pixel, absolute_length)));
}

// https://drafts.csswg.org/css-borders-4/#propdef-corner-top-left-shape
NonnullRefPtr<StyleValue const> StyleComputer::compute_corner_shape(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // The keyword-to-superellipse mapping lives in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_corner_shape_parameter(absolutized_value->rust_style_value_data());
    VERIFY(result.handled);
    if (result.unchanged)
        return absolutized_value;

    // NB: The round value is cached since it is the initial value of the corner-*-shape properties.
    if (result.value == 1) {
        static auto const& cached_round_value = SuperellipseStyleValue::create(NumberStyleValue::create(1)).leak_ref();
        return cached_round_value;
    }
    return SuperellipseStyleValue::create(NumberStyleValue::create(result.value));
}
NonnullRefPtr<StyleValue const> StyleComputer::compute_font_size(NonnullRefPtr<StyleValue const> const& absolutized_value, int computed_math_depth, Optional<DOM::AbstractElement> const& inheritance_parent, CSSPixels initial_font_size)
{
    auto inherited_font_size = inheritance_parent.has_value() && inheritance_parent->has_style()
        ? inheritance_parent->computed_style()->font_size()
        : initial_font_size;

    auto inherited_math_depth = inheritance_parent.has_value() && inheritance_parent->has_style()
        ? inheritance_parent->computed_style()->math_depth()
        : InitialValues::math_depth();

    // The size keyword tables and the math scaling rules live in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_font_size(absolutized_value->rust_style_value_data(), computed_math_depth, inherited_font_size.raw_value(), inherited_math_depth, default_user_font_size().raw_value());
    if (result.handled) {
        if (result.unchanged)
            return absolutized_value;
        return LengthStyleValue::create(Length::make_px(result.value));
    }

    VERIFY(absolutized_value->is_calculated());
    return LengthStyleValue::create(absolutized_value->as_calculated().resolve_length({ .percentage_basis = Length::make_px(inherited_font_size) }).value());
}

// The FontStyleKeyword discriminants cross the boundary as the mapped keyword code; pin them.
static_assert(to_underlying(FontStyleKeyword::Normal) == 0);
static_assert(to_underlying(FontStyleKeyword::Italic) == 1);
static_assert(to_underlying(FontStyleKeyword::Left) == 2);
static_assert(to_underlying(FontStyleKeyword::Right) == 3);
static_assert(to_underlying(FontStyleKeyword::Oblique) == 4);

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_style(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // https://drafts.csswg.org/css-fonts-4/#font-style-prop
    // the keyword specified, plus angle in degrees if specified

    // The keyword-to-font-style-keyword mapping lives in the Rust style computation core.
    // NB: We always parse as a FontStyleStyleValue, but StylePropertyMap is able to set a KeywordStyleValue directly.
    auto computation = ComputedValuesFFI::rust_compute_font_style(absolutized_value->rust_style_value_data());
    if (computation.is_keyword)
        return FontStyleStyleValue::create(static_cast<FontStyleKeyword>(computation.font_style_keyword));

    return absolutized_value;
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_weight(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent)
{
    auto inherited_font_weight = inheritance_parent.has_value() && inheritance_parent->has_style()
        ? inheritance_parent->computed_style()->font_weight()
        : InitialValues::font_weight();

    // The weight chart lives in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_font_weight(absolutized_value->rust_style_value_data(), inherited_font_weight);
    if (result.handled) {
        if (result.unchanged)
            return absolutized_value;
        return NumberStyleValue::create(result.value);
    }

    // AD-HOC: Anywhere we support a numbers we should also support calcs
    VERIFY(absolutized_value->is_calculated());
    return NumberStyleValue::create(absolutized_value->as_calculated().resolve_number({}).value());
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_width(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // The width keyword percentage table lives in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_font_width(absolutized_value->rust_style_value_data());
    if (result.handled) {
        if (result.unchanged)
            return absolutized_value;
        return PercentageStyleValue::create(Percentage(result.value));
    }

    // AD-HOC: We support calculated percentages as well
    VERIFY(absolutized_value->is_calculated());
    return PercentageStyleValue::create(absolutized_value->as_calculated().resolve_percentage({}).value());
}
NonnullRefPtr<StyleValue const> StyleComputer::compute_line_height(NonnullRefPtr<StyleValue const> const& absolutized_value, CSSPixels computed_font_size)
{
    // The line-height rules live in the Rust style computation core, including calc
    // resolution against the computed font size.
    auto result = ComputedValuesFFI::rust_compute_line_height(absolutized_value->rust_style_value_data(), computed_font_size.raw_value());
    if (result.handled) {
        if (result.unchanged)
            return absolutized_value;
        if (result.is_number)
            return NumberStyleValue::create(result.value);
        return LengthStyleValue::create(Length::make_px(result.value));
    }

    // NOTE: We also support calc()'d lengths (percentages resolve to lengths so we don't have to handle them separately)
    if (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_length_percentage())
        return LengthStyleValue::create(absolutized_value->as_calculated().resolve_length({ .percentage_basis = Length::make_px(computed_font_size) }).value());

    // NOTE: We also support calc()'d numbers
    VERIFY(absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_number());
    return NumberStyleValue::create(absolutized_value->as_calculated().resolve_number({ .percentage_basis = Length::make_px(computed_font_size) }).value());
}

// https://w3c.github.io/mathml-core/#propdef-math-depth
NonnullRefPtr<StyleValue const> StyleComputer::compute_math_depth(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent)
{
    auto inherited_math_depth = inheritance_parent.has_value() && inheritance_parent->has_style()
        ? inheritance_parent->computed_style()->math_depth()
        : InitialValues::math_depth();

    auto inherited_math_style = inheritance_parent.has_value() && inheritance_parent->has_style()
        ? inheritance_parent->computed_style()->math_style()
        : InitialValues::math_style();

    // The math-depth rules live in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_math_depth(absolutized_value->rust_style_value_data(), inherited_math_depth, inherited_math_style == MathStyle::Compact);
    if (result.handled)
        return IntegerStyleValue::create(static_cast<i64>(result.value));

    // - If the specified value of math-depth is of the form add(<integer>) then the computed value of
    //   math-depth of the element is its inherited value plus the specified integer.
    if (absolutized_value->is_function())
        return IntegerStyleValue::create(AK::saturating_add(inherited_math_depth, int_from_style_value(absolutized_value->as_function().value())));

    VERIFY(absolutized_value->is_calculated());
    return IntegerStyleValue::create(int_from_style_value(absolutized_value));
}

// https://drafts.csswg.org/css-transforms/#transform-origin-property
NonnullRefPtr<StyleValue const> StyleComputer::compute_transform_origin(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    auto const* computed_value = ComputedValuesFFI::rust_compute_transform_origin(absolutized_value->rust_style_value_data());
    if (!computed_value)
        return absolutized_value;
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(computed_value));
}

}
