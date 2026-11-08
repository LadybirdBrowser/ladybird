/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Serialization of a CSS declaration block.

use super::*;
use crate::css::declaration_block::{DeclarationBlockHeader, DeclarationRef};
use crate::css::property_metadata::{
    expanded_longhands_for_shorthand, longhands_for_shorthand, property_is_legacy_shorthand,
    property_is_logical_alias_including_shorthands, property_is_shorthand, property_logical_group, property_name,
    shorthands_for_longhand,
};
use crate::css::style_value::{RetainedPropertyIdList, RetainedStyleValueData, RetainedStyleValueDataList};
use crate::css::style_value::{ShorthandPayload, StyleValueBox};
use crate::css::style_value_arc::StyleValueArc;
use std::collections::HashSet;

fn append_property_to_already_serialized(already_serialized: &mut HashSet<u16>, property: u16) {
    already_serialized.insert(property);

    // AD-HOC: The spec assumes that we only store values against expanded longhands, there are however limited
    //         circumstances where we store against shorthands directly in addition to the expanded longhands. For
    //         example if the value of the shorthand is unresolved we store an UnresolvedStyleValue against the
    //         shorthand directly and a PendingSubstitutionStyleValue against each of the longhands. In the case we
    //         serialize a shorthand directly we should also mark it's longhands as serialized to avoid serializing
    //         them separately.
    if property_is_shorthand(property) {
        for &longhand in longhands_for_shorthand(property) {
            append_property_to_already_serialized(already_serialized, longhand);
        }
    }
}

// https://drafts.csswg.org/cssom/#serialize-a-css-declaration
fn serialize_a_css_declaration(list: &mut Vec<TextSink>, property: &StringUnits, value: TextSink, important: bool) {
    // 1. Let s be the empty string.
    let mut s = TextSink::new();

    // 2. Append property to s.
    // AD-HOC: There's no place currently on the spec where the property name properly escaped,
    //         and this needs to be done when custom properties have special characters.
    //         Related spec issues:
    //          - https://github.com/w3c/csswg-drafts/issues/11729
    //          - https://github.com/w3c/csswg-drafts/issues/12258
    serialize_an_identifier(&mut s, property);

    // 3. Append ": " (U+003A U+0020) to s.
    s.push_ascii(": ");

    // 4. If value contains any non-whitespace characters, append value to s.
    let value = value.into_utf16();
    if !value.iter().all(|&unit| unit == 0x20 || (0x09..=0x0D).contains(&unit)) {
        for unit in value {
            s.push_code_unit(unit);
        }
    }

    // 5. If the important flag is set, append " !important" (U+0020 U+0021 U+0069 U+006D U+0070 U+006F U+0072
    //    U+0061 U+006E U+0074) to s.
    if important {
        s.push_ascii(" !important");
    }

    // 6. Append ";" (U+003B) to s.
    s.push_ascii(";");

    // 7. Return s.
    list.push(s);
}

fn shorthand_value(shorthand: u16, longhand_values: Vec<RetainedStyleValueData>) -> StyleValueArc {
    StyleValueArc::new(StyleValueData::Shorthand(StyleValueBox::new(ShorthandPayload {
        shorthand_property: shorthand,
        sub_properties: RetainedPropertyIdList::from_vec(longhands_for_shorthand(shorthand).to_vec()),
        values: RetainedStyleValueDataList::from_retained_values(longhand_values),
    })))
}

/// Whether the value of a longhand still waits for an arbitrary substitution function.
fn awaits_substitution(value: &StyleValueData) -> bool {
    matches!(
        value,
        StyleValueData::PendingSubstitution { .. } | StyleValueData::Unresolved(_)
    )
}

