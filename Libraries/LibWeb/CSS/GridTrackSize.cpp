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

namespace Web::CSS {

GridSize::GridSize(Size size)
    : m_value(move(size))
{
}

GridSize::GridSize(Flex flex_factor)
    : m_value(move(flex_factor))
{
}

GridSize::~GridSize() = default;

bool GridSize::is_auto(Layout::AvailableSize const& available_size) const
{
    if (auto const* size = m_value.get_pointer<Size>()) {
        if (size->is_auto())
            return true;
        if (size->contains_percentage())
            return !available_size.is_definite();
        return false;
    }

    return false;
}

bool GridSize::is_fixed(Layout::AvailableSize const& available_size) const
{
    if (auto const* size = m_value.get_pointer<Size>()) {
        if (!size->is_length_percentage())
            return false;
        if (size->contains_percentage())
            return available_size.is_definite();
        return true;
    }

    return false;
}

bool GridSize::is_flexible_length() const
{
    return m_value.has<Flex>();
}

bool GridSize::is_fit_content() const
{
    if (auto const* size = m_value.get_pointer<Size>())
        return size->is_fit_content();

    return false;
}

bool GridSize::is_max_content() const
{
    if (auto const* size = m_value.get_pointer<Size>())
        return size->is_max_content();

    return false;
}

bool GridSize::is_min_content() const
{
    if (auto const* size = m_value.get_pointer<Size>())
        return size->is_min_content();

    return false;
}

bool GridSize::is_intrinsic(Layout::AvailableSize const& available_size) const
{
    return m_value.visit(
        [&available_size](Size const& size) {
            return size.is_auto()
                || size.is_max_content()
                || size.is_min_content()
                || size.is_fit_content()
                || (size.contains_percentage() && !available_size.is_definite());
        },
        [](Flex const&) {
            return false;
        });
}

bool GridSize::is_definite() const
{
    return m_value.visit(
        [](Size const& size) { return size.is_length_percentage(); },
        [](Flex const&) { return false; });
}

GridSize GridSize::make_auto()
{
    return GridSize(Size::make_auto());
}

String GridSize::to_string(SerializationMode mode) const
{
    return m_value.visit([mode](auto const& it) { return it.to_string(mode); });
}

GridSize GridSize::absolutized(ComputationContext const& context) const
{
    auto absolutize_length_percentage = [&context](LengthPercentage const& length_percentage) -> Optional<LengthPercentage> {
        if (length_percentage.is_length()) {
            auto length = length_percentage.length().absolutize(context.length_resolution_context.viewport_rect, context.length_resolution_context.font_metrics, context.length_resolution_context.root_font_metrics);
            if (length.has_value())
                return length.release_value();
            return {};
        }

        if (length_percentage.is_calculated())
            return LengthPercentage::from_style_value(length_percentage.calculated()->absolutized(context));

        return {};
    };
    return m_value.visit(
        [&](Size const& size) -> GridSize {
            if (size.is_length_percentage()) {
                if (auto result = absolutize_length_percentage(size.length_percentage()); result.has_value())
                    return Size::make_length_percentage(result.release_value());
            }

            if (size.is_fit_content() && size.fit_content_available_space().has_value()) {
                if (auto result = absolutize_length_percentage(size.fit_content_available_space().value()); result.has_value()) {
                    return Size::make_fit_content(result.release_value());
                }
            }

            return GridSize { size };
        },
        [](Flex const& flex) {
            return GridSize { flex };
        });
}

GridMinMax::GridMinMax(GridSize min_grid_size, GridSize max_grid_size)
    : m_min_grid_size(move(min_grid_size))
    , m_max_grid_size(move(max_grid_size))
{
}

String GridMinMax::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append("minmax("sv);
    builder.appendff("{}", m_min_grid_size.to_string(mode));
    builder.append(", "sv);
    builder.appendff("{}", m_max_grid_size.to_string(mode));
    builder.append(")"sv);
    return MUST(builder.to_string());
}

GridMinMax GridMinMax::absolutized(ComputationContext const& context) const
{
    return GridMinMax {
        m_min_grid_size.absolutized(context),
        m_max_grid_size.absolutized(context),
    };
}

GridRepeat::GridRepeat(GridRepeatType grid_repeat_type, GridTrackSizeList&& grid_track_size_list, size_t repeat_count)
    : m_type(grid_repeat_type)
    , m_grid_track_size_list(move(grid_track_size_list))
    , m_repeat_count(repeat_count)
{
}

GridRepeat::GridRepeat(GridTrackSizeList&& grid_track_size_list, GridRepeatParams const& params)
    : GridRepeat(params.type, move(grid_track_size_list), params.count)
{
}

String GridRepeat::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append("repeat("sv);
    switch (m_type) {
    case GridRepeatType::AutoFit:
        builder.append("auto-fit"sv);
        break;
    case GridRepeatType::AutoFill:
        builder.append("auto-fill"sv);
        break;
    case GridRepeatType::Fixed:
        builder.appendff("{}", m_repeat_count);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    builder.append(", "sv);
    builder.appendff("{}", m_grid_track_size_list.to_string(mode));
    builder.append(")"sv);
    return MUST(builder.to_string());
}

GridRepeat GridRepeat::absolutized(ComputationContext const& context) const
{
    return GridRepeat {
        m_type,
        m_grid_track_size_list.absolutized(context),
        m_repeat_count,
    };
}

ExplicitGridTrack::ExplicitGridTrack(Variant<GridRepeat, GridMinMax, GridSize>&& value)
    : m_value(move(value))
{
}

String ExplicitGridTrack::to_string(SerializationMode mode) const
{
    return m_value.visit([&mode](auto const& track) {
        return track.to_string(mode);
    });
}

ExplicitGridTrack ExplicitGridTrack::absolutized(ComputationContext const& context) const
{
    return m_value.visit([&](auto const& it) {
        return ExplicitGridTrack { it.absolutized(context) };
    });
}

String GridLineNames::to_string() const
{
    StringBuilder builder;
    builder.append("["sv);
    for (size_t i = 0; i < m_names.size(); ++i) {
        if (i > 0)
            builder.append(" "sv);
        builder.append(m_names[i].name);
    }
    builder.append("]"sv);
    return MUST(builder.to_string());
}

GridTrackSizeList GridTrackSizeList::make_none()
{
    return GridTrackSizeList();
}

String GridTrackSizeList::to_string(SerializationMode mode) const
{
    if (m_list.is_empty())
        return "none"_string;

    StringBuilder builder;
    for (auto const& line_definition_or_name : m_list) {
        if (!builder.is_empty())
            builder.append(" "sv);
        if (line_definition_or_name.has<ExplicitGridTrack>()) {
            builder.append(line_definition_or_name.get<ExplicitGridTrack>().to_string(mode));
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

}
