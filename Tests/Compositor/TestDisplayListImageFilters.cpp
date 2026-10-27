/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/DisplayListRasterCache.h>
#include <Compositor/PaintingSurface.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/DisplayList/VisualContextTreeTestBuilder.h>
#include <LibGfx/Bitmap.h>
#include <LibTest/TestCase.h>
#include <new>

static Compositing::ImageFrameResourceId const image_id { 1 };
static Gfx::IntRect const rect { 0, 0, 1, 1 };

static NonnullRefPtr<Gfx::Bitmap> replace_image(Compositing::DisplayListResourceStorage& storage, Compositor::DisplayListRasterCache& cache, Gfx::Color color)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, rect.size()));
    bitmap->set_pixel(0, 0, color);
    Compositing::DisplayListResourceTransaction transaction;
    transaction.image_frames.append({ image_id, Gfx::DecodedImageFrame { *bitmap } });
    cache.evict(storage.apply_transaction(move(transaction)));
    return bitmap;
}

static Gfx::Color render(Compositor::DisplayListPlayerSkia& player, Compositing::DisplayList const& list, Compositing::AccumulatedVisualContextTree const& tree, Compositing::DisplayListResourceStorage const& storage, Compositor::DisplayListRasterCache& cache)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, rect.size()));
    auto surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
    player.execute(list, tree, storage, cache, {}, surface, nullptr, nullptr);
    player.flush(*surface);
    return bitmap->get_pixel(0, 0);
}

TEST_CASE(image_filters_use_the_current_resource_storage)
{
    Compositing::VisualContextTreeTestBuilder builder;
    auto effect = builder.append_image_filter(image_id.value(), rect);
    auto tree = builder.finish();
    auto list = Compositing::VisualContextTreeTestBuilder::filled_rect_display_list(tree, { .effect = effect }, rect, Gfx::Color::White);
    Compositor::DisplayListPlayerSkia player { nullptr };
    Compositing::DisplayListResourceStorage blue_storage;
    Compositing::DisplayListResourceStorage green_storage;
    Compositor::DisplayListRasterCache blue_cache;
    Compositor::DisplayListRasterCache green_cache;
    (void)replace_image(blue_storage, blue_cache, Gfx::Color::Blue);
    (void)replace_image(green_storage, green_cache, Gfx::Color::Green);

    EXPECT_EQ(render(player, *list, tree, blue_storage, blue_cache), Gfx::Color::Blue);
    EXPECT_EQ(render(player, *list, tree, green_storage, green_cache), Gfx::Color::Green);
    EXPECT_EQ(render(player, *list, tree, blue_storage, blue_cache), Gfx::Color::Blue);
}

TEST_CASE(image_filters_drop_replaced_image_resources)
{
    Compositing::VisualContextTreeTestBuilder builder;
    auto effect = builder.append_image_filter(image_id.value(), rect);
    auto tree = builder.finish();
    auto list = Compositing::VisualContextTreeTestBuilder::filled_rect_display_list(tree, { .effect = effect }, rect, Gfx::Color::White);
    Compositor::DisplayListPlayerSkia player { nullptr };
    Compositing::DisplayListResourceStorage storage;
    Compositor::DisplayListRasterCache cache;
    auto original_bitmap = replace_image(storage, cache, Gfx::Color::Blue);
    EXPECT_EQ(render(player, *list, tree, storage, cache), Gfx::Color::Blue);
    (void)replace_image(storage, cache, Gfx::Color::Green);
    EXPECT_EQ(render(player, *list, tree, storage, cache), Gfx::Color::Green);
    EXPECT_EQ(original_bitmap->get_pixel(0, 0), Gfx::Color::Blue);
}

TEST_CASE(image_filters_do_not_keep_destroyed_resource_namespaces)
{
    Compositing::VisualContextTreeTestBuilder builder;
    auto effect = builder.append_image_filter(image_id.value(), rect);
    auto tree = builder.finish();
    auto list = Compositing::VisualContextTreeTestBuilder::filled_rect_display_list(tree, { .effect = effect }, rect, Gfx::Color::White);
    Compositor::DisplayListPlayerSkia player { nullptr };
    struct ResourceNamespace {
        Compositing::DisplayListResourceStorage storage;
        Compositor::DisplayListRasterCache cache;
    };
    alignas(ResourceNamespace) u8 memory[sizeof(ResourceNamespace)];
    Vector<NonnullRefPtr<Gfx::Bitmap>> kept_images;
    for (auto color : { Gfx::Color::Blue, Gfx::Color::Green }) {
        auto* resources = new (memory) ResourceNamespace;
        kept_images.append(replace_image(resources->storage, resources->cache, color));
        EXPECT_EQ(render(player, *list, tree, resources->storage, resources->cache), color);
        resources->~ResourceNamespace();
    }
}
