/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://w3c.github.io/battery/#the-batterymanager-interface
class WEB_API BatteryManager final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(BatteryManager, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(BatteryManager);

public:
    static GC::Ref<BatteryManager> create(JS::Realm&);

    bool charging() const { return true; }
    double charging_time() const { return 0; }
    double discharging_time() const;
    double level() const { return 1.0; }

    void set_onchargingchange(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onchargingchange();

    void set_onchargingtimechange(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onchargingtimechange();

    void set_ondischargingtimechange(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> ondischargingtimechange();

    void set_onlevelchange(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onlevelchange();

private:
    explicit BatteryManager(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
