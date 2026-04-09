/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Variant.h>
#include <LibGC/Forward.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>

namespace Web {

class AutoScrollHandler;
class CSSPixels;
class DisplayListRecordingContext;
class DragAndDropEventHandler;
class ElementResizeAction;
class EventHandler;
class InputEventsTarget;
class LoadRequest;
class Page;
class PageClient;
class Resource;
class ResourceLoader;
class XMLDocumentBuilder;

enum class InvalidateDisplayList;
enum class TraversalDecision;

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(i64, UniqueNodeID, Comparison, Increment, CastToUnderlying);

}

namespace Web::Painting {

class BackingStore;
class DevicePixelConverter;
class DisplayList;
class DisplayListPlayerSkia;
class DisplayListRecorder;
class ExternalContentSource;
class SVGGradientPaintStyle;
class SVGPaintServerPaintStyle;
class SVGPatternPaintStyle;
class ScrollStateSnapshot;
using PaintStyle = RefPtr<SVGPaintServerPaintStyle>;
using PaintStyleOrColor = Variant<PaintStyle, Gfx::Color>;
using ScrollStateSnapshotByDisplayList = HashMap<NonnullRefPtr<DisplayList>, ScrollStateSnapshot>;

}

namespace Web::Animations {

class Animatable;
class Animation;
class AnimationEffect;
class AnimationPlaybackEvent;
class AnimationTimeline;
class DocumentTimeline;
class KeyframeEffect;
class ScrollTimeline;

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
enum class OffscreenRenderingContextId : u8;
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

struct SystemClipboardItem;
struct SystemClipboardRepresentation;

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

class BaseUriDirective;
class ChildSourceDirective;
class ConnectSourceDirective;
class DefaultSourceDirective;
class Directive;
class FontSourceDirective;
class FormActionDirective;
class FrameAncestorsDirective;
class FrameSourceDirective;
class ImageSourceDirective;
class ManifestSourceDirective;
class MediaSourceDirective;
class ObjectSourceDirective;
class ReportToDirective;
class ReportUriDirective;
class SandboxDirective;
class ScriptSourceAttributeDirective;
class ScriptSourceDirective;
class ScriptSourceElementDirective;
class StyleSourceAttributeDirective;
class StyleSourceDirective;
class StyleSourceElementDirective;
class WebRTCDirective;
class WorkerSourceDirective;
struct SerializedDirective;

}

