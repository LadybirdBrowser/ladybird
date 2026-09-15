/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

}
