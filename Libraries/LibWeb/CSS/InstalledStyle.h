/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/StyleEngineRustFFI.h>

namespace Web::CSS {

enum class StyleRecordDependencyFlag : u8;

// The computed style an element or a pseudo-element installed: a counted reference to the record the engine installed
// for it, or to the sample of its animations over that record. Records never change, so reading the installed style
// reads the record and asks nothing of the engine, which may be in a frame in flight.
class InstalledStyle {
public:
    InstalledStyle() = default;
    // Takes a reference to `record`, a record the engine handed out, or none for null.
    explicit InstalledStyle(void const* record)
        : m_record(record)
    {
        if (m_record)
            StyleEngineFFI::style_engine_record_retain(m_record);
    }
    // Takes over a reference to `record` the engine handed out, or none for null.
    static InstalledStyle adopt(void const* record)
    {
        InstalledStyle installed;
        installed.m_record = record;
        return installed;
    }
    InstalledStyle(InstalledStyle const& other)
        : InstalledStyle(other.m_record)
    {
    }
    InstalledStyle(InstalledStyle&& other)
        : m_record(exchange(other.m_record, nullptr))
    {
    }
    InstalledStyle& operator=(InstalledStyle const& other)
    {
        if (this != &other) {
            InstalledStyle copy { other };
            swap(m_record, copy.m_record);
        }
        return *this;
    }
    InstalledStyle& operator=(InstalledStyle&& other)
    {
        if (this != &other) {
            InstalledStyle moved { move(other) };
            swap(m_record, moved.m_record);
        }
        return *this;
    }
    ~InstalledStyle()
    {
        if (m_record)
            StyleEngineFFI::style_engine_record_release(m_record);
    }

    StyleRecordID record() const { return m_record ? StyleRecordID { StyleEngineFFI::style_engine_record_identity(m_record) } : StyleRecordID {}; }
    StyleEngineFFI::FfiStyleRecordView view() const { return m_record ? StyleEngineFFI::style_engine_record_view(m_record) : StyleEngineFFI::FfiStyleRecordView {}; }
    explicit operator bool() const { return m_record; }
    // Whether both hold the same record, which no other record shares the identity of.
    bool operator==(InstalledStyle const& other) const { return m_record == other.m_record; }

    // What the record's values depend on, or nothing where nothing is installed.
    StyleRecordDependencyFlag dependency_flags() const { return static_cast<StyleRecordDependencyFlag>(view().dependency_flags); }

    // The identity of the custom-property environment the record was resolved in, or 0.
    u64 custom_property_environment() const { return m_record ? StyleEngineFFI::style_engine_record_custom_property_environment(m_record) : 0; }

    // The record itself, as the engine's entries that read a record the host holds take it, or null.
    void const* record_pointer() const { return m_record; }

    // The record's groups, which `CSS::style_record_group` reads, or null where nothing is installed. NB: A record's
    // groups are at its head.
    void const* groups() const { return m_record; }

    // Whether the record's own display, beneath any animation, is none, read straight out of its box group payload.
    bool display_is_none() const;
    bool display_is_none_ignoring_animations() const;

    // Whether `animated_overlay` changes any effective value of the record in place of its own overlay, read from the
    // record itself.
    bool animation_overlay_changed(void const* animated_overlay) const
    {
        VERIFY(m_record);
        auto view = this->view();
        return StyleEngineFFI::style_engine_animation_overlay_changed(&view, animated_overlay);
    }

private:
    void const* m_record { nullptr };
};

// NB: Every element and pseudo-element the host styles holds one of these.
static_assert(sizeof(InstalledStyle) == sizeof(void*));

}
