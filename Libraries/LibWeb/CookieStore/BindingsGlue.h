/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>

namespace Web::CookieStore {

class CookieChangeEvent;

}

namespace Web::Bindings {

class WrapperWorld;

WEB_API GC::Ptr<JS::Object> cached_changed(CookieStore::CookieChangeEvent const&, WrapperWorld const&);
WEB_API void set_cached_changed(CookieStore::CookieChangeEvent const&, WrapperWorld const&, GC::Ptr<JS::Object>);
WEB_API GC::Ptr<JS::Object> cached_deleted(CookieStore::CookieChangeEvent const&, WrapperWorld const&);
WEB_API void set_cached_deleted(CookieStore::CookieChangeEvent const&, WrapperWorld const&, GC::Ptr<JS::Object>);

}
