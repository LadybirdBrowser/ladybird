const getIteratorPrototype = () =>
    Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));

test("prototype of %IteratorPrototype% is %ObjectPrototype%", () => {
    let itProto = getIteratorPrototype();
    expect(Object.getPrototypeOf(itProto)).toBe(Object.getPrototypeOf({}));
});

test("@@iterator of %IteratorPrototype% is itself", () => {
    let itProto = getIteratorPrototype();
    expect(itProto[Symbol.iterator]()).toBe(itProto);
});
