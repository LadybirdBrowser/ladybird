/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibCompositing/Forward.h>

namespace Web {

using Compositing::CSSPixels;
using Compositing::UniqueNodeID;

enum class NavigationTarget : u8;
struct InitiatorSourceSnapshot;

}

namespace Web::Bindings {

enum class NavigationType : u8;
enum class RequestCredentials : u8;
enum class WorkerType : u8;

}

namespace Web::Clipboard {

struct SystemClipboardItem;
struct SystemClipboardRepresentation;

}

namespace Web::ContentSecurityPolicy {

struct SerializedPolicy;

}

namespace Web::ContentSecurityPolicy::Directives {

struct SerializedDirective;

}

namespace Web::CSS {

struct StyleSheetIdentifier;

}

namespace Web::Fetch::Infrastructure {

struct ConnectionTimingInfo;

}

namespace Web::HTML {

enum class AllowMultipleFiles;
struct BroadcastChannelMessage;
struct EmbedderPolicy;
struct HistoryNavigationPopulation;
enum class HistoryStepResult;
struct NavigationPopulationRequest;
struct NavigationPopulationResult;
struct NavigationStartRequest;
struct OpenerPolicy;
struct OpenerPolicyEnforcementResult;
struct POSTResource;
struct PostedMessageDescriptor;
struct PreparedNavigationDescriptor;
struct ReplicatedContainerState;
enum class SandboxingFlagSet : u32;
class SelectedFile;
struct SerializedPolicyContainer;
struct SerializedTransferRecord;
struct SessionHistoryEntryDescriptor;
struct TargetSnapshotParams;
class TransferDataEncoder;

}

namespace Web::MimeSniff {

class MimeType;

}

namespace Web::ReferrerPolicy {

enum class ReferrerPolicy;

}

namespace Web::StorageAPI {

struct StorageEndpoint;

}

namespace WebView {

struct Attribute;
struct ConsoleOutput;
struct DOMNodeProperties;
struct DebuggerBinding;
struct DebuggerBreakpointLocation;
struct DebuggerBreakpointOptions;
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
struct Mutation;
struct ProcessHandle;

}
