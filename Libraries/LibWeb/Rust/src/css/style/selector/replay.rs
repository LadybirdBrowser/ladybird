/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Stable serialization of the semantic selector IR.
//!
//! The bridge receives process-local C++ selector pointers, but replay only needs the immutable
//! program produced from them. Recording this representation also keeps parsing and C++ selector
//! ownership outside the timed Rust-core workload.

use super::AttributeCase;
use super::AttributeOperator;
use super::AttributeTest;
use super::CachedDispatchMetadata;
use super::FeatureTest;
use super::NamespaceTest;
use super::NthPosition;
use super::SelectorEntry;
use super::SelectorEntryProperties;
use super::SelectorNodeID;
use super::SelectorOp;
use super::SelectorProgram;
use super::Specificity;
use super::TagTest;
use super::ValueStateTestKind;
use crate::css::style::index::LocalFeatureKey;
use crate::css::style::index::StyleAtomID;
use crate::css::style::record_replay::Error;
use crate::css::style::record_replay::PayloadReader;
use crate::css::style::record_replay::PayloadWriter;
use crate::css::style::relative_selector::RelativeAxis;
use crate::css::style::relative_selector::RelativeQuery;
use crate::css::style::relative_selector::RelativeQueryID;
use crate::css::style::transaction::StateFact;
use crate::css::style::tree::PseudoElementKind;
use crate::css::style::tree::PseudoElementTarget;
use crate::css::style::tree::StyleNodeID;

pub fn write(program: &SelectorProgram, payload: &mut PayloadWriter) {
    payload.write_length(program.nodes.len());
    for operation in &program.nodes {
        write_operation(*operation, payload);
    }
    payload.write_length(program.operands.len());
    for operand in &program.operands {
        payload.write_u32(operand.0);
    }
    payload.write_u16_slice(&program.text);
    payload.write_length(program.entries.len());
    for entry in &program.entries {
        write_entry(*entry, payload);
    }
    payload.write_length(program.relative_queries.len());
    for query in &program.relative_queries {
        write_relative_query(*query, payload);
    }
    payload.write_length(program.language_ranges.len());
    for (offset, length) in &program.language_ranges {
        payload.write_u32(*offset);
        payload.write_u32(*length);
    }
    payload.write_bool(program.can_leave_scope);
    payload.write_bool(program.subject_can_leave_scope);
}

pub fn read(payload: &mut PayloadReader) -> Result<SelectorProgram, Error> {
    let node_count = payload.read_length()?;
    let mut nodes = Vec::with_capacity(node_count);
    for _ in 0..node_count {
        nodes.push(read_operation(payload)?);
    }
    let operand_count = payload.read_length()?;
    let mut operands = Vec::with_capacity(operand_count);
    for _ in 0..operand_count {
        operands.push(SelectorNodeID(payload.read_u32()?));
    }
    let text = payload.read_u16_vec()?;
    let entry_count = payload.read_length()?;
    let mut entries = Vec::with_capacity(entry_count);
    for _ in 0..entry_count {
        entries.push(read_entry(payload)?);
    }
    let query_count = payload.read_length()?;
    let mut relative_queries = Vec::with_capacity(query_count);
    for _ in 0..query_count {
        relative_queries.push(read_relative_query(payload)?);
    }
    let language_range_count = payload.read_length()?;
    let mut language_ranges = Vec::with_capacity(language_range_count);
    for _ in 0..language_range_count {
        language_ranges.push((payload.read_u32()?, payload.read_u32()?));
    }
    let mut program = SelectorProgram {
        nodes,
        operands,
        text,
        entries,
        relative_queries,
        language_ranges,
        dispatch_metadata: CachedDispatchMetadata::default(),
        relation_target_blooms: Box::default(),
        can_leave_scope: payload.read_bool()?,
        subject_can_leave_scope: payload.read_bool()?,
    };
    program.cache_dispatch_metadata();
    Ok(program)
}

