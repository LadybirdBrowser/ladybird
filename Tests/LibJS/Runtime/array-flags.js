test("a length that is not writable sticks", () => {
    const array = [1, 2, 3];
    Object.defineProperty(array, "length", { writable: false });

    expect(Object.getOwnPropertyDescriptor(array, "length").writable).toBeFalse();
    expect(() => array.push(4)).toThrow(TypeError);
    expect(() => array.pop()).toThrow(TypeError);
    expect(array.length).toBe(3);
    array[1] = "two";
    expect(array[1]).toBe("two");
});

test("arrays that are the target of a proxy stay arrays", () => {
    const array = [1, 2, 3];
    const proxy = new Proxy(array, {});

    expect(Array.isArray(proxy)).toBeTrue();
    expect(proxy.length).toBe(3);
    proxy.push(4);
    expect(array.length).toBe(4);
    array.push(5);
    expect(proxy[4]).toBe(5);

    Object.defineProperty(array, "length", { writable: false });
    expect(() => proxy.push(6)).toThrow(TypeError);
});

test("arrays from another realm keep their own intrinsics in mind", () => {
    const array = [1, 2, 3];
    Object.setPrototypeOf(array, Object.prototype);
    expect(array.concat).toBeUndefined();
    Object.setPrototypeOf(array, Array.prototype);
    expect(array.concat([4])).toEqual([1, 2, 3, 4]);
    for (const value of array) expect(value).toBeGreaterThan(0);
});
