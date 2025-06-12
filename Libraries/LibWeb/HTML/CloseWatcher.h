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
    GC::Ptr<DOM::AbortSignal> signal;
};

// https://html.spec.whatwg.org/multipage/interaction.html#the-closewatcher-interface
class CloseWatcher final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(CloseWatcher, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(CloseWatcher);

public:
    static WebIDL::ExceptionOr<GC::Ref<CloseWatcher>> construct_impl(JS::Realm&, CloseWatcherOptions const& = {});
    [[nodiscard]] static GC::Ref<CloseWatcher> establish(HTML::Window&);

    void request_close_for_bindings();
    void close();
    void destroy();

    bool request_close(bool require_history_action_activation);

    bool get_enabled_state() const { return m_is_enabled; }
    void set_enabled(bool enabled) { m_is_enabled = enabled; }

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
    bool m_is_enabled { true };
};

}
