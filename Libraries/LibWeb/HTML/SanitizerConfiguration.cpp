/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Sanitizer.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/Sanitizer.h>
#include <LibWeb/HTML/SanitizerConfiguration.h>
#include <LibWeb/HTML/WindowEventHandlers.h>
#include <LibWeb/TrustedTypes/TrustedHTMLParserOptions.h>

namespace Web::HTML {

// NB: These match the enums of the same names in sanitizer_configuration.rs.
enum class SanitizerInputKind : u8 {
    None,
    DefaultPreset,
    Config,
    Sanitizer,
    TrustedOptions,
};

enum class SanitizerConfigurationStatus : u8 {
    Null,
    Configuration,
    Invalid,
};

enum class SanitizerOptionalBoolean : u8 {
    Absent,
    False,
    True,
};

struct SanitizerNameInput {
    bool is_string { false };
    FlatPtr name { 0 };
    FlatPtr namespace_ { 0 };
};

template<typename T>
struct SanitizerListInput {
    bool present { false };
    ReadonlySpan<T> values;
};

struct SanitizerElementInput {
    SanitizerNameInput name;
    SanitizerListInput<SanitizerNameInput> attributes;
    SanitizerListInput<SanitizerNameInput> remove_attributes;
};

struct SanitizerConfigInput {
    SanitizerListInput<SanitizerElementInput> elements;
    SanitizerListInput<SanitizerNameInput> remove_elements;
    SanitizerListInput<SanitizerNameInput> replace_with_children_elements;
    SanitizerListInput<SanitizerNameInput> processing_instructions;
    SanitizerListInput<SanitizerNameInput> remove_processing_instructions;
    SanitizerListInput<SanitizerNameInput> attributes;
    SanitizerListInput<SanitizerNameInput> remove_attributes;
    SanitizerOptionalBoolean comments { SanitizerOptionalBoolean::Absent };
    SanitizerOptionalBoolean data_attributes { SanitizerOptionalBoolean::Absent };
    SanitizerOptionalBoolean javascript_urls { SanitizerOptionalBoolean::Absent };
};

struct SanitizerStringView {
    ReadonlyBytes latin1;
    ReadonlySpan<u16> utf16;
};

extern "C" {
void web_rust_sanitizer_configuration_retain(void const*);
void web_rust_sanitizer_configuration_release(void const*);
SanitizerConfigurationStatus web_rust_get_a_sanitizer_config_from_options(SanitizerInputKind, void const* input, bool safe, void const** out_configuration);
Sanitizer* web_rust_get_a_sanitizer_from_sanitizer_input(JSVM*, SanitizerInputKind, void const* input);
bool web_rust_sanitizer_config_allows_comments(void const*);
bool web_rust_sanitizer_config_allows_processing_instruction_target(void const*, SanitizerStringView);
SanitizerAction web_rust_sanitizer_action_for_element_name(void const*, FlatPtr name, FlatPtr namespace_);
bool web_rust_sanitizer_config_allows_attribute(void const*, FlatPtr name, FlatPtr namespace_, SanitizerStringView, FlatPtr element_name, FlatPtr element_namespace);
void web_sanitizer_event_handler_content_attributes(ReadonlySpan<FlatPtr>*);
}

SanitizerConfiguration::SanitizerConfiguration(void const* configuration)
    : m_configuration(configuration)
{
    VERIFY(m_configuration);
}

SanitizerConfiguration::SanitizerConfiguration(SanitizerConfiguration const& other)
    : m_configuration(other.m_configuration)
{
    if (m_configuration)
        web_rust_sanitizer_configuration_retain(m_configuration);
}

SanitizerConfiguration::SanitizerConfiguration(SanitizerConfiguration&& other)
    : m_configuration(exchange(other.m_configuration, nullptr))
{
}

SanitizerConfiguration& SanitizerConfiguration::operator=(SanitizerConfiguration const& other)
{
    if (this != &other) {
        if (other.m_configuration)
            web_rust_sanitizer_configuration_retain(other.m_configuration);
        web_rust_sanitizer_configuration_release(m_configuration);
        m_configuration = other.m_configuration;
    }
    return *this;
}

SanitizerConfiguration& SanitizerConfiguration::operator=(SanitizerConfiguration&& other)
{
    if (this != &other) {
        web_rust_sanitizer_configuration_release(m_configuration);
        m_configuration = exchange(other.m_configuration, nullptr);
    }
    return *this;
}

SanitizerConfiguration::~SanitizerConfiguration()
{
    web_rust_sanitizer_configuration_release(m_configuration);
}

void const* SanitizerConfiguration::rust_configuration() const
{
    VERIFY(m_configuration);
    return m_configuration;
}

static SanitizerNameInput to_name_input(Utf16String const& string)
{
    return { true, string.raw_identity(), 0 };
}

template<typename Dictionary>
static SanitizerNameInput to_name_input(Dictionary const& dictionary)
{
    return { false, dictionary.name.raw_identity(), dictionary.namespace_.has_value() ? dictionary.namespace_->raw_identity() : 0 };
}

template<typename Input>
static SanitizerNameInput to_pi_input(Input const& input)
{
    return input.visit(
        [](Utf16String const& string) { return SanitizerNameInput { true, string.raw_identity(), 0 }; },
        [](Bindings::SanitizerProcessingInstruction const& dictionary) { return SanitizerNameInput { false, dictionary.target.raw_identity(), 0 }; });
}

struct SanitizerConfigView {
    explicit SanitizerConfigView(Bindings::SanitizerConfig const& config)
    {
        auto names = [](auto const& value) { return value.visit([](auto const& name) { return to_name_input(name); }); };
        if (config.elements.has_value()) {
            for (auto const& element : *config.elements) {
                elements.append(element.visit(
                    [](Utf16String const& string) { return SanitizerElementInput { to_name_input(string), {}, {} }; },
                    [&](Bindings::SanitizerElementNamespaceWithAttributes const& dictionary) {
                        return SanitizerElementInput { to_name_input(dictionary), list(dictionary.attributes, names), list(dictionary.remove_attributes, names) };
                    }));
            }
            input.elements = { true, elements };
        }
        input.remove_elements = list(config.remove_elements, names);
        input.replace_with_children_elements = list(config.replace_with_children_elements, names);
        input.processing_instructions = list(config.processing_instructions, [](auto const& pi) { return to_pi_input(pi); });
        input.remove_processing_instructions = list(config.remove_processing_instructions, [](auto const& pi) { return to_pi_input(pi); });
        input.attributes = list(config.attributes, names);
        input.remove_attributes = list(config.remove_attributes, names);
        auto boolean = [](Optional<bool> value) {
            if (!value.has_value())
                return SanitizerOptionalBoolean::Absent;
            return *value ? SanitizerOptionalBoolean::True : SanitizerOptionalBoolean::False;
        };
        input.comments = boolean(config.comments);
        input.data_attributes = boolean(config.data_attributes);
        input.javascript_urls = boolean(config.javascript_ur_ls);
    }

