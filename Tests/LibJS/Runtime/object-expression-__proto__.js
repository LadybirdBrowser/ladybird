test("__proto__ property", () => {
    expect(Object.getPrototypeOf({ __proto__: null })).toBeNull();
    expect(Object.getPrototypeOf({ __proto__: Array.prototype })).toEqual(Array.prototype);
    expect(Object.getPrototypeOf({ "__proto__": Array.prototype })).toEqual(Array.prototype); // prettier-ignore
    expect(Object.getOwnPropertyNames({ __proto__: Array.prototype, test: 1 })).toEqual(["test"]);
});
