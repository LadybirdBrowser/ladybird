/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::parser::value_parser::{ParseContext, ParseOutcome, parse_css_value};
use crate::css::property_metadata::{longhands_for_shorthand, property_id, property_initial_value};
use crate::css::style_value::{RetainedPropertyIdList, RetainedStyleValueData, RetainedStyleValueDataList};
use std::borrow::Cow;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serialize_shorthands_and_calculated_transforms_without_cpp_on_a_worker() {
        std::thread::spawn(|| {
            for (property, source, expected) in [
                (
                    property_id::ANIMATION,
                    "2s ease 1s 3 alternate both paused 雪",
                    "2s 1s 3 alternate both paused 雪",
                ),
                (
                    property_id::ANIMATION,
                    "1s linear linear, 2s ease-in 雨",
                    "1s linear linear, 2s ease-in 雨",
                ),
                (
                    property_id::TRANSITION,
                    "opacity 1s ease 2s, color 3s linear",
                    "opacity 1s 2s, color 3s linear",
                ),
                (
                    property_id::SCROLL_TIMELINE,
                    "--雪 inline, --雨 block",
                    "--雪 inline, --雨",
                ),
                (
                    property_id::BACKGROUND,
                    "url(a.png), linear-gradient(red, blue)",
                    "url(\"a.png\"), linear-gradient(red, blue)",
                ),
                (
                    property_id::MASK,
                    "url(a.svg) left top / 20px 30px no-repeat padding-box no-clip",
                    "url(\"a.svg\") left top / 20px 30px no-repeat padding-box no-clip",
                ),
                (property_id::BORDER, "1px solid red", "1px solid red"),
                (property_id::BORDER_BLOCK, "1px solid red", "1px solid red"),
                (property_id::BORDER_INLINE, "medium solid currentcolor", "solid"),
                (
                    property_id::FONT,
                    "italic small-caps bold condensed 20px / 1.5 '雪'",
                    "italic small-caps bold condensed 20px / 1.5 \"雪\"",
                ),
                (
                    property_id::GRID,
                    "auto-flow dense 20px / 1fr 2fr",
                    "auto-flow dense 20px / 1fr 2fr",
                ),
                (
                    property_id::GRID_TEMPLATE,
                    "[start] '雪 雨' minmax(10px, 1fr) [end] / 1fr 2fr",
                    "[start] \"雪 雨\" minmax(10px, 1fr) [end] / 1fr 2fr",
                ),
                (property_id::ROTATE, "calc(1 + 1) 0 0 30deg", "x 30deg"),
                (property_id::SCALE, "calc(1 + 1) calc(50%)", "2 0.5"),
                (
                    property_id::TRANSFORM,
                    "scale(0.00001%)",
                    "scale(1.0000000000000001e-7)",
                ),
            ] {
                let units: Vec<u16> = source.encode_utf16().collect();
                let value = parse(property, TokenizerInput::Utf16(&units)).unwrap();
                let result = text(&value, SerializationMode::Normal).unwrap().into_utf16();
                assert_eq!(result, expected.encode_utf16().collect::<Vec<_>>(), "{source}");
            }
            assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
        })
        .join()
        .unwrap();
    }
}

fn longhand(value: &StyleValueData, property: u16) -> Option<&StyleValueData> {
    let StyleValueData::Shorthand {
        sub_properties, values, ..
    } = value
    else {
        return None;
    };
    values
        .as_slice()
        .get(sub_properties.as_slice().iter().position(|id| *id == property)?)?
        .optional_data()
}

fn parse(property: u16, source: TokenizerInput<'_>) -> Option<std::sync::Arc<StyleValueData>> {
    use crate::css::css_tokenizer::tokenize_for_parser;
    use crate::css::parser::component_value::consume_a_list_of_component_values;
    let values = consume_a_list_of_component_values(tokenize_for_parser(source)).ok()?;
    // All fields are booleans, integers, or nullable pointers. Serialization has no document context.
    let context: ParseContext = unsafe { std::mem::zeroed() };
    match parse_css_value(&context, property, &values) {
        ParseOutcome::Parsed(value) => Some(value),
        _ => None,
    }
}