fn write_operation(operation: SelectorOp, payload: &mut PayloadWriter) {
    match operation {
        SelectorOp::Feature(test) => {
            payload.write_u8(0);
            write_feature_test(test, payload);
        }
        SelectorOp::State(fact) => {
            payload.write_u8(1);
            payload.write_u8(fact as u8);
        }
        SelectorOp::And { first, count } => write_pair(2, first, count, payload),
        SelectorOp::Or { first, count } => write_pair(3, first, count, payload),
        SelectorOp::Where(node) => write_node(4, node, payload),
        SelectorOp::Not(node) => write_node(5, node, payload),
        SelectorOp::Parent(node) => write_node(6, node, payload),
        SelectorOp::Ancestor(node) => write_node(7, node, payload),
        SelectorOp::PreviousSibling(node) => write_node(8, node, payload),
        SelectorOp::PrecedingSibling(node) => write_node(9, node, payload),
        SelectorOp::NthPosition(position) => {
            payload.write_u8(10);
            write_nth_position(position, payload);
        }
        SelectorOp::RelativeExists(query) => {
            payload.write_u8(11);
            payload.write_u32(query.0);
        }
        SelectorOp::Host(node) => write_node(12, node, payload),
        SelectorOp::Slotted(node) => write_node(13, node, payload),
        SelectorOp::Part(atom) => write_atom(14, atom, payload),
        SelectorOp::ExposedToHost { host, parts } => {
            payload.write_u8(15);
            payload.write_u32(host.0);
            payload.write_u32(parts.0);
        }
        SelectorOp::Root => payload.write_u8(16),
        SelectorOp::Empty => payload.write_u8(17),
        SelectorOp::Scope => payload.write_u8(18),
        SelectorOp::AssignedSlot(node) => write_node(19, node, payload),
        SelectorOp::ScopeRootInstance => payload.write_u8(20),
        SelectorOp::RelativeAnchorInstance => payload.write_u8(21),
        SelectorOp::IsNode(node) => {
            payload.write_u8(22);
            payload.write_u32(node.raw());
        }
        SelectorOp::InScope {
            root,
            limit,
            inner,
            names_the_scope,
        } => {
            payload.write_u8(23);
            payload.write_u32(root.0);
            write_optional_node(limit, payload);
            payload.write_u32(inner.0);
            payload.write_bool(names_the_scope);
        }
        SelectorOp::ValueState { kind, value } => {
            payload.write_u8(24);
            payload.write_u8(match kind {
                ValueStateTestKind::Directionality => 0,
                ValueStateTestKind::CustomState => 1,
            });
            payload.write_u32(value.0);
        }
        SelectorOp::Language { first, count } => write_pair(25, first, count, payload),
        SelectorOp::Heading(levels) => {
            payload.write_u8(26);
            payload.write_u16(levels);
        }
    }
}

fn read_operation(payload: &mut PayloadReader) -> Result<SelectorOp, Error> {
    Ok(match payload.read_u8()? {
        0 => SelectorOp::Feature(read_feature_test(payload)?),
        1 => SelectorOp::State(read_state_fact(payload)?),
        2 => SelectorOp::And {
            first: payload.read_u32()?,
            count: payload.read_u32()?,
        },
        3 => SelectorOp::Or {
            first: payload.read_u32()?,
            count: payload.read_u32()?,
        },
        4 => SelectorOp::Where(read_node(payload)?),
        5 => SelectorOp::Not(read_node(payload)?),
        6 => SelectorOp::Parent(read_node(payload)?),
        7 => SelectorOp::Ancestor(read_node(payload)?),
        8 => SelectorOp::PreviousSibling(read_node(payload)?),
        9 => SelectorOp::PrecedingSibling(read_node(payload)?),
        10 => SelectorOp::NthPosition(read_nth_position(payload)?),
        11 => SelectorOp::RelativeExists(RelativeQueryID(payload.read_u32()?)),
        12 => SelectorOp::Host(read_node(payload)?),
        13 => SelectorOp::Slotted(read_node(payload)?),
        14 => SelectorOp::Part(read_atom(payload)?),
        15 => SelectorOp::ExposedToHost {
            host: read_node(payload)?,
            parts: read_node(payload)?,
        },
        16 => SelectorOp::Root,
        17 => SelectorOp::Empty,
        18 => SelectorOp::Scope,
        19 => SelectorOp::AssignedSlot(read_node(payload)?),
        20 => SelectorOp::ScopeRootInstance,
        21 => SelectorOp::RelativeAnchorInstance,
        22 => SelectorOp::IsNode(StyleNodeID::from_raw(payload.read_u32()?).ok_or(Error::InvalidTag {
            category: "style node",
            value: 0,
        })?),
        23 => SelectorOp::InScope {
            root: read_node(payload)?,
            limit: read_optional_node(payload)?,
            inner: read_node(payload)?,
            names_the_scope: payload.read_bool()?,
        },
        24 => SelectorOp::ValueState {
            kind: match payload.read_u8()? {
                0 => ValueStateTestKind::Directionality,
                1 => ValueStateTestKind::CustomState,
                value => {
                    return Err(Error::InvalidTag {
                        category: "value state",
                        value,
                    });
                }
            },
            value: read_atom(payload)?,
        },
        25 => SelectorOp::Language {
            first: payload.read_u32()?,
            count: payload.read_u32()?,
        },
        26 => SelectorOp::Heading(payload.read_u16()?),
        value => {
            return Err(Error::InvalidTag {
                category: "selector operation",
                value,
            });
        }
    })
}