/// The shorthand and the value that the shorthand was written with, which a longhand that pends it holds. A longhand of
/// a shorthand nested in the one written pends the nested shorthand, which pends the written one in turn.
fn written_shorthand(value: &RetainedStyleValueData) -> Option<(u16, &RetainedStyleValueData)> {
    let StyleValueData::PendingSubstitution {
        original_shorthand,
        original_shorthand_value,
    } = value.data()
    else {
        return None;
    };
    written_shorthand(original_shorthand_value).or(Some((*original_shorthand, original_shorthand_value)))
}

/// The value of the shorthand whose longhands, in canonical order, have the values, or None if it serializes to the
/// empty string.
pub(crate) fn shorthand_value_of_longhands(
    shorthand: u16,
    longhand_values: Vec<RetainedStyleValueData>,
) -> Option<StyleValueArc> {
    // https://drafts.csswg.org/css-values-5/#pending-substitution-value
    // If all of the component longhand properties for a given shorthand are pending-substitution values from the same
    // original shorthand value, the shorthand property must serialize to that original (arbitrary substitution
    // function-containing) value.
    // NB: Writing an equal value again leaves longhands holding equal values, not the same one, so what they have in
    //     common is the shorthand written and its value as written.
    let first = longhand_values.first()?;
    if let StyleValueData::PendingSubstitution {
        original_shorthand_value,
        ..
    } = first.data()
    {
        let first_written_shorthand = written_shorthand(first);
        if longhand_values
            .iter()
            .all(|value| written_shorthand(value).is_some() && written_shorthand(value) == first_written_shorthand)
        {
            return Some(original_shorthand_value.clone().into_arc());
        }
    }

    // Otherwise, if any of the component longhand properties for a given shorthand are pending-substitution values, or
    // contain arbitrary substitution functions of their own that have not yet been substituted, the shorthand property
    // must serialize to the empty string.
    if longhand_values.iter().any(|value| awaits_substitution(value.data())) {
        return None;
    }

    Some(shorthand_value(shorthand, longhand_values))
}

fn make_shorthand_value(shorthand: u16, list: &[DeclarationRef]) -> StyleValueArc {
    let longhand_values = longhands_for_shorthand(shorthand)
        .iter()
        .map(|&longhand_id| {
            RetainedStyleValueData::from_arc(if property_is_shorthand(longhand_id) {
                make_shorthand_value(longhand_id, list)
            } else {
                list.iter()
                    .find(|declaration| declaration.property_id == longhand_id)
                    .expect("the list holds every longhand of the shorthand")
                    .value
                    .clone()
            })
        })
        .collect();
    shorthand_value(shorthand, longhand_values)
}

// https://www.w3.org/TR/cssom/#serialize-a-css-value
fn serialize_a_css_value_of_list(list: &[DeclarationRef]) -> TextSink {
    let mut value = TextSink::new();
    let Some(first) = list.first() else {
        return value;
    };

    // 1. Let shorthand be the first shorthand property, in preferred order, that exactly maps to all of the longhand
    //    properties in list.
    let shorthand = shorthands_for_longhand(first.property_id).iter().find(|&&shorthand| {
        let longhands_for_potential_shorthand = expanded_longhands_for_shorthand(shorthand);

        // The potential shorthand exactly maps to all of the longhand properties in list if:
        // a. The number of longhand properties in the list is equal to the number of longhand properties that the
        //    potential shorthand maps to.
        if longhands_for_potential_shorthand.len() != list.len() {
            return false;
        }

        // b. All longhand properties in the list are contained in the list of longhands for the potential shorthand.
        longhands_for_potential_shorthand
            .iter()
            .all(|&longhand| list.iter().any(|declaration| declaration.property_id == longhand))
    });

    // 2. If there is no such shorthand or shorthand cannot exactly represent the values of all the properties in
    //    list, return the empty string.
    let Some(&shorthand) = shorthand else {
        return value;
    };

    // https://drafts.csswg.org/css-values-5/#pending-substitution-value
    // If any of the component longhand properties for a given shorthand are pending-substitution
    // values, or contain arbitrary substitution functions of their own that have not yet been substituted, the
    // shorthand property must serialize to the empty string.
    if list.iter().any(|declaration| awaits_substitution(declaration.value)) {
        return value;
    }

    // 3. Otherwise, serialize a CSS value from a hypothetical declaration of the property shorthand with its value
    //    representing the combined values of the declarations in list.
    serialize_style_value(
        &mut value,
        &make_shorthand_value(shorthand, list),
        SerializationMode::Normal,
    );
    value
}

