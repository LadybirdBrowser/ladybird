/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/Forward.h>

#define ENUMERATE_WINDOW_EVENT_HANDLERS(E)                        \
    E(onafterprint, HTML::EventNames::afterprint)                 \
    E(onbeforeprint, HTML::EventNames::beforeprint)               \
    E(onbeforeunload, HTML::EventNames::beforeunload)             \
    E(onhashchange, HTML::EventNames::hashchange)                 \
    E(onlanguagechange, HTML::EventNames::languagechange)         \
    E(onmessage, HTML::EventNames::message)                       \
    E(onmessageerror, HTML::EventNames::messageerror)             \
    E(onoffline, HTML::EventNames::offline)                       \
    E(ononline, HTML::EventNames::online)                         \
    E(onpagehide, HTML::EventNames::pagehide)                     \
    E(onpageshow, HTML::EventNames::pageshow)                     \
    E(onpopstate, HTML::EventNames::popstate)                     \
    E(onrejectionhandled, HTML::EventNames::rejectionhandled)     \
    E(onstorage, HTML::EventNames::storage)                       \
    E(onunhandledrejection, HTML::EventNames::unhandledrejection) \
    E(onunload, HTML::EventNames::unload)

namespace Web::HTML {

class WindowEventHandlers {
public:
    virtual ~WindowEventHandlers();

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

protected:
    virtual JS::GCPtr<DOM::EventTarget> window_event_handlers_to_event_target() = 0;
};

}
