/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/DisjointChunks.h>
#include <AK/FixedArray.h>
#include <AK/Vector.h>

TEST_CASE(basic)
{
    DisjointChunks<size_t> chunks;
    EXPECT(chunks.is_empty());
    chunks.append({});
    EXPECT(chunks.is_empty());
    chunks.last_chunk().append(0);
    EXPECT(!chunks.is_empty());
    chunks.append({});
    chunks.last_chunk().append(1);
    chunks.last_chunk().append(2);
    chunks.last_chunk().append(3);
    chunks.append({});
    chunks.append({});
    chunks.last_chunk().append(4);

    for (size_t i = 0; i < 5u; ++i)
        EXPECT_EQ(chunks.at(i), i);

    auto it = chunks.begin();
    for (size_t i = 0; i < 5u; ++i, ++it)
        EXPECT_EQ(*it, i);

    EXPECT_EQ(it, chunks.end());

    DisjointChunks<size_t> new_chunks;
    new_chunks.extend(move(chunks));
    EXPECT_EQ(new_chunks.size(), 5u);

    new_chunks.last_chunk().append(5);

    auto cut_off_slice = new_chunks.release_slice(2, 3);
    EXPECT_EQ(new_chunks.size(), 3u);
    EXPECT_EQ(cut_off_slice.size(), 3u);

    EXPECT_EQ(cut_off_slice[0], 2u);
    EXPECT_EQ(cut_off_slice[1], 3u);
    EXPECT_EQ(cut_off_slice[2], 4u);

    EXPECT_EQ(new_chunks[0], 0u);
    EXPECT_EQ(new_chunks[1], 1u);
    EXPECT_EQ(new_chunks[2], 5u);
}

TEST_CASE(fixed_array)
{
    DisjointChunks<size_t, FixedArray<size_t>> chunks;
    EXPECT(chunks.is_empty());
    chunks.append({});
    EXPECT(chunks.is_empty());
    chunks.append(MUST(FixedArray<size_t>::create({ 0, 1 })));
    EXPECT(!chunks.is_empty());
    chunks.append({});
    chunks.append(MUST(FixedArray<size_t>::create(3)));
    chunks.last_chunk()[0] = 2;
    chunks.last_chunk()[1] = 3;
    chunks.last_chunk()[2] = 4;
    chunks.append({});
    chunks.append(MUST(FixedArray<size_t>::create(1)));
    chunks.last_chunk()[0] = 5;

    for (size_t i = 0; i < 6u; ++i)
        EXPECT_EQ(chunks.at(i), i);

    auto it = chunks.begin();
    for (size_t i = 0; i < 6u; ++i, ++it)
        EXPECT_EQ(*it, i);

    EXPECT_EQ(it, chunks.end());

    DisjointChunks<size_t, FixedArray<size_t>> new_chunks;
    new_chunks.extend(move(chunks));
    EXPECT_EQ(new_chunks.size(), 6u);

    auto cut_off_slice = new_chunks.release_slice(2, 3);
    EXPECT_EQ(new_chunks.size(), 3u);
    EXPECT_EQ(cut_off_slice.size(), 3u);

    EXPECT_EQ(cut_off_slice[0], 2u);
    EXPECT_EQ(cut_off_slice[1], 3u);
    EXPECT_EQ(cut_off_slice[2], 4u);

    EXPECT_EQ(new_chunks[0], 0u);
    EXPECT_EQ(new_chunks[1], 1u);
}

TEST_CASE(spans)
{
    DisjointChunks<size_t> chunks;
    chunks.append({ 0, 1, 2, 3, 4, 5 });
    chunks.append({ 6, 7, 8, 9 });

    auto spans = chunks.spans();
    EXPECT_EQ(spans.size(), 10u);

    auto slice = spans.slice(1, 4);
    EXPECT_EQ(slice.size(), 4u);
    EXPECT_EQ(slice[0], 1u);
    EXPECT_EQ(slice[1], 2u);
    EXPECT_EQ(slice[2], 3u);
    EXPECT_EQ(slice[3], 4u);

    auto cross_chunk_slice = spans.slice(4, 4);
    EXPECT_EQ(cross_chunk_slice.size(), 4u);
    EXPECT_EQ(cross_chunk_slice[0], 4u);
    EXPECT_EQ(cross_chunk_slice[1], 5u);
    EXPECT_EQ(cross_chunk_slice[2], 6u);
    EXPECT_EQ(cross_chunk_slice[3], 7u);

    auto it = cross_chunk_slice.begin();
    for (size_t i = 0; i < 4u; ++i, ++it)
        EXPECT_EQ(*it, i + 4u);

    EXPECT_EQ(it, cross_chunk_slice.end());
}

#define INIT_ITERATIONS (1'000'000)
#define ITERATIONS (100)

static DisjointChunks<int> basic_really_empty_chunks;

BENCHMARK_CASE(basic_really_empty)
{
    DisjointChunks<int> chunks;
    for (size_t i = 0; i < ITERATIONS; ++i)
        EXPECT(chunks.is_empty());
}

static DisjointChunks<int> basic_really_empty_large_chunks = []() {
    DisjointChunks<int> chunks;
    chunks.ensure_capacity(INIT_ITERATIONS);
    for (size_t i = 0; i < INIT_ITERATIONS; ++i)
        chunks.append({});
    return chunks;
}();

BENCHMARK_CASE(basic_really_empty_large)
{
    for (size_t i = 0; i < ITERATIONS; ++i)
        EXPECT(basic_really_empty_large_chunks.is_empty());
}

static DisjointChunks<int> basic_mostly_empty_chunks = []() {
    DisjointChunks<int> chunks;
    chunks.ensure_capacity(INIT_ITERATIONS + 1);
    for (size_t i = 0; i < INIT_ITERATIONS; ++i)
        chunks.append({});
    chunks.append({ 1, 2, 3 });
    return chunks;
}();

BENCHMARK_CASE(basic_mostly_empty)
{
    for (size_t i = 0; i < ITERATIONS; ++i) {
        EXPECT(!basic_mostly_empty_chunks.is_empty());
    }
}

static DisjointChunks<int> basic_full_chunks = []() {
    DisjointChunks<int> chunks;
    chunks.ensure_capacity(INIT_ITERATIONS + 1);
    for (size_t i = 0; i < INIT_ITERATIONS; ++i)
        chunks.append({ 1, 2, 3 });
    return chunks;
}();

BENCHMARK_CASE(basic_full)
{
    for (size_t i = 0; i < ITERATIONS; ++i)
        EXPECT(!basic_full_chunks.is_empty());
}
