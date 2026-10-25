/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

enum class SanitizerPresets : u8;
struct SanitizerConfig;
struct SetHTMLOptions;
struct SetHTMLUnsafeOptions;
struct ParseHTMLUnsafeOptions;
struct SanitizerElementNamespace;
struct SanitizerElementNamespaceWithAttributes;
struct SanitizerAttributeNamespace;
struct SanitizerProcessingInstruction;

}

namespace Web::HTML {

// NB: A canonicalized SanitizerElementNamespace or SanitizerAttributeNamespace dictionary.
struct SanitizerName {
    Utf16FlyString name;
    Optional<Utf16FlyString> namespace_;

    bool operator==(SanitizerName const&) const = default;
};

// NB: A canonicalized SanitizerElementNamespaceWithAttributes dictionary.
struct SanitizerElementWithAttributes {
    SanitizerName element;
    Optional<Vector<SanitizerName>> attributes;
    Optional<Vector<SanitizerName>> remove_attributes;

    bool operator==(SanitizerElementWithAttributes const&) const = default;
};

// NB: A canonicalized SanitizerConfig dictionary. Processing instructions are represented by their target.
struct SanitizerConfiguration {
    Optional<Vector<SanitizerElementWithAttributes>> elements;
    Optional<Vector<SanitizerName>> remove_elements;
    Optional<Vector<SanitizerName>> replace_with_children_elements;
    Optional<Vector<Utf16String>> processing_instructions;
    Optional<Vector<Utf16String>> remove_processing_instructions;
    Optional<Vector<SanitizerName>> attributes;
    Optional<Vector<SanitizerName>> remove_attributes;
    Optional<bool> comments;
    Optional<bool> data_attributes;
    Optional<bool> javascript_urls;
};

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-action
enum class SanitizerAction : u8 {
    Keep,
    Remove,
    ReplaceWithChildren,
};

enum class PermissiveDefaults : u8 {
    No,
    Yes,
};

SanitizerConfiguration canonicalize_sanitizer_config(Bindings::SanitizerConfig const&);
SanitizerName canonicalize_sanitizer_element(Variant<Utf16String, Bindings::SanitizerElementNamespace> const&);
SanitizerElementWithAttributes canonicalize_sanitizer_element_with_attributes(Variant<Utf16String, Bindings::SanitizerElementNamespaceWithAttributes> const&);
SanitizerName canonicalize_sanitizer_attribute(Variant<Utf16String, Bindings::SanitizerAttributeNamespace> const&);
Utf16String canonicalize_sanitizer_processing_instruction(Variant<Utf16String, Bindings::SanitizerProcessingInstruction> const&);
void canonicalize_the_configuration(SanitizerConfiguration&, PermissiveDefaults);
bool sanitizer_configuration_is_valid(SanitizerConfiguration const&);
WebIDL::ExceptionOr<SanitizerConfiguration> configure_sanitizer_configuration(SanitizerConfiguration, PermissiveDefaults);
Bindings::SanitizerConfig sanitizer_configuration_to_bindings(SanitizerConfiguration const&);

bool remove_an_element_from_sanitizer_config(SanitizerConfiguration&, SanitizerName const&);
bool remove_an_attribute_from_sanitizer_config(SanitizerConfiguration&, SanitizerName const&);
bool remove_unsafe_from_sanitizer_config(SanitizerConfiguration&);

bool is_custom_data_attribute(SanitizerName const&);
bool sanitizer_list_contains(Vector<SanitizerName> const&, SanitizerName const&);
bool sanitizer_list_remove(Optional<Vector<SanitizerName>>&, SanitizerName const&);

WEB_API bool sanitizer_config_allows_comments(SanitizerConfiguration const&);
WEB_API bool sanitizer_config_allows_processing_instruction_target(Utf16View const& target, SanitizerConfiguration const&);
WEB_API SanitizerAction sanitizer_action_for_element_name(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_, SanitizerConfiguration const&);
WEB_API bool sanitizer_config_allows_attribute(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_, Utf16View const& value, Utf16FlyString const& element_local_name, Optional<Utf16FlyString> const& element_namespace, SanitizerConfiguration const&);

SanitizerConfiguration const& built_in_safe_default_configuration();
SanitizerConfiguration const& built_in_safe_baseline_configuration();
bool built_in_non_replaceable_elements_list_contains(SanitizerName const&);

enum class SanitizerSafety : u8 {
    Unsafe,
    Safe,
};

using SanitizerOptions = Variant<Bindings::SetHTMLOptions const*, Bindings::SetHTMLUnsafeOptions const*, Bindings::ParseHTMLUnsafeOptions const*, GC::Ref<TrustedTypes::TrustedHTMLParserOptions>>;

WEB_API WebIDL::ExceptionOr<Optional<SanitizerConfiguration>> get_a_sanitizer_config_from_options(SanitizerOptions const&, SanitizerSafety);

using SanitizerInput = Variant<GC::Ref<Sanitizer>, Bindings::SanitizerConfig, Bindings::SanitizerPresets>;
WebIDL::ExceptionOr<GC::Ref<Sanitizer>> get_a_sanitizer_from_sanitizer_input(SanitizerInput const&);

}
