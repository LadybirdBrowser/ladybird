// Zero BigInts in various bases should be falsy
var a = 0n ? "truthy" : "falsy";
var b = 0x0n ? "truthy" : "falsy";
var c = 0b0n ? "truthy" : "falsy";
var d = 0o0n ? "truthy" : "falsy";

// Non-zero BigInts should be truthy
var e = 1n ? "truthy" : "falsy";
var f = 0x1n ? "truthy" : "falsy";
var g = 0b1n ? "truthy" : "falsy";

// ! and !! should agree with branch elimination
var h = !0x0n;
var i = !!0x0n;