pub(super) fn initial(property: u16) -> Cow<'static, StyleValueData> {
    let longhands = longhands_for_shorthand(property);
    if !longhands.is_empty() {
        return Cow::Owned(StyleValueData::Shorthand {
            shorthand_property: property,
            sub_properties: RetainedPropertyIdList::from_property_ids(longhands.to_vec()),
            values: RetainedStyleValueDataList::from_retained_values(
                longhands
                    .iter()
                    .map(|id| RetainedStyleValueData::from_owned(initial(*id).into_owned()))
                    .collect(),
            ),
        });
    }
    if let Some(value) = crate::css::style_compute::initial_value_if_available(property) {
        return Cow::Borrowed(value);
    }
    // Standalone Rust parsing can run before the engine installs its initial-value table.
    Cow::Owned(
        (*parse(
            property,
            TokenizerInput::Ascii(property_initial_value(property).as_bytes()),
        )
        .unwrap())
        .clone(),
    )
}

fn text(value: &StyleValueData, mode: SerializationMode) -> Option<TextSink> {
    let mut sink = TextSink::new();
    serialize_style_value(&mut sink, value, mode).then_some(sink)
}

fn write(sink: &mut TextSink, value: &StyleValueData, mode: SerializationMode) -> Option<()> {
    serialize_style_value(sink, value, mode).then_some(())
}

fn keyword_is(value: &StyleValueData, expected: u16) -> bool {
    matches!(value, StyleValueData::Keyword { keyword } if *keyword == expected)
}

fn layer_count(value: &StyleValueData) -> usize {
    match value {
        StyleValueData::ValueList { values, .. } => values.as_slice().len(),
        _ => 1,
    }
}

fn layer(value: &StyleValueData, index: usize) -> &StyleValueData {
    match value {
        StyleValueData::ValueList { values, .. } => values.as_slice()[index % values.as_slice().len()].data(),
        _ => value,
    }
}

