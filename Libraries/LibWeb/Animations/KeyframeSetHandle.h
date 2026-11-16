/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AnyOf.h>
#include <AK/BinarySearch.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/Animations/RustFFI.h>
#include <LibWeb/CSS/PropertyNameAndID.h>

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#target-property
// The properties the keyframes of a set animate, borrowed from the set for as long as what lent it holds it.
class TargetProperties {
public:
    TargetProperties() = default;
    explicit TargetProperties(RustFFI::FfiKeyframeSetFacts const& facts)
        : m_properties(facts.target_properties, facts.target_property_count)
        , m_custom_properties(facts.target_custom_properties, facts.target_custom_property_count)
    {
    }

    bool is_empty() const { return size() == 0; }
    size_t size() const { return m_properties.size() + m_custom_properties.size(); }

    bool contains(CSS::PropertyNameAndID const& property) const
    {
        if (!property.is_custom_property())
            return binary_search(m_properties, static_cast<u16>(property.id())) != nullptr;
        return any_of(m_custom_properties, [&](size_t name) { return Utf16FlyString::from_raw(name) == property.name(); });
    }

    class Iterator {
    public:
        CSS::PropertyNameAndID operator*() const
        {
            if (m_index < m_properties->m_properties.size())
                return CSS::PropertyNameAndID::from_id(static_cast<CSS::PropertyID>(m_properties->m_properties[m_index]));
            return CSS::PropertyNameAndID::from_name(Utf16FlyString::from_raw(m_properties->m_custom_properties[m_index - m_properties->m_properties.size()])).release_value();
        }

        Iterator& operator++()
        {
            ++m_index;
            return *this;
        }

        bool operator==(Iterator const& other) const { return m_index == other.m_index; }

    private:
        friend class TargetProperties;

        Iterator(TargetProperties const& properties, size_t index)
            : m_properties(&properties)
            , m_index(index)
        {
        }

        TargetProperties const* m_properties { nullptr };
        size_t m_index { 0 };
    };

    Iterator begin() const { return { *this, 0 }; }
    Iterator end() const { return { *this, size() }; }

private:
    ReadonlySpan<u16> m_properties;
    ReadonlySpan<size_t> m_custom_properties;
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

    size_t keyframe_count() const { return RustFFI::animations_keyframe_set_facts(m_set).keyframe_count; }
    RustFFI::FfiKeyframeValue value(size_t index, u16 property_id) const { return RustFFI::animations_keyframe_set_value(m_set, index, property_id); }
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
