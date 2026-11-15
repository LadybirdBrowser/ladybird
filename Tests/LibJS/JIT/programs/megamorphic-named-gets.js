// Named gets that see more shapes than their caches hold find properties in the VM's keyed property lookup cache:
// own properties, properties of the prototype chain, missing properties and getters, of objects, dictionaries and
// primitives, while the prototypes change under them.
const proto = { inherited: "p" };
function make(i) {
    const o = Object.create(proto);
    o["k" + (i % 40)] = i;
    if (i % 3 === 0) o.own = "o" + i;
    if (i % 7 === 0)
        Object.defineProperty(o, "own", {
            get() {
                return "g" + i;
            },
            configurable: true,
        });
    if (i % 11 === 0) {
        o.gone = 1;
        delete o.gone;
    }
    return o;
}

function read(o) {
    return o.own + "/" + o.inherited + "/" + o.missing;
}

const objects = [];
for (let i = 0; i < 80; i++) objects.push(make(i));
const primitives = ["str", 1.5, 7, true];
let out = [];
for (let round = 0; round < 60; round++) {
    if (round === 20) proto.inherited = "changed";
    if (round === 30) proto.missing = "now here";
    if (round === 40) delete proto.missing;
    if (round === 45)
        Object.defineProperty(proto, "inherited", {
            get() {
                return "getter";
            },
            configurable: true,
        });
    if (round === 50) String.prototype.own = "string own";
    let s = "";
    for (const o of objects) s += read(o) + ",";
    for (const p of primitives) s += read(p) + ",";
    out.push(s);
}
let hash = 0;
for (const c of out.join("|")) hash = (hash * 31 + c.charCodeAt(0)) | 0;
print(out[0].slice(0, 200));
print(out[59].slice(-200));
print(hash);
