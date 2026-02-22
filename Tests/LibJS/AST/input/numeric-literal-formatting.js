// Numeric literals with various formatting needs
var a = 0.1;
var b = 100000000000000000000;
var c = 0.000001;
var d = 1e20;
var e = 5e-7;
var f = 1.5;
// Values that exercise float-to-string tie-breaking edge cases
var g = 0.000035656023101182655;
var h = 0.08424758911132812;
// Overflow to Infinity
var i = 1e999;
var j = -1e999;
