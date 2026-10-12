// Strings made at run time become property keys the first time they are used as one. With a low JIT threshold the
// functions are compiled, so later accesses go through the keyed caches of JIT code.

function write(object, key, value) {
    object[key] = value;
}

function read(object, key) {
    return object[key];
}

function makeKeys() {
    const digits = "0123456789".split("");
    const unicode = "äöüåæø中文\u{1f600}";
    return {
        inline: digits.map(digit => "inline_key_" + digit),
        rope: digits.map(digit => "a_rope_that_is_too_long_for_a_cell_" + digit),
        unicodeRope: digits.map(digit => unicode + "_rope_" + digit),
        substring: digits.map(digit => ("substring_source_" + digit + "_with_a_tail_of_some_length").slice(0, 18)),
        longSubstring: digits.map(digit =>
            ("a_substring_that_is_too_long_for_a_cell_" + digit + "_and_its_tail").substring(2, 42)
        ),
        unicodeSubstring: digits.map(digit => (unicode + unicode + "_" + digit + "_tail").slice(3, 22)),
        joined: digits.map(digit => ["a_joined_key_", digit].join("")),
        lowercased: digits.map(digit => ("A_LOWERCASED_KEY_THAT_IS_TOO_LONG_FOR_A_CELL_" + digit).toLowerCase()),
        json: digits.map(digit => JSON.stringify({ digit, unicode, padding: "x".repeat(100) })),
    };
}

describe("string keys made at run time", () => {
    for (const [kind, keys] of Object.entries(makeKeys())) {
        test(`${kind} strings`, () => {
            const expected = keys.map(key => Array.from(key, character => character.codePointAt(0)));
            const object = {};
            for (let round = 0; round < 200; ++round) {
                for (let i = 0; i < keys.length; ++i) write(object, keys[i], round * keys.length + i);
            }
            for (let i = 0; i < keys.length; ++i) {
                const key = keys[i];
                expect(read(object, key)).toBe(199 * keys.length + i);
                // An equal string that was made otherwise names the same property.
                expect(read(object, key.split("").join(""))).toBe(199 * keys.length + i);
                expect(Object.hasOwn(object, key)).toBeTrue();
                // The strings are unchanged by being keys.
                expect(Array.from(key, character => character.codePointAt(0))).toEqual(expected[i]);
                expect(key.length).toBe(key.split("").length);
                expect(key + "!").toBe(key.split("").join("") + "!");
                expect(key.slice(1)).toBe(key.split("").slice(1).join(""));
            }
            expect(Object.keys(object)).toEqual(keys);
        });
    }

    test("index keys", () => {
        const array = [];
        for (let i = 0; i < 100; ++i) {
            const inlineIndex = "1000" + "0000" + (i % 10);
            const ropeIndex = "4000" + "0000" + "0";
            write(array, inlineIndex, i);
            write(array, ropeIndex.slice(0, 9), i);
        }
        expect(array.length).toBe(400000001);
        expect(array[100000009]).toBe(99);
        expect(array[400000000]).toBe(99);
        expect(read(array, "1000" + "00009")).toBe(99);
        expect(Object.keys(array).length).toBe(11);
    });
});
