/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Bitmap.h>
#include <AK/Platform.h>
#include <AK/String.h>
#include <LibCore/GamepadFinder.h>
#include <LibCore/System.h>

#include <libudev.h>
#include <linux/input.h>

#if !defined(AK_OS_LINUX)
static_assert(false, "This file must only be used for Linux");
#endif

namespace Core {

static ErrorOr<bool> is_gamepad(StringView path)
{
    auto result_or_error = System::open(path, O_RDONLY);
    if (result_or_error.is_error()) {
        warnln("Failed to open input device {}: {}", path, result_or_error.release_error());
        return false;
    }

    int fd = result_or_error.release_value();

    Bitmap events = TRY(Bitmap::create(EV_MAX, false));
    Bitmap keys = TRY(Bitmap::create(KEY_MAX, false));

    if (System::ioctl(fd, EVIOCGBIT(0, events.size_in_bytes()), events.data()).is_error())
        return false;

    if (System::ioctl(fd, EVIOCGBIT(EV_KEY, keys.size_in_bytes()), keys.data()).is_error())
        return false;

    TRY(System::close(fd));

    if (events.get(EV_KEY) && keys.get(BTN_GAMEPAD))
        return true;

    return false;
}

ErrorOr<Vector<String>> find_all_connected_gamepads()
{
    Vector<String> gamepads;

    struct udev* udev = udev_new();
    if (!udev)
        return Error::from_string_literal("Failed to create udev object");

    struct udev_enumerate* enumerate = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerate, "input");

    udev_enumerate_scan_devices(enumerate);
    struct udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry* entry = nullptr;
    udev_list_entry_foreach(entry, devices)
    {
        char const* path = udev_list_entry_get_name(entry);
        udev_device* device = udev_device_new_from_syspath(udev, path);

        char const* device_path_c = udev_device_get_devnode(device);

        if (device_path_c) {
            StringView device_path = StringView(device_path_c, strlen(device_path_c));

            if (TRY(is_gamepad(device_path)))
                gamepads.append(TRY(String::from_utf8(device_path)));
        }

        udev_device_unref(device);
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udev);

    return gamepads;
}

}
