test("basic functionality", () => {
    function A() {}
    function B() {}

    A.prototype = new B();
    const C = new A();

    expect(A.prototype.isPrototypeOf(C)).toBeTrue();
    expect(B.prototype.isPrototypeOf(C)).toBeTrue();

    expect(A.isPrototypeOf(C)).toBeFalse();
    expect(B.isPrototypeOf(C)).toBeFalse();

    const D = new Object();
    expect(Object.prototype.isPrototypeOf(D)).toBeTrue();
    expect(Function.prototype.isPrototypeOf(D.toString)).toBeTrue();
    expect(Array.prototype.isPrototypeOf([])).toBeTrue();
});
