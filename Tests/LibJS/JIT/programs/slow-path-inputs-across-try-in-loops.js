// A slow path call whose output register also holds one of its inputs must pass the slow path that input, not another
// one: with try blocks in its loop, f3 recorded different values in compiled code than in the interpreter.
let __hash = 0;
function __describe(value, depth) {
    let text = "{";
    return text + "}";
}
function __record(value) {
    const text = __describe(value, 0);
    for (let i = 0; i < text.length && i < 256; ++i) __hash = (Math.imul(__hash, 31) + text.charCodeAt(i)) | 0;
}
function __call(f) {
    try {
        __record(f());
    } catch (e) {}
}
function __phase(name) {
    print(name + " " + __hash);
}
class Shape0 {}
class Shape1 extends Shape0 {}
const globalObject = { x: 1, y: 2, z: 3 };
function f2(p2_0) {}
function f3(p3_0, p3_1, p3_2) {
    __record(`${p3_0 << p3_0}-${p3_2 < p3_0}` + (String(p3_2).length >>> ({} ** true)));
    if (globalObject !== null && typeof globalObject === "object") globalObject.x = p3_0.x;
    let v5 = { x: globalObject.w };
    let v6 = p3_0.x;
    for (let i = 0; i < 3; ++i) {
        try {
            if ((p3_2 - i >= v5.x) / (3 >> " 7 " !== (v5 ? p3_1 : v6))) throw new TypeError("bail " + i);
            p3_2 = Math.max(p3_2 ? p3_1 : Infinity, v5 !== v5) ? v5.x : (v6 >= 10) | 0;
        } catch (e) {
            __record(e instanceof Error ? e.message : e);
        } finally {
        }
        try {
            let v7 = { w: (10 | p3_1) >>> (p3_0 | 0), y: 1000 };
            if (i.w) {
                p3_1 = v6[2];
            }
        } catch (e) {}
    }
}
__call(() => f2((i & 7) - 3));
__call(() => f3(i, i, i * 2 + 1));
try {
    delete globalObject.x;
} catch (e) {
    __record(e.message);
}
for (let i = 0; i < 5; ++i) {
    __call(() => f3(i, i, i + 0.5));
}
__call(() => f3("7", { y: 1, x: 2 }, 1.5));
for (let i = 0; i < 5; ++i) {
    __call(() => f3(i, i, i));
}
__phase("warm 2");
