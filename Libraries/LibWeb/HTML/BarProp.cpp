/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/BarPropPrototype.h>
#include <LibWeb/HTML/BarProp.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(BarProp);

GC::Ref<BarProp> BarProp::create(JS::Realm& realm)
{
    return realm.create<BarProp>(realm);
}

BarProp::BarProp(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-barprop-visible
bool BarProp::visible() const
{
    // 1. Let browsingContext be this's relevant global object's browsing context.
    auto& global_object = HTML::relevant_global_object(*this);
    auto* browsing_context = as<HTML::Window>(global_object).associated_document().browsing_context();

    // 2. If browsingContext is null, then return true.
    if (!browsing_context) {
        return true;
    }

    // 3. Return the negation of browsingContext's top-level browsing context's is popup.
    return browsing_context->top_level_browsing_context()->is_popup() != TokenizedFeature::Popup::Yes;
}

void BarProp::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BarProp);
}
}
