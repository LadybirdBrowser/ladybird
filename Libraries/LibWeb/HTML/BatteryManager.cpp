/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/Bindings/BatteryManager.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/BatteryManager.h>
#include <LibWeb/HTML/EventNames.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(BatteryManager);

GC::Ref<BatteryManager> BatteryManager::create(JS::Realm& realm)
{
    return realm.create<BatteryManager>(realm);
}

BatteryManager::BatteryManager(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void BatteryManager::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BatteryManager);
    Base::initialize(realm);
}

double BatteryManager::discharging_time() const
{
    return AK::Infinity<double>;
}

void BatteryManager::set_onchargingchange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::chargingchange, event_handler);
}

GC::Ptr<WebIDL::CallbackType> BatteryManager::onchargingchange()
{
    return event_handler_attribute(EventNames::chargingchange);
}

void BatteryManager::set_onchargingtimechange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::chargingtimechange, event_handler);
}

GC::Ptr<WebIDL::CallbackType> BatteryManager::onchargingtimechange()
{
    return event_handler_attribute(EventNames::chargingtimechange);
}

void BatteryManager::set_ondischargingtimechange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::dischargingtimechange, event_handler);
}

GC::Ptr<WebIDL::CallbackType> BatteryManager::ondischargingtimechange()
{
    return event_handler_attribute(EventNames::dischargingtimechange);
}

void BatteryManager::set_onlevelchange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::levelchange, event_handler);
}

GC::Ptr<WebIDL::CallbackType> BatteryManager::onlevelchange()
{
    return event_handler_attribute(EventNames::levelchange);
}

}
