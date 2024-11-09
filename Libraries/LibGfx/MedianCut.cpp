/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/Statistics.h>
#include <LibGfx/MedianCut.h>

namespace Gfx {

namespace {

using Bucket = Vector<ARGB32>;
using Buckets = Vector<Bucket>;

void sort_along_color(Bucket& bucket, u8 color_index)
{
    auto less_than = [=](ARGB32 first, ARGB32 second) {
        auto const first_color = Color::from_argb(first);
        auto const second_color = Color::from_argb(second);
        switch (color_index) {
        case 0:
            return first_color.red() < second_color.red();
        case 1:
            return first_color.green() < second_color.green();
        case 2:
            return first_color.blue() < second_color.blue();
        default:
            VERIFY_NOT_REACHED();
        }
    };

    AK::quick_sort(bucket, less_than);
}

template<typename T>
struct MaxAndIndex {
    T maximum;
    u32 index;
};

template<typename T, class GreaterThan>
MaxAndIndex<T> max_and_index(Span<T> values, GreaterThan greater_than)
{
    VERIFY(values.size() != 0);

    u32 max_index = 0;
    RemoveCV<T> max_value = values[0];
    for (u32 i = 0; i < values.size(); ++i) {
        if (greater_than(values[i], max_value)) {
            max_value = values[i];
            max_index = i;
        }
    }

    return { max_value, max_index };
}

ErrorOr<void> split_bucket(Buckets& buckets, u32 index_to_split_at, u8 color_index)
{
    auto& to_split = buckets[index_to_split_at];

    sort_along_color(to_split, color_index);

    Bucket new_bucket {};

    auto const middle = to_split.size() / 2;

    auto const span_to_move = to_split.span().slice(middle);
    // FIXME: Make Vector::try_extend() take a span
    for (u32 i = 0; i < span_to_move.size(); ++i)
        TRY(new_bucket.try_append(span_to_move[i]));
    to_split.remove(middle, span_to_move.size());

    TRY(buckets.try_append(move(new_bucket)));

    return {};
}

struct IndexAndChannel {
    u32 bucket_index {};
    float score {};
    u8 color_index {};
};

ErrorOr<Optional<IndexAndChannel>> find_largest_bucket(Buckets const& buckets)
{
    Vector<IndexAndChannel> bucket_stats {};

    for (u32 i = 0; i < buckets.size(); ++i) {
        auto const& bucket = buckets[i];

        if (bucket.size() == 1)
            continue;

        Statistics<u32> red {};
        Statistics<u32> green {};
        Statistics<u32> blue {};
        for (auto const argb : bucket) {
            auto const color = Color::from_argb(argb);
            red.add(color.red());
            green.add(color.green());
            blue.add(color.blue());
        }

        Array const variances = { red.variance(), green.variance(), blue.variance() };

        auto const stats = max_and_index(variances.span(), [](auto a, auto b) { return a > b; });

        TRY(bucket_stats.try_append({ i, stats.maximum, static_cast<u8>(stats.index) }));
    }

    if (bucket_stats.size() == 0)
        return OptionalNone {};

    return bucket_stats[max_and_index(bucket_stats.span(), [](auto a, auto b) { return a.score > b.score; }).index];
}

ErrorOr<void> split_largest_bucket(Buckets& buckets)
{
    if (auto const bucket_info = TRY(find_largest_bucket(buckets)); bucket_info.has_value())
        TRY(split_bucket(buckets, bucket_info->bucket_index, bucket_info->color_index));

    return {};
}

ErrorOr<ColorPalette> color_palette_from_buckets(Buckets const& buckets)
{
    Vector<Color> palette;
    HashMap<Color, ColorPalette::ColorAndIndex> conversion_table;

    for (auto const& bucket : buckets) {
        u32 average_r {};
        u32 average_g {};
        u32 average_b {};

        for (auto const argb : bucket) {
            auto const color = Color::from_argb(argb);
            average_r += color.red();
            average_g += color.green();
            average_b += color.blue();
        }

        auto const bucket_size = bucket.size();
        auto const average_color = Color(
            round_to<u32>(static_cast<double>(average_r) / bucket_size),
            round_to<u32>(static_cast<double>(average_g) / bucket_size),
            round_to<u32>(static_cast<double>(average_b) / bucket_size));

        TRY(palette.try_append(average_color));
        for (auto const color : bucket)
            TRY(conversion_table.try_set(Color::from_argb(color), { average_color, palette.size() - 1 }));
    }

    return ColorPalette { move(palette), move(conversion_table) };
}

}

ErrorOr<ColorPalette> median_cut(Bitmap const& bitmap, u16 palette_size)
{
    HashTable<ARGB32> color_set;
    for (auto color : bitmap)
        TRY(color_set.try_set(color));

    Vector<ARGB32> first_bucket;
    TRY(first_bucket.try_ensure_capacity(color_set.size()));
    for (auto const color : color_set)
        first_bucket.append(color);

    Buckets bucket_list;
    TRY(bucket_list.try_append(first_bucket));

    u16 old_bucket_size = 0;
    while (bucket_list.size() > old_bucket_size && bucket_list.size() < palette_size) {
        old_bucket_size = bucket_list.size();
        TRY(split_largest_bucket(bucket_list));
    }

    return color_palette_from_buckets(bucket_list);
}

}
