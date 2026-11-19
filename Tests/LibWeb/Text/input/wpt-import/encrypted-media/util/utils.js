function testnamePrefix( qualifier, keysystem ) {
    return ( qualifier || '' ) + ( keysystem === 'org.w3.clearkey' ? keysystem : 'drm' );
}

function getInitData(initDataType) {

    // FIXME: This is messed up, because here we are hard coding the key ids for the different content
    //        that we use for clearkey testing: webm and mp4. For keyids we return the mp4 one
    //
    //        The content used with the DRM today servers has a different key id altogether

    if (initDataType == 'webm') {
      return new Uint8Array([
          0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
          0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
      ]);
    }

    if (initDataType == 'cenc') {
        return new Uint8Array([
            0x00, 0x00, 0x00, 0x34,   // size
            0x70, 0x73, 0x73, 0x68, // 'pssh'
            0x01, // version = 1
            0x00, 0x00, 0x00, // flags
            0x10, 0x77, 0xEF, 0xEC, 0xC0, 0xB2, 0x4D, 0x02, // Common SystemID
            0xAC, 0xE3, 0x3C, 0x1E, 0x52, 0xE2, 0xFB, 0x4B,
            0x00, 0x00, 0x00, 0x01, // key count
            0x00, 0x00, 0x00, 0x00, 0x03, 0xd2, 0xfc, 0x41, // key id
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00 // datasize
        ]);
    }
    if (initDataType == 'keyids') {
        var keyId = new Uint8Array([
            0x00, 0x00, 0x00, 0x00, 0x03, 0xd2, 0xfc, 0x41,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        ]);
        return stringToUint8Array(createKeyIDs(keyId));
    }
    throw 'initDataType ' + initDataType + ' not supported.';
}

