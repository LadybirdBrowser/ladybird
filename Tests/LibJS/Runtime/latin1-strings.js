// NB: This file spells characters above U+00FF as escapes, so that its source is Latin-1 as well.

describe("strings with code units up to U+00FF", () => {
    test("case conversion", () => {
        expect("café".toUpperCase()).toBe("CAFÉ");
        expect("CAFÉ".toLowerCase()).toBe("café");
        expect("ÿ".toUpperCase()).toBe("\u0178");
        expect("µ".toUpperCase()).toBe("\u039C");
        expect("ß".toUpperCase()).toBe("SS");
        expect("café".toLocaleUpperCase("tr")).toBe("CAFÉ");
    });

    test("code units", () => {
        const string = "crème brûlée";
        expect(string).toHaveLength(12);
        expect(string.charCodeAt(2)).toBe(0xe8);
        expect(string.codePointAt(10)).toBe(0xe9);
        expect(string.at(-2)).toBe("é");
        expect([...string]).toHaveLength(12);
        expect(string.indexOf("brû")).toBe(6);
        expect(string.split(" ")).toEqual(["crème", "brûlée"]);
        expect(string.slice(8)).toBe("ûlée");
        expect(String.fromCharCode(0x63, 0xe9)).toBe("cé");
    });

    test("equal to the same text stored with two bytes per code unit", () => {
        const wide = ["c", "a", "f", "é", "\u20AC"].join("").slice(0, 4);
        expect(wide).toBe("café");
        expect(new Map([["café", 1]]).get(wide)).toBe(1);
        expect({ café: 1 }[wide]).toBe(1);
        expect(new Set(["café", wide]).size).toBe(1);
        expect(wide.localeCompare("café")).toBe(0);
        expect("caf" + "é" === "café").toBeTrue();
    });

    test("ordering", () => {
        expect("é" < "\u20AC").toBeTrue();
        expect("ÿ" > "é").toBeTrue();
        expect(["é", "\u20AC", "f", "e"].sort()).toEqual(["e", "f", "é", "\u20AC"]);
        expect(["é", "f", "e"].sort(new Intl.Collator("en").compare)).toEqual(["e", "é", "f"]);
        expect("é".localeCompare("f", "en")).toBe(-1);
    });

    test("concatenation and building", () => {
        expect("é".repeat(3)).toBe("ééé");
        expect(["à", "b", "ç"].join("-")).toBe("à-b-ç");
        expect(`${"é"}${1}`).toBe("é1");
        expect("é" + "\u20AC").toHaveLength(2);
        expect("é".padStart(3, "à")).toBe("ààé");
        expect("a-é".replace("-", "ü")).toBe("aüé");
        expect("aéa".replaceAll("a", "\u20AC")).toBe("\u20ACé\u20AC");
        expect(String.raw`é${1}`).toBe("é1");
    });

    test("JSON", () => {
        expect(JSON.stringify({ clé: "valeur é" })).toBe('{"clé":"valeur é"}');
        const reviver = (key, value) => (typeof value === "string" ? value + "!" : value);
        expect(JSON.parse('{"clé":"é"}', reviver)).toEqual({ clé: "é!" });
    });

    test("URI functions", () => {
        expect(encodeURIComponent("é")).toBe("%C3%A9");
        expect(decodeURIComponent("%C3%A9")).toBe("é");
        expect(escape("é")).toBe("%E9");
        expect(unescape("%E9")).toBe("é");
    });

    test("regular expressions", () => {
        expect(/é/.test("café")).toBeTrue();
        expect(/É/i.test("café")).toBeTrue();
        expect(/ÿ/iu.test("\u0178")).toBeTrue();
        expect(/^.$/u.test("é")).toBeTrue();
        expect(/[à-ÿ]+/.exec("crème")[0]).toBe("è");
        expect("crème brûlée".match(/[^\x00-\x7f]/g)).toEqual(["è", "û", "é"]);
        expect("é\u20AC".replace(/\u20AC/, "e")).toBe("ée");
    });

    test("normalization", () => {
        expect("e\u0301".normalize("NFC")).toBe("é");
        expect("é".normalize("NFD")).toBe("e\u0301");
    });

    test("Intl", () => {
        expect([...new Intl.Segmenter("en", { granularity: "grapheme" }).segment("café")]).toHaveLength(4);
        expect(() => new Intl.Locale("é")).toThrowWithMessage(RangeError, "é is not a structurally valid language tag");
    });
});
