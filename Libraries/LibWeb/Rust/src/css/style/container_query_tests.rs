/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::computed_values::{
    BoxValues, MiscResetValues, clone_group_payload, default_group_payload, release_group_payload,
};
use crate::css::custom_properties::{
    CustomPropertyRegistry, FfiCustomPropertyRegistration, FfiCustomPropertyRegistryContext,
    rust_custom_property_registry_create, rust_custom_property_registry_update,
};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::syntax::{SyntaxNode, rust_parse_syntax};
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use crate::css::table_group_builder::group_index;
use std::sync::Arc;

fn engine_with_containers() -> (StyleEngine, [StyleNodeID; 3]) {
    let mut engine = StyleEngine::new();
    let mut raw = [0; 3];
    engine.allocate_style_nodes(&mut raw);
    let nodes = raw.map(|raw| StyleNodeID::from_raw(raw).unwrap());
    for (index, node) in nodes.iter().copied().enumerate() {
        let parent = index.checked_sub(1).map(|index| nodes[index]);
        engine.record_tree_delta(
            node,
            None,
            Some(TreeRelations {
                parent,
                ..TreeRelations::detached(TreeScopeID::DOCUMENT)
            }),
        );
    }
    super::tests::discard_transaction(&mut engine);
    assert_eq!(engine.tree.flat_tree_parent(nodes[1]), Some(nodes[0]));
    assert_eq!(engine.tree.flat_tree_parent(nodes[2]), Some(nodes[1]));
    publish_box(
        &mut engine,
        nodes[0],
        bridge::ElementBoxKind::FromDisplay,
        crate::css::display::FfiDisplay::block(),
        crate::css::css_enums::appearance::AUTO,
    );
    publish_box(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::FromDisplay,
        crate::css::display::FfiDisplay::block(),
        crate::css::css_enums::appearance::AUTO,
    );
    set_width(&mut engine, nodes[0], 500);
    set_width(&mut engine, nodes[1], 200);
    engine.evaluate_size_containers_needing_evaluation_after_layout();
    (engine, nodes)
}

fn publish_box(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    kind: bridge::ElementBoxKind,
    display: crate::css::display::FfiDisplay,
    appearance: u8,
) -> u64 {
    publish_box_with_types(engine, node, kind, display, appearance, (false, true, false))
}

fn publish_box_with_types(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    kind: bridge::ElementBoxKind,
    display: crate::css::display::FfiDisplay,
    appearance: u8,
    types: (bool, bool, bool),
) -> u64 {
    engine.set_element_box_kind(node, kind);
    let payloads: Vec<_> = (0..group_index::COUNT)
        .map(|index| {
            // SAFETY: The source is the immutable default payload for this group.
            SharedPayload::new(unsafe { clone_group_payload(index, default_group_payload(index)) }.cast_const())
        })
        .collect();
    // SAFETY: The clones are privately owned and unpublished, with the indicated group layouts.
    unsafe {
        let box_values = &mut *payloads[group_index::BOX].as_ptr().cast_mut().cast::<BoxValues>();
        box_values.display = display;
        box_values.is_size_container = types.0;
        box_values.is_inline_size_container = types.1;
        box_values.is_scroll_state_container = types.2;
        (*payloads[group_index::MISC_RESET]
            .as_ptr()
            .cast_mut()
            .cast::<MiscResetValues>())
        .appearance = appearance;
    }
    let publication = engine.publish_computed_groups(
        computed::ComputedStyleTarget::new(node, u8::MAX),
        &payloads,
        computed::ENGINE_INHERITED_GROUP_COUNT,
        0,
        computed::ComputedMetadataInput {
            pseudo_element_styles: 0,
            dependency_flags: 0,
            counter_style_environment_identity: 0,
            animation_overlay_identity: 0,
            animated_overlay: HostShared::null(),
            animation_overlay_payloads: &[],
            longhand_table: HostShared::null(),
        },
    );
    let record = publication.style_record_identity.raw();
    engine.set_held_style_record(node, record);
    for (index, payload) in payloads.iter().enumerate() {
        release_group_payload(index, payload.as_ptr());
    }
    record
}