// https://drafts.csswg.org/cssom/#serialize-a-css-declaration-block
pub(crate) fn serialize_a_css_declaration_block(block: &DeclarationBlockHeader) -> TextSink {
    // 1. Let list be an empty array.
    let mut list = Vec::new();

    // 2. Let already serialized be an empty array.
    let mut already_serialized = HashSet::new();

    // NB: The spec treats custom properties the same as any other property, and expects the loop below to handle
    //     them. However, our implementation separates them from regular properties, so we handle them here.
    // FIXME: Is the relative order of custom properties and regular properties supposed to be preserved?
    for declaration in block.custom_properties().iter() {
        // 5. Let value be the result of invoking serialize a CSS value of declaration.
        let mut value = TextSink::new();
        serialize_style_value(&mut value, declaration.declaration.value, SerializationMode::Normal);

        // 6. Let serialized declaration be the result of invoking serialize a CSS declaration with property name
        //    property, value value, and the important flag set if declaration has its important flag set.
        // 7. Append serialized declaration to list.
        serialize_a_css_declaration(
            &mut list,
            &StringUnits::Utf16(declaration.name.units()),
            value,
            declaration.declaration.important,
        );
    }

    let declarations: Vec<DeclarationRef> = block.properties().iter().collect();

    // 3. Declaration loop: For each CSS declaration declaration in declaration block's declarations, follow these
    //    substeps:
    for declaration in &declarations {
        // 1. Let property be declaration's property name.
        let property = declaration.property_id;

        // 2. If property is in already serialized, continue with the steps labeled declaration loop.
        if already_serialized.contains(&property) {
            continue;
        }

        // AD-HOC: A shorthand pending an outer shorthand has no value of its own to serialize. The outer shorthand
        //         serializes for it, or else each of its longhands does.
        if property_is_shorthand(property) && matches!(&**declaration.value, StyleValueData::PendingSubstitution { .. })
        {
            continue;
        }

        // 3. If property maps to one or more shorthand properties, let shorthands be an array of those shorthand
        //    properties, in preferred order.
        let shorthands = shorthands_for_longhand(property);

        // 4. Shorthand loop: For each shorthand in shorthands, follow these substeps:
        for &shorthand in shorthands {
            // 1. Let longhands be an array consisting of all CSS declarations in declaration block's declarations
            //    that are not in already serialized and have a property name that maps to one of the shorthand
            //    properties in shorthands.
            let longhands: Vec<DeclarationRef> = declarations
                .iter()
                .filter(|longhand| {
                    !already_serialized.contains(&longhand.property_id)
                        && shorthands_for_longhand(longhand.property_id).contains(&shorthand)
                })
                .copied()
                .collect();

            // 2. If not all properties that map to shorthand are present in longhands, continue with the steps
            //    labeled shorthand loop.
            if expanded_longhands_for_shorthand(shorthand)
                .iter()
                .any(|&longhand_id| !longhands.iter().any(|longhand| longhand.property_id == longhand_id))
            {
                continue;
            }

            // 3. Let current longhands be an empty array.
            // 4. Append all CSS declarations in longhands that have a property name that maps to shorthand to current
            //    longhands.
            // NB: longhands holds only those already, as it is made for this shorthand alone.
            let current_longhands = longhands;

            // 5. If there are one or more CSS declarations in current longhands have their important flag set and
            //    one or more with it unset, continue with the steps labeled shorthand loop.
            if current_longhands
                .iter()
                .any(|longhand| longhand.important != current_longhands[0].important)
            {
                continue;
            }

            // 6. If there is any declaration in declaration block in between the first and the last longhand
            //    in current longhands which belongs to the same logical property group, but has a different
            //    mapping logic as any of the longhands in current longhands, and is not in current
            //    longhands, continue with the steps labeled shorthand loop.
            let index_of = |property_id: u16| {
                declarations
                    .iter()
                    .position(|current_declaration| current_declaration.property_id == property_id)
                    .expect("current longhands are declarations of the block")
            };
            let first_current_longhand_index = index_of(current_longhands[0].property_id);
            let last_current_longhand_index = index_of(current_longhands[current_longhands.len() - 1].property_id);
            let has_conflicting_declaration = declarations[first_current_longhand_index..=last_current_longhand_index]
                .iter()
                .any(|current_declaration| {
                    // NB: Declaration is in current longhands
                    if current_longhands
                        .iter()
                        .any(|longhand| longhand.property_id == current_declaration.property_id)
                    {
                        return false;
                    }

                    let Some(group) = property_logical_group(current_declaration.property_id) else {
                        return false;
                    };
                    let is_logical_alias =
                        property_is_logical_alias_including_shorthands(current_declaration.property_id);

                    // NB: Declaration has any counterpart in current longhands with same logical property group but
                    //     different mapping logic
                    current_longhands.iter().any(|longhand| {
                        property_logical_group(longhand.property_id) == Some(group)
                            && property_is_logical_alias_including_shorthands(longhand.property_id) != is_logical_alias
                    })
                });
            if has_conflicting_declaration {
                continue;
            }

            // 7. Let value be the result of invoking serialize a CSS value with current longhands.
            let value = serialize_a_css_value_of_list(&current_longhands);

            // 8. If value is the empty string, continue with the steps labeled shorthand loop.
            if value.len() == 0 {
                continue;
            }

            // 9. Let serialized declaration be the result of invoking serialize a CSS declaration with property
            //    name shorthand, value value, and the important flag set if the CSS declarations in current
            //    longhands have their important flag set.
            // 10. Append serialized declaration to list.
            serialize_a_css_declaration(
                &mut list,
                &StringUnits::Ascii(property_name(shorthand).as_bytes()),
                value,
                current_longhands[0].important,
            );

            // 11. Append the property names of all items of current longhands to already serialized.
            for longhand in &current_longhands {
                append_property_to_already_serialized(&mut already_serialized, longhand.property_id);
            }

            // 12. Continue with the steps labeled declaration loop.
            // NB: The check below skips the rest of the declaration loop, and the remaining shorthands no longer have
            //     all of their longhands in longhands.
        }

        // FIXME: File spec issue that this should only be run if we haven't serialized this declaration in the above
        //        shorthand loop.
        if !already_serialized.contains(&property) {
            // 5. Let value be the result of invoking serialize a CSS value of declaration.
            let mut value = TextSink::new();
            serialize_style_value(&mut value, declaration.value, SerializationMode::Normal);

            // 6. Let serialized declaration be the result of invoking serialize a CSS declaration with property name
            //    property, value value, and the important flag set if declaration has its important flag set.
            // 7. Append serialized declaration to list.
            let serialized_property_name = if property_is_legacy_shorthand(property) {
                longhands_for_shorthand(property)[0]
            } else {
                property
            };
            serialize_a_css_declaration(
                &mut list,
                &StringUnits::Ascii(property_name(serialized_property_name).as_bytes()),
                value,
                declaration.important,
            );

            // 8. Append property to already serialized.
            append_property_to_already_serialized(&mut already_serialized, property);
        }
    }

    // 4. Return list joined with " " (U+0020).
    let mut result = TextSink::new();
    for (index, declaration) in list.into_iter().enumerate() {
        if index != 0 {
            result.push_ascii(" ");
        }
        result.push_sink(&declaration);
    }
    result
}
