// Test that numeric property keys in object literals correctly emit
// ToPrimitiveWithStringHint before PutOwnByValue.

var obj = { 20: 3 };
