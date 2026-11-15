// Named puts that see more shapes than their caches hold go through the VM's keyed property store cache: changes of
// own properties, additions, setters of the prototype chain, read-only and frozen objects and dictionaries, while the
// prototype changes under them.
"use strict";
const proto = {};
let setterCalls = 0;
function make(i) {
    const o = Object.create(proto);
    o["k" + (i % 40)] = i;
    if (i % 2 === 0) o.x = 0;
    if (i % 9 === 0) Object.defineProperty(o, "x", { value: "fixed", writable: false, configurable: true });
    if (i % 13 === 0) {
        o.gone = 1;
        delete o.gone;
    }
    if (i % 17 === 0) Object.freeze(o);
    return o;
}

function write(o, v) {
    try {
        o.x = v;
        return "ok";
    } catch (e) {
        return e.constructor.name;
    }
}

const objects = [];
for (let i = 0; i < 80; i++) objects.push(make(i));
const out = [];
for (let round = 0; round < 60; round++) {
    if (round === 20)
        Object.defineProperty(proto, "x", {
            set(v) {
                setterCalls++;
            },
            configurable: true,
        });
    if (round === 35) delete proto.x;
    if (round === 45) Object.defineProperty(proto, "x", { value: "proto", writable: false, configurable: true });
    let s = "";
    for (const o of objects) s += write(o, round) + ":" + o.x + ":" + Object.keys(o).length + ",";
    out.push(s);
}
let hash = 0;
for (const c of out.join("|")) hash = (hash * 31 + c.charCodeAt(0)) | 0;
print(out[0].slice(0, 200));
print(out[59].slice(-200));
print(setterCalls, hash);
