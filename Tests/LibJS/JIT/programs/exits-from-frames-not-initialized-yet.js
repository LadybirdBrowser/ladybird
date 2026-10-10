// Exits from compiled code before it initialized its frame must not keep the registers the frame state lists as held in
// the frame, which are left over from earlier frames: with an exit at every check (stress-exits=1), the garbage
// collector visited one of them as a cell and crashed.
"use strict";
let __hash = 0;
function __describe(value, depth) {
    if (typeof value === "string") return JSON.stringify(value);
    if (typeof value !== "object") return String(value);
    let text = "[";
    for (let i = 0; i < value.length && i < 8; ++i) text += (i in value ? __describe(value[i], depth + 1) : "_") + ",";
    return text + "]";
}
function __record(value) {
    const text = __describe(value, 0);
    __hash = (Math.imul(__hash, 31) + text.length) | 0;
    for (let i = 0; i < text.length && i < 256; ++i) __hash = (Math.imul(__hash, 31) + text.charCodeAt(i)) | 0;
}
function __call(f) {
    try {
        __record(f());
    } catch (e) {
        __record("threw " + (e instanceof Error ? e.constructor.name + ": " + e.message : __describe(e, 0)));
    }
}
class Shape0 {}
class Shape1 extends Shape0 {
    constructor(a, b) {
        super(b, a);
    }
}
const objects = [
    { x: 1, y: 2 },
    { x: 3, y: 4 },
];
const globalObject = { x: 1, y: 2, z: 3 };
const globalArray = [1, 2, 3];
const shared = new Shape0(1, 2);
function f0(p0_0) {
    let v0 = globalObject[1];
    p0_0 = `${p0_0}-${v0}` - (v0 instanceof Shape1 ? 1 : 2) > (v0 % p0_0 < ("\u00e9" | 0));
    for (let i = 0; i < 3; ++i) {
        let v1 = new Shape1(
            (2147483647 - p0_0 ?? 1n ?? v0) >>> ((-1 ?? 1000) >> ({} ? v0 : 0.5)),
            p0_0.y >>> ((p0_0 ** []) | 0)
        );
    }
    try {
        v0 = p0_0.y % (p0_0 ? v0 : p0_0) ? (v0 | 0) >> p0_0[-1] : v0 >>> p0_0 ? 100 & [1, 2] : v0 ** v0;
    } catch (e) {}
    try {
        if (`${globalArray.z}-${3.0 ? p0_0 : 100}` | [v0 ^ 1, 0 ? 10 : p0_0]) throw new RangeError("thrown " + p0_0);
    } catch (e) {}
    p0_0 <<= [String(v0 | 0).length, p0_0.x | 0];
    let v3 = v0[[]];
}
function f1(p1_0, p1_1) {
    p1_0 -= String(f0(p1_0, p1_1) ?? p1_0 - p1_1).length;
    let v4 = [p1_0.w === (5 ?? 1n), p1_0[3], f0(...[p1_0 < p1_1]), p1_1.z];
    if ((Math.floor(v4, p1_1) === "a" in Object(p1_0)) ** p1_0[0]) {
    } else {
        for (let i = 0; i < 3; ++i) {
            __record([p1_0.w > (0 instanceof Shape1 ? 1 : 2), i.z ? v4 | v4 : p1_1 === undefined]);
            try {
                p1_1 *= ((i ? [1, 2] : p1_1) ? v4 | v4 : p1_1 != p1_1) | 0;
            } finally {
            }
        }
    }
}
function f2(p2_0) {
    try {
        if (shared !== null && typeof shared === "object") shared.w = typeof (p2_0 != p2_0) & (f0(...[]) ?? 1 ** p2_0);
        __record(f1(...[globalArray.length]));
    } catch (e) {
        __record(e instanceof Error ? e.message : e);
    } finally {
        __record(-Infinity);
    }
    try {
    } catch (e) {}
    let v6 = { w: p2_0, length: f1(p2_0, 2147483647) <= String(p2_0).slice(1) };
    for (let i = 0; i < 2; ++i) {
        let v7 = `${p2_0 >>> p2_0 >= (i ? undefined : p2_0)}-${String({} <= v6).length}`;
    }
    for (const element of [v6, p2_0]) __record(typeof element);
    for (let i = 0; i < 5; ++i) {
        __record(typeof (v6 ** 10, `${i}-${2147483647}`) === "object");
    }
    return String(f1(1.5 == v6)).length;
}
function f3(p3_0) {}
function f4(p4_0) {}
for (let i = 0; i < 100; ++i) {
    __call(() => f0(i));
    __call(() => f2(i));
}
__call(() => f1(undefined, [1.5, "two"]));
for (let i = 0; i < 100; ++i) {
    __call(() => f0(objects[i & 1]));
    __call(() => f2((i & 7) - 3));
}
__call(() => f2(new Float64Array([0.5, NaN])));
for (let i = 0; i < 100; ++i) {
    __call(() => f0(i));
    __call(() => f1(i, i * 2 + 1));
    __call(() => f2(objects[i & 1]));
}
for (let i = 0; i < 100; ++i) {
    __call(() => f0(i));
    __call(() => f1((i & 7) - 3, "s" + (i & 3)));
    __call(() => f2("s" + (i & 3)));
    __call(() => f3(i * 2 + 1));
}
