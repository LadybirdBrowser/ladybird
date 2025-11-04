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
    __ENUMERATE_INJECTION_SINKS(Element_##attribute_name, "Element " #attribute_name)

// https://w3c.github.io/trusted-types/dist/spec/#injection-sink
#define ENUMERATE_INJECTION_SINKS                                                                   \
    __ENUMERATE_INJECTION_SINKS(Document_parseHTMLUnsafe, "Document parseHTMLUnsafe")               \
    __ENUMERATE_INJECTION_SINKS(Document_write, "Document write")                                   \
    __ENUMERATE_INJECTION_SINKS(Document_writeln, "Document writeln")                               \
    __ENUMERATE_INJECTION_SINKS(Document_execCommand, "Document execCommand")                       \
    __ENUMERATE_INJECTION_SINKS(DOMParser_parseFromString, "DOMParser parseFromString")             \
    __ENUMERATE_INJECTION_SINKS(Element_innerHTML, "Element innerHTML")                             \
    __ENUMERATE_INJECTION_SINKS(Element_insertAdjacentHTML, "Element insertAdjacentHTML")           \
    __ENUMERATE_INJECTION_SINKS(Element_outerHTML, "Element outerHTML")                             \
    __ENUMERATE_INJECTION_SINKS(Element_setHTMLUnsafe, "Element setHTMLUnsafe")                     \
    __ENUMERATE_INJECTION_SINKS(Function, "Function")                                               \
    __ENUMERATE_INJECTION_SINKS(HTMLIFrameElement_srcdoc, "HTMLIFrameElement srcdoc")               \
    __ENUMERATE_INJECTION_SINKS(HTMLScriptElement_innerText, "HTMLScriptElement innerText")         \
    __ENUMERATE_INJECTION_SINKS(HTMLScriptElement_src, "HTMLScriptElement src")                     \
    __ENUMERATE_INJECTION_SINKS(HTMLScriptElement_text, "HTMLScriptElement text")                   \
    __ENUMERATE_INJECTION_SINKS(HTMLScriptElement_textContent, "HTMLScriptElement textContent")     \
    __ENUMERATE_INJECTION_SINKS(Location_href, "Location href")                                     \
    __ENUMERATE_INJECTION_SINKS(Range_createContextualFragment, "Range createContextualFragment")   \
    __ENUMERATE_INJECTION_SINKS(ServiceWorkerContainer_register, "ServiceWorkerContainer register") \
    __ENUMERATE_INJECTION_SINKS(ShadowRoot_innerHTML, "ShadowRoot innerHTML")                       \
    __ENUMERATE_INJECTION_SINKS(ShadowRoot_setHTMLUnsafe, "ShadowRoot setHTMLUnsafe")               \
    __ENUMERATE_INJECTION_SINKS(SharedWorker_constructor, "SharedWorker constructor")               \
    __ENUMERATE_INJECTION_SINKS(SVGScriptElement_href, "SVGScriptElement href")                     \
    __ENUMERATE_INJECTION_SINKS(Worker_constructor, "Worker constructor")                           \
    ENUMERATE_GLOBAL_EVENT_HANDLERS(EVENT_HANDLERS_INJECTION_SINKS)                                 \
    ENUMERATE_WINDOW_EVENT_HANDLERS(EVENT_HANDLERS_INJECTION_SINKS)

enum class InjectionSink {
#define __ENUMERATE_INJECTION_SINKS(name, value) name,
    ENUMERATE_INJECTION_SINKS
#undef __ENUMERATE_INJECTION_SINKS
};

Utf16String to_string(InjectionSink sink);

}
