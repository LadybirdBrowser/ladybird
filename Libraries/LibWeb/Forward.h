/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibJS/Forward.h>

namespace Web {
class DragAndDropEventHandler;
class EventHandler;
class InputEventsTarget;
class LoadRequest;
class Page;
class PageClient;
class PaintContext;
class Resource;
class ResourceLoader;
class XMLDocumentBuilder;

enum class InvalidateDisplayList;
enum class TraversalDecision;

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(i64, UniqueNodeID, Comparison, Increment, CastToUnderlying);
}

namespace Web::Painting {
class BackingStore;
class DisplayList;
class DisplayListRecorder;
class SVGGradientPaintStyle;
using PaintStyle = RefPtr<SVGGradientPaintStyle>;
}

namespace Web::Animations {
class Animatable;
class Animation;
class AnimationEffect;
class AnimationPlaybackEvent;
class AnimationTimeline;
class DocumentTimeline;
class KeyframeEffect;
}

namespace Web::ARIA {
class AriaData;
class ARIAMixin;

enum class StateAndProperties;
}

namespace Web::Bindings {
class Intrinsics;
class OptionConstructor;

enum class AudioContextLatencyCategory : u8;
enum class CanPlayTypeResult : u8;
enum class CanvasFillRule : u8;
enum class CanvasTextAlign : u8;
enum class CanvasTextBaseline : u8;
enum class ColorGamut : u8;
enum class DOMParserSupportedType : u8;
enum class EndingType : u8;
enum class HdrMetadataType : u8;
enum class ImageSmoothingQuality : u8;
enum class MediaDecodingType : u8;
enum class MediaEncodingType : u8;
enum class MediaKeysRequirement : u8;
enum class ReadableStreamReaderMode : u8;
enum class ReferrerPolicy : u8;
enum class RenderBlockingStatusType : u8;
enum class RequestCache : u8;
enum class RequestCredentials : u8;
enum class RequestDestination : u8;
enum class RequestDuplex : u8;
enum class RequestMode : u8;
enum class RequestPriority : u8;
enum class RequestRedirect : u8;
enum class ResizeObserverBoxOptions : u8;
enum class ResponseType : u8;
enum class TextTrackKind : u8;
enum class TransferFunction : u8;
enum class XMLHttpRequestResponseType : u8;
}

namespace Web::Clipboard {
class Clipboard;
class ClipboardItem;
}

namespace Web::Compression {
class CompressionStream;
class DecompressionStream;
}

namespace Web::ContentSecurityPolicy {
class Policy;
class PolicyList;
class SecurityPolicyViolationEvent;
class Violation;
struct SecurityPolicyViolationEventInit;
struct SerializedPolicy;
}

namespace Web::ContentSecurityPolicy::Directives {
class Directive;
struct SerializedDirective;
}

namespace Web::Cookie {
struct Cookie;
struct ParsedCookie;

enum class Source;
}

namespace Web::CredentialManagement {
class Credential;
class CredentialsContainer;
class FederatedCredential;
class PasswordCredential;

struct CredentialData;
struct CredentialRequestOptions;
struct CredentialCreationOptions;
struct FederatedCredentialRequestOptions;
struct FederatedCredentialInit;
struct PasswordCredentialData;
}

namespace Web::Crypto {
class Crypto;
class SubtleCrypto;
}

