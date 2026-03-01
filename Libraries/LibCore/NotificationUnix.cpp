/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/String.h>
#include <LibCore/Notification.h>
#include <LibCore/System.h>
#include <dbus/dbus.h>

namespace Core {

class PlatformNotificationImpl final : public PlatformNotification {
public:
    static ErrorOr<NonnullOwnPtr<PlatformNotificationImpl>> create()
    {
        DBusError error;
        dbus_error_init(&error);

        DBusConnection* connection = dbus_bus_get(DBUS_BUS_SESSION, &error);

        if (dbus_error_is_set(&error)) {
            dbus_error_free(&error);
            return Error::from_string_literal("Failed to connect to DBus session bus");
        }

        return adopt_own(*new PlatformNotificationImpl(connection));
    }

    ErrorOr<void> show_notification(String const& title) override
    {
        // Create notification message
        DBusMessage* message = dbus_message_new_method_call(
            m_destination,
            m_path,
            m_interface,
            "Notify" // method
        );

        if (!message) {
            return Error::from_string_literal("Failed to create D-Bus notification message");
        }
        // Add arguments
        char const* app_name = "Ladybird";
        dbus_uint32_t replaces_id = 0;
        char const* icon = "";
        auto title_bytes = title.to_byte_string();
        char const* summary = title_bytes.characters();
        char const* body_text = "";
        dbus_int32_t timeout = 5000;

        DBusMessageIter args;
        dbus_message_iter_init_append(message, &args);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &app_name);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &replaces_id);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &icon);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &summary);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &body_text);

        // Empty actions array
        DBusMessageIter actions;
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &actions);
        dbus_message_iter_close_container(&args, &actions);

        // Empty hints dict
        DBusMessageIter hints;
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &hints);
        dbus_message_iter_close_container(&args, &hints);

        dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &timeout);

        // Send message
        dbus_connection_send(m_connection, message, nullptr);
        dbus_connection_flush(m_connection);

        // Cleanup
        dbus_message_unref(message);
        return {};
    }

private:
    explicit PlatformNotificationImpl(DBusConnection* connection)
        : m_connection(connection)
        , m_destination("org.freedesktop.Notifications")
        , m_path("/org/freedesktop/Notifications")
        , m_interface("org.freedesktop.Notifications")
    {
    }

    DBusConnection* m_connection { nullptr };

    char const* m_destination;
    char const* m_path;
    char const* m_interface;
};

ErrorOr<NonnullOwnPtr<PlatformNotification>> PlatformNotification::create()
{
    return PlatformNotificationImpl::create();
}

}
