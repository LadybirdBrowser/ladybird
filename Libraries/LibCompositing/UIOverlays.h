/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/PausedDebuggerOverlay.h>
#include <LibCompositing/PictureInPictureControls.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Page/InputEvent.h>

namespace Compositing {

struct PausedDebuggerOverlayState {
    Optional<String> font_family;
    Optional<PausedDebuggerOverlayAction> hovered_action;

    bool operator==(PausedDebuggerOverlayState const&) const = default;
};

// The controls stay present while they are hidden, so that they fade out.
struct PictureInPictureControlsState {
    bool shown { false };
    Optional<PictureInPictureControl> hovered_control;

    bool operator==(PictureInPictureControlsState const&) const = default;
};

// What the browser draws over a context's page, on top of anything the page draws, with each overlay the view has
// present. The browser handles their input before the page sees any of it.
struct UIOverlays {
    Optional<PausedDebuggerOverlayState> paused_debugger;
    Optional<PictureInPictureControlsState> picture_in_picture_controls;

    bool operator==(UIOverlays const&) const = default;
};

// Which of an overlay's buttons the pointer is over, and which it pressed. A press acts once it is released over that
// same button.
template<typename Action>
class OverlayPointerState {
public:
    struct Outcome {
        bool hover_changed { false };
        Optional<Action> activated_action;
    };

    Outcome handle_mouse_event(Web::MouseEvent const& event, Optional<Action> action_under_pointer)
    {
        auto previously_hovered_action = m_hovered_action;
        Optional<Action> activated_action;

        switch (event.type) {
        case Web::MouseEvent::Type::MouseMove:
            m_hovered_action = action_under_pointer;
            if ((event.buttons & Web::UIEvents::MouseButton::Primary) == Web::UIEvents::MouseButton::None)
                m_pressed_action.clear();
            break;
        case Web::MouseEvent::Type::MouseLeave:
            reset();
            break;
        case Web::MouseEvent::Type::MouseDown:
            if (event.button == Web::UIEvents::MouseButton::Primary)
                m_pressed_action = action_under_pointer;
            break;
        case Web::MouseEvent::Type::MouseUp:
            if (event.button == Web::UIEvents::MouseButton::Primary) {
                if (m_pressed_action == action_under_pointer)
                    activated_action = m_pressed_action;
                m_pressed_action.clear();
            }
            break;
        case Web::MouseEvent::Type::MouseCancel:
            m_pressed_action.clear();
            break;
        case Web::MouseEvent::Type::MouseWheel:
            break;
        }

        return { m_hovered_action != previously_hovered_action, activated_action };
    }

    void reset()
    {
        m_hovered_action.clear();
        m_pressed_action.clear();
    }

    Optional<Action> hovered_action() const { return m_hovered_action; }
    bool is_pressing() const { return m_pressed_action.has_value(); }

private:
    Optional<Action> m_hovered_action;
    Optional<Action> m_pressed_action;
};

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::PausedDebuggerOverlayState const&);
template<>
COMPOSITING_API ErrorOr<Compositing::PausedDebuggerOverlayState> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::PictureInPictureControlsState const&);
template<>
COMPOSITING_API ErrorOr<Compositing::PictureInPictureControlsState> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::UIOverlays const&);
template<>
COMPOSITING_API ErrorOr<Compositing::UIOverlays> decode(Decoder&);

}