namespace Web::CSS {
class AbstractImageStyleValue;
class Angle;
class AngleOrCalculated;
class AnglePercentage;
class AngleStyleValue;
class BackgroundRepeatStyleValue;
class BackgroundSizeStyleValue;
class BasicShapeStyleValue;
class BorderRadiusStyleValue;
class CalculatedStyleValue;
class Clip;
class ColorSchemeStyleValue;
class ConicGradientStyleValue;
class ContentStyleValue;
class CounterDefinitionsStyleValue;
class CounterStyleValue;
class CSSAnimation;
class CSSColorValue;
class CSSConditionRule;
class CSSFontFaceRule;
class CSSGroupingRule;
class CSSHSL;
class CSSHWB;
class CSSImportRule;
class CSSKeyframeRule;
class CSSKeyframesRule;
class CSSKeywordValue;
class CSSLayerBlockRule;
class CSSLayerStatementRule;
class CSSMediaRule;
class CSSNamespaceRule;
class CSSNestedDeclarations;
class CSSOKLab;
class CSSOKLCH;
class CSSPropertyRule;
class CSSRGB;
class CSSRule;
class CSSRuleList;
class CSSStyleDeclaration;
class CSSStyleProperties;
class CSSStyleRule;
class CSSStyleSheet;
class CSSStyleValue;
class CSSSupportsRule;
class CursorStyleValue;
class CustomIdentStyleValue;
class Display;
class DisplayStyleValue;
class EasingStyleValue;
class EdgeStyleValue;
class ExplicitGridTrack;
class FilterValueListStyleValue;
class FitContentStyleValue;
class Flex;
class FlexOrCalculated;
class FlexStyleValue;
class FontFace;
class FontFaceSet;
class Frequency;
class FrequencyOrCalculated;
class FrequencyPercentage;
class FrequencyStyleValue;
class GridAutoFlowStyleValue;
class GridFitContent;
class GridMinMax;
class GridRepeat;
class GridSize;
class GridTemplateAreaStyleValue;
class GridTrackPlacement;
class GridTrackPlacementStyleValue;
class GridTrackSizeList;
class GridTrackSizeListStyleValue;
class ImageStyleValue;
class IntegerOrCalculated;
class IntegerStyleValue;
class Length;
class LengthBox;
class LengthOrCalculated;
class LengthPercentage;
class LengthStyleValue;
class LinearGradientStyleValue;
class MathDepthStyleValue;
class MediaFeatureValue;
class MediaList;
class MediaQuery;
class MediaQueryList;
class MediaQueryListEvent;
class Number;
class NumberOrCalculated;
class NumberStyleValue;
class OpenTypeTaggedStyleValue;
class ParsedFontFace;
class Percentage;
class PercentageOrCalculated;
class PercentageStyleValue;
class PositionStyleValue;
class RadialGradientStyleValue;
class Ratio;
class RatioStyleValue;
class RectStyleValue;
class Resolution;
class ResolutionOrCalculated;
class ResolutionStyleValue;
class Screen;
class ScreenOrientation;
class ScrollbarGutterStyleValue;
class Selector;
class ShadowStyleValue;
class ShorthandStyleValue;
class Size;
class StringStyleValue;
class StyleComputer;
class ComputedProperties;
class StyleSheet;
class StyleSheetList;
class StyleValueList;
class Supports;
class SVGPaint;
class Time;
class TimeOrCalculated;
class TimePercentage;
class TimeStyleValue;
class Transformation;
class TransformationStyleValue;
class TransitionStyleValue;
class UnicodeRangeStyleValue;
class UnresolvedStyleValue;
class URLStyleValue;
class VisualViewport;

enum class Keyword;
enum class MediaFeatureID;
enum class PropertyID;

struct BackgroundLayerData;
struct CSSStyleSheetInit;
struct StyleSheetIdentifier;
}

namespace Web::CSS::Parser {
class ComponentValue;
class Parser;
class Token;
class Tokenizer;

struct AtRule;
struct Declaration;
struct Function;
struct QualifiedRule;
struct SimpleBlock;
}

