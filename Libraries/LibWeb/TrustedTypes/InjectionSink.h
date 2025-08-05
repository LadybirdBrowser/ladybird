/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/WindowEventHandlers.h>

namespace Web::TrustedTypes {

#define EVENT_HANDLERS_INJECTION_SINKS(attribute_name, event_name) \
    __ENUMERATE_INJECTION_SINKS(Element##attribute_name, "Element " #attribute_name)

// https://w3c.github.io/trusted-types/dist/spec/#injection-sink
#define ENUMERATE_INJECTION_SINKS                                                    \
    __ENUMERATE_INJECTION_SINKS(Documentwrite, "Document write")                     \
    __ENUMERATE_INJECTION_SINKS(Documentwriteln, "Document writeln")                 \
    __ENUMERATE_INJECTION_SINKS(Function, "Function")                                \
    __ENUMERATE_INJECTION_SINKS(HTMLIFrameElementsrcdoc, "HTMLIFrameElement srcdoc") \
    __ENUMERATE_INJECTION_SINKS(HTMLScriptElementsrc, "HTMLScriptElement src")       \
    __ENUMERATE_INJECTION_SINKS(HTMLScriptElementtext, "HTMLScriptElement text")     \
    __ENUMERATE_INJECTION_SINKS(Locationhref, "Location href")                       \
    __ENUMERATE_INJECTION_SINKS(SVGScriptElementhref, "SVGScriptElement href")       \
    ENUMERATE_GLOBAL_EVENT_HANDLERS(EVENT_HANDLERS_INJECTION_SINKS)                  \
    ENUMERATE_WINDOW_EVENT_HANDLERS(EVENT_HANDLERS_INJECTION_SINKS)

enum class InjectionSink {
#define __ENUMERATE_INJECTION_SINKS(name, value) name,
    ENUMERATE_INJECTION_SINKS
#undef __ENUMERATE_INJECTION_SINKS
};

Utf16String to_string(InjectionSink sink);

}
