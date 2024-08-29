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

    private var aborted = false
    private var hasEmittedEOF = false

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
        self.cursor = self.input.index(self.cursor, offsetBy: count, limitedBy: self.input.endIndex) ?? input.endIndex
        self.previousCursor = self.input.index(before: self.cursor)
    }

    func peekCodePoint(_ offset: Int = 0) -> Character? {
        guard let index = self.input.index(self.cursor, offsetBy: offset, limitedBy: self.input.index(before: self.input.endIndex)) else {
            return nil
        }
        return self.input[index]
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
        // FIXME: Assign Position
    }

    enum NextTokenState {
        case Emit(token: HTMLToken?)
        case SwitchTo
        case Reconsume(inputCharacter: Character?)
        case ReprocessQueue
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
            case .SwitchTo:
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

    func switchTo(_ state: State) -> NextTokenState {
        self.state = state
        return .SwitchTo
    }

    func reconsume(_ character: Character, `in` state: State) -> NextTokenState {
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

    func nextTokenImpl(_ nextInputCharacter: Character? = nil) -> NextTokenState {
        let dontConsumeNextInputCharacter = {
            self.restoreCursorToPrevious()
        }
        let _ = dontConsumeNextInputCharacter

        // FIXME: flushCodepointsConsumedAsACharacterReference needs currentBuilder

        // Handle reconsume by passing the character around in the state enum
        let currentInputCharacter = nextInputCharacter ?? nextCodePoint()

        switch self.state {
        // 13.2.5.1 Data state, https://html.spec.whatwg.org/multipage/parsing.html#data-state
        case .Data:
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
        default:
            print("TODO: In state \(self.state) with input \(Swift.String(describing: currentInputCharacter))")
            return emitEOF()
        }
    }
}
