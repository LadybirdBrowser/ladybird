/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Page/Page.h>

class TestPageClient final : public Web::PageClient {
    GC_CELL(TestPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(TestPageClient);

public:
    virtual u64 id() const override { return 1; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { VERIFY_NOT_REACHED(); }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1; }
    virtual double device_pixel_ratio() const override { return 1; }
    virtual double device_pixels_per_css_pixel() const override { return 1; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event(u64, Web::EventResult) override { }
    virtual void request_frame() override { }
    virtual void request_file(Web::FileRequest) override { }
    virtual bool is_headless() const override { return true; }
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    virtual bool page_did_request_traverse_the_history_by_delta(int delta, Web::HistoryTraversalPrecheck history_traversal_precheck) override
    {
        ++traversal_request_count;
        last_traversal_delta = delta;
        last_history_traversal_precheck = history_traversal_precheck;
        return accept_traversal_request;
    }

    bool accept_traversal_request { true };
    size_t traversal_request_count { 0 };
    Optional<int> last_traversal_delta;
    Optional<Web::HistoryTraversalPrecheck> last_history_traversal_precheck;
    GC::Ptr<Web::Page> m_page;
};

GC_DEFINE_ALLOCATOR(TestPageClient);

TEST_CASE(browser_traversal_requests_embedder)
{
    auto vm = JS::VM::create();
    auto client = vm->heap().allocate<TestPageClient>();
    auto page = Web::Page::create(*vm, client);
    client->m_page = page.ptr();

    page->traverse_the_history_by_delta(-1);

    EXPECT_EQ(client->traversal_request_count, 1uz);
    VERIFY(client->last_traversal_delta.has_value());
    EXPECT_EQ(*client->last_traversal_delta, -1);
    VERIFY(client->last_history_traversal_precheck.has_value());
    EXPECT_EQ(*client->last_history_traversal_precheck, Web::HistoryTraversalPrecheck::Needed);
}
