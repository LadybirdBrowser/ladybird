/*
 * Copyright (c) 2022, Martin Falisse <mfalisse@outlook.com>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTrackSize.h"
#include <AK/String.h>
#include <LibWeb/CSS/Size.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>

namespace Web::CSS {

GridSize::GridSize(NonnullRefPtr<StyleValue const> size)
    : m_value(move(size))
{
}

GridSize::~GridSize() = default;

bool GridSize::is_auto(Layout::AvailableSize const& available_size) const
{
    if (m_value->to_keyword() == Keyword::Auto)
        return true;

    if (m_value->is_percentage() || (m_value->is_calculated() && m_value->as_calculated().contains_percentage()))
        return !available_size.is_definite();

    return false;
}

static bool is_length_percentage(NonnullRefPtr<StyleValue const> const& value)
{
    return value->is_length() || value->is_percentage() || (value->is_calculated() && value->as_calculated().resolves_to_length());
}

bool GridSize::is_fixed(Layout::AvailableSize const& available_size) const
{
    if (!is_length_percentage(m_value))
        return false;

    if (m_value->is_percentage() || (m_value->is_calculated() && m_value->as_calculated().contains_percentage()))
        return available_size.is_definite();

    return true;
}

bool GridSize::is_flexible_length() const
{
    return m_value->is_flex() || (m_value->is_calculated() && m_value->as_calculated().resolves_to_flex());
}

bool GridSize::is_fit_content() const
{
    if (m_value->to_keyword() == Keyword::FitContent)
        return true;

    if (m_value->is_function() && m_value->as_function().name() == "fit-content"_fly_string)
        return true;

    return false;
}

bool GridSize::is_max_content() const
{
    return m_value->to_keyword() == Keyword::MaxContent;
}

bool GridSize::is_min_content() const
{
    return m_value->to_keyword() == Keyword::MinContent;
}

bool GridSize::is_intrinsic(Layout::AvailableSize const& available_size) const
{
    return is_auto(available_size) || is_max_content() || is_min_content() || is_fit_content();
}

bool GridSize::is_definite() const
{
    return is_length_percentage(m_value);
}

GridSize GridSize::make_auto()
{
    return GridSize(KeywordStyleValue::create(Keyword::Auto));
}

void GridSize::serialize(StringBuilder& builder, SerializationMode mode) const
{
    m_value->serialize(builder, mode);
}

String GridSize::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return MUST(builder.to_string());
}

GridSize GridSize::absolutized(ComputationContext const& context) const
{
    auto absolutized_value = m_value->absolutized(context);

    if (absolutized_value == m_value)
        return *this;

    return GridSize { absolutized_value };
}

GridMinMax::GridMinMax(GridSize min_grid_size, GridSize max_grid_size)
    : m_min_grid_size(move(min_grid_size))
    , m_max_grid_size(move(max_grid_size))
{
}

void GridMinMax::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("minmax("sv);
    m_min_grid_size.serialize(builder, mode);
    builder.append(", "sv);
    m_max_grid_size.serialize(builder, mode);
    builder.append(")"sv);
}

String GridMinMax::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return MUST(builder.to_string());
}

GridMinMax GridMinMax::absolutized(ComputationContext const& context) const
{
    return GridMinMax {
        m_min_grid_size.absolutized(context),
        m_max_grid_size.absolutized(context),
    };
}

GridRepeat::GridRepeat(GridRepeatType grid_repeat_type, GridTrackSizeList&& grid_track_size_list, RefPtr<StyleValue const> repeat_count)
    : m_type(grid_repeat_type)
    , m_grid_track_size_list(move(grid_track_size_list))
    , m_repeat_count(move(repeat_count))
{
}

GridRepeat::GridRepeat(GridTrackSizeList&& grid_track_size_list, GridRepeatParams const& params)
    : GridRepeat(params.type, move(grid_track_size_list), params.count)
{
}

void GridRepeat::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("repeat("sv);
    switch (m_type) {
    case GridRepeatType::AutoFit:
        builder.append("auto-fit"sv);
        break;
    case GridRepeatType::AutoFill:
        builder.append("auto-fill"sv);
        break;
    case GridRepeatType::Fixed:
        m_repeat_count->serialize(builder, mode);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    builder.append(", "sv);
    m_grid_track_size_list.serialize(builder, mode);
    builder.append(")"sv);
}

String GridRepeat::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return MUST(builder.to_string());
}

GridRepeat GridRepeat::absolutized(ComputationContext const& context) const
{
    return GridRepeat {
        m_type,
        m_grid_track_size_list.absolutized(context),
        m_repeat_count ? RefPtr<StyleValue const> { m_repeat_count->absolutized(context) } : nullptr,
    };
}

ExplicitGridTrack::ExplicitGridTrack(Variant<GridRepeat, GridMinMax, GridSize>&& value)
    : m_value(move(value))
{
}

void ExplicitGridTrack::serialize(StringBuilder& builder, SerializationMode mode) const
{
    m_value.visit([&builder, mode](auto const& track) {
        track.serialize(builder, mode);
    });
}

String ExplicitGridTrack::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return MUST(builder.to_string());
}

ExplicitGridTrack ExplicitGridTrack::absolutized(ComputationContext const& context) const
{
    return m_value.visit([&](auto const& it) {
        return ExplicitGridTrack { it.absolutized(context) };
    });
}

void GridLineNames::serialize(StringBuilder& builder) const
{
    builder.append("["sv);
    for (size_t i = 0; i < m_names.size(); ++i) {
        if (i > 0)
            builder.append(" "sv);
        builder.append(m_names[i].name);
    }
    builder.append("]"sv);
}

String GridLineNames::to_string() const
{
    StringBuilder builder;
    serialize(builder);
    return MUST(builder.to_string());
}

GridTrackSizeList GridTrackSizeList::make_none()
{
    return GridTrackSizeList();
}

void GridTrackSizeList::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (m_list.is_empty()) {
        builder.append("none"sv);
        return;
    }

    bool first = true;
    for (auto const& line_definition_or_name : m_list) {
        if (!first)
            builder.append(" "sv);
        first = false;
        if (line_definition_or_name.has<ExplicitGridTrack>()) {
            line_definition_or_name.get<ExplicitGridTrack>().serialize(builder, mode);
        } else if (line_definition_or_name.has<GridLineNames>()) {
            line_definition_or_name.get<GridLineNames>().serialize(builder);
        }
    }
}

String GridTrackSizeList::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return MUST(builder.to_string());
}

Vector<ExplicitGridTrack> GridTrackSizeList::track_list() const
{
    Vector<ExplicitGridTrack> track_list;
    for (auto const& line_definition_or_name : m_list) {
        if (line_definition_or_name.has<ExplicitGridTrack>())
            track_list.append(line_definition_or_name.get<ExplicitGridTrack>());
    }
    return track_list;
}

bool GridTrackSizeList::operator==(GridTrackSizeList const& other) const = default;

void GridTrackSizeList::append(GridLineNames&& line_names)
{
    if (!m_list.is_empty() && m_list.last().has<GridLineNames>()) {
        auto& last_line_names = m_list.last().get<GridLineNames>();
        for (auto const& name : line_names.names())
            last_line_names.append(name.name);
        return;
    }
    m_list.append(move(line_names));
}

void GridTrackSizeList::append(ExplicitGridTrack&& explicit_track)
{
    m_list.append(move(explicit_track));
}

GridTrackSizeList GridTrackSizeList::absolutized(ComputationContext const& context) const
{
    GridTrackSizeList result;
    for (auto const& item : m_list) {
        item.visit(
            [&result, &context](ExplicitGridTrack const& track) {
                result.append(track.absolutized(context));
            },
            [&result](GridLineNames names) {
                result.append(move(names));
            });
    }
    return result;
}

bool GridTrackSizeList::is_computationally_independent() const
{
    return all_of(m_list, [](auto const& item) {
        return item.visit(
            [](ExplicitGridTrack const& track) { return track.is_computationally_independent(); },
            [](GridLineNames const&) { return true; });
    });
}

}
