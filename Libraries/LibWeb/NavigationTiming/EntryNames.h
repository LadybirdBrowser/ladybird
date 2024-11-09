/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::NavigationTiming::EntryNames {

#define ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES                                                          \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(navigationStart, navigation_start)                          \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(unloadEventStart, unload_event_start)                       \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(unloadEventEnd, unload_event_end)                           \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(redirectStart, redirect_start)                              \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(redirectEnd, redirect_end)                                  \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(fetchStart, fetch_start)                                    \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(domainLookupStart, domain_lookup_start)                     \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(domainLookupEnd, domain_lookup_end)                         \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(connectStart, connect_start)                                \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(connectEnd, connect_end)                                    \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(secureConnectionStart, secure_connection_start)             \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(requestStart, request_start)                                \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(responseStart, response_start)                              \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(responseEnd, response_end)                                  \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(domLoading, dom_loading)                                    \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(domInteractive, dom_interactive)                            \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(domContentLoadedEventStart, dom_content_loaded_event_start) \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(domContentLoadedEventEnd, dom_content_loaded_event_end)     \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(domComplete, dom_complete)                                  \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(loadEventStart, load_event_start)                           \
    __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(loadEventEnd, load_event_end)

#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(name, _) extern FlyString name;
ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

void initialize_strings();

}