namespace Web::CookieStore {

class CookieChangeEvent;
class CookieStore;

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

namespace Web::CSS::FilterOperation {

struct Blur;
struct DropShadow;
struct HueRotate;
struct Color;

}

namespace Web::CSS {

class AbstractImageStyleValue;
class AddFunctionStyleValue;
class AnchorStyleValue;
class AnchorSizeStyleValue;
class Angle;
class AngleOrCalculated;
class AnglePercentage;
class AngleStyleValue;
class BackgroundSizeStyleValue;
class BasicShapeStyleValue;
class BorderImageSliceStyleValue;
class BorderRadiusRectStyleValue;
class BorderRadiusStyleValue;
class CalculatedStyleValue;
class CalculationNode;
class CascadedProperties;
class CustomPropertyData;
class Clip;
class ColorMixStyleValue;
class ColorSchemeStyleValue;
class ColorFunctionStyleValue;
class ColorStyleValue;
class ComputedProperties;
class ConicGradientStyleValue;
class ContentStyleValue;
class CounterDefinitionsStyleValue;
class CounterStyle;
class CounterStyleStyleValue;
class CounterStyleSystemStyleValue;
class CounterStyleValue;
class CountersSet;
class CSSAnimation;
class CSSConditionRule;
class CSSCounterStyleRule;
class CSSDescriptors;
class CSSFontFaceDescriptors;
class CSSFontFaceRule;
class CSSFontFeatureValuesMap;
class CSSFontFeatureValuesRule;
class CSSGroupingRule;
class CSSImageValue;
class CSSImportRule;
class CSSKeyframeRule;
class CSSKeyframesRule;
class CSSKeywordValue;
class CSSLayerBlockRule;
class CSSLayerStatementRule;
class CSSMarginRule;
class CSSMathClamp;
class CSSMathInvert;
class CSSMathMax;
class CSSMathMin;
class CSSMathNegate;
class CSSMathProduct;
class CSSMathSum;
class CSSMathValue;
class CSSMatrixComponent;
class CSSMediaRule;
class CSSNamespaceRule;
class CSSNestedDeclarations;
class CSSNumericArray;
class CSSNumericValue;
class CSSPageRule;
class CSSPageDescriptors;
class CSSPerspective;
class CSSPropertyRule;
class CSSRotate;
class CSSRule;
class CSSRuleList;
class CSSScale;
class CSSSkew;
class CSSSkewX;
class CSSSkewY;
class CSSStyleDeclaration;
class CSSStyleProperties;
class CSSStyleRule;
class CSSStyleSheet;
class CSSStyleValue;
class CSSSupportsRule;
class CSSTransformComponent;
class CSSTransformValue;
class CSSTranslate;
class CSSUnitValue;
class CSSUnparsedValue;
class CSSVariableReferenceValue;
class CursorStyleValue;
class CustomIdentStyleValue;
class DimensionStyleValue;
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
class FontComputer;
class FontFace;
class FontFaceSet;
class FontSourceStyleValue;
class FontStyleStyleValue;
class FontVariantAlternatesFunctionStyleValue;
class Frequency;
class FrequencyOrCalculated;
class FrequencyPercentage;
class FrequencyStyleValue;
class GridAutoFlowStyleValue;
class GridLineNames;
class GridMinMax;
class GridRepeat;
class GridSize;
class GridTemplateAreaStyleValue;
class GridTrackPlacement;
class GridTrackPlacementStyleValue;
class GridTrackSizeList;
class GridTrackSizeListStyleValue;
class GuaranteedInvalidStyleValue;
class HSLColorStyleValue;
class HWBColorStyleValue;
class ImageStyleValue;
class IntegerOrCalculated;
class IntegerStyleValue;
class InvalidationSet;
class KeywordStyleValue;
class Length;
class LengthBox;
class LengthOrAuto;
class LengthOrAutoOrCalculated;
class LengthOrCalculated;
class LengthPercentage;
class LengthPercentageOrAuto;
class LengthStyleValue;
class LinearGradientStyleValue;
class MediaFeatureValue;
class MediaList;
class MediaQuery;
class MediaQueryList;
class MediaQueryListEvent;
class Number;
class NumberOrCalculated;
class NumberStyleValue;
class NumericType;
class OKLabColorStyleValue;
class OKLCHColorStyleValue;
class OpenTypeTaggedStyleValue;
class ParsedFontFace;
class PendingSubstitutionStyleValue;
class Percentage;
class PercentageOrCalculated;
class PercentageStyleValue;
class PositionStyleValue;
class PropertyNameAndID;
class RadialGradientStyleValue;
class RadialSizeStyleValue;
class RandomValueSharingStyleValue;
class Ratio;
class RatioStyleValue;
class RectStyleValue;
class RepeatStyleStyleValue;
class Resolution;
class ResolutionOrCalculated;
class ResolutionStyleValue;
class RGBColorStyleValue;
class Screen;
class ScreenOrientation;
class ScrollbarGutterStyleValue;
class Selector;
class ShadowStyleValue;
class ShorthandStyleValue;
class Size;
class ScrollbarColorStyleValue;
class ScrollFunctionStyleValue;
class StringStyleValue;
class StyleComputer;
class StylePropertyMap;
class StylePropertyMapReadOnly;
class StyleScope;
class StyleSheet;
class StyleSheetList;
class StyleValue;
class StyleValueList;
class SuperellipseStyleValue;
class Supports;
class SVGPaint;
class TextIndentStyleValue;
class TextUnderlinePositionStyleValue;
class Time;
class TimeOrCalculated;
class TimePercentage;
class TimeStyleValue;
class TransformationStyleValue;
class TreeCountingFunctionStyleValue;
class TupleStyleValue;
class UnicodeRangeStyleValue;
class UnresolvedStyleValue;
class URL;
class URLStyleValue;
class ViewFunctionStyleValue;
class VisualViewport;

enum class FontFeatureValueType : u8;
enum class Keyword : u16;
enum class MediaFeatureID : u8;
enum class PropertyID : u16;
enum class ValueType : u8;
enum class AnimatedPropertyResultOfTransition : u8;

enum class AbsoluteSize : u8;
enum class AnchorSize : u8;
enum class AnimationComposition : u8;
enum class AnimationDirection : u8;
enum class AnimationFillMode : u8;
enum class AnimationPlayState : u8;
enum class Axis : u8;
enum class CommonLigValue : u8;
enum class ContextualAltValue : u8;
enum class CounterStyleSystem : u8;
enum class CrossOriginModifierValue : u8;
enum class Direction : u8;
enum class DiscretionaryLigValue : u8;
enum class DisplayBox : u8;
enum class DisplayInside : u8;
enum class DisplayInternal : u8;
enum class DisplayOutside : u8;
enum class EastAsianVariant : u8;
enum class EastAsianWidth : u8;
enum class FontDisplay : u8;
enum class FontKerning : u8;
enum class FontOpticalSizing : u8;
enum class FontStyleKeyword : u8;
enum class FontTech : u8;
enum class FontVariantCaps : u8;
enum class FontVariantEmoji : u8;
enum class FontVariantPosition : u8;
enum class HistoricalLigValue : u8;
enum class HueInterpolationMethod : u8;
enum class ImageRendering : u8;
enum class MixBlendMode : u8;
enum class NumericFigureValue : u8;
enum class NumericSpacingValue : u8;
enum class NumericFractionValue : u8;
enum class PaintOrder : u8;
enum class PositionEdge : u8;
enum class RadialExtent : u8;
enum class ReferrerPolicyModifierValue : u8;
enum class RelativeSize : u8;
enum class Repetition : u8;
enum class RoundingStrategy : u8;
enum class Scroller : u8;
enum class StepPosition : u8;
enum class StrokeLinecap : u8;
enum class StrokeLinejoin : u8;
enum class SymbolsType : u8;
enum class TextRendering : u8;
enum class TextUnderlinePositionHorizontal : u8;
enum class TextUnderlinePositionVertical : u8;
enum class TransitionBehavior : u8;
enum class WritingMode : u8;

struct BackgroundLayerData;
struct CalculationContext;
struct CalculationResolutionContext;
struct CSSStyleSheetInit;
struct GridRepeatParams;
struct LogicalAliasMappingContext;
struct RandomCachingKey;
struct RequiredInvalidationAfterStyleChange;
struct StyleSheetIdentifier;
struct TransitionProperties;

// https://drafts.css-houdini.org/css-typed-om-1/#typedefdef-cssnumberish
using CSSNumberish = Variant<double, GC::Root<CSSNumericValue>>;
using PaintOrderList = Array<PaintOrder, 3>;
using StyleValueVector = Vector<ValueComparingNonnullRefPtr<StyleValue const>>;
using StyleValueTuple = Vector<ValueComparingRefPtr<StyleValue const>>;

using FilterValue = Variant<FilterOperation::Blur, FilterOperation::DropShadow, FilterOperation::HueRotate, FilterOperation::Color, URL>;

}

namespace Web::CSS::Parser {

class ComponentValue;
class GuardedSubstitutionContexts;
class Parser;
class SyntaxNode;
class Token;
class Tokenizer;

struct AtRule;
struct Declaration;
struct Function;
struct GuaranteedInvalidValue;
struct ParsingParams;
struct QualifiedRule;
struct SimpleBlock;

}

namespace Web::DOM {

class AbortController;
class AbortSignal;
class AbstractElement;
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
class PseudoElement;
class Range;
class RegisteredObserver;
class ShadowRoot;
class SlotRegistry;
class StaticNodeList;
class StaticRange;
class StyleInvalidator;
class Text;
class TreeWalker;
class XMLDocument;

enum class QuirksMode;
enum class SetNeedsLayoutReason;

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

namespace Web::EncryptedMediaExtensions {

class MediaKeySystemAccess;

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
class IncrementalReadLoopReadRequest;
class Request;
class Response;

struct BodyWithType;
struct ConnectionTimingInfo;

}

namespace Web::FileAPI {

class Blob;
class File;
class FileList;

}

namespace Web::Gamepad {

class NavigatorGamepadPartial;
class Gamepad;
class GamepadButton;
class GamepadEvent;
class GamepadHapticActuator;

}

namespace Web::Geolocation {

class Geolocation;
class GeolocationCoordinates;
class GeolocationPosition;
class GeolocationPositionError;

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
class CustomStateSet;
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
class External;
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
class HTMLSelectedContentElement;
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
class OffscreenCanvas;
class OffscreenCanvasRenderingContext2D;
class PageTransitionEvent;
class Path2D;
class Plugin;
class PluginArray;
class PopoverTargetAttributes;
class PromiseRejectionEvent;
class RadioNodeList;
class SelectedFile;
class SessionHistoryEntry;
class SharedResourceRequest;
class SharedWorker;
class SharedWorkerGlobalScope;
class Storage;
class SubmitEvent;
class TextMetrics;
class TextTrack;
class TextTrackCue;
class TextTrackCueList;
class TextTrackList;
class TextTrackObserver;
class Timer;
class TimeRanges;
class ToggleEvent;
class TrackEvent;
class TransferDataDecoder;
class TransferDataEncoder;
class TraversableNavigable;
class UserActivation;
class ValidityState;
class VideoTrack;
class VideoTrackList;
class WebWorkerClient;
class Window;
class WindowEnvironmentSettingsObject;
class WindowProxy;
class Worker;
class WorkerAgentParent;
class WorkerDebugConsoleClient;
class WorkerEnvironmentSettingsObject;
class WorkerGlobalScope;
class WorkerLocation;
class WorkerNavigator;
class XMLSerializer;

enum class AllowMultipleFiles;
enum class RequireWellFormed;
enum class SandboxingFlagSet;

struct Agent;
struct DeserializedTransferRecord;
struct EmbedderPolicy;
struct Environment;
struct EnvironmentSettingsObject;
struct NavigationParams;
struct OpenerPolicy;
struct OpenerPolicyEnforcementResult;
struct PaintConfig;
struct PolicyContainer;
struct POSTResource;
struct ScrollOptions;
struct ScrollToOptions;
struct SerializedFormData;
struct SerializedPolicyContainer;
struct SerializedTransferRecord;
struct SourceSnapshotParams;
struct StructuredSerializeOptions;
struct SyntheticRealmSettings;
struct ToggleTaskTracker;

}

namespace Web::HighResolutionTime {

class Performance;

}

namespace Web::IndexedDB {

class Database;
class IDBCursor;
class IDBCursorWithValue;
class IDBDatabase;
class IDBDatabaseObserver;
class IDBFactory;
class IDBIndex;
class IDBKeyRange;
class IDBObjectStore;
class IDBOpenDBRequest;
class IDBRecord;
class IDBRequest;
class IDBRequestObserver;
class IDBTransaction;
class IDBTransactionObserver;
class IDBVersionChangeEvent;
class Index;
class ObjectStore;
class RequestList;

}

namespace Web::Internals {

class FakeXRDevice;
class Internals;
class InternalGamepad;
class WebUI;
class XRTest;

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
class SVGSVGBox;
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
class MathMLMiElement;
class MathMLMspaceElement;

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

namespace Web::NotificationsAPI {

struct NotificationAction;
struct NotificationOptions;

class Notification;

}

namespace Web::Painting {

class AudioPaintable;
class CheckBoxPaintable;
class FieldSetPaintable;
class LabelablePaintable;
class MediaPaintable;
class Paintable;
class PaintableBox;
class PaintableWithLines;
class ScrollStateSnapshot;
class StackingContext;
class TextPaintable;
class VideoPaintable;
class ViewportPaintable;

enum class PaintPhase;
enum class ShouldAntiAlias : bool;

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

namespace Web::Serial {

class Serial;
class SerialPort;

struct SerialPortFilter;
struct SerialPortRequestOptions;
struct SerialOptions;
struct SerialOutputSignals;
struct SerialInputSignals;
struct SerialPortInfo;

}

namespace Web::ServiceWorker {

class ServiceWorker;
class ServiceWorkerContainer;
class ServiceWorkerRegistration;

}

namespace Web::Speech {

class SpeechGrammar;
class SpeechGrammarList;
class SpeechRecognition;
class SpeechRecognitionAlternative;
class SpeechRecognitionEvent;
class SpeechRecognitionPhrase;
class SpeechRecognitionResult;
class SpeechRecognitionResultList;
class SpeechSynthesis;
class SpeechSynthesisUtterance;
class SpeechSynthesisVoice;

}

namespace Web::Streams {

class ByteLengthQueuingStrategy;
class CountQueuingStrategy;
class ReadableByteStreamController;
class ReadableStream;
class ReadableStreamAsyncIterator;
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
class StorageBottle;
class StorageBucket;
class StorageManager;
class StorageShed;
class StorageShelf;

struct StorageEndpoint;

}

namespace Web::SVG {

class Path;
class SVGAnimatedEnumeration;
class SVGAnimatedInteger;
class SVGAnimatedLength;
class SVGAnimatedLengthList;
class SVGAnimatedNumber;
class SVGAnimatedNumberList;
class SVGAnimatedRect;
class SVGAnimationElement;
class SVGCircleElement;
class SVGClipPathElement;
class SVGComponentTransferFunctionElement;
class SVGDecodedImageData;
class SVGDefsElement;
class SVGDescElement;
class SVGElement;
class SVGEllipseElement;
class SVGFEBlendElement;
class SVGFEColorMatrixElement;
class SVGFEComponentTransferElement;
class SVGFECompositeElement;
class SVGFEFloodElement;
class SVGFEFuncAElement;
class SVGFEFuncBElement;
class SVGFEFuncGElement;
class SVGFEFuncRElement;
class SVGFEGaussianBlurElement;
class SVGFEImageElement;
class SVGFEMorphologyElement;
class SVGFETurbulenceElement;
class SVGFilterElement;
class SVGFitToViewBox;
class SVGForeignObjectElement;
class SVGGeometryElement;
class SVGGraphicsElement;
class SVGImageElement;
class SVGLength;
class SVGLengthList;
class SVGLineElement;
class SVGMaskElement;
class SVGMetadataElement;
class SVGNumber;
class SVGNumberList;
class SVGPathElement;
class SVGPatternElement;
class SVGPolygonElement;
class SVGPolylineElement;
class SVGRectElement;
class SVGScriptElement;
class SVGSVGElement;
class SVGTitleElement;
class SVGTransformList;
class SVGViewElement;

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

namespace Web::ViewTransition {

class ViewTransition;

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
class ControlMessageQueue;
class DynamicsCompressorNode;
class GainNode;
class OfflineAudioCompletionEvent;
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
class WebGLQuery;
class WebGLRenderbuffer;
class WebGLRenderingContext;
class WebGLSampler;
class WebGLShader;
class WebGLShaderPrecisionFormat;
class WebGLSync;
class WebGLTexture;
class WebGLTransformFeedback;
class WebGLUniformLocation;
class WebGLVertexArrayObject;

}

namespace Web::WebGL::Extensions {

class ANGLEInstancedArrays;
class EXTBlendMinMax;
class EXTColorBufferFloat;
class EXTRenderSnorm;
class EXTTextureFilterAnisotropic;
class EXTTextureNorm16;
class OESElementIndexUint;
class OESStandardDerivatives;
class OESVertexArrayObject;
class WebGLCompressedTextureS3tc;
class WebGLCompressedTextureS3tcSrgb;
class WebGLDrawBuffers;
class WebGLVertexArrayObjectOES;

}

namespace Web::WebIDL {

class ArrayBufferView;
class BufferSource;
class CallbackType;
class DOMException;
class ObservableArray;

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

namespace Web::WebXR {

class XRSession;
class XRSessionEvent;
class XRSystem;

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
WEB_API ErrorOr<void> encode(Encoder&, Web::UniqueNodeID const&);

template<>
WEB_API ErrorOr<Web::UniqueNodeID> decode(Decoder&);

}

namespace Web::TrustedTypes {

class TrustedHTML;
class TrustedScript;
class TrustedScriptURL;
class TrustedTypePolicy;
class TrustedTypePolicyFactory;
struct TrustedTypePolicyOptions;

}

namespace Web::XPath {

class XPathEvaluator;
class XPathExpression;
class XPathNSResolver;
class XPathResult;

}
