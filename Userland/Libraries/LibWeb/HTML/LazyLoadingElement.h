/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/AttributeNames.h>

namespace Web::HTML {

// Lazy-loaded elements should invoke this macro to inject overridden LazyLoadingElement methods.
#define LAZY_LOADING_ELEMENT(ElementClass)                                                                     \
private:                                                                                                       \
    virtual JS::GCPtr<JS::HeapFunction<void()>> take_lazy_load_resumption_steps(Badge<DOM::Document>) override \
    {                                                                                                          \
        return take_lazy_load_resumption_steps_internal();                                                     \
    }                                                                                                          \
                                                                                                               \
    virtual bool is_lazy_loading() const override { return true; }

enum class LazyLoading {
    Lazy,
    Eager,
};

template<typename T>
class LazyLoadingElement {
public:
    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#lazy-loading-attributes
    [[nodiscard]] LazyLoading lazy_loading_attribute() const
    {
        auto& element = static_cast<T const&>(*this);

        auto value = element.attribute(HTML::AttributeNames::loading);
        if (value.has_value() && value->equals_ignoring_ascii_case("lazy"sv))
            return LazyLoading::Lazy;
        return LazyLoading::Eager;
    }

    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#will-lazy-load-element-steps
    [[nodiscard]] bool will_lazy_load_element() const
    {
        auto& element = static_cast<T const&>(*this);

        // 1. If scripting is disabled for element, then return false.
        // Spec Note: This is an anti-tracking measure, because if a user agent supported lazy loading when scripting is
        //            disabled, it would still be possible for a site to track a user's approximate scroll position throughout
        //            a session, by strategically placing images in a page's markup such that a server can track how many
        //            images are requested and when.
        if (element.is_scripting_disabled())
            return false;

        // 2. If element's lazy loading attribute is in the Lazy state, then return true.
        // 3. Return false.
        return lazy_loading_attribute() == LazyLoading::Lazy;
    }

    void set_lazy_load_resumption_steps(Function<void()> steps)
    {
        auto& element = static_cast<T&>(*this);

        m_lazy_load_resumption_steps = JS::create_heap_function(element.vm().heap(), move(steps));
    }

    void visit_lazy_loading_element(JS::Cell::Visitor& visitor)
    {
        visitor.visit(m_lazy_load_resumption_steps);
    }

protected:
    LazyLoadingElement() = default;
    virtual ~LazyLoadingElement() = default;

    JS::GCPtr<JS::HeapFunction<void()>> take_lazy_load_resumption_steps_internal()
    {
        auto lazy_load_resumption_steps = m_lazy_load_resumption_steps;
        m_lazy_load_resumption_steps = nullptr;
        return lazy_load_resumption_steps;
    }

private:
    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#lazy-load-resumption-steps
    // Each img and iframe element has associated lazy load resumption steps, initially null.
    JS::GCPtr<JS::HeapFunction<void()>> m_lazy_load_resumption_steps;
};

}
