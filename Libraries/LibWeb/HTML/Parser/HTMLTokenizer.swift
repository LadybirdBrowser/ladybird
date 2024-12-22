/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Collections
import Foundation
@_exported import WebCxx

extension Swift.String {
    public init?(decoding: AK.StringView, as: AK.StringView) {
        let maybe_decoded = Web.HTML.decode_to_utf8(decoding, `as`)
        if maybe_decoded.hasValue {
            self.init(akString: maybe_decoded.value!)
        } else {
            return nil
        }
    }

    public mutating func takeString() -> Swift.String {
        let result = self
        self = ""
        return result
    }
}

public class HTMLTokenizer {

    public enum State {
        case Data
        case RCDATA
        case RAWTEXT
        case ScriptData
        case PLAINTEXT
        case TagOpen
        case EndTagOpen
        case TagName
        case RCDATALessThanSign
        case RCDATAEndTagOpen
        case RCDATAEndTagName
        case RAWTEXTLessThanSign
        case RAWTEXTEndTagOpen
        case RAWTEXTEndTagName
        case ScriptDataLessThanSign
        case ScriptDataEndTagOpen
        case ScriptDataEndTagName
        case ScriptDataEscapeStart
        case ScriptDataEscapeStartDash
        case ScriptDataEscaped
        case ScriptDataEscapedDash
        case ScriptDataEscapedDashDash
        case ScriptDataEscapedLessThanSign
        case ScriptDataEscapedEndTagOpen
        case ScriptDataEscapedEndTagName
        case ScriptDataDoubleEscapeStart
        case ScriptDataDoubleEscaped
        case ScriptDataDoubleEscapedDash
        case ScriptDataDoubleEscapedDashDash
        case ScriptDataDoubleEscapedLessThanSign
        case ScriptDataDoubleEscapeEnd
        case BeforeAttributeName
        case AttributeName
        case AfterAttributeName
        case BeforeAttributeValue
        case AttributeValueDoubleQuoted
        case AttributeValueSingleQuoted
        case AttributeValueUnquoted
        case AfterAttributeValueQuoted
        case SelfClosingStartTag
        case BogusComment
        case MarkupDeclarationOpen
        case CommentStart
        case CommentStartDash
        case Comment
        case CommentLessThanSign
        case CommentLessThanSignBang
        case CommentLessThanSignBangDash
        case CommentLessThanSignBangDashDash
        case CommentEndDash
        case CommentEnd
        case CommentEndBang
        case DOCTYPE
        case BeforeDOCTYPEName
        case DOCTYPEName
        case AfterDOCTYPEName
        case AfterDOCTYPEPublicKeyword
        case BeforeDOCTYPEPublicIdentifier
        case DOCTYPEPublicIdentifierDoubleQuoted
        case DOCTYPEPublicIdentifierSingleQuoted
        case AfterDOCTYPEPublicIdentifier
        case BetweenDOCTYPEPublicAndSystemIdentifiers
        case AfterDOCTYPESystemKeyword
        case BeforeDOCTYPESystemIdentifier
        case DOCTYPESystemIdentifierDoubleQuoted
        case DOCTYPESystemIdentifierSingleQuoted
        case AfterDOCTYPESystemIdentifier
        case BogusDOCTYPE
        case CDATASection
        case CDATASectionBracket
        case CDATASectionEnd
        case CharacterReference
        case NamedCharacterReference
        case AmbiguousAmpersand
        case NumericCharacterReference
        case HexadecimalCharacterReferenceStart
        case DecimalCharacterReferenceStart
        case HexadecimalCharacterReference
        case DecimalCharacterReference
        case NumericCharacterReferenceEnd
    }

    private var input = Swift.String()
    private var cursor: Swift.String.Index
    private var previousCursor: Swift.String.Index

    public private(set) var state = State.Data
    private var returnState = State.Data

    private var currentToken = HTMLToken()
    private var queuedTokens = Deque<HTMLToken>()

    private var currentBuilder = Swift.String()
    private var temporaryBuffer = Swift.String()
    private var lastStartTagName: Swift.String? = nil
    private var currentTokensAttributes: [HTMLToken.Attribute]? = nil
    private var currentAttribute: HTMLToken.Attribute? = nil
    private var characterReferenceCode: Int = 0

    private var aborted = false
    private var hasEmittedEOF = false

    // https://infra.spec.whatwg.org/#noncharacter
    // A noncharacter is a code point that is in the range U+FDD0 to U+FDEF, inclusive,
    // or U+FFFE, U+FFFF, U+1FFFE, U+1FFFF, U+2FFFE, U+2FFFF, U+3FFFE, U+3FFFF, U+4FFFE, U+4FFFF,
    // U+5FFFE, U+5FFFF, U+6FFFE, U+6FFFF, U+7FFFE, U+7FFFF, U+8FFFE, U+8FFFF, U+9FFFE, U+9FFFF,
    // U+AFFFE, U+AFFFF, U+BFFFE, U+BFFFF, U+CFFFE, U+CFFFF, U+DFFFE, U+DFFFF, U+EFFFE, U+EFFFF,
    // U+FFFFE, U+FFFFF, U+10FFFE, or U+10FFFF.
    static private var nonCharacter = CharacterSet(charactersIn: Unicode.Scalar(0xFDD0)!...Unicode.Scalar(0xFDEF)!)
        .union(CharacterSet(charactersIn: "\u{FFFE}"..."\u{FFFF}"))
        .union(CharacterSet(charactersIn: "\u{1FFFE}"..."\u{1FFFF}"))
        .union(CharacterSet(charactersIn: "\u{2FFFE}"..."\u{2FFFF}"))
        .union(CharacterSet(charactersIn: "\u{3FFFE}"..."\u{3FFFF}"))
        .union(CharacterSet(charactersIn: "\u{4FFFE}"..."\u{4FFFF}"))
        .union(CharacterSet(charactersIn: "\u{5FFFE}"..."\u{5FFFF}"))
        .union(CharacterSet(charactersIn: "\u{6FFFE}"..."\u{6FFFF}"))
        .union(CharacterSet(charactersIn: "\u{7FFFE}"..."\u{7FFFF}"))
        .union(CharacterSet(charactersIn: "\u{8FFFE}"..."\u{8FFFF}"))
        .union(CharacterSet(charactersIn: "\u{9FFFE}"..."\u{9FFFF}"))
        .union(CharacterSet(charactersIn: "\u{AFFFE}"..."\u{AFFFF}"))
        .union(CharacterSet(charactersIn: "\u{BFFFE}"..."\u{BFFFF}"))
        .union(CharacterSet(charactersIn: "\u{CFFFE}"..."\u{CFFFF}"))
        .union(CharacterSet(charactersIn: "\u{DFFFE}"..."\u{DFFFF}"))
        .union(CharacterSet(charactersIn: "\u{EFFFE}"..."\u{EFFFF}"))
        .union(CharacterSet(charactersIn: "\u{FFFFE}"..."\u{FFFFF}"))
        .union(CharacterSet(charactersIn: "\u{10FFFE}"..."\u{10FFFF}"))

    // https://infra.spec.whatwg.org/#ascii-whitespace
    static private var asciiWhitespace = CharacterSet(charactersIn: "\t\n\u{000C}\u{000D} ")

    // https://infra.spec.whatwg.org/#c0-control
    static private var c0Control = CharacterSet(charactersIn: "\u{0000}"..."\u{001F}")

    // https://infra.spec.whatwg.org/#control
    static private var control = c0Control.union(CharacterSet(charactersIn: "\u{007F}"..."\u{009F}"))

    // IMPLEMENTATION DEFINED: Used for the numeric character reference end state
    static private var controlNotAsciiWhitespace = control.subtracting(asciiWhitespace)

    // https://infra.spec.whatwg.org/#ascii-digit
    static private var asciiDigit = CharacterSet(charactersIn: "0123456789")

    // https://infra.spec.whatwg.org/#ascii-upper-hex-digit
    static private var asciiUpperHexDigit = CharacterSet(charactersIn: "ABCDEF")

    // https://infra.spec.whatwg.org/#ascii-lower-hex-digit
    static private var asciiLowerHexDigit = CharacterSet(charactersIn: "abcdef")

    // https://infra.spec.whatwg.org/#ascii-hex-digit
    static private var asciiHexDigit = asciiUpperHexDigit.union(asciiLowerHexDigit)

    // https://infra.spec.whatwg.org/#ascii-upper-alpha
    static private var asciiUpperAlpha = CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZ")

    // https://infra.spec.whatwg.org/#ascii-lower-alpha
    static private var asciiLowerAlpha = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyz")

    // https://infra.spec.whatwg.org/#ascii-upper-alpha
    static private var asciiAlpha = asciiUpperAlpha.union(asciiLowerAlpha)

