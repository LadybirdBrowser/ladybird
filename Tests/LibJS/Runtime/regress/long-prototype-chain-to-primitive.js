// https://github.com/LadybirdBrowser/ladybird/issues/3584
// Converting a pathologically-deep-prototype-chain object to a primitive used to recurse Object::internal_get til the
// native stack overflowed. The code now throws a catchable "call stack size exceeded" error instead.
test("converting an object with a very deep prototype chain to a primitive does not crash", () => {
    // Object.create() sets the prototype at creation without a cycle check — so the chain is built in linear time.
    let object = {};
    for (let i = 0; i < 1_000_000; ++i) object = Object.create(object);

    expect(() => {
        Number(object);
    }).toThrowWithMessage(InternalError, "Call stack size limit exceeded");
});
