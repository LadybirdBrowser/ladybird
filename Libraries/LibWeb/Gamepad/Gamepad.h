/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibCore/Gamepad.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/GamepadPrototype.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Gamepad/GamepadButton.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Gamepad {

class Gamepad final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Gamepad, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Gamepad);

public:
    static WebIDL::ExceptionOr<GC::Ref<Gamepad>> create(JS::Realm&, NonnullRefPtr<Core::Gamepad> gamepad);
    virtual ~Gamepad() override;

    String id() const { return m_id; }
    void set_id(String id) { m_id = id; }

    int index() const { return m_index; }
    void set_index(int index) { m_index = index; }

    bool connected() const { return m_connected; }
    void set_connected(bool connected) { m_connected = connected; }

    HighResolutionTime::DOMHighResTimeStamp timestamp() const { return m_timestamp; }
    void set_timestamp(HighResolutionTime::DOMHighResTimeStamp timestamp) { m_timestamp = timestamp; }

    Bindings::GamepadMappingType mapping()
    {
        // TODO: If the gamepad contains unrecognized axes, return Empty
        return Bindings::GamepadMappingType::Standard;
    }

    WebIDL::ExceptionOr<Vector<double>> const axes();
    WebIDL::ExceptionOr<Vector<GC::Ref<GamepadButton>>> const buttons();

    NonnullRefPtr<Core::Gamepad> gamepad() const { return m_gamepad; }

    bool exposed() const { return m_exposed; }
    void set_exposed(bool exposed) { m_exposed = exposed; }

private:
    Gamepad(JS::Realm&, NonnullRefPtr<Core::Gamepad> gamepad);

    virtual void initialize(JS::Realm&) override;

    void update_gamepad_state();

    NonnullRefPtr<Core::Gamepad> m_gamepad;
    RefPtr<Core::Timer> m_poll_timer;

    String m_id;
    int m_index;
    bool m_connected;
    HighResolutionTime::DOMHighResTimeStamp m_timestamp;
    bool m_exposed;
};

}
