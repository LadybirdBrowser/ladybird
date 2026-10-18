// META: global=window,dedicatedworker,sharedworker
// META: script=/common/utils.js

promise_test(async () => {
    const jsonModule = await import('./bom-utf-8.txt', { with: { type: 'text' } });
    assert_equals(jsonModule.default, 'text file\n');
}, 'UTF-8 BOM should be stripped when decoding text module script');

promise_test(async () => {
    const jsonModule = await import('./bom-utf-16be.txt', { with: { type: 'text' } });
    assert_equals(jsonModule.default, '��\x00t\x00e\x00x\x00t\x00 \x00f\x00i\x00l\x00e\x00\n');
}, 'UTF-16BE with BOM should be parsed as UTF-8');

promise_test(async () => {
    const jsonModule = await import('./bom-utf-16le.txt', { with: { type: 'text' } });
    assert_equals(jsonModule.default, '��t\x00e\x00x\x00t\x00 \x00f\x00i\x00l\x00e\x00\n\x00');
}, 'UTF-16LE with BOM should be parsed as UTF-8');
