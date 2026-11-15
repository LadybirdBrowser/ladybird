/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/NotificationsAPI/Notification.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWebCommon/NotificationsAPI/NotificationData.h>

namespace Web::NotificationsAPI {

// https://notifications.spec.whatwg.org/#icon-resource
class NotificationIcon final : public GC::Cell {
    GC_CELL(NotificationIcon, GC::Cell);
    GC_DECLARE_ALLOCATOR(NotificationIcon);

public:
    explicit NotificationIcon(GC::Ref<Notification> notification)
        : m_notification(notification)
    {
    }

    GC::Ref<Notification> m_notification;
    RefPtr<Gfx::Bitmap const> m_resource;
    GC::Ptr<Fetch::Infrastructure::FetchController> m_fetch_controller;

private:
    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_notification);
        visitor.visit(m_fetch_controller);
    }
};

GC_DEFINE_ALLOCATOR(NotificationIcon);

namespace {

struct NotificationDataAbi {
    FlatPtr title;
    FlatPtr body;
    FlatPtr tag;
    FlatPtr language;
    u8 direction;
    bool has_silent;
    bool silent;
    bool require_interaction;
    bool renotify;
};

}

extern "C" {
// Defined in Bindings/Rust/src/notifications_api/notification.rs.
u64 web_rust_notifications_api_id_for_testing(Notification*);
DOM::Document* web_rust_notifications_api_document(Notification*);
bool web_rust_notifications_api_is_pending(Notification*);
void web_rust_notifications_api_icon_bytes_arrived(Notification*);
void web_rust_notifications_api_icon_fetched(JSVM*, Notification*);
void web_rust_notifications_api_abandon(JSVM*, Notification*);
void web_rust_notifications_api_did_show(JSVM*, Notification*);
void web_rust_notifications_api_did_fail_to_show(JSVM*, Notification*);
void web_rust_notifications_api_did_click(JSVM*, Notification*);
void web_rust_notifications_api_did_close(JSVM*, Notification*);

u64 web_notifications_api_register_notification(HTML::Window*, Notification*);
void web_notifications_api_unregister_notification(HTML::Window*, u64 notification_id);
NotificationIcon* web_notifications_api_fetch_icon(Notification*, HTML::EnvironmentSettingsObject*, URL::FFI::RustUrl const*);
void web_notifications_api_stop_icon_fetch(NotificationIcon*);
void web_notifications_api_show_notification(HTML::Window*, u64 notification_id, NotificationDataAbi const*, NotificationIcon*);
void web_notifications_api_request_close_notification(HTML::Window*, u64 notification_id);
void web_notifications_api_abandon_notification(HTML::Window*, u64 notification_id);
void web_notifications_api_handle_notification_click(HTML::Window*, u64 notification_id, bool into_focus);
}

static_assert(to_underlying(Bindings::NotificationDirection::Auto) == 0);
static_assert(to_underlying(Bindings::NotificationDirection::Ltr) == 1);
static_assert(to_underlying(Bindings::NotificationDirection::Rtl) == 2);

static JSVM* the_vm()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

u64 Notification::id_for_testing() const
{
    return web_rust_notifications_api_id_for_testing(const_cast<Notification*>(this));
}

DOM::Document const* Notification::document() const
{
    return web_rust_notifications_api_document(const_cast<Notification*>(this));
}

void Notification::abandon()
{
    web_rust_notifications_api_abandon(the_vm(), this);
}

void Notification::did_show()
{
    web_rust_notifications_api_did_show(the_vm(), this);
}

void Notification::did_fail_to_show()
{
    web_rust_notifications_api_did_fail_to_show(the_vm(), this);
}

void Notification::did_click()
{
    web_rust_notifications_api_did_click(the_vm(), this);
}

void Notification::did_close(bool)
{
    web_rust_notifications_api_did_close(the_vm(), this);
}

u64 web_notifications_api_register_notification(HTML::Window* window, Notification* notification)
{
    auto& page = window->page();
    auto notification_id = page.allocate_notification_id();
    page.register_showing_notification(notification_id, *notification);
    return notification_id;
}

void web_notifications_api_unregister_notification(HTML::Window* window, u64 notification_id)
{
    window->page().unregister_notification(notification_id);
}

// The longest side an icon is sent to the platform at; both desktop platforms show icons smaller than this.
static constexpr int icon_maximum_side = 128;

