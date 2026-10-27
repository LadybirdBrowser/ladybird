/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/JSONObject.h>
#include <LibWeb/Bindings/NewGlobalObject.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::Bindings {

JS::Completion throw_copy_of_error_in_current_realm(JS::VM& vm, JS::Value error)
{
    auto& realm = *vm.current_realm();
    auto serialized_error = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] { return HTML::structured_serialize(vm, error); }));
    return JS::throw_completion(TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] { return HTML::structured_deserialize(vm, serialized_error, realm); })));
}

JS::ThrowCompletionOr<Utf16String> serialize_to_json_in_a_new_global_object(JS::VM& vm, Function<JS::Value(JS::Realm&)> const& create_value)
{
    auto json = TRY(run_in_a_new_global_object<Optional<Utf16String>>(vm, [&](JS::Realm& realm) {
        return JS::JSONObject::stringify_impl(vm, create_value(realm), JS::Value {}, JS::Value {});
    }));
    if (!json.has_value())
        return vm.throw_completion<JS::TypeError>("Result of stringifying value must not be undefined"_utf16);
    return json.release_value();
}

}
