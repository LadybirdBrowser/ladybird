/*
 * Copyright (c) 2022, Ben Abraham <ben.d.abraham@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/AbstractWorker.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerAgent.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#define ENUMERATE_WORKER_EVENT_HANDLERS(E)  \
    E(onmessage, HTML::EventNames::message) \
    E(onmessageerror, HTML::EventNames::messageerror)

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/workers.html#dedicated-workers-and-the-worker-interface
class Worker
    : public DOM::EventTarget
    , public HTML::AbstractWorker {
    WEB_PLATFORM_OBJECT(Worker, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Worker);

public:
    static WebIDL::ExceptionOr<GC::Ref<Worker>> create(String const& script_url, WorkerOptions const& options, DOM::Document& document);
    static WebIDL::ExceptionOr<GC::Ref<Worker>> construct_impl(JS::Realm& realm, String const& script_url, WorkerOptions const& options)
    {
        auto& window = as<HTML::Window>(realm.global_object());
        return Worker::create(script_url, options, window.associated_document());
    }

    WebIDL::ExceptionOr<void> terminate();

    WebIDL::ExceptionOr<void> post_message(JS::Value message, StructuredSerializeOptions const&);
    WebIDL::ExceptionOr<void> post_message(JS::Value message, Vector<GC::Root<JS::Object>> const& transfer);

    virtual ~Worker() = default;

    GC::Ptr<MessagePort> outside_message_port() { return m_outside_port; }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_WORKER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

protected:
    Worker(String const&, WorkerOptions const&, DOM::Document&);

    // ^AbstractWorker
    virtual DOM::EventTarget& this_event_target() override { return *this; }

private:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    String m_script_url;
    WorkerOptions m_options;

    GC::Ptr<DOM::Document> m_document;
    GC::Ptr<MessagePort> m_outside_port;

    GC::Ptr<WorkerAgent> m_agent;

    void run_a_worker(URL::URL& url, EnvironmentSettingsObject& outside_settings, GC::Ptr<MessagePort> outside_port, WorkerOptions const& options);
};

}
