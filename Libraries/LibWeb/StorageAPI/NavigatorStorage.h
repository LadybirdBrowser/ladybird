/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::StorageAPI {

class NavigatorStorage {
public:
    virtual ~NavigatorStorage() = default;

    GC::Ref<StorageManager> storage();

protected:
    virtual HTML::EnvironmentSettingsObject& navigator_storage_settings_object() const = 0;
};

}
