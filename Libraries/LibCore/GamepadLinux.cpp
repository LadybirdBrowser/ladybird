/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Bitmap.h>
#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/Platform.h>
#include <AK/QuickSort.h>
#include <AK/Tuple.h>
#include <LibCore/Gamepad.h>
#include <LibCore/System.h>

#include <linux/input.h>

#if !defined(AK_OS_LINUX)
static_assert(false, "This file must only be used for Linux");
#endif

namespace Core {

struct GamepadAxis {
    int minimum;
    int maximum;
    int value;
};

class GamepadImpl final : public Gamepad {
public:
    static ErrorOr<NonnullRefPtr<GamepadImpl>> create(StringView path)
    {
        int fd = TRY(System::open(path, O_RDONLY | O_NONBLOCK));

        char name[128];
        TRY(System::ioctl(fd, EVIOCGNAME(sizeof(name)), name));

        Bitmap keys = TRY(Bitmap::create(KEY_MAX, false));
        Bitmap absolute = TRY(Bitmap::create(ABS_MAX, false));

        HashMap<u16, GamepadAxis> axes;
        HashMap<u16, bool> buttons;

        TRY(System::ioctl(fd, EVIOCGBIT(EV_KEY, keys.size_in_bytes()), keys.data()));
        TRY(System::ioctl(fd, EVIOCGBIT(EV_ABS, absolute.size_in_bytes()), absolute.data()));

        for (int i = 0; i < KEY_MAX; ++i) {
            if (keys.get(i)) {
                buttons.set(to_standard_button(i), false);
            }
        }

        for (int i = 0; i < ABS_MISC; ++i) {
            if (absolute.get(i)) {
                if (i >= ABS_HAT0X && i <= ABS_HAT3Y) {
                    auto hat = hat_to_standard_buttons(i);
                    buttons.set(hat.get<0>(), false);
                    buttons.set(hat.get<1>(), false);
                } else {
                    struct input_absinfo info;
                    TRY(System::ioctl(fd, EVIOCGABS(i), &info));

                    axes.set(to_standard_axis(i), GamepadAxis { info.minimum, info.maximum, 0 });
                }
            }
        }

        return adopt_ref(*new GamepadImpl(fd, TRY(String::from_utf8(path)), TRY(String::from_utf8(StringView(name, strlen(name)))), sort_hashmap(axes), sort_hashmap(buttons)));
    }

    ~GamepadImpl()
    {
        MUST(System::close(m_fd));
    }

    virtual String const& path() const override { return m_path; }
    virtual String const& name() const override { return m_name; }

    virtual ErrorOr<Vector<double>> get_axes() override
    {
        Vector<double> axes;
        for (auto [axis, value] : m_axes)
            axes.append(AK::normalize_value_in_range(value.value, value.minimum, value.maximum));
        return axes;
    }

    virtual ErrorOr<Vector<bool>> get_buttons() override
    {
        return m_buttons.values();
    }

    virtual ErrorOr<bool> poll_all_events() override
    {
        struct input_event event;
        bool changed = false;

        while (true) {
            auto result = System::read(m_fd, Bytes(&event, sizeof(struct input_event)));
            if (result.is_error()) {
                if (result.error().code() == EAGAIN)
                    break;

                return result.release_error();
            } else if (result.release_value() < static_cast<ssize_t>(sizeof(struct input_event))) {
                break;
            }

            changed = true;

            if (event.type == EV_KEY) {
                VERIFY(m_buttons.contains(to_standard_button(event.code)));
                m_buttons.set(to_standard_button(event.code), static_cast<bool>(event.value));

            } else if (event.type == EV_ABS) {
                if (event.code >= ABS_HAT0X && event.code <= ABS_HAT3Y) {
                    auto hat = hat_to_standard_buttons(event.code);

                    VERIFY(m_buttons.contains(hat.get<0>()));
                    VERIFY(m_buttons.contains(hat.get<1>()));

                    m_buttons.set(hat.get<0>(), event.value == -1);
                    m_buttons.set(hat.get<1>(), event.value == 1);
                } else {
                    VERIFY(m_axes.contains(to_standard_axis(event.code)));
                    m_axes.get(to_standard_axis(event.code))->value = event.value;
                }
            }
        }

        return changed;
    }

private:
    explicit GamepadImpl(int fd, String path, String name, OrderedHashMap<u16, GamepadAxis> const& axes, OrderedHashMap<u16, bool> const& buttons)
        : m_fd(fd)
        , m_path(path)
        , m_name(name)
        , m_axes(axes)
        , m_buttons(buttons)
    {
    }

    static u16 to_standard_axis(u16 axis)
    {
        switch (axis) {
        case ABS_X:
            return 0;
        case ABS_Y:
            return 1;
        case ABS_RX:
            return 2;
        case ABS_RY:
            return 3;
        case ABS_Z:
            return 4;
        case ABS_RZ:
            return 5;
        default:
            return axis;
        }

        VERIFY_NOT_REACHED();
    }

    static u16 to_standard_button(u16 button)
    {
        switch (button) {
        case BTN_A:
            return 0;
        case BTN_B:
            return 1;
        case BTN_X:
            return 2;
        case BTN_Y:
            return 3;
        case BTN_TL:
            return 4;
        case BTN_TR:
            return 5;
        case BTN_SELECT:
            return 8;
        case BTN_START:
            return 9;
        case BTN_THUMBL:
            return 10;
        case BTN_THUMBR:
            return 11;
        case BTN_MODE:
            return 16;
        default:
            return button;
        }

        VERIFY_NOT_REACHED();
    }

    static Tuple<u16, u16> hat_to_standard_buttons(u16 axis)
    {
        switch (axis) {
        case ABS_HAT0X:
            return { 14, 15 };
        case ABS_HAT0Y:
            return { 12, 13 };
        default:
            return { axis, axis + 1 };
        }

        VERIFY_NOT_REACHED();
    }

    int m_fd;

    String m_path;
    String m_name;
    OrderedHashMap<u16, GamepadAxis> m_axes;
    OrderedHashMap<u16, bool> m_buttons;
};

ErrorOr<NonnullRefPtr<Gamepad>> Gamepad::create(StringView path)
{
    return GamepadImpl::create(path);
}

}
