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

test("harmony/regexp-property-scripts", () => {
    function t(re, s) {
        assertTrue(re.test(s));
    }
    function f(re, s) {
        assertFalse(re.test(s));
    }

    t(/\p{Script=Common}+/u, ".");
    f(/\p{Script=Common}+/u, "supercalifragilisticexpialidocious");

    t(/\p{Script=Han}+/u, "话说天下大势，分久必合，合久必分");
    t(/\p{Script=Hani}+/u, "吾庄后有一桃园，花开正盛");
    f(/\p{Script=Han}+/u, "おはようございます");
    f(/\p{Script=Hani}+/u, "Something is rotten in the state of Denmark");

    t(/\p{Script=Latin}+/u, "Wie froh bin ich, daß ich weg bin!");
    t(/\p{Script=Latn}+/u, "It was a bright day in April, and the clocks were striking thirteen");
    f(/\p{Script=Latin}+/u, "奔腾千里荡尘埃，渡水登山紫雾开");
    f(/\p{Script=Latn}+/u, "いただきます");

    t(/\p{sc=Hiragana}/u, "いただきます");
    t(/\p{sc=Hira}/u, "ありがとうございました");
    f(/\p{sc=Hiragana}/u, "Als Gregor Samsa eines Morgens aus unruhigen Träumen erwachte");
    f(/\p{sc=Hira}/u, "Call me Ishmael");

    t(/\p{sc=Phoenician}/u, "\u{10900}\u{1091a}");
    t(/\p{sc=Phnx}/u, "\u{1091f}\u{10916}");
    f(/\p{sc=Phoenician}/u, "Arthur est un perroquet");
    f(/\p{sc=Phnx}/u, "设心狠毒非良士，操卓原来一路人");

    t(/\p{sc=Grek}/u, "ἄνδρα μοι ἔννεπε, μοῦσα, πολύτροπον, ὃς μάλα πολλὰ");
    t(/\p{sc=Greek}/u, "μῆνιν ἄειδε θεὰ Πηληϊάδεω Ἀχιλῆος");
    f(/\p{sc=Greek}/u, "高贤未服英雄志，屈节偏生杰士疑");
    f(/\p{sc=Greek}/u, "Mr. Jones, of the Manor Farm, had locked the hen-houses for the night");
});