fn write_feature_test(test: FeatureTest, payload: &mut PayloadWriter) {
    match test {
        FeatureTest::AnyElement => payload.write_u8(0),
        FeatureTest::TagName(test) => {
            payload.write_u8(1);
            payload.write_u32(test.written.0);
            payload.write_u32(test.folded.0);
            payload.write_u32(test.fold_in_namespace.0);
        }
        FeatureTest::Id(atom) => write_atom(2, atom, payload),
        FeatureTest::Class(atom) => write_atom(3, atom, payload),
        FeatureTest::Attribute(test) => {
            payload.write_u8(4);
            payload.write_u32(test.name.0);
            payload.write_bool(test.any_namespace);
            payload.write_u32(test.folded.0);
            payload.write_u32(test.fold_in_namespace.0);
            payload.write_u8(match test.operator {
                AttributeOperator::Presence => 0,
                AttributeOperator::Exact => 1,
                AttributeOperator::Includes => 2,
                AttributeOperator::DashMatch => 3,
                AttributeOperator::Prefix => 4,
                AttributeOperator::Suffix => 5,
                AttributeOperator::Substring => 6,
            });
            payload.write_u32(test.value_atom.0);
            payload.write_u32(test.value_offset);
            payload.write_u32(test.value_length);
            match test.case {
                AttributeCase::Sensitive => payload.write_u8(0),
                AttributeCase::Insensitive => payload.write_u8(1),
                AttributeCase::InsensitiveForNamespace(namespace) => {
                    payload.write_u8(2);
                    payload.write_u32(namespace.0);
                }
            }
        }
        FeatureTest::Namespace(NamespaceTest::None) => {
            payload.write_u8(5);
            payload.write_u8(0);
        }
        FeatureTest::Namespace(NamespaceTest::Named(namespace)) => {
            payload.write_u8(5);
            payload.write_u8(1);
            payload.write_u32(namespace.0);
        }
    }
}

fn read_feature_test(payload: &mut PayloadReader) -> Result<FeatureTest, Error> {
    Ok(match payload.read_u8()? {
        0 => FeatureTest::AnyElement,
        1 => FeatureTest::TagName(TagTest {
            written: read_atom(payload)?,
            folded: read_atom(payload)?,
            fold_in_namespace: read_atom(payload)?,
        }),
        2 => FeatureTest::Id(read_atom(payload)?),
        3 => FeatureTest::Class(read_atom(payload)?),
        4 => FeatureTest::Attribute(AttributeTest {
            name: read_atom(payload)?,
            any_namespace: payload.read_bool()?,
            folded: read_atom(payload)?,
            fold_in_namespace: read_atom(payload)?,
            operator: match payload.read_u8()? {
                0 => AttributeOperator::Presence,
                1 => AttributeOperator::Exact,
                2 => AttributeOperator::Includes,
                3 => AttributeOperator::DashMatch,
                4 => AttributeOperator::Prefix,
                5 => AttributeOperator::Suffix,
                6 => AttributeOperator::Substring,
                value => {
                    return Err(Error::InvalidTag {
                        category: "attribute operator",
                        value,
                    });
                }
            },
            value_atom: read_atom(payload)?,
            value_offset: payload.read_u32()?,
            value_length: payload.read_u32()?,
            case: match payload.read_u8()? {
                0 => AttributeCase::Sensitive,
                1 => AttributeCase::Insensitive,
                2 => AttributeCase::InsensitiveForNamespace(read_atom(payload)?),
                value => {
                    return Err(Error::InvalidTag {
                        category: "attribute case",
                        value,
                    });
                }
            },
        }),
        5 => FeatureTest::Namespace(match payload.read_u8()? {
            0 => NamespaceTest::None,
            1 => NamespaceTest::Named(read_atom(payload)?),
            value => {
                return Err(Error::InvalidTag {
                    category: "namespace test",
                    value,
                });
            }
        }),
        value => {
            return Err(Error::InvalidTag {
                category: "feature test",
                value,
            });
        }
    })
}

