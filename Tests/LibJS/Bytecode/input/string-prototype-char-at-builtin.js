// Test that non-computed String.prototype.charAt calls specialize to the
// dedicated builtin opcode, while computed property access remains generic.

"abc".charAt(1);
"abc"["charAt"](1);
