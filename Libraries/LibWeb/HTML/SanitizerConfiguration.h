/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Variant.h>
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

}

namespace Web::HTML {

class WEB_API SanitizerConfiguration {
public:
    explicit SanitizerConfiguration(void const*);
    SanitizerConfiguration(SanitizerConfiguration const&);
    SanitizerConfiguration(SanitizerConfiguration&&);
    SanitizerConfiguration& operator=(SanitizerConfiguration const&);
    SanitizerConfiguration& operator=(SanitizerConfiguration&&);
    ~SanitizerConfiguration();

    void const* rust_configuration() const;

private:
    void const* m_configuration { nullptr };
};

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-action
// NB: SanitizerAction in sanitizer_configuration.rs matches this.
enum class SanitizerAction : u8 {
    Keep,
    Remove,
    ReplaceWithChildren,
};

WEB_API bool sanitizer_config_allows_comments(SanitizerConfiguration const&);
WEB_API bool sanitizer_config_allows_processing_instruction_target(Utf16View const& target, SanitizerConfiguration const&);
WEB_API SanitizerAction sanitizer_action_for_element_name(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_, SanitizerConfiguration const&);
WEB_API bool sanitizer_config_allows_attribute(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_, Utf16View const& value, Utf16FlyString const& element_local_name, Optional<Utf16FlyString> const& element_namespace, SanitizerConfiguration const&);

enum class SanitizerSafety : u8 {
    Unsafe,
    Safe,
};

using SanitizerOptions = Variant<Bindings::SetHTMLOptions const*, Bindings::SetHTMLUnsafeOptions const*, Bindings::ParseHTMLUnsafeOptions const*, GC::Ref<TrustedTypes::TrustedHTMLParserOptions>>;

WEB_API WebIDL::ExceptionOr<Optional<SanitizerConfiguration>> get_a_sanitizer_config_from_options(SanitizerOptions const&, SanitizerSafety);

using SanitizerInput = Variant<GC::Ref<Sanitizer>, Bindings::SanitizerConfig, Bindings::SanitizerPresets>;
WebIDL::ExceptionOr<GC::Ref<Sanitizer>> get_a_sanitizer_from_sanitizer_input(SanitizerInput const&);

}