fn set_width(engine: &mut StyleEngine, node: StyleNodeID, width: i32) {
    engine.apply_layout_style_snapshot_commit(&[crate::layout::style_snapshot::CommittedGeometry {
        node,
        content_width_raw: crate::css::css_pixels::CssPixels::from_integer(i64::from(width)).raw_value(),
        content_height_raw: 0,
        has_committed_box: true,
    }]);
}

fn length_registry() -> Arc<CustomPropertyRegistry> {
    let initial = RetainedStyleValueData::from_owned(StyleValueData::Length {
        value: 0.0,
        unit: crate::css::style_compute::px_length_unit(),
    });
    let syntax = rust_parse_syntax(FfiUtf16View::from_ascii(b"<length>"), false);
    let registration = FfiCustomPropertyRegistration {
        name: FfiUtf16View::from_ascii(b"--width"),
        syntax,
        inherits: false,
        has_initial_value: true,
        initial_value: FfiUtf16View::from_ascii(b"0px"),
        computed_initial_value: initial.pointer().cast(),
    };
    let context = FfiCustomPropertyRegistryContext {
        document_url: std::ptr::null(),
        document_url_length: 0,
        document_base_url: std::ptr::null(),
        document_base_url_length: 0,
    };
    // SAFETY: The registry, syntax, strings, and initial value are live for the call; update consumes the old registry.
    let registry = unsafe {
        rust_custom_property_registry_update(
            rust_custom_property_registry_create(),
            &raw const context,
            &raw const registration,
            1,
        )
    };
    // SAFETY: Both APIs return one owned Arc reference. The registry copied the parsed syntax.
    unsafe {
        drop(Arc::from_raw(syntax.cast::<SyntaxNode>()));
        Arc::from_raw(registry.cast::<CustomPropertyRegistry>())
    }
}

fn resolve_width(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    pseudo: Option<u8>,
    registry: Option<&CustomPropertyRegistry>,
    settled: Option<&custom_property_cascade::RegisteredValueContext>,
) -> (RetainedStyleValueData, Option<container_queries::ContainerVerdict>) {
    let name: Vec<_> = "--width".encode_utf16().collect();
    let fly = ak::Utf16FlyString::from_utf16(&name);
    let atom = engine.intern_atom(fly.raw_identity());
    // SAFETY: The fly string is live and the engine retains its noted name.
    unsafe {
        engine.note_custom_property_name(atom, fly.raw_identity(), &name);
    }
    let source: Vec<_> = "10cqw".encode_utf16().collect();
    let value = RetainedStyleValueData::from_owned(crate::css::parser::value_parser::unresolved_value_from_string(
        &source,
        Default::default(),
    ));
    let declaration = program::CustomDeclaration {
        name: atom,
        important: false,
        operator: cascade::CascadeOperator::Declared,
        value: cascade::SpecifiedValueID(value.pointer() as u64),
    };
    let inputs = bridge::FfiDocumentStyleComputationInputs {
        custom_property_registry: registry
            .map_or_default(|registry| bridge::FfiHostHandle::from_pointer(std::ptr::from_ref(registry).cast())),
        viewport_width: 800.0,
        viewport_height: 600.0,
        ..Default::default()
    };
    engine.document_style_computation_inputs = inputs;
    let environment = engine
        .engine_custom_property_environment_over(node, pseudo, vec![(declaration, value)], 0, &inputs, settled)
        .unwrap_or_else(|error| panic!("registered value should resolve: {error:?}"));
    let value = engine
        .custom_property_environments
        .store(environment)
        .unwrap()
        .get(fly.raw_identity())
        .unwrap()
        .value
        .clone();
    let effects = engine.take_container_effects_for_host(node);
    (value, effects)
}

fn px(value: &RetainedStyleValueData) -> f64 {
    match value.data() {
        StyleValueData::Length { value, unit } if *unit == crate::css::style_compute::px_length_unit() => *value,
        _ => panic!("expected a computed absolute length"),
    }
}

fn uses_container(effects: &Option<container_queries::ContainerVerdict>, expected: StyleNodeID) -> bool {
    effects.as_ref().is_some_and(|effects| {
        effects
            .effects
            .iter()
            .any(|(node, kind)| *node == expected && matches!(kind, bridge::FfiContainerEffectKind::SizeContainerUsage))
    })
}

