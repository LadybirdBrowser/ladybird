/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::StorageAPI {

class NavigatorStorage {
public:
    virtual ~NavigatorStorage() = default;

    GC::Ref<StorageManager> storage();

protected:
    virtual Bindings::PlatformObject const& this_navigator_storage_object() const = 0;
};

}
