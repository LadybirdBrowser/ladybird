/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibJS/Runtime/Error.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StorageManager.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/StorageAPI/StorageManager.h>
#include <LibWeb/StorageAPI/StorageType.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(StorageManager);

WebIDL::ExceptionOr<GC::Ref<StorageManager>> StorageManager::create(JS::Realm& realm)
{
    return realm.create<StorageManager>(realm);
}

StorageManager::StorageManager(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void StorageManager::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StorageManager);
    Base::initialize(realm);
}

// https://storage.spec.whatwg.org/#dom-storagemanager-estimate
GC::Ref<WebIDL::Promise> StorageManager::estimate()
{
    auto& realm = this->realm();

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Let global be this's relevant global object.
    auto& global = HTML::relevant_global_object(*this);

    // 3. Let shelf be the result of running obtain a local storage shelf with this's relevant settings object.
    // AD-HOC: We don't keep a single in-process local storage shelf. localStorage lives in StorageJar, and
    //         sessionStorage lives on the traversable navigable — so rather than a shelf, we obtain the origin's
    //         storage key here, and read usage from those stores below. Obtaining a storage key fails for an opaque
    //         origin — which stands in for the spec's failure case.
    auto& environment = HTML::relevant_settings_object(*this);
    auto storage_key = obtain_a_storage_key(environment);

    // 4. If shelf is failure, then reject promise with a TypeError.
    if (!storage_key.has_value()) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Unable to obtain a storage shelf for this origin"_utf16));
        return promise;
    }

    // 5. Otherwise, run these steps in parallel:
    // AD-HOC: Our implementation of storage access is synchronous (localStorage usage is read over a sync IPC, and
    //         sessionStorage is in-process). So, rather than running in parallel and queuing a storage task to settle
    //         the promise, we compute the values, and settle the promise synchronously below.

    //    1. Let usage be storage usage for shelf.
    //       NB: The spec's "storage usage" is an implementation-defined rough estimate of the bytes used. We report the
    //       summed byte size of the origin's localStorage and sessionStorage entries.
    u64 usage = 0;
    if (auto* window = as_if<HTML::Window>(global)) {
        usage += window->page().client().page_did_request_storage_usage(StorageEndpointType::LocalStorage, storage_key->to_string());

        if (auto session_bottle = obtain_a_storage_bottle_map(StorageType::Session, environment, StorageEndpointType::SessionStorage)) {
            for (auto const& bottle_key : session_bottle->keys()) {
                usage += bottle_key.bytes().size();
                if (auto value = session_bottle->get(bottle_key); value.has_value())
                    usage += value->bytes().size();
            }
        }
    }

    //    2. Let quota be storage quota for shelf.
    //       NB: The spec's "storage quota" is an implementation-defined conservative estimate of the total bytes it can
    //            hold. We report the sum of the per-origin budgets we enforce for the tracked endpoints.
    auto quota = StorageEndpoint::LOCAL_STORAGE_QUOTA + StorageEndpoint::SESSION_STORAGE_QUOTA;

    //    3. Let dictionary be a new StorageEstimate dictionary whose usage member is usage and quota member is quota.
    auto dictionary = JS::Object::create(realm, realm.intrinsics().object_prototype());
    MUST(dictionary->create_data_property("usage"_utf16_fly_string, JS::Value(static_cast<double>(usage))));
    MUST(dictionary->create_data_property("quota"_utf16_fly_string, JS::Value(static_cast<double>(quota))));

    //    4. If there was an internal error while obtaining usage and quota, then queue a storage task with global to
    //       reject promise with a TypeError.
    //    AD-HOC: There's no recoverable internal-error path here; a disconnected storage IPC terminates the process.

    //    5. Otherwise, queue a storage task with global to resolve promise with dictionary.
    WebIDL::resolve_promise(realm, promise, dictionary);

    // 6. Return promise.
    return promise;
}

}
