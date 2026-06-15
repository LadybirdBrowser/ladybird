/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/DecodedImageData.h>

namespace Web::HTML {

class AnimatedDecodedImageData : public DecodedImageData {
    GC_CELL(AnimatedDecodedImageData, DecodedImageData);
    GC_DECLARE_ALLOCATOR(AnimatedDecodedImageData);

public:
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void restart_animation() override;

protected:
    AnimatedDecodedImageData(GC::Ref<DOM::DocumentObserver>);

    virtual void start_animation() = 0;
    virtual void reset_animation() = 0;
    virtual void stop_animation() = 0;

    virtual void on_client_registered() override;

private:
    void start_animation_if_needed();

    GC::Ref<DOM::DocumentObserver> m_document_observer;
    bool m_should_start_animation_on_client_registration { true };
};

}
