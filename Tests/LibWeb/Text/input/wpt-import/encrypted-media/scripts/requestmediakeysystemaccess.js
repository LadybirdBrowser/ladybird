function runTest(config, qualifier) {

    var prefix = testnamePrefix(qualifier, config.keysystem) + ', requestMediaKeySystemAccess: ';

    function expect_error(keySystem, configurations, expectedError, testname) {

        var audioCapabilities = configurations.length ? configurations[0].audioCapabilities : undefined,
            videoCapabilities = configurations.length ? configurations[0].videoCapabilities : undefined,
            audiocontenttypes = audioCapabilities ? audioCapabilities.map( function(ac) { return "'" + ac.contentType + "'"; } ).join(',') : '',
            videocontenttypes = videoCapabilities ? videoCapabilities.map( function(ac) { return "'" + ac.contentType + "'"; } ).join(',') : '',
            modifiedtestname = testname.replace( '%audiocontenttype', audiocontenttypes ).replace( '%videocontenttype', videocontenttypes );

        promise_test(function(test) {
            var p = navigator.requestMediaKeySystemAccess(keySystem, configurations);
            // expectedError is a string name for the error.  We can differentiate
            // JS Errors from DOMExceptions by checking whether
            // window[expectedError] exists.  If it does, expectedError is the name
            // of a JS Error subclass and window[expectedError] is the constructor
            // for that subclass.  Otherwise it's a name for a DOMException.
            if (window[expectedError]) {
                return promise_rejects_js(test, window[expectedError], p);
            } else {
                return promise_rejects_dom(test, expectedError, p);
            }
        }, prefix + modifiedtestname + ' should result in ' + expectedError );
    }

    function assert_subset(actual, expected, path) {
        if (typeof expected == 'string') {
            assert_equals(actual, expected, path);
        } else {
            if (expected.hasOwnProperty('length')) {
                assert_equals(actual.length, expected.length, path + '.length');
            }
            for (property in expected) {
                assert_subset(actual[property], expected[property], path + '.' + property);
            }
        }
    }

    function expect_config(keySystem, configurations, expectedConfiguration, testname) {
        promise_test(function(test) {
            return navigator.requestMediaKeySystemAccess(keySystem, configurations).then(function(a) {
                assert_subset(a.getConfiguration(), expectedConfiguration, testname + ': ');
            });
        }, testname);
    }

    // Tests for Key System.
    expect_error('', [{}], 'TypeError', 'Empty Key System');
    expect_error('com.example.unsupported', [{}], 'NotSupportedError', 'Unsupported Key System');
    expect_error(config.keysystem + '.', [{}], 'NotSupportedError', 'Key System ending in "."');
    expect_error(config.keysystem.toUpperCase(), [{}], 'NotSupportedError', 'Capitalized Key System');
    expect_error(config.keysystem + '\u028F', [{}], 'NotSupportedError', 'Non-ASCII Key System');

    // Parent of Clear Key not supported.
    expect_error(config.keysystem.match(/^(.*?)\./)[1], [{}], 'NotSupportedError', 'Root domain of Key System alone');
    expect_error(config.keysystem.match(/^(.*?)\./)[0], [{}], 'NotSupportedError', 'Root domain of Key System, with dot');
    expect_error(config.keysystem.match(/^(.*?\..*?)\./)[1], [{}], 'NotSupportedError', 'Domain of Key System along');
    expect_error(config.keysystem.match(/^(.*?\..*?)\./)[0], [{}], 'NotSupportedError', 'Domain of Key System, with dot');

    // Child of Clear Key not supported.
    expect_error(config.keysystem+'.foo', [{}], 'NotSupportedError', 'Child of Key System');

    // Prefixed Clear Key not supported.
    expect_error('webkit-'+config.keysystem, [{}], 'NotSupportedError', 'Prefixed Key System');

    // Incomplete names.
    expect_error(config.keysystem.substr(0,7)+config.keysystem.substr(8), [{}], 'NotSupportedError', 'Missing characters in middle of Key System name');
    expect_error(config.keysystem.substr(0,config.keysystem.length-1), [{}], 'NotSupportedError', 'Missing characters at end of Key System name');

    // Spaces in key system name not supported.
    expect_error(' '+config.keysystem, [{}], 'NotSupportedError', 'Leading space in Key System name');
    expect_error(config.keysystem.substr(0,6) + ' ' + config.keysystem.substr(6), [{}], 'NotSupportedError', 'Extra space in Key System name');
    expect_error(config.keysystem + ' ', [{}], 'NotSupportedError', 'Trailing space in Key System name');

    // Extra dots in key systems names not supported.
    expect_error('.' + config.keysystem, [{}], 'NotSupportedError', 'Leading dot in Key System name');
    expect_error(config.keysystem.substr(0,6) + '.' + config.keysystem.substr(6), [{}], 'NotSupportedError', 'Extra dot in middle of Key System name');
    expect_error(config.keysystem + '.', [{}], 'NotSupportedError', 'Trailing dot in Key System name');

    // Key system name is case sensitive.
    if (config.keysystem !== config.keysystem.toUpperCase()) {
        expect_error(config.keysystem.toUpperCase(), [{}], 'NotSupportedError', 'Key System name is case sensitive');
    }

    if (config.keysystem !== config.keysystem.toLowerCase()) {
        expect_error(config.keysystem.toLowerCase(), [{}], 'NotSupportedError', 'Key System name is case sensitive');
    }

    // Tests for trivial configurations.
    expect_error(config.keysystem, [], 'TypeError', 'Empty supportedConfigurations');
    expect_error(config.keysystem, [{}], 'NotSupportedError', 'Empty configuration');

    // Various combinations of supportedConfigurations.
    expect_config(config.keysystem, [{
        initDataTypes: [config.initDataType],
        audioCapabilities: [{contentType: config.audioType}],
        videoCapabilities: [{contentType: config.videoType}],
        label: 'abcd',
    }], {
        initDataTypes: [config.initDataType],
        audioCapabilities: [{contentType: config.audioType}],
        videoCapabilities: [{contentType: config.videoType}],
        label: 'abcd',
    }, 'Basic supported configuration');

    expect_config(config.keysystem, [{
        initDataTypes: ['fakeidt', config.initDataType],
        audioCapabilities: [{contentType: 'audio/fake'}, {contentType: config.audioType}],
        videoCapabilities: [{contentType: 'video/fake'}, {contentType: config.videoType}],
    }], {
        initDataTypes: [config.initDataType],
        audioCapabilities: [{contentType: config.audioType}],
        videoCapabilities: [{contentType: config.videoType}],
    }, 'Partially supported configuration');

    expect_config(config.keysystem, [{
        audioCapabilities: [{contentType: config.audioType}],
    }], {
        audioCapabilities: [{contentType: config.audioType}],
    }, 'Supported audio codec');

    expect_config(config.keysystem, [{
        audioCapabilities: [{contentType: config.audioType.replace(/^(.*?);(.*)/, "$1;  $2")}],
    }], {
        audioCapabilities: [{contentType: config.audioType.replace(/^(.*?);(.*)/, "$1;  $2")}],
    }, 'ContentType formatting must be preserved');

    expect_error(config.keysystem, [{
        audioCapabilities: [{contentType: 'audio/webm; codecs=fake'}],
    }], 'NotSupportedError', 'Unsupported audio codec (%audiocontenttype)');

    expect_error(config.keysystem, [{
        audioCapabilities: [{contentType: 'video/webm; codecs=fake'}],
    }], 'NotSupportedError', 'Unsupported video codec (%videocontenttype)');

    expect_error(config.keysystem, [{
        audioCapabilities: [
            {contentType: 'audio/webm; codecs=mp4a'},
            {contentType: 'audio/webm; codecs=mp4a.40.2'}
        ],
    }], 'NotSupportedError', 'Mismatched audio container/codec (%audiocontenttype)');

    expect_error(config.keysystem, [{
        audioCapabilities: [{contentType: config.videoType}],
    }], 'NotSupportedError', 'Video codec specified in audio field (%audiocontenttype)');

    expect_error(config.keysystem, [{
        videoCapabilities: [{contentType: config.audioType}],
    }], 'NotSupportedError', 'Audio codec specified in video field (%videocontenttype)');

    expect_error(config.keysystem, [{
        audioCapabilities: [
            {contentType: 'audio/webm; codecs=avc1'},
            {contentType: 'audio/webm; codecs=avc1.42e01e'}
        ],
    }], 'NotSupportedError', 'Mismatched audio container/codec (%audiocontenttype)');

    expect_error(config.keysystem, [{
        audioCapabilities: [
            {contentType: 'audio/mp4; codecs=vorbis'}
        ],
    }], 'NotSupportedError', 'Mismatched audio container/codec (%audiocontenttype)');

    expect_config(config.keysystem, [{
        initDataTypes: ['fakeidt'],
        videoCapabilities: [{contentType: config.videoType}]
      }, {
        initDataTypes: [config.initDataType],
        videoCapabilities: [{contentType: config.videoType}]
      }
    ], {
        initDataTypes: [config.initDataType],
        videoCapabilities: [{contentType: config.videoType}]
    }, 'Two configurations, one supported');

    expect_config(config.keysystem, [{
        initDataTypes: [config.initDataType],
        videoCapabilities: [{contentType: config.videoType}]
      }, {
        videoCapabilities: [{contentType: config.videoType}]
      }
    ], {
        initDataTypes: [config.initDataType],
        videoCapabilities: [{contentType: config.videoType}]
    }, 'Two configurations, both supported');

    // Audio MIME type does not support video codecs.
    expect_error(config.keysystem, [{
        audioCapabilities: [
            {contentType: 'audio/webm; codecs="vp8,vorbis"'},
            {contentType: 'audio/webm; codecs="vorbis, vp8"'},
            {contentType: 'audio/webm; codecs="vp8"'}
        ],
    }], 'NotSupportedError', 'Audio MIME type does not support video codecs (webm) (%audiocontenttype)');

    expect_error(config.keysystem, [{
        audioCapabilities: [
            {contentType: 'audio/mp4; codecs="avc1"'},
            {contentType: 'audio/mp4; codecs="avc1.4d401e"'},
        ],
    }], 'NotSupportedError', 'Audio MIME type does not support video codecs (mp4) (%audiocontenttype)');

    // Video MIME type does not support audio codecs.
    expect_error(config.keysystem, [{
        videoCapabilities: [
            {contentType: 'video/webm; codecs="vp8,vorbis"'},
            {contentType: 'video/webm; codecs="vorbis, vp8"'},
            {contentType: 'video/webm; codecs="vorbis"'}
        ],
    }], 'NotSupportedError', 'Video MIME type does not support audio codecs (webm) (%videocontenttype)');

    expect_error(config.keysystem, [{
        videoCapabilities: [
            {contentType: 'video/mp4; codecs="mp4a"'},
            {contentType: 'video/mp4; codecs="mp4a.40.2"'}
        ],
    }], 'NotSupportedError', 'Video MIME type does not support audio codecs (mp4) (%videocontenttype)');

    // WebM does not support AVC1/AAC.
    expect_error(config.keysystem, [{
        audioCapabilities: [
            {contentType: 'audio/webm; codecs="aac"'},
            {contentType: 'audio/webm; codecs="avc1"'},
            {contentType: 'audio/webm; codecs="vp8,aac"'}
        ],
    }], 'NotSupportedError', 'WebM audio does not support AVC1/AAC (%audiocontenttype)');

    expect_error(config.keysystem, [{
        videoCapabilities: [
            {contentType: 'video/webm; codecs="aac"'},
            {contentType: 'video/webm; codecs="avc1"'},
            {contentType: 'video/webm; codecs="vp8,aac"'}
        ],
    }], 'NotSupportedError', 'WebM video does not support AVC1/AAC (%videocontenttype)');

    // Extra space is allowed in contentType.
    expect_config(config.keysystem, [{
        videoCapabilities: [{contentType: ' ' + config.videoType}],
    }], {
        videoCapabilities: [{contentType: ' ' + config.videoType}],
    }, 'Leading space in contentType');

    expect_config(config.keysystem, [{
        videoCapabilities: [{contentType: config.videoType.replace( /^(.*?);(.*)/, "$1 ;$2")}],
    }], {
        videoCapabilities: [{contentType: config.videoType.replace( /^(.*?);(.*)/, "$1 ;$2")}],
    }, 'Space before ; in contentType');


    expect_config(config.keysystem, [{
      videoCapabilities: [{contentType: config.videoType + ' '}],
    }], {
      videoCapabilities: [{contentType: config.videoType + ' '}],
    }, 'Trailing space in contentType');

    expect_config(config.keysystem, [{
        videoCapabilities: [{contentType: config.videoType.replace( /^(.*?codecs=\")(.*)/, "$1 $2")}],
    }], {
        videoCapabilities: [{contentType: config.videoType.replace( /^(.*?codecs=\")(.*)/, "$1 $2")}],
    }, 'Space at start of codecs parameter');

    expect_config(config.keysystem, [{
        videoCapabilities: [{contentType: config.videoType.replace( /^(.*?codecs=\".*)\"/, "$1 \"")}],
    }], {
        videoCapabilities: [{contentType: config.videoType.replace( /^(.*?codecs=\".*)\"/, "$1 \"")}],
    }, 'Space at end of codecs parameter');

    // contentType is not case sensitive (except the codec names).
    expect_config(config.keysystem, [{
        videoCapabilities: [{contentType: 'V' + config.videoType.substr(1)}],
    }], {
        videoCapabilities: [{contentType: 'V' + config.videoType.substr(1)}],
    }, 'Video/' );

    expect_config(config.keysystem, [{
        videoCapabilities: [{contentType: config.videoType.replace( /^(.*?)c(odecs.*)/, "$1C$2")}],
    }], {
        videoCapabilities: [{contentType: config.videoType.replace( /^(.*?)c(odecs.*)/, "$1C$2")}],
    }, 'Codecs=');

    var t = config.videoType.match(/(.*?)(;.*)/);
    expect_config(config.keysystem, [{
        videoCapabilities: [{contentType: t[1].toUpperCase() + t[2]}],
    }], {
        videoCapabilities: [{contentType: t[1].toUpperCase() + t[2]}],
    }, 'Upper case MIME type');

    t = config.videoType.match(/(.*?)codecs(.*)/);
    expect_config(config.keysystem, [{
        videoCapabilities: [{contentType: t[1] + 'CODECS' + t[2]}],
    }], {
        videoCapabilities: [{contentType: t[1] + 'CODECS' + t[2]}],
    }, 'CODECS=');

    // Unrecognized attributes are not allowed.
    expect_error(config.keysystem, [{
      videoCapabilities: [{contentType: 'video/webm; foo="bar"'}],
    }], 'NotSupportedError', 'Unrecognized foo with webm (%videocontenttype)');

    expect_error(config.keysystem, [{
      videoCapabilities: [{contentType: 'video/mp4; foo="bar"'}],
    }], 'NotSupportedError', 'Unrecognized foo with mp4 (%videocontenttype)');

    expect_error(config.keysystem, [{
      videoCapabilities: [{contentType: config.videoType + '; foo="bar"'}],
    }], 'NotSupportedError', 'Unrecognized foo with codecs (%videocontenttype)');

    // Invalid contentTypes.
    expect_error(config.keysystem, [{
        videoCapabilities: [{contentType: 'fake'}],
    }], 'NotSupportedError', 'contentType: %videocontenttype');

    expect_error(config.keysystem, [{
        audioCapabilities: [{contentType: 'audio/fake'}],
    }], 'NotSupportedError', 'contentType: %audiocontenttype');

    expect_error(config.keysystem, [{
        videoCapabilities: [{contentType: 'video/fake'}],
    }], 'NotSupportedError', 'contentType: %videocontenttype');

    // The actual codec names are case sensitive.
    t = config.videoType.match( /(.*?codecs=\")(.*?\")(.*)/ );
    if (t[2] !== t[2].toUpperCase()) {
        expect_error(config.keysystem, [{
            videoCapabilities: [{contentType: t[1] + t[2].toUpperCase() + t[3] }],
        }], 'NotSupportedError', 'contentType: %videocontenttype');
    }

    if (t[2] !== t[2].toLowerCase()) {
        expect_error(config.keysystem, [{
            videoCapabilities: [{contentType: t[1] + t[2].toLowerCase() + t[3] }],
        }], 'NotSupportedError', 'contentType: %videocontenttype');
    }

    // Extra comma is not allowed in codecs.
    expect_error(config.keysystem, [{
        videoCapabilities: [{contentType: t[1] + ',' + t[2] + t[3] }],
    }], 'NotSupportedError', 'contentType: %videocontenttype');
}
