/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HistoryExecutor.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace {

class TestPageClient final : public Web::PageClient {
    GC_CELL(TestPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(TestPageClient);

public:
    virtual Compositing::PageId id() const override { return Compositing::PageId { 1 }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { VERIFY_NOT_REACHED(); }
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

    virtual void request_navigation_population(Web::HTML::LocalNavigable&, Web::NavigationTarget, Web::HTML::NavigationPopulationRequest) override { }
    virtual void page_did_request_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters) override { m_last_history_operation = operation_id; }

    Optional<Web::HTML::CrossProcessId> m_last_history_operation;
    GC::Ptr<Web::Page> m_page;
    u64 m_next_cross_process_id { 1 };
};

GC_DEFINE_ALLOCATOR(TestPageClient);

}

TEST_CASE(superseded_navigation_keeps_the_successors_container_load_delay)
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
    auto element = MUST(Web::DOM::create_element(*document, "iframe"_utf16_fly_string, Web::Namespace::HTML));
    MUST(document->body()->append_child(element));
    auto& container = as<Web::HTML::HTMLIFrameElement>(*element);
    auto& navigable = as<Web::HTML::LocalNavigable>(*container.content_navigable());
    navigable.set_has_session_history_entry_and_ready_for_navigation();
    navigable.active_document()->set_ready_for_post_load_tasks(true);
    navigable.set_delaying_load_events(false);
    EXPECT(!container.currently_delays_the_load_event());

    // Hold population in the test PageClient until after a successor owns the delay.
    navigable.request_population_for_reconstructed_history_entry({
        .navigable_id = navigable.id(),
        .history_entry = Web::HTML::create_pending_session_history_entry_descriptor(*navigable.active_session_history_entry()),
        .source_snapshot_params = {},
        .target_snapshot_params = {},
        .csp_navigation_type = {},
        .history_handling = {},
        .user_involvement = {},
        .navigation_id = "predecessor"_utf16,
    });
    navigable.set_ongoing_navigation_without_informing_navigation_api("successor"_utf16);
    navigable.set_delaying_load_events(true);
    EXPECT(navigable.resume_navigation_params_creation("predecessor"_utf16, {}));
    EXPECT(navigable.is_delaying_load_events());
    EXPECT(container.currently_delays_the_load_event());
    EXPECT(document->anything_is_delaying_the_load_event());

    // Complete a stale finalization while the same successor is still held.
    IGNORE_USE_IN_ESCAPING_LAMBDA bool completed = false;
    Web::HTML::finalize_a_cross_document_navigation(navigable, Web::HTML::HistoryHandlingBehavior::Replace,
        Web::HTML::UserNavigationInvolvement::None, *navigable.active_session_history_entry(), nullptr,
        "predecessor"_utf16, GC::create_function(vm.heap(), [&](Web::HTML::HistoryStepResult) { completed = true; }));
    VERIFY(client->m_last_history_operation.has_value());
    auto operation_id = *client->m_last_history_operation;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool ready = false;
    page->history_executor().handle_ui_history_operation_started(operation_id,
        GC::create_function(vm.heap(), [&](Web::HistoryOperationReadyResult result) {
            EXPECT(result.has<Web::HTML::HistoryStepResult>());
            ready = true;
        }));
    EXPECT(ready);
    page->history_executor().complete_ui_history_operation(operation_id, Web::HTML::HistoryStepResult::Applied, {});
    EXPECT(completed);
    EXPECT(navigable.is_delaying_load_events());
    EXPECT(container.currently_delays_the_load_event());
    EXPECT(document->anything_is_delaying_the_load_event());

    navigable.set_ongoing_navigation_without_informing_navigation_api(Empty {});
    navigable.set_delaying_load_events(false);
    EXPECT(!container.currently_delays_the_load_event());
    EXPECT(!document->anything_is_delaying_the_load_event());
}
