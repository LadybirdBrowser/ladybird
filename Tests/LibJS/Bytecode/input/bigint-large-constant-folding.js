// Large BigInt constant folding that exceeds i128 range.
var a = 0xfedcba9876543210n * 0xfedcba9876543210n;
var b = 0xffffffffffffffffffffffffffffffffn + 1n;
var c = 0xfedcba9876543210n ** 3n;