    // https://infra.spec.whatwg.org/#ascii-alphanumeric
    static private var asciiAlphanumeric = asciiAlpha.union(asciiDigit)

    static private var characterReferenceControlCodeMapping: [Int: Unicode.Scalar] =
        [
            0x80: Unicode.Scalar(0x20AC)!,  // €
            0x82: Unicode.Scalar(0x201A)!,  // ‚
            0x83: Unicode.Scalar(0x0192)!,  // ƒ
            0x84: Unicode.Scalar(0x201E)!,  // „
            0x85: Unicode.Scalar(0x2026)!,  // …
            0x86: Unicode.Scalar(0x2020)!,  // †
            0x87: Unicode.Scalar(0x2021)!,  // ‡
            0x88: Unicode.Scalar(0x02C6)!,  // ˆ
            0x89: Unicode.Scalar(0x2030)!,  // ‰
            0x8A: Unicode.Scalar(0x0160)!,  // Š
            0x8B: Unicode.Scalar(0x2039)!,  // ‹
            0x8C: Unicode.Scalar(0x0152)!,  // Œ
            0x8E: Unicode.Scalar(0x017D)!,  // Ž
            0x91: Unicode.Scalar(0x2018)!,  // ‘
            0x92: Unicode.Scalar(0x2019)!,  // ’
            0x93: Unicode.Scalar(0x201C)!,  // “
            0x94: Unicode.Scalar(0x201D)!,  // ”
            0x95: Unicode.Scalar(0x2022)!,  // •
            0x96: Unicode.Scalar(0x2013)!,  // –
            0x97: Unicode.Scalar(0x2014)!,  // —
            0x98: Unicode.Scalar(0x02DC)!,  // ˜
            0x99: Unicode.Scalar(0x2122)!,  // ™
            0x9A: Unicode.Scalar(0x0161)!,  // š
            0x9B: Unicode.Scalar(0x203A)!,  // ›
            0x9C: Unicode.Scalar(0x0153)!,  // œ
            0x9E: Unicode.Scalar(0x017E)!,  // ž
            0x9F: Unicode.Scalar(0x0178)!,  // Ÿ
        ]

    public init() {
        self.cursor = self.input.startIndex
        self.previousCursor = self.input.startIndex
    }
    public init?(input: AK.StringView, encoding: AK.StringView) {
        if let string = Swift.String(decoding: input, as: encoding) {
            self.input = string
        } else {
            return nil
        }
        self.cursor = self.input.startIndex
        self.previousCursor = self.input.startIndex
    }

    public convenience init?(input: AK.StringView) {
        self.init(input: input, encoding: "UTF-8")
    }

    public func abort() {
        self.aborted = true
    }

    func skip(_ count: Int) {
        self.previousCursor = self.cursor
        self.cursor = self.input.index(self.cursor, offsetBy: count, limitedBy: self.input.endIndex) ?? input.endIndex
    }

    func peekCodePoint(_ offset: Int = 0) -> Character? {
        guard let index = self.input.index(self.cursor, offsetBy: offset, limitedBy: self.input.index(before: self.input.endIndex)) else {
            return nil
        }
        return self.input[index]
    }

    func peekNext(count: Int) -> Swift.Substring? {
        guard let endIndex = self.input.index(self.cursor, offsetBy: count, limitedBy: self.input.index(before: self.input.endIndex)) else {
            return nil
        }
        return self.input[self.cursor..<endIndex]
    }

    func nextCodePoint() -> Character? {
        guard self.cursor < self.input.endIndex else {
            return nil
        }

        // https://html.spec.whatwg.org/multipage/parsing.html#preprocessing-the-input-stream:tokenization
        // https://infra.spec.whatwg.org/#normalize-newlines
        var codePoint: Character
        if let peeked = peekCodePoint(), let peekedNext = peekCodePoint(1), peeked == "\r", peekedNext == "\n" {
            // replace every U+000D CR U+000A LF code point pair with a single U+000A LF code point,
            skip(2)
            codePoint = "\n"
        } else if let peeked = peekCodePoint(), peeked == "\r" {
            // replace every remaining U+000D CR code point with a U+000A LF code point.
            skip(1)
            codePoint = "\n"
        } else {
            skip(1)
            codePoint = self.input[self.previousCursor]
        }
        return codePoint
    }

    func restoreCursorToPrevious() {
        self.cursor = self.previousCursor
    }

    func createNewToken(_ token: HTMLToken) {
        self.currentToken = token
        if self.currentToken.isTag() {
            self.currentTokensAttributes = []
        }
        // FIXME: Assign Position
    }

    enum AttributeStringBehavior {
        case SetName
        case SetValue
        case IgnoreString
    }
    func finalizeCurrentAttribute(_ behavior: AttributeStringBehavior) {
        precondition(self.currentAttribute != nil && self.currentTokensAttributes != nil)
        switch behavior {
        case .SetName:
            self.currentAttribute!.localName = self.currentBuilder.takeString()
        case .SetValue:
            self.currentAttribute!.value = self.currentBuilder.takeString()
        case .IgnoreString:
            _ = self.currentBuilder.takeString()
        }
        self.currentTokensAttributes!.append(self.currentAttribute!)
        self.currentAttribute = nil
    }

    enum NextTokenState {
        case Emit(token: HTMLToken?)
        case SwitchTo
        case Reconsume(inputCharacter: Character?)
        case ReprocessQueue
        case Continue
    }

    public func nextToken(stopAtInsertionPoint: Bool = false) -> HTMLToken? {

        let processQueue = { () -> HTMLToken?? in
            if let token = self.queuedTokens.popFirst() {
                return token
            }
            return self.aborted ? Optional(nil) : nil
        }

        if let maybeToken = processQueue() {
            return maybeToken
        }

        var nextInputCharacter: Character? = nil
        while true {
            // FIXME: Handle insertion point
            switch nextTokenImpl(nextInputCharacter) {
            case .Emit(let token):
                return token
            case .SwitchTo, .Continue:
                nextInputCharacter = nil
                break
            case .Reconsume(let character):
                nextInputCharacter = character
                break
            case .ReprocessQueue:
                if let maybeToken = processQueue() {
                    return maybeToken
                }
                nextInputCharacter = nil
                break
            }
        }
    }

    func continueInCurrentState() -> NextTokenState {
        return .Continue
    }

    func switchTo(_ state: State) -> NextTokenState {
        self.state = state
        return .SwitchTo
    }

    func reconsume(_ character: Character?, `in` state: State) -> NextTokenState {
        self.state = state
        return .Reconsume(inputCharacter: character)
    }

    func switchToReturnState() -> NextTokenState {
        self.state = self.returnState
        return .ReprocessQueue
    }

    func reconsumeInReturnState(_ character: Character?) -> NextTokenState {
        self.state = self.returnState
        if character != nil {
            restoreCursorToPrevious()
        }
        return .ReprocessQueue
    }

    func switchToAndEmitCurrentToken(_ state: State) -> NextTokenState {
        self.state = state
        if self.currentToken.isTag() {
            self.currentToken.attributes = self.currentTokensAttributes ?? []
            self.currentTokensAttributes = nil
        }
        self.queuedTokens.append(self.currentToken)
        self.currentToken = HTMLToken()
        return .Emit(token: self.queuedTokens.popFirst()!)
    }

    func switchToAndEmitCharacter(_ state: State, character: Character) -> NextTokenState {
        self.state = state
        return emitCharacter(character)
    }

    func emitCharacterAndReconsume(_ character: Character, `in`: State, currentInputCharacter: Character?) -> NextTokenState {
        self.queuedTokens.append(HTMLToken(type: .Character(codePoint: character)))
        self.state = `in`
        return .Reconsume(inputCharacter: currentInputCharacter)
    }

    func emitEOF() -> NextTokenState {
        if self.hasEmittedEOF {
            return .Emit(token: nil)
        }
        self.hasEmittedEOF = true
        createNewToken(HTMLToken(type: .EndOfFile))
        self.queuedTokens.append(self.currentToken)
        self.currentToken = HTMLToken()
        return .Emit(token: self.queuedTokens.popFirst()!)
    }

    func emitCurrentTokenFollowedByEOF() -> NextTokenState {
        precondition(!self.hasEmittedEOF)
        if self.currentToken.isTag() {
            self.currentToken.attributes = self.currentTokensAttributes ?? []
            self.currentTokensAttributes = nil
        }
        self.queuedTokens.append(self.currentToken)
        self.currentToken = HTMLToken()
        return emitEOF()
    }

    func emitCharacter(_ character: Character) -> NextTokenState {
        createNewToken(HTMLToken(type: .Character(codePoint: character)))
        self.queuedTokens.append(self.currentToken)
        self.currentToken = HTMLToken()
        return .Emit(token: self.queuedTokens.popFirst()!)
    }

