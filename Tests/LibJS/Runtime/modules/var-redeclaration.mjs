var repeated = 1;
var repeated;
var other = 2,
    repeated;

for (var repeated of []) {
}

{
    var other;
}

export const passed = repeated === 1 && other === 2;
