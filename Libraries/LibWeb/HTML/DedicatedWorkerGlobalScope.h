/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DedicatedWorkerGlobalScope.h>
#include <LibWeb/Bindings/WorkerGlobalScope.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::HTML {

class WEB_API DedicatedWorkerGlobalScope
    : public WorkerGlobalScope
    , public Bindings::DedicatedWorkerGlobalScopeGlobalMixin {
    WEB_PLATFORM_OBJECT(DedicatedWorkerGlobalScope, WorkerGlobalScope);
    GC_DECLARE_ALLOCATOR(DedicatedWorkerGlobalScope);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~DedicatedWorkerGlobalScope() override;

    WebIDL::ExceptionOr<void> post_message(JS::Value message, Bindings::StructuredSerializeOptions const&);
    WebIDL::ExceptionOr<void> post_message(JS::Value message, GC::RootVector<GC::Ref<JS::Object>> const& transfer);

    void close();

    WebIDL::ExceptionOr<WebIDL::UnsignedLong> request_animation_frame(GC::Ref<WebIDL::CallbackType>);
    WebIDL::ExceptionOr<void> cancel_animation_frame(WebIDL::UnsignedLong handle);

    // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#concept-AnimationFrameProvider-supported
    bool is_supported() const;

    // Requests a near-term "update the rendering of that dedicated worker" pass:
    // run pending animation frame callbacks and commit OffscreenCanvas frames.
    void schedule_rendering_update();

    WebIDL::CallbackType* onmessage();
    void set_onmessage(WebIDL::CallbackType* callback);

    WebIDL::CallbackType* onmessageerror();
    void set_onmessageerror(WebIDL::CallbackType* callback);

    virtual void finalize() override;

private:
    DedicatedWorkerGlobalScope(JS::Realm&, GC::Ref<Web::Page>);

    virtual void initialize_web_interfaces_impl() override;
    virtual void visit_edges(Cell::Visitor&) override;

    AnimationFrameCallbackDriver& animation_frame_callback_driver();
    bool has_animation_frame_callbacks() const;
    void run_rendering_update();

    GC::Ptr<AnimationFrameCallbackDriver> m_animation_frame_callback_driver;
    GC::Ptr<Platform::Timer> m_rendering_update_timer;
};

}
