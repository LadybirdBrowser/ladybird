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

    test("keys that are put together again for every access", () => {
        const names = ["click", "change", "keydown", "focus", "blur", "input", "submit", "dblclick"];
        const handlers = {};
        for (const name of names) write(handlers, "on" + name, name.length);
        let sum = 0;
        for (let i = 0; i < 4000; ++i) sum += read(handlers, "on" + names[i & 7]);
        expect(sum).toBe(500 * (5 + 6 + 7 + 5 + 4 + 5 + 6 + 8));
        expect(Object.keys(handlers)).toEqual(names.map(name => "on" + name));

        // Ropes of ropes, of substrings and of strings with a NUL character find the same properties.
        const object = {};
        for (let i = 0; i < 400; ++i) {
            const digit = String(i % 10);
            write(object, "a_" + digit + ("_nested_" + digit) + "_rope", i);
            write(object, "from_" + ("a_substring_source_" + digit).slice(2, 11) + "_" + digit, i);
            write(object, "nul\0" + "_key_" + digit, i);
            write(object, "nul" + "_key_" + digit, -i);
            // 32 characters fit in a string cell, and 33 do not.
            write(object, "a_key_of_32_characters_in_all_" + digit + digit, i);
            write(object, "a_key_of_33_characters_in_total_" + digit, i);
        }
        expect(Object.keys(object).length).toBe(60);
        for (let digit = 0; digit < 10; ++digit) {
            expect(read(object, ["a_", digit, "_nested_", digit, "_rope"].join(""))).toBe(390 + digit);
            expect(read(object, `from_substring_${digit}`)).toBe(390 + digit);
            expect(read(object, `nul\0_key_${digit}`)).toBe(390 + digit);
            expect(read(object, `nul_key_${digit}`)).toBe(-390 - digit);
            expect(read(object, `a_key_of_32_characters_in_all_${digit}${digit}`)).toBe(390 + digit);
            expect(read(object, `a_key_of_33_characters_in_total_${digit}`)).toBe(390 + digit);
        }

        // A key keeps its property across a collection, which empties the VM's cache of such keys.
        gc();
        expect(read(handlers, "on" + names[7])).toBe(8);
        expect(read(object, "a_" + "9" + ("_nested_" + "9") + "_rope")).toBe(399);
    });

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
