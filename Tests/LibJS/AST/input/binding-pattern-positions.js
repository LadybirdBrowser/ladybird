// Identifiers inside a binding pattern should carry their own start
// position. Previously they inherited the pattern's opening `[` or `{`
// position, which made source maps and devtools point at the bracket
// rather than the binding name.

let { foo, bar } = {};
let { x: aliased, y: also } = {};
let [ first, , third ] = [];
let { nested: { inner }, ...rest } = {};
let [ a, [ b, c ] ] = [];

function destructured({ p, q: r }, [ s, t ]) {
    return p + r + s + t;
}
