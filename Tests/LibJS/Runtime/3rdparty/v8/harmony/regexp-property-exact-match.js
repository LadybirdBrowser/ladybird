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

test.xfail("harmony/regexp-property-exact-match", () => {
    assertThrows("/\\p{In CJK}/u");
    assertThrows("/\\p{InCJKUnifiedIdeographs}/u");
    assertThrows("/\\p{InCJK}/u");
    assertThrows("/\\p{InCJK_Unified_Ideographs}/u");

    assertThrows("/\\p{InCyrillic_Sup}/u");
    assertThrows("/\\p{InCyrillic_Supplement}/u");
    assertThrows("/\\p{InCyrillic_Supplementary}/u");
    assertThrows("/\\p{InCyrillicSupplementary}/u");
    assertThrows("/\\p{InCyrillic_supplementary}/u");

    assertDoesNotThrow("/\\p{C}/u");
    assertDoesNotThrow("/\\p{Other}/u");
    assertDoesNotThrow("/\\p{Cc}/u");
    assertDoesNotThrow("/\\p{Control}/u");
    assertDoesNotThrow("/\\p{cntrl}/u");
    assertDoesNotThrow("/\\p{M}/u");
    assertDoesNotThrow("/\\p{Mark}/u");
    assertDoesNotThrow("/\\p{Combining_Mark}/u");
    assertThrows("/\\p{Combining Mark}/u");

    assertDoesNotThrow("/\\p{Script=Copt}/u");
    assertThrows("/\\p{Coptic}/u");
    assertThrows("/\\p{Qaac}/u");
    assertThrows("/\\p{Egyp}/u");
    assertDoesNotThrow("/\\p{Script=Egyptian_Hieroglyphs}/u");
    assertThrows("/\\p{EgyptianHieroglyphs}/u");

    assertThrows("/\\p{BidiClass=LeftToRight}/u");
    assertThrows("/\\p{BidiC=LeftToRight}/u");
    assertThrows("/\\p{bidi_c=Left_To_Right}/u");

    assertThrows("/\\p{Block=CJK}/u");
    assertThrows("/\\p{Block = CJK}/u");
    assertThrows("/\\p{Block=cjk}/u");
    assertThrows("/\\p{BLK=CJK}/u");
});
