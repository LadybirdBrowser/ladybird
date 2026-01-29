/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibGfx/ImageFormats/ImageDecoderStream.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibTest/TestCase.h>

#define TEST_INPUT(x) ("test-inputs/" x)

auto small_image_stream = adopt_ref(*new Gfx::ImageDecoderStream());
auto small_image_setup = [] {
    auto small_image = Core::File::open(TEST_INPUT("jpg/rgb24.jpg"sv), Core::File::OpenMode::Read).release_value()->read_until_eof().release_value();
    small_image_stream->append_chunk(move(small_image));
    small_image_stream->close();
    return Empty {};
}();

auto big_image_stream = adopt_ref(*new Gfx::ImageDecoderStream());
auto big_image_setup = [] {
    auto big_image = Core::File::open(TEST_INPUT("jpg/big_image.jpg"sv), Core::File::OpenMode::Read).release_value()->read_until_eof().release_value();
    big_image_stream->append_chunk(move(big_image));
    big_image_stream->close();
    return Empty {};
}();

auto rgb_image_stream = adopt_ref(*new Gfx::ImageDecoderStream());
auto rgb_image_setup = [] {
    auto rgb_image = Core::File::open(TEST_INPUT("jpg/rgb_components.jpg"sv), Core::File::OpenMode::Read).release_value()->read_until_eof().release_value();
    rgb_image_stream->append_chunk(move(rgb_image));
    rgb_image_stream->close();
    return Empty {};
}();

auto several_scans_stream = adopt_ref(*new Gfx::ImageDecoderStream());
auto several_scans_setup = [] {
    auto several_scans = Core::File::open(TEST_INPUT("jpg/several_scans.jpg"sv), Core::File::OpenMode::Read).release_value()->read_until_eof().release_value();
    several_scans_stream->append_chunk(move(several_scans));
    several_scans_stream->close();
    return Empty {};
}();

BENCHMARK_CASE(small_image)
{
    auto plugin_decoder = MUST(Gfx::JPEGImageDecoderPlugin::create(small_image_stream));
    MUST(plugin_decoder->frame(0));
}

BENCHMARK_CASE(big_image)
{
    auto plugin_decoder = MUST(Gfx::JPEGImageDecoderPlugin::create(big_image_stream));
    MUST(plugin_decoder->frame(0));
}

BENCHMARK_CASE(rgb_image)
{
    auto plugin_decoder = MUST(Gfx::JPEGImageDecoderPlugin::create(rgb_image_stream));
    MUST(plugin_decoder->frame(0));
}

BENCHMARK_CASE(several_scans)
{
    auto plugin_decoder = MUST(Gfx::JPEGImageDecoderPlugin::create(several_scans_stream));
    MUST(plugin_decoder->frame(0));
}
