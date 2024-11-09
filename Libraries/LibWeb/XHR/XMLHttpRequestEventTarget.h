/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::XHR {

#define ENUMERATE_XML_HTTP_REQUEST_EVENT_TARGET_EVENT_HANDLERS(E) \
    E(onloadstart, XHR::EventNames::loadstart)                    \
    E(onprogress, XHR::EventNames::progress)                      \
    E(onabort, XHR::EventNames::abort)                            \
    E(onerror, XHR::EventNames::error)                            \
    E(onload, XHR::EventNames::load)                              \
    E(ontimeout, XHR::EventNames::timeout)                        \
    E(onloadend, XHR::EventNames::loadend)

class XMLHttpRequestEventTarget : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(XMLHttpRequestEventTarget, DOM::EventTarget);

public:
    virtual ~XMLHttpRequestEventTarget() override {};

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_XML_HTTP_REQUEST_EVENT_TARGET_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

protected:
    XMLHttpRequestEventTarget(JS::Realm& realm)
        : DOM::EventTarget(realm)
    {
    }
};

}
