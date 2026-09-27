/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace {

class TestPageClient final : public Web::PageClient {
    GC_CELL(TestPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(TestPageClient);

public:
    TestPageClient()
        : m_palette(Gfx::PaletteImpl::create_with_anonymous_buffer(MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)))))
    {
    }

    virtual Compositing::PageId id() const override { return Compositing::PageId { 1 }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { return m_palette; }
    virtual Compositing::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1; }
    virtual double device_pixel_ratio() const override { return 1; }
    virtual double device_pixels_per_css_pixel() const override { return 1; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event(Compositing::PageId, u64, Web::EventResult) override { }
    virtual Web::HTML::CrossProcessId allocate_cross_process_id() override { return { 1, m_next_cross_process_id++ }; }
    virtual void request_frame() override { }
    virtual void request_file(Web::FileRequest) override { }
    virtual bool is_headless() const override { return true; }
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    Gfx::Palette m_palette;
    GC::Ptr<Web::Page> m_page;
    u64 m_next_cross_process_id { 1 };
};

GC_DEFINE_ALLOCATOR(TestPageClient);

}

TEST_CASE(replacing_custom_property_only_inline_blocks_updates_dependent_style)
{
    static Web::Platform::FontPlugin font_plugin(false);
    Web::Platform::FontPlugin::install(font_plugin);
    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal_realm.ptr());
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    if (!document->body())
        MUST(document->populate_with_html_head_and_body());
    auto parent = MUST(Web::DOM::create_element(*document, "div"_utf16_fly_string, Web::Namespace::HTML));
    auto child = MUST(Web::DOM::create_element(*document, "div"_utf16_fly_string, Web::Namespace::HTML));
    MUST(document->body()->append_child(parent));
    MUST(parent->append_child(child));
    MUST(child->style()->set_property(Web::CSS::PropertyID::Color, "var(--paint, black)"_utf16));
    document->update_style();

    auto replace_block = [&](Utf16View declarations, Utf16View expected_color) {
        auto block = Web::CSS::CSSStyleProperties::create({}, {});
        MUST(block->set_css_text(declarations));
        parent->set_inline_style(block);
        document->update_style();
        auto resolved = Web::CSS::CSSStyleProperties::create_resolved_style(Web::DOM::AbstractElement { *child });
        EXPECT_EQ(resolved->get_property_value("color"_utf16_fly_string), expected_color);
    };
    replace_block("--paint: red"_utf16, "rgb(255, 0, 0)"_utf16);
    replace_block("--paint: blue"_utf16, "rgb(0, 0, 255)"_utf16);
    replace_block(""_utf16, "rgb(0, 0, 0)"_utf16);
}
