/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/kmalloc.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/Timer.h>
#include <LibWebView/NotificationBackendFreedesktop.h>

#include <dbus/dbus.h>

namespace WebView {

static constexpr auto bus_name = "org.freedesktop.Notifications";
static constexpr auto object_path = "/org/freedesktop/Notifications";
static constexpr auto interface_name = "org.freedesktop.Notifications";

// Reasons the daemon gives in NotificationClosed.
static constexpr u32 closed_reason_dismissed_by_user = 2;

NotificationBackendFreedesktop::NotificationBackendFreedesktop()
{
    if (!connect())
        return;
    request_capabilities();
}

NotificationBackendFreedesktop::~NotificationBackendFreedesktop()
{
    if (!m_connection)
        return;
    dbus_connection_set_watch_functions(m_connection, nullptr, nullptr, nullptr, nullptr, nullptr);
    dbus_connection_set_timeout_functions(m_connection, nullptr, nullptr, nullptr, nullptr, nullptr);
    dbus_connection_close(m_connection);
    dbus_connection_unref(m_connection);
}

bool NotificationBackendFreedesktop::connect()
{
    DBusError error;
    dbus_error_init(&error);
    m_connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!m_connection) {
        dbgln("Notifications: No session bus to reach a notification daemon over: {}", error.message ? error.message : "unknown error");
        dbus_error_free(&error);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(m_connection, FALSE);

    dbus_connection_set_watch_functions(m_connection, add_watch, remove_watch, toggle_watch, this, nullptr);
    dbus_connection_set_timeout_functions(m_connection, add_timeout, remove_timeout, toggle_timeout, this, nullptr);
    dbus_connection_set_dispatch_status_function(
        m_connection, [](DBusConnection*, DBusDispatchStatus status, void* data) {
            if (status == DBUS_DISPATCH_DATA_REMAINS)
                Core::deferred_invoke([backend = static_cast<NotificationBackendFreedesktop*>(data)] { backend->dispatch(); });
        },
        this, nullptr);

    dbus_connection_add_filter(
        m_connection, [](DBusConnection*, DBusMessage* message, void* data) -> DBusHandlerResult {
            static_cast<NotificationBackendFreedesktop*>(data)->did_receive_signal(message);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        },
        this, nullptr);
    dbus_bus_add_match(m_connection, "type='signal',interface='org.freedesktop.Notifications'", nullptr);
    return true;
}

void NotificationBackendFreedesktop::dispatch()
{
    while (dbus_connection_dispatch(m_connection) == DBUS_DISPATCH_DATA_REMAINS) { }
}

unsigned NotificationBackendFreedesktop::add_watch(DBusWatch* watch, void* data)
{
    auto& backend = *static_cast<NotificationBackendFreedesktop*>(data);
    auto flags = dbus_watch_get_flags(watch);
    auto type = Core::Notifier::Type::None;
    if (flags & DBUS_WATCH_READABLE)
        type |= Core::Notifier::Type::Read;
    if (flags & DBUS_WATCH_WRITABLE)
        type |= Core::Notifier::Type::Write;
    auto notifier = Core::Notifier::construct(dbus_watch_get_unix_fd(watch), type);
    notifier->on_activation = [watch, &backend, flags] {
        dbus_watch_handle(watch, flags & (DBUS_WATCH_READABLE | DBUS_WATCH_WRITABLE));
        backend.dispatch();
    };
    notifier->set_enabled(dbus_watch_get_enabled(watch));
    backend.m_watches.append({ watch, move(notifier) });
    return TRUE;
}

void NotificationBackendFreedesktop::remove_watch(DBusWatch* watch, void* data)
{
    auto& backend = *static_cast<NotificationBackendFreedesktop*>(data);
    backend.m_watches.remove_all_matching([&](auto const& entry) { return entry.watch == watch; });
}

void NotificationBackendFreedesktop::toggle_watch(DBusWatch* watch, void* data)
{
    auto& backend = *static_cast<NotificationBackendFreedesktop*>(data);
    for (auto& entry : backend.m_watches) {
        if (entry.watch == watch)
            entry.notifier->set_enabled(dbus_watch_get_enabled(watch));
    }
}

unsigned NotificationBackendFreedesktop::add_timeout(DBusTimeout* timeout, void* data)
{
    auto& backend = *static_cast<NotificationBackendFreedesktop*>(data);
    auto timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(dbus_timeout_get_interval(timeout)), [timeout, &backend] {
        dbus_timeout_handle(timeout);
        backend.dispatch();
    });
    if (dbus_timeout_get_enabled(timeout))
        timer->start();
    backend.m_timeouts.append({ timeout, move(timer) });
    return TRUE;
}

void NotificationBackendFreedesktop::remove_timeout(DBusTimeout* timeout, void* data)
{
    auto& backend = *static_cast<NotificationBackendFreedesktop*>(data);
    backend.m_timeouts.remove_all_matching([&](auto const& entry) { return entry.timeout == timeout; });
}

