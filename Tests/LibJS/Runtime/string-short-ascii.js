// Strings of a few ASCII characters, made by concatenation, substrings and JSON.parse, of every length around the ones
// that keep their characters inline.
const lengths = Array.from({ length: 40 }, (_, i) => i + 1);
const alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

function expectSameString(actual, expected) {
    expect(actual).toBe(expected);
    expect(actual.length).toBe(expected.length);
    for (let i = 0; i < expected.length; ++i) {
        expect(actual.charCodeAt(i)).toBe(expected.charCodeAt(i));
        expect(actual.charAt(i)).toBe(expected[i]);
        expect(actual[i]).toBe(expected[i]);
    }
    expect(actual.charCodeAt(expected.length)).toBeNaN();
    expect(actual.charAt(expected.length)).toBe("");
}

test("concatenation", () => {
    for (const length of lengths) {
        const expected = alphabet.slice(0, length);
        for (let split = 0; split <= length; ++split) {
            const lhs = alphabet.slice(0, split);
            const rhs = alphabet.slice(split, length);
            expectSameString(lhs + rhs, expected);
            expectSameString(`${lhs}${rhs}`, expected);
            expectSameString(lhs.concat(rhs), expected);
        }
    }
});

test("concatenation with non-ASCII strings", () => {
    expectSameString("abcdefgh" + "é", "abcdefghé");
    expectSameString("é" + "abcdefgh", "éabcdefgh");
    expectSameString("abcdefgh" + "\u{1F600}", "abcdefgh\u{1F600}");
});

test("substrings", () => {
    const source = alphabet + alphabet;
    for (const length of lengths) {
        for (const start of [0, 1, 7, 30]) {
            const expected = Array.from({ length }, (_, i) => source[start + i]).join("");
            expectSameString(source.slice(start, start + length), expected);
            expectSameString(source.substring(start, start + length), expected);
            expectSameString((source + "!").slice(start, start + length), expected);
        }
    }
    const nonAscii = "é" + alphabet;
    expectSameString(nonAscii.slice(1, 20), alphabet.slice(0, 19));
    expectSameString(nonAscii.slice(0, 20), "é" + alphabet.slice(0, 19));
});

test("strings of JSON texts", () => {
    for (const length of lengths) {
        const expected = alphabet.slice(0, length);
        expectSameString(JSON.parse(`"${expected}"`), expected);
        expectSameString(JSON.parse(`["${expected}"]`)[0], expected);
    }
});

test("as property keys, in collections and compared", () => {
    const object = {};
    const map = new Map();
    const set = new Set();
    for (const length of lengths) {
        const flat = alphabet.slice(0, length);
        const made = alphabet.slice(0, length >> 1) + alphabet.slice(length >> 1, length);
        object[made] = length;
        map.set(made, length);
        set.add(made);
        expect(object[flat]).toBe(length);
        expect(map.get(flat)).toBe(length);
        expect(set.has(flat)).toBeTrue();
        expect(made === flat).toBeTrue();
        expect(made < flat + "!").toBeTrue();
        expect(Object.keys(object).includes(flat)).toBeTrue();
    }
    expect(Object.keys(object)).toHaveLength(lengths.length);
    expect(map.size).toBe(lengths.length);
    expect(set.size).toBe(lengths.length);

    const index = "1234567" + "8";
    const array = [];
    array[index] = "x";
    expect(array[12345678]).toBe("x");
});

test("string methods", () => {
    const string = "Hello" + ", world" + "!!";
    expect(string).toBe("Hello, world!!");
    expect(string.toUpperCase()).toBe("HELLO, WORLD!!");
    expect(string.indexOf("world")).toBe(7);
    expect(string.split(", ")).toEqual(["Hello", "world!!"]);
    expect(string.replace("world", "there")).toBe("Hello, there!!");
    expect(string.replace(/o/g, "0")).toBe("Hell0, w0rld!!");
    expect(string.at(-1)).toBe("!");
    expect(string.startsWith("Hello")).toBeTrue();
    expect([...string]).toHaveLength(14);
    expect(String(string)).toBe(string);
    expect(new String(string).length).toBe(14);
    expect(JSON.stringify(string)).toBe('"Hello, world!!"');
    expect(parseInt("123456" + "78")).toBe(12345678);
    expect(Number("1234" + ".5678")).toBe(1234.5678);
    expect(Symbol.for("symbol" + " key") === Symbol.for("symbol key")).toBeTrue();
});
