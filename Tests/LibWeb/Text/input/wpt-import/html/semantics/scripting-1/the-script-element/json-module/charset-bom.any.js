// META: global=window,dedicatedworker,sharedworker
// META: script=/common/utils.js

promise_test(async () => {
    const jsonModule = await import('./bom-utf-8.json', { with: { type: 'json' } });
    assert_equals(jsonModule.default.data, 'hello');
}, 'UTF-8 BOM should be stripped when decoding JSON module script');

promise_test(async test => {
    await promise_rejects_js(test, SyntaxError,
        import('./bom-utf-16be.json', { with: { type: 'json' } }), 'Expected parse error from UTF-16BE BOM');
}, 'UTF-16BE BOM should result in parse error in JSON module script');

promise_test(async test => {
    await promise_rejects_js(test, SyntaxError,
        import('./bom-utf-16le.json', { with: { type: 'json' } }), 'Expected parse error from UTF-16LE BOM');
}, 'UTF-16LE BOM should result in parse error in JSON module script');