void NotificationBackendFreedesktop::toggle_timeout(DBusTimeout* timeout, void* data)
{
    auto& backend = *static_cast<NotificationBackendFreedesktop*>(data);
    for (auto& entry : backend.m_timeouts) {
        if (entry.timeout != timeout)
            continue;
        if (dbus_timeout_get_enabled(timeout))
            entry.timer->start(AK::Duration::from_milliseconds(dbus_timeout_get_interval(timeout)));
        else
            entry.timer->stop();
    }
}

void NotificationBackendFreedesktop::request_capabilities()
{
    m_capabilities_requested = true;
    auto* message = dbus_message_new_method_call(bus_name, object_path, interface_name, "GetCapabilities");
    DBusPendingCall* pending = nullptr;
    if (!dbus_connection_send_with_reply(m_connection, message, &pending, -1) || !pending) {
        dbus_message_unref(message);
        m_capabilities = FreedesktopCapabilities {};
        return;
    }
    dbus_message_unref(message);
    dbus_pending_call_set_notify(
        pending, [](DBusPendingCall* pending, void* data) {
            auto* reply = dbus_pending_call_steal_reply(pending);
            static_cast<NotificationBackendFreedesktop*>(data)->did_receive_capabilities(reply);
            if (reply)
                dbus_message_unref(reply);
        },
        this, nullptr);
    dbus_pending_call_unref(pending);
}

void NotificationBackendFreedesktop::did_receive_capabilities(DBusMessage* reply)
{
    FreedesktopCapabilities capabilities;
    DBusMessageIter iterator;
    if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN && dbus_message_iter_init(reply, &iterator) && dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_ARRAY) {
        DBusMessageIter entries;
        dbus_message_iter_recurse(&iterator, &entries);
        while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_STRING) {
            char const* capability = nullptr;
            dbus_message_iter_get_basic(&entries, &capability);
            StringView name { capability, strlen(capability) };
            if (name == "body"sv)
                capabilities.body = true;
            else if (name == "body-markup"sv)
                capabilities.body_markup = true;
            else if (name == "persistence"sv)
                capabilities.persistence = true;
            else if (name == "actions"sv)
                capabilities.actions = true;
            dbus_message_iter_next(&entries);
        }
    }
    m_capabilities = capabilities;
    for (auto& notification : exchange(m_waiting_for_capabilities, {}))
        send_notify(notification);
}

void NotificationBackendFreedesktop::show(PlatformNotification const& notification)
{
    if (!m_connection) {
        if (on_failed)
            on_failed(notification.id);
        return;
    }
    if (!m_capabilities.has_value()) {
        m_waiting_for_capabilities.append(notification);
        return;
    }
    send_notify(notification);
}

static void append_string(DBusMessageIter& iterator, String const& string)
{
    auto bytes = string.to_byte_string();
    char const* characters = bytes.characters();
    dbus_message_iter_append_basic(&iterator, DBUS_TYPE_STRING, &characters);
}

static void append_hint(DBusMessageIter& hints, FreedesktopHint const& hint)
{
    DBusMessageIter entry;
    dbus_message_iter_open_container(&hints, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    append_string(entry, hint.name);

    DBusMessageIter variant;
    hint.value.visit(
        [&](u8 value) {
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, DBUS_TYPE_BYTE_AS_STRING, &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &value);
        },
        [&](bool value) {
            dbus_bool_t dbus_value = value;
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &dbus_value);
        },
        [&](String const& value) {
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, DBUS_TYPE_STRING_AS_STRING, &variant);
            append_string(variant, value);
        },
        [&](FreedesktopImageData const& image) {
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "(iiibiiay)", &variant);
            DBusMessageIter structure;
            dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, nullptr, &structure);
            dbus_message_iter_append_basic(&structure, DBUS_TYPE_INT32, &image.width);
            dbus_message_iter_append_basic(&structure, DBUS_TYPE_INT32, &image.height);
            dbus_message_iter_append_basic(&structure, DBUS_TYPE_INT32, &image.rowstride);
            dbus_bool_t has_alpha = image.has_alpha;
            dbus_message_iter_append_basic(&structure, DBUS_TYPE_BOOLEAN, &has_alpha);
            dbus_message_iter_append_basic(&structure, DBUS_TYPE_INT32, &image.bits_per_sample);
            dbus_message_iter_append_basic(&structure, DBUS_TYPE_INT32, &image.channels);
            DBusMessageIter bytes;
            dbus_message_iter_open_container(&structure, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &bytes);
            auto const* pixels = image.pixels.data();
            int count = static_cast<int>(image.pixels.size());
            dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, &pixels, count);
            dbus_message_iter_close_container(&structure, &bytes);
            dbus_message_iter_close_container(&variant, &structure);
        });
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&hints, &entry);
}

