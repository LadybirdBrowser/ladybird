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

test("harmony/regexp-property-invalid", () => {
    assertThrows("/\p{Block=ASCII}+/u");
    assertThrows("/\p{Block=ASCII}+/u");
    assertThrows("/\p{Block=Basic_Latin}+/u");
    assertThrows("/\p{Block=Basic_Latin}+/u");

    assertThrows("/\p{blk=CJK}+/u");
    assertThrows("/\p{blk=CJK_Unified_Ideographs}+/u");
    assertThrows("/\p{blk=CJK}+/u");
    assertThrows("/\p{blk=CJK_Unified_Ideographs}+/u");

    assertThrows("/\p{Block=ASCII}+/u");
    assertThrows("/\p{Block=ASCII}+/u");
    assertThrows("/\p{Block=Basic_Latin}+/u");
    assertThrows("/\p{Block=Basic_Latin}+/u");

    assertThrows("/\p{NFKD_Quick_Check=Y}+/u");
    assertThrows("/\p{NFKD_QC=Yes}+/u");

    assertThrows("/\p{Numeric_Type=Decimal}+/u");
    assertThrows("/\p{nt=De}+/u");

    assertThrows("/\p{Bidi_Class=Arabic_Letter}+/u");
    assertThrows("/\p{Bidi_Class=AN}+/u");

    assertThrows("/\p{ccc=OV}+/u");

    assertThrows("/\p{Sentence_Break=Format}+/u");

    assertThrows("/\\p{In}/u");
    assertThrows("/\\pI/u");
    assertThrows("/\\p{I}/u");
    assertThrows("/\\p{CJK}/u");

    assertThrows("/\\p{}/u");
});
