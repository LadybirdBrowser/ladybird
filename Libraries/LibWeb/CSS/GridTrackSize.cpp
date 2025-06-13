/*
 * Copyright (c) 2022, Martin Falisse <mfalisse@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTrackSize.h"
#include <AK/String.h>
#include <LibWeb/CSS/Size.h>

namespace Web::CSS {

GridSize::GridSize(Type type, LengthPercentage length_percentage)
    : m_value(move(length_percentage))
{
    VERIFY(type == Type::FitContent);
    m_type = type;
}

GridSize::GridSize(LengthPercentage length_percentage)
    : m_type(Type::LengthPercentage)
    , m_value(move(length_percentage))
{
}

GridSize::GridSize(Flex flex_factor)
    : m_type(Type::FlexibleLength)
    , m_value(move(flex_factor))
{
}

GridSize::GridSize(Type type)
    : m_value { Empty() }
{
    VERIFY(type == Type::MinContent || type == Type::MaxContent);
    m_type = type;
}

GridSize::~GridSize() = default;

bool GridSize::is_auto(Layout::AvailableSize const& available_size) const
{
    if (m_type == Type::LengthPercentage) {
        auto& length_percentage = m_value.get<LengthPercentage>();
        if (length_percentage.contains_percentage())
            return !available_size.is_definite();
        return length_percentage.is_auto();
    }

    return false;
}

bool GridSize::is_fixed(Layout::AvailableSize const& available_size) const
{
    if (m_type == Type::LengthPercentage) {
        auto& length_percentage = m_value.get<LengthPercentage>();
        if (length_percentage.contains_percentage())
            return available_size.is_definite();
        return !length_percentage.is_auto();
    }

    return false;
}

bool GridSize::is_intrinsic(Layout::AvailableSize const& available_size) const
{
    return is_auto(available_size) || is_max_content() || is_min_content() || is_fit_content();
}

GridSize GridSize::make_auto()
{
    return GridSize(CSS::Length::make_auto());
}

Size GridSize::css_size() const
{
    VERIFY(m_type == Type::LengthPercentage || m_type == Type::FitContent);
    auto& length_percentage = m_value.get<LengthPercentage>();
    if (length_percentage.is_auto())
        return CSS::Size::make_auto();
    if (length_percentage.is_length())
        return CSS::Size::make_length(length_percentage.length());
    if (length_percentage.is_calculated())
        return CSS::Size::make_calculated(length_percentage.calculated());
    return CSS::Size::make_percentage(length_percentage.percentage());
}

String GridSize::to_string() const
{
    switch (m_type) {
    case Type::LengthPercentage:
    case Type::FitContent:
        return m_value.get<LengthPercentage>().to_string();
    case Type::FlexibleLength:
        return m_value.get<Flex>().to_string();
    case Type::MaxContent:
        return "max-content"_string;
    case Type::MinContent:
        return "min-content"_string;
    }
    VERIFY_NOT_REACHED();
}

GridMinMax::GridMinMax(GridSize min_grid_size, GridSize max_grid_size)
    : m_min_grid_size(min_grid_size)
    , m_max_grid_size(max_grid_size)
{
}

String GridMinMax::to_string() const
{
    StringBuilder builder;
    builder.append("minmax("sv);
    builder.appendff("{}", m_min_grid_size.to_string());
    builder.append(", "sv);
    builder.appendff("{}", m_max_grid_size.to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

GridFitContent::GridFitContent(GridSize max_grid_size)
    : m_max_grid_size(max_grid_size)
{
}

String GridFitContent::to_string() const
{
    return MUST(String::formatted("fit-content({})", m_max_grid_size.to_string()));
}

GridRepeat::GridRepeat(GridTrackSizeList grid_track_size_list, int repeat_count)
    : m_type(Type::Default)
    , m_grid_track_size_list(grid_track_size_list)
    , m_repeat_count(repeat_count)
{
}

GridRepeat::GridRepeat(GridTrackSizeList grid_track_size_list, Type type)
    : m_type(type)
    , m_grid_track_size_list(grid_track_size_list)
{
}

String GridRepeat::to_string() const
{
    StringBuilder builder;
    builder.append("repeat("sv);
    switch (m_type) {
    case Type::AutoFit:
        builder.append("auto-fill"sv);
        break;
    case Type::AutoFill:
        builder.append("auto-fit"sv);
        break;
    case Type::Default:
        builder.appendff("{}", m_repeat_count);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    builder.append(", "sv);
    builder.appendff("{}", m_grid_track_size_list.to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

ExplicitGridTrack::ExplicitGridTrack(Variant<GridFitContent, GridRepeat, GridMinMax, GridSize>&& value)
    : m_value(move(value))
{
}

String ExplicitGridTrack::to_string() const
{
    return m_value.visit([](auto const& track) {
        return track.to_string();
    });
}

String GridLineNames::to_string() const
{
    StringBuilder builder;
    builder.append("["sv);
    builder.join(' ', names);
    builder.append("]"sv);
    return MUST(builder.to_string());
}

GridTrackSizeList::GridTrackSizeList(Vector<Variant<ExplicitGridTrack, GridLineNames>>&& list)
    : m_list(move(list))
{
}

GridTrackSizeList::GridTrackSizeList()
{
}

GridTrackSizeList GridTrackSizeList::make_none()
{
    return GridTrackSizeList();
}

String GridTrackSizeList::to_string() const
{
    if (m_list.is_empty())
        return "none"_string;

    StringBuilder builder;
    for (auto const& line_definition_or_name : m_list) {
        if (!builder.is_empty())
            builder.append(" "sv);
        if (line_definition_or_name.has<ExplicitGridTrack>()) {
            builder.append(line_definition_or_name.get<ExplicitGridTrack>().to_string());
        } else if (line_definition_or_name.has<GridLineNames>()) {
            auto const& line_names = line_definition_or_name.get<GridLineNames>();
            builder.append(line_names.to_string());
        }
    }
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

}
