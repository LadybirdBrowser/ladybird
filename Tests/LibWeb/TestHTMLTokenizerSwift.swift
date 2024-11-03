/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Testing
import Web

@Suite
struct TestHTMLTokenizerSwift {

    @Test func tokenTypes() {
        let default_token = HTMLToken()
        default_token.type = .Character(codePoint: "a")
        #expect(default_token.isCharacter())

        #expect("\(default_token)" == "HTMLToken(type: Character(codePoint: a))")
    }

    @Test func parserWhitespace() {
        for codePoint: Character in ["\t", "\n", "\r", "\u{000C}", " "] {
            let token = HTMLToken(type: .Character(codePoint: codePoint))
            #expect(token.isParserWhitespace())
        }

        for codePoint: Character in ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"] {
            let token = HTMLToken(type: .Character(codePoint: codePoint))
            #expect(!token.isParserWhitespace())
        }
    }

    @Test func dataStateNoInput() {
        let tokenizer = HTMLTokenizer()
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .EndOfFile)

        let token2 = tokenizer.nextToken()
        #expect(token2 == nil)
        #expect(tokenizer.state == HTMLTokenizer.State.Data)
    }

    @Test func dataStateSingleChar() {
        guard let tokenizer = HTMLTokenizer(input: "X") else {
            Issue.record("Failed to create tokenizer for 'X'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .Character(codePoint: "X"))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndOfFile)

        let token3 = tokenizer.nextToken()
        #expect(token3 == nil)
        #expect(tokenizer.state == HTMLTokenizer.State.Data)
    }

    @Test func dataStateAmpersand() {
        guard let tokenizer = HTMLTokenizer(input: "&") else {
            Issue.record("Failed to create tokenizer for '&'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .EndOfFile)
        #expect(tokenizer.state == HTMLTokenizer.State.CharacterReference)

        let token2 = tokenizer.nextToken()
        #expect(token2 == nil)
    }

    @Test func tagOpenOnly() {
        guard let tokenizer = HTMLTokenizer(input: "<") else {
            Issue.record("Failed to create tokenizer for '<'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .Character(codePoint: "<"))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndOfFile)
        #expect(tokenizer.state == HTMLTokenizer.State.TagOpen)

        let token3 = tokenizer.nextToken()
        #expect(token3 == nil)
    }

    @Test func dataStateNulChar() {
        guard let tokenizer = HTMLTokenizer(input: "H\0I") else {
            Issue.record("Failed to create tokenizer for 'H\\0I'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .Character(codePoint: "H"))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .Character(codePoint: "\u{FFFD}"))

        let token3 = tokenizer.nextToken()
        #expect(token3?.type == .Character(codePoint: "I"))

        let token4 = tokenizer.nextToken()
        #expect(token4?.type == .EndOfFile)

        #expect(tokenizer.state == HTMLTokenizer.State.Data)
    }

    @Test func scriptTagWithAttributes() {
        guard let tokenizer = HTMLTokenizer(input: "<script type=\"text/javascript\">") else {
            Issue.record("Failed to create tokenizer for '<script type=\"text/javascript\">'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .StartTag(tagName: "script", attributes: [HTMLToken.Attribute(localName: "type", value: "text/javascript")]))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndOfFile)

        #expect(tokenizer.state == HTMLTokenizer.State.Data)
    }

    @Test func scriptWithContent() {
        guard let tokenizer = HTMLTokenizer(input: "<script>var x = 1;</script>") else {
            Issue.record("Failed to create tokenizer for '<script>var x = 1;</script>'")
            return
        }

        let token = tokenizer.nextToken()
        #expect(token?.type == .StartTag(tagName: "script", attributes: []))

        for codePoint in "var x = 1;" {
            let token = tokenizer.nextToken()
            #expect(token?.type == .Character(codePoint: codePoint))
        }

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndTag(tagName: "script"))

        let token3 = tokenizer.nextToken()
        #expect(token3?.type == .EndOfFile)
    }

    @Test func simpleDivWithContent() {
        guard let tokenizer = HTMLTokenizer(input: "<div>hi</div>") else {
            Issue.record("Failed to create tokenizer for '<div>hi</div>'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .StartTag(tagName: "div", attributes: []))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .Character(codePoint: "h"))

        let token3 = tokenizer.nextToken()
        #expect(token3?.type == .Character(codePoint: "i"))

        let token4 = tokenizer.nextToken()
        #expect(token4?.type == .EndTag(tagName: "div"))

        let token5 = tokenizer.nextToken()
        #expect(token5?.type == .EndOfFile)
    }

    @Test func simpleDivWithContentAndAttributes() {
        guard let tokenizer = HTMLTokenizer(input: "<div class=\"foo\">hi</div>") else {
            Issue.record("Failed to create tokenizer for '<div class=\"foo\">hi</div>'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .StartTag(tagName: "div", attributes: [HTMLToken.Attribute(localName: "class", value: "foo")]))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .Character(codePoint: "h"))

        let token3 = tokenizer.nextToken()
        #expect(token3?.type == .Character(codePoint: "i"))

        let token4 = tokenizer.nextToken()
        #expect(token4?.type == .EndTag(tagName: "div"))

        let token5 = tokenizer.nextToken()
        #expect(token5?.type == .EndOfFile)
    }

    @Test func severalDivsWithAttributesAndContent() {
        // Explicitly use unquoted and single quotes for attribute values
        guard let tokenizer = HTMLTokenizer(input: "<div class=foo>hi</div><div class='bar'>bye</div>") else {
            Issue.record("Failed to create tokenizer for '<div class=\"foo\">hi</div><div class=\"bar\">bye</div>'")
            return
        }

        let token = tokenizer.nextToken()
        #expect(token?.type == .StartTag(tagName: "div", attributes: [HTMLToken.Attribute(localName: "class", value: "foo")]))

        for codePoint in "hi" {
            let token = tokenizer.nextToken()
            #expect(token?.type == .Character(codePoint: codePoint))
        }

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndTag(tagName: "div"))

        let token3 = tokenizer.nextToken()
        #expect(token3?.type == .StartTag(tagName: "div", attributes: [HTMLToken.Attribute(localName: "class", value: "bar")]))

        for codePoint in "bye" {
            let token = tokenizer.nextToken()
            #expect(token?.type == .Character(codePoint: codePoint))
        }

        let token4 = tokenizer.nextToken()
        #expect(token4?.type == .EndTag(tagName: "div"))

        let token5 = tokenizer.nextToken()
        #expect(token5?.type == .EndOfFile)
    }

    @Test func startTagWithMultipleAttributes() {
        guard let tokenizer = HTMLTokenizer(input: "<div class=\"foo\" id=\"bar\">hi</div attr=endTagAttributeWhee>") else {
            Issue.record("Failed to create tokenizer for '<div class=\"foo\" id=\"bar\">hi</div>'")
            return
        }

        let token = tokenizer.nextToken()
        #expect(token?.type == .StartTag(tagName: "div", attributes: [HTMLToken.Attribute(localName: "class", value: "foo"), HTMLToken.Attribute(localName: "id", value: "bar")]))

        for codePoint in "hi" {
            let token = tokenizer.nextToken()
            #expect(token?.type == .Character(codePoint: codePoint))
        }

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndTag(tagName: "div", attributes: [HTMLToken.Attribute(localName: "attr", value: "endTagAttributeWhee")]))

        let token3 = tokenizer.nextToken()
        #expect(token3?.type == .EndOfFile)
    }

    @Test func xmlDeclaration() {
        guard let tokenizer = HTMLTokenizer(input: "<?xml version=\"1.0\" encoding=\"UTF-8\"?>") else {
            Issue.record("Failed to create tokenizer for '<?xml version=\"1.0\" encoding=\"UTF-8\"?>'")
            return
        }

        let token = tokenizer.nextToken()
        #expect(token?.type == .Comment(data: "?xml version=\"1.0\" encoding=\"UTF-8\"?"))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndOfFile)
    }

    @Test func simpleComment() {
        guard let tokenizer = HTMLTokenizer(input: "<!-- comment -->") else {
            Issue.record("Failed to create tokenizer for '<!-- comment -->'")
            return
        }

        let token = tokenizer.nextToken()
        #expect(token?.type == .Comment(data: " comment "))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndOfFile)
    }

    @Test func nestedComment() {
        guard let tokenizer = HTMLTokenizer(input: "<!-- <!-- nested --> -->") else {
            Issue.record("Failed to create tokenizer for '<!-- <!-- nested --> -->'")
            return
        }

        let token = tokenizer.nextToken()
        #expect(token?.type == .Comment(data: " <!-- nested "))

        for codePoint in " -->" {
            let token = tokenizer.nextToken()
            #expect(token?.type == .Character(codePoint: codePoint))
        }

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndOfFile)
    }

    @Test func commentWithScriptTagInside() {
        guard let tokenizer = HTMLTokenizer(input: "<!-- <script>var x = 1;</script> -->") else {
            Issue.record("Failed to create tokenizer for '<!-- <script>var x = 1;</script> -->'")
            return
        }

        let token = tokenizer.nextToken()
        #expect(token?.type == .Comment(data: " <script>var x = 1;</script> "))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndOfFile)
    }
}
