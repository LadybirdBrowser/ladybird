/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/SanitizerConfiguration.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

enum class SanitizerPresets : u8;
struct SanitizerElementNamespace;
struct SanitizerElementNamespaceWithAttributes;
struct SanitizerAttributeNamespace;
struct SanitizerProcessingInstruction;

}

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer
class Sanitizer final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Sanitizer, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Sanitizer);

public:
    static WebIDL::ExceptionOr<GC::Ref<Sanitizer>> create(Variant<Bindings::SanitizerConfig, Bindings::SanitizerPresets> const& configuration);
    static GC::Ref<Sanitizer> create_with_configuration(SanitizerConfiguration);

    virtual ~Sanitizer() override;

    Bindings::SanitizerConfig get() const;

    bool allow_element(Variant<Utf16String, Bindings::SanitizerElementNamespaceWithAttributes> const&);
    bool remove_element(Variant<Utf16String, Bindings::SanitizerElementNamespace> const&);
    bool replace_element_with_children(Variant<Utf16String, Bindings::SanitizerElementNamespace> const&);
    bool allow_processing_instruction(Variant<Utf16String, Bindings::SanitizerProcessingInstruction> const&);
    bool remove_processing_instruction(Variant<Utf16String, Bindings::SanitizerProcessingInstruction> const&);
    bool allow_attribute(Variant<Utf16String, Bindings::SanitizerAttributeNamespace> const&);
    bool remove_attribute(Variant<Utf16String, Bindings::SanitizerAttributeNamespace> const&);
    bool set_comments(bool allow);
    bool set_data_attributes(bool allow);
    bool set_javascript_urls(bool allow);
    bool remove_unsafe();

    SanitizerConfiguration const& configuration() const { return m_configuration; }

private:
    explicit Sanitizer(SanitizerConfiguration);

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-configuration
    SanitizerConfiguration m_configuration;
};

}