fn write_entry(entry: SelectorEntry, payload: &mut PayloadWriter) {
    payload.write_u32(entry.root.0);
    payload.write_u16(entry.specificity.ids);
    payload.write_u16(entry.specificity.classes);
    payload.write_u16(entry.specificity.types);
    payload.write_bool(entry.pseudo_element.is_some());
    if let Some(pseudo_element) = entry.pseudo_element {
        payload.write_u16(pseudo_element.kind.0);
    }
    write_optional_node(entry.scope_root, payload);
    let mut properties = 0_u8;
    properties |= u8::from(entry.properties.monotone_under_arrivals);
    properties |= u8::from(entry.properties.can_use_before_sibling_relations) << 1;
    properties |= u8::from(entry.properties.observes_sibling_relation) << 2;
    properties |= u8::from(entry.properties.has_prefix_chain) << 3;
    properties |= u8::from(entry.properties.prefix_chain_has_only_local_facts) << 4;
    payload.write_u8(properties);
}

fn read_entry(payload: &mut PayloadReader) -> Result<SelectorEntry, Error> {
    let root = read_node(payload)?;
    let specificity = Specificity {
        ids: payload.read_u16()?,
        classes: payload.read_u16()?,
        types: payload.read_u16()?,
    };
    let pseudo_element = payload
        .read_bool()?
        .then(|| {
            payload
                .read_u16()
                .map(|kind| PseudoElementTarget::new(PseudoElementKind(kind)))
        })
        .transpose()?;
    let scope_root = read_optional_node(payload)?;
    let properties = payload.read_u8()?;
    if properties & !0x1f != 0 {
        return Err(Error::InvalidTag {
            category: "selector entry properties",
            value: properties,
        });
    }
    Ok(SelectorEntry {
        root,
        specificity,
        pseudo_element,
        scope_root,
        properties: SelectorEntryProperties {
            monotone_under_arrivals: properties & 1 != 0,
            can_use_before_sibling_relations: properties & 2 != 0,
            observes_sibling_relation: properties & 4 != 0,
            has_prefix_chain: properties & 8 != 0,
            prefix_chain_has_only_local_facts: properties & 16 != 0,
        },
    })
}

fn write_relative_query(query: RelativeQuery, payload: &mut PayloadWriter) {
    payload.write_u8(match query.axis {
        RelativeAxis::Descendant => 0,
        RelativeAxis::Child => 1,
        RelativeAxis::NextSibling => 2,
        RelativeAxis::FollowingSibling => 3,
        RelativeAxis::NextSiblingSubtree => 4,
        RelativeAxis::FollowingSiblingSubtree => 5,
    });
    payload.write_u32(query.compound.0);
    payload.write_bool(query.driving_feature.is_some());
    if let Some(feature) = query.driving_feature {
        write_feature_key(feature, payload);
    }
    payload.write_bool(query.simple);
    payload.write_bool(query.witness_is_below_the_axis);
    payload.write_bool(query.match_in_shadow_tree);
}