namespace Web::DOM {
class AbortController;
class AbortSignal;
class AbstractRange;
class AccessibilityTreeNode;
class Attr;
class CDATASection;
class CharacterData;
class Comment;
class CustomEvent;
class Document;
class DocumentFragment;
class DocumentLoadEventDelayer;
class DocumentObserver;
class DocumentType;
class DOMEventListener;
class DOMImplementation;
class DOMTokenList;
class EditingHostManager;
class Element;
class ElementByIdMap;
class Event;
class EventHandler;
class EventTarget;
class HTMLCollection;
class IDLEventListener;
class LiveNodeList;
class MutationObserver;
class MutationRecord;
class NamedNodeMap;
class Node;
class NodeFilter;
class NodeIterator;
class NodeList;
class ParentNode;
class Position;
class ProcessingInstruction;
class Range;
class RegisteredObserver;
class ShadowRoot;
class StaticNodeList;
class StaticRange;
class Text;
class TreeWalker;
class XMLDocument;

enum class QuirksMode;

struct AddEventListenerOptions;
struct EventListenerOptions;
}

namespace Web::Encoding {
class TextDecoder;
class TextEncoder;
class TextEncoderStream;

struct TextDecodeOptions;
struct TextDecoderOptions;
struct TextEncoderEncodeIntoResult;
}

namespace Web::EntriesAPI {
class FileSystemEntry;
}

namespace Web::EventTiming {
class PerformanceEventTiming;
}

namespace Web::Fetch {
class BodyMixin;
class Headers;
class HeadersIterator;
class Request;
class Response;
}

namespace Web::Fetch::Fetching {
class PendingResponse;
class RefCountedFlag;
}

namespace Web::Fetch::Infrastructure {
class Body;
class FetchAlgorithms;
class FetchController;
class FetchParams;
class FetchRecord;
class FetchTimingInfo;
class HeaderList;
class IncrementalReadLoopReadRequest;
class Request;
class Response;

struct BodyWithType;
struct ConnectionTimingInfo;
struct Header;
}

namespace Web::FileAPI {
class Blob;
class File;
class FileList;
}

namespace Web::Geometry {
class DOMMatrix;
class DOMMatrixReadOnly;
class DOMPoint;
class DOMPointReadOnly;
class DOMQuad;
class DOMRect;
class DOMRectList;
class DOMRectReadOnly;

struct DOMMatrix2DInit;
struct DOMMatrixInit;
struct DOMPointInit;
}