    template<typename T, typename Convert>
    SanitizerListInput<SanitizerNameInput> list(Optional<Vector<T>> const& values, Convert convert)
    {
        if (!values.has_value())
            return {};
        Vector<SanitizerNameInput> converted;
        converted.ensure_capacity(values->size());
        for (auto const& value : *values)
            converted.unchecked_append(convert(value));
        lists.append(move(converted));
        return { true, lists.last() };
    }

    SanitizerConfigInput input;
    Vector<SanitizerElementInput> elements;
    Vector<Vector<SanitizerNameInput>> lists;
};

template<typename Function>
static auto with_sanitizer_input(SanitizerInput const& input, Function function)
{
    return input.visit(
        [&](GC::Ref<Sanitizer> sanitizer) { return function(SanitizerInputKind::Sanitizer, sanitizer.ptr()); },
        [&](Bindings::SanitizerConfig const& config) {
            SanitizerConfigView view { config };
            return function(SanitizerInputKind::Config, &view.input);
        },
        [&](Bindings::SanitizerPresets preset) {
            VERIFY(preset == Bindings::SanitizerPresets::Default);
            return function(SanitizerInputKind::DefaultPreset, nullptr);
        });
}

// NB: The Rust implementation throws this error itself when script calls the Sanitizer constructor.
static WebIDL::SimpleException invalid_configuration_error()
{
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Sanitizer configuration is not valid"_utf16 };
}

WebIDL::ExceptionOr<Optional<SanitizerConfiguration>> get_a_sanitizer_config_from_options(SanitizerOptions const& options, SanitizerSafety safety)
{
    auto get_configuration = [&](SanitizerInputKind kind, void const* input) -> WebIDL::ExceptionOr<Optional<SanitizerConfiguration>> {
        void const* configuration = nullptr;
        switch (web_rust_get_a_sanitizer_config_from_options(kind, input, safety == SanitizerSafety::Safe, &configuration)) {
        case SanitizerConfigurationStatus::Null:
            return Optional<SanitizerConfiguration> {};
        case SanitizerConfigurationStatus::Configuration:
            return Optional<SanitizerConfiguration> { SanitizerConfiguration { configuration } };
        case SanitizerConfigurationStatus::Invalid:
            return invalid_configuration_error();
        }
        VERIFY_NOT_REACHED();
    };
    return options.visit(
        [&](GC::Ref<TrustedTypes::TrustedHTMLParserOptions> trusted_options) {
            auto const& configuration = trusted_options->sanitizer_configuration();
            return get_configuration(SanitizerInputKind::TrustedOptions, configuration.has_value() ? configuration->rust_configuration() : nullptr);
        },
        [&](Bindings::SetHTMLOptions const* dictionary) {
            return with_sanitizer_input(dictionary->sanitizer, get_configuration);
        },
        [&](auto const* dictionary) -> WebIDL::ExceptionOr<Optional<SanitizerConfiguration>> {
            if (!dictionary->sanitizer.has_value())
                return get_configuration(SanitizerInputKind::None, nullptr);
            return with_sanitizer_input(*dictionary->sanitizer, get_configuration);
        });
}

WebIDL::ExceptionOr<GC::Ref<Sanitizer>> get_a_sanitizer_from_sanitizer_input(SanitizerInput const& input)
{
    return with_sanitizer_input(input, [](SanitizerInputKind kind, void const* data) -> WebIDL::ExceptionOr<GC::Ref<Sanitizer>> {
        auto* sanitizer = web_rust_get_a_sanitizer_from_sanitizer_input(JS::HostABI::vm_to_abi(JS::VM::the()), kind, data);
        if (!sanitizer)
            return invalid_configuration_error();
        return GC::Ref { *sanitizer };
    });
}

static SanitizerStringView to_string_view(Utf16View value)
{
    if (value.has_latin1_storage())
        return { value.latin1_span(), {} };
    auto units = value.utf16_span();
    return { {}, { reinterpret_cast<u16 const*>(units.data()), units.size() } };
}

bool sanitizer_config_allows_comments(SanitizerConfiguration const& configuration)
{
    return web_rust_sanitizer_config_allows_comments(configuration.rust_configuration());
}

bool sanitizer_config_allows_processing_instruction_target(Utf16View const& target, SanitizerConfiguration const& configuration)
{
    return web_rust_sanitizer_config_allows_processing_instruction_target(configuration.rust_configuration(), to_string_view(target));
}

SanitizerAction sanitizer_action_for_element_name(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_, SanitizerConfiguration const& configuration)
{
    return web_rust_sanitizer_action_for_element_name(configuration.rust_configuration(), local_name.raw_identity(), namespace_.has_value() ? namespace_->raw_identity() : 0);
}

bool sanitizer_config_allows_attribute(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_, Utf16View const& value, Utf16FlyString const& element_local_name, Optional<Utf16FlyString> const& element_namespace, SanitizerConfiguration const& configuration)
{
    return web_rust_sanitizer_config_allows_attribute(configuration.rust_configuration(), local_name.raw_identity(), namespace_.has_value() ? namespace_->raw_identity() : 0, to_string_view(value), element_local_name.raw_identity(), element_namespace.has_value() ? element_namespace->raw_identity() : 0);
}

static auto const& event_handler_content_attributes()
{
    struct Attributes {
        Vector<Utf16String> names;
        Vector<FlatPtr> representations;
    };
    static NeverDestroyed<Attributes const> attributes { [] {
        Attributes attributes;
#define __ENUMERATE(attribute_name, event_name) \
    attributes.names.append(HTML::AttributeNames::attribute_name.to_utf16_string());
        ENUMERATE_GLOBAL_EVENT_HANDLERS(__ENUMERATE)
        ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE
        attributes.names.append("onbegin"_utf16);
        attributes.names.append("onend"_utf16);
        attributes.names.append("onrepeat"_utf16);
        attributes.representations.ensure_capacity(attributes.names.size());
        for (auto const& name : attributes.names)
            attributes.representations.unchecked_append(name.raw_identity());
        return attributes;
    }() };
    return *attributes;
}

void web_sanitizer_event_handler_content_attributes(ReadonlySpan<FlatPtr>* names)
{
    *names = event_handler_content_attributes().representations;
}

}
