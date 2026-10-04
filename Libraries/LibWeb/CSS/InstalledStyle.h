/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/StyleEngineRustFFI.h>

namespace Web::CSS {

// The style record an element or a pseudo-element installed, with the view of it the host reads. The element holds the
// record in the style engine for as long as it has it installed, and a record's view is written once and replaced,
// never changed, so reading the installed style asks nothing of the engine, which may be in a frame in flight.
class InstalledStyle {
public:
    InstalledStyle() = default;
    InstalledStyle(StyleRecordID record, StyleEngineFFI::FfiStyleRecordView const& view)
        : m_record(record)
        , m_view(view)
    {
    }

    StyleRecordID record() const { return m_record; }
    StyleEngineFFI::FfiStyleRecordView const& view() const { return m_view; }
    explicit operator bool() const { return m_view.present; }

    // The record's group payloads, one per style group, or null where nothing is installed.
    void const* payloads() const { return m_view.present ? m_view.payloads : nullptr; }

    // Whether the record's own display, beneath any animation, is none, read straight out of its box group payload.
    bool display_is_none() const;

    // Whether `animated_overlay` changes any effective value of the record in place of its own overlay, read from the
    // record itself.
    bool animation_overlay_changed(void const* animated_overlay) const
    {
        VERIFY(m_view.present);
        return StyleEngineFFI::style_engine_animation_overlay_changed(&m_view, animated_overlay);
    }

private:
    StyleRecordID m_record;
    StyleEngineFFI::FfiStyleRecordView m_view {};
};

}
