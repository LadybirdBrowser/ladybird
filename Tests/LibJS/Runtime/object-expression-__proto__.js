test("__proto__ property", () => {
    expect(Object.getPrototypeOf({ __proto__: null })).toBeNull();
    expect(Object.getPrototypeOf({ __proto__: Array.prototype })).toEqual(Array.prototype);
    expect(Object.getPrototypeOf({ "__proto__": Array.prototype })).toEqual(Array.prototype); // prettier-ignore
    expect(Object.getOwnPropertyNames({ __proto__: Array.prototype, test: 1 })).toEqual(["test"]);
});

test("duplicate __proto__ properties are a syntax error however the key is spelled", () => {
    expect('({ __proto__: null, "__proto__": null })').not.toEval();
    expect('({ "\\x5f_proto__": null, __proto__: null })').not.toEval();
    expect('({ "\\u005f_proto__": null, "__proto__": null })').not.toEval();
    expect('({ "__proto__": null, ["__proto__"]: null })').toEval();
    expect("({ __proto__: null, __proto__ })").toEval();
});