#[test]
fn registered_pseudo_finalization() {
    let (mut engine, nodes) = engine_with_containers();
    let registry = length_registry();
    let (value, effects) = resolve_width(
        &mut engine,
        nodes[1],
        Some(super::publication::pseudo_kind::BEFORE),
        Some(&registry),
        None,
    );
    assert_eq!(px(&value), 20.0);
    assert!(uses_container(&effects, nodes[1]));
}

#[test]
fn registered_pseudo_resize_and_retarget() {
    let (mut engine, nodes) = engine_with_containers();
    let registry = length_registry();
    set_width(&mut engine, nodes[1], 300);
    let (value, effects) = resolve_width(
        &mut engine,
        nodes[1],
        Some(super::publication::pseudo_kind::BEFORE),
        Some(&registry),
        None,
    );
    assert_eq!(px(&value), 30.0);
    assert!(uses_container(&effects, nodes[1]));
    publish_box(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::FromDisplay,
        crate::css::display::FfiDisplay::none(),
        crate::css::css_enums::appearance::AUTO,
    );
    let (retargeted, effects) = resolve_width(
        &mut engine,
        nodes[1],
        Some(super::publication::pseudo_kind::BEFORE),
        Some(&registry),
        None,
    );
    assert_eq!(px(&retargeted), 50.0);
    assert!(uses_container(&effects, nodes[0]));
    assert!(!uses_container(&effects, nodes[1]));
}

#[test]
fn registered_pseudo_settled_context() {
    let (mut engine, nodes) = engine_with_containers();
    let registry = length_registry();
    let settled = custom_property_cascade::RegisteredValueContext {
        length: crate::css::style_compute::FfiLengthResolutionContext {
            container_width_basis: 999.0,
            has_container_width_basis: true,
            ..Default::default()
        },
        color_scheme: 0,
    };
    let (value, effects) = resolve_width(
        &mut engine,
        nodes[1],
        Some(super::publication::pseudo_kind::BEFORE),
        Some(&registry),
        Some(&settled),
    );
    assert_eq!(px(&value), 20.0);
    assert!(uses_container(&effects, nodes[1]));
}

#[test]
fn unregistered_pseudo_preserves_tokens() {
    let (mut engine, nodes) = engine_with_containers();
    let (value, effects) = resolve_width(
        &mut engine,
        nodes[1],
        Some(super::publication::pseudo_kind::BEFORE),
        None,
        None,
    );
    assert_eq!(
        String::from_utf16(&crate::css::serialize::serialize_style_value_to_utf16(value.data())).unwrap(),
        "10cqw"
    );
    assert!(effects.is_none());
}

#[test]
fn registered_ordinary_ancestor() {
    let (mut engine, nodes) = engine_with_containers();
    let registry = length_registry();
    let (value, effects) = resolve_width(&mut engine, nodes[1], None, Some(&registry), None);
    assert_eq!(px(&value), 50.0);
    assert!(uses_container(&effects, nodes[0]));
}

#[test]
fn direct_pseudo_origin() {
    let (engine, nodes) = engine_with_containers();
    let mut length = crate::css::style_compute::FfiLengthResolutionContext::default();
    engine.container_unit_bases(nodes[1], true).apply_to(&mut length);
    assert_eq!(length.container_width_basis, 200.0);
}

#[test]
fn box_kind_row_refresh() {
    let (mut engine, nodes) = engine_with_containers();
    let node = nodes[1];
    let record = publish_box(
        &mut engine,
        node,
        bridge::ElementBoxKind::Image,
        crate::css::display::FfiDisplay::inline(),
        crate::css::css_enums::appearance::AUTO,
    );
    assert!(engine.container_query_inputs(node).unwrap().size_queries_available);
    engine.set_element_box_kind(node, bridge::ElementBoxKind::FromDisplay);
    assert!(!engine.container_query_inputs(node).unwrap().size_queries_available);
    assert_eq!(engine.held_style_records.get(&node), Some(&record));
    engine.set_element_box_kind(node, bridge::ElementBoxKind::Image);
    assert!(engine.container_query_inputs(node).unwrap().size_queries_available);
}

