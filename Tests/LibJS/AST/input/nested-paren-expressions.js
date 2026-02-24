// Deeply nested parenthesized expressions where each level looks like it
// could be an arrow function parameter with a default value.
// Without memoization of failed arrow function attempts, the parser would
// do exponential work re-attempting arrow parsing at the same positions.
var x = (a = (b = (c = (d = (e = (f = (g = (h = 0))))))));
var y = (a = [b, (c = [d, (e = 0)])]);
