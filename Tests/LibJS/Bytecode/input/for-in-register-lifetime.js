var result = "";
var obj = { a: 1, b: 2, c: 3 };
for (var k in obj) {
    result += k + obj[k];
}