#[test]
fn native_appearance_eligibility() {
    for kind in [
        bridge::ElementBoxKind::InputButton,
        bridge::ElementBoxKind::InputCheckBox,
        bridge::ElementBoxKind::InputRadioButton,
        bridge::ElementBoxKind::InputRange,
        bridge::ElementBoxKind::InputText,
    ] {
        let (mut engine, nodes) = engine_with_containers();
        engine.set_element_construction_facts(nodes[1], bridge::element_construction_fact::IS_HTML_INPUT_ELEMENT);
        publish_box(
            &mut engine,
            nodes[1],
            kind,
            crate::css::display::FfiDisplay::inline(),
            crate::css::css_enums::appearance::AUTO,
        );
        assert!(engine.container_query_inputs(nodes[1]).unwrap().size_queries_available);
        publish_box(
            &mut engine,
            nodes[1],
            kind,
            crate::css::display::FfiDisplay::inline(),
            crate::css::css_enums::appearance::NONE,
        );
        assert!(
            engine.container_query_inputs(nodes[1]).unwrap().size_queries_available,
            "{kind:?}"
        );
    }
}

#[test]
fn native_appearance_atomic_controls() {
    let (mut engine, nodes) = engine_with_containers();
    publish_box(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::InputButton,
        crate::css::display::FfiDisplay::from_single_keyword(crate::css::css_enums::keyword::INLINE_BLOCK).unwrap(),
        crate::css::css_enums::appearance::NONE,
    );
    assert!(engine.container_query_inputs(nodes[1]).unwrap().size_queries_available);
    publish_box(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::Image,
        crate::css::display::FfiDisplay::inline(),
        crate::css::css_enums::appearance::NONE,
    );
    assert!(engine.container_query_inputs(nodes[1]).unwrap().size_queries_available);
}

#[test]
fn box_kind_change_invalidates_same_size_dependents() {
    let (mut engine, nodes) = engine_with_containers();
    publish_box(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::Image,
        crate::css::display::FfiDisplay::inline(),
        crate::css::css_enums::appearance::AUTO,
    );
    engine.set_element_size_container_query_facts(nodes[1], true, false);
    engine.set_element_size_container_query_facts(nodes[2], false, true);
    engine.set_element_box_kind(nodes[1], bridge::ElementBoxKind::FromDisplay);
    assert!(engine.has_size_containers_needing_evaluation_after_layout());
    engine.evaluate_size_containers_needing_evaluation_after_layout();
    assert!(engine.row_inputs_moved.containers_moved(nodes[2]));
}

#[test]
fn no_box_skips_query_container() {
    for display in [
        crate::css::display::FfiDisplay::block(),
        crate::css::display::FfiDisplay::from_single_keyword(crate::css::css_enums::keyword::INLINE_BLOCK).unwrap(),
    ] {
        let (mut engine, nodes) = engine_with_containers();
        publish_box(
            &mut engine,
            nodes[1],
            bridge::ElementBoxKind::NoBox,
            display,
            crate::css::css_enums::appearance::AUTO,
        );
        engine.apply_layout_style_snapshot_commit(&[crate::layout::style_snapshot::CommittedGeometry {
            node: nodes[1],
            content_width_raw: 0,
            content_height_raw: 0,
            has_committed_box: false,
        }]);
        assert!(!engine.container_query_inputs(nodes[1]).unwrap().size_queries_available);
        let mut length = crate::css::style_compute::FfiLengthResolutionContext::default();
        engine.container_unit_bases(nodes[2], false).apply_to(&mut length);
        assert_eq!(length.container_width_basis, 500.0);
    }
}