    func flushCodepointsConsumedAsACharacterReference() {
        if consumedAsPartOfAnAttribute() {
            self.currentBuilder += self.temporaryBuffer.takeString()
        } else {
            for codePoint in self.temporaryBuffer.takeString() {
                self.queuedTokens.append(HTMLToken(type: .Character(codePoint: codePoint)))
            }
        }
    }

    func consumedAsPartOfAnAttribute() -> Bool {
        return self.returnState == .AttributeValueDoubleQuoted || self.returnState == .AttributeValueSingleQuoted || self.returnState == .AttributeValueUnquoted
    }

    func isAppropriateEndTagToken(_ token: HTMLToken) -> Bool {
        guard case let .EndTag(endTagName, _, _, _) = token.type else {
            preconditionFailure("isAppropriateEndTagToken called with non-end-tag token")
        }
        if let startTagName = self.lastStartTagName {
            return startTagName == endTagName
        } else {
            return false
        }
    }

    func nextTokenImpl(_ nextInputCharacter: Character? = nil) -> NextTokenState {
        let dontConsumeNextInputCharacter = {
            self.restoreCursorToPrevious()
        }
        let _ = dontConsumeNextInputCharacter

        // Handle reconsume by passing the character around in the state enum
        let currentInputCharacter = nextInputCharacter ?? nextCodePoint()

        switch self.state {
        // 13.2.5.1 Data state, https://html.spec.whatwg.org/multipage/parsing.html#data-state
        case .Data:
            precondition(currentTokensAttributes == nil)
            switch currentInputCharacter {
            case "&":
                self.returnState = .Data
                return switchTo(.CharacterReference)
            case "<":
                return switchTo(.TagOpen)
            case "\0":
                // FIXME: log_parse_error()
                return emitCharacter("\u{FFFD}")
            case nil:
                return emitEOF()
            default:
                return emitCharacter(currentInputCharacter!)
            }

        // 13.2.5.2 RCDATA state, https://html.spec.whatwg.org/multipage/parsing.html#rcdata-state
        case .RCDATA:
            switch currentInputCharacter {
            case "&":
                self.returnState = .RCDATA
                return switchTo(.CharacterReference)
            case "<":
                return switchTo(.RCDATALessThanSign)
            case "\0":
                // FIXME: log_parse_error()
                return emitCharacter("\u{FFFD}")
            case nil:
                return emitEOF()
            default:
                return emitCharacter(currentInputCharacter!)
            }

        // 13.2.5.3. RAWTEXT state, https://html.spec.whatwg.org/multipage/parsing.html#rawtext-state
        case .RAWTEXT:
            switch currentInputCharacter {
            case "<":
                return switchTo(.RAWTEXTLessThanSign)
            case "\0":
                // FIXME: log_parse_error()
                return emitCharacter("\u{FFFD}")
            case nil:
                return emitEOF()
            default:
                return emitCharacter(currentInputCharacter!)
            }
        // 13.2.5.4 Script data state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-state
        case .ScriptData:
            switch currentInputCharacter {
            case "<":
                return switchTo(.ScriptDataLessThanSign)
            case "\0":
                // FIXME: log_parse_error()
                return emitCharacter("\u{FFFD}")
            case nil:
                return emitEOF()
            default:
                return emitCharacter(currentInputCharacter!)
            }
        // 13.2.5.5 PLAINTEXT state, https://html.spec.whatwg.org/multipage/parsing.html#plaintext-state
        case .PLAINTEXT:
            switch currentInputCharacter {
            case "\0":
                // FIXME: log_parse_error()
                return emitCharacter("\u{FFFD}")
            case nil:
                return emitEOF()
            default:
                return emitCharacter(currentInputCharacter!)
            }
        // 13.2.5.6 Tag open state https://html.spec.whatwg.org/multipage/parsing.html#tag-open-state
        case .TagOpen:
            switch currentInputCharacter {
            case "!":
                return switchTo(.MarkupDeclarationOpen)
            case "/":
                return switchTo(.EndTagOpen)
            case let c? where HTMLTokenizer.asciiAlpha.contains(c.unicodeScalars.first!):
                createNewToken(HTMLToken(type: .StartTag(tagName: "")))
                return reconsume(currentInputCharacter!, in: .TagName)
            case "?":
                // FIXME: log_parse_error()
                createNewToken(HTMLToken(type: .Comment(data: "")))
                return reconsume(currentInputCharacter!, in: .BogusComment)
            case nil:
                // FIXME: log_parse_error()
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
                return emitEOF()
            default:
                // FIXME: log_parse_error()
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
                return reconsume(currentInputCharacter!, in: .Data)
            }
        // 13.2.5.7 End tag open state, https://html.spec.whatwg.org/multipage/parsing.html#end-tag-open-state
        case .EndTagOpen:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiAlpha.contains(c.unicodeScalars.first!):
                createNewToken(HTMLToken(type: .EndTag(tagName: "")))
                return reconsume(currentInputCharacter!, in: .TagName)
            default:
                return emitEOF()
            }
        // 13.2.5.8 Tag name state, https://html.spec.whatwg.org/multipage/parsing.html#tag-name-state
        case .TagName:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                self.currentToken.tagName = self.currentBuilder.takeString()
                return switchTo(.BeforeAttributeName)
            case "/":
                self.currentToken.tagName = self.currentBuilder.takeString()
                return switchTo(.SelfClosingStartTag)
            case ">":
                self.currentToken.tagName = self.currentBuilder.takeString()
                return switchToAndEmitCurrentToken(.Data)
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                currentBuilder.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                return continueInCurrentState()
            case "\0":
                // FIXME: log_parse_error()
                currentBuilder += "\u{FFFD}"
                return continueInCurrentState()
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.9 RCDATA less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#rcdata-less-than-sign-state
        case .RCDATALessThanSign:
            switch currentInputCharacter {
            case "/":
                self.temporaryBuffer = ""
                return switchTo(.RCDATAEndTagOpen)
            default:
                return emitCharacterAndReconsume("<", in: .RCDATA, currentInputCharacter: currentInputCharacter)
            }
        // 13.2.5.10 RCDATA end tag open state, https://html.spec.whatwg.org/multipage/parsing.html#rcdata-end-tag-open-state
        case .RCDATAEndTagOpen:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiAlpha.contains(c.unicodeScalars.first!):
                createNewToken(HTMLToken(type: .EndTag(tagName: "")))
                return reconsume(currentInputCharacter!, in: .RCDATAEndTagName)
            default:
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "/")))
                return reconsume(currentInputCharacter, in: .RCDATA)
            }
        // 13.2.5.11 RCDATA end tag name state, https://html.spec.whatwg.org/multipage/parsing.html#rcdata-end-tag-name-state
        case .RCDATAEndTagName:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchTo(.BeforeAttributeName)
                }
                break
            case "/":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchTo(.SelfClosingStartTag)
                }
                break
            case ">":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchToAndEmitCurrentToken(.Data)
                }
                break
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                self.temporaryBuffer.append(c)
                return continueInCurrentState()
            case let c? where HTMLTokenizer.asciiLowerAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(c)
                self.temporaryBuffer.append(c)
                return continueInCurrentState()
            default:
                break
            }

            // First three steps fall through to the "anything else" block
            self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
            self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "/")))
            // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
            self.currentBuilder = ""
            for codePoint in self.temporaryBuffer {
                self.queuedTokens.append(HTMLToken(type: .Character(codePoint: codePoint)))
            }
            return reconsume(currentInputCharacter, in: .RCDATA)
        // 13.2.5.12 RAWTEXT less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#rawtext-less-than-sign-state
        case .RAWTEXTLessThanSign:
            switch currentInputCharacter {
            case "/":
                self.temporaryBuffer = ""
                return switchTo(.RAWTEXTEndTagOpen)
            default:
                return emitCharacterAndReconsume("<", in: .RAWTEXT, currentInputCharacter: currentInputCharacter)
            }
        // 13.2.5.13 RAWTEXT end tag open state, https://html.spec.whatwg.org/multipage/parsing.html#rawtext-end-tag-open-state
        case .RAWTEXTEndTagOpen:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiAlpha.contains(c.unicodeScalars.first!):
                createNewToken(HTMLToken(type: .EndTag(tagName: "")))
                return reconsume(currentInputCharacter!, in: .RAWTEXTEndTagName)
            default:
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "/")))
                return reconsume(currentInputCharacter, in: .RAWTEXT)
            }
        // 13.2.5.14 RAWTEXT end tag name state, https://html.spec.whatwg.org/multipage/parsing.html#rawtext-end-tag-name-state
        case .RAWTEXTEndTagName:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchTo(.BeforeAttributeName)
                }
                break
            case "/":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchTo(.SelfClosingStartTag)
                }
                break
            case ">":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchToAndEmitCurrentToken(.Data)
                }
                break
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                self.temporaryBuffer.append(c)
                return continueInCurrentState()
            case let c? where HTMLTokenizer.asciiLowerAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(c)
                self.temporaryBuffer.append(c)
                return continueInCurrentState()
            default:
                break
            }

            // First three steps fall through to the "anything else" block
            self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
            self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "/")))
            // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
            self.currentBuilder = ""
            for codePoint in self.temporaryBuffer {
                self.queuedTokens.append(HTMLToken(type: .Character(codePoint: codePoint)))
            }
            return reconsume(currentInputCharacter, in: .RAWTEXT)
        // 13.2.5.15 Script data less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-less-than-sign-state
        case .ScriptDataLessThanSign:
            switch currentInputCharacter {
            case "/":
                self.temporaryBuffer = ""
                return switchTo(.ScriptDataEndTagOpen)
            case "!":
                self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
                self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "!")))
                return switchTo(.ScriptDataEscapeStart)
            default:
                return emitCharacterAndReconsume("<", in: .ScriptData, currentInputCharacter: currentInputCharacter)
            }
        // 13.2.5.16 Script data end tag open state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-end-tag-open-state
        case .ScriptDataEndTagOpen:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiAlpha.contains(c.unicodeScalars.first!):
                createNewToken(HTMLToken(type: .EndTag(tagName: "")))
                return reconsume(currentInputCharacter!, in: .ScriptDataEndTagName)
            default:
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "/")))
                return reconsume(currentInputCharacter, in: .ScriptData)
            }
        // 13.2.5.17 Script data end tag name state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-end-tag-name-state
        case .ScriptDataEndTagName:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchTo(.BeforeAttributeName)
                }
                break
            case "/":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchTo(.SelfClosingStartTag)
                }
                break
            case ">":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchToAndEmitCurrentToken(.Data)
                }
                break
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                self.temporaryBuffer.append(c)
                return continueInCurrentState()
            case let c? where HTMLTokenizer.asciiLowerAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(c)
                self.temporaryBuffer.append(c)
                return continueInCurrentState()
            default:
                break
            }

            // First three steps fall through to the "anything else" block
            self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
            self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "/")))
            // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
            self.currentBuilder = ""
            for codePoint in self.temporaryBuffer {
                self.queuedTokens.append(HTMLToken(type: .Character(codePoint: codePoint)))
            }
            return reconsume(currentInputCharacter, in: .ScriptData)
        // 13.2.5.18 Script data escape start state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escape-start-state
        case .ScriptDataEscapeStart:
            switch currentInputCharacter {
            case "-":
                return switchToAndEmitCharacter(.ScriptDataEscapeStartDash, character: "-")
            default:
                return reconsume(currentInputCharacter, in: .ScriptData)
            }
        // 13.2.5.19 Script data escape start dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escape-start-dash-state
        case .ScriptDataEscapeStartDash:
            switch currentInputCharacter {
            case "-":
                return switchToAndEmitCharacter(.ScriptDataEscapedDashDash, character: "-")
            default:
                return reconsume(currentInputCharacter, in: .ScriptData)
            }
        // 13.2.5.20 Script data escaped state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-state
        case .ScriptDataEscaped:
            switch currentInputCharacter {
            case "-":
                return switchToAndEmitCharacter(.ScriptDataEscapedDash, character: "-")
            case "<":
                return switchTo(.ScriptDataEscapedLessThanSign)
            case "\0":
                // FIXME: log_parse_error()
                return emitCharacter("\u{FFFD}")
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                return emitCharacter(currentInputCharacter!)
            }
        // 13.2.5.21 Script data escaped dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-dash-state
        case .ScriptDataEscapedDash:
            switch currentInputCharacter {
            case "-":
                return switchToAndEmitCharacter(.ScriptDataEscapedDashDash, character: "-")
            case "<":
                return switchTo(.ScriptDataEscapedLessThanSign)
            case "\0":
                // FIXME: log_parse_error()
                return switchToAndEmitCharacter(.ScriptDataEscaped, character: "\u{FFFD}")
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                return switchToAndEmitCharacter(.ScriptDataEscaped, character: currentInputCharacter!)
            }
        // 13.2.5.22 Script data escaped dash dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-dash-dash-state
        case .ScriptDataEscapedDashDash:
            switch currentInputCharacter {
            case "-":
                return emitCharacter("-")
            case "<":
                return switchTo(.ScriptDataEscapedLessThanSign)
            case ">":
                return switchToAndEmitCharacter(.ScriptData, character: ">")
            case "\0":
                // FIXME: log_parse_error()
                return switchToAndEmitCharacter(.ScriptDataEscaped, character: "\u{FFFD}")
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                return switchToAndEmitCharacter(.ScriptDataEscaped, character: currentInputCharacter!)
            }
        // 13.2.5.23 Script data escaped less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-less-than-sign-state
        case .ScriptDataEscapedLessThanSign:
            switch currentInputCharacter {
            case "/":
                self.temporaryBuffer = ""
                return switchTo(.ScriptDataEscapedEndTagOpen)
            case let c? where HTMLTokenizer.asciiAlpha.contains(c.unicodeScalars.first!):
                self.temporaryBuffer = ""
                self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
                return reconsume(currentInputCharacter!, in: .ScriptDataDoubleEscapeStart)
            default:
                return emitCharacterAndReconsume("<", in: .ScriptDataEscaped, currentInputCharacter: currentInputCharacter)
            }
        // 13.2.5.24 Script data escaped end tag open state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-end-tag-open-state
        case .ScriptDataEscapedEndTagOpen:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiAlpha.contains(c.unicodeScalars.first!):
                createNewToken(HTMLToken(type: .EndTag(tagName: "")))
                return reconsume(currentInputCharacter!, in: .ScriptDataEscapedEndTagName)
            default:
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "/")))
                return reconsume(currentInputCharacter, in: .ScriptDataEscaped)
            }
        // 13.2.5.25 Script data escaped end tag name state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-end-tag-name-state
        case .ScriptDataEscapedEndTagName:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchTo(.BeforeAttributeName)
                }
                break
            case "/":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchTo(.SelfClosingStartTag)
                }
                break
            case ">":
                if self.isAppropriateEndTagToken(currentToken) {
                    return switchToAndEmitCurrentToken(.Data)
                }
                break
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                self.temporaryBuffer.append(c)
                return continueInCurrentState()
            case let c? where HTMLTokenizer.asciiLowerAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(c)
                self.temporaryBuffer.append(c)
                return continueInCurrentState()
            default:
                break
            }

            // First three steps fall through to the "anything else" block
            self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "<")))
            self.queuedTokens.append(HTMLToken(type: .Character(codePoint: "/")))
            // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
            self.currentBuilder = ""
            for codePoint in self.temporaryBuffer {
                self.queuedTokens.append(HTMLToken(type: .Character(codePoint: codePoint)))
            }
            return reconsume(currentInputCharacter, in: .ScriptDataEscaped)
        // 13.2.5.26 Script data double escape start state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escape-start-state
        case .ScriptDataDoubleEscapeStart:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ", "/", ">":
                if self.temporaryBuffer == "script" {
                    return switchToAndEmitCharacter(.ScriptDataDoubleEscaped, character: currentInputCharacter!)
                } else {
                    return switchToAndEmitCharacter(.ScriptDataEscaped, character: currentInputCharacter!)
                }
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                self.temporaryBuffer.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                return emitCharacter(currentInputCharacter!)
            case let c? where HTMLTokenizer.asciiLowerAlpha.contains(c.unicodeScalars.first!):
                self.temporaryBuffer.append(c)
                return emitCharacter(currentInputCharacter!)
            default:
                return reconsume(currentInputCharacter, in: .ScriptDataEscaped)
            }
        // 13.2.5.27 Script data double escaped state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-state
        case .ScriptDataDoubleEscaped:
            switch currentInputCharacter {
            case "-":
                return switchToAndEmitCharacter(.ScriptDataDoubleEscapedDash, character: "-")
            case "<":
                return switchTo(.ScriptDataDoubleEscapedLessThanSign)
            case "\0":
                // FIXME: log_parse_error()
                return emitCharacter("\u{FFFD}")
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                return emitCharacter(currentInputCharacter!)
            }
        // 13.2.5.28 Script data double escaped dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-dash-state
        case .ScriptDataDoubleEscapedDash:
            switch currentInputCharacter {
            case "-":
                return switchToAndEmitCharacter(.ScriptDataDoubleEscapedDashDash, character: "-")
            case "<":
                return switchTo(.ScriptDataDoubleEscapedLessThanSign)
            case "\0":
                // FIXME: log_parse_error()
                return switchToAndEmitCharacter(.ScriptDataDoubleEscaped, character: "\u{FFFD}")
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                return switchToAndEmitCharacter(.ScriptDataDoubleEscaped, character: currentInputCharacter!)
            }
        // 13.2.5.29 Script data double escaped dash dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-dash-dash-state
        case .ScriptDataDoubleEscapedDashDash:
            switch currentInputCharacter {
            case "-":
                return emitCharacter("-")
            case "<":
                return switchToAndEmitCharacter(.ScriptDataDoubleEscapedLessThanSign, character: "<")
            case ">":
                return switchToAndEmitCharacter(.ScriptData, character: ">")
            case "\0":
                // FIXME: log_parse_error()
                return switchToAndEmitCharacter(.ScriptDataDoubleEscaped, character: "\u{FFFD}")
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                return switchToAndEmitCharacter(.ScriptDataDoubleEscaped, character: currentInputCharacter!)
            }
        // 13.2.5.30 Script data double escaped less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-less-than-sign-state
        case .ScriptDataDoubleEscapedLessThanSign:
            switch currentInputCharacter {
            case "/":
                self.temporaryBuffer = ""
                return switchToAndEmitCharacter(.ScriptDataDoubleEscapeEnd, character: "/")
            default:
                return reconsume(currentInputCharacter, in: .ScriptDataDoubleEscaped)
            }
        // 13.2.5.31 Script data double escape end state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escape-end-state
        case .ScriptDataDoubleEscapeEnd:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ", "/", ">":
                if self.temporaryBuffer == "script" {
                    return switchToAndEmitCharacter(.ScriptDataEscaped, character: currentInputCharacter!)
                } else {
                    return switchToAndEmitCharacter(.ScriptDataDoubleEscaped, character: currentInputCharacter!)
                }
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                self.temporaryBuffer.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                return emitCharacter(currentInputCharacter!)
            case let c? where HTMLTokenizer.asciiLowerAlpha.contains(c.unicodeScalars.first!):
                self.temporaryBuffer.append(c)
                return emitCharacter(currentInputCharacter!)
            default:
                return reconsume(currentInputCharacter, in: .ScriptDataDoubleEscaped)
            }
        // 13.2.5.32 Before attribute name state, https://html.spec.whatwg.org/multipage/parsing.html#before-attribute-name-state
        case .BeforeAttributeName:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return continueInCurrentState()
            case "/", ">", nil:
                return reconsume(currentInputCharacter, in: .AfterAttributeName)
            case "=":
                // FIXME: log_parse_error()
                self.currentBuilder = Swift.String(currentInputCharacter!)
                self.currentAttribute = HTMLToken.Attribute(localName: "", value: "")
                return switchTo(.AttributeName)
            default:
                self.currentAttribute = HTMLToken.Attribute(localName: "", value: "")
                return reconsume(currentInputCharacter!, in: .AttributeName)
            }
        // 13.2.5.33 Attribute name state, https://html.spec.whatwg.org/multipage/parsing.html#attribute-name-state
        case .AttributeName:
            // FIXME: When the user agent leaves the attribute name state (and before emitting the tag token, if appropriate),
            //        the complete attribute's name must be compared to the other attributes on the same token;
            //        if there is already an attribute on the token with the exact same name, then this is a duplicate-attribute
            //        parse error and the new attribute must be removed from the token.
            // NOTE:  If an attribute is so removed from a token, it, and the value that gets associated with it, if any,
            //        are never subsequently used by the parser, and are therefore effectively discarded. Removing the attribute
            //        in this way does not change its status as the "current attribute" for the purposes of the tokenizer, however.
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ", "/", ">", nil:
                // FIXME: set name position
                self.currentAttribute!.localName = self.currentBuilder.takeString()
                return reconsume(currentInputCharacter, in: .AfterAttributeName)
            case "=":
                // FIXME: set name position
                self.currentAttribute!.localName = self.currentBuilder.takeString()
                return switchTo(.BeforeAttributeValue)
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                return continueInCurrentState()
            case "\0":
                // FIXME: log_parse_error()
                self.currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.34 After attribute name state, https://html.spec.whatwg.org/multipage/parsing.html#after-attribute-name-state
        case .AfterAttributeName:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return continueInCurrentState()
            case "/":
                self.finalizeCurrentAttribute(.SetName)
                return switchTo(.SelfClosingStartTag)
            case "=":
                self.finalizeCurrentAttribute(.SetName)
                return switchTo(.BeforeAttributeValue)
            case ">":
                self.finalizeCurrentAttribute(.SetName)
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.finalizeCurrentAttribute(.IgnoreString)
                return emitEOF()
            default:
                self.finalizeCurrentAttribute(.SetName)
                self.currentAttribute = HTMLToken.Attribute(localName: "", value: "")
                return reconsume(currentInputCharacter!, in: .AttributeName)
            }
        // 13.2.5.35 Before attribute value state, https://html.spec.whatwg.org/multipage/parsing.html#before-attribute-value-state
        case .BeforeAttributeValue:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return continueInCurrentState()
            case "\"":
                return switchTo(.AttributeValueDoubleQuoted)
            case "'":
                return switchTo(.AttributeValueSingleQuoted)
            case ">":
                // FIXME: log_parse_error()
                self.finalizeCurrentAttribute(.IgnoreString)
                return switchToAndEmitCurrentToken(.Data)
            default:
                return reconsume(currentInputCharacter, in: .AttributeValueUnquoted)
            }
        // 13.2.5.36 Attribute value (double-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#attribute-value-double-quoted-state
        case .AttributeValueDoubleQuoted:
            switch currentInputCharacter {
            case "\"":
                return switchTo(.AfterAttributeValueQuoted)
            case "&":
                self.returnState = .AttributeValueDoubleQuoted
                return switchTo(.CharacterReference)
            case "\0":
                // FIXME: log_parse_error()
                self.currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            case nil:
                // FIXME: log_parse_error()
                self.finalizeCurrentAttribute(.IgnoreString)
                return emitEOF()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.37 Attribute value (single-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#attribute-value-single-quoted-state
        case .AttributeValueSingleQuoted:
            switch currentInputCharacter {
            case "'":
                return switchTo(.AfterAttributeValueQuoted)
            case "&":
                self.returnState = .AttributeValueSingleQuoted
                return switchTo(.CharacterReference)
            case "\0":
                // FIXME: log_parse_error()
                self.currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.38 Attribute value (unquoted) state, https://html.spec.whatwg.org/multipage/parsing.html#attribute-value-unquoted-state
        case .AttributeValueUnquoted:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                self.finalizeCurrentAttribute(.SetValue)
                return switchTo(.BeforeAttributeName)
            case "&":
                self.returnState = .AttributeValueUnquoted
                return switchTo(.CharacterReference)
            case ">":
                self.finalizeCurrentAttribute(.SetValue)
                return switchToAndEmitCurrentToken(.Data)
            case "\0":
                // FIXME: log_parse_error()
                self.currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            case "\"", "'", "<", "=", "`":
                // FIXME: log_parse_error()
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            case nil:
                // FIXME: log_parse_error()
                self.finalizeCurrentAttribute(.IgnoreString)
                return emitEOF()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.39 After attribute value (quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#after-attribute-value-quoted-state
        case .AfterAttributeValueQuoted:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                self.finalizeCurrentAttribute(.SetValue)
                return switchTo(.BeforeAttributeName)
            case "/":
                self.finalizeCurrentAttribute(.SetValue)
                return switchTo(.SelfClosingStartTag)
            case ">":
                self.finalizeCurrentAttribute(.SetValue)
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.finalizeCurrentAttribute(.IgnoreString)
                return emitEOF()
            default:
                // FIXME: log_parse_error()
                self.finalizeCurrentAttribute(.SetValue)
                return reconsume(currentInputCharacter!, in: .BeforeAttributeName)
            }
        // 13.2.5.40 Self-closing start tag state, https://html.spec.whatwg.org/multipage/parsing.html#self-closing-start-tag-state
        case .SelfClosingStartTag:
            switch currentInputCharacter {
            case ">":
                self.currentToken.selfClosing = true
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                // FIXME: log_parse_error()
                return reconsume(currentInputCharacter!, in: .BeforeAttributeName)
            }
        // 13.2.5.41 Bogus comment state, https://html.spec.whatwg.org/multipage/parsing.html#bogus-comment-state
        case .BogusComment:
            switch currentInputCharacter {
            case ">":
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return emitCurrentTokenFollowedByEOF()
            case "\0":
                // FIXME: log_parse_error()
                currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.42 Markup declaration open state, https://html.spec.whatwg.org/multipage/parsing.html#markup-declaration-open-state
        case .MarkupDeclarationOpen:
            dontConsumeNextInputCharacter()
            if let nextTwo = peekNext(count: 2), nextTwo == "--" {
                skip(2)
                return switchTo(.CommentStart)
            } else if let nextSeven = peekNext(count: 7), nextSeven.uppercased() == "DOCTYPE" {
                skip(7)
                return switchTo(.DOCTYPE)
            } else if let nextSeven = peekNext(count: 7), nextSeven.uppercased() == "[CDATA[" {
                skip(7)
                // FIXME: If there is an adjusted current node and it is not an element in the HTML namespace,
                // then switch to the CDATA section state.
                // FIXME: log_parse_error()
                self.currentBuilder = "[CDATA["
                self.currentToken = HTMLToken(type: .Comment(data: ""))
                return switchTo(.BogusComment)
            } else {
                // FIXME: log_parse_error()
                self.currentToken = HTMLToken(type: .Comment(data: ""))
                return switchTo(.BogusComment)
            }
        // 13.2.5.43 Comment start state, https://html.spec.whatwg.org/multipage/parsing.html#comment-start-state
        case .CommentStart:
            switch currentInputCharacter {
            case "-":
                return switchTo(.CommentStartDash)
            case ">":
                // FIXME: log_parse_error()
                return switchToAndEmitCurrentToken(.Data)
            default:
                return reconsume(currentInputCharacter, in: .Comment)
            }
        // 13.2.5.44 Comment start dash state, https://html.spec.whatwg.org/multipage/parsing.html#comment-start-dash-state
        case .CommentStartDash:
            switch currentInputCharacter {
            case "-":
                return switchTo(.CommentEnd)
            case ">":
                // FIXME: log_parse_error()
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return emitCurrentTokenFollowedByEOF()
            default:
                currentBuilder.append("-")
                return reconsume(currentInputCharacter, in: .Comment)
            }
        // 13.2.5.45 Comment state, https://html.spec.whatwg.org/multipage/parsing.html#comment-state
        case .Comment:
            switch currentInputCharacter {
            case "<":
                currentBuilder.append("<")
                return switchTo(.CommentLessThanSign)
            case "-":
                return switchTo(.CommentEndDash)
            case "\0":
                // FIXME: log_parse_error()
                currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            case nil:
                // FIXME: log_parse_error()
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return emitCurrentTokenFollowedByEOF()
            default:
                currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.46 Comment less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#comment-less-than-sign-state
        case .CommentLessThanSign:
            switch currentInputCharacter {
            case "!":
                currentBuilder.append(currentInputCharacter!)
                return switchTo(.CommentLessThanSignBang)
            case "<":
                currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            default:
                return reconsume(currentInputCharacter, in: .Comment)
            }
        // 13.2.5.47 Comment less-than sign bang state, https://html.spec.whatwg.org/multipage/parsing.html#comment-less-than-sign-bang-state
        case .CommentLessThanSignBang:
            switch currentInputCharacter {
            case "-":
                return switchTo(.CommentLessThanSignBangDash)
            default:
                return reconsume(currentInputCharacter, in: .Comment)
            }
        // 13.2.5.48 Comment less-than sign bang dash state, https://html.spec.whatwg.org/multipage/parsing.html#comment-less-than-sign-bang-dash-state
        case .CommentLessThanSignBangDash:
            switch currentInputCharacter {
            case "-":
                return switchTo(.CommentLessThanSignBangDashDash)
            default:
                return reconsume(currentInputCharacter, in: .CommentEndDash)
            }
        // 13.2.5.49 Comment less-than sign bang dash dash state, https://html.spec.whatwg.org/multipage/parsing.html#comment-less-than-sign-bang-dash-dash-state
        case .CommentLessThanSignBangDashDash:
            switch currentInputCharacter {
            case ">", nil:
                return reconsume(currentInputCharacter, in: .CommentEnd)
            default:
                // FIXME: log_parse_error()
                return reconsume(currentInputCharacter, in: .CommentEnd)
            }
        // 13.2.5.50 Comment end dash state, https://html.spec.whatwg.org/multipage/parsing.html#comment-end-dash-state
        case .CommentEndDash:
            switch currentInputCharacter {
            case "-":
                return switchTo(.CommentEnd)
            case nil:
                // FIXME: log_parse_error()
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return emitCurrentTokenFollowedByEOF()
            default:
                currentBuilder.append("-")
                return reconsume(currentInputCharacter, in: .Comment)
            }
        // 13.2.5.51 Comment end state, https://html.spec.whatwg.org/multipage/parsing.html#comment-end-state
        case .CommentEnd:
            switch currentInputCharacter {
            case ">":
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return switchToAndEmitCurrentToken(.Data)
            case "!":
                return switchTo(.CommentEndBang)
            case "-":
                currentBuilder.append("-")
                return continueInCurrentState()
            case nil:
                // FIXME: log_parse_error()
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return emitCurrentTokenFollowedByEOF()
            default:
                currentBuilder.append("--")
                return reconsume(currentInputCharacter, in: .Comment)
            }
        // 13.2.5.52 Comment end bang state, https://html.spec.whatwg.org/multipage/parsing.html#comment-end-bang-state
        case .CommentEndBang:
            switch currentInputCharacter {
            case "-":
                currentBuilder.append("--!")
                return switchTo(.CommentEndDash)
            case ">":
                // FIXME: log_parse_error()
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                currentToken = HTMLToken(type: .Comment(data: currentBuilder.takeString()))
                return emitCurrentTokenFollowedByEOF()
            default:
                currentBuilder.append("--!")
                return reconsume(currentInputCharacter, in: .Comment)
            }
        // 13.2.5.53 DOCTYPE state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-state
        case .DOCTYPE:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return switchTo(.BeforeDOCTYPEName)
            case ">":
                return reconsume(currentInputCharacter, in: .BeforeDOCTYPEName)
            case nil:
                // FIXME: log_parse_error()
                currentToken = HTMLToken(type: .DOCTYPE(name: nil, publicIdentifier: nil, systemIdentifier: nil, forceQuirksMode: true))
                return emitCurrentTokenFollowedByEOF()
            default:
                // FIXME: log_parse_error()
                return reconsume(currentInputCharacter!, in: .BeforeDOCTYPEName)
            }
        // 13.2.5.54 Before DOCTYPE name state, https://html.spec.whatwg.org/multipage/parsing.html#before-doctype-name-state
        case .BeforeDOCTYPEName:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return continueInCurrentState()
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                precondition(self.currentBuilder.isEmpty)
                self.currentToken = HTMLToken(type: .DOCTYPE(name: nil, publicIdentifier: nil, systemIdentifier: nil, forceQuirksMode: false))
                self.currentBuilder.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                return switchTo(.DOCTYPEName)
            case "\0":
                // FIXME: log_parse_error()
                precondition(self.currentBuilder.isEmpty)
                self.currentToken = HTMLToken(type: .DOCTYPE(name: nil, publicIdentifier: nil, systemIdentifier: nil, forceQuirksMode: false))
                self.currentBuilder.append("\u{FFFD}")
                return switchTo(.DOCTYPEName)
            case ">":
                // FIXME: log_parse_error()
                self.currentToken = HTMLToken(type: .DOCTYPE(name: nil, publicIdentifier: nil, systemIdentifier: nil, forceQuirksMode: true))
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken = HTMLToken(type: .DOCTYPE(name: nil, publicIdentifier: nil, systemIdentifier: nil, forceQuirksMode: true))
                return emitCurrentTokenFollowedByEOF()
            default:
                // FIXME: log_parse_error()
                precondition(self.currentBuilder.isEmpty)
                self.currentToken = HTMLToken(type: .DOCTYPE(name: nil, publicIdentifier: nil, systemIdentifier: nil, forceQuirksMode: false))
                self.currentBuilder.append(currentInputCharacter!)
                return switchTo(.DOCTYPEName)
            }
        // 13.2.5.55 DOCTYPE name state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-name-state
        case .DOCTYPEName:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                self.currentToken.name = self.currentBuilder.takeString()
                return switchTo(.AfterDOCTYPEName)
            case ">":
                self.currentToken.name = self.currentBuilder.takeString()
                return switchToAndEmitCurrentToken(.Data)
            case let c? where HTMLTokenizer.asciiUpperAlpha.contains(c.unicodeScalars.first!):
                self.currentBuilder.append(Character(Unicode.Scalar(c.asciiValue! + 0x20)))
                return continueInCurrentState()
            case "\0":
                // FIXME: log_parse_error()
                self.currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                self.currentToken.name = self.currentBuilder.takeString()
                return emitCurrentTokenFollowedByEOF()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.56 After DOCTYPE name state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-name-state
        case .AfterDOCTYPEName:
            precondition(self.currentBuilder.isEmpty)
            precondition(self.currentToken.name != nil)
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return continueInCurrentState()
            case ">":
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                if "pP".contains(currentInputCharacter!), peekNext(count: 5)?.uppercased() == "UBLIC" {
                    skip(5)
                    return switchTo(.AfterDOCTYPEPublicKeyword)
                }
                if "sS".contains(currentInputCharacter!), peekNext(count: 5)?.uppercased() == "YSTEM" {
                    skip(5)
                    return switchTo(.AfterDOCTYPESystemKeyword)
                }

                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return reconsume(currentInputCharacter!, in: .BogusDOCTYPE)
            }
        // 13.2.5.57 After DOCTYPE public keyword state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-public-keyword-state
        case .AfterDOCTYPEPublicKeyword:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return switchTo(.BeforeDOCTYPEPublicIdentifier)
            case "\"":
                // FIXME: log_parse_error()
                self.currentToken.publicIdentifier = ""
                return switchTo(.DOCTYPEPublicIdentifierDoubleQuoted)
            case "'":
                self.currentToken.publicIdentifier = ""
                return switchTo(.DOCTYPEPublicIdentifierSingleQuoted)
            case ">":
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return reconsume(currentInputCharacter!, in: .BogusDOCTYPE)
            }
        // 13.2.5.58 Before DOCTYPE public identifier state, https://html.spec.whatwg.org/multipage/parsing.html#before-doctype-public-identifier-state
        case .BeforeDOCTYPEPublicIdentifier:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return continueInCurrentState()
            case "\"":
                self.currentToken.publicIdentifier = ""
                return switchTo(.DOCTYPEPublicIdentifierDoubleQuoted)
            case "'":
                self.currentToken.publicIdentifier = ""
                return switchTo(.DOCTYPEPublicIdentifierSingleQuoted)
            case ">":
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return reconsume(currentInputCharacter!, in: .BogusDOCTYPE)
            }
        // 13.2.5.59 DOCTYPE public identifier (double-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-public-identifier-double-quoted-state
        case .DOCTYPEPublicIdentifierDoubleQuoted:
            switch currentInputCharacter {
            case "\"":
                self.currentToken.publicIdentifier = self.currentBuilder.takeString()
                return switchTo(.AfterDOCTYPEPublicIdentifier)
            case "\0":
                // FIXME: log_parse_error()
                self.currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            case ">":
                // FIXME: log_parse_error()
                self.currentToken.publicIdentifier = self.currentBuilder.takeString()
                self.currentToken.forceQuirks = true
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.60 DOCTYPE public identifier (single-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-public-identifier-single-quoted-state
        case .DOCTYPEPublicIdentifierSingleQuoted:
            switch currentInputCharacter {
            case "'":
                self.currentToken.publicIdentifier = self.currentBuilder.takeString()
                return switchTo(.AfterDOCTYPEPublicIdentifier)
            case "\0":
                // FIXME: log_parse_error()
                self.currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            case ">":
                // FIXME: log_parse_error()
                self.currentToken.publicIdentifier = self.currentBuilder.takeString()
                self.currentToken.forceQuirks = true
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.61 After DOCTYPE public identifier state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-public-identifier-state
        case .AfterDOCTYPEPublicIdentifier:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return switchTo(.BetweenDOCTYPEPublicAndSystemIdentifiers)
            case ">":
                return switchToAndEmitCurrentToken(.Data)
            case "\"":
                // FIXME: log_parse_error()
                self.currentToken.systemIdentifier = ""
                return switchTo(.DOCTYPESystemIdentifierDoubleQuoted)
            case "'":
                // FIXME: log_parse_error()
                self.currentToken.systemIdentifier = ""
                return switchTo(.DOCTYPESystemIdentifierSingleQuoted)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return reconsume(currentInputCharacter!, in: .BogusDOCTYPE)
            }
        // 13.2.5.62 Between DOCTYPE public and system identifiers state, https://html.spec.whatwg.org/multipage/parsing.html#between-doctype-public-and-system-identifiers-state
        case .BetweenDOCTYPEPublicAndSystemIdentifiers:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return continueInCurrentState()
            case ">":
                return switchToAndEmitCurrentToken(.Data)
            case "\"":
                self.currentToken.systemIdentifier = ""
                return switchTo(.DOCTYPESystemIdentifierDoubleQuoted)
            case "'":
                self.currentToken.systemIdentifier = ""
                return switchTo(.DOCTYPESystemIdentifierSingleQuoted)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return reconsume(currentInputCharacter!, in: .BogusDOCTYPE)
            }
        // 13.2.5.63 After DOCTYPE system keyword state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-system-keyword-state
        case .AfterDOCTYPESystemKeyword:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return switchTo(.BeforeDOCTYPESystemIdentifier)
            case "\"":
                // FIXME: log_parse_error()
                self.currentToken.systemIdentifier = ""
                return switchTo(.DOCTYPESystemIdentifierDoubleQuoted)
            case "'":
                // FIXME: log_parse_error()
                self.currentToken.systemIdentifier = ""
                return switchTo(.DOCTYPESystemIdentifierSingleQuoted)
            case ">":
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return reconsume(currentInputCharacter!, in: .BogusDOCTYPE)
            }
        // 13.2.5.64 Before DOCTYPE system identifier state, https://html.spec.whatwg.org/multipage/parsing.html#before-doctype-system-identifier-state
        case .BeforeDOCTYPESystemIdentifier:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return continueInCurrentState()
            case "\"":
                self.currentToken.systemIdentifier = ""
                return switchTo(.DOCTYPESystemIdentifierDoubleQuoted)
            case "'":
                self.currentToken.systemIdentifier = ""
                return switchTo(.DOCTYPESystemIdentifierSingleQuoted)
            case ">":
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return reconsume(currentInputCharacter!, in: .BogusDOCTYPE)
            }
        // 13.2.5.65 DOCTYPE system identifier (double-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-system-identifier-double-quoted-state
        case .DOCTYPESystemIdentifierDoubleQuoted:
            switch currentInputCharacter {
            case "\"":
                self.currentToken.systemIdentifier = self.currentBuilder.takeString()
                return switchTo(.AfterDOCTYPESystemIdentifier)
            case "\0":
                // FIXME: log_parse_error()
                self.currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            case ">":
                // FIXME: log_parse_error()
                self.currentToken.systemIdentifier = self.currentBuilder.takeString()
                self.currentToken.forceQuirks = true
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.66 DOCTYPE system identifier (single-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-system-identifier-single-quoted-state
        case .DOCTYPESystemIdentifierSingleQuoted:
            switch currentInputCharacter {
            case "'":
                return switchTo(.AfterDOCTYPESystemIdentifier)
            case "\0":
                // FIXME: log_parse_error()
                self.currentBuilder.append("\u{FFFD}")
                return continueInCurrentState()
            case ">":
                // FIXME: log_parse_error()
                self.currentToken.systemIdentifier = self.currentBuilder.takeString()
                self.currentToken.forceQuirks = true
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                self.currentBuilder.append(currentInputCharacter!)
                return continueInCurrentState()
            }
        // 13.2.5.67 After DOCTYPE system identifier state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-system-identifier-state
        case .AfterDOCTYPESystemIdentifier:
            switch currentInputCharacter {
            case "\t", "\n", "\u{000C}", " ":
                return continueInCurrentState()
            case ">":
                return switchToAndEmitCurrentToken(.Data)
            case nil:
                // FIXME: log_parse_error()
                self.currentToken.forceQuirks = true
                return emitCurrentTokenFollowedByEOF()
            default:
                // FIXME: log_parse_error()
                // NOTE: This does not set the current DOCTYPE token's force-quirks flag to on.
                return reconsume(currentInputCharacter!, in: .BogusDOCTYPE)
            }
        // 13.2.5.68 Bogus DOCTYPE state, https://html.spec.whatwg.org/multipage/parsing.html#bogus-doctype-state
        case .BogusDOCTYPE:
            switch currentInputCharacter {
            case ">":
                return switchToAndEmitCurrentToken(.Data)
            case "\0":
                // FIXME: log_parse_error()
                return continueInCurrentState()
            case nil:
                return emitCurrentTokenFollowedByEOF()
            default:
                return continueInCurrentState()
            }
        // 13.2.5.69 CDATA section state, https://html.spec.whatwg.org/multipage/parsing.html#cdata-section-state
        case .CDATASection:
            switch currentInputCharacter {
            case "]":
                return switchTo(.CDATASectionBracket)
            case nil:
                // FIXME: log_parse_error()
                return emitEOF()
            default:
                // NOTE: U+0000 NULL characters are handled in the tree construction stage,
                //       as part of the in foreign content insertion mode, which is the only place where CDATA sections can appear.
                return emitCharacter(currentInputCharacter!)
            }
        // 13.2.5.70 CDATA section bracket state, https://html.spec.whatwg.org/multipage/parsing.html#cdata-section-bracket-state
        case .CDATASectionBracket:
            switch currentInputCharacter {
            case "]":
                return switchTo(.CDATASectionEnd)
            default:
                return emitCharacterAndReconsume("]", in: .CDATASection, currentInputCharacter: currentInputCharacter)
            }

        // 13.2.5.71 CDATA section end state, https://html.spec.whatwg.org/multipage/parsing.html#cdata-section-end-state
        case .CDATASectionEnd:
            switch currentInputCharacter {
            case "]":
                return emitCharacter("]")
            case ">":
                return switchTo(.Data)
            default:
                queuedTokens.append(HTMLToken(type: .Character(codePoint: "]")))
                return emitCharacterAndReconsume("]", in: .CDATASection, currentInputCharacter: currentInputCharacter)
            }
        // 13.2.5.72 Character reference state, https://html.spec.whatwg.org/multipage/parsing.html#character-reference-state
        case .CharacterReference:
            self.temporaryBuffer = "&"
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiAlphanumeric.contains(c.unicodeScalars.first!):
                return reconsume(currentInputCharacter!, in: .NamedCharacterReference)
            case "#":
                self.temporaryBuffer.append(currentInputCharacter!)
                return switchTo(.NumericCharacterReference)
            default:
                self.flushCodepointsConsumedAsACharacterReference()
                return reconsume(currentInputCharacter, in: self.returnState)
            }
        // 13.2.5.73 Named character reference state, https://html.spec.whatwg.org/multipage/parsing.html#named-character-reference-state
        case .NamedCharacterReference:
            // FIXME: This should be updated to match the C++ implementation, this
            //        won't handle one-character-at-a-time document.write calls properly.
            var subString = self.input[self.previousCursor...]
            let entityMatch = subString.withUTF8 { utf8 in
                return Web.HTML.match_entity_for_named_character_reference(AK.StringView(utf8.baseAddress!, utf8.count))
            }
            if entityMatch.hasValue {
                let entity = entityMatch.value!.entity
                skip(entity.length())
                // FIXME: Iterate over the entity's code points and add them instead of creating a string
                self.temporaryBuffer.append(Swift.String(akStringView: entity)!)

                if self.consumedAsPartOfAnAttribute(), !entity.endsWith(";") {
                    if let peeked = peekCodePoint(), peeked == "=" || HTMLTokenizer.asciiAlphanumeric.contains(peeked.unicodeScalars.first!) {
                        self.flushCodepointsConsumedAsACharacterReference()
                        return switchTo(self.returnState)
                    }
                }

                if !entity.endsWith(";") {
                    // FIXME: log_parse_error()
                }

                self.temporaryBuffer = ""

                // FIXME: This AK::Vector<u32, 2> should be CxxConvertibleToContainer, but https://github.com/swiftlang/swift/issues/77607
                let codePoints = entityMatch.value!.code_points
                for i in 0...codePoints.size() {
                    self.temporaryBuffer.append(Character(Unicode.Scalar(codePoints[i])!))
                }
                self.flushCodepointsConsumedAsACharacterReference()
                return switchTo(self.returnState)
            }

            self.flushCodepointsConsumedAsACharacterReference()
            return reconsume(currentInputCharacter, in: .AmbiguousAmpersand)
        // 13.2.5.74 Ambiguous ampersand state, https://html.spec.whatwg.org/multipage/parsing.html#ambiguous-ampersand-state
        case .AmbiguousAmpersand:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiAlphanumeric.contains(c.unicodeScalars.first!):
                if self.consumedAsPartOfAnAttribute() {
                    self.currentBuilder.append(currentInputCharacter!)
                    return continueInCurrentState()
                }
                return emitCharacter(currentInputCharacter!)
            case ";":
                // FIXME: log_parse_error()
                return reconsume(currentInputCharacter!, in: self.returnState)
            default:
                return reconsume(currentInputCharacter, in: self.returnState)
            }
        // 13.2.5.75 Numeric character reference state, https://html.spec.whatwg.org/multipage/parsing.html#numeric-character-reference-state
        case .NumericCharacterReference:
            self.characterReferenceCode = 0
            switch currentInputCharacter {
            case "x", "X":
                self.temporaryBuffer.append(currentInputCharacter!)
                return switchTo(.HexadecimalCharacterReferenceStart)
            default:
                return reconsume(currentInputCharacter, in: .DecimalCharacterReferenceStart)
            }
        // 13.2.5.76 Hexadecimal character reference start state, https://html.spec.whatwg.org/multipage/parsing.html#hexadecimal-character-reference-start-state
        case .HexadecimalCharacterReferenceStart:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiHexDigit.contains(c.unicodeScalars.first!):
                return reconsume(currentInputCharacter!, in: .HexadecimalCharacterReference)
            default:
                // FIXME: log_parse_error()
                self.flushCodepointsConsumedAsACharacterReference()
                return reconsume(currentInputCharacter, in: self.returnState)
            }
        // 13.2.5.77 Decimal character reference start state, https://html.spec.whatwg.org/multipage/parsing.html#decimal-character-reference-start-state
        case .DecimalCharacterReferenceStart:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiDigit.contains(c.unicodeScalars.first!):
                return reconsume(currentInputCharacter!, in: .DecimalCharacterReference)
            default:
                // FIXME: log_parse_error()
                self.flushCodepointsConsumedAsACharacterReference()
                return reconsume(currentInputCharacter, in: self.returnState)
            }
        // 13.2.5.78 Hexadecimal character reference state, https://html.spec.whatwg.org/multipage/parsing.html#hexadecimal-character-reference-state
        case .HexadecimalCharacterReference:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiDigit.contains(c.unicodeScalars.first!):
                self.characterReferenceCode = self.characterReferenceCode * 16 + Int(c.asciiValue! - 0x30)
                return continueInCurrentState()
            case let c? where HTMLTokenizer.asciiUpperHexDigit.contains(c.unicodeScalars.first!):
                self.characterReferenceCode = self.characterReferenceCode * 16 + Int(c.asciiValue! - 0x37)
                return continueInCurrentState()
            case let c? where HTMLTokenizer.asciiLowerHexDigit.contains(c.unicodeScalars.first!):
                self.characterReferenceCode = self.characterReferenceCode * 16 + Int(c.asciiValue! - 0x57)
                return continueInCurrentState()
            case ";":
                return switchTo(.NumericCharacterReferenceEnd)
            default:
                // FIXME: log_parse_error()
                return reconsume(currentInputCharacter, in: .NumericCharacterReferenceEnd)
            }

        // 13.2.5.79 Decimal character reference state, https://html.spec.whatwg.org/multipage/parsing.html#decimal-character-reference-state
        case .DecimalCharacterReference:
            switch currentInputCharacter {
            case let c? where HTMLTokenizer.asciiDigit.contains(c.unicodeScalars.first!):
                self.characterReferenceCode = self.characterReferenceCode * 10 + Int(c.asciiValue! - 0x30)
                return continueInCurrentState()
            case ";":
                return switchTo(.NumericCharacterReferenceEnd)
            default:
                // FIXME: log_parse_error()
                return reconsume(currentInputCharacter, in: .NumericCharacterReferenceEnd)
            }

        // 13.2.5.80 Numeric character reference end state, https://html.spec.whatwg.org/multipage/parsing.html#numeric-character-reference-end-state
        case .NumericCharacterReferenceEnd:
            dontConsumeNextInputCharacter()
            let codePoint: UnicodeScalar =
                switch self.characterReferenceCode {
                case 0x00:
                    // FIXME: log_parse_error()
                    UnicodeScalar(0xFFFD)!
                case let c where c > 0x10FFFF:
                    // FIXME: log_parse_error()
                    UnicodeScalar(0xFFFD)!
                case let c where UTF16.CodeUnit(exactly: c).map({ UTF16.isSurrogate($0) }) != nil:
                    // FIXME: log_parse_error()
                    UnicodeScalar(0xFFFD)!
                case let c where UnicodeScalar(c)! == "\u{000D}" || HTMLTokenizer.controlNotAsciiWhitespace.contains(UnicodeScalar(c)!):
                    // FIXME: log_parse_error()
                    if let codePoint = HTMLTokenizer.characterReferenceControlCodeMapping[c] {
                        codePoint
                    } else {
                        UnicodeScalar(c)!
                    }
                case let c where HTMLTokenizer.nonCharacter.contains(UnicodeScalar(c)!):
                    // FIXME: log_parse_error()
                    UnicodeScalar(c)!
                default:
                    UnicodeScalar(self.characterReferenceCode)!
                }

            self.temporaryBuffer = Swift.String(Character(codePoint))
            self.flushCodepointsConsumedAsACharacterReference()
            return switchTo(self.returnState)
        }
    }
}
