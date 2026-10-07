test("strings with the same contents are equal however they were made", () => {
    const long = "a string constant that is too long to be stored inline";
    const built = ["a string constant", "that is too long", "to be stored inline"].join(" ");
    const concatenated = "a string constant that is " + "too long to be stored inline";
    const sliced = ("xx" + long).slice(2);
    const fromKeys = Object.keys({ [long]: 1 })[0];
    for (const other of [built, concatenated, sliced, fromKeys]) {
        expect(long === other).toBeTrue();
        expect(other === long).toBeTrue();
        expect(long == other).toBeTrue();
        expect(long !== other).toBeFalse();
    }
    expect("ab" === "a" + "b").toBeTrue();
    expect(String.fromCharCode(0x61, 0x62) === "ab").toBeTrue();
});

test("strings with different contents are not equal", () => {
    expect("abc" === "abd").toBeFalse();
    expect("abc" == "abd").toBeFalse();
    expect("a string constant that is too long to be stored inline" === "another long string constant").toBeFalse();
    const key = Object.keys({ foo: 1 })[0];
    expect(key === "bar").toBeFalse();
    expect(key === "foo").toBeTrue();
});

test("string switch", () => {
    const classify = value => {
        switch (value) {
            case "first":
                return 1;
            case "second":
                return 2;
            default:
                return 0;
        }
    };
    expect(classify("fir" + "st")).toBe(1);
    expect(classify(["sec", "ond"].join(""))).toBe(2);
    expect(classify("third")).toBe(0);
});

test("constants of different functions and typeof results", () => {
    const first = () => "a shared constant";
    const second = () => "a shared constant";
    const third = () => "another constant";
    expect(first() === second()).toBeTrue();
    expect(first() === third()).toBeFalse();
    expect(typeof 1 === "number").toBeTrue();
    expect(typeof "" === "number").toBeFalse();
    expect(typeof {} === ["obj", "ect"].join("")).toBeTrue();
    expect("é" === String.fromCharCode(0xe9)).toBeTrue();
    expect("é" === "è").toBeFalse();
    expect("" === "".slice(0)).toBeTrue();
});

test("long constants and code units of constants", () => {
    const long = "x".repeat(300);
    const longConstant = eval(`"${long}"`);
    expect(longConstant === long).toBeTrue();
    expect(longConstant === long + "y").toBeFalse();
    const constant = "abcdefghijklmnopqrstuvwxyz";
    let sum = 0;
    for (let i = 0; i < constant.length; ++i) sum += constant.charCodeAt(i);
    expect(sum).toBe(2847);
    expect(constant[3]).toBe("d");
});

test("constants and keys after garbage collection", () => {
    evaluateSource('var __internedConstant = ["an interned", "constant"].join(" ");');
    gc();
    expect(evaluateSource('"an interned constant"') === globalThis.__internedConstant).toBeTrue();
    const object = { "a property name that is long": 1 };
    gc();
    expect(Object.keys(object)[0] === "a property name that is long").toBeTrue();
    expect(Object.keys(object)[0] === "a property name that is short").toBeFalse();
});
