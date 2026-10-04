/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/Document.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHeadElement.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace {

class CountingImageCodecPlugin final : public Web::Platform::ImageCodecPlugin {
public:
    virtual NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> decode_image(ReadonlyBytes, Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected) override
    {
        ++decode_count;
        auto promise = Core::Promise<Web::Platform::DecodedImage>::construct();
        promise->on_resolution = move(on_resolved);
        promise->on_rejection = move(on_rejected);
        Web::Platform::DecodedImage image;
        image.frame_count = 1;
        image.frames.empend(MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 32, 32 })), 0u);
        promise->resolve(move(image));
        return promise;
    }

    virtual void request_animation_frames(i64, u32, u32) override { VERIFY_NOT_REACHED(); }
    virtual void stop_animation_decode(i64) override { VERIFY_NOT_REACHED(); }

    size_t decode_count { 0 };
};

}

TEST_CASE(each_favicon_is_decoded_once)
{
    Core::EventLoop event_loop;
    static Web::Platform::FontPlugin font_plugin { false };
    Web::Platform::FontPlugin::install(font_plugin);
    static CountingImageCodecPlugin image_codec;
    Web::Platform::ImageCodecPlugin::install(image_codec);

    auto realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& page = Web::Bindings::principal_host_defined_page(*realm);
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page.set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };

    size_t expected_decode_count = 0;

    for (auto const& source : { "data:image/png,first"_utf16, "data:image/png,second"_utf16 }) {
        auto element = MUST(document->create_element("link"_utf16, Web::Bindings::ElementCreationOptions {}));
        auto& link = static_cast<Web::HTML::HTMLLinkElement&>(*element);
        link.set_rel("icon"_utf16);
        link.set_href(source);
        MUST(document->head()->append_child(element));
        Web::HTML::main_thread_event_loop().spin_until(GC::create_function(document->heap(), [link = GC::Ref { link }] {
            return link->has_loaded_icon();
        }));
        ++expected_decode_count;
        EXPECT_EQ(image_codec.decode_count, expected_decode_count);
    }

    EXPECT(document->has_active_favicon());
    EXPECT_EQ(image_codec.decode_count, 2u);

    // Selecting among the same loaded icons again must reuse their decoded bitmaps.
    for (size_t i = 0; i < 3; ++i)
        document->check_favicon_after_loading_link_resource();

    EXPECT_EQ(image_codec.decode_count, 2u);
}