// https://notifications.spec.whatwg.org/#fetch-a-notification-resource
NotificationIcon* web_notifications_api_fetch_icon(Notification* notification, HTML::EnvironmentSettingsObject* settings, URL::FFI::RustUrl const* rust_url)
{
    URL::URL url;
    url.set_from_rust(*rust_url);
    auto icon = GC::Heap::the().allocate<NotificationIcon>(*notification);
    GC::Ptr<Page> page;
    if (auto* window = HTML::window_from_global_object(settings->global_object()))
        page = window->page();

    // 1. Let request be a new request whose URL is url, client is settings, destination is "image", mode is
    //    "no-cors", and credentials mode is "include".
    auto request = Fetch::Infrastructure::Request::create();
    request->set_url(url);
    request->set_client(settings->address());
    request->set_destination(Fetch::Infrastructure::Request::Destination::Image);
    request->set_mode(Fetch::Infrastructure::Request::Mode::NoCORS);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);

    // 2. If settings is null, then set request's origin to notification's origin, request's referrer to
    //    "no-referrer", and request's service-workers mode to "none".
    // NB: A page's notification always has its settings object.

    // 3. Fetch request with useParallelQueue set to true and processResponseConsumeBody set to the following steps
    //    given a response response and null, failure, or a byte sequence bytes:
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::ConsumeForCallback };
    fetch_algorithms_input.process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(icon, page), [](auto& icon, auto& page, GC::Ref<Fetch::Infrastructure::ResponseView>, Fetch::Infrastructure::FetchAlgorithms::BodyBytes bytes) {
        icon->m_fetch_controller = nullptr;
        // The bytes are here, so the wait is no longer on the network: Decoding them finishes on its own, however
        // long a busy machine takes over it.
        web_rust_notifications_api_icon_bytes_arrived(icon->m_notification.ptr());

        // 1. If bytes is null or failure, then return.
        // AD-HOC: The show steps wait on this fetch, so an absent resource is reported as such, rather than left
        //         for the timeout to notice.
        if (!bytes.has<Core::ImmutableBytes>()) {
            web_rust_notifications_api_icon_fetched(the_vm(), icon->m_notification.ptr());
            return;
        }

        // 2. Attempt to decode bytes as an image.
        // 3. If the image format is supported, then run processResource given the decoded image.
        // NB: The decoder answers from outside the heap's view, so the icon, which holds the notification, is rooted
        //     for the wait. An answer for a notification that's gone in the meantime is dropped.
        auto on_decoded = [icon = GC::Root(icon), page = GC::Root(page)](Platform::DecodedImage& decoded_image) -> ErrorOr<void> {
            // NB: Only the first frame is used, so an animation the decoder opened is stopped right away.
            if (decoded_image.session_id != 0)
                Platform::ImageCodecPlugin::the().stop_animation_decode(decoded_image.session_id);
            if (!web_rust_notifications_api_is_pending(icon->m_notification.ptr()))
                return {};
            auto* m_page = page.ptr();
            RefPtr<Gfx::Bitmap const> bitmap;
            if (!decoded_image.frames.is_empty() && decoded_image.frames[0].bitmap) {
                bitmap = decoded_image.frames[0].bitmap;
                auto size = bitmap->size();
                if (size.width() > icon_maximum_side || size.height() > icon_maximum_side) {
                    auto scale = static_cast<float>(icon_maximum_side) / static_cast<float>(max(size.width(), size.height()));
                    Gfx::IntSize scaled_size { max(1, static_cast<int>(roundf(size.width() * scale))), max(1, static_cast<int>(roundf(size.height() * scale))) };
                    // Without a compositor to scale with, the platform gets the icon as it came.
                    if (m_page && m_page->has_compositor_host()) {
                        auto output = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, scaled_size);
                        auto frame = Gfx::DecodedImageFrame::create(*bitmap);
                        if (!output.is_error() && !frame.is_error()) {
                            if (Painting::rasterize_image_frame(m_page->compositor_host(), frame.value(), Gfx::FloatRect { {}, scaled_size.to_type<float>() }, Gfx::ScalingMode::BilinearMipmap, output.value()))
                                bitmap = output.release_value();
                        }
                    }
                }
            }
            icon->m_resource = move(bitmap);
            web_rust_notifications_api_icon_fetched(the_vm(), icon->m_notification.ptr());
            return {};
        };
        auto on_decode_failed = [icon = GC::Root(icon)](Error&) {
            if (!web_rust_notifications_api_is_pending(icon->m_notification.ptr()))
                return;
            web_rust_notifications_api_icon_fetched(the_vm(), icon->m_notification.ptr());
        };
        (void)Platform::ImageCodecPlugin::the().decode_image(bytes.get<Core::ImmutableBytes>().bytes(), move(on_decoded), move(on_decode_failed));
    });
    icon->m_fetch_controller = Fetch::Fetching::fetch(settings->realm(), request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)), Fetch::Fetching::UseParallelQueue::Yes);
    return icon.ptr();
}

void web_notifications_api_stop_icon_fetch(NotificationIcon* icon)
{
    if (icon->m_fetch_controller)
        icon->m_fetch_controller->stop_fetch();
    icon->m_fetch_controller = nullptr;
}

void web_notifications_api_show_notification(HTML::Window* window, u64 notification_id, NotificationDataAbi const* data, NotificationIcon* icon)
{
    NotificationData notification_data {
        .title = Utf16String::from_raw(data->title),
        .body = Utf16String::from_raw(data->body),
        .tag = Utf16String::from_raw(data->tag),
        .language = Utf16String::from_raw(data->language),
        .direction = static_cast<Bindings::NotificationDirection>(data->direction),
        .silent = data->has_silent ? Optional<bool> { data->silent } : Optional<bool> {},
        .require_interaction = data->require_interaction,
        .renotify = data->renotify,
    };
    RefPtr<Gfx::Bitmap const> icon_resource = icon ? icon->m_resource : nullptr;
    window->page().client().page_did_request_show_notification(notification_id, window->associated_document().id(), notification_data, icon_resource);
}

void web_notifications_api_request_close_notification(HTML::Window* window, u64 notification_id)
{
    window->page().client().page_did_request_close_notification(notification_id);
}

void web_notifications_api_abandon_notification(HTML::Window* window, u64 notification_id)
{
    window->page().client().page_did_abandon_notification(notification_id);
}

void web_notifications_api_handle_notification_click(HTML::Window* window, u64 notification_id, bool into_focus)
{
    window->page().client().page_did_handle_notification_click(notification_id, into_focus);
}

}
