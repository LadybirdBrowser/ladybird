/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::ContentSecurityPolicy::Directives::KeywordSources {

// https://w3c.github.io/webappsec-csp/#grammardef-keyword-source
#define ENUMERATE_KEYWORD_SOURCES                                                \
    __ENUMERATE_KEYWORD_SOURCE(Self, "'self'")                                   \
    __ENUMERATE_KEYWORD_SOURCE(UnsafeInline, "'unsafe-inline'")                  \
    __ENUMERATE_KEYWORD_SOURCE(UnsafeEval, "'unsafe-eval'")                      \
    __ENUMERATE_KEYWORD_SOURCE(StrictDynamic, "'strict-dynamic'")                \
    __ENUMERATE_KEYWORD_SOURCE(UnsafeHashes, "'unsafe-hashes'")                  \
    __ENUMERATE_KEYWORD_SOURCE(ReportSample, "'report-sample'")                  \
    __ENUMERATE_KEYWORD_SOURCE(UnsafeAllowRedirects, "'unsafe-allow-redirects'") \
    __ENUMERATE_KEYWORD_SOURCE(WasmUnsafeEval, "'wasm-unsafe-eval'")

#define __ENUMERATE_KEYWORD_SOURCE(name, value) extern FlyString name;
ENUMERATE_KEYWORD_SOURCES
#undef __ENUMERATE_KEYWORD_SOURCE

}
