/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/HostObjectABI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

// What a function of the C ABI that the steps call returns to them: the value, or the exception, for which a
// DOMException is created in the realm.
WEB_API JSCompletion completion_to_abi(JS::Realm&, WebIDL::ExceptionOr<JS::Value>);

}
