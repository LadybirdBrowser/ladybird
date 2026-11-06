// NB: This file and the code handed to eval() below have one-byte storage, which the parser reads as it is.

function fonctionÉté(paramètre, àvec) {
    return paramètre + àvec;
}

class Élément {
    #privé = 1;
    get privé() {
        return this.#privé;
    }
}

test("identifiers with Latin-1 letters", () => {
    var café = 1;
    const ñandú = { clé: 2, clé2: 3 };
    expect(fonctionÉté(café, ñandú.clé)).toBe(3);
    expect(ñandú.clé2).toBe(3);
    expect(fonctionÉté.name).toBe("fonctionÉté");
    expect(new Élément().privé).toBe(1);
    expect(Object.keys(ñandú)).toEqual(["clé", "clé2"]);
});

test("escapes and Latin-1 letters spell the same identifier", () => {
    var éa = 1;
    expect(éa).toBe(1);
    var aé = 2;
    expect(aé).toBe(2);
});

test("string, template and regular expression literals", () => {
    const value = "héllo\xE9";
    expect(value.length).toBe(6);
    expect(value.charCodeAt(1)).toBe(0xe9);
    expect(value.charCodeAt(5)).toBe(0xe9);
    expect(`ÿ${value}þ`.length).toBe(8);
    expect(String.raw`é\n`.length).toBe(3);
    expect(/caf[é-ÿ]/.test("café")).toBeTrue();
    expect(/caf[é-ÿ]/.source).toBe("caf[é-ÿ]");
});

test("no-break space is whitespace, and the multiplication sign and U+0085 are neither names nor whitespace", () => {
    expect(eval("1 + 1")).toBe(2);
    expect(() => eval("var a×b;")).toThrow(SyntaxError);
    expect(() => eval("1\u0085")).toThrow(SyntaxError);
});

test("source offsets count code units", () => {
    const code = "/* éééé */ (function é(à) { return 'é' + à; })";
    const value = eval(code);
    expect(value.toString()).toBe("function é(à) { return 'é' + à; }");
    expect(value("è")).toBe("éè");
});

test("error positions count code units", () => {
    let message = "";
    try {
        eval("/* éééé */ @");
    } catch (error) {
        message = error.message;
    }
    expect(message.includes("column: 12")).toBeTrue();
});
