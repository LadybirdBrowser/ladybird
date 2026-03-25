// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// V8 assertion compatibility shim for Ladybird's test-js harness

function assertEquals(expected, actual, msg) {
    if (expected instanceof RegExp && actual instanceof RegExp) {
        expect(actual.source).toBe(expected.source);
        expect(actual.flags).toBe(expected.flags);
    } else if (Array.isArray(expected) && Array.isArray(actual)) {
        expect(actual).toEqual(expected);
    } else if (expected !== null && typeof expected === "object" && actual !== null && typeof actual === "object") {
        expect(actual).toEqual(expected);
    } else {
        expect(actual).toBe(expected);
    }
}

function assertTrue(val, msg) {
    expect(val).toBeTrue();
}

function assertFalse(val, msg) {
    expect(val).toBeFalse();
}

function assertNull(val, msg) {
    expect(val).toBeNull();
}

function assertNotNull(val, msg) {
    expect(val).not.toBeNull();
}

function assertThrows(fn, type_opt, msg_opt) {
    if (typeof fn === "string") {
        try {
            fn = new Function(fn);
        } catch (e) {
            return;
        }
    }
    expect(fn).toThrow();
}

function assertDoesNotThrow(fn, msg) {
    fn();
}

function assertInstanceof(val, type, msg) {
    expect(val instanceof type).toBeTrue();
}

function assertUnreachable(msg) {
    expect().fail("unreachable" + (msg ? ": " + msg : ""));
}

function assertEarlyError(code) {
    assertThrows(() => new Function(code));
}

function assertThrowsAtRuntime(code, type_opt) {
    const f = new Function(code);
    assertThrows(f, type_opt);
}

function assertArrayEquals(expected, actual) {
    expect(actual).toEqual(expected);
}

test.xfail("es6/unicode-regexp-ignore-case-noi18n", () => {
    assertTrue(/[a]/iu.test("\u{20a0}a"));
    assertTrue(/[a]/iu.test("\u{20a0}A"));
    assertTrue(/[A]/iu.test("\u{20a0}a"));
    assertTrue(/[A]/iu.test("\u{20a0}A"));

    // Non-unicode use toUpperCase mappings.
    assertFalse(/[\u00e5]/i.test("\u212b"));
    assertFalse(/[\u212b]/i.test("\u00e5\u1234"));
    assertFalse(/[\u212b]/i.test("\u00e5"));

    assertTrue("\u212b".toLowerCase() == "\u00e5");
    assertTrue("\u00c5".toLowerCase() == "\u00e5");
    assertTrue("\u00e5".toUpperCase() == "\u00c5");

    // Unicode uses case folding mappings.
    assertFalse(/\u00e5/iu.test("\u212b"));
    assertTrue(/\u00e5/iu.test("\u00c5"));
    assertTrue(/\u00e5/iu.test("\u00e5"));
    assertFalse(/\u00e5/iu.test("\u212b"));
    assertTrue(/\u00c5/iu.test("\u00e5"));
    assertFalse(/\u00c5/iu.test("\u212b"));
    assertTrue(/\u00c5/iu.test("\u00c5"));
    assertFalse(/\u212b/iu.test("\u00c5"));
    assertFalse(/\u212b/iu.test("\u00e5"));
    assertTrue(/\u212b/iu.test("\u212b"));

    // Non-BMP.
    assertFalse(/\u{10400}/i.test("\u{10428}"));
    assertFalse(/\u{10400}/iu.test("\u{10428}"));
    assertFalse(/\ud801\udc00/iu.test("\u{10428}"));
    assertFalse(/[\u{10428}]/iu.test("\u{10400}"));
    assertFalse(/[\ud801\udc28]/iu.test("\u{10400}"));
    assertEquals(["\uff21\u{10400}"], /[\uff40-\u{10428}]+/iu.exec("\uff21\u{10400}abc"));

    // TODO(v8:10120): Investigate why these don't behave as expected.
    {
        // Should be:
        // assertEquals(["abc"], /[^\uff40-\u{10428}]+/ui.exec("\uff21\u{10400}abc\uff23"));
        //
        // But is:
        assertEquals(["\u{ff21}"], /[^\uff40-\u{10428}]+/iu.exec("\uff21\u{10400}abc\uff23"));
    }

    assertEquals(["\uff53\u24bb"], /[\u24d5-\uff33]+/iu.exec("\uff54\uff53\u24bb\u24ba"));

    // Full mappings are ignored.
    assertFalse(/\u00df/iu.test("SS"));
    assertFalse(/\u1f8d/iu.test("\u1f05\u03b9"));

    // Simple mappings.
    assertFalse(/\u1f8d/iu.test("\u1f85"));

    // Common mappings.
    assertTrue(/\u1f6b/iu.test("\u1f63"));

    // Back references.
    assertNull(/(.)\1\1/iu.exec("\u00e5\u212b\u00c5"));
    assertNull(/(.)\1/iu.exec("\u{118aa}\u{118ca}"));

    // Non-Latin1 maps to Latin1.
    assertNull(/^\u017F/iu.exec("s"));
    assertNull(/^\u017F/iu.exec("s\u1234"));
    assertNull(/^a[\u017F]/iu.exec("as"));
    assertNull(/^a[\u017F]/iu.exec("as\u1234"));
});
