// For-in loops exit where the property iterator cache stops holding their object's keys, and the keys their bodies
// read come straight from the cache. These loops change the shape of the object they iterate over while they run,
// leave early, nest, read keys after the loop, read keys of other objects, and iterate over objects with accessors,
// indices and dictionary shapes.
function keysAddingProperties(o) {
    let s = "";
    for (const k in o) {
        s += k;
        if (k === "b") o["added" + s.length] = 1;
    }
    return s;
}

function keysDeletingProperties(o) {
    let s = "";
    for (const k in o) {
        s += k + o[k];
        delete o.c;
    }
    return s;
}

function nested(a, b) {
    let n = 0;
    for (const x in a) for (const y in b) if (a.hasOwnProperty(x) && b[y] !== a[x]) n += x.length + y.length;
    return n;
}

function lastKey(o) {
    let last;
    for (var k in o) {
        if (k === "stop") break;
        last = k;
    }
    return [last, k];
}

function inherited(o) {
    let s = "";
    for (const k in o) s += k + (o.hasOwnProperty(k) ? "!" : "?") + o[k];
    return s;
}

function valuesHere(o, other) {
    let s = "";
    for (const k in o) s += k + o[k] + other[k] + o.hasOwnProperty(k) + other.hasOwnProperty(k) + ",";
    return s;
}

function withAccessors(i) {
    return {
        a: i,
        get b() {
            if (i % 4 === 0) delete this.c;
            else this.c = "set" + i;
            return "got";
        },
        c: 3,
        d: 4,
    };
}

function dictionary(i) {
    const o = { a: 1, b: 2, c: 3, d: i };
    delete o.b;
    o.e = 5;
    return o;
}

const proto = { p: 1 };
let out = [];
for (let i = 0; i < 300; i++) {
    out.push(keysAddingProperties({ a: 1, b: 2, c: 3 }));
    out.push(keysDeletingProperties({ a: 1, b: 2, c: 3, d: 4 }));
    out.push(nested({ a: 1, bb: 2 }, { a: 1, c: i }));
    out.push(lastKey(i % 3 ? { x: 1, y: 2 } : { x: 1, stop: 2, y: 3 }).join());
    const child = Object.create(proto);
    child.q = i;
    out.push(inherited(i % 2 ? child : { q: i }));
    const array = [i, 2, 3];
    array.x = "named";
    const others = [{ a: 9, b: 8, c: 7, d: 6 }, { d: 6, c: 7 }, withAccessors(i + 1), array, dictionary(i + 2)];
    const objects = [{ a: i, b: 2, c: 3, d: 4 }, withAccessors(i), array, dictionary(i), child];
    out.push(valuesHere(objects[i % objects.length], others[(i >> 2) % others.length]));
}
print(out.slice(0, 10).join("|"));
print(out.slice(-10).join("|"));
print(out.length);
let hash = 0;
for (const c of out.join("|")) hash = (hash * 31 + c.charCodeAt(0)) | 0;
print(hash);
