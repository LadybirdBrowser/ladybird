/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AnimatedDecodedImageData.h"
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>

namespace Web::HTML {

void AnimatedDecodedImageData::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document_observer);
}

AnimatedDecodedImageData::AnimatedDecodedImageData(GC::Ref<DOM::DocumentObserver> document_observer)
    : m_document_observer(document_observer)
{
    auto weak_this = GC::Weak { *this };

    // OPTIMIZATION: To avoid CPU churn in background tabs we cancel the animation when the document is inactive or
    //               hidden. Other browsers disagree on what should happen when the document becomes active again,
    //               Blink continues the animation from where it would be if it had been running the whole time, and
    //               Gecko restarts the animation. For now we restart the animation since that's simpler.
    m_document_observer->set_document_became_inactive([weak_this] {
        if (auto self = weak_this.ptr())
            self->stop_animation();
    });

    m_document_observer->set_document_became_active([weak_this] {
        if (auto self = weak_this.ptr()) {
            if (!self->has_clients())
                self->m_should_start_animation_on_client_registration = true;

            self->restart_animation();
        }
    });

    m_document_observer->set_document_visibility_state_observer([weak_this](HTML::VisibilityState visibility_state) {
        if (auto self = weak_this.ptr()) {
            switch (visibility_state) {
            case HTML::VisibilityState::Hidden:
                self->stop_animation();
                break;
            case HTML::VisibilityState::Visible:
                if (!self->has_clients())
                    self->m_should_start_animation_on_client_registration = true;
                self->restart_animation();
                break;
            }
        }
    });
}

void AnimatedDecodedImageData::restart_animation()
{
    stop_animation();
    reset_animation();
    start_animation_if_needed();
}

void AnimatedDecodedImageData::on_client_registered()
{
    if (!m_should_start_animation_on_client_registration)
        return;

    m_should_start_animation_on_client_registration = false;

    start_animation_if_needed();
}

void AnimatedDecodedImageData::start_animation_if_needed()
{
    // NB: Animations should start when the first client is registered while the document is active and visible.
    if (!has_clients() || !m_document_observer->document()->is_fully_active() || m_document_observer->document()->hidden())
        return;

    start_animation();
}

}
