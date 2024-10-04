/*
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/HTML/Window.h>

namespace WebContent {

class ConsoleGlobalEnvironmentExtensions final : public JS::Object {
    JS_OBJECT(ConsoleGlobalEnvironmentExtensions, JS::Object);
    JS_DECLARE_ALLOCATOR(ConsoleGlobalEnvironmentExtensions);

public:
    ConsoleGlobalEnvironmentExtensions(JS::Realm&, Web::HTML::Window&);
    virtual void initialize(JS::Realm&) override;
    virtual ~ConsoleGlobalEnvironmentExtensions() override = default;

    void set_most_recent_result(JS::Value result) { m_most_recent_result = move(result); }

private:
    virtual void visit_edges(Visitor&) override;

    // $0, the DOM node currently selected in the inspector
    JS_DECLARE_NATIVE_FUNCTION($0_getter);
    // $_, the value of the most recent expression entered into the console
    JS_DECLARE_NATIVE_FUNCTION($__getter);
    // $(selector, element), equivalent to `(element || document).querySelector(selector)`
    JS_DECLARE_NATIVE_FUNCTION($_function);
    // $$(selector, element), equivalent to `(element || document).querySelectorAll(selector)`
    JS_DECLARE_NATIVE_FUNCTION($$_function);

    JS::NonnullGCPtr<Web::HTML::Window> m_window_object;
    JS::Value m_most_recent_result { JS::js_undefined() };
};

}