pub(super) fn serialize(
    sink: &mut TextSink,
    whole: &StyleValueData,
    property: u16,
    mode: SerializationMode,
    default: impl Fn(&mut TextSink) -> bool,
) -> bool {
    (|| -> Option<()> {
        let get = |id| longhand(whole, id);
        let is_initial = |value: &StyleValueData, id| *value == *initial(id);
        match property {
            property_id::ANIMATION
            | property_id::TRANSITION
            | property_id::SCROLL_TIMELINE
            | property_id::VIEW_TIMELINE => {
                let StyleValueData::Shorthand { sub_properties, .. } = whole else {
                    return None;
                };
                let reset_only = |id| property == property_id::ANIMATION && id == property_id::ANIMATION_TIMELINE;
                let required = |id| {
                    (property == property_id::SCROLL_TIMELINE && id == property_id::SCROLL_TIMELINE_NAME)
                        || (property == property_id::VIEW_TIMELINE && id == property_id::VIEW_TIMELINE_NAME)
                };
                let ids = sub_properties.as_slice();
                let count = layer_count(get(*ids.first()?)?);
                for &id in ids {
                    let value = get(id)?;
                    if reset_only(id) {
                        if !is_initial(value, id) {
                            return Some(());
                        }
                    } else if !matches!(value, StyleValueData::ValueList { .. }) || layer_count(value) != count {
                        return Some(());
                    }
                }
                let item_is_initial = |value: &StyleValueData, id| {
                    if property == property_id::ANIMATION
                        && id == property_id::ANIMATION_DURATION
                        && matches!(value, StyleValueData::Time { value: 0.0, .. })
                    {
                        return true;
                    }
                    *value == *layer(&initial(id), 0)
                };
                for entry in 0..count {
                    if entry != 0 {
                        sink.push_ascii(", ");
                    }
                    let mut first = true;
                    for (index, &id) in ids.iter().enumerate() {
                        if reset_only(id) {
                            continue;
                        }
                        let value = layer(get(id)?, entry);
                        let mut include = required(id) || !item_is_initial(value, id);
                        if !include {
                            for &other_id in &ids[index + 1..] {
                                if reset_only(other_id) {
                                    continue;
                                }
                                let other = layer(get(other_id)?, entry);
                                if item_is_initial(other, other_id) {
                                    continue;
                                }
                                let source = text(other, mode)?;
                                let input = if source.is_ascii {
                                    TokenizerInput::Ascii(&source.ascii)
                                } else {
                                    TokenizerInput::Utf16(&source.utf16)
                                };
                                if parse(id, input).is_some() {
                                    include = true;
                                    break;
                                }
                            }
                        }
                        if include {
                            if !first {
                                sink.push_ascii(" ");
                            }
                            write(sink, value, mode)?;
                            first = false;
                        }
                    }
                    if first {
                        sink.push_ascii(if property == property_id::ANIMATION {
                            "none"
                        } else if property == property_id::TRANSITION {
                            "all"
                        } else {
                            ""
                        });
                    }
                }
            }
            property_id::BACKGROUND_POSITION => {
                let x = get(property_id::BACKGROUND_POSITION_X)?;
                let y = get(property_id::BACKGROUND_POSITION_Y)?;
                let count = layer_count(x).max(layer_count(y));
                for index in 0..count {
                    if index != 0 {
                        sink.push_ascii(", ");
                    }
                    write(sink, if count == 1 { x } else { layer(x, index) }, mode)?;
                    sink.push_ascii(" ");
                    write(sink, if count == 1 { y } else { layer(y, index) }, mode)?;
                }
            }
            property_id::BACKGROUND | property_id::MASK => {
                let background = property == property_id::BACKGROUND;
                let ids = if background {
                    &[
                        property_id::BACKGROUND_COLOR,
                        property_id::BACKGROUND_IMAGE,
                        property_id::BACKGROUND_POSITION_X,
                        property_id::BACKGROUND_POSITION_Y,
                        property_id::BACKGROUND_SIZE,
                        property_id::BACKGROUND_REPEAT,
                        property_id::BACKGROUND_ATTACHMENT,
                        property_id::BACKGROUND_ORIGIN,
                        property_id::BACKGROUND_CLIP,
                    ][..]
                } else {
                    &[
                        property_id::MASK_IMAGE,
                        property_id::MASK_POSITION,
                        property_id::MASK_SIZE,
                        property_id::MASK_REPEAT,
                        property_id::MASK_ORIGIN,
                        property_id::MASK_CLIP,
                        property_id::MASK_COMPOSITE,
                        property_id::MASK_MODE,
                    ][..]
                };
                let values = ids
                    .iter()
                    .map(|&id| {
                        if id == property_id::BACKGROUND_POSITION_X || id == property_id::BACKGROUND_POSITION_Y {
                            longhand(get(property_id::BACKGROUND_POSITION)?, id)
                        } else {
                            get(id)
                        }
                    })
                    .collect::<Option<Vec<_>>>()?;
                let count = values
                    .iter()
                    .skip(usize::from(background))
                    .map(|value| layer_count(value))
                    .max()
                    .unwrap_or(1);
                let initials = ids
                    .iter()
                    .map(|id| text(&initial(*id), mode))
                    .collect::<Option<Vec<_>>>()?;
                for entry in 0..count {
                    if entry != 0 {
                        sink.push_ascii(", ");
                    }
                    let texts = values
                        .iter()
                        .map(|value| text(if count == 1 { value } else { layer(value, entry) }, mode))
                        .collect::<Option<Vec<_>>>()?;
                    let initial_at = |index: usize| texts[index].content_equals(&initials[index]);
                    let mut first = true;
                    for index in 0..ids.len() {
                        if background {
                            if (index == 0 && entry + 1 != count) || initial_at(index) {
                                continue;
                            }
                        } else {
                            if index == 2 {
                                continue;
                            }
                            let can_skip = match index {
                                1 => initial_at(2),
                                4 => initial_at(5) || sink_is_ascii_str(&texts[5], "no-clip"),
                                _ => true,
                            };
                            if (initial_at(index) && can_skip) || (index == 5 && texts[5].content_equals(&texts[4])) {
                                continue;
                            }
                        }
                        if !first {
                            sink.push_ascii(" ");
                        }
                        sink.push_sink(&texts[index]);
                        if !background && index == 1 && !initial_at(2) {
                            sink.push_ascii(" / ");
                            sink.push_sink(&texts[2]);
                        }
                        first = false;
                    }
                    if first {
                        sink.push_ascii("none");
                    }
                }
            }
            property_id::BORDER => {
                if !text(get(property_id::BORDER_IMAGE)?, mode)?
                    .content_equals(&text(&initial(property_id::BORDER_IMAGE), mode)?)
                {
                    return Some(());
                }
                let mut first = true;
                for id in [
                    property_id::BORDER_WIDTH,
                    property_id::BORDER_STYLE,
                    property_id::BORDER_COLOR,
                ] {
                    let value = get(id)?;
                    let StyleValueData::Shorthand { values, .. } = value else {
                        return None;
                    };
                    if !values
                        .as_slice()
                        .iter()
                        .all(|value| value.data() == values.as_slice()[0].data())
                    {
                        return Some(());
                    }
                }
                for id in [
                    property_id::BORDER_WIDTH,
                    property_id::BORDER_STYLE,
                    property_id::BORDER_COLOR,
                ] {
                    let value = get(id)?;
                    if is_initial(value, id) {
                        continue;
                    }
                    if !first {
                        sink.push_ascii(" ");
                    }
                    write(sink, value, mode)?;
                    first = false;
                }
                if first {
                    write(sink, get(property_id::BORDER_WIDTH)?, mode)?;
                }
            }
            property_id::FONT_VARIANT => {
                let ids = [
                    property_id::FONT_VARIANT_LIGATURES,
                    property_id::FONT_VARIANT_CAPS,
                    property_id::FONT_VARIANT_ALTERNATES,
                    property_id::FONT_VARIANT_NUMERIC,
                    property_id::FONT_VARIANT_EAST_ASIAN,
                    property_id::FONT_VARIANT_POSITION,
                    property_id::FONT_VARIANT_EMOJI,
                ];
                if keyword_is(get(ids[0])?, keyword::NONE)
                    && ids[1..]
                        .iter()
                        .any(|id| get(*id).is_none_or(|value| !keyword_is(value, keyword::NORMAL)))
                {
                    return Some(());
                }
                let mut first = true;
                for id in ids {
                    let value = get(id)?;
                    if keyword_is(value, keyword::NORMAL) {
                        continue;
                    }
                    if !first {
                        sink.push_ascii(" ");
                    }
                    write(sink, value, mode)?;
                    first = false;
                }
                if first {
                    sink.push_ascii("normal");
                }
            }
            property_id::FONT => {
                for id in [
                    property_id::FONT_FEATURE_SETTINGS,
                    property_id::FONT_KERNING,
                    property_id::FONT_LANGUAGE_OVERRIDE,
                    property_id::FONT_OPTICAL_SIZING,
                    property_id::FONT_VARIATION_SETTINGS,
                ] {
                    if !is_initial(get(id)?, id) {
                        return Some(());
                    }
                }
                let variant = text(get(property_id::FONT_VARIANT)?, mode)?;
                if ![
                    "normal",
                    "small-caps",
                    "initial",
                    "inherit",
                    "unset",
                    "revert",
                    "revert-layer",
                ]
                .iter()
                .any(|name| sink_is_ascii_str(&variant, name))
                {
                    return Some(());
                }
                let width = get(property_id::FONT_WIDTH)?;
                let width_keyword = match width {
                    StyleValueData::Keyword { keyword: code }
                        if [
                            keyword::NORMAL,
                            keyword::ULTRA_CONDENSED,
                            keyword::EXTRA_CONDENSED,
                            keyword::CONDENSED,
                            keyword::SEMI_CONDENSED,
                            keyword::SEMI_EXPANDED,
                            keyword::EXPANDED,
                            keyword::EXTRA_EXPANDED,
                            keyword::ULTRA_EXPANDED,
                        ]
                        .contains(code) =>
                    {
                        Some(*code)
                    }
                    _ => {
                        let percentage = match width {
                            StyleValueData::Percentage { value } => Some(*value),
                            StyleValueData::Calculated { .. } => {
                                crate::css::calc::resolve_calculated_percentage_without_context(width)
                            }
                            _ => None,
                        };
                        [
                            (50.0, keyword::ULTRA_CONDENSED),
                            (62.5, keyword::EXTRA_CONDENSED),
                            (75.0, keyword::CONDENSED),
                            (87.5, keyword::SEMI_CONDENSED),
                            (100.0, keyword::NORMAL),
                            (112.5, keyword::SEMI_EXPANDED),
                            (125.0, keyword::EXPANDED),
                            (150.0, keyword::EXTRA_EXPANDED),
                            (200.0, keyword::ULTRA_EXPANDED),
                        ]
                        .iter()
                        .find(|(value, _)| Some(*value) == percentage)
                        .map(|(_, code)| *code)
                    }
                };
                let Some(width_keyword) = width_keyword else {
                    return Some(());
                };
                let mut first = true;
                for id in [
                    property_id::FONT_STYLE,
                    property_id::FONT_VARIANT,
                    property_id::FONT_WEIGHT,
                ] {
                    let value = text(get(id)?, mode)?;
                    if sink_is_ascii_str(&value, "normal")
                        || (id == property_id::FONT_WEIGHT && sink_is_ascii_str(&value, "400"))
                    {
                        continue;
                    }
                    if !first {
                        sink.push_ascii(" ");
                    }
                    sink.push_sink(&value);
                    first = false;
                }
                if width_keyword != keyword::NORMAL {
                    if !first {
                        sink.push_ascii(" ");
                    }
                    sink.push_ascii(keyword::NAMES[width_keyword as usize]);
                    first = false;
                }
                if !first {
                    sink.push_ascii(" ");
                }
                write(sink, get(property_id::FONT_SIZE)?, mode)?;
                if !keyword_is(get(property_id::LINE_HEIGHT)?, keyword::NORMAL) {
                    sink.push_ascii(" / ");
                    write(sink, get(property_id::LINE_HEIGHT)?, mode)?;
                }
                sink.push_ascii(" ");
                write(sink, get(property_id::FONT_FAMILY)?, mode)?;
            }
            property_id::GRID_AREA => {
                let mut values = [
                    get(property_id::GRID_ROW_START)?,
                    get(property_id::GRID_COLUMN_START)?,
                    get(property_id::GRID_ROW_END)?,
                    get(property_id::GRID_COLUMN_END)?,
                ];
                let auto = |value: &StyleValueData| matches!(value, StyleValueData::GridTrackPlacement { kind: 0, .. });
                let ident = |value: &StyleValueData| matches!(value, StyleValueData::CustomIdent { .. });
                if auto(values[3]) && ident(values[1]) {
                    values[3] = values[1];
                }
                if auto(values[1]) && ident(values[0]) {
                    values = [values[0]; 4];
                }
                if auto(values[2]) && ident(values[0]) {
                    values[2] = values[0];
                }
                let count = if values.iter().all(|value| *value == values[0]) {
                    1
                } else if values[0] == values[2] && values[1] == values[3] {
                    2
                } else if values[1] == values[3] {
                    if auto(values[2]) {
                        if auto(values[1]) { 1 } else { 2 }
                    } else {
                        3
                    }
                } else {
                    4
                };
                for (index, value) in values[..count].iter().enumerate() {
                    if index != 0 {
                        sink.push_ascii(" / ");
                    }
                    write(sink, value, mode)?;
                }
            }
            property_id::GRID | property_id::GRID_TEMPLATE => {
                use crate::css::style_value::GridTrackEntryKind;
                if property == property_id::GRID {
                    let flow = get(property_id::GRID_AUTO_FLOW)?;
                    let auto_rows = get(property_id::GRID_AUTO_ROWS)?;
                    let auto_columns = get(property_id::GRID_AUTO_COLUMNS)?;
                    if !is_initial(flow, property_id::GRID_AUTO_FLOW)
                        || !is_initial(auto_rows, property_id::GRID_AUTO_ROWS)
                        || !is_initial(auto_columns, property_id::GRID_AUTO_COLUMNS)
                    {
                        let StyleValueData::GridAutoFlow { row, dense } = flow else {
                            return None;
                        };
                        if !is_initial(get(property_id::GRID_TEMPLATE_AREAS)?, property_id::GRID_TEMPLATE_AREAS) {
                            return Some(());
                        }
                        if *row
                            && is_initial(auto_columns, property_id::GRID_AUTO_COLUMNS)
                            && is_initial(get(property_id::GRID_TEMPLATE_ROWS)?, property_id::GRID_TEMPLATE_ROWS)
                        {
                            sink.push_ascii("auto-flow");
                            if *dense {
                                sink.push_ascii(" dense");
                            }
                            if !is_initial(auto_rows, property_id::GRID_AUTO_ROWS) {
                                sink.push_ascii(" ");
                                write(sink, auto_rows, mode)?;
                            }
                            sink.push_ascii(" / ");
                            write(sink, get(property_id::GRID_TEMPLATE_COLUMNS)?, mode)?;
                        } else if !*row
                            && is_initial(auto_rows, property_id::GRID_AUTO_ROWS)
                            && is_initial(
                                get(property_id::GRID_TEMPLATE_COLUMNS)?,
                                property_id::GRID_TEMPLATE_COLUMNS,
                            )
                        {
                            write(sink, get(property_id::GRID_TEMPLATE_ROWS)?, mode)?;
                            sink.push_ascii(" / auto-flow");
                            if *dense {
                                sink.push_ascii(" dense");
                            }
                            if !is_initial(auto_columns, property_id::GRID_AUTO_COLUMNS) {
                                sink.push_ascii(" ");
                                write(sink, auto_columns, mode)?;
                            }
                        }
                        return Some(());
                    }
                }
                let (
                    StyleValueData::GridTemplateArea {
                        grid_areas,
                        row_count,
                        column_count,
                    },
                    StyleValueData::GridTrackSizeList {
                        is_subgrid: rows_subgrid,
                        entries: rows,
                        ..
                    },
                    StyleValueData::GridTrackSizeList {
                        is_subgrid: columns_subgrid,
                        entries: columns,
                        ..
                    },
                ) = (
                    get(property_id::GRID_TEMPLATE_AREAS)?,
                    get(property_id::GRID_TEMPLATE_ROWS)?,
                    get(property_id::GRID_TEMPLATE_COLUMNS)?,
                )
                else {
                    return default(sink).then_some(());
                };
                let rows = rows.as_slice();
                let columns = columns.as_slice();
                if *row_count == 0 {
                    if rows.is_empty() && columns.is_empty() && !*rows_subgrid && !*columns_subgrid {
                        sink.push_ascii("none");
                        return Some(());
                    }
                    serialize_grid_track_size_list(sink, *rows_subgrid, rows, mode).then_some(())?;
                    sink.push_ascii(" / ");
                    serialize_grid_track_size_list(sink, *columns_subgrid, columns, mode).then_some(())?;
                    return Some(());
                }
                if *rows_subgrid
                    || *columns_subgrid
                    || rows
                        .iter()
                        .chain(columns)
                        .any(|entry| matches!(entry.kind, GridTrackEntryKind::Repeat))
                    || rows
                        .iter()
                        .filter(|entry| matches!(entry.kind, GridTrackEntryKind::Size | GridTrackEntryKind::MinMax))
                        .count()
                        != *row_count
                {
                    return Some(());
                }
                let mut row_index = 0;
                let mut row_text = TextSink::new();
                for (index, entry) in rows.iter().enumerate() {
                    match entry.kind {
                        GridTrackEntryKind::LineNames => {
                            if index != 0 {
                                row_text.push_ascii(" ");
                            }
                            serialize_grid_track_size_list(&mut row_text, false, std::slice::from_ref(entry), mode)
                                .then_some(())?;
                        }
                        GridTrackEntryKind::Size | GridTrackEntryKind::MinMax => {
                            if row_text.len() != 0 {
                                row_text.push_ascii(" ");
                            }
                            row_text.push_ascii("\"");
                            for column in 0..*column_count {
                                if column != 0 {
                                    row_text.push_ascii(" ");
                                }
                                match grid_areas
                                    .as_slice()
                                    .iter()
                                    .find(|area| area.covers_cell(row_index, column))
                                {
                                    Some(area) => with_fly_string_units(area.name(), |units| {
                                        push_units_raw(&mut row_text, &units);
                                    }),
                                    None => row_text.push_ascii("."),
                                }
                            }
                            row_text.push_ascii("\"");
                            let mut size = TextSink::new();
                            serialize_grid_track_size_list(&mut size, false, std::slice::from_ref(entry), mode)
                                .then_some(())?;
                            if !sink_is_ascii_str(&size, "auto") {
                                row_text.push_ascii(" ");
                                row_text.push_sink(&size);
                            }
                            row_index += 1;
                        }
                        GridTrackEntryKind::Repeat => unreachable!(),
                    }
                }
                sink.push_sink(&row_text);
                if !columns.is_empty() {
                    sink.push_ascii(" / ");
                    serialize_grid_track_size_list(sink, false, columns, mode).then_some(())?;
                }
            }
            _ => unreachable!(),
        }
        Some(())
    })()
    .is_some()
}
