String.prototype[5] = "five";
String.prototype.foo = "foo";
var lastSetThisValue = null;
class TerribleClass {
    get #privateStrict() {
        "use strict";
        lastSetThisValue = this;
    }
    get #privateNonStrict() {
        lastSetThisValue = this;
    }
    get 10() {
        "use strict";
        return this.#privateStrict;
    }
    get 11() {
        return this.#privateNonStrict;
    }
    set 12(v) {
        "use strict";
        return this.#privateStrict;
    }
    set 13(v) {
        return this.#privateNonStrict;
    }
    get strictGetPrivate() {
        "use strict";
        return this.#privateStrict;
    }
    get nonStrictGetPrivate() {
        return this.#privateNonStrict;
    }
    set strictSetPrivate(v) {
        "use strict";
        this.#privateStrict;
    }
    set nonStrictSetPrivate(v) {
        this.#privateNonStrict;
    }
}
String.prototype.__proto__ = {
    get nonStrictThis() {
        return this;
    },
    get strictThis() {
        "use strict";
        return this;
    },
    set setNonStrictThis(v) {
        lastSetThisValue = this;
    },
    set setStrictThis(v) {
        "use strict";
        lastSetThisValue = this;
    },
    get 6() {
        "use strict";
        return this;
    },
    get 7() {
        return this;
    },
    set 8(v) {
        "use strict";
        lastSetThisValue = this;
    },
    set 9(v) {
        lastSetThisValue = this;
    },
};
String.prototype.__proto__.__proto__ = new TerribleClass();