namespace Web::HTML {
class AnimationFrameCallbackDriver;
class AudioTrack;
class AudioTrackList;
class BarProp;
class BeforeUnloadEvent;
class BroadcastChannel;
class BrowsingContext;
class BrowsingContextGroup;
class CanvasRenderingContext2D;
class ClassicScript;
class CloseEvent;
class CloseWatcher;
class CloseWatcherManager;
class CustomElementDefinition;
class CustomElementRegistry;
class DataTransfer;
class DataTransferItem;
class DataTransferItemList;
class DecodedImageData;
class DocumentState;
class DOMParser;
class DOMStringList;
class DOMStringMap;
class DragDataStore;
class DragEvent;
class ElementInternals;
class ErrorEvent;
class EventHandler;
class EventLoop;
class EventSource;
class FormAssociatedElement;
class FormDataEvent;
class History;
class HTMLAllCollection;
class HTMLAnchorElement;
class HTMLAreaElement;
class HTMLAudioElement;
class HTMLBaseElement;
class HTMLBodyElement;
class HTMLBRElement;
class HTMLButtonElement;
class HTMLCanvasElement;
class HTMLDataElement;
class HTMLDataListElement;
class HTMLDetailsElement;
class HTMLDialogElement;
class HTMLDirectoryElement;
class HTMLDivElement;
class HTMLDListElement;
class HTMLElement;
class HTMLEmbedElement;
class HTMLFieldSetElement;
class HTMLFontElement;
class HTMLFormControlsCollection;
class HTMLFormElement;
class HTMLFrameElement;
class HTMLFrameSetElement;
class HTMLHeadElement;
class HTMLHeadingElement;
class HTMLHRElement;
class HTMLHtmlElement;
class HTMLIFrameElement;
class HTMLImageElement;
class HTMLInputElement;
class HTMLLabelElement;
class HTMLLegendElement;
class HTMLLIElement;
class HTMLLinkElement;
class HTMLMapElement;
class HTMLMarqueeElement;
class HTMLMediaElement;
class HTMLMenuElement;
class HTMLMetaElement;
class HTMLMeterElement;
class HTMLModElement;
class HTMLObjectElement;
class HTMLOListElement;
class HTMLOptGroupElement;
class HTMLOptionElement;
class HTMLOptionsCollection;
class HTMLOutputElement;
class HTMLParagraphElement;
class HTMLParamElement;
class HTMLParser;
class HTMLPictureElement;
class HTMLPreElement;
class HTMLProgressElement;
class HTMLQuoteElement;
class HTMLScriptElement;
class HTMLSelectElement;
class HTMLSlotElement;
class HTMLSourceElement;
class HTMLSpanElement;
class HTMLStyleElement;
class HTMLSummaryElement;
class HTMLTableCaptionElement;
class HTMLTableCellElement;
class HTMLTableColElement;
class HTMLTableElement;
class HTMLTableRowElement;
class HTMLTableSectionElement;
class HTMLTemplateElement;
class HTMLTextAreaElement;
class HTMLTimeElement;
class HTMLTitleElement;
class HTMLTrackElement;
class HTMLUListElement;
class HTMLUnknownElement;
class HTMLVideoElement;
class ImageBitmap;
class ImageData;
class ImageRequest;
class ListOfAvailableImages;
class Location;
class MediaError;
class MessageChannel;
class MessageEvent;
class MessagePort;
class MimeType;
class MimeTypeArray;
class ModuleMap;
class Navigable;
class NavigableContainer;
class NavigateEvent;
class Navigation;
class NavigationCurrentEntryChangeEvent;
class NavigationDestination;
class NavigationHistoryEntry;
class NavigationObserver;
class NavigationTransition;
class Navigator;
class PageTransitionEvent;
class Path2D;
class Plugin;
class PluginArray;
class PopoverInvokerElement;
class PromiseRejectionEvent;
class RadioNodeList;
class SelectedFile;
class SessionHistoryEntry;
class SharedResourceRequest;
class Storage;
class SubmitEvent;
class TextMetrics;
class TextTrack;
class TextTrackCue;
class TextTrackCueList;
class TextTrackList;
class Timer;
class TimeRanges;
class ToggleEvent;
class TrackEvent;
class TraversableNavigable;
class UserActivation;
class ValidityState;
class VideoTrack;
class VideoTrackList;
class Window;
class WindowEnvironmentSettingsObject;
class WindowProxy;
class Worker;
class WorkerAgent;
class WorkerDebugConsoleClient;
class WorkerEnvironmentSettingsObject;
class WorkerGlobalScope;
class WorkerLocation;
class WorkerNavigator;
class XMLSerializer;

enum class AllowMultipleFiles;
enum class MediaSeekMode;
enum class SandboxingFlagSet;

struct Agent;
struct EmbedderPolicy;
struct Environment;
struct EnvironmentSettingsObject;
struct NavigationParams;
struct OpenerPolicy;
struct OpenerPolicyEnforcementResult;
struct PolicyContainer;
struct POSTResource;
struct ScrollOptions;
struct ScrollToOptions;
struct SerializedFormData;
struct SerializedPolicyContainer;
struct StructuredSerializeOptions;
struct SyntheticRealmSettings;
struct ToggleTaskTracker;
struct TransferDataHolder;
}

namespace Web::HighResolutionTime {
class Performance;
}

namespace Web::IndexedDB {
class Database;
class IDBCursor;
class IDBDatabase;
class IDBFactory;
class IDBIndex;
class IDBKeyRange;
class IDBObjectStore;
class IDBOpenDBRequest;
class IDBRequest;
class IDBTransaction;
class IDBVersionChangeEvent;
class ObjectStore;
class RequestList;
}

