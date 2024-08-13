/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/StorageAPI/NavigatorStorage.h>
#include <LibWeb/StorageAPI/StorageManager.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#dom-navigatorstorage-storage
JS::NonnullGCPtr<StorageManager> NavigatorStorage::storage()
{
    // The storage getter steps are to return this’s relevant settings object’s StorageManager object.
    return HTML::relevant_settings_object(this_navigator_storage_object()).storage_manager();
}

}
