/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibTest/TestCase.h>

#define TEST_INPUT(x) ("test-inputs/" x)

auto small_image = Core::File::open(TEST_INPUT("jpg/rgb24.jpg"sv), Core::File::OpenMode::Read).release_value()->read_until_eof().release_value();
auto big_image = Core::File::open(TEST_INPUT("jpg/big_image.jpg"sv), Core::File::OpenMode::Read).release_value()->read_until_eof().release_value();
auto rgb_image = Core::File::open(TEST_INPUT("jpg/rgb_components.jpg"sv), Core::File::OpenMode::Read).release_value()->read_until_eof().release_value();
auto several_scans = Core::File::open(TEST_INPUT("jpg/several_scans.jpg"sv), Core::File::OpenMode::Read).release_value()->read_until_eof().release_value();

BENCHMARK_CASE(small_image)
{
    auto plugin_decoder = MUST(Gfx::JPEGImageDecoderPlugin::create(small_image));
    MUST(plugin_decoder->frame(0));
}

BENCHMARK_CASE(big_image)
{
    auto plugin_decoder = MUST(Gfx::JPEGImageDecoderPlugin::create(big_image));
    MUST(plugin_decoder->frame(0));
}

BENCHMARK_CASE(rgb_image)
{
    auto plugin_decoder = MUST(Gfx::JPEGImageDecoderPlugin::create(rgb_image));
    MUST(plugin_decoder->frame(0));
}

BENCHMARK_CASE(several_scans)
{
    auto plugin_decoder = MUST(Gfx::JPEGImageDecoderPlugin::create(several_scans));
    MUST(plugin_decoder->frame(0));
}