namespace Web::Internals {
class Internals;
class WebUI;
}

namespace Web::IntersectionObserver {
class IntersectionObserver;
class IntersectionObserverEntry;

struct IntersectionObserverRegistration;
}

namespace Web::Layout {
class AudioBox;
class BlockContainer;
class BlockFormattingContext;
class Box;
class ButtonBox;
class CheckBox;
class FieldSetBox;
class FlexFormattingContext;
class FormattingContext;
class ImageBox;
class InlineFormattingContext;
class InlineNode;
class Label;
class LabelableNode;
class LegendBox;
class LineBox;
class LineBoxFragment;
class ListItemBox;
class ListItemMarkerBox;
class Node;
class NodeWithStyle;
class NodeWithStyleAndBoxModelMetrics;
class RadioButton;
class ReplacedBox;
class TableWrapper;
class TextNode;
class TreeBuilder;
class VideoBox;
class Viewport;

enum class LayoutMode;

struct LayoutState;
}

namespace Web::MathML {
class MathMLElement;
}

namespace Web::MediaCapabilitiesAPI {
class MediaCapabilities;

struct AudioConfiguration;
struct KeySystemTrackConfiguration;
struct MediaCapabilitiesDecodingInfo;
struct MediaCapabilitiesEncodingInfo;
struct MediaCapabilitiesInfo;
struct MediaCapabilitiesKeySystemConfiguration;
struct MediaConfiguration;
struct MediaDecodingConfiguration;
struct MediaEncodingConfiguration;
struct VideoConfiguration;
}

namespace Web::MediaSourceExtensions {
class BufferedChangeEvent;
class ManagedMediaSource;
class ManagedSourceBuffer;
class MediaSource;
class MediaSourceHandle;
class SourceBuffer;
class SourceBufferList;
}

namespace Web::MimeSniff {
class MimeType;
}

namespace Web::NavigationTiming {
class PerformanceNavigation;
class PerformanceTiming;
}

namespace Web::Painting {
class AudioPaintable;
class ButtonPaintable;
class CheckBoxPaintable;
class FieldSetPaintable;
class LabelablePaintable;
class MediaPaintable;
class Paintable;
class PaintableBox;
class PaintableWithLines;
class StackingContext;
class TextPaintable;
class VideoPaintable;
class ViewportPaintable;

enum class PaintPhase;

struct BorderRadiiData;
struct BorderRadiusData;
struct LinearGradientData;
}

namespace Web::PerformanceTimeline {
class PerformanceEntry;
class PerformanceObserver;
class PerformanceObserverEntryList;

struct PerformanceObserverInit;
}

namespace Web::PermissionsPolicy {
class AutoplayAllowlist;
}

namespace Web::Platform {
class AudioCodecPlugin;
class Timer;
}

namespace Web::ReferrerPolicy {
enum class ReferrerPolicy;
}

namespace Web::RequestIdleCallback {
class IdleDeadline;
}

namespace Web::ResizeObserver {
class ResizeObserver;
}

namespace Web::ResourceTiming {
class PerformanceResourceTiming;
}

namespace Web::Selection {
class Selection;
}

namespace Web::ServiceWorker {
class ServiceWorker;
class ServiceWorkerContainer;
class ServiceWorkerRegistration;
}

namespace Web::Streams {
class ByteLengthQueuingStrategy;
class CountQueuingStrategy;
class ReadableByteStreamController;
class ReadableStream;
class ReadableStreamBYOBReader;
class ReadableStreamBYOBRequest;
class ReadableStreamDefaultController;
class ReadableStreamDefaultReader;
class ReadableStreamGenericReaderMixin;
class ReadIntoRequest;
class ReadRequest;
class TransformStream;
class TransformStreamDefaultController;
class WritableStream;
class WritableStreamDefaultController;
class WritableStreamDefaultWriter;

struct PullIntoDescriptor;
struct QueuingStrategy;
struct QueuingStrategyInit;
struct ReadableStreamGetReaderOptions;
struct Transformer;
struct UnderlyingSink;
struct UnderlyingSource;
}

