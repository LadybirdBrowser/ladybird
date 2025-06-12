/*
 * Copyright (c) 2022, Martin Falisse <mfalisse@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/Layout/AvailableSpace.h>

namespace Web::CSS {

class GridSize {
public:
    enum class Type {
        LengthPercentage,
        FlexibleLength,
        FitContent,
        MaxContent,
        MinContent,
    };

    GridSize(Type, LengthPercentage);
    GridSize(LengthPercentage);
    GridSize(Flex);
    GridSize(Type);
    GridSize();
    ~GridSize();

    static GridSize make_auto();

    Type type() const { return m_type; }

    bool is_auto(Layout::AvailableSize const&) const;
    bool is_fixed(Layout::AvailableSize const&) const;
    bool is_flexible_length() const { return m_type == Type::FlexibleLength; }
    bool is_fit_content() const { return m_type == Type::FitContent; }
    bool is_max_content() const { return m_type == Type::MaxContent; }
    bool is_min_content() const { return m_type == Type::MinContent; }

    LengthPercentage length_percentage() const { return m_value.get<LengthPercentage>(); }
    double flex_factor() const { return m_value.get<Flex>().to_fr(); }

    // https://www.w3.org/TR/css-grid-2/#layout-algorithm
    // An intrinsic sizing function (min-content, max-content, auto, fit-content()).
    bool is_intrinsic(Layout::AvailableSize const&) const;

    bool is_definite() const
    {
        return type() == Type::LengthPercentage && !length_percentage().is_auto();
    }

    Size css_size() const;

    String to_string() const;
    bool operator==(GridSize const& other) const
    {
        return m_type == other.type()
            && m_value == other.m_value;
    }

private:
    Type m_type;
    Variant<Empty, LengthPercentage, Flex> m_value;
};

class GridFitContent {
public:
    GridFitContent(GridSize);
    GridFitContent();

    GridSize max_grid_size() const& { return m_max_grid_size; }

    String to_string() const;
    bool operator==(GridFitContent const& other) const
    {
        return m_max_grid_size == other.m_max_grid_size;
    }

private:
    GridSize m_max_grid_size;
};

class GridMinMax {
public:
    GridMinMax(CSS::GridSize min_grid_size, CSS::GridSize max_grid_size);
    GridMinMax() = default;

    GridSize min_grid_size() const& { return m_min_grid_size; }
    GridSize max_grid_size() const& { return m_max_grid_size; }

    String to_string() const;
    bool operator==(GridMinMax const& other) const
    {
        return m_min_grid_size == other.min_grid_size()
            && m_max_grid_size == other.max_grid_size();
    }

private:
    GridSize m_min_grid_size;
    GridSize m_max_grid_size;
};

struct GridLineNames {
    Vector<String> names;

    String to_string() const;
    bool operator==(GridLineNames const& other) const { return names == other.names; }
};

class GridTrackSizeList {
public:
    GridTrackSizeList(Vector<Variant<ExplicitGridTrack, GridLineNames>>&& list);
    GridTrackSizeList();

    static GridTrackSizeList make_none();

    Vector<CSS::ExplicitGridTrack> track_list() const;
    Vector<Variant<ExplicitGridTrack, GridLineNames>> list() const { return m_list; }

    String to_string() const;
    bool operator==(GridTrackSizeList const& other) const;

private:
    Vector<Variant<ExplicitGridTrack, GridLineNames>> m_list;
};

class GridRepeat {
public:
    enum class Type {
        AutoFit,
        AutoFill,
        Default,
    };
    GridRepeat(GridTrackSizeList, int repeat_count);
    GridRepeat(GridTrackSizeList, Type);
    GridRepeat();

    bool is_auto_fill() const { return m_type == Type::AutoFill; }
    bool is_auto_fit() const { return m_type == Type::AutoFit; }
    bool is_default() const { return m_type == Type::Default; }
    int repeat_count() const
    {
        VERIFY(is_default());
        return m_repeat_count;
    }
    GridTrackSizeList grid_track_size_list() const& { return m_grid_track_size_list; }
    Type type() const& { return m_type; }

    String to_string() const;
    bool operator==(GridRepeat const& other) const
    {
        if (m_type != other.type())
            return false;
        if (m_type == Type::Default && m_repeat_count != other.repeat_count())
            return false;
        return m_grid_track_size_list == other.grid_track_size_list();
    }

private:
    Type m_type;
    GridTrackSizeList m_grid_track_size_list;
    int m_repeat_count { 0 };
};

class ExplicitGridTrack {
public:
    enum class Type {
        FitContent,
        MinMax,
        Repeat,
        Default,
    };
    ExplicitGridTrack(CSS::GridFitContent);
    ExplicitGridTrack(CSS::GridRepeat);
    ExplicitGridTrack(CSS::GridMinMax);
    ExplicitGridTrack(CSS::GridSize);

    bool is_fit_content() const { return m_type == Type::FitContent; }
    GridFitContent fit_content() const
    {
        VERIFY(is_fit_content());
        return m_grid_fit_content;
    }

    bool is_repeat() const { return m_type == Type::Repeat; }
    GridRepeat repeat() const
    {
        VERIFY(is_repeat());
        return m_grid_repeat;
    }

    bool is_minmax() const { return m_type == Type::MinMax; }
    GridMinMax minmax() const
    {
        VERIFY(is_minmax());
        return m_grid_minmax;
    }

    bool is_default() const { return m_type == Type::Default; }
    GridSize grid_size() const
    {
        VERIFY(is_default());
        return m_grid_size;
    }

    Type type() const { return m_type; }

    String to_string() const;
    bool operator==(ExplicitGridTrack const& other) const
    {
        if (is_fit_content() && other.is_fit_content())
            return m_grid_fit_content == other.fit_content();
        if (is_repeat() && other.is_repeat())
            return m_grid_repeat == other.repeat();
        if (is_minmax() && other.is_minmax())
            return m_grid_minmax == other.minmax();
        if (is_default() && other.is_default())
            return m_grid_size == other.grid_size();
        return false;
    }

private:
    Type m_type;
    GridFitContent m_grid_fit_content;
    GridRepeat m_grid_repeat;
    GridMinMax m_grid_minmax;
    GridSize m_grid_size;
};

}
