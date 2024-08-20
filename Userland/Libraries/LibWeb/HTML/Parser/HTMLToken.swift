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

extension HTMLToken.Position: Equatable, CustomStringConvertible {
    public var description: String {
        return "\(self.line):\(self.column)"
    }
}

extension HTMLToken.TokenType: CustomStringConvertible {
    // FIXME: Print attributes for start/end tags
    public var description: String {
        switch self {
        case .Invalid:
            return "Invalid"
        case .DOCTYPE(let name, let publicIdentifier, let systemIdentifier, let forceQuirksMode):
            return "DOCTYPE(name: \(name ?? "nil"), publicIdentifier: \(publicIdentifier ?? "nil"), systemIdentifier: \(systemIdentifier ?? "nil"), forceQuirksMode: \(forceQuirksMode))"
        case .StartTag(let tagName, let selfClosing, let selfClosingAcknowledged, let attributes):
            return "StartTag(tagName: \(tagName), selfClosing: \(selfClosing), selfClosingAcknowledged: \(selfClosingAcknowledged), attributes: \(attributes))"
        case .EndTag(let tagName, let selfClosing, let selfClosingAcknowledged, let attributes):
            return "EndTag(tagName: \(tagName), selfClosing: \(selfClosing), selfClosingAcknowledged: \(selfClosingAcknowledged), attributes: \(attributes))"
        case .Comment(let data):
            return "Comment(data: \(data))"
        case .Character(let codePoint):
            return "Character(codePoint: \(codePoint))"
        case .EndOfFile:
            return "EndOfFile"
        }
    }
}

extension HTMLToken: CustomStringConvertible {
    public var description: String {
        if (self.startPosition == Position()) {
            return "HTMLToken(type: \(self.type))"
        }
        else if (self.endPosition == Position()) {
            return "HTMLToken(type: \(self.type))@\(self.startPosition)"
        }
        else {
            return "HTMLToken(type: \(self.type))@\(self.startPosition)-\(self.endPosition)"
        }
    }
}