test("primitive string: numeric indexing", () => {
    expect(""[0]).toBeUndefined();
    expect("foo"[0]).toBe("f");
    expect("foo"[2]).toBe("o");
    expect("foo"[3]).toBeUndefined();
    expect("foo"[-1]).toBeUndefined();
    expect("foo"[1.5]).toBeUndefined();
    expect("foo"[5]).toBe("five");
    expect(typeof "foo"[6]).toBe("string");
    expect("foo"[6]).toBe("foo");
    expect(typeof "foo"[7]).toBe("object");
    expect("foo"[7] instanceof String).toBeTrue();
    expect("foo"[7] !== "foo"[7]).toBeTrue();
    expect("foo"[7] !== String.prototype).toBeTrue();
    "foo"[8] = "test";
    expect(typeof lastSetThisValue).toBe("string");
    expect(lastSetThisValue).toBe("foo");
    lastSetThisValue = null;
    "foo"[9] = "test";
    expect(typeof lastSetThisValue).toBe("object");
    expect(lastSetThisValue instanceof String).toBeTrue();
    expect(lastSetThisValue !== String.prototype);
    let oldThisValue = lastSetThisValue;
    lastSetThisValue = null;
    "foo"[9] = "test";
    expect(lastSetThisValue !== oldThisValue).toBeTrue();
    lastSetThisValue = null;

    expect(() => "foo"[10]).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => "foo"[11]).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => ("foo"[12] = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => ("foo"[13] = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
});
test("primitive string: string property indexing", () => {
    expect(""["0"]).toBeUndefined();
    expect("foo"["0"]).toBe("f");
    expect("foo"["01"]).toBeUndefined();
    expect("foo"[" 1"]).toBeUndefined();
    expect("foo"["1 "]).toBeUndefined();
    expect("foo"["2"]).toBe("o");
    expect("foo"["3"]).toBeUndefined();
    expect("foo"["-1"]).toBeUndefined();
    expect("foo"["1.5"]).toBeUndefined();
    expect("foo"["5"]).toBe("five");
    expect(typeof "foo"["6"]).toBe("string");
    expect("foo"["6"]).toBe("foo");
    expect(typeof "foo"["7"]).toBe("object");
    expect("foo"["7"] instanceof String).toBeTrue();
    expect("foo"["7"] !== "foo"[7]).toBeTrue();
    expect("foo"["7"] !== String.prototope).toBeTrue();
    expect(""["length"]).toBe(0);
    expect("foo"["length"]).toBe(3);
    "foo"["8"] = "test";
    expect(typeof lastSetThisValue).toBe("string");
    expect(lastSetThisValue).toBe("foo");
    lastSetThisValue = null;
    "foo"["9"] = "test";
    expect(typeof lastSetThisValue).toBe("object");
    expect(lastSetThisValue instanceof String).toBeTrue();
    expect(lastSetThisValue !== String.prototype);
    let oldThisValue = lastSetThisValue;
    lastSetThisValue = null;
    "foo"["9"] = "test";
    expect(lastSetThisValue !== oldThisValue).toBeTrue();
    lastSetThisValue = null;

    expect(() => "foo"["10"]).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => "foo"["11"]).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => ("foo"["12"] = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => ("foo"["13"] = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
});

test("primitive string: string property name access", () => {
    expect("".length).toBe(0);
    expect("foo".length).toBe(3);
    expect("foo".bar).toBeUndefined();
    expect("foo".foo).toBe("foo");
    expect(typeof "foo".strictThis).toBe("string");
    expect("foo".strictThis).toBe("foo");
    expect(typeof "foo".nonStrictThis).toBe("object");
    expect("foo".nonStrictThis !== "foo".nonStrictThis).toBeTrue();
    expect("foo".nonStrictThis !== String.prototype).toBeTrue();
    expect("foo".nonStrictThis instanceof String).toBeTrue();
    let str = new String("foo");
    str.setStrictThis = "test";
    expect(typeof lastSetThisValue).toBe("object");
    expect(lastSetThisValue).toBe(str);
    lastSetThisValue = null;
    str.setNonStrictThis = "test";
    expect(typeof lastSetThisValue).toBe("object");
    expect(lastSetThisValue instanceof String).toBeTrue();
    expect(lastSetThisValue).toBe(str);
    let oldThisValue = lastSetThisValue;
    lastSetThisValue = null;
    str.setNonStrictThis = "test";
    expect(lastSetThisValue === oldThisValue).toBeTrue();
    lastSetThisValue = null;

    expect(() => "foo".strictGetPrivate).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => "foo".nonStrictGetPrivate).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => ("foo".strictSetPrivate = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => ("foo".nonStrictSetPrivate = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
});

test("string object: string numeric indexing", () => {
    expect(new String("")[0]).toBeUndefined();
    expect(new String("foo")[0]).toBe("f");
    expect(new String("foo")[2]).toBe("o");
    expect(new String("foo")[3]).toBeUndefined();
    expect(new String("foo")[-1]).toBeUndefined();
    expect(new String("foo")[1.5]).toBeUndefined();
    expect(new String("foo")[5]).toBe("five");
    expect(typeof new String("foo")[6]).toBe("object");
    let str = new String("foo");
    expect(str[7]).toBe(str);
    expect(typeof "foo"[7]).toBe("object");
    expect(str[7] instanceof String).toBeTrue();
    expect(str[7]).toBe(str);
    expect(str[7] !== String.prototope).toBeTrue();
    str["8"] = "test";
    expect(typeof lastSetThisValue).toBe("object");
    expect(lastSetThisValue).toBe(str);
    lastSetThisValue = null;
    str["9"] = "test";
    expect(typeof lastSetThisValue).toBe("object");
    expect(lastSetThisValue instanceof String).toBeTrue();
    expect(lastSetThisValue).toBe(str);
    let oldThisValue = lastSetThisValue;
    lastSetThisValue = null;
    str["9"] = "test";
    expect(lastSetThisValue === oldThisValue).toBeTrue();
    lastSetThisValue = null;
});
test("string object: string property indexing", () => {
    expect(new String("")["0"]).toBeUndefined();
    expect(new String("foo")["0"]).toBe("f");
    expect(new String("foo")["2"]).toBe("o");
    expect(new String("foo")["3"]).toBeUndefined();
    expect(new String("foo")["-1"]).toBeUndefined();
    expect(new String("foo")["1.5"]).toBeUndefined();
    expect(new String("foo")["5"]).toBe("five");
    expect(typeof new String("foo")["6"]).toBe("object");
    let str = new String("foo");
    expect(str["7"]).toBe(str);
    expect(typeof "foo"["7"]).toBe("object");
    expect(str["7"] instanceof String).toBeTrue();
    expect(str["7"]).toBe(str);
    expect(str["7"] !== String.prototope).toBeTrue();
    str["setStrictThis"] = "test";
    expect(typeof lastSetThisValue).toBe("object");
    expect(lastSetThisValue).toBe(str);
    lastSetThisValue = null;
    str["setNonStrictThis"] = "test";
    expect(typeof lastSetThisValue).toBe("object");
    expect(lastSetThisValue instanceof String).toBeTrue();
    expect(lastSetThisValue).toBe(str);
    let oldThisValue = lastSetThisValue;
    lastSetThisValue = null;
    str["setNonStrictThis"] = "test";
    expect(lastSetThisValue === oldThisValue).toBeTrue();
    lastSetThisValue = null;

    expect(() => str.strictGetPrivate).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => str.nonStrictGetPrivate).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => (str.strictSetPrivate = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => (str.nonStrictSetPrivate = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
});
test("string object: string property name access", () => {
    expect(new String("").length).toBe(0);
    expect(new String("foo").length).toBe(3);
    expect(new String("foo").bar).toBeUndefined();
    expect(new String("foo").foo).toBe("foo");
    expect(typeof new String("foo").strictThis).toBe("object");
    let str = new String("foo");
    expect(str.strictThis).toBe(str);
    expect(typeof str.nonStrictThis).toBe("object");
    expect(str.nonStrictThis === str.nonStrictThis).toBeTrue();
    expect(str.nonStrictThis !== String.prototype).toBeTrue();
    expect(str.nonStrictThis instanceof String).toBeTrue();
    expect(new String("foo").nonStrictThis !== new String("foo").nonStrictThis).toBeTrue();
    expect(new String("foo").strictThis !== new String("foo").strictThis).toBeTrue();

    lastSetThisValue = null;
    expect(() => str.strictGetPrivate).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => str.nonStrictGetPrivate).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => (str.strictSetPrivate = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
    expect(() => (str.nonStrictSetPrivate = "wat")).toThrow();
    expect(lastSetThisValue).toBeNull();
    lastSetThisValue = null;
});
