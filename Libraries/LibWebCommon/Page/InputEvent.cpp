/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/Page/InputEvent.h>

namespace Web {

PointerInputData::PointerInputData(Optional<PointerProperties> properties)
{
    if (properties.has_value())
        m_properties = make<PointerProperties>(*properties);
}

PointerInputData::PointerInputData(PointerInputData const& other)
    : PointerInputData(other.properties())
{
}

Optional<PointerProperties> PointerInputData::properties() const
{
    if (m_properties)
        return *m_properties;
    return {};
}

KeyEvent KeyEvent::clone_without_browser_data() const
{
    return { type, key, modifiers, code_point, repeat, should_insert_text, nullptr, async_scroll_performed_default_action, id, editing_commands, native_editing_binding_resolved };
}

MouseEvent MouseEvent::clone_without_browser_data() const
{
    return { type, position, screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y, wheel_tick_120ths_x, wheel_tick_120ths_y, wheel_delta_precision, scroll_gesture_phase, click_count, nullptr, async_scroll_performed_default_action, id, scrollbar_dragged_by_compositor, pointer_data };
}

}
