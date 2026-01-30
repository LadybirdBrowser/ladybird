/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/OhNoesNodePrototype.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// Debug-only helper node created via Internals.createOhNoesNode().
// Not exposed to normal JavaScript.

class OhNoesNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(OhNoesNode, AudioNode);
    GC_DECLARE_ALLOCATOR(OhNoesNode);

public:
    virtual ~OhNoesNode() override;

    static WebIDL::ExceptionOr<GC::Ref<OhNoesNode>> create_for_internals(JS::Realm&, GC::Ref<BaseAudioContext>, String path);

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    WebIDL::ExceptionOr<void> start();
    WebIDL::ExceptionOr<void> stop();
    WebIDL::ExceptionOr<void> set_path(String path);
    WebIDL::ExceptionOr<void> set_strip_zero_buffers(bool enabled);

    String const& base_path_for_rendering() const { return m_base_path; }
    bool emit_enabled_for_rendering() const { return m_emit_enabled; }
    bool strip_zero_buffers_for_rendering() const { return m_strip_zero_buffers; }

private:
    OhNoesNode(JS::Realm&, GC::Ref<BaseAudioContext>, String path);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    String m_base_path;
    bool m_emit_enabled { true };
    bool m_strip_zero_buffers { false };
};

}
