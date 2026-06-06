/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#the-closewatcher-interface
class CloseWatcher final : public DOM::EventTarget {
    WEB_WRAPPABLE(CloseWatcher, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(CloseWatcher);

public:
    using GetEnabledState = GC::Ref<GC::Function<bool()>>;

    static WebIDL::ExceptionOr<GC::Ref<CloseWatcher>> construct_impl(Window&, Bindings::CloseWatcherOptions const&);
    [[nodiscard]] static GC::Ref<CloseWatcher> establish(HTML::Window&, GetEnabledState);

    void request_close_for_bindings();
    void close();
    void destroy();

    bool request_close(bool require_history_action_activation);

    bool get_enabled_state() const;

    virtual ~CloseWatcher() override = default;

    void set_oncancel(WebIDL::CallbackType*);
    WebIDL::CallbackType* oncancel();

    void set_onclose(WebIDL::CallbackType*);
    WebIDL::CallbackType* onclose();

private:
    CloseWatcher(Window&, GetEnabledState);
    virtual void visit_edges(Visitor&) override;

    bool m_is_running_cancel_action { false };
    bool m_is_active { true };
    GetEnabledState m_get_enabled_state;
    GC::Ref<Window> m_window;
};

}
