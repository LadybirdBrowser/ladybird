/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Request.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Bindings {

WEB_API WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> fetch(JS::Realm&, HTML::WindowOrWorkerGlobalScopeMixin&, Fetch::RequestInfo const&, RequestInit const&);

}
