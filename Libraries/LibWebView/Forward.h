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
class AutocompleteService;
class BookmarkStore;
class CanonicalNavigable;
class CanonicalTraversable;
class CompositorClient;
class CompositorConnection;
class CompositorHostBase;
class CookieJar;
class DownloadStore;
class ExternalURLHandler;
class FaviconStore;
class FontService;
class HistoryStore;
class HSTSStore;
class Menu;
class OutOfProcessWebView;
class ProcessManager;
class SessionStore;
class Settings;
class SiteIsolationManager;
class StorageJar;
class TraversableSessionHistory;
class ViewImplementation;
class WebContentClient;
class WebDriverBrowserConnection;
class WebWorkerClient;
class WebUI;

enum class PausedDebuggerOverlayAction : u8;

struct Attribute;
struct DownloadRecord;
struct AutocompleteEngine;
struct BookmarkItem;
struct BrowserOptions;
struct ConsoleOutput;
struct CookieStorageKey;
struct DebuggerBreakpointLocation;
struct DebuggerBreakpointOptions;
struct DebuggerBinding;
struct DebuggerConfiguration;
struct DebuggerEnvironment;
struct DebuggerEvaluationResult;
struct DebuggerFrame;
struct DebuggerLocation;
struct DebuggerObjectProperties;
struct DebuggerPause;
struct DebuggerProperty;
struct DebuggerSourcePosition;
struct DebuggerValue;
struct DictionaryLookup;
struct DictionaryLookupTextStyle;
struct DOMNodeProperties;
struct HistoryEntry;
struct Mutation;
struct ProcessHandle;
struct SearchEngine;
struct WebContentOptions;

}

namespace AK {

template<>
struct Traits<WebView::CookieStorageKey>;

}