fn read_relative_query(payload: &mut PayloadReader) -> Result<RelativeQuery, Error> {
    Ok(RelativeQuery {
        axis: match payload.read_u8()? {
            0 => RelativeAxis::Descendant,
            1 => RelativeAxis::Child,
            2 => RelativeAxis::NextSibling,
            3 => RelativeAxis::FollowingSibling,
            4 => RelativeAxis::NextSiblingSubtree,
            5 => RelativeAxis::FollowingSiblingSubtree,
            value => {
                return Err(Error::InvalidTag {
                    category: "relative axis",
                    value,
                });
            }
        },
        compound: read_node(payload)?,
        driving_feature: payload.read_bool()?.then(|| read_feature_key(payload)).transpose()?,
        simple: payload.read_bool()?,
        witness_is_below_the_axis: payload.read_bool()?,
        match_in_shadow_tree: payload.read_bool()?,
    })
}

fn write_feature_key(feature: LocalFeatureKey, payload: &mut PayloadWriter) {
    match feature {
        LocalFeatureKey::TagName => payload.write_u8(0),
        LocalFeatureKey::FoldedTagName => payload.write_u8(1),
        LocalFeatureKey::Id => payload.write_u8(2),
        LocalFeatureKey::Class(atom) => write_atom(3, atom, payload),
        LocalFeatureKey::Part(atom) => write_atom(4, atom, payload),
        LocalFeatureKey::CustomState(atom) => write_atom(5, atom, payload),
        LocalFeatureKey::Emptiness => payload.write_u8(6),
        LocalFeatureKey::Attribute(atom) => write_atom(7, atom, payload),
        LocalFeatureKey::Language => payload.write_u8(8),
        LocalFeatureKey::Directionality => payload.write_u8(9),
        LocalFeatureKey::PartExposure => payload.write_u8(10),
        LocalFeatureKey::ArrivingFacts => payload.write_u8(11),
        LocalFeatureKey::HeadingLevel => payload.write_u8(12),
    }
}

fn read_feature_key(payload: &mut PayloadReader) -> Result<LocalFeatureKey, Error> {
    Ok(match payload.read_u8()? {
        0 => LocalFeatureKey::TagName,
        1 => LocalFeatureKey::FoldedTagName,
        2 => LocalFeatureKey::Id,
        3 => LocalFeatureKey::Class(read_atom(payload)?),
        4 => LocalFeatureKey::Part(read_atom(payload)?),
        5 => LocalFeatureKey::CustomState(read_atom(payload)?),
        6 => LocalFeatureKey::Emptiness,
        7 => LocalFeatureKey::Attribute(read_atom(payload)?),
        8 => LocalFeatureKey::Language,
        9 => LocalFeatureKey::Directionality,
        10 => LocalFeatureKey::PartExposure,
        11 => LocalFeatureKey::ArrivingFacts,
        12 => LocalFeatureKey::HeadingLevel,
        value => {
            return Err(Error::InvalidTag {
                category: "feature key",
                value,
            });
        }
    })
}

fn write_nth_position(position: NthPosition, payload: &mut PayloadWriter) {
    payload.write_i32(position.step);
    payload.write_i32(position.offset);
    payload.write_bool(position.from_end);
    write_optional_node(position.of_selector, payload);
    payload.write_bool(position.of_type);
}

fn read_nth_position(payload: &mut PayloadReader) -> Result<NthPosition, Error> {
    Ok(NthPosition {
        step: payload.read_i32()?,
        offset: payload.read_i32()?,
        from_end: payload.read_bool()?,
        of_selector: read_optional_node(payload)?,
        of_type: payload.read_bool()?,
    })
}

fn read_state_fact(payload: &mut PayloadReader) -> Result<StateFact, Error> {
    let value = payload.read_u8()?;
    StateFact::ALL.get(value as usize).copied().ok_or(Error::InvalidTag {
        category: "state fact",
        value,
    })
}

fn write_node(tag: u8, node: SelectorNodeID, payload: &mut PayloadWriter) {
    payload.write_u8(tag);
    payload.write_u32(node.0);
}

fn write_atom(tag: u8, atom: StyleAtomID, payload: &mut PayloadWriter) {
    payload.write_u8(tag);
    payload.write_u32(atom.0);
}

