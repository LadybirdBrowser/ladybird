/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/DataTransferPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/DataTransfer.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(DataTransfer);

namespace DataTransferEffect {

#define __ENUMERATE_DATA_TRANSFER_EFFECT(name) FlyString name = #name##_fly_string;
ENUMERATE_DATA_TRANSFER_EFFECTS
#undef __ENUMERATE_DATA_TRANSFER_EFFECT

}

JS::NonnullGCPtr<DataTransfer> DataTransfer::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<DataTransfer>(realm, realm);
}

DataTransfer::DataTransfer(JS::Realm& realm)
    : PlatformObject(realm)
{
}

DataTransfer::~DataTransfer() = default;

void DataTransfer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DataTransfer);
}

void DataTransfer::set_drop_effect(String const& drop_effect)
{
    set_drop_effect(FlyString { drop_effect });
}

void DataTransfer::set_drop_effect(FlyString drop_effect)
{
    using namespace DataTransferEffect;

    // On setting, if the new value is one of "none", "copy", "link", or "move", then the attribute's current value must
    // be set to the new value. Other values must be ignored.
    if (drop_effect.is_one_of(none, copy, link, move))
        m_drop_effect = AK::move(drop_effect);
}

void DataTransfer::set_effect_allowed(String const& effect_allowed)
{
    set_effect_allowed(FlyString { effect_allowed });
}

void DataTransfer::set_effect_allowed(FlyString effect_allowed)
{
    // On setting, if drag data store's mode is the read/write mode and the new value is one of "none", "copy", "copyLink",
    // "copyMove", "link", "linkMove", "move", "all", or "uninitialized", then the attribute's current value must be set
    // to the new value. Otherwise, it must be left unchanged.
    if (m_associated_drag_data_store.has_value() && m_associated_drag_data_store->mode() == DragDataStore::Mode::ReadWrite)
        set_effect_allowed_internal(move(effect_allowed));
}

void DataTransfer::set_effect_allowed_internal(FlyString effect_allowed)
{
    // AD-HOC: We need to be able to set the effectAllowed attribute internally regardless of the state of the drag data store.
    using namespace DataTransferEffect;

    if (effect_allowed.is_one_of(none, copy, copyLink, copyMove, link, linkMove, move, all, uninitialized))
        m_effect_allowed = AK::move(effect_allowed);
}

}