function stringToUint8Array(str)
{
    var result = new Uint8Array(str.length);
    for(var i = 0; i < str.length; i++) {
        result[i] = str.charCodeAt(i);
    }
    return result;
}
// Encodes |data| into base64url string. There is no '=' padding, and the
// characters '-' and '_' must be used instead of '+' and '/', respectively.
function base64urlEncode(data) {
    var result = btoa(String.fromCharCode.apply(null, data));
    return result.replace(/=+$/g, '').replace(/\+/g, "-").replace(/\//g, "_");
}
// Decode |encoded| using base64url decoding.
function base64urlDecode(encoded) {
    return atob(encoded.replace(/\-/g, "+").replace(/\_/g, "/"));
}
// Decode |encoded| using base64 to a Uint8Array
function base64DecodeToUnit8Array(encoded) {
    return new Uint8Array( atob( encoded ).split('').map( function(c){return c.charCodeAt(0);} ) );
}
// Clear Key can also support Key IDs Initialization Data.
// ref: http://w3c.github.io/encrypted-media/keyids-format.html
// Each parameter is expected to be a key id in an Uint8Array.
function createKeyIDs() {
    var keyIds = '{"kids":["';
    for (var i = 0; i < arguments.length; i++) {
        if (i != 0) keyIds += '","';
        keyIds += base64urlEncode(arguments[i]);
    }
    keyIds += '"]}';
    return keyIds;
}

async function getSupportedKeySystem() {
    var userAgent = navigator.userAgent.toLowerCase();
    var keysystem = undefined;
    if (userAgent.indexOf('edge') > -1 ) {
        keysystem = 'com.microsoft.playready';
    } else if (userAgent.indexOf('chrome') > -1) {
        keysystem = 'com.widevine.alpha';
    } else if (userAgent.indexOf('firefox') > -1) {
        keysystem = 'com.widevine.alpha';
        if (userAgent.includes("win")) {
            // Firefox on Windows may use PlayReady via Media Foundation or
            // Widevine, depending on its configuration.
            try {
                await navigator.requestMediaKeySystemAccess(
                    'com.microsoft.playready.recommendation',
                    [{ initDataTypes: ['cenc'],
                       videoCapabilities: [{ contentType: 'video/mp4;codecs="avc1.4d401e"' }] }]);
                keysystem = 'com.microsoft.playready.recommendation';
            } catch (e) {
            }
        }
    }
    return keysystem;
}

function waitForEventAndRunStep(eventName, element, func, stepTest)
{
    var eventCallback = function(event) {
        if (func)
            func(event);
    }

    element.addEventListener(eventName, stepTest.step_func(eventCallback), true);
}

function waitForEvent(eventName, element) {
    return new Promise(function(resolve) {
        element.addEventListener(eventName, resolve, true);
    })
}

var consoleDiv = null;

function consoleWrite(text)
{
    if (!consoleDiv && document.body) {
        consoleDiv = document.createElement('div');
        document.body.appendChild(consoleDiv);
    }
    var span = document.createElement('span');
    span.appendChild(document.createTextNode(text));
    span.appendChild(document.createElement('br'));
    consoleDiv.appendChild(span);
}

function forceTestFailureFromPromise(test, error, message)
{
    test.step_func(assert_unreached)(message ? message + ': ' + error.message : error);
}

// Returns an array of audioCapabilities that includes entries for a set of
// codecs that should cover all user agents.
function getPossibleAudioCapabilities()
{
    return [
        { contentType: 'audio/mp4; codecs="mp4a.40.2"' },
        { contentType: 'audio/webm; codecs="opus"' },
    ];
}

// Returns a trivial MediaKeySystemConfiguration that should be accepted,
// possibly as a subset of the specified capabilities, by all user agents.
function getSimpleConfiguration()
{
    return [ {
        initDataTypes : [ 'webm', 'cenc', 'keyids' ],
        audioCapabilities: getPossibleAudioCapabilities()
    } ];
}

// Returns a MediaKeySystemConfiguration for |initDataType| that should be
// accepted, possibly as a subset of the specified capabilities, by all
// user agents.
function getSimpleConfigurationForInitDataType(initDataType)
{
    return [ {
        initDataTypes: [ initDataType ],
        audioCapabilities: getPossibleAudioCapabilities()
    } ];
}

// Returns a promise that is fulfilled with true if |initDataType| is supported,
// by keysystem or false if not.
function isInitDataTypeSupported(keysystem,initDataType)
{
    return navigator.requestMediaKeySystemAccess(
                        keysystem, getSimpleConfigurationForInitDataType(initDataType))
        .then(function() { return true; }, function() { return false; });
}

function getSupportedInitDataTypes( keysystem )
{
    return [ 'cenc', 'keyids', 'webm' ].filter( isInitDataTypeSupported.bind( null, keysystem ) );
}

function arrayBufferAsString(buffer)
{
    var array = [];
    Array.prototype.push.apply( array, new Uint8Array( buffer ) );
    return '0x' + array.map( function( x ) { return x < 16 ? '0'+x.toString(16) : x.toString(16); } ).join('');
}

function dumpKeyStatuses(keyStatuses,short)
{
    var userAgent = navigator.userAgent.toLowerCase();
    if (userAgent.indexOf('edge') === -1) {
        if (!short) { consoleWrite("for (var entry of keyStatuses)"); }
        for (var entry of keyStatuses) {
            consoleWrite(arrayBufferAsString(entry[0]) + ": " + entry[1]);
        }
        if (!short) {
            consoleWrite("for (var keyId of keyStatuses.keys())");
            for (var keyId of keyStatuses.keys()) {
                consoleWrite(arrayBufferAsString(keyId));
            }
            consoleWrite("for (var status of keyStatuses.values())");
            for (var status of keyStatuses.values()) {
                consoleWrite(status);
            }
            consoleWrite("for (var entry of keyStatuses.entries())");
            for (var entry of keyStatuses.entries()) {
                consoleWrite(arrayBufferAsString(entry[0]) + ": " + entry[1]);
            }
            consoleWrite("keyStatuses.forEach()");
            keyStatuses.forEach(function(status, keyId) {
                consoleWrite(arrayBufferAsString(keyId) + ": " + status);
            });
        }
    } else {
        if (!short) { consoleWrite("keyStatuses.forEach()"); }
        keyStatuses.forEach(function(keyId, status) {
            consoleWrite(arrayBufferAsString(keyId) + ": " + status);
        });
    }
}

// Verify that |keyStatuses| contains just the keys in |keys.expected|
// and none of the keys in |keys.unexpected|. All keys should have status
// 'usable'. Example call: verifyKeyStatuses(mediaKeySession.keyStatuses,
// { expected: [key1], unexpected: [key2] });
function verifyKeyStatuses(keyStatuses, keys)
{
    var expected = keys.expected || [];
    var unexpected = keys.unexpected || [];

    // |keyStatuses| should have same size as number of |keys.expected|.
    assert_equals(keyStatuses.size, expected.length, "keystatuses should have expected size");

    // All |keys.expected| should be found.
    expected.map(function(key) {
        assert_true(keyStatuses.has(key), "keystatuses should have the expected keys");
        assert_equals(keyStatuses.get(key), 'usable', "keystatus value should be 'usable'");
    });

    // All |keys.unexpected| should not be found.
    unexpected.map(function(key) {
        assert_false(keyStatuses.has(key), "keystatuses should not have unexpected keys");
        assert_equals(keyStatuses.get(key), undefined, "keystatus for unexpected key should be undefined");
    });
}

// This function checks that calling |testCase.func| returns a
// rejected Promise with the error.name equal to
// |testCase.exception|.
function test_exception(testCase /*...*/) {
    var func = testCase.func;
    var exception = testCase.exception;
    var args = Array.prototype.slice.call(arguments, 1);

    // This should really be rewritten in terms of the promise_rejects_*
    // testharness utility functions, but that needs the async test involved
    // passed in, and we don't have that here.
    return func.apply(null, args).then(
        function (result) {
            assert_unreached(format_value(func));
        },
        function (error) {
            assert_not_equals(error.message, "", format_value(func));
            // `exception` is a string name for the error.  We can differentiate
            // JS Errors from DOMExceptions by checking whether
            // window[exception] exists.  If it does, expectedError is the name
            // of a JS Error subclass and window[exception] is the constructor
            // for that subclass.  Otherwise it's a name for a DOMException.
            if (window[exception]) {
                assert_throws_js(window[exception],
                                 () => { throw error; },
                                 format_value(func));
            } else {
                assert_throws_dom(exception,
                                  () => { throw error; },
                                  format_value(func));
            }
        }
    );
}

// Check that the events sequence (array of strings) matches the pattern (array of either strings, or
// arrays of strings, with the latter representing a possibly repeating sub-sequence)
function checkEventSequence(events,pattern) {
    function th(i) { return i + (i < 4 ? ["th", "st", "nd", "rd"][i] : "th"); }
    var i = 0, j=0, k=0;
    while(i < events.length && j < pattern.length) {
        if (!Array.isArray(pattern[j])) {
            assert_equals(events[i], pattern[j], "Expected " + th(i+1) + " event to be '" + pattern[j] + "'");
            ++i;
            ++j;
        } else {
            assert_equals(events[i], pattern[j][k], "Expected " + th(i+1) + " event to be '" + pattern[j][k] + "'");
            ++i;
            k = (k+1)%pattern[j].length;
            if (k === 0 && events[i] !== pattern[j][0]) {
                ++j;
            }
        }
    }
    assert_equals(i,events.length,"Received more events than expected");
    assert_equals(j,pattern.length,"Expected more events than received");
}


