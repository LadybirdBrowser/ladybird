/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import Collections
import Foundation
import LibWeb
import SwiftAK

extension Swift.String {
    public init?(decoding: AK.StringView, as: AK.StringView) {
        let maybe_decoded = Web.HTML.decode_to_utf8(decoding, `as`)
        if maybe_decoded.hasValue {
            self.init(maybe_decoded.value!)
        } else {
            return nil
        }
    }
}

class HTMLTokenizer {

    enum State {
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

    var input = Swift.String()
    var state = State.Data
    var returnState = State.Data

    var currentToken = HTMLToken()
    var queuedTokens = Deque<HTMLToken>()

    public init() {}
    public init?(input: AK.StringView, encoding: AK.StringView) {
        if let string = Swift.String(decoding: input, as: encoding) {
            self.input = string
        } else {
            return nil
        }
    }

    public func nextToken(stopAtInsertionPoint: Bool = false) -> HTMLToken? {

        while !queuedTokens.isEmpty {
            return queuedTokens.popFirst()
        }

        return nil
    }

}
