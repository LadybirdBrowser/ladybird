/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Fetch {

GC::Ref<WebIDL::Promise> fetch(JS::VM&, RequestInfo const& input, RequestInit const& init = {});
void abort_fetch(JS::Realm&, WebIDL::Promise const&, GC::Ref<Infrastructure::Request>, GC::Ptr<Response>, JS::Value error);

}