namespace Web::StorageAPI {
class NavigatorStorage;
class StorageManager;
class StorageShed;

struct StorageBottle;
struct StorageBucket;
struct StorageEndpoint;
struct StorageShelf;
}

namespace Web::SVG {
class SVGAnimatedEnumeration;
class SVGAnimatedLength;
class SVGAnimatedRect;
class SVGCircleElement;
class SVGClipPathElement;
class SVGDefsElement;
class SVGDescElement;
class SVGElement;
class SVGEllipseElement;
class SVGFilterElement;
class SVGForeignObjectElement;
class SVGGeometryElement;
class SVGGraphicsElement;
class SVGImageElement;
class SVGLength;
class SVGLineElement;
class SVGMaskElement;
class SVGMetadataElement;
class SVGPathElement;
class SVGPolygonElement;
class SVGPolylineElement;
class SVGRectElement;
class SVGScriptElement;
class SVGSVGElement;
class SVGTitleElement;
}

namespace Web::UIEvents {
class CompositionEvent;
class InputEvent;
class KeyboardEvent;
class MouseEvent;
class PointerEvent;
class TextEvent;
class UIEvent;
}

namespace Web::URLPattern {
class URLPattern;
}

namespace Web::DOMURL {
class DOMURL;
class URLSearchParams;
class URLSearchParamsIterator;
}

namespace Web::UserTiming {
class PerformanceMark;
class PerformanceMeasure;
}

namespace Web::WebAssembly {
class Global;
class Instance;
class Memory;
class Module;
class Table;
}

namespace Web::WebAudio {
class AudioBuffer;
class AudioBufferSourceNode;
class AudioContext;
class AudioDestinationNode;
class AudioListener;
class AudioNode;
class AudioParam;
class AudioScheduledSourceNode;
class BaseAudioContext;
class BiquadFilterNode;
class DynamicsCompressorNode;
class GainNode;
class OfflineAudioContext;
class OscillatorNode;
class PannerNode;
class PeriodicWave;

enum class AudioContextState;

struct AudioContextOptions;
struct DynamicsCompressorOptions;
struct OscillatorOptions;
}

namespace Web::WebGL {
class OpenGLContext;
class WebGL2RenderingContext;
class WebGLActiveInfo;
class WebGLBuffer;
class WebGLContextEvent;
class WebGLFramebuffer;
class WebGLObject;
class WebGLProgram;
class WebGLRenderbuffer;
class WebGLRenderingContext;
class WebGLSampler;
class WebGLShader;
class WebGLShaderPrecisionFormat;
class WebGLSync;
class WebGLTexture;
class WebGLUniformLocation;
class WebGLVertexArrayObject;
}

namespace Web::WebGL::Extensions {
class ANGLEInstancedArrays;
class EXTColorBufferFloat;
class OESVertexArrayObject;
class WebGLCompressedTextureS3tc;
class WebGLDrawBuffers;
class WebGLVertexArrayObjectOES;
}

namespace Web::WebIDL {
class ArrayBufferView;
class BufferSource;
class CallbackType;
class DOMException;

template<typename ValueType>
class ExceptionOr;

using Promise = JS::PromiseCapability;
}

namespace Web::WebDriver {
class HeapTimer;

struct ActionObject;
struct InputState;
};

namespace Web::WebSockets {
class WebSocket;
}

namespace Web::WebVTT {
class VTTCue;
class VTTRegion;
}

namespace Web::XHR {
class FormData;
class FormDataIterator;
class ProgressEvent;
class XMLHttpRequest;
class XMLHttpRequestEventTarget;
class XMLHttpRequestUpload;

struct FormDataEntry;
}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::UniqueNodeID const&);

template<>
ErrorOr<Web::UniqueNodeID> decode(Decoder&);

}
