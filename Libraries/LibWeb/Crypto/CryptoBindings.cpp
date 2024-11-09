/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Crypto/CryptoBindings.h>

namespace Web::Bindings {

JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Object>> JsonWebKey::to_object(JS::Realm& realm)
{
    auto& vm = realm.vm();
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());

    if (kty.has_value())
        TRY(object->create_data_property("kty", JS::PrimitiveString::create(vm, kty.value())));

    if (use.has_value())
        TRY(object->create_data_property("use", JS::PrimitiveString::create(vm, use.value())));

    if (key_ops.has_value()) {
        auto key_ops_array = JS::Array::create_from<String>(realm, key_ops.value().span(), [&](auto& key_usage) -> JS::Value {
            return JS::PrimitiveString::create(realm.vm(), key_usage);
        });
        TRY(object->create_data_property("key_ops", move(key_ops_array)));
    }

    if (alg.has_value())
        TRY(object->create_data_property("alg", JS::PrimitiveString::create(vm, alg.value())));

    if (ext.has_value())
        TRY(object->create_data_property("ext", JS::Value(ext.value())));

    if (crv.has_value())
        TRY(object->create_data_property("crv", JS::PrimitiveString::create(vm, crv.value())));

    if (x.has_value())
        TRY(object->create_data_property("x", JS::PrimitiveString::create(vm, x.value())));

    if (y.has_value())
        TRY(object->create_data_property("y", JS::PrimitiveString::create(vm, y.value())));

    if (d.has_value())
        TRY(object->create_data_property("d", JS::PrimitiveString::create(vm, d.value())));

    if (n.has_value())
        TRY(object->create_data_property("n", JS::PrimitiveString::create(vm, n.value())));

    if (e.has_value())
        TRY(object->create_data_property("e", JS::PrimitiveString::create(vm, e.value())));

    if (p.has_value())
        TRY(object->create_data_property("p", JS::PrimitiveString::create(vm, p.value())));

    if (q.has_value())
        TRY(object->create_data_property("q", JS::PrimitiveString::create(vm, q.value())));

    if (dp.has_value())
        TRY(object->create_data_property("dp", JS::PrimitiveString::create(vm, dp.value())));

    if (dq.has_value())
        TRY(object->create_data_property("dq", JS::PrimitiveString::create(vm, dq.value())));

    if (qi.has_value())
        TRY(object->create_data_property("qi", JS::PrimitiveString::create(vm, qi.value())));

    if (oth.has_value()) {
        TODO();
    }

    if (k.has_value())
        TRY(object->create_data_property("k", JS::PrimitiveString::create(vm, k.value())));

    return object;
}

}