void NotificationBackendFreedesktop::send_notify(PlatformNotification const& notification)
{
    u32 replaces_daemon_id = 0;
    if (notification.replaces.has_value()) {
        if (auto daemon_id = m_daemon_id_for.take(*notification.replaces); daemon_id.has_value()) {
            replaces_daemon_id = *daemon_id;
            m_id_for_daemon_id.remove(replaces_daemon_id);
        }
    }
    auto arguments = build_notify_arguments(notification, *m_capabilities, replaces_daemon_id);

    auto* message = dbus_message_new_method_call(bus_name, object_path, interface_name, "Notify");
    DBusMessageIter iterator;
    dbus_message_iter_init_append(message, &iterator);
    append_string(iterator, arguments.app_name);
    dbus_message_iter_append_basic(&iterator, DBUS_TYPE_UINT32, &arguments.replaces_id);
    append_string(iterator, arguments.app_icon);
    append_string(iterator, arguments.summary);
    append_string(iterator, arguments.body);

    DBusMessageIter actions;
    dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &actions);
    for (auto const& action : arguments.actions)
        append_string(actions, action);
    dbus_message_iter_close_container(&iterator, &actions);

    DBusMessageIter hints;
    dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY, "{sv}", &hints);
    for (auto const& hint : arguments.hints)
        append_hint(hints, hint);
    dbus_message_iter_close_container(&iterator, &hints);

    dbus_message_iter_append_basic(&iterator, DBUS_TYPE_INT32, &arguments.expire_timeout);

    DBusPendingCall* pending = nullptr;
    if (!dbus_connection_send_with_reply(m_connection, message, &pending, -1) || !pending) {
        dbus_message_unref(message);
        if (on_failed)
            on_failed(notification.id);
        return;
    }
    dbus_message_unref(message);

    struct Context {
        AK_ALLOC_WITH_KMALLOC;
        NotificationBackendFreedesktop* backend;
        u64 id;
    };
    dbus_pending_call_set_notify(
        pending, [](DBusPendingCall* pending, void* data) {
            auto* context = static_cast<Context*>(data);
            auto* reply = dbus_pending_call_steal_reply(pending);
            context->backend->did_receive_notify_reply(context->id, reply);
            if (reply)
                dbus_message_unref(reply); },
        new Context { this, notification.id }, [](void* data) { delete static_cast<Context*>(data); });
    dbus_pending_call_unref(pending);
}

void NotificationBackendFreedesktop::did_receive_notify_reply(u64 id, DBusMessage* reply)
{
    u32 daemon_id = 0;
    if (!reply || dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN || !dbus_message_get_args(reply, nullptr, DBUS_TYPE_UINT32, &daemon_id, DBUS_TYPE_INVALID)) {
        if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR)
            dbgln("Notifications: The notification daemon refused the request: {}", dbus_message_get_error_name(reply));
        if (on_failed)
            on_failed(id);
        return;
    }
    m_daemon_id_for.set(id, daemon_id);
    m_id_for_daemon_id.set(daemon_id, id);
    if (on_shown)
        on_shown(id);
}

void NotificationBackendFreedesktop::close(u64 id)
{
    auto daemon_id = m_daemon_id_for.take(id);
    if (!daemon_id.has_value())
        return;
    m_id_for_daemon_id.remove(*daemon_id);
    auto* message = dbus_message_new_method_call(bus_name, object_path, interface_name, "CloseNotification");
    dbus_message_append_args(message, DBUS_TYPE_UINT32, &*daemon_id, DBUS_TYPE_INVALID);
    dbus_message_set_no_reply(message, TRUE);
    dbus_connection_send(m_connection, message, nullptr);
    dbus_message_unref(message);
    // The daemon's NotificationClosed for this id finds no mapping any more, so the close is reported once, here.
    if (on_closed)
        on_closed(id, ByUser::No);
}

void NotificationBackendFreedesktop::did_receive_signal(DBusMessage* message)
{
    if (dbus_message_is_signal(message, interface_name, "ActionInvoked")) {
        u32 daemon_id = 0;
        char const* action = nullptr;
        if (!dbus_message_get_args(message, nullptr, DBUS_TYPE_UINT32, &daemon_id, DBUS_TYPE_STRING, &action, DBUS_TYPE_INVALID))
            return;
        if (auto id = m_id_for_daemon_id.get(daemon_id); id.has_value() && on_clicked)
            on_clicked(*id);
        return;
    }

    if (dbus_message_is_signal(message, interface_name, "NotificationClosed")) {
        u32 daemon_id = 0;
        u32 reason = 0;
        if (!dbus_message_get_args(message, nullptr, DBUS_TYPE_UINT32, &daemon_id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID))
            return;
        auto id = m_id_for_daemon_id.take(daemon_id);
        if (!id.has_value())
            return;
        m_daemon_id_for.remove(*id);
        if (on_closed)
            on_closed(*id, reason == closed_reason_dismissed_by_user ? ByUser::Yes : ByUser::No);
    }
}

}
