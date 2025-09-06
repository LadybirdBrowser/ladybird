/*
 * Copyright (c) 2022, The SerenityOS developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/Traits.h>
#include <LibWebView/Export.h>

namespace WebView {

class Action;
class Application;
class Autocomplete;
class CookieJar;
class Database;
class Menu;
class OutOfProcessWebView;
class ProcessManager;
class Settings;
class ViewImplementation;
class WebContentClient;
class WebUI;

#if defined(AK_OS_MACOS)
class MachPortServer;
#endif

struct Attribute;
struct AutocompleteEngine;
struct BrowserOptions;
struct ConsoleOutput;
struct CookieStorageKey;
struct DOMNodeProperties;
struct Mutation;
struct ProcessHandle;
struct SearchEngine;
struct WebContentOptions;

}

namespace AK {

template<>
struct Traits<WebView::CookieStorageKey>;

}
