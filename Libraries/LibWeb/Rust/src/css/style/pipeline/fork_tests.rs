/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a fork of a styled document's engine allocates. The render clock and the hover lane fork
//! the engine whenever the host moves on under them, so a fork is made per frame while the host is
//! busy: it should copy chunk pointers and reference counts, not tables, and so allocate no more
//! for a document twice as large.

use super::super::*;
use super::allocation_count::count_live_bytes;
use super::test_support::*;
use crate::css::computed_value_types::FontValues;
use crate::css::computed_values::{BoxValues, StyleGroups};
use crate::css::table_group_builder::group_index;

/// How many elements the smaller of the two documents a test forks holds.
const ELEMENTS: usize = 256;

/// Builds a document element with `count - 1` children, owned by the engine, as a document's
/// host leaves it after its first frame: one query container late in tree order, every eighth
/// element with replaced content, and the damage of moving every element between two records
/// compared once.
fn styled_document(count: usize) -> StyleEngine {
    let (mut engine, nodes) = document_with_children(count);
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".c { container-type: inline-size; } .q { color: rgb(0, 128, 0); }",
    );
    for (index, &node) in nodes.iter().enumerate().skip(1) {
        add_class(&mut engine, node, "q");
        if index % 8 == 0 {
            engine.set_element_replaced_content_input(
                node,
                inputs::ReplacedContentInput::Input {
                    size: 20,
                    is_text_entry: true,
                },
            );
        }
    }
    let container = *nodes.last().unwrap();
    add_class(&mut engine, container, "c");
    start_live(&mut engine, nodes[0]);
    hold_container_record(&mut engine, container);
    // NB: Comparing records reads their font cascade lists, which the default font group holds
    //     none of. The test stubs compare lists by address.
    let font_cascade_list: *const std::ffi::c_void = (&raw const *Box::leak(Box::new(0_u8))).cast();
    let mut payloads = engine.default_groups().groups().clone();
    // SAFETY: The font group holds a `FontValues`.
    unsafe {
        payloads.write_group::<FontValues>(group_index::FONT, |font| {
            font.font_cascade_list = libgfx_rust::font::FontCascadeListHandle::retain(font_cascade_list);
        });
    }
    // NB: Records of equal content are one, so each element moves between records of its own.
    for (index, &node) in nodes.iter().enumerate() {
        let opacity = index as f64 / 1024.0;
        let old = install_record(&mut engine, node, &payloads, opacity);
        let new = install_record(&mut engine, node, &payloads, 1.0 - opacity);
        engine.element_record_damage(node, false, old, new);
    }
    engine
}

/// Installs a record of `payloads` for `node` over a longhand table holding `opacity` and the
/// initial value of every other longhand, answering its identity.
fn install_record(engine: &mut StyleEngine, node: StyleNodeID, payloads: &StyleGroups, opacity: f64) -> u64 {
    let value_of = |property| match property {
        crate::css::property_metadata::property_id::OPACITY => {
            RetainedStyleValueData::from_owned(crate::css::style_value::StyleValueData::Number { value: opacity })
        }
        _ => crate::css::style_compute::initial_value_handle(property).clone(),
    };
    engine
        .install_test_record(
            computed::ComputedStyleTarget::new(node, u8::MAX),
            Some(&value_of),
            Some(payloads),
            0,
        )
        .raw()
}

/// Has the host hold a record for `node` that makes it an inline-size query container.
fn hold_container_record(engine: &mut StyleEngine, node: StyleNodeID) {
    engine.set_element_box_kind(node, bridge::ElementBoxKind::FromDisplay);
    let mut payloads = engine.default_groups().groups().clone();
    // SAFETY: The group at this index holds box values.
    unsafe {
        payloads.write_group::<BoxValues>(group_index::BOX, |box_values| {
            box_values.display = crate::css::display::FfiDisplay::block();
            box_values.is_inline_size_container = true;
        });
    }
    let record = install_record(engine, node, &payloads, 1.0);
    let held = engine.computed_group_sets.host_record(record).cloned();
    engine.set_held_style_record(node, held);
}

/// The bytes a fork of `engine` holds of its own, as `RenderState::fork` makes one.
///
/// NB: The fork does not retain the font cascades its font resolutions name, as a fork of the
///     render state does: the test's font resolver makes none.
fn fork_bytes(engine: &StyleEngine) -> i64 {
    let (fork, bytes) = count_live_bytes(|| {
        let mut fork = engine.clone();
        fork.detach_tree_depth_for_fork();
        fork
    });
    drop(fork);
    bytes
}

/// What a fork of the smaller document allocates, and how much more per element a fork of one
/// twice as large does, at most.
const FORK_BYTES: i64 = 5_000;
const FORK_BYTES_PER_ELEMENT: f64 = 1.5;

#[test]
fn forking_a_styled_document_allocates_at_most_as_today() {
    let small = fork_bytes(&styled_document(ELEMENTS));
    let large = fork_bytes(&styled_document(2 * ELEMENTS));
    let per_element = (large - small) as f64 / ELEMENTS as f64;
    // FIXME: A fork copies chunk pointers and reference counts, not tables: drive both bounds to
    //        what the chunks written since the fork hold.
    assert!(
        small <= FORK_BYTES,
        "a fork of {ELEMENTS} elements allocates {small} bytes"
    );
    assert!(
        per_element <= FORK_BYTES_PER_ELEMENT,
        "a fork allocates {per_element:.1} bytes per element"
    );
}
