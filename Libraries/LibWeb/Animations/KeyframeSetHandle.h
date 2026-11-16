/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BinarySearch.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/Animations/RustFFI.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#target-property
// The properties the keyframes of a set animate, borrowed from the set for as long as what lent it holds it.
class TargetProperties {
public:
    TargetProperties() = default;
    explicit TargetProperties(RustFFI::FfiKeyframeSetFacts const& facts)
        : m_properties(facts.target_properties, facts.target_property_count)
    {
    }

    bool contains(CSS::PropertyID property_id) const
    {
        return binary_search(m_properties, static_cast<u16>(property_id)) != nullptr;
    }

private:
    ReadonlySpan<u16> m_properties;
};

// A borrowed view of an immutable keyframe set, valid for as long as what lent it holds the set.
class KeyframeSetView {
public:
    KeyframeSetView() = default;
    explicit KeyframeSetView(RustFFI::KeyframeSet const* set)
        : m_set(set)
    {
    }

    RustFFI::KeyframeSet const* ptr() const { return m_set; }
    explicit operator bool() const { return m_set; }
    bool operator==(KeyframeSetView const& other) const { return m_set == other.m_set; }

    RustFFI::FfiKeyframeSetFacts facts() const { return RustFFI::animations_keyframe_set_facts(m_set); }
    TargetProperties target_properties() const { return m_set ? TargetProperties { facts() } : TargetProperties {}; }

protected:
    RustFFI::KeyframeSet const* m_set { nullptr };
};

// A reference to an immutable keyframe set, which Rust owns and shares between everything that runs it.
class KeyframeSetHandle : public KeyframeSetView {
public:
    KeyframeSetHandle() = default;

    // Takes over the one reference a keyframe set builder handed out.
    static KeyframeSetHandle adopt(RustFFI::KeyframeSet const* set)
    {
        KeyframeSetHandle handle;
        handle.m_set = set;
        return handle;
    }

    // Takes a reference of its own to a set something else holds one to.
    static KeyframeSetHandle retained(RustFFI::KeyframeSet const* set)
    {
        if (set)
            RustFFI::animations_keyframe_set_retain(set);
        return adopt(set);
    }

    KeyframeSetHandle(KeyframeSetHandle const& other)
        : KeyframeSetHandle(retained(other.m_set))
    {
    }

    KeyframeSetHandle(KeyframeSetHandle&& other)
        : KeyframeSetView(exchange(other.m_set, nullptr))
    {
    }

    KeyframeSetHandle& operator=(KeyframeSetHandle const& other)
    {
        if (this != &other)
            *this = retained(other.m_set);
        return *this;
    }

    KeyframeSetHandle& operator=(KeyframeSetHandle&& other)
    {
        if (this != &other) {
            release();
            m_set = exchange(other.m_set, nullptr);
        }
        return *this;
    }

    ~KeyframeSetHandle() { release(); }

private:
    void release()
    {
        if (auto const* set = exchange(m_set, nullptr))
            RustFFI::animations_keyframe_set_release(set);
    }
};

}
