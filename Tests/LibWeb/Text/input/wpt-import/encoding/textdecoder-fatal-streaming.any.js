// META: global=window,dedicatedworker
// META: title=Encoding API: End-of-file

test(function() {
    [
        {encoding: 'utf-8', sequence: [0xC0]},
        {encoding: 'utf-16le', sequence: [0x00]},
        {encoding: 'utf-16be', sequence: [0x00]}
    ].forEach(function(testCase) {

        assert_throws_js(TypeError, function() {
            var decoder = new TextDecoder(testCase.encoding, {fatal: true});
            decoder.decode(new Uint8Array(testCase.sequence));
        }, 'Unterminated ' + testCase.encoding + ' sequence should throw if fatal flag is set');

        assert_equals(
            new TextDecoder(testCase.encoding).decode(new Uint8Array([testCase.sequence])),
            '\uFFFD',
            'Unterminated UTF-8 sequence should emit replacement character if fatal flag is unset');
    });
}, 'Fatal flag, non-streaming cases');

test(function() {

    var decoder = new TextDecoder('utf-16le', {fatal: true});
    var odd = new Uint8Array([0x00]);
    var even = new Uint8Array([0x00, 0x00]);

    assert_equals(decoder.decode(odd, {stream: true}), '');
    assert_equals(decoder.decode(odd), '\u0000');

    assert_throws_js(TypeError, function() {
        decoder.decode(even, {stream: true});
        decoder.decode(odd)
    });

    assert_throws_js(TypeError, function() {
        decoder.decode(odd, {stream: true});
        decoder.decode(even);
    });

    assert_equals(decoder.decode(even, {stream: true}), '\u0000');
    assert_equals(decoder.decode(even), '\u0000');

}, 'Fatal flag, streaming cases');

[
    {encoding: 'utf-8', bom: [0xEF, 0xBB, 0xBF], invalid: [0xFF], a: [0x41]},
    {encoding: 'utf-16le', bom: [0xFF, 0xFE], invalid: [0x00, 0xDC], a: [0x41, 0x00]},
    {encoding: 'utf-16be', bom: [0xFE, 0xFF], invalid: [0xDC, 0x00], a: [0x00, 0x41]}
].forEach(function(testCase) {

    test(function() {
        var decoder = new TextDecoder(testCase.encoding, {fatal: true});
        assert_throws_js(TypeError, function() {
            decoder.decode(new Uint8Array(testCase.invalid.concat(testCase.a)), {stream: true});
        });
        assert_equals(decoder.decode(new Uint8Array(testCase.a)), 'A');
    }, 'Fatal flag, streaming error discards the remaining input: ' + testCase.encoding);

    test(function() {
        var decoder = new TextDecoder(testCase.encoding, {fatal: true});
        assert_equals(decoder.decode(new Uint8Array(testCase.bom), {stream: true}), '');
        assert_throws_js(TypeError, function() {
            decoder.decode(new Uint8Array(testCase.invalid), {stream: true});
        });
        assert_equals(decoder.decode(new Uint8Array(testCase.bom.concat(testCase.a))), 'A');
    }, 'Fatal flag, streaming error resets BOM handling: ' + testCase.encoding);
});
