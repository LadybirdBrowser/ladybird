test("basic Uint8Array", () => {
    var a = new Uint8Array(1);
    expect(typeof a).toBe("object");
    expect(a instanceof Uint8Array).toBe(true);
    expect(a.length).toBe(1);
    a[0] = 1;
    expect(a[0]).toBe(1);
    a[0] -= 2;
    expect(a[0]).toBe(0xff);
    ++a[0];
    expect(a[0]).toBe(0);
});

test("basic Uint16Array", () => {
    var a = new Uint16Array(1);
    expect(typeof a).toBe("object");
    expect(a instanceof Uint16Array).toBe(true);
    expect(a.length).toBe(1);
    a[0] = 1;
    expect(a[0]).toBe(1);
    a[0] -= 2;
    expect(a[0]).toBe(0xffff);
    ++a[0];
    expect(a[0]).toBe(0);
});

test("basic Uint32Array", () => {
    var a = new Uint32Array(1);
    expect(typeof a).toBe("object");
    expect(a instanceof Uint32Array).toBe(true);
    expect(a.length).toBe(1);
    a[0] = 1;
    expect(a[0]).toBe(1);
    a[0] -= 2;
    expect(a[0]).toBe(0xffffffff);
    ++a[0];
    expect(a[0]).toBe(0);
});

test("basic Int8Array", () => {
    var a = new Int8Array(1);
    expect(typeof a).toBe("object");
    expect(a instanceof Int8Array).toBe(true);
    expect(a.length).toBe(1);
    a[0] = 1;
    expect(a[0]).toBe(1);
    a[0] -= 2;
    expect(a[0]).toBe(-1);
    ++a[0];
    expect(a[0]).toBe(0);
    a[0] = 127;
    a[0]++;
    expect(a[0]).toBe(-128);
});

test("basic Int16Array", () => {
    var a = new Int16Array(1);
    expect(typeof a).toBe("object");
    expect(a instanceof Int16Array).toBe(true);
    expect(a.length).toBe(1);
    a[0] = 1;
    expect(a[0]).toBe(1);
    a[0] -= 2;
    expect(a[0]).toBe(-1);
    ++a[0];
    expect(a[0]).toBe(0);
    a[0] = 32767;
    a[0]++;
    expect(a[0]).toBe(-32768);
});

test("basic Int32Array", () => {
    var a = new Int32Array(1);
    expect(typeof a).toBe("object");
    expect(a instanceof Int32Array).toBe(true);
    expect(a.length).toBe(1);
    a[0] = 1;
    expect(a[0]).toBe(1);
    a[0] -= 2;
    expect(a[0]).toBe(-1);
    ++a[0];
    expect(a[0]).toBe(0);
    a[0] = 0x7fffffff;
    a[0]++;
    expect(a[0]).toBe(-0x80000000);
});
