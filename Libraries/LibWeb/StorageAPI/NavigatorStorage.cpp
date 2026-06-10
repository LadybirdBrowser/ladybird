/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/StorageAPI/NavigatorStorage.h>
#include <LibWeb/StorageAPI/StorageManager.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#dom-navigatorstorage-storage
GC::Ref<StorageManager> NavigatorStorage::storage()
{
    // The storage getter steps are to return this’s relevant settings object’s StorageManager object.
    return navigator_storage_settings_object().storage_manager();
}

}
