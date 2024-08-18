/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

public class HTMLToken {
    public struct Position {
        var line = UInt()
        var column = UInt()
        var byteOffset = UInt()
    }

    public struct Attribute {
        var prefix: String?
        var localName: String
        var namespace_: String?
        var value: String
        var nameStartPosition: Position
        var nameEndPosition: Position
        var valueStartPosition: Position
        var valueEndPosition: Position
    }

    public enum TokenType {
        case Invalid
        case DOCTYPE(
            name: String?,
            publicIdentifier: String?,
            systemIdentifier: String?,
            forceQuirksMode: Bool)
        case StartTag(
            tagName: String,
            selfClosing: Bool,
            selfClosingAcknowledged: Bool,
            attributes: [Attribute])
        case EndTag(
            tagName: String,
            selfClosing: Bool,
            selfClosingAcknowledged: Bool,
            attributes: [Attribute])
        case Comment(data: String)
        case Character(codePoint: Character)
        case EndOfFile
    }

    public func isCharacter() -> Bool {
        if case .Character(_) = self.type {
            return true
        }
        return false
    }

    public func isParserWhitespace() -> Bool {
        precondition(isCharacter(), "isParserWhitespace() called on non-character token")

        // NOTE: The parser considers '\r' to be whitespace, while the tokenizer does not.
        switch self.type {
        case .Character(codePoint: "\t"),
            .Character(codePoint: "\n"),
            .Character(codePoint: "\u{000C}"),  // \f
            .Character(codePoint: "\r"),
            .Character(codePoint: " "):
            return true
        default:
            return false
        }
    }

    public var type = TokenType.Invalid
    public var startPosition = Position()
    public var endPosition = Position()

    public init() {}
    public init(type: TokenType) {
        self.type = type
    }
}