#[test]
fn newly_eligible_unqueried_container_invalidates_dependents() {
    let (mut engine, nodes) = engine_with_containers();
    publish_box(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::FromDisplay,
        crate::css::display::FfiDisplay::inline(),
        crate::css::css_enums::appearance::AUTO,
    );
    engine.evaluate_size_containers_needing_evaluation_after_layout();
    engine.set_element_size_container_query_facts(nodes[1], false, false);
    engine.set_element_size_container_query_facts(nodes[2], false, true);
    engine.set_element_box_kind(nodes[1], bridge::ElementBoxKind::Image);
    engine.size_query_container_scan_visits(true);
    engine.record_size_container_query_dependents(nodes[1]);
    assert_eq!(engine.size_query_container_scan_visits(true), 0);
    assert!(engine.has_size_containers_needing_evaluation_after_layout());
    engine.evaluate_size_containers_needing_evaluation_after_layout();
    assert!(engine.row_inputs_moved.containers_moved(nodes[2]));
    engine.size_query_container_scan_visits(true);
    engine.record_size_container_query_dependents(nodes[1]);
    assert_eq!(engine.size_query_container_scan_visits(true), 0);
}

#[test]
fn box_kind_no_change_and_no_held_controls() {
    let (mut engine, nodes) = engine_with_containers();
    engine.set_element_box_kind(nodes[1], bridge::ElementBoxKind::FromDisplay);
    assert!(!engine.has_size_containers_needing_evaluation_after_layout());
    engine.set_element_box_kind(nodes[2], bridge::ElementBoxKind::Image);
    assert!(engine.container_query_inputs(nodes[2]).is_none());
    assert!(!engine.has_size_containers_needing_evaluation_after_layout());
}

#[test]
fn input_construction_fact_keeps_generic_boxes_atomic() {
    for appearance in [
        crate::css::css_enums::appearance::AUTO,
        crate::css::css_enums::appearance::NONE,
    ] {
        for kind in [bridge::ElementBoxKind::FromDisplay, bridge::ElementBoxKind::NoBox] {
            let (mut engine, nodes) = engine_with_containers();
            engine.set_element_construction_facts(nodes[1], bridge::element_construction_fact::IS_HTML_INPUT_ELEMENT);
            publish_box(
                &mut engine,
                nodes[1],
                kind,
                crate::css::display::FfiDisplay::inline(),
                appearance,
            );
            assert_eq!(
                engine.container_query_inputs(nodes[1]).unwrap().size_queries_available,
                kind != bridge::ElementBoxKind::NoBox
            );
        }
    }
}

fn assert_scroll_only_publication_does_not_scan(unrelated_dependent: bool) {
    let (mut engine, nodes) = engine_with_containers();
    engine.container_query_inputs.clear(nodes[1]);
    engine.set_element_size_container_query_facts(nodes[0], false, unrelated_dependent);
    engine.size_query_container_scan_visits(true);
    publish_box_with_types(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::FromDisplay,
        crate::css::display::FfiDisplay::block(),
        crate::css::css_enums::appearance::AUTO,
        (false, false, true),
    );
    engine.evaluate_size_containers_needing_evaluation_after_layout();
    assert_eq!(
        engine.size_query_container_scan_visits(true),
        0,
        "unrelated dependent: {unrelated_dependent}"
    );
    publish_box_with_types(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::FromDisplay,
        crate::css::display::FfiDisplay::block(),
        crate::css::css_enums::appearance::AUTO,
        (false, false, true),
    );
    assert!(!engine.has_size_containers_needing_evaluation_after_layout());
    engine.evaluate_size_containers_needing_evaluation_after_layout();
    assert_eq!(engine.size_query_container_scan_visits(true), 0);
}

#[test]
fn scroll_only_publication_does_not_scan_size_dependents() {
    assert_scroll_only_publication_does_not_scan(false);
}
#[test]
fn scroll_only_publication_with_unrelated_dependents_does_not_scan() {
    assert_scroll_only_publication_does_not_scan(true);
}

#[test]
fn size_eligibility_without_any_dependents_does_not_walk() {
    let (mut engine, nodes) = engine_with_containers();
    publish_box(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::FromDisplay,
        crate::css::display::FfiDisplay::inline(),
        crate::css::css_enums::appearance::AUTO,
    );
    engine.size_query_container_scan_visits(true);
    engine.evaluate_size_containers_needing_evaluation_after_layout();
    assert_eq!(engine.size_query_container_scan_visits(true), 0);
}

