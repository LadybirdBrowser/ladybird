/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/SanitizerConfiguration.h>

namespace Web::TrustedTypes {

// https://w3c.github.io/trusted-types/dist/spec/#trusted-html-parser-options
class TrustedHTMLParserOptions final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(TrustedHTMLParserOptions, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(TrustedHTMLParserOptions);

public:
    static GC::Ref<TrustedHTMLParserOptions> create(Optional<HTML::SanitizerConfiguration>, bool run_scripts);

    virtual ~TrustedHTMLParserOptions() override;

    bool run_scripts() const { return m_run_scripts; }
    Optional<HTML::SanitizerConfiguration> const& sanitizer_configuration() const { return m_sanitizer_configuration; }

private:
    TrustedHTMLParserOptions(Optional<HTML::SanitizerConfiguration>, bool run_scripts);

    // TrustedHTMLParserOptions objects have an associated boolean runScripts and an associated SanitizerConfig-or-null
    // sanitizer configuration. These values are set when the object is created, and will never change during its
    // lifetime.
    Optional<HTML::SanitizerConfiguration> const m_sanitizer_configuration;
    bool const m_run_scripts { false };
};

}
