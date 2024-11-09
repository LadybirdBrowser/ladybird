/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#closewatcheroptions
struct CloseWatcherOptions {
    JS::GCPtr<DOM::AbortSignal> signal;
};

// https://html.spec.whatwg.org/multipage/interaction.html#the-closewatcher-interface
class CloseWatcher final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(CloseWatcher, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(CloseWatcher);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<CloseWatcher>> construct_impl(JS::Realm&, CloseWatcherOptions const& = {});
    [[nodiscard]] static JS::NonnullGCPtr<CloseWatcher> establish(HTML::Window&);

    bool request_close();
    void close();
    void destroy();

    virtual ~CloseWatcher() override = default;

    void set_oncancel(WebIDL::CallbackType*);
    WebIDL::CallbackType* oncancel();

    void set_onclose(WebIDL::CallbackType*);
    WebIDL::CallbackType* onclose();

private:
    CloseWatcher(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    bool m_is_running_cancel_action { false };
    bool m_is_active { true };
};

}