#[test]
fn foreign_object_inline_box_matches_layout_atomicity_and_selection() {
    let (mut engine, nodes) = engine_with_containers();
    let record = publish_box(
        &mut engine,
        nodes[1],
        bridge::ElementBoxKind::SvgForeignObject,
        crate::css::display::FfiDisplay::inline(),
        crate::css::css_enums::appearance::AUTO,
    );
    let payloads = engine.computed_group_sets.style_record_payloads(record).unwrap();
    let values = crate::css::computed_value_views::ComputedValuesView::new(SharedPayload::as_pointer_slice(payloads));
    let kind = crate::layout::node_data::NodeKind::SVGForeignObjectBox;
    let flags = crate::layout::node_facts::construction_flags(kind, false, 0);
    assert!(!crate::layout::node_facts::kind_is_replaced_box(kind));
    assert!(crate::layout::node_facts::node_is_atomic_inline(
        &(kind, flags),
        Some(values)
    ));
    let available = engine.container_query_inputs(nodes[1]).unwrap().size_queries_available;
    let mut length = crate::css::style_compute::FfiLengthResolutionContext::default();
    engine.container_unit_bases(nodes[2], false).apply_to(&mut length);
    assert_eq!((available, length.container_width_basis), (true, 200.0));
}

#[test]
fn foreign_object_eligibility_preserves_display_and_no_box_exclusions() {
    use crate::css::css_enums::keyword;
    for (display, available) in [
        (keyword::BLOCK, true),
        (keyword::NONE, false),
        (keyword::CONTENTS, false),
        (keyword::RUBY, false),
        (keyword::TABLE, false),
        (keyword::TABLE_ROW, false),
    ] {
        let (mut engine, nodes) = engine_with_containers();
        publish_box(
            &mut engine,
            nodes[1],
            bridge::ElementBoxKind::SvgForeignObject,
            crate::css::display::FfiDisplay::from_single_keyword(display).unwrap(),
            crate::css::css_enums::appearance::AUTO,
        );
        assert_eq!(
            engine.container_query_inputs(nodes[1]).unwrap().size_queries_available,
            available
        );
        let mut length = crate::css::style_compute::FfiLengthResolutionContext::default();
        engine.container_unit_bases(nodes[2], false).apply_to(&mut length);
        assert_eq!(length.container_width_basis, if available { 200.0 } else { 500.0 });
    }
    for display in [
        crate::css::display::FfiDisplay::inline(),
        crate::css::display::FfiDisplay::block(),
    ] {
        let (mut engine, nodes) = engine_with_containers();
        publish_box(
            &mut engine,
            nodes[1],
            bridge::ElementBoxKind::NoBox,
            display,
            crate::css::css_enums::appearance::AUTO,
        );
        assert!(!engine.container_query_inputs(nodes[1]).unwrap().size_queries_available);
        let mut length = crate::css::style_compute::FfiLengthResolutionContext::default();
        engine.container_unit_bases(nodes[2], false).apply_to(&mut length);
        assert_eq!(length.container_width_basis, 500.0);
    }
}

#[test]
fn other_svg_box_eligibility_keeps_existing_atomic_kinds() {
    use bridge::ElementBoxKind;
    for (kind, available) in [
        (ElementBoxKind::SvgSvg, true),
        (ElementBoxKind::SvgGraphics, false),
        (ElementBoxKind::SvgText, false),
        (ElementBoxKind::SvgTextPath, false),
        (ElementBoxKind::SvgImage, false),
        (ElementBoxKind::SvgGeometry, false),
    ] {
        let (mut engine, nodes) = engine_with_containers();
        publish_box(
            &mut engine,
            nodes[1],
            kind,
            crate::css::display::FfiDisplay::inline(),
            crate::css::css_enums::appearance::AUTO,
        );
        assert_eq!(
            engine.container_query_inputs(nodes[1]).unwrap().size_queries_available,
            available,
            "{kind:?}"
        );
        let mut length = crate::css::style_compute::FfiLengthResolutionContext::default();
        engine.container_unit_bases(nodes[2], false).apply_to(&mut length);
        assert_eq!(length.container_width_basis, if available { 200.0 } else { 500.0 });
    }
}