fn write_pair(tag: u8, first: u32, count: u32, payload: &mut PayloadWriter) {
    payload.write_u8(tag);
    payload.write_u32(first);
    payload.write_u32(count);
}

fn write_optional_node(node: Option<SelectorNodeID>, payload: &mut PayloadWriter) {
    payload.write_bool(node.is_some());
    if let Some(node) = node {
        payload.write_u32(node.0);
    }
}

fn read_optional_node(payload: &mut PayloadReader) -> Result<Option<SelectorNodeID>, Error> {
    payload.read_bool()?.then(|| read_node(payload)).transpose()
}

fn read_node(payload: &mut PayloadReader) -> Result<SelectorNodeID, Error> {
    Ok(SelectorNodeID(payload.read_u32()?))
}

fn read_atom(payload: &mut PayloadReader) -> Result<StyleAtomID, Error> {
    Ok(StyleAtomID(payload.read_u32()?))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::record_replay::EventKind;
    use crate::css::style::record_replay::LogReader;
    use crate::css::style::record_replay::LogWriter;
    use std::io::Cursor;

    #[test]
    fn semantic_program_round_trip() {
        let mut program = SelectorProgram {
            nodes: vec![
                SelectorOp::Feature(FeatureTest::Attribute(AttributeTest {
                    name: StyleAtomID(1),
                    any_namespace: true,
                    folded: StyleAtomID(2),
                    fold_in_namespace: StyleAtomID(3),
                    operator: AttributeOperator::Substring,
                    value_atom: StyleAtomID(4),
                    value_offset: 5,
                    value_length: 6,
                    case: AttributeCase::InsensitiveForNamespace(StyleAtomID(7)),
                })),
                SelectorOp::State(StateFact::Hover),
                SelectorOp::NthPosition(NthPosition {
                    step: -2,
                    offset: 3,
                    from_end: true,
                    of_selector: Some(SelectorNodeID(0)),
                    of_type: true,
                }),
                SelectorOp::InScope {
                    root: SelectorNodeID(0),
                    limit: Some(SelectorNodeID(1)),
                    inner: SelectorNodeID(2),
                    names_the_scope: true,
                },
                SelectorOp::ValueState {
                    kind: ValueStateTestKind::CustomState,
                    value: StyleAtomID(8),
                },
                SelectorOp::Language { first: 0, count: 1 },
            ],
            operands: vec![SelectorNodeID(0), SelectorNodeID(1)],
            text: vec![b'e' as u16, b'n' as u16],
            entries: vec![SelectorEntry {
                root: SelectorNodeID(3),
                specificity: Specificity {
                    ids: 1,
                    classes: 2,
                    types: 3,
                },
                pseudo_element: Some(PseudoElementTarget::new(PseudoElementKind(4))),
                scope_root: Some(SelectorNodeID(0)),
                properties: SelectorEntryProperties {
                    monotone_under_arrivals: true,
                    can_use_before_sibling_relations: false,
                    observes_sibling_relation: true,
                    has_prefix_chain: true,
                    prefix_chain_has_only_local_facts: false,
                },
            }],
            relative_queries: vec![RelativeQuery {
                axis: RelativeAxis::NextSiblingSubtree,
                compound: SelectorNodeID(2),
                driving_feature: Some(LocalFeatureKey::Class(StyleAtomID(9))),
                simple: true,
                witness_is_below_the_axis: true,
                match_in_shadow_tree: true,
            }],
            language_ranges: vec![(0, 2)],
            dispatch_metadata: CachedDispatchMetadata::default(),
            can_leave_scope: true,
            subject_can_leave_scope: false,
        };
        program.cache_dispatch_metadata();

        let mut output = Vec::new();
        let mut writer = LogWriter::new(&mut output).unwrap();
        let mut payload = PayloadWriter::default();
        write(&program, &mut payload);
        writer.write_event(EventKind::CreateGraph, &payload).unwrap();
        writer.flush().unwrap();

        let mut reader = LogReader::new(Cursor::new(output)).unwrap();
        let mut event = reader.read_event().unwrap().unwrap();
        let decoded = read(&mut event.payload).unwrap();
        event.payload.finish().unwrap();
        assert_eq!(decoded, program);
    }
}
