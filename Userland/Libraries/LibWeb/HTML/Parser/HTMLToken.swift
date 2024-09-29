/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

@_exported import WebCxx

public class HTMLToken {
    public struct Position: Equatable {
        var line = UInt()
        var column = UInt()
        var byteOffset = UInt()
    }

    public struct Attribute: Equatable {
        public var prefix: Swift.String? = nil
        public var localName: Swift.String
        public var namespace_: Swift.String? = nil
        public var value: Swift.String
        public var nameStartPosition = Position()
        public var nameEndPosition = Position()
        public var valueStartPosition = Position()
        public var valueEndPosition = Position()

        public init(localName: Swift.String, value: Swift.String) {
            self.localName = localName
            self.value = value
        }
    }

    public enum TokenType: Equatable {
        case Invalid
        case DOCTYPE(
            name: Swift.String?,
            publicIdentifier: Swift.String?,
            systemIdentifier: Swift.String?,
            forceQuirksMode: Bool)
        case StartTag(
            tagName: Swift.String,
            selfClosing: Bool = false,
            selfClosingAcknowledged: Bool = false,
            attributes: [Attribute] = [])
        case EndTag(
            tagName: Swift.String,
            selfClosing: Bool = false,
            selfClosingAcknowledged: Bool = false,
            attributes: [Attribute] = [])
        case Comment(data: Swift.String)
        case Character(codePoint: Character)
        case EndOfFile
    }

    public func isCharacter() -> Bool {
        if case .Character(_) = self.type {
            return true
        }
        return false
    }

    public func isEndTag() -> Bool {
        if case .EndTag(_, _, _, _) = self.type {
            return true
        }
        return false
    }

    public func isStartTag() -> Bool {
        if case .StartTag(_, _, _, _) = self.type {
            return true
        }
        return false
    }

    public func isTag() -> Bool {
        return isStartTag() || isEndTag()
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

    // Is in-place mutating enums a thing? Seems not https://forums.swift.org/t/in-place-mutation-of-an-enum-associated-value/11747
    public var attributes: [Attribute] {
        get {
            switch self.type {
            case .StartTag(_, _, _, let attributes):
                return attributes
            case .EndTag(_, _, _, let attributes):
                return attributes
            default:
                preconditionFailure("attributes called on non-tag token")
            }
        }
        set {
            switch self.type {
            case .StartTag(let tagName, let selfClosing, let selfClosingAcknowledged, attributes: _):
                self.type = .StartTag(tagName: tagName, selfClosing: selfClosing, selfClosingAcknowledged: selfClosingAcknowledged, attributes: newValue)
            case .EndTag(let tagName, let selfClosing, let selfClosingAcknowledged, attributes: _):
                self.type = .EndTag(tagName: tagName, selfClosing: selfClosing, selfClosingAcknowledged: selfClosingAcknowledged, attributes: newValue)
            default:
                preconditionFailure("attributes= called on non-tag token")
            }
        }
    }
    public var tagName: Swift.String {
        get {
            switch self.type {
            case .StartTag(let tagName, _, _, _):
                return tagName
            case .EndTag(let tagName, _, _, _):
                return tagName
            default:
                preconditionFailure("tagName called on non-tag token")
            }
        }
        set {
            switch self.type {
            case .StartTag(tagName: _, let selfClosing, let selfClosingAcknowledged, let attributes):
                self.type = .StartTag(tagName: newValue, selfClosing: selfClosing, selfClosingAcknowledged: selfClosingAcknowledged, attributes: attributes)
            case .EndTag(tagName: _, let selfClosing, let selfClosingAcknowledged, let attributes):
                self.type = .EndTag(tagName: newValue, selfClosing: selfClosing, selfClosingAcknowledged: selfClosingAcknowledged, attributes: attributes)
            default:
                preconditionFailure("tagName= called on non-tag token")
            }
        }
    }

    public init() {}
    public init(type: TokenType) {
        self.type = type
    }
}

extension HTMLToken.Position: CustomStringConvertible {
    public var description: Swift.String {
        return "\(self.line):\(self.column)"
    }
}

extension HTMLToken.TokenType: CustomStringConvertible {
    // FIXME: Print attributes for start/end tags
    public var description: Swift.String {
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
    public var description: Swift.String {
        if self.startPosition == Position() {
            return "HTMLToken(type: \(self.type))"
        } else if self.endPosition == Position() {
            return "HTMLToken(type: \(self.type))@\(self.startPosition)"
        } else {
            return "HTMLToken(type: \(self.type))@\(self.startPosition)-\(self.endPosition)"
        }
    }
}
