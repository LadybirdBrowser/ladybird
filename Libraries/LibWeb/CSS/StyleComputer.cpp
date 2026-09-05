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
#include <LibWeb/Animations/ScrollTimeline.h>
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
#include <LibWeb/CSS/ContainerQuery.h>
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
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
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
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
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

struct SubstitutionData {
    struct Attribute {
        Utf16String name;
        Utf16String value;
    };
    struct FunctionParameter {
        Utf16String name;
        void const* syntax { nullptr };
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
        void const* return_syntax { nullptr };
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
            .is_ua_style_sheet = false,
            .value_contexts = nullptr,
            .value_context_count = 0,
            .declared_namespaces = nullptr,
            .declared_namespace_count = 0,
            .document_url = document_url.bytes().data(),
            .document_url_length = document_url.bytes().size(),
            .document_base_url = document_base_url.bytes().data(),
            .document_base_url_length = document_base_url.bytes().size(),
            .intern_utf16_fly_string = retain_utf16_fly_string_for_substitution,
            .length_resolution_context = nullptr,
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
                    .return_syntax = definition.function->return_type_internal().data(),
                    .declarations = {},
                    .ffi_parameters = {},
                    .ffi_declarations = {},
                };
                snapshot.parameters.ensure_capacity(definition.function->parameters_internal().size());
                for (auto const& parameter : definition.function->parameters_internal()) {
                    snapshot.parameters.unchecked_append({
                        .name = parameter.name.to_utf16_string(),
                        .syntax = parameter.type.data(),
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
                    .syntax = parameter.syntax,
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
                .return_syntax = definition.return_syntax,
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

static u8 evaluate_style_query_for_substitution(AbstractOrHypotheticalElement element, ComputedValuesFFI::FfiUtf16View source)
{
    auto query = Parser::RustQueryParser::parse_style_query(utf16_view(source));
    if (!query.has_value())
        return 2;
    prepare_for_style_query_evaluation();
    auto matches = evaluate_style_query(*query, element) == MatchResult::True;
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
            if (entry.explicitly_inherited_non_inherited_style_groups != 0 && !!entry.parent_style_record_identity)
                unpin_style_record(entry.parent_style_record_identity);
            if (entry.style_record_identity.has_value())
                unpin_style_record(*entry.style_record_identity);
        }
    }
    m_style_sharing_cache.clear();
    m_style_sharing_donor_index.clear();
    m_style_sharing_cache_entry_count = 0;
}

void StyleComputer::prepare_for_style_engine_transaction() const
{
    ++m_style_sharing_transaction_generation;
    if (m_style_sharing_cache_entry_count > maximum_persistent_style_sharing_entries)
        clear_style_sharing_cache();
    m_style_engine_cascade_input_cache.clear();
    sweep_custom_property_environments();
}

void StyleComputer::begin_style_update() const
{
    ++m_style_update_depth;
}

void StyleComputer::end_style_update() const
{
    VERIFY(m_style_update_depth > 0);
    if (--m_style_update_depth != 0)
        return;
    m_style_update_ffi_media_environment.clear();
    m_style_update_media_environment.clear();
}

Parser::ValueParserFFI::FfiMediaEnvironment const* StyleComputer::cached_media_environment_for_style_update() const
{
    if (m_style_update_depth == 0)
        return nullptr;
    return m_style_update_ffi_media_environment.has_value() ? &*m_style_update_ffi_media_environment : nullptr;
}

Parser::ValueParserFFI::FfiMediaEnvironment const* StyleComputer::ensure_media_environment_for_style_update() const
{
    // NB: Outside a style update there is nothing to clear the cached snapshot, so always take a
    //     fresh one. Every style-computation entry point currently opens a scope, so this is a
    //     defensive path rather than one the engine relies on.
    if (m_style_update_depth == 0 || !m_style_update_media_environment.has_value()) {
        m_style_update_media_environment.emplace(m_document);
        m_style_update_ffi_media_environment = m_style_update_media_environment->ffi_environment();
    }
    return &*m_style_update_ffi_media_environment;
}

void StyleComputer::drop_style_sharing_cache() const
{
    clear_style_sharing_cache();
    m_style_engine_cascade_input_cache.clear();
    sweep_custom_property_environments();
}

ComputedStyleRecordView StyleComputer::computed_style_record_view(StyleRecordID style_record_identity) const
{
    if (!style_record_identity)
        return {};
    auto view = m_style_engine.style_record_view(style_record_identity);
    if (!view.present)
        return {};
    bool owns_style_record_pin = m_style_record_view_epoch_depth == 0 || view.animation_overlay_identity != 0;
    if (owns_style_record_pin) {
        pin_style_record(style_record_identity);
        ++m_computed_style_record_view_pin_count;
    }
    return ComputedStyleRecordView { view, *this, style_record_identity, owns_style_record_pin };
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

void StyleComputer::begin_style_record_view_epoch() const
{
    if (m_style_record_view_epoch_depth++ == 0)
        const_cast<StyleComputer&>(*this).m_style_engine.begin_style_record_view_epoch();
}

void StyleComputer::end_style_record_view_epoch() const
{
    VERIFY(m_style_record_view_epoch_depth > 0);
    if (--m_style_record_view_epoch_depth == 0)
        const_cast<StyleComputer&>(*this).m_style_engine.end_style_record_view_epoch();
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
        publish_animated_custom_properties(computed_properties, abstract_element);
        return;
    }
    m_keyframes_inherited_non_inherited_style_groups = 0;
    collect_animation_effects_into(abstract_element, effects, computed_properties);
    publish_animated_custom_properties(computed_properties, abstract_element);
    // An animation-only overlay update resolves keyframe values just like a full style computation does, so a
    // keyframe-borne `inherit` on a non-inherited property discovered here must leave the same invalidation
    // mark behind, or a later change to the parent's value never reaches this element's animated style.
    if (m_keyframes_inherited_non_inherited_style_groups != 0) {
        if (auto* parent = abstract_element.element().parent())
            parent->add_children_explicitly_inherited_non_inherited_style_groups(m_keyframes_inherited_non_inherited_style_groups);
        m_keyframes_inherited_non_inherited_style_groups = 0;
    }
    if (computed_properties.requires_animated_post_compute_adjustments()) {
        computed_properties.prepare_for_animated_post_compute_adjustments(Badge<StyleComputer> {});
        finalize_style(computed_properties, abstract_element, ComputedValuesFFI::FfiStyleFinalizationMode::AnimatedBoxType);
    }
}

void StyleComputer::collect_animation_effects_into(DOM::AbstractElement abstract_element, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects, ComputedStyleWorkingSet& computed_properties) const
{
    struct KeyframeDeclaration {
        size_t keyframe_index { 0 };
        PropertyNameAndID property;
        RustStyleValueHandle value;
        StyleValueFFI::FfiAnimationStyleSheetResourceContext style_sheet_resource_context {};
        bool use_initial { false };
        bool is_transition { false };
    };
    Vector<KeyframeDeclaration> keyframe_declarations;
    Vector<StyleValueFFI::FfiAnimationEffect> ffi_effects;
    Vector<StyleValueFFI::FfiAnimationKeyframe> ffi_keyframes;
    Vector<Vector<StyleValueFFI::FfiLinearEasingPoint>> linear_easing_points;

    auto to_ffi_composite_operation = [](Bindings::CompositeOperation operation) {
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
    auto to_ffi_easing = [](CSS::EasingFunction const& easing, Vector<StyleValueFFI::FfiLinearEasingPoint>& points) {
        return easing.visit(
            [&](LinearEasingFunction const& linear) {
                points.ensure_capacity(linear.control_points.size());
                for (auto const& point : linear.control_points)
                    points.unchecked_append({ .input = point.input, .output = point.output });
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
    };

    auto base_custom_property_data = [&]() -> RefPtr<CustomPropertyData const> {
        auto data = abstract_element.custom_property_data();
        if (data && data->is_animation_overlay())
            return data->parent();
        return data;
    }();
    auto underlying_custom_property_value = [&](Utf16FlyString const& name) -> NonnullRefPtr<StyleValue const> {
        if (base_custom_property_data) {
            if (auto const* style_property = base_custom_property_data->get(name))
                return *style_property->value;
        }
        return initial_custom_property_value(m_document->get_registered_custom_property(name), *m_document);
    };

    struct ActiveEffect {
        GC::Ref<Animations::KeyframeEffect> effect;
        GC::Ref<Animations::Animation> animation;
        double current_key { 0 };
    };
    Vector<ActiveEffect, 1> active_effects;
    Vector<StyleValueFFI::FfiAnimationPreparationEffect, 1> preparation_effects;
    Vector<double, 1> current_keys;
    for (auto effect : effects) {
        auto animation = effect->associated_animation();
        auto output_progress = effect->transformed_progress();
        if (!animation || !output_progress.has_value() || !effect->key_frame_set())
            continue;
        auto const& key_frame_set = *effect->key_frame_set();
        auto& keyframes = key_frame_set.keyframes_by_key;
        if (keyframes.size() < 2)
            continue;
        double current_key = *output_progress * 100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor;
        current_key = clamp(current_key, static_cast<double>(NumericLimits<i64>::min()), static_cast<double>(NumericLimits<i64>::max()));
        active_effects.append({ effect, *animation, current_key });
        preparation_effects.append({
            .identity = effect->animation_preparation_identity(),
            .generation = effect->animation_preparation_generation(),
        });
        current_keys.append(current_key);
    }
    if (active_effects.is_empty()) {
        computed_properties.clear_animated_properties(Badge<StyleComputer> {});
        return;
    }

    StyleValueFFI::FfiAnimationPreparationKey preparation_key {
        .effects = preparation_effects.data(),
        .effect_count = preparation_effects.size(),
    };
    auto const* animation_overlay = computed_properties.animated_overlay(Badge<StyleComputer> {});
    if (StyleValueFFI::rust_animation_preparation_matches(animation_overlay, &preparation_key)) {
        auto* mutable_animation_overlay = computed_properties.prepare_animated_overlay_for_rust_mutation(Badge<StyleComputer> {});
        StyleValueFFI::FfiAnimationContext animation_context {};
        animation_context.current_color = static_cast<StyleValueFFI::StyleValueData const*>(computed_properties.effective_property_data(PropertyID::Color));
        if (auto const* layout_node = abstract_element.element().unsafe_layout_node(); layout_node && Painting::has_committed_box(*layout_node)) {
            auto reference_box = Painting::transform_reference_box(*layout_node);
            animation_context.has_transform_reference_box = true;
            animation_context.transform_reference_box_width = reference_box.width().to_double();
            animation_context.transform_reference_box_height = reference_box.height().to_double();
        }
        StyleValueFFI::FfiComputedAnimationBatch computed_batch {
            .context = animation_context,
            .preparation_key = &preparation_key,
            .current_keys = current_keys.data(),
            .current_key_count = current_keys.size(),
            .cache_preparation = true,
            .resolved_animation_storage = nullptr,
            .computed_keyframe_storage = nullptr,
            .underlying_longhand_table = computed_properties.computed_longhand_table(),
            .overlay = mutable_animation_overlay,
            .custom_underlying_values = nullptr,
            .custom_initial_values = nullptr,
            .custom_value_count = 0,
            .custom_results = nullptr,
            .custom_result_count = nullptr,
        };
        StyleValueFFI::rust_evaluate_animations(&computed_batch);
        computed_properties.finish_animated_overlay_rust_mutation(Badge<StyleComputer> {});
        clear_computation_context_caches();
        return;
    }

    for (auto const& active_effect : active_effects) {
        auto effect = active_effect.effect;
        auto animation = active_effect.animation;
        auto const& key_frame_set = *effect->key_frame_set();
        auto& keyframes = key_frame_set.keyframes_by_key;
        StyleValueFFI::FfiAnimationStyleSheetResourceContext style_sheet_resource_context {};
        if (key_frame_set.style_sheet_resource_context.has_value()) {
            auto bytes = key_frame_set.style_sheet_resource_context->base_url.bytes();
            style_sheet_resource_context = {
                .base_url = bytes.data(),
                .base_url_length = bytes.size(),
                .has_value = true,
                .origin_clean = key_frame_set.style_sheet_resource_context->origin_clean,
            };
        }
        auto first_keyframe_index = ffi_keyframes.size();
        for (auto it = keyframes.begin(); it != keyframes.end(); ++it) {
            auto keyframe_index = ffi_keyframes.size();
            auto easing = it->easing.visit(
                [](Empty) -> Optional<CSS::EasingFunction> { return {}; },
                [](CSS::EasingFunction const& easing) -> Optional<CSS::EasingFunction> { return easing; },
                [&](RustStyleValueHandle const& value) -> Optional<CSS::EasingFunction> {
                    auto style_value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value.data()));
                    return resolve_keyframe_easing(*style_value, abstract_element);
                });
            if (!easing.has_value()) {
                easing = animation->is_css_animation()
                    ? static_cast<CSSAnimation const&>(*animation).default_easing()
                    : CSS::EasingFunction::linear();
            }
            auto composite_operation = [&] {
                switch (it->composite) {
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
            }();
            linear_easing_points.empend();
            auto& points = linear_easing_points.last();
            ffi_keyframes.append({
                .key = static_cast<i64>(it.key()),
                .easing = to_ffi_easing(*easing, points),
                .composite = to_ffi_composite_operation(composite_operation),
            });
            for (auto const& [property, value] : it->properties) {
                bool is_use_initial = false;
                auto style_value = value.visit(
                    [&](Animations::KeyframeEffect::KeyFrameSet::UseInitial) -> RustStyleValueHandle {
                        if (property.is_custom_property()) {
                            is_use_initial = true;
                            return RustStyleValueHandle::retained(underlying_custom_property_value(property.name())->rust_style_value_data());
                        }
                        if (property_is_shorthand(property.id()))
                            return {};
                        is_use_initial = true;
                        return RustStyleValueHandle::retained(computed_properties.property(property.id(), ComputedStyleWorkingSet::WithAnimationsApplied::No).rust_style_value_data());
                    },
                    [](RustStyleValueHandle const& value) -> RustStyleValueHandle { return value; });
                if (!style_value || style_value->tag == StyleValueFFI::StyleValueData::Tag::PendingSubstitution)
                    continue;
                if (style_value->tag == StyleValueFFI::StyleValueData::Tag::Unresolved) {
                    // Substitution needs the typed facade, but only var()-bearing keyframes take this path,
                    // and those re-parse the value every frame anyway.
                    auto unresolved = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(style_value.data()));
                    if (!property.is_custom_property() || unresolved->as_unresolved().contains_arbitrary_substitution_function()) {
                        auto resolved = abstract_element.document().style_computer().resolve_unresolved_style_value(abstract_element, property, unresolved->as_unresolved());
                        style_value = RustStyleValueHandle::retained(resolved->rust_style_value_data());
                    }
                }
                // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
                // When substitution results in a guaranteed-invalid value, treat it as unset
                // (i.e. inherit for inherited properties, initial for non-inherited properties).
                if (style_value->tag == StyleValueFFI::StyleValueData::Tag::GuaranteedInvalid && !property.is_custom_property())
                    continue;
                keyframe_declarations.append({
                    .keyframe_index = keyframe_index,
                    .property = property,
                    .value = move(style_value),
                    .style_sheet_resource_context = style_sheet_resource_context,
                    .use_initial = is_use_initial,
                    .is_transition = animation->is_css_transition(),
                });
            }
        }
        ffi_effects.append({
            .first_keyframe_index = first_keyframe_index,
            .keyframe_count = ffi_keyframes.size() - first_keyframe_index,
            .current_key = active_effect.current_key,
            .result_of_transition = animation->is_css_transition(),
        });
    }

    if (keyframe_declarations.is_empty()) {
        return;
    }

    Vector<PropertyNameAndID> custom_properties_by_name_id;
    struct CustomPropertyAnimationInfo {
        bool is_inherited { true };
        bool is_important { false };
    };
    Vector<CustomPropertyAnimationInfo> custom_property_infos;
    HashMap<Utf16FlyString, u32> custom_property_name_ids;
    auto element_declares_own_custom_properties = [&] {
        if (!base_custom_property_data)
            return false;
        auto inherit_from = abstract_element.element_to_inherit_style_from();
        return !(inherit_from.has_value() && inherit_from->custom_property_data().ptr() == base_custom_property_data.ptr());
    }();
    auto custom_name_id_for = [&](PropertyNameAndID const& property) -> u32 {
        return custom_property_name_ids.ensure(property.name(), [&] {
            auto registration = m_document->get_registered_custom_property(property.name());
            bool is_important = false;
            if (element_declares_own_custom_properties) {
                size_t declared_index = 0;
                for (auto const& [name, style_property] : base_custom_property_data->own_values()) {
                    if (declared_index++ >= base_custom_property_data->declared_count())
                        break;
                    if (name == property.name()) {
                        is_important = style_property.important == Important::Yes;
                        break;
                    }
                }
            }
            custom_properties_by_name_id.append(property);
            custom_property_infos.append({
                .is_inherited = !registration.has_value() || registration->inherit,
                .is_important = is_important,
            });
            return static_cast<u32>(custom_properties_by_name_id.size());
        });
    };

    Vector<StyleValueFFI::FfiAnimationDeclaration> ffi_declarations;
    ffi_declarations.ensure_capacity(keyframe_declarations.size());
    for (auto const& declaration : keyframe_declarations) {
        u32 custom_name_id = 0;
        CustomPropertyAnimationInfo custom_property_info;
        if (declaration.property.is_custom_property()) {
            custom_name_id = custom_name_id_for(declaration.property);
            custom_property_info = custom_property_infos[custom_name_id - 1];
        }
        ffi_declarations.unchecked_append({
            .keyframe_index = declaration.keyframe_index,
            .property_id = to_underlying(declaration.property.id()),
            .custom_name_id = custom_name_id,
            .custom_is_inherited = custom_property_info.is_inherited,
            .custom_is_important = custom_property_info.is_important,
            .value = declaration.value.data(),
            .style_sheet_resource_context = declaration.style_sheet_resource_context,
            .use_initial = declaration.use_initial,
            .is_transition = declaration.is_transition,
        });
    }
    // The table's importance bitmap already uses the byte layout the animation core expects.
    Vector<u8> important_property_bitmap;
    important_property_bitmap.append(computed_properties.property_importance_bitmap().data(), computed_properties.property_importance_bitmap().size());

    Vector<NonnullRefPtr<StyleValue const>> custom_animation_value_storage;
    Vector<StyleValueFFI::StyleValueData const*> custom_underlying_values;
    Vector<StyleValueFFI::StyleValueData const*> custom_initial_values;
    Vector<StyleValueFFI::FfiAnimatedCustomProperty> custom_results;
    size_t custom_result_count = 0;

    auto compute_animation_values = [&](ReadonlySpan<StyleValueFFI::FfiResolvedAnimationProperty> resolved_properties, StyleValueFFI::FfiResolvedAnimationProperties const& resolved_batch) -> StyleValueFFI::FfiComputedAnimationBatch {
        VERIFY(computation_context_cache_is_empty());
        for (auto const& property : resolved_properties) {
            if (property.custom_name_id != 0)
                continue;
            auto source_longhand_id = static_cast<PropertyID>(property.source_longhand_id);
            if (property.value_source == StyleValueFFI::FfiAnimationSpecifiedValueSource::Inherited && !is_inherited_property(source_longhand_id))
                m_keyframes_inherited_non_inherited_style_groups |= ComputedValues::style_group_bit_of_property(source_longhand_id);
        }

        Vector<NonnullRefPtr<StyleValue const>> custom_keyframe_value_storage;
        Vector<void const*> custom_keyframe_values;
        custom_keyframe_values.resize(resolved_properties.size());
        for (size_t index = 0; index < resolved_properties.size(); ++index) {
            auto const& property = resolved_properties[index];
            if (property.custom_name_id == 0)
                continue;
            auto const& name = custom_properties_by_name_id[property.custom_name_id - 1].name();
            auto registration = m_document->get_registered_custom_property(name);
            auto specified_value = [&]() -> NonnullRefPtr<StyleValue const> {
                switch (property.value_source) {
                case StyleValueFFI::FfiAnimationSpecifiedValueSource::Inherited:
                    return inherited_custom_property_value(registration, AbstractOrHypotheticalElement { abstract_element }, name, &computed_properties);
                case StyleValueFFI::FfiAnimationSpecifiedValueSource::Initial:
                    return initial_custom_property_value(registration, *m_document);
                case StyleValueFFI::FfiAnimationSpecifiedValueSource::Underlying:
                    return underlying_custom_property_value(name);
                case StyleValueFFI::FfiAnimationSpecifiedValueSource::Value:
                    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(property.value));
                }
                VERIFY_NOT_REACHED();
            }();
            auto computed_value = compute_animated_custom_property_value(name, move(specified_value), computed_properties, abstract_element);
            custom_keyframe_values[index] = computed_value->rust_style_value_data();
            custom_keyframe_value_storage.append(move(computed_value));
        }

        Optional<DOM::AbstractElement::TreeCountingFunctionResolutionContext> tree_counting_context;
        if (resolved_batch.uses_tree_counting_function)
            tree_counting_context = abstract_element.tree_counting_function_resolution_context();
        Vector<ComputedValuesFFI::FfiRandomBaseValue> random_base_values;
        random_base_values.ensure_capacity(resolved_batch.unfixed_random_sharing_count);
        for (auto const& sharing : ReadonlySpan<StyleValueFFI::FfiAnimationUnfixedRandomSharing> { resolved_batch.unfixed_random_sharings, resolved_batch.unfixed_random_sharing_count }) {
            VERIFY(sharing.name != 0);
            RandomCachingKey random_caching_key {
                .name = Utf16FlyString::from_raw(sharing.name),
                .element_id = sharing.element_shared
                    ? Optional<UniqueNodeID> { OptionalNone {} }
                    : Optional<UniqueNodeID> { abstract_element.element().unique_id() },
            };
            random_base_values.empend(sharing.source, const_cast<DOM::Element&>(abstract_element.element()).ensure_css_random_base_value(random_caching_key));
        }
        String document_base_url;
        if (resolved_batch.needs_document_base_url)
            document_base_url = abstract_element.document().base_url().to_string();
        auto document_base_url_bytes = document_base_url.bytes();
        Vector<u8> document_supported_color_scheme_codes;
        auto document_supported_color_schemes = document().supported_color_schemes();
        if (document_supported_color_schemes.has_value()) {
            document_supported_color_scheme_codes.ensure_capacity(document_supported_color_schemes->size());
            for (auto const& scheme : *document_supported_color_schemes)
                document_supported_color_scheme_codes.unchecked_append(to_underlying(preferred_color_scheme_from_string(scheme)));
        }
        ComputedValuesFFI::FfiStyleComputationEnvironment const computation_environment {
            .box_type_input = {},
            .color_scheme_input = {
                .preferred_color_scheme = static_cast<u8>(to_underlying(document().page().preferred_color_scheme())),
                .has_document_supported_schemes = document_supported_color_schemes.has_value(),
                .document_supported_scheme_codes = document_supported_color_scheme_codes.data(),
                .document_supported_scheme_count = document_supported_color_scheme_codes.size(),
            },
            .is_th_element = false,
            .has_new_font_size = false,
            .has_tree_counting_context = tree_counting_context.has_value(),
            .sibling_count = tree_counting_context.has_value() ? static_cast<u64>(tree_counting_context->sibling_count) : 0,
            .sibling_index = tree_counting_context.has_value() ? static_cast<u64>(tree_counting_context->sibling_index) : 0,
            .random_base_values = random_base_values.data(),
            .random_base_value_count = random_base_values.size(),
            .document_base_url = document_base_url_bytes.data(),
            .document_base_url_length = document_base_url_bytes.size(),
            .style_sheet_resource_contexts = nullptr,
            .style_sheet_resource_context_count = 0,
            .device_pixels_per_css_pixel = m_document->page().client().device_pixels_per_css_pixel(),
            .initial_font_size_raw = InitialValues::font_size().raw_value(),
            .default_font_size_raw = default_user_font_size().raw_value(),
        };
        auto font_length_resolution_context = to_ffi_length_resolution_context_with_container_bases(
            get_computation_context_for_property(PropertyID::FontFamily, computed_properties, abstract_element).length_resolution_context,
            resolved_batch.container_relative_length_unit_mask);
        auto line_height_length_resolution_context = to_ffi_length_resolution_context_with_container_bases(
            get_computation_context_for_property(PropertyID::LineHeight, computed_properties, abstract_element).length_resolution_context,
            resolved_batch.container_relative_length_unit_mask);
        auto remaining_length_resolution_context = to_ffi_length_resolution_context_with_container_bases(
            get_computation_context_for_property(PropertyID::Color, computed_properties, abstract_element).length_resolution_context,
            resolved_batch.container_relative_length_unit_mask);

        auto inheritance_parent = abstract_element.element_to_inherit_style_from();
        ComputedValuesFFI::FfiAnimationKeyframeLonghandInput const keyframe_input {
            .underlying_longhand_table = computed_properties.computed_longhand_table(),
            .style_engine = m_style_engine.rust_handle(),
            .inheritance_parent_style_record = inheritance_parent.has_value() ? inheritance_parent->style_record_identity().value() : 0,
            .resolved_properties = resolved_properties.data(),
            .property_count = resolved_properties.size(),
            .environment = &computation_environment,
            .font_length_resolution_context = &font_length_resolution_context,
            .line_height_length_resolution_context = &line_height_length_resolution_context,
            .remaining_length_resolution_context = &remaining_length_resolution_context,
            .custom_property_values = custom_keyframe_values.data(),
        };
        auto computed_keyframe_batch = ComputedValuesFFI::rust_compute_animation_keyframe_longhands(&keyframe_input);
        VERIFY(computed_keyframe_batch.value_count == resolved_properties.size());
        if (computed_keyframe_batch.depends_on_viewport_metrics)
            computed_properties.set_depends_on_viewport_metrics();
        if (computed_keyframe_batch.font_metrics_depend_on_viewport_metrics)
            computed_properties.set_font_metrics_depend_on_viewport_metrics();
        if (resolved_batch.uses_tree_counting_function)
            const_cast<DOM::Element&>(abstract_element.element()).set_style_uses_tree_counting_function();

        auto const& color_computation_context = get_computation_context_for_property(PropertyID::Color, computed_properties, abstract_element);
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
        if (auto const* layout_node = abstract_element.element().unsafe_layout_node(); layout_node && Painting::has_committed_box(*layout_node)) {
            auto reference_box = Painting::transform_reference_box(*layout_node);
            animation_context.has_transform_reference_box = true;
            animation_context.transform_reference_box_width = reference_box.width().to_double();
            animation_context.transform_reference_box_height = reference_box.height().to_double();
        }
        custom_underlying_values.ensure_capacity(custom_properties_by_name_id.size());
        custom_initial_values.ensure_capacity(custom_properties_by_name_id.size());
        for (auto const& property : custom_properties_by_name_id) {
            auto underlying_value = underlying_custom_property_value(property.name());
            auto initial_value = initial_custom_property_value(m_document->get_registered_custom_property(property.name()), *m_document);
            custom_underlying_values.unchecked_append(underlying_value->rust_style_value_data());
            custom_initial_values.unchecked_append(initial_value->rust_style_value_data());
            custom_animation_value_storage.append(move(underlying_value));
            custom_animation_value_storage.append(move(initial_value));
        }
        custom_results.resize(custom_properties_by_name_id.size());

        return StyleValueFFI::FfiComputedAnimationBatch {
            .context = animation_context,
            .preparation_key = &preparation_key,
            .current_keys = current_keys.data(),
            .current_key_count = current_keys.size(),
            .cache_preparation = custom_properties_by_name_id.is_empty()
                && !resolved_batch.uses_tree_counting_function
                && resolved_batch.container_relative_length_unit_mask == 0
                && !resolved_batch.needs_document_base_url
                && resolved_batch.unfixed_random_sharing_count == 0,
            .resolved_animation_storage = resolved_batch.storage,
            .computed_keyframe_storage = computed_keyframe_batch.storage,
            .underlying_longhand_table = computed_properties.computed_longhand_table(),
            .overlay = computed_properties.prepare_animated_overlay_for_rust_mutation(Badge<StyleComputer> {}),
            .custom_underlying_values = custom_underlying_values.data(),
            .custom_initial_values = custom_initial_values.data(),
            .custom_value_count = custom_underlying_values.size(),
            .custom_results = custom_results.data(),
            .custom_result_count = &custom_result_count,
        };
    };

    StyleValueFFI::FfiAnimationBatch batch {
        .declarations = ffi_declarations.data(),
        .declaration_count = ffi_declarations.size(),
        .effects = ffi_effects.data(),
        .effect_count = ffi_effects.size(),
        .keyframes = ffi_keyframes.data(),
        .keyframe_count = ffi_keyframes.size(),
        .writing_mode = to_underlying(computed_properties.writing_mode()),
        .direction = to_underlying(computed_properties.direction()),
        .important_property_bitmap = important_property_bitmap.data(),
        .important_property_bitmap_length = important_property_bitmap.size(),
    };
    auto resolved_properties = StyleValueFFI::rust_resolve_animation_declarations(&batch);
    if (resolved_properties.count == 0)
        return;
    auto computed_batch = compute_animation_values(ReadonlySpan<StyleValueFFI::FfiResolvedAnimationProperty> {
                                                       resolved_properties.properties, resolved_properties.count },
        resolved_properties);
    auto result_count = StyleValueFFI::rust_evaluate_animations(&computed_batch);
    VERIFY(result_count == resolved_properties.animation_value_count);
    VERIFY(custom_result_count <= custom_results.size());
    for (size_t index = 0; index < custom_result_count; ++index) {
        auto const& result = custom_results[index];
        VERIFY(result.custom_name_id != 0 && result.custom_name_id <= custom_properties_by_name_id.size());
        auto const& property = custom_properties_by_name_id[result.custom_name_id - 1];
        auto style_value = StyleValue::adopt_rust_style_value_data(result.value);
        computed_properties.set_animated_custom_property(Badge<StyleComputer> {}, property.name(), move(style_value));
    }
    computed_properties.finish_animated_overlay_rust_mutation(Badge<StyleComputer> {});

    clear_computation_context_caches();
}

// https://drafts.css-houdini.org/css-properties-values-api/#calculation-of-computed-values
NonnullRefPtr<StyleValue const> StyleComputer::compute_animated_custom_property_value(Utf16FlyString const& name, NonnullRefPtr<StyleValue const> specified_value, ComputedStyleWorkingSet& computed_properties, DOM::AbstractElement abstract_element) const
{
    auto registration = m_document->get_registered_custom_property(name);
    if (!registration.has_value() || registration->syntax.is_universal())
        return specified_value;

    return finalize_custom_property_value(&computed_properties, AbstractOrHypotheticalElement { abstract_element }, name, move(specified_value));
}

void StyleComputer::publish_animated_custom_properties(ComputedStyleWorkingSet& computed_properties, DOM::AbstractElement abstract_element) const
{
    auto data = abstract_element.custom_property_data();
    RefPtr<CustomPropertyData const> base = data;
    if (data && data->is_animation_overlay())
        base = data->parent();

    auto const& animated_values = computed_properties.animated_custom_properties();
    if (animated_values.is_empty()) {
        if (base.ptr() != data.ptr()) {
            abstract_element.replace_custom_property_data(Badge<StyleComputer> {}, base);
            invalidate_animated_custom_property_readers(abstract_element, animated_values);
        }
        return;
    }

    if (data && data->is_animation_overlay() && data->own_values().size() == animated_values.size()) {
        bool values_unchanged = true;
        for (auto const& [name, value] : animated_values) {
            auto existing = data->own_values().find(name);
            if (existing == data->own_values().end() || !existing->value.value->equals(*value)) {
                values_unchanged = false;
                break;
            }
        }
        if (values_unchanged)
            return;
    }

    OrderedHashMap<Utf16FlyString, StyleProperty> overlay_values;
    for (auto const& [name, value] : animated_values) {
        overlay_values.set(name,
            StyleProperty {
                .important = Important::No,
                .property_id = PropertyID::Custom,
                .value = value,
            });
    }
    abstract_element.replace_custom_property_data(Badge<StyleComputer> {}, CustomPropertyData::create_animation_overlay(move(overlay_values), move(base)));
    invalidate_animated_custom_property_readers(abstract_element, animated_values);
}

void StyleComputer::invalidate_animated_custom_property_readers(DOM::AbstractElement abstract_element, OrderedHashMap<Utf16FlyString, NonnullRefPtr<StyleValue const>> const& animated_values) const
{
    auto& element = abstract_element.element();
    // Which custom properties the element's own declarations read is not recorded: an element
    // whose custom properties animate recomputes.
    auto& style_engine = element.document().style_computer().style_engine();
    style_engine.record_element_style_input_change(element.style_node_id());

    auto any_animated_custom_property_inherits = [&] {
        if (animated_values.is_empty())
            return true;
        for (auto const& [name, value] : animated_values) {
            auto registration = m_document->get_registered_custom_property(name);
            if (!registration.has_value() || registration->inherit)
                return true;
        }
        return false;
    };
    if (!abstract_element.pseudo_element().has_value() && any_animated_custom_property_inherits()) {
        style_engine.record_flat_tree_descendant_style_input_changes(
            element.style_node_id(),
            StyleEngine::InheritedStyle,
            RequiredInvalidationAfterStyleChange::all_inherited_style_groups);
    }
}

void StyleComputer::process_animation_definitions(ComputedStyleWorkingSet const& computed_properties, CascadedProperties const& cascaded_properties, DOM::AbstractElement& abstract_element, ReadonlySpan<AnimationProperties> animation_definitions) const
{
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

static void compute_transitioned_properties(Vector<TransitionProperties> transitions, bool delay_and_duration_are_single_zero, DOM::AbstractElement abstract_element)
{
    // FIXME: For now we don't bother registering transitions on the first computation since they can't run (because
    //        there is nothing to transition from) but this will change once we implement @starting-style
    if (!abstract_element.has_style())
        return;
    // FIXME: Add transition helpers on AbstractElement.
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();

    element.clear_registered_transitions(pseudo_element);

    // OPTIMIZATION: Registered transitions with a "combined duration" of less than or equal to 0s are equivalent to not
    //               having a transition registered at all, except in the case that we already have an associated
    //               transition for that property, so we can skip registering them. This implementation intentionally
    //               ignores some of those cases (e.g. transitions being registered but for other properties, multiple
    //               transitions, negative delays, etc) since it covers the common (initial property values) case and
    //               the other cases are rare enough that the cost of identifying them would likely more than offset any
    //               gains.
    if (
        element.property_ids_with_existing_transitions(pseudo_element).is_empty()
        && delay_and_duration_are_single_zero) {
        return;
    }

    element.add_transitioned_properties(pseudo_element, move(transitions));
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
Vector<GC::Ref<Animations::KeyframeEffect>> StyleComputer::start_needed_transitions(ComputedStyleWorkingSet& new_style, DOM::AbstractElement abstract_element) const
{
    auto had_pending_animated_style_update = m_document->needs_animated_style_update();

    // FIXME: Add some transition helpers to AbstractElement.
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();
    auto style_node_id = element.style_node_id();
    Optional<u64> transition_target_key;
    if (style_node_id != 0)
        transition_target_key = (static_cast<u64>(style_node_id.value()) << 8) | pseudo_element_to_ffi(pseudo_element);
    auto transition_baseline_style_record = abstract_element.style_record_identity();
    VERIFY(transition_baseline_style_record);
    if (transition_target_key.has_value()
        && (abstract_element.style_scope().rule_cache().has_size_container_queries
            || document().is_in_style_stabilization_feedback_epoch())) {
        record_transition_stabilization_baseline(abstract_element);
    }
    if (transition_target_key.has_value()) {
        if (auto existing_baseline = m_transition_stabilization_baselines.get(*transition_target_key); existing_baseline.has_value())
            transition_baseline_style_record = *existing_baseline;
    }
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
            if (existing_transition) {
                old_reversing_shortening_factor = existing_transition->reversing_shortening_factor();
                if (has_running_transition)
                    old_timing_function_output = existing_transition->timing_function_output_at_time(style_change_event_time);
            }
        }

        ffi_properties.append({
            .property_id = to_underlying(property_id),
            .before_change_value = nullptr,
            .after_change_value = nullptr,
            .current_value = nullptr,
            .existing_end_value = existing_transition ? existing_transition->transition_end_value()->rust_style_value_data() : nullptr,
            .reversing_adjusted_start_value = existing_transition ? existing_transition->reversing_adjusted_start_value()->rust_style_value_data() : nullptr,
            .has_matching_transition = has_matching_transition == HasMatchingTransition::Yes,
            .allow_discrete = allow_discrete,
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
            .before_change_value = {},
            .after_change_value = {},
            .current_value = {},
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
    m_style_engine.decide_transitions(
        transition_baseline_style_record,
        new_style.computed_longhand_table(),
        new_style.animated_overlay(Badge<StyleComputer> {}),
        input,
        actions.data());
    auto retain_style_value = [](StyleValueFFI::StyleValueData const* value) -> RefPtr<StyleValue const> {
        if (!value)
            return {};
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value));
    };
    for (size_t index = 0; index < prepared_transitions.size(); ++index) {
        auto& prepared_transition = prepared_transitions[index];
        auto const& property = ffi_properties[index];
        prepared_transition.before_change_value = retain_style_value(property.before_change_value);
        prepared_transition.after_change_value = retain_style_value(property.after_change_value);
        prepared_transition.current_value = retain_style_value(property.current_value);
    }

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
        // inventories and custom-property declarations keep using their CSSOM identity, since
        // their C++ declarations may differ.
        auto const use_semantic_source_identity = value_comparison == CascadeBlockKeyValueComparison::ByIdentity
            && block.source && block.semantic_declaration_id != 0
            && !block_declares_custom_properties(block.custom_properties);
        key.append(use_semantic_source_identity ? block.semantic_declaration_id : 0);
        key.append(block.source && !use_semantic_source_identity ? block.source->identity() : 0);
        key.append(block.source && !use_semantic_source_identity ? block.source->revision() : 0);
        auto const declares_animation_name = any_of(block.properties, [](auto const& property) { return property.property_id == PropertyID::AnimationName; });
        key.append(declares_animation_name ? bit_cast<FlatPtr>(block.source_shadow_root.ptr()) : 0);
        if (!block.source) {
            key.append(block.properties.size());
            for (auto const& property : block.properties) {
                key.append(to_underlying(property.property_id) | (static_cast<u64>(property.important == Important::Yes) << 32));
                pinned_values.append(property.value);
            }
        }
        return CascadeBlockKeyDependencies {
            .reads_custom_properties = reads_custom_properties,
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
        dependencies.reads_style_scope |= block_dependencies.reads_style_scope;
    }

    if (!presentational_hint_properties.is_empty()) {
        auto block_dependencies = append_block({
            .properties = presentational_hint_properties,
            .origin = CascadeOrigin::AuthorPresentationalHint,
        });
        dependencies.reads_custom_properties |= block_dependencies.reads_custom_properties;
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

static bool custom_property_value_matches_parent(CustomPropertyData const* parent, Utf16FlyString const& name, StyleProperty const& property)
{
    if (!parent)
        return false;
    auto const* parent_property = parent->get(name);
    return parent_property && parent_property->value->rust_style_value_data() == property.value->rust_style_value_data();
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

RefPtr<CustomPropertyData const> StyleComputer::engine_custom_property_environment(u64 identity, RefPtr<CustomPropertyData const> const& inherited) const
{
    if (!StyleEngine::is_engine_custom_property_environment(identity))
        return {};
    if (auto existing = m_engine_custom_property_environments.get(identity); existing.has_value())
        return *existing;
    u64 parent_identity = 0;
    auto const* store = m_style_engine.borrow_engine_custom_property_environment(identity, parent_identity);
    if (!store)
        return {};
    if (parent_identity != (inherited ? inherited->identity() : 0)) {
        ComputedValuesFFI::rust_custom_property_store_destroy(store);
        return {};
    }
    OrderedHashMap<Utf16FlyString, StyleProperty> own_values;
    ComputedValuesFFI::rust_custom_property_store_for_each_own_entry(store, &own_values, [](void* context, size_t name_raw, bool important, void const* data) {
        auto& own_values = *static_cast<OrderedHashMap<Utf16FlyString, StyleProperty>*>(context);
        own_values.set(
            Utf16FlyString::from_raw(name_raw),
            StyleProperty {
                .important = important ? Important::Yes : Important::No,
                .property_id = PropertyID::Custom,
                .value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data))),
            });
    });
    auto data = CustomPropertyData::create(move(own_values), inherited, store, identity);
    m_engine_custom_property_environments.set(identity, data);
    return data;
}

// An environment nothing but the table holds is one no element is in, and the table is the only
// thing keeping it - and its parent chain - alive.
void StyleComputer::sweep_custom_property_environments() const
{
    // The memo of what a declaration list resolves to holds environments too, so it goes first or
    // nothing below it is ever the last reference.
    m_cascaded_custom_property_environments.clear();
    m_registered_custom_property_parses.clear();
    m_custom_property_environments.remove_all_matching([](auto&, Vector<NonnullRefPtr<CustomPropertyData const>>& bucket) {
        bucket.remove_all_matching([](auto const& data) { return data->ref_count() == 1; });
        return bucket.is_empty();
    });
    m_engine_custom_property_environments.remove_all_matching([](auto&, NonnullRefPtr<CustomPropertyData const> const& data) { return data->ref_count() == 1; });
}

NonnullRefPtr<CascadedProperties> StyleComputer::compute_cascaded_values(DOM::AbstractElement abstract_element, CascadeInput const& cascade_input, IncludeInlineStyle include_inline_style, StyleSharingCandidate* sharing, Vector<StyleProperty> const* precomputed_presentational_hints) const
{
    begin_style_update();
    ScopeGuard end_style_update = [&] { this->end_style_update(); };
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
                    .name = ffi_utf16_view(name),
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
        auto dependencies = append_cascade_blocks_to_key(sharing->key.computation_inputs, sharing->key.pinned_values, cascade_input, presentational_hint_properties, inline_style, CascadeBlockKeyValueComparison::ByIdentity);
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

    RefPtr<CustomPropertyData const> parent_custom_property_data;
    RefPtr<CustomPropertyData const> inheritance_custom_property_data;
    auto inherit_from = abstract_element.element_to_inherit_style_from();
    if (inherit_from.has_value()) {
        parent_custom_property_data = inheritable_custom_property_data(*inherit_from);
        inheritance_custom_property_data = inherit_from->custom_property_data();
    }

    struct BulkCascadeContext {
        CascadedProperties& cascaded_properties;
        DOM::AbstractElement& abstract_element;
        Vector<BlockSource> const& block_sources;
        RefPtr<CustomPropertyData const> parent_custom_property_data;
        u64 custom_property_environment_identity { 0 };
        void const* inheritance_custom_property_store { nullptr };
        void const* (*install_custom_properties)(BulkCascadeContext&, ComputedValuesFFI::FfiCascadedCustomProperty const*, size_t, void const*&) { nullptr };
    } bulk_context {
        .cascaded_properties = *cascaded_properties,
        .abstract_element = abstract_element,
        .block_sources = block_sources,
        .parent_custom_property_data = parent_custom_property_data,
        .inheritance_custom_property_store = inheritance_custom_property_data ? inheritance_custom_property_data->rust_store() : nullptr,
    };

    // The cascade only reads this value's data pointer, so mint a bare Rust handle instead of a wrapper.
    RustStyleValueHandle const unset_value { StyleValueFFI::rust_style_value_create_keyword(to_underlying(Keyword::Unset)) };

    auto install_custom_properties = [](BulkCascadeContext& bulk_context, ComputedValuesFFI::FfiCascadedCustomProperty const* properties, size_t count, void const*& rust_store) -> void const* {
        auto& document = bulk_context.abstract_element.element().document();
        auto& style_computer = document.style_computer();

        // OPTIMIZATION: The declarations below name the whole answer, together with what the
        //               element inherits and which names are registered, so an element handed the
        //               same list against the same environment gets the same one back.
        auto& key = style_computer.m_cascaded_custom_property_key_scratch;
        key.clear_with_capacity();
        key.append(bit_cast<FlatPtr>(bulk_context.parent_custom_property_data.ptr()));
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
            if (!result || result == bulk_context.parent_custom_property_data) {
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
            if (custom_property_value_matches_parent(bulk_context.parent_custom_property_data.ptr(), name, property))
                continue;
            cascaded_own.set(name, move(property));
        }

        RefPtr<CustomPropertyData const> resolved;
        if (cascaded_own.is_empty())
            resolved = bulk_context.parent_custom_property_data;
        else {
            VERIFY(rust_store);
            resolved = style_computer.intern_custom_property_data(
                CustomPropertyData::create(move(cascaded_own), bulk_context.parent_custom_property_data, rust_store));
            rust_store = nullptr;
        }
        memo_bucket.append({ key, bulk_context.parent_custom_property_data, resolved });
        return apply_environment(resolved);
    };
    bulk_context.install_custom_properties = install_custom_properties;

    auto& document = bulk_context.abstract_element.document();
    SubstitutionData substitution_data { abstract_element, has_unresolved_declarations, has_custom_function_declarations };
    ComputedValuesFFI::FfiCascadeResolutionContext resolution_context {
        .parse_context = &substitution_data.parse_context,
        .media_environment = cached_media_environment_for_style_update(),
        .load_media_environment = [](void* context) -> void const* {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            return bulk_context.abstract_element.document().style_computer().ensure_media_environment_for_style_update();
        },
        .custom_property_store = parent_custom_property_data ? parent_custom_property_data->rust_store() : nullptr,
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
        .install_custom_properties = [](void* context, ComputedValuesFFI::FfiCascadedCustomProperty const* properties, size_t count, void const** rust_store) -> void const* {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            return bulk_context.install_custom_properties(bulk_context, properties, count, *rust_store);
        },
        .resolve_custom_function = resolve_custom_function_for_substitution,
        .evaluate_style_query = [](void* context, ComputedValuesFFI::FfiUtf16View source) -> u8 {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            return evaluate_style_query_for_substitution(bulk_context.abstract_element, source);
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
                bulk_context.abstract_element.element().set_style_uses_custom_function(); },
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
        &resolution_context);
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

Length::FontMetrics StyleComputer::calculate_root_element_font_metrics(ComputedStyleWorkingSet const& style) const
{
    auto const& root_value = style.property(CSS::PropertyID::FontSize);

    auto font_pixel_metrics = style.first_available_computed_font(document().font_computer())->pixel_metrics();
    Length::FontMetrics font_metrics { m_default_font_metrics.font_size, font_pixel_metrics, InitialValues::line_height() };
    font_metrics.font_size = root_value.as_length().length().to_px(viewport_rect(), font_metrics, font_metrics);
    font_metrics.line_height = style.line_height(document().font_computer());

    return font_metrics;
}

void StyleComputer::update_root_element_font_metrics(ComputedValues const& values)
{
    m_root_element_font_metrics = Length::FontMetrics { values.font_size(), values.font_list().first_available_font().pixel_metrics(), values.line_height() };
    m_root_element_font_metrics_depend_on_viewport_metrics = values.font_metrics_depend_on_viewport_metrics();
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

ComputationContext StyleComputer::make_computation_context_for_property(PropertyID property_id, ComputedStyleWorkingSet const& style, Optional<DOM::AbstractElement> abstract_element) const
{
    auto subject_inline_axis_is_horizontal = [&]() {
        auto writing_mode = [&](DOM::AbstractElement const& candidate) -> Optional<WritingMode> {
            auto record = m_style_engine.style_record_view(candidate.style_record_identity());
            if (!record.present)
                return {};
            auto const* inherited_box = static_cast<ComputedValuesFFI::InheritedBoxValues const*>(record.payloads[to_underlying(StyleGroupIndex::InheritedBoxValues)]);
            return static_cast<WritingMode>(inherited_box->writing_mode);
        };
        if (!abstract_element.has_value())
            return true;
        if (auto mode = writing_mode(*abstract_element); mode.has_value())
            return *mode == WritingMode::HorizontalTb;
        if (auto inheritance_parent = abstract_element->element_to_inherit_style_from(); inheritance_parent.has_value() && inheritance_parent->has_style())
            return writing_mode(*inheritance_parent).value_or(WritingMode::HorizontalTb) == WritingMode::HorizontalTb;
        return true;
    }();

    switch (property_id) {
    // FIXME: While `color-scheme` doesn't actually require a computation context (since it only takes keyword values),
    //        callers request one uniformly. Since `color-scheme` must be computed before creating a generic computation
    //        context, use the font context instead.
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

static ComputedValuesFFI::FfiBoxTypeTransformationInput make_box_type_transformation_input(
    DOM::AbstractElement abstract_element, Optional<Display> known_parent_display = {})
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
    bool should_adjust_element = !abstract_element.pseudo_element().has_value();
    auto facts = element_style_adjustment_facts(element);
    auto adjusts = [&](ElementStyleAdjustmentFact fact) { return should_adjust_element && (facts & fact) != 0; };

    return {
        .display = to_ffi_display(InitialValues::display()),
        .position = to_underlying(Keyword::Static),
        .float_value = to_underlying(Keyword::None),
        .is_br_element = adjusts(ElementStyleAdjustmentFact::IsBr),
        .is_document_element = (facts & ElementStyleAdjustmentFact::IsDocumentElement) != 0,
        .is_mathml_element = (facts & ElementStyleAdjustmentFact::IsMathML) != 0,
        .is_mathml_mtable = (facts & ElementStyleAdjustmentFact::IsMathMLMtable) != 0,
        .is_mathml_mtr = (facts & ElementStyleAdjustmentFact::IsMathMLMtr) != 0,
        .is_mathml_mtd = (facts & ElementStyleAdjustmentFact::IsMathMLMtd) != 0,
        .has_parent_display = has_parent_display,
        .parent_display = has_parent_display ? to_ffi_display(*parent_display) : ComputedValuesFFI::FfiDisplay {},
        .is_wbr_element = adjusts(ElementStyleAdjustmentFact::IsWbr),
        .disallow_display_contents = adjusts(ElementStyleAdjustmentFact::DisallowDisplayContents),
        .rewrite_inline_flow = adjusts(ElementStyleAdjustmentFact::RewriteInlineFlow),
        .is_button_element = adjusts(ElementStyleAdjustmentFact::IsButton),
        .force_line_height_normal = adjusts(ElementStyleAdjustmentFact::ForceLineHeightNormal),
        .check_input_line_height = adjusts(ElementStyleAdjustmentFact::CheckInputLineHeight),
        .hide_audio_without_controls = adjusts(ElementStyleAdjustmentFact::HideAudioWithoutControls),
        .is_table_element = adjusts(ElementStyleAdjustmentFact::IsTable),
        .force_position_static = adjusts(ElementStyleAdjustmentFact::ForcePositionStatic),
        .force_symbol_display_inline = adjusts(ElementStyleAdjustmentFact::ForceSymbolDisplayInline),
        .webkit_box_layout_transformation_applies = false,
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

void StyleComputer::finalize_style(ComputedStyleWorkingSet& style, DOM::AbstractElement abstract_element, ComputedValuesFFI::FfiStyleFinalizationMode mode) const
{
    bool const animated_box_type = mode == ComputedValuesFFI::FfiStyleFinalizationMode::AnimatedBoxType;
    VERIFY(animated_box_type || mode == ComputedValuesFFI::FfiStyleFinalizationMode::BoxType);
    ComputedValuesFFI::FfiStyleFinalizationInput input {};
    input.mode = mode;
    input.box_type = make_box_type_transformation_input(abstract_element);
    auto line_height_metrics = input_line_height_metrics(style, abstract_element, input.box_type.check_input_line_height);
    auto* animated_overlay = style.prepare_animated_overlay_for_rust_finalization(
        Badge<StyleComputer> {}, animated_box_type ? ComputedStyleWorkingSet::CreateAnimatedOverlay::Yes : ComputedStyleWorkingSet::CreateAnimatedOverlay::No);
    auto finalization = ComputedValuesFFI::rust_finalize_style(
        &input, style.mutable_computed_longhand_table(), animated_overlay, &line_height_metrics);
    style.did_apply_style_finalization_from_rust(finalization.invalidated_longhands);
    style.finish_animated_overlay_rust_mutation(Badge<StyleComputer> {});
}

NonnullRefPtr<ComputedValues const> StyleComputer::create_document_style() const
{
    ensure_style_metadata_tables_installed();

    Vector<u8> document_supported_color_scheme_codes;
    auto document_supported_color_schemes = document().supported_color_schemes();
    if (document_supported_color_schemes.has_value()) {
        document_supported_color_scheme_codes.ensure_capacity(document_supported_color_schemes->size());
        for (auto const& scheme : *document_supported_color_schemes)
            document_supported_color_scheme_codes.unchecked_append(to_underlying(preferred_color_scheme_from_string(scheme)));
    }
    auto length_resolution_context = CSS::Length::ResolutionContext::for_document(document());
    auto viewport_rect = this->viewport_rect();
    ComputedValuesFFI::FfiDocumentLonghandInput const input {
        .color_scheme_input = {
            .preferred_color_scheme = static_cast<u8>(to_underlying(document().page().preferred_color_scheme())),
            .has_document_supported_schemes = document_supported_color_schemes.has_value(),
            .document_supported_scheme_codes = document_supported_color_scheme_codes.data(),
            .document_supported_scheme_count = document_supported_color_scheme_codes.size(),
        },
        .length_resolution_context = to_ffi_length_resolution_context(length_resolution_context),
        .device_pixels_per_css_pixel = m_document->page().client().device_pixels_per_css_pixel(),
        .initial_font_size_raw = InitialValues::font_size().raw_value(),
        .default_font_size_raw = default_user_font_size().raw_value(),
        .viewport_width = viewport_rect.width().to_double(),
        .viewport_height = viewport_rect.height().to_double(),
    };
    auto computed_properties = CSS::ComputedStyleWorkingSet::create_with_longhand_table(ComputedValuesFFI::rust_create_document_longhand_table(&input));
    CSS::ColorResolutionContext color_resolution_context {
        .color_scheme = document().page().preferred_color_scheme(),
        .current_color = CSS::InitialValues::color(),
        .current_color_style_value = &computed_properties->property(PropertyID::Color),
        .calculation_resolution_context = { .length_resolution_context = CSS::Length::ResolutionContext::for_document(document()) },
    };
    auto computed_values = CSS::ComputedValues::create(*computed_properties, document(), document().style_scope(), move(color_resolution_context));
    return computed_values;
}

u64 StyleSharingKey::hash_without_values() const
{
    VERIFY(computation_inputs.size() >= ComputedValues::inherited_style_group_count);
    Fnv1a64 hash;
    for (size_t i = ComputedValues::inherited_style_group_count; i < computation_inputs.size(); ++i)
        hash.add(computation_inputs[i]);
    return hash.value();
}

u64 StyleSharingKey::hash() const
{
    VERIFY(computation_inputs.size() >= ComputedValues::inherited_style_group_count);
    Fnv1a64 hash;
    for (size_t i = ComputedValues::inherited_style_group_count; i < computation_inputs.size(); ++i)
        hash.add(computation_inputs[i]);
    for (auto const& value : pinned_values)
        hash.add(bit_cast<FlatPtr>(value->rust_style_value_data()));
    return hash.value();
}

bool StyleSharingKey::inputs_after_parent_groups_equal(StyleSharingKey const& other) const
{
    if (computation_inputs.size() != other.computation_inputs.size())
        return false;
    VERIFY(computation_inputs.size() >= ComputedValues::inherited_style_group_count);
    for (size_t i = ComputedValues::inherited_style_group_count; i < computation_inputs.size(); ++i) {
        if (computation_inputs[i] != other.computation_inputs[i])
            return false;
    }
    return true;
}

bool StyleSharingKey::values_equal(StyleSharingKey const& other) const
{
    if (pinned_values.size() != other.pinned_values.size())
        return false;
    for (size_t i = 0; i < pinned_values.size(); ++i) {
        if (pinned_values[i]->rust_style_value_data() != other.pinned_values[i]->rust_style_value_data())
            return false;
    }
    return true;
}

bool StyleSharingKey::parent_groups_equal(StyleSharingKey const& other) const
{
    for (size_t i = 0; i < ComputedValues::inherited_style_group_count; ++i) {
        if (computation_inputs[i] == other.computation_inputs[i])
            continue;
        if (!ComputedValuesFFI::rust_style_group_payloads_equal(
                i,
                bit_cast<void const*>(static_cast<FlatPtr>(computation_inputs[i])),
                bit_cast<void const*>(static_cast<FlatPtr>(other.computation_inputs[i]))))
            return false;
    }
    return true;
}

bool StyleSharingKey::equals(StyleSharingKey const& other) const
{
    return inputs_after_parent_groups_equal(other) && values_equal(other) && parent_groups_equal(other);
}

bool StyleSharingKey::equals_without_values(StyleSharingKey const& other) const
{
    return inputs_after_parent_groups_equal(other) && parent_groups_equal(other);
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

static bool computed_content_depends_on_counter_style_environment(StyleValue const& content)
{
    if (!content.is_content())
        return false;
    auto item_depends_on_counter_style_environment = [](auto const& item) {
        return item->is_counter() && item->as_counter().counter_style()->as_counter_style().value().template has<Utf16FlyString>();
    };
    auto const& content_value = content.as_content();
    return any_of(content_value.content().values(), item_depends_on_counter_style_environment)
        || (content_value.alt_text() && any_of(content_value.alt_text()->values(), item_depends_on_counter_style_environment));
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

StyleEngine::StyleRecordDelta StyleComputer::publish_animation_overlay(DOM::AbstractElement abstract_element, ComputedValues const& values) const
{
    auto animated_properties = values.animated_properties();
    Array<void const*, to_underlying(StyleGroupIndex::Count)> payloads;
    ReadonlySpan<void const*> payload_span;
    if (animated_properties) {
        for (size_t index = 0; index < payloads.size(); ++index)
            payloads[index] = values.style_group_payload(static_cast<StyleGroupIndex>(index));
        payload_span = payloads;
    }
    auto publication = const_cast<StyleComputer&>(*this).style_engine().publish_animation_overlay(
        abstract_element.element().style_node_id(),
        pseudo_element_to_ffi(abstract_element.pseudo_element()),
        animated_properties ? animated_properties->identity() : 0,
        animated_properties ? animated_properties->overlay() : nullptr,
        payload_span);
    if (publication.has_value())
        return publication.release_value();
    return publish_computed_style_inputs(abstract_element, values);
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
    bool inherited_group_swap_candidate = false;
    if (abstract_element.has_value() && !abstract_element->pseudo_element().has_value()) {
        auto& element = abstract_element->element();
        inherited_group_swap_candidate = element.style_input_record()
            && !values.has_animated_values() && !values.animated_properties()
            && !element.has_relevant_animations() && !element.has_css_defined_animations()
            && element.property_ids_with_existing_transitions({}).is_empty()
            && element.property_ids_with_matching_transition_property_entry({}).is_empty();
    }
    u64 counter_style_environment_identity = 0;
    bool const is_pseudo = abstract_element.has_value() && abstract_element->pseudo_element().has_value();
    bool const list_style_type_depends_on_counter_style_environment = base.list_style_type_depends_on_counter_style_environment()
        && (is_pseudo || !base.list_style_type_uses_non_overridable_counter_style());
    if (abstract_element.has_value()
        && (computed_content_depends_on_counter_style_environment(base.computed_content())
            || list_style_type_depends_on_counter_style_environment))
        counter_style_environment_identity = abstract_element->style_scope().counter_style_environment_identity();
    auto animated_properties = style_node_id != 0 ? values.animated_properties() : nullptr;
    u64 animation_overlay_identity = animated_properties ? animated_properties->identity() : 0;
    Array<void const*, to_underlying(StyleGroupIndex::Count)> animation_overlay_payloads;
    if (animated_properties) {
        for (size_t index = 0; index < animation_overlay_payloads.size(); ++index)
            animation_overlay_payloads[index] = values.style_group_payload(static_cast<StyleGroupIndex>(index));
    }
    auto pseudo_kind = pseudo_element_to_ffi(abstract_element.has_value() ? abstract_element->pseudo_element() : Optional<CSS::PseudoElement> {});
    auto publication = const_cast<StyleComputer&>(*this).style_engine().publish_computed_groups(style_node_id, pseudo_kind, payloads, ComputedValues::inherited_style_group_count, custom_property_environment ? custom_property_environment->identity() : 0, inherited_group_swap_candidate, counter_style_environment_identity, animation_overlay_identity, animated_properties ? animated_properties->overlay() : nullptr, animated_properties ? animation_overlay_payloads.span() : ReadonlySpan<void const*> {}, base.computed_longhand_table(), custom_property_environment ? custom_property_environment->rust_store() : nullptr);
    return publication;
}

NonnullRefPtr<ComputedValues const> StyleComputer::materialize_style_record(DOM::AbstractElement abstract_element, Optional<bool&> did_change_custom_properties, StyleEngineMatchResult* reusable_matches, Optional<StyleEngine::StyleRecordDelta&> style_record_delta, StyleSharingMode style_sharing_mode) const
{
    auto was_materializing_for_targeted_style_update = m_materializing_for_targeted_style_update;
    m_materializing_for_targeted_style_update = true;
    ScopeGuard restore_materialization_mode = [&] {
        m_materializing_for_targeted_style_update = was_materializing_for_targeted_style_update;
    };
    StyleSharingCandidate sharing;
    sharing.may_reuse_or_publish_shared_style = style_sharing_mode == StyleSharingMode::Enabled;
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
    if (sharing.donor_values)
        previous_base = &sharing.donor_values->base_values();
    auto groups_to_rebuild = sharing.computed_groups_to_rebuild.value_or(ComputedValues::all_style_groups);
    auto& element = abstract_element.element();
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

    // The element's own next computation can be skipped only if this computation read nothing about
    // this one beyond what the record holds. Everything that can read more says so on the element: a
    // container unit or query asks about its container, `attr()` about its attributes, a
    // tree-counting function about its place among its siblings, and `if()` about the environment.
    // The monospace font-size recascade reads the whole ancestor chain rather than the inherited
    // context. `var()` is not among them: what it resolves against is the cascaded custom
    // properties, which the blocks in the record decide, and the inherited environment, which the
    // parent in the record decides. Neither is an animation or transition: it carries state on the
    // element itself, and its values are published by the animation refresh rather than derived here.
    bool const computation_read_only_the_record = !element.style_uses_attr_css_function()
        && !element.style_uses_if_css_function()
        && !element.style_uses_custom_function()
        && !element.style_uses_tree_counting_function()
        && !element.style_depends_on_viewport_metrics()
        && !element.style_depends_on_size_container_query()
        && !element.style_depends_on_style_container_query()
        && !sharing.cascade_font_family_is_monospace;
    // The flags are cleared for the next computation, so the record is what has to answer whether
    // that computation can be skipped.
    if (sharing.style_input_recorded) {
        auto* record = element.style_input_record();
        VERIFY(record);
        record->read_beyond_the_record = !computation_read_only_the_record;
        record->style_uses_var_css_function = element.style_uses_var_css_function();
        record->style_uses_inherit_css_function = element.style_uses_inherit_css_function();
        record->explicitly_inherited_non_inherited_style_groups = sharing.explicitly_inherited_non_inherited_style_groups;
    }
    if (sharing.is_candidate && sharing.may_reuse_or_publish_shared_style) {
        // The result answers for another element only if it also holds no animated values: an
        // animation or transition is state of this element that the other element does not have.
        bool const computation_read_only_the_key = computation_read_only_the_record
            && !computed_values->animated_properties()
            && !computed_values->has_animated_values();
        if (computation_read_only_the_key) {
            auto key_hash = sharing.key.hash();
            Vector<u64> style_input_declaration_words;
            Vector<NonnullRefPtr<StyleValue const>> pinned_style_input_values;
            if (!abstract_element.pseudo_element().has_value()) {
                auto declaration_words = element.style_input_record()->words.span().slice(style_input_record_block_index);
                style_input_declaration_words.append(declaration_words.data(), declaration_words.size());
                pinned_style_input_values = element.style_input_record()->pinned_values;
            }
            if (sharing.explicitly_inherited_non_inherited_style_groups != 0 && !!sharing.parent_style_record_identity)
                pin_style_record(sharing.parent_style_record_identity);
            if (!sharing.key.pinned_values.is_empty()
                && !abstract_element.pseudo_element().has_value()
                && sharing.explicitly_inherited_non_inherited_style_groups == 0) {
                auto& donors = m_style_sharing_donor_index.ensure(sharing.key.hash_without_values());
                if (donors.size() == maximum_style_sharing_donors_per_key)
                    donors.remove(0);
                donors.append(key_hash);
            }
            m_style_sharing_cache.ensure(key_hash).append({
                .key = move(sharing.key),
                .pinned_parent_groups = move(sharing.pinned_parent_groups),
                .style_node_id = abstract_element.pseudo_element().has_value() ? StyleNodeID {} : element.style_node_id(),
                .parent_style_record_identity = sharing.parent_style_record_identity,
                .explicitly_inherited_non_inherited_style_groups = sharing.explicitly_inherited_non_inherited_style_groups,
                .values = computed_values,
                .custom_property_data = abstract_element.custom_property_data(),
                .style_record_identity = {},
                .style_input_declaration_words = move(style_input_declaration_words),
                .pinned_style_input_values = move(pinned_style_input_values),
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
        for (auto const& entry : animated_properties->entries()) {
            auto property_id = static_cast<PropertyID>(entry.property);
            auto group = ComputedValues::style_group_of_property(property_id);
            if (!group.has_value() || property_id == PropertyID::Color) {
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
    auto color_resolution_context = [&] {
        if ((groups_to_apply & (1u << to_underlying(StyleGroupIndex::FontValues))) == 0) {
            return ColorResolutionContext {
                .color_scheme = previous_values.color_scheme(),
                .current_color = InitialValues::color(),
                .current_color_style_value_data = computed_properties.effective_property_data(PropertyID::Color),
                .calculation_resolution_context = { .length_resolution_context = Length::ResolutionContext::for_element(abstract_element, previous_values) },
            };
        }
        auto const& computation_context = get_computation_context_for_property(PropertyID::Color, computed_properties, abstract_element);
        return ColorResolutionContext {
            .color_scheme = computation_context.color_scheme,
            .current_color = InitialValues::color(),
            .current_color_style_value_data = computed_properties.effective_property_data(PropertyID::Color),
            .calculation_resolution_context = { .length_resolution_context = computation_context.length_resolution_context },
        };
    }();

    auto base_values = ComputedValues::Builder { previous_values.base_values() }.build();
    auto animated_values = ComputedValues::create_over_base(computed_properties, document(), style_scope, move(color_resolution_context), *base_values, groups_to_apply);
    ComputedValues::Builder builder { *animated_values };
    ComputedValuesFFI::FfiStyleFinalizationInput finalization_input {};
    finalization_input.mode = ComputedValuesFFI::FfiStyleFinalizationMode::Overflow;
    finalization_input.overflow_x = to_underlying(to_keyword(animated_values->overflow_x()));
    finalization_input.overflow_y = to_underlying(to_keyword(animated_values->overflow_y()));
    auto effective_overflow = ComputedValuesFFI::rust_finalize_style(&finalization_input, nullptr, nullptr, nullptr).overflow;
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
    for (auto const& entry : animated_properties->entries()) {
        auto property_id = static_cast<PropertyID>(entry.property);
        style.set_animated_property(
            Badge<StyleComputer> {}, property_id, animated_properties->property(property_id),
            entry.result_of_transition ? AnimatedPropertyResultOfTransition::Yes : AnimatedPropertyResultOfTransition::No,
            entry.inherited ? ComputedStyleWorkingSet::Inherited::Yes : ComputedStyleWorkingSet::Inherited::No);
    }
}

NonnullRefPtr<ComputedStyleWorkingSet> StyleComputer::reconstruct_computed_properties_for_animation(StyleRecordID style_record) const
{
    auto record = m_style_engine.style_record_view(style_record);
    VERIFY(record.present);
    auto style = ComputedStyleWorkingSet::create_for_animation_update(
        static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(record.longhand_table),
        static_cast<ComputedValuesFFI::AnimatedOverlay const*>(record.animated_overlay));
    if (record.animated_overlay && ComputedValuesFFI::rust_animated_overlay_contains(static_cast<ComputedValuesFFI::AnimatedOverlay const*>(record.animated_overlay), to_underlying(PropertyID::Display))) {
        auto const* box = static_cast<ComputedValuesFFI::BoxValues const*>(record.base_payloads[to_underlying(StyleGroupIndex::BoxValues)]);
        style->set_display_before_box_type_transformation(display_from_ffi_display(box->display));
    }
    return style;
}

// Whether the element's own shape can be read off a fixed set of questions, so that two elements
// answering them the same way are interchangeable to the computation. Anything the computation
// discovers about the element outside these is caught afterwards, when the result is offered for
// sharing; this is only what has to be asked before the computation runs.
static Array<u64, 3> element_shape_style_sharing_key(DOM::AbstractElement abstract_element, Optional<Display> known_parent_display)
{
    auto const shape = make_box_type_transformation_input(
        abstract_element, known_parent_display);
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

        finalize_style(*style, abstract_element, ComputedValuesFFI::FfiStyleFinalizationMode::BoxType);
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
    if (sharing) {
        auto& element = abstract_element.element();
        // A registered or running transition is decided per element from the style it is replacing,
        // and starting one is not something the values carry.
        sharing->is_candidate = inheritance_parent_style_record.present && !element.is_document_element()
            && !inheritance_parent_style_record.animated_overlay
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
            sharing->key.computation_inputs.append(bit_cast<FlatPtr>(group));
        sharing->pinned_parent_groups.set(inherited_group_identities);
        sharing->pinned_parent_custom_property_data = inheritable_custom_property_data(*inheritance_parent);
        sharing->key.computation_inputs.append(0);
        if (previous_style_record.present) {
            auto const* inherited_box = static_cast<ComputedValuesFFI::InheritedBoxValues const*>(previous_style_record.payloads[to_underlying(StyleGroupIndex::InheritedBoxValues)]);
            sharing->key.computation_inputs.append(inherited_box->writing_mode + 1);
        } else {
            sharing->key.computation_inputs.append(0);
        }
        sharing->key.computation_inputs.append(0);
        sharing->key.computation_inputs.append(document().style_environment_version());
        sharing->key.computation_inputs.append(abstract_element.pseudo_element().has_value() ? to_underlying(*abstract_element.pseudo_element()) + 1 : 0);
        sharing->key.computation_inputs.append(cascade_input.matching_pseudo_element_styles);
        sharing->parent_style_record_identity = inheritance_parent->style_record_identity();
        append_element_shape_key(sharing->key.computation_inputs);
    }

    // What this computation is allowed to read, recorded so the next one on this element can ask
    // whether any of it moved. Nothing is answered from it yet: it says how often a recomputation
    // could have been, and which half of its input moved when it could not.
    Vector<StyleProperty> presentational_hint_properties;
    bool collected_presentational_hints = false;
    StyleInputRecord* new_style_input_record = nullptr;
    bool style_input_is_unchanged = false;
    bool only_declarations_changed = false;
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
        bool style_depends_on_viewport_metrics { false };
        bool style_depends_on_size_container_query { false };
        bool style_depends_on_style_container_query { false };
        u32 explicitly_inherited_non_inherited_style_groups { 0 };
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
        record->read_beyond_the_record = true;
        record->style_uses_attr_css_function = false;
        record->style_uses_var_css_function = false;
        record->style_uses_if_css_function = false;
        record->style_uses_custom_function = false;
        record->style_uses_inherit_css_function = false;
        record->style_uses_tree_counting_function = false;
        record->style_depends_on_viewport_metrics = false;
        record->style_depends_on_size_container_query = false;
        record->style_depends_on_style_container_query = false;
        record->explicitly_inherited_non_inherited_style_groups = 0;
        record->cascade_reads_custom_properties = false;
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
        } else {
            auto const inline_style = include_inline_style == IncludeInlineStyle::Yes && cascade_input.inline_style_context_index.has_value()
                ? abstract_element.inline_style()
                : GC::Ptr<CSSStyleProperties const> {};
            auto const dependencies = append_cascade_blocks_to_key(record->words, record->pinned_values, cascade_input, presentational_hint_properties, inline_style, CascadeBlockKeyValueComparison::ByValue);
            record->cascade_reads_custom_properties = dependencies.reads_custom_properties;
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
                record->style_depends_on_viewport_metrics = previous->style_depends_on_viewport_metrics;
                record->style_depends_on_size_container_query = previous->style_depends_on_size_container_query;
                record->style_depends_on_style_container_query = previous->style_depends_on_style_container_query;
                record->explicitly_inherited_non_inherited_style_groups = previous->explicitly_inherited_non_inherited_style_groups;
                break;
            case StyleInputRecord::Difference::ParentStyle:
                counters.element_style_input_changed_by_parent_style++;
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
                    .style_depends_on_viewport_metrics = previous->style_depends_on_viewport_metrics,
                    .style_depends_on_size_container_query = previous->style_depends_on_size_container_query,
                    .style_depends_on_style_container_query = previous->style_depends_on_style_container_query,
                    .explicitly_inherited_non_inherited_style_groups = previous->explicitly_inherited_non_inherited_style_groups,
                };
                break;
            }
        }
        // The buffer the element gives up becomes the next element's, so a pass over a document
        // allocates one record's worth of words and reuses it.
        m_style_input_record_scratch = element.take_style_input_record();
        element.set_style_input_record(move(record));
        new_style_input_record = element.style_input_record();
        sharing->style_input_recorded = true;
    };

    // Nothing this element's last computation read has moved, so the style it produced is still the
    // answer and deriving it again would produce the same one. What the skipped computation would
    // have decided besides the values - the marks it leaves on the element and on its parent - the
    // record carries, because nothing else leaves them.
    auto last_style_still_stands = [&]() -> bool {
        if (!sharing || !sharing->is_candidate || !new_style_input_record)
            return false;
        if (!previous_style_record.present
            || previous_style_record.animated_overlay
            || previous_style_record.animation_overlay_identity != 0)
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
        if (record.style_depends_on_viewport_metrics)
            element.set_style_depends_on_viewport_metrics();
        if (record.style_depends_on_size_container_query)
            element.set_style_depends_on_size_container_query();
        if (record.style_depends_on_style_container_query)
            element.set_style_depends_on_style_container_query();
        if (record.explicitly_inherited_non_inherited_style_groups != 0) {
            if (auto* parent = element.parent())
                parent->add_children_explicitly_inherited_non_inherited_style_groups(record.explicitly_inherited_non_inherited_style_groups);
        }
        auto existing = abstract_element.computed_style();
        VERIFY(existing);
        sharing->reused_values = ComputedValues::Builder { *existing }.build();
    };

    auto reuse_computed_style = [&]() -> bool {
        if (!sharing || !sharing->may_reuse_or_publish_shared_style)
            return false;
        if (!style_input_is_unchanged || new_style_input_record->read_beyond_the_record || !last_style_still_stands())
            return false;
        if (auto data = abstract_element.custom_property_data(); data && data->is_animation_overlay())
            return false;
        reuse_last_computed_style();
        return true;
    };

    bool has_complete_sharing_key = false;
    auto find_shared_style = [&]() {
        if (!sharing || !sharing->may_reuse_or_publish_shared_style)
            return false;
        if (auto data = abstract_element.custom_property_data(); data && data->is_animation_overlay())
            return false;
        auto key_hash = sharing->key.hash();
        auto bucket = m_style_sharing_cache.get(key_hash);
        if (!bucket.has_value())
            return false;
        for (auto const& entry : *bucket) {
            if (!entry.key.equals(sharing->key))
                continue;
            // An entry that read the half of its inherited style the key does not name answers only
            // for an element inheriting from that very style.
            if (entry.explicitly_inherited_non_inherited_style_groups != 0 && entry.parent_style_record_identity != inheritance_parent->style_record_identity())
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
            if (entry.explicitly_inherited_non_inherited_style_groups != 0) {
                if (auto* parent = abstract_element.element().parent())
                    parent->add_children_explicitly_inherited_non_inherited_style_groups(entry.explicitly_inherited_non_inherited_style_groups);
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
                record->explicitly_inherited_non_inherited_style_groups = entry.explicitly_inherited_non_inherited_style_groups;
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
            auto const counters_before_verification = counters;
            auto reused = sharing->reused_values.release_nonnull();
            auto custom_property_data = abstract_element.custom_property_data();
            auto cascaded = compute_cascaded_values(abstract_element, cascade_input, include_inline_style, nullptr,
                collected_presentational_hints ? &presentational_hint_properties : nullptr);
            auto derived = compute_properties(abstract_element, cascaded, cascade_input.matching_pseudo_element_styles, nullptr);
            abstract_element.set_custom_property_data(move(custom_property_data));
            counters = counters_before_verification;
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
        // The style is the one the last cascade produced from these same inputs, so the winner
        // state that cascade bound still stands behind the publication about to reuse it.
        if (auto node = abstract_element.element().style_node_id(); node != 0 && !abstract_element.pseudo_element().has_value())
            const_cast<StyleComputer&>(*this).style_engine().retain_exact_cascade_state(node);
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
        auto const dependencies = append_cascade_blocks_to_key(sharing->key.computation_inputs, sharing->key.pinned_values, cascade_input, presentational_hint_properties, inline_style, CascadeBlockKeyValueComparison::ByIdentity);
        sharing->cascade_reads_custom_properties = dependencies.reads_custom_properties;
        if (dependencies.reads_style_scope)
            sharing->key.computation_inputs[style_sharing_style_scope_index] = style_scope.style_engine_tree_scope().value();

        // The key names every block the cascade will apply, so what those blocks read of the
        // inherited custom property environment is settled here rather than after the cascade.
        if (sharing->cascade_reads_custom_properties && inheritance_parent.has_value()) {
            sharing->pinned_parent_custom_property_data = inheritance_parent->custom_property_data();
            sharing->key.computation_inputs.append(bit_cast<FlatPtr>(sharing->pinned_parent_custom_property_data.ptr()));
        }
        sharing->key.computation_inputs.append(sharing->cascade_reads_custom_properties ? static_cast<FlatPtr>(previous_style_record_identity.value()) : 0);
        sharing->key.computation_inputs.append(sharing->cascade_reads_custom_properties ? m_style_sharing_transaction_generation : 0);

        has_complete_sharing_key = true;
        if (find_shared_style()) {
            if (auto node = abstract_element.element().style_node_id(); node != 0 && !abstract_element.pseudo_element().has_value()) {
                const_cast<StyleComputer&>(*this).style_engine().prepare_shared_exact_cascade_state(node);
                VERIFY(new_style_input_record);
                new_style_input_record->bind_next_published_style = true;
            }
            document().style_invalidation_counters().element_style_shared_computations++;
            return {};
        }
    }

    if (!new_style_input_record)
        record_style_input();

    auto cascade_started_at = MonotonicTime::now();
    auto cascaded_properties = compute_cascaded_values(
        abstract_element,
        cascade_input,
        include_inline_style,
        sharing && sharing->is_candidate && !has_complete_sharing_key ? sharing : nullptr,
        collected_presentational_hints ? &presentational_hint_properties : nullptr);
    document().style_invalidation_counters().style_cascade_microseconds += (MonotonicTime::now() - cascade_started_at).to_microseconds();

    // The inherited custom property environment is named only now, because only the collection above
    // can say whether anything in the cascade reads it. A key already complete before the cascade
    // has named it there.
    if (sharing && sharing->is_candidate && !has_complete_sharing_key && sharing->cascade_reads_custom_properties && inheritance_parent.has_value()) {
        sharing->pinned_parent_custom_property_data = inheritance_parent->custom_property_data();
        sharing->key.computation_inputs.append(bit_cast<FlatPtr>(sharing->pinned_parent_custom_property_data.ptr()));
    }
    if (sharing && sharing->is_candidate && !has_complete_sharing_key)
        sharing->key.computation_inputs.append(sharing->cascade_reads_custom_properties ? static_cast<FlatPtr>(previous_style_record_identity.value()) : 0);
    if (sharing && sharing->is_candidate && !has_complete_sharing_key)
        sharing->key.computation_inputs.append(sharing->cascade_reads_custom_properties ? m_style_sharing_transaction_generation : 0);

    auto find_style_sharing_donor = [&]() -> StyleSharingEntry const* {
        if (previous_style_record.present || !sharing || !sharing->may_reuse_or_publish_shared_style || !sharing->is_candidate || abstract_element.pseudo_element().has_value())
            return nullptr;
        if (sharing->key.pinned_values.is_empty())
            return nullptr;
        auto donors = m_style_sharing_donor_index.get(sharing->key.hash_without_values());
        if (!donors.has_value())
            return nullptr;
        StyleSharingEntry const* best_donor = nullptr;
        size_t best_donor_equal_values = 0;
        for (auto key_hash : donors->in_reverse()) {
            auto bucket = m_style_sharing_cache.get(key_hash);
            if (!bucket.has_value())
                continue;
            for (auto const& entry : *bucket) {
                if (!entry.style_record_identity.has_value() || !entry.style_node_id)
                    continue;
                if (!entry.key.equals_without_values(sharing->key))
                    continue;
                size_t equal_values = 0;
                for (size_t i = 0; i < entry.key.pinned_values.size(); ++i) {
                    if (entry.key.pinned_values[i]->equals(sharing->key.pinned_values[i]))
                        ++equal_values;
                }
                if (!best_donor || equal_values > best_donor_equal_values) {
                    best_donor = &entry;
                    best_donor_equal_values = equal_values;
                }
            }
        }
        return best_donor;
    };

    // What the cascade decided is what the rest of the computation reads, so an element whose
    // cascade came out exactly as it did last time computes the style it already has. A stylesheet
    // arriving mid-load changes which declarations most elements match and which of them win for
    // very few, and this is the case the record's declaration half cannot tell apart on its own.
    bool exact_cascade_is_unchanged = false;
    bool use_retained_style_computation_selection = false;
    StyleRecordID donor_style_record;
    if (auto node = abstract_element.element().style_node_id(); sharing && sharing->may_reuse_or_publish_shared_style && node != 0) {
        auto const* donor = find_style_sharing_donor();
        auto publication = const_cast<StyleComputer&>(*this).style_engine().publish_exact_cascade_state(
            node,
            pseudo_element_to_ffi(abstract_element.pseudo_element()),
            cascaded_properties->rust_store(),
            0,
            donor ? donor->style_node_id : StyleNodeID {},
            donor ? *donor->style_record_identity : StyleRecordID {});
        exact_cascade_is_unchanged = publication.unchanged;
        if (previous_style_record.present
            && only_declarations_changed
            && previous_computation.has_value()
            && !previous_computation->read_beyond_the_record
            && !previous_computation->style_uses_var_css_function
            && !previous_computation->style_uses_inherit_css_function
            && previous_computation->explicitly_inherited_non_inherited_style_groups == 0) {
            sharing->computed_groups_to_rebuild = publication.computed_group_mask & ComputedValues::all_style_groups;
            use_retained_style_computation_selection = true;
        } else if (donor && publication.donor_used) {
            sharing->computed_groups_to_rebuild = publication.computed_group_mask & ComputedValues::all_style_groups;
            sharing->donor_values = donor->values;
            donor_style_record = *donor->style_record_identity;
            use_retained_style_computation_selection = true;
        }
    }
    if (previous_computation.has_value()
        && !previous_computation->read_beyond_the_record
        && exact_cascade_is_unchanged
        // A computation that read the other half of its inherited style, through `inherit` on a
        // non-inherited property, read what the record does not name, so an unchanged cascade does
        // not mean an unchanged answer.
        && previous_computation->explicitly_inherited_non_inherited_style_groups == 0
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
        new_style_input_record->style_depends_on_viewport_metrics = previous_computation->style_depends_on_viewport_metrics;
        new_style_input_record->style_depends_on_size_container_query = previous_computation->style_depends_on_size_container_query;
        new_style_input_record->style_depends_on_style_container_query = previous_computation->style_depends_on_style_container_query;
        new_style_input_record->explicitly_inherited_non_inherited_style_groups = previous_computation->explicitly_inherited_non_inherited_style_groups;
        reuse_last_computed_style();
        return {};
    }

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

    auto previous_computed_style_record = new_style_input_record
            && new_style_input_record->computed_style_record == previous_style_record_identity
        ? previous_style_record_identity
        : StyleRecordID {};
    if (sharing && sharing->donor_values) {
        if (sharing->is_candidate) {
            previous_computed_style_record = donor_style_record;
        } else {
            sharing->donor_values = nullptr;
            sharing->computed_groups_to_rebuild = {};
            use_retained_style_computation_selection = false;
        }
    }
    u32 computed_group_mask = ComputedValues::all_style_groups;
    auto computed_properties = compute_properties(abstract_element, cascaded_properties, cascade_input.matching_pseudo_element_styles,
        sharing ? &sharing->explicitly_inherited_non_inherited_style_groups : nullptr, previous_computed_style_record,
        sharing && sharing->is_candidate ? sharing->computed_groups_to_rebuild.value_or(ComputedValues::all_style_groups) : ComputedValues::all_style_groups,
        use_retained_style_computation_selection, false, &computed_group_mask,
        sharing ? &sharing->cascade_font_family_is_monospace : nullptr);
    if (new_style_input_record)
        new_style_input_record->bind_next_published_style = true;
    static bool const verify_computed_closure = getenv("LIBWEB_VERIFY_COMPUTED_CLOSURE") != nullptr;
    if (verify_computed_closure && computed_group_mask != ComputedValues::all_style_groups) {
        auto custom_property_data = abstract_element.custom_property_data();
        auto counters = document().style_invalidation_counters();
        auto fully_computed_properties = compute_properties(
            abstract_element, cascaded_properties, cascade_input.matching_pseudo_element_styles,
            nullptr, {}, ComputedValues::all_style_groups, false, true);
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

// HACK: This function implements time-travelling inheritance for the font-size property
//       in situations where the cascade ended up with `font-family: monospace`.
//       In such cases, other browsers will magically change the meaning of keyword font sizes
//       *even in earlier stages of the cascade!!* to be relative to the default monospace font size (13px)
//       instead of the default font size (16px).
//       See this blog post for a lot more details about this weirdness:
//       https://manishearth.github.io/blog/2017/08/10/font-size-an-unexpectedly-complex-css-property/
RefPtr<StyleValue const> StyleComputer::recascade_font_size_if_needed(DOM::AbstractElement abstract_element, bool has_monospace_font_family, bool& depends_on_viewport_metrics) const
{
    // Some CSS frameworks use `font-family: monospace, monospace` to work around this behavior.
    if (!has_monospace_font_family)
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

    GC::ConservativeVector<DOM::AbstractElement> recascade_ancestors;
    Vector<u64> ancestor_style_records;
    for (auto& ancestor : ancestors.in_reverse()) {
        recascade_ancestors.append(ancestor);
        ancestor_style_records.append(ancestor.style_record_identity().value());
    }

    auto run_recascade_batch = [&](size_t start_index, CSSPixels current_size, bool current_size_depends_on_viewport_metrics, ComputedValuesFFI::FfiLengthResolutionContext const* length_resolution_context) {
        return ComputedValuesFFI::rust_recascade_font_size_batch(
            m_style_engine.rust_handle(),
            ancestor_style_records.data(),
            ancestor_style_records.size(),
            start_index,
            current_size.raw_value(),
            current_size_depends_on_viewport_metrics,
            default_monospace_font_size_in_px.raw_value(),
            length_resolution_context);
    };

    auto batch = run_recascade_batch(0, default_monospace_font_size_in_px, false, nullptr);
    bool skipped_calculated_value = batch.skipped_calculated_value;
    while (batch.status != ComputedValuesFFI::FontSizeRecascadeStatus::Complete) {
        VERIFY(batch.status == ComputedValuesFFI::FontSizeRecascadeStatus::NeedsLengthResolution);
        VERIFY(batch.next_index < recascade_ancestors.size());
        auto& ancestor = recascade_ancestors[batch.next_index];
        auto ancestor_computed_values = ancestor.computed_style();
        VERIFY(ancestor_computed_values);
        auto font_size_value = ancestor_computed_values->raw_cascaded_font_size();
        VERIFY(font_size_value);
        auto current_size_in_px = CSSPixels::from_raw(batch.current_size_raw);

        bool inherited_font_metrics_depend_on_viewport_metrics = false;
        auto inherited_line_height = InitialValues::line_height();
        if (auto parent_element = ancestor.element_to_inherit_style_from(); parent_element.has_value()) {
            if (auto parent_style = parent_element->computed_style()) {
                inherited_font_metrics_depend_on_viewport_metrics = parent_style->font_metrics_depend_on_viewport_metrics();
                inherited_line_height = parent_style->line_height();
            }
        }

        bool did_resolve_viewport_relative_length = false;
        Length::ResolutionContext resolution_context {
            .viewport_rect = viewport_rect(),
            .font_metrics = { current_size_in_px, monospace_font.with_size(current_size_in_px * 0.75f)->pixel_metrics(), inherited_line_height },
            .root_font_metrics = m_root_element_font_metrics,
            .font_metrics_depend_on_viewport_metrics = batch.depends_on_viewport_metrics || inherited_font_metrics_depend_on_viewport_metrics,
            .root_font_metrics_depend_on_viewport_metrics = m_root_element_font_metrics_depend_on_viewport_metrics,
            .subject_inline_axis_is_horizontal = ancestor_computed_values->writing_mode() == WritingMode::HorizontalTb,
            .subject_element = &ancestor.element(),
        };
        auto ffi_resolution_context = to_ffi_length_resolution_context(resolution_context);
        auto resumed_batch = run_recascade_batch(batch.next_index, current_size_in_px, batch.depends_on_viewport_metrics, &ffi_resolution_context);
        skipped_calculated_value |= resumed_batch.skipped_calculated_value;
        if (resumed_batch.status == ComputedValuesFFI::FontSizeRecascadeStatus::NeedsCppLengthResolution) {
            VERIFY(resumed_batch.next_index == batch.next_index);
            VERIFY(font_size_value->is_length());
            resolution_context.set_did_resolve_viewport_relative_length(did_resolve_viewport_relative_length);
            current_size_in_px = font_size_value->as_length().length().to_px(resolution_context);
            batch = run_recascade_batch(batch.next_index + 1, current_size_in_px, did_resolve_viewport_relative_length, nullptr);
            skipped_calculated_value |= batch.skipped_calculated_value;
        } else {
            batch = resumed_batch;
        }
    }

    if (skipped_calculated_value)
        dbgln("FIXME: Support calc() when time-traveling for monospace font-size");
    depends_on_viewport_metrics = batch.depends_on_viewport_metrics;
    return CSS::LengthStyleValue::create(CSS::Length::make_px(CSSPixels::from_raw(batch.current_size_raw)));
}

void StyleComputer::ensure_style_metadata_tables_installed()
{
    static bool const installed = [] {
        // Transfer one shared Rust reference for every longhand initial value, so
        // initial-value selection never crosses the FFI.
        Vector<void const*> initial_value_entries;
        initial_value_entries.ensure_capacity(number_of_longhand_properties);
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto initial_value = property_initial_value(static_cast<PropertyID>(i));
            initial_value_entries.unchecked_append(StyleValueFFI::rust_style_value_retain(initial_value->rust_style_value_data()));
        }
        ComputedValuesFFI::rust_style_metadata_set_initial_value_table(initial_value_entries.data(), initial_value_entries.size());

        return true;
    }();
    (void)installed;
}

NonnullRefPtr<ComputedStyleWorkingSet> StyleComputer::compute_properties(DOM::AbstractElement abstract_element, CascadedProperties& cascaded_properties, u64 matching_pseudo_element_styles, u32* explicitly_inherited_non_inherited_style_groups, StyleRecordID previous_style_record, u32 initial_computed_group_mask, bool use_retained_style_computation_selection, bool stop_after_longhand_drive, u32* selected_computed_group_mask, bool* cascade_font_family_is_monospace) const
{
    begin_style_update();
    ScopeGuard end_style_update = [&] { this->end_style_update(); };

    ensure_style_metadata_tables_installed();
    VERIFY(computation_context_cache_is_empty());

    Vector<u16> selected_transition_properties;
    if (use_retained_style_computation_selection) {
        auto append_transition_property = [&](PropertyID property_id) {
            selected_transition_properties.append(to_underlying(property_id));
        };
        for (auto property_id : abstract_element.element().property_ids_with_matching_transition_property_entry(abstract_element.pseudo_element()))
            append_transition_property(property_id);
        for (auto property_id : abstract_element.element().property_ids_with_existing_transitions(abstract_element.pseudo_element()))
            append_transition_property(property_id);
    }
    auto inheritance_parent = abstract_element.element_to_inherit_style_from();
    struct CustomPropertyResolutionState {
        NonnullRefPtr<CustomPropertyData const> data;
        RefPtr<CustomPropertyData const> parent_data;
        AbstractOrHypotheticalElement resolution_element;
        SubstitutionData substitution_data;
        ComputedValuesFFI::FfiCascadeResolutionContext resolution_context {};
        FlatPtr document_identity;
        size_t registration_generation;
        Optional<PreferredColorScheme> color_scheme;
        bool root_font_metrics_prepared { false };

        CustomPropertyResolutionState(NonnullRefPtr<CustomPropertyData const> data, RefPtr<CustomPropertyData const> parent_data, DOM::AbstractElement element, FlatPtr document_identity, size_t registration_generation)
            : data(move(data))
            , parent_data(move(parent_data))
            , resolution_element(element)
            , substitution_data(resolution_element, true, true)
            , document_identity(document_identity)
            , registration_generation(registration_generation)
        {
        }
    };
    using PreparePhaseContext = void (*)(void*, u8, ComputedValuesFFI::FfiLonghandPhaseContext*);
    struct NativeLonghandState {
        NonnullRefPtr<ComputedStyleWorkingSet> working_set;
        RefPtr<StyleValue const> new_font_size;
        Vector<u8> document_supported_color_scheme_codes;
        ComputedValuesFFI::FfiEffectiveColorSchemeInput effective_color_scheme_input {};
        ComputedValuesFFI::FfiBoxTypeTransformationInput box_type_input {};
        Optional<DOM::AbstractElement::TreeCountingFunctionResolutionContext> tree_counting_context;
        Vector<ComputedValuesFFI::FfiRandomBaseValue> random_base_values;
        Vector<String> style_sheet_base_urls;
        Vector<ComputedValuesFFI::FfiStyleSheetResourceContext> style_sheet_resource_contexts;
        String document_base_url;
        ComputedValuesFFI::FfiStyleComputationEnvironment computation_environment {};
        OwnPtr<CustomPropertyResolutionState> custom_property_resolution;
        GC::RootVector<GC::Ref<Animations::KeyframeEffect>> animation_effects;
        Vector<AnimationProperties> animation_definitions;
        Vector<TransitionProperties> transitions;
        bool transition_delay_and_duration_are_single_zero { false };
        u64 container_relative_length_unit_mask { 0 };

        explicit NativeLonghandState(NonnullRefPtr<ComputedStyleWorkingSet> working_set)
            : working_set(move(working_set))
        {
        }
    };
    struct NativeComputePropertiesContext {
        GC::Ref<StyleComputer const> style_computer;
        DOM::AbstractElement abstract_element;
        CascadedProperties& cascaded_properties;
        u64 matching_pseudo_element_styles;
        u32* explicitly_inherited_non_inherited_style_groups;
        bool stop_after_longhand_drive;
        u32* selected_computed_group_mask;
        bool* cascade_font_family_is_monospace;
        PreparePhaseContext prepare_phase_context;
        OwnPtr<NativeLonghandState> state;
    };
    auto prepare_phase_context = [](void* context_pointer, u8 phase, ComputedValuesFFI::FfiLonghandPhaseContext* output) {
        auto& context = *static_cast<NativeComputePropertiesContext*>(context_pointer);
        auto& state = *context.state;
        auto& computed_style = *state.working_set;
        auto context_property = phase == ComputedValuesFFI::LONGHAND_PHASE_CONTEXT_AFTER_FONT ? PropertyID::LineHeight : PropertyID::Color;
        auto const& computation_context = context.abstract_element.document().style_computer().get_computation_context_for_property(context_property, computed_style, context.abstract_element);
        bool is_after_line_height = phase == ComputedValuesFFI::LONGHAND_PHASE_CONTEXT_AFTER_LINE_HEIGHT;
        *output = {
            .length_resolution_context = to_ffi_length_resolution_context_with_container_bases(computation_context.length_resolution_context, state.container_relative_length_unit_mask),
            .input_line_height_metrics = is_after_line_height ? input_line_height_metrics(computed_style, context.abstract_element, state.box_type_input.check_input_line_height) : ComputedValuesFFI::FfiInputLineHeightMetrics {},
            .line_height_before_adjustments = is_after_line_height ? computed_style.effective_property_data(PropertyID::LineHeight) : nullptr,
            .custom_property_input = {},
        };
        if (!is_after_line_height || !state.custom_property_resolution)
            return;

        auto& resolution_state = *state.custom_property_resolution;
        auto& document = context.abstract_element.document();
        resolution_state.color_scheme = computed_style.color_scheme(document.page().preferred_color_scheme(), document.supported_color_schemes());
        if (auto cached = resolution_state.data->cached_resolution(resolution_state.document_identity, resolution_state.registration_generation, *resolution_state.color_scheme)) {
            context.abstract_element.set_custom_property_data(move(cached));
            return;
        }
        document.style_invalidation_counters().custom_property_elements++;
        document.style_invalidation_counters().custom_property_resolutions += resolution_state.data->declared_count();
        document.style_invalidation_counters().custom_property_value_computations += resolution_state.data->declared_count();
        output->custom_property_input = {
            .store = resolution_state.data->rust_store(),
            .resolved_parent_store = resolution_state.parent_data ? resolution_state.parent_data->rust_store() : resolution_state.data->parent() ? resolution_state.data->parent()->rust_store()
                                                                                                                                                 : nullptr,
            .reuse_resolved_parent_if_empty = resolution_state.parent_data != nullptr,
            .resolution_context = &resolution_state.resolution_context,
            .finalizer_context = &context,
            .finalize_component = [](void* context_pointer, size_t const* names, u32 const* members, size_t member_count, ComputedValuesFFI::FfiResolvedStyleValue* resolved) {
                auto& context = *static_cast<NativeComputePropertiesContext*>(context_pointer);
                auto& computed_style = *context.state->working_set;
                auto& state = *context.state->custom_property_resolution;
                auto& style_computer = context.abstract_element.document().style_computer();
                if (!state.root_font_metrics_prepared) {
                    if (is<HTML::HTMLHtmlElement>(context.abstract_element.element())) {
                        style_computer.m_root_element_font_metrics = style_computer.calculate_root_element_font_metrics(computed_style);
                        style_computer.m_root_element_font_metrics_depend_on_viewport_metrics = computed_style.font_metrics_depend_on_viewport_metrics();
                    }
                    state.root_font_metrics_prepared = true;
                }
                for (auto member : ReadonlySpan<u32> { members, member_count }) {
                    auto substituted = StyleValue::adopt_rust_style_value_data(
                        static_cast<StyleValueFFI::StyleValueData const*>(resolved[member].data));
                    auto finalized = style_computer.finalize_custom_property_value(
                        &computed_style,
                        context.abstract_element,
                        Utf16FlyString::from_raw(names[member]),
                        move(substituted));
                    resolved[member].data = StyleValueFFI::rust_style_value_retain(finalized->rust_style_value_data());
                }
            },
        };
    };
    NativeComputePropertiesContext native_context {
        .style_computer = *this,
        .abstract_element = abstract_element,
        .cascaded_properties = cascaded_properties,
        .matching_pseudo_element_styles = matching_pseudo_element_styles,
        .explicitly_inherited_non_inherited_style_groups = explicitly_inherited_non_inherited_style_groups,
        .stop_after_longhand_drive = stop_after_longhand_drive,
        .selected_computed_group_mask = selected_computed_group_mask,
        .cascade_font_family_is_monospace = cascade_font_family_is_monospace,
        .prepare_phase_context = prepare_phase_context,
        .state = nullptr,
    };
    ComputedValuesFFI::FfiComputePropertiesInput const input {
        .store = cascaded_properties.rust_store(),
        .style_engine = m_style_engine.rust_handle(),
        .style_node = abstract_element.element().style_node_id().value(),
        .pseudo_kind = pseudo_element_to_ffi(abstract_element.pseudo_element()),
        .previous_style_record = previous_style_record.value(),
        .inheritance_parent_style_record = inheritance_parent.has_value() ? inheritance_parent->style_record_identity().value() : 0,
        .initial_computed_group_mask = initial_computed_group_mask,
        .all_computed_groups = ComputedValues::all_style_groups,
        .use_retained_style_computation_selection = use_retained_style_computation_selection,
        .selected_transition_properties = selected_transition_properties.data(),
        .selected_transition_property_count = selected_transition_properties.size(),
        .has_relevant_animations = abstract_element.element().has_relevant_animations(),
        .has_css_defined_animations = abstract_element.element().has_css_defined_animations(),
        .stop_after_longhand_drive = stop_after_longhand_drive,
        .callback_context = &native_context,
        .prepare_longhand_drive = [](void* context_pointer, ComputedValuesFFI::FfiStyleComputationRequirements const* computation_requirements, ComputedValuesFFI::ComputedLonghandTable* longhand_table, bool parent_has_animated_values, ComputedValuesFFI::FfiLonghandDriveInput* output) {
            auto& context = *static_cast<NativeComputePropertiesContext*>(context_pointer);
            auto& style_computer = *context.style_computer;
            auto abstract_element = context.abstract_element;
            auto computed_group_mask = computation_requirements->computed_group_mask;
            if (context.selected_computed_group_mask)
                *context.selected_computed_group_mask = computed_group_mask;
            if (context.cascade_font_family_is_monospace)
                *context.cascade_font_family_is_monospace = computation_requirements->has_monospace_font_family;
            auto const* computed_properties_to_evaluate = computation_requirements->has_computed_property_selection
                ? computation_requirements->computed_property_words
                : nullptr;
            auto working_set = CSS::ComputedStyleWorkingSet::create_with_longhand_table(longhand_table);
            context.state = make<NativeLonghandState>(move(working_set));
            auto& state = *context.state;
            auto& computed_style = *state.working_set;
            computed_style.set_has_pseudo_element_styles(context.matching_pseudo_element_styles);

            bool recascaded_font_size_depends_on_viewport_metrics = false;
            state.new_font_size = style_computer.recascade_font_size_if_needed(abstract_element, computation_requirements->has_monospace_font_family, recascaded_font_size_depends_on_viewport_metrics);
            if (state.new_font_size) {
                computed_style.set_property(PropertyID::FontSize, *state.new_font_size, ComputedStyleWorkingSet::Inherited::No, Important::No);
                if (recascaded_font_size_depends_on_viewport_metrics) {
                    computed_style.set_depends_on_viewport_metrics();
                    computed_style.set_font_metrics_depend_on_viewport_metrics();
                }
            }

            auto inheritance_parent = abstract_element.element_to_inherit_style_from();
            auto document_supported_color_schemes = style_computer.document().supported_color_schemes();
            if (document_supported_color_schemes.has_value()) {
                state.document_supported_color_scheme_codes.ensure_capacity(document_supported_color_schemes->size());
                for (auto const& scheme : *document_supported_color_schemes)
                    state.document_supported_color_scheme_codes.unchecked_append(to_underlying(preferred_color_scheme_from_string(scheme)));
            }
            state.effective_color_scheme_input = {
                .preferred_color_scheme = static_cast<u8>(to_underlying(style_computer.document().page().preferred_color_scheme())),
                .has_document_supported_schemes = document_supported_color_schemes.has_value(),
                .document_supported_scheme_codes = state.document_supported_color_scheme_codes.data(),
                .document_supported_scheme_count = state.document_supported_color_scheme_codes.size(),
            };
            computed_style.clear_effective_color_scheme();

            state.box_type_input = make_box_type_transformation_input(abstract_element);
            if (computation_requirements->uses_tree_counting_function)
                state.tree_counting_context = abstract_element.tree_counting_function_resolution_context();
            state.random_base_values.ensure_capacity(computation_requirements->unfixed_random_sharing_count);
            for (auto const& sharing : ReadonlySpan<ComputedValuesFFI::FfiUnfixedRandomSharing> { computation_requirements->unfixed_random_sharings, computation_requirements->unfixed_random_sharing_count }) {
                VERIFY(sharing.name != 0);
                RandomCachingKey random_caching_key {
                    .name = Utf16FlyString::from_raw(sharing.name),
                    .element_id = sharing.element_shared
                        ? Optional<UniqueNodeID> { OptionalNone {} }
                        : Optional<UniqueNodeID> { abstract_element.element().unique_id() },
                };
                state.random_base_values.empend(sharing.source, const_cast<DOM::Element&>(abstract_element.element()).ensure_css_random_base_value(random_caching_key));
            }
            if (computation_requirements->environment_requirements & ComputedValuesFFI::CASCADED_ENVIRONMENT_NEEDS_STYLE_SHEET_CONTEXT) {
                state.style_sheet_base_urls.resize(context.cascaded_properties.source_slot_count());
                state.style_sheet_resource_contexts.resize(context.cascaded_properties.source_slot_count());
                for (size_t slot = 0; slot < context.cascaded_properties.source_slot_count(); ++slot) {
                    auto& resource_context = state.style_sheet_resource_contexts[slot];
                    auto source = context.cascaded_properties.source_for_slot(static_cast<u32>(slot));
                    if (!source || !source->parent_rule())
                        continue;
                    auto style_sheet = source->parent_rule()->parent_style_sheet();
                    if (!style_sheet)
                        continue;
                    computed_style.set_style_sheet_for_source_slot(static_cast<u32>(slot), style_sheet);
                    auto base_url = style_sheet->base_url()
                                        .value_or_lazy_evaluated_optional([&]() { return style_sheet->location(); })
                                        .value_or_lazy_evaluated_optional([&]() -> Optional<::URL::URL> {
                                            if (auto document = style_sheet->owning_document())
                                                return HTML::relevant_settings_object(*document).api_base_url();
                                            return {};
                                        });
                    if (base_url.has_value())
                        state.style_sheet_base_urls[slot] = base_url->to_string();
                    resource_context.has_value = true;
                    resource_context.origin_clean = style_sheet->is_origin_clean();
                }
                for (size_t slot = 0; slot < state.style_sheet_resource_contexts.size(); ++slot) {
                    auto bytes = state.style_sheet_base_urls[slot].bytes();
                    state.style_sheet_resource_contexts[slot].base_url = bytes.data();
                    state.style_sheet_resource_contexts[slot].base_url_length = bytes.size();
                }
            }
            if (computation_requirements->environment_requirements & ComputedValuesFFI::CASCADED_ENVIRONMENT_NEEDS_DOCUMENT_BASE_URL)
                state.document_base_url = abstract_element.document().base_url().to_string();
            auto document_base_url_bytes = state.document_base_url.bytes();
            state.computation_environment = {
                .box_type_input = state.box_type_input,
                .color_scheme_input = state.effective_color_scheme_input,
                .is_th_element = abstract_element.element().local_name() == HTML::TagNames::th,
                .has_new_font_size = state.new_font_size != nullptr,
                .has_tree_counting_context = state.tree_counting_context.has_value(),
                .sibling_count = state.tree_counting_context.has_value() ? static_cast<u64>(state.tree_counting_context->sibling_count) : 0,
                .sibling_index = state.tree_counting_context.has_value() ? static_cast<u64>(state.tree_counting_context->sibling_index) : 0,
                .random_base_values = state.random_base_values.data(),
                .random_base_value_count = state.random_base_values.size(),
                .document_base_url = document_base_url_bytes.data(),
                .document_base_url_length = document_base_url_bytes.size(),
                .style_sheet_resource_contexts = state.style_sheet_resource_contexts.data(),
                .style_sheet_resource_context_count = state.style_sheet_resource_contexts.size(),
                .device_pixels_per_css_pixel = style_computer.m_document->page().client().device_pixels_per_css_pixel(),
                .initial_font_size_raw = InitialValues::font_size().raw_value(),
                .default_font_size_raw = style_computer.default_user_font_size().raw_value(),
            };

            auto data = abstract_element.custom_property_data();
            if (data && data->is_animation_overlay())
                data = data->parent();
            if (data && data->declared_count() > 0) {
                bool shares_parent_data = inheritance_parent.has_value() && inheritance_parent->custom_property_data().ptr() == data.ptr();
                if (!shares_parent_data) {
                    auto parent_data = inheritance_parent.has_value() ? inheritable_custom_property_data(*inheritance_parent) : nullptr;
                    state.custom_property_resolution = make<CustomPropertyResolutionState>(data.release_nonnull(), move(parent_data), abstract_element, bit_cast<FlatPtr>(&style_computer.document()), style_computer.document().custom_property_registration_generation());
                    auto inheritance_data = inheritance_parent.has_value() ? inheritance_parent->custom_property_data() : nullptr;
                    auto& resolution = *state.custom_property_resolution;
                    resolution.resolution_context = {
                        .parse_context = &resolution.substitution_data.parse_context,
                        .media_environment = style_computer.cached_media_environment_for_style_update(),
                        .load_media_environment = [](void* context) -> void const* {
                            auto& element = *static_cast<AbstractOrHypotheticalElement*>(context);
                            return element.document().style_computer().ensure_media_environment_for_style_update();
                        },
                        .custom_property_store = resolution.data->rust_store(),
                        .inheritance_custom_property_store = inheritance_data ? inheritance_data->rust_store() : nullptr,
                        .custom_property_registry = style_computer.document().rust_custom_property_registry(),
                        .root_custom_property_name = {},
                        .attributes = resolution.substitution_data.ffi_attributes.data(),
                        .attribute_count = resolution.substitution_data.ffi_attributes.size(),
                        .attribute_names_are_ascii_case_insensitive = abstract_element.element().namespace_uri() == Namespace::HTML && style_computer.document().is_html_document(),
                        .custom_functions = resolution.substitution_data.ffi_functions.data(),
                        .custom_function_count = resolution.substitution_data.ffi_functions.size(),
                        .custom_function_scope_identity = bit_cast<FlatPtr>(&resolution.resolution_element.style_scope()),
                        .callback_context = &resolution.resolution_element,
                        .install_custom_properties = nullptr,
                        .resolve_custom_function = resolve_custom_function_for_substitution,
                        .evaluate_style_query = [](void* context, ComputedValuesFFI::FfiUtf16View source) -> u8 {
                            return evaluate_style_query_for_substitution(*static_cast<AbstractOrHypotheticalElement*>(context), source);
                        },
                        .note_substitution = [](void* context, void const* unresolved_data) {
                            auto& element = *static_cast<AbstractOrHypotheticalElement*>(context);
                            auto& dom_element = element.abstract_element().element();
                            auto unresolved = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                                static_cast<StyleValueFFI::StyleValueData const*>(unresolved_data)));
                            if (unresolved->as_unresolved().includes_var_function())
                                dom_element.set_style_uses_var_css_function();
                            if (unresolved->as_unresolved().includes_attr_function())
                                dom_element.set_style_uses_attr_css_function();
                            if (unresolved->as_unresolved().includes_if_function())
                                dom_element.set_style_uses_if_css_function();
                            if (unresolved->as_unresolved().includes_inherit_function())
                                dom_element.set_style_uses_inherit_css_function();
                            if (unresolved->as_unresolved().includes_dashed_function())
                                dom_element.set_style_uses_custom_function(); },
                    };
                }
            }

            state.container_relative_length_unit_mask = computation_requirements->container_relative_length_unit_mask;
            auto const& font_computation_context = style_computer.get_computation_context_for_property(PropertyID::FontFamily, computed_style, abstract_element);
            *output = {
                .longhand_table = computed_style.mutable_computed_longhand_table(),
                .animated_overlay = parent_has_animated_values
                    ? computed_style.prepare_animated_overlay_for_rust_mutation(Badge<StyleComputer> {})
                    : nullptr,
                .store = context.cascaded_properties.rust_store(),
                .environment = &state.computation_environment,
                .computed_group_mask = computed_group_mask,
                .computed_property_words = computed_properties_to_evaluate,
                .font_length_resolution_context = to_ffi_length_resolution_context_with_container_bases(font_computation_context.length_resolution_context, computation_requirements->container_relative_length_unit_mask),
                .callback_context = &context,
                .prepare_phase_context = context.prepare_phase_context,
            }; },
        .finish_longhand_drive = [](void* context_pointer, ComputedValuesFFI::FfiLonghandDriveResult const* longhand_result) {
            auto& context = *static_cast<NativeComputePropertiesContext*>(context_pointer);
            auto& style_computer = *context.style_computer;
            auto& state = *context.state;
            auto& computed_style = *state.working_set;
            if (state.custom_property_resolution && longhand_result->custom_properties.did_resolve) {
                auto& resolution_state = *state.custom_property_resolution;
                auto const& resolution = longhand_result->custom_properties;
                style_computer.document().style_invalidation_counters().custom_property_overlay_hits += resolution.stats.final_value_hits;
                style_computer.document().style_invalidation_counters().custom_property_value_computations += resolution.stats.final_value_misses;
                style_computer.document().style_invalidation_counters().custom_property_cycle_participants += resolution.stats.cycle_participants;

                OrderedHashMap<Utf16FlyString, StyleProperty> resolved_own;
                for (auto const& property : ReadonlySpan<ComputedValuesFFI::FfiResolvedCustomProperty> { resolution.properties, resolution.count }) {
                    auto name = Utf16FlyString::from_raw(property.name_raw);
                    auto value = StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(property.data));
                    resolved_own.set(name, {
                                               .important = property.important ? Important::Yes : Important::No,
                                               .property_id = PropertyID::Custom,
                                               .value = move(value),
                                           });
                }

                auto& element = context.abstract_element.element();
                bool resolution_read_only_the_environment = !element.style_uses_attr_css_function()
                    && !element.style_uses_if_css_function()
                    && !element.style_uses_custom_function()
                    && !element.style_uses_tree_counting_function();
                RefPtr<CustomPropertyData const> resolved;
                if (resolved_own.is_empty() && resolution_state.parent_data) {
                    resolved = resolution_state.parent_data;
                } else {
                    VERIFY(resolution.rust_store);
                    resolved = style_computer.intern_custom_property_data(
                        CustomPropertyData::create(move(resolved_own), resolution_state.parent_data ? resolution_state.parent_data : resolution_state.data->parent(), resolution.rust_store));
                }
                if (resolution_read_only_the_environment)
                    resolution_state.data->set_cached_resolution(resolution_state.document_identity, resolution_state.registration_generation, resolution_state.color_scheme.value(), resolved);
                context.abstract_element.set_custom_property_data(move(resolved));
            }
            state.transitions.ensure_capacity(longhand_result->transitions.count);
            for (auto const& transition : ReadonlySpan<ComputedValuesFFI::FfiComputedTransition> { longhand_result->transitions.transitions, longhand_result->transitions.count }) {
                Vector<PropertyID> properties;
                properties.ensure_capacity(transition.property_count);
                for (auto property : ReadonlySpan<u16> { transition.properties, transition.property_count })
                    properties.unchecked_append(static_cast<PropertyID>(property));
                auto timing_function = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                    static_cast<StyleValueFFI::StyleValueData const*>(transition.timing_function)));
                state.transitions.unchecked_append({
                    .properties = move(properties),
                    .duration = transition.duration,
                    .timing_function = EasingFunction::from_style_value(timing_function),
                    .delay = transition.delay,
                    .transition_behavior = static_cast<TransitionBehavior>(transition.behavior),
                });
            }
            state.transition_delay_and_duration_are_single_zero = longhand_result->transitions.delay_and_duration_are_single_zero;
            state.animation_definitions.ensure_capacity(longhand_result->animations.count);
            for (auto const& animation : ReadonlySpan<ComputedValuesFFI::FfiComputedAnimation> { longhand_result->animations.animations, longhand_result->animations.count }) {
                Variant<double, Utf16String> duration { animation.duration };
                if (animation.duration_is_auto)
                    duration = "auto"_utf16;
                auto timing_function = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                    static_cast<StyleValueFFI::StyleValueData const*>(animation.timing_function)));
                GC::Ptr<Animations::AnimationTimeline> timeline;
                switch (animation.timeline_kind) {
                case ComputedValuesFFI::FfiAnimationTimelineKind::Document:
                    timeline = context.abstract_element.document().timeline();
                    break;
                case ComputedValuesFFI::FfiAnimationTimelineKind::None:
                    break;
                case ComputedValuesFFI::FfiAnimationTimelineKind::Scroll:
                    timeline = Animations::ScrollTimeline::create(
                        context.abstract_element.document(),
                        Animations::ScrollTimeline::AnonymousSource {
                            .scroller = static_cast<Scroller>(animation.scroll_scroller),
                            .target = context.abstract_element,
                        },
                        Animations::scroll_axis_from_css_axis(static_cast<Axis>(animation.scroll_axis)));
                    break;
                }
                state.animation_definitions.unchecked_append({
                    .duration = move(duration),
                    .timing_function = EasingFunction::from_style_value(timing_function),
                    .iteration_count = animation.iteration_count,
                    .direction = static_cast<AnimationDirection>(animation.direction),
                    .play_state = static_cast<AnimationPlayState>(animation.play_state),
                    .delay = animation.delay,
                    .fill_mode = static_cast<AnimationFillMode>(animation.fill_mode),
                    .composition = static_cast<AnimationComposition>(animation.composition),
                    .name = Utf16FlyString::from_raw(animation.name_raw),
                    .timeline = timeline,
                });
            }
            auto const& driver_results = longhand_result->driver_results;
            style_computer.document().style_invalidation_counters().computed_longhand_evaluations += driver_results.longhand_evaluations;
            if (driver_results.uses_tree_counting_function)
                context.abstract_element.element().set_style_uses_tree_counting_function();

            auto invalidate_post_adjusted_longhand = [&](u8 flag, PropertyID property_id) {
                if (driver_results.post_adjusted_longhands & flag)
                    computed_style.did_store_property_data_from_drive(property_id);
            };
            invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_FLOAT, PropertyID::Float);
            invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_DISPLAY, PropertyID::Display);
            invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_LINE_HEIGHT, PropertyID::LineHeight);
            invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_POSITION, PropertyID::Position);
            invalidate_post_adjusted_longhand(ComputedValuesFFI::POST_ADJUSTED_TEXT_ALIGN, PropertyID::TextAlign);
            if (!context.stop_after_longhand_drive && driver_results.explicitly_inherited_non_inherited_style_groups != 0) {
                auto style_groups = driver_results.explicitly_inherited_non_inherited_style_groups;
                if (style_groups == NumericLimits<u32>::max())
                    style_groups = ComputedValues::all_style_groups;
                if (auto* parent = context.abstract_element.element().parent())
                    parent->add_children_explicitly_inherited_non_inherited_style_groups(style_groups);
                if (context.explicitly_inherited_non_inherited_style_groups)
                    *context.explicitly_inherited_non_inherited_style_groups |= style_groups;
            }
            if (!context.stop_after_longhand_drive && is<HTML::HTMLHtmlElement>(context.abstract_element.element())) {
                style_computer.m_root_element_font_metrics = style_computer.calculate_root_element_font_metrics(computed_style);
                style_computer.m_root_element_font_metrics_depend_on_viewport_metrics = computed_style.font_metrics_depend_on_viewport_metrics();
            }
            style_computer.clear_computation_context_caches(); },
        .process_animation_definitions = [](void* context_pointer) {
            auto& context = *static_cast<NativeComputePropertiesContext*>(context_pointer);
            context.style_computer->process_animation_definitions(*context.state->working_set, context.cascaded_properties, context.abstract_element, context.state->animation_definitions.span());
            context.style_computer->m_keyframes_inherited_non_inherited_style_groups = 0; },
        .prepare_animations = [](void* context_pointer) -> bool {
            auto& context = *static_cast<NativeComputePropertiesContext*>(context_pointer);
            auto animations = context.abstract_element.element().get_animations_internal(
                Animations::Animatable::GetAnimationsSorted::Yes,
                Animations::Animatable::GetAnimationsOptions { .subtree = false, .pseudo_element = {} });
            if (animations.is_exception()) {
                dbgln("Error getting animations for element {}", context.abstract_element.debug_description());
                return false;
            }
            for (auto& animation : animations.value()) {
                if (auto effect = animation->effect(); effect && effect->is_keyframe_effect()) {
                    auto& keyframe_effect = *static_cast<Animations::KeyframeEffect*>(effect.ptr());
                    if (keyframe_effect.pseudo_element_type() == context.abstract_element.pseudo_element())
                        context.state->animation_effects.append(keyframe_effect);
                }
            }
            return !context.state->animation_effects.is_empty();
        },
        .apply_animations = [](void* context_pointer, bool should_measure_line_height, ComputedValuesFFI::FfiInputLineHeightMetrics* line_height_metrics) -> ComputedValuesFFI::AnimatedOverlay* {
            auto& context = *static_cast<NativeComputePropertiesContext*>(context_pointer);
            auto& computed_style = *context.state->working_set;
            context.style_computer->collect_animations_into(context.abstract_element, context.state->animation_effects.span(), computed_style, AnimationRefresh::No);
            *line_height_metrics = input_line_height_metrics(computed_style, context.abstract_element, should_measure_line_height);
            return computed_style.prepare_animated_overlay_for_rust_finalization(
                Badge<StyleComputer> {}, ComputedStyleWorkingSet::CreateAnimatedOverlay::No); },
        .did_mutate_post_compute = [](void* context_pointer, u16 invalidated_longhands) {
            auto& context = *static_cast<NativeComputePropertiesContext*>(context_pointer);
            context.state->working_set->did_apply_style_finalization_from_rust(invalidated_longhands); },
        .finish_properties = [](void* context_pointer, bool parent_style_in_display_none_subtree) {
            auto& context = *static_cast<NativeComputePropertiesContext*>(context_pointer);
            auto& style_computer = *context.style_computer;
            auto& computed_style = *context.state->working_set;
            computed_style.finish_animated_overlay_rust_mutation(Badge<StyleComputer> {});
            if (context.stop_after_longhand_drive)
                return;

            // Transition declarations [css-transitions-1]
            // Theoretically this should be part of the cascade, but it works with computed values.
            compute_transitioned_properties(move(context.state->transitions), context.state->transition_delay_and_duration_are_single_zero, context.abstract_element);
            if (auto previous_style = context.abstract_element.computed_style()) {
                // https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
                if (!previous_style->in_display_none_subtree() && !parent_style_in_display_none_subtree)
                    style_computer.start_needed_transitions(computed_style, context.abstract_element);
            }

            if (style_computer.m_keyframes_inherited_non_inherited_style_groups != 0) {
                if (auto* parent = context.abstract_element.element().parent())
                    parent->add_children_explicitly_inherited_non_inherited_style_groups(style_computer.m_keyframes_inherited_non_inherited_style_groups);
                if (context.explicitly_inherited_non_inherited_style_groups)
                    *context.explicitly_inherited_non_inherited_style_groups |= style_computer.m_keyframes_inherited_non_inherited_style_groups;
                style_computer.m_keyframes_inherited_non_inherited_style_groups = 0;
            } },
    };
    ComputedValuesFFI::rust_compute_properties(&input);
    return native_context.state->working_set;
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
    begin_style_update();
    ScopeGuard end_style_update = [&] { this->end_style_update(); };
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
    auto& document = element.document();
    SubstitutionData substitution_data { element, true, true };

    auto custom_property_data = element.custom_property_data();
    auto inheritance_data = element.element_to_inherit_style_from().map([](auto const& parent) {
                                                                       return parent.custom_property_data();
                                                                   })
                                .value_or(nullptr);
    ComputedValuesFFI::FfiCascadeResolutionContext resolution_context {
        .parse_context = &substitution_data.parse_context,
        .media_environment = cached_media_environment_for_style_update(),
        .load_media_environment = [](void* context) -> void const* {
            auto& element = *static_cast<AbstractOrHypotheticalElement*>(context);
            return element.document().style_computer().ensure_media_environment_for_style_update();
        },
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
        .install_custom_properties = nullptr,
        .resolve_custom_function = resolve_custom_function_for_substitution,
        .evaluate_style_query = [](void* context, ComputedValuesFFI::FfiUtf16View source) -> u8 {
            auto& element = *static_cast<AbstractOrHypotheticalElement*>(context);
            return evaluate_style_query_for_substitution(element, source);
        },
        .note_substitution = nullptr,
    };
    ComputedValuesFFI::FfiUnresolvedStyleValue input {
        .property_id = to_underlying(property.id()),
        .root_custom_property_name = resolution_context.root_custom_property_name,
        .data = unresolved.rust_style_value_data(),
        .resolve_substitutions = true,
    };
    ComputedValuesFFI::FfiResolvedStyleValue output {};
    ComputedValuesFFI::rust_resolve_unresolved_style_values(
        &resolution_context, &input, 1, &output, nullptr, nullptr);
    VERIFY(output.data);
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(output.data));
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_value_of_custom_property(ComputedStyleWorkingSet const* computed_style_for_custom_property_resolution, AbstractOrHypotheticalElement const& element, Utf16FlyString const& name, DeclaredValueSource declared_value_source) const
{
    // https://drafts.csswg.org/css-variables/#propdef-
    // The computed value of a custom property is its specified value with any arbitrary-substitution functions replaced.
    // FIXME: These should probably be part of the computed style itself.
    auto& document = element.document();

    document.style_invalidation_counters().custom_property_value_computations++;
    auto registration = element.get_registered_custom_property(name);

    auto value = [&]() -> RefPtr<StyleValue const> {
        if (declared_value_source == DeclaredValueSource::PublishedEnvironment)
            return element.get_custom_property(name);
        auto data = element.custom_property_data();
        if (data && data->is_animation_overlay())
            data = data->parent();
        if (!data)
            return nullptr;
        if (auto const* property = data->get(name))
            return property->value;
        return nullptr;
    }();
    auto resolved_value = value ? value.release_nonnull() : initial_custom_property_value(registration, document);

    return finalize_custom_property_value(computed_style_for_custom_property_resolution, element, name, move(resolved_value));
}

NonnullRefPtr<StyleValue const> StyleComputer::finalize_custom_property_value(ComputedStyleWorkingSet const* computed_style_for_custom_property_resolution, AbstractOrHypotheticalElement const& element, Utf16FlyString const& name, NonnullRefPtr<StyleValue const> resolved_value) const
{
    auto& document = element.document();
    auto registration = element.get_registered_custom_property(name);

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
        if (!registration.has_value() || registration->syntax.is_universal()) {
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

    if (!registration.has_value() || registration->syntax.is_universal())
        return resolved_value;

    auto resolved_value_contains_attr_tainted_values = resolved_value->is_unresolved() && resolved_value->as_unresolved().contains_attr_tainted_values();
    auto parsed_value = [&]() -> NonnullRefPtr<StyleValue const> {
        auto registration_generation = document.custom_property_registration_generation();
        auto& parses = m_registered_custom_property_parses.ensure(resolved_value->rust_style_value_data());
        for (auto const& parse : parses) {
            if (parse.syntax_identity == registration->syntax.data() && parse.registration_generation == registration_generation)
                return parse.parsed;
        }
        auto parsing_params = Parser::ParsingParams { document };
        parsing_params.value_context.append(PropertyID::Custom);
        auto source = resolved_value->is_unresolved()
            ? resolved_value->as_unresolved().token_source()
            : resolved_value->to_utf16_string(SerializationMode::ResolvedValueForReparse);
        auto parsed = Parser::parse_with_a_syntax(parsing_params, source, registration->syntax);
        parses.append({ resolved_value, registration->syntax.data(), registration_generation, parsed });
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

}
