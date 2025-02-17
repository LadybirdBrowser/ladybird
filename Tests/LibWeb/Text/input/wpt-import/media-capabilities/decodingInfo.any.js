// META: timeout=long
'use strict';

// Minimal VideoConfiguration that will be allowed per spec. All optional
// properties are missing.
const minimalVideoConfiguration = {
  contentType: 'video/webm; codecs="vp09.00.10.08"',
  width: 800,
  height: 600,
  bitrate: 3000,
  framerate: 24,
};

// Minimal AudioConfiguration that will be allowed per spec. All optional
// properties are missing.
const minimalAudioConfiguration = {
  contentType: 'audio/webm; codecs="opus"',
};

// AudioConfiguration with optional spatialRendering param.
const audioConfigurationWithSpatialRendering = {
  contentType: 'audio/webm; codecs="opus"',
  spatialRendering: true,
};

// VideoConfiguration with optional hdrMetadataType, colorGamut, and
// transferFunction properties.
const videoConfigurationWithDynamicRange = {
  contentType: 'video/webm; codecs="vp09.00.10.08.00.09.16.09.00"',
  width: 800,
  height: 600,
  bitrate: 3000,
  framerate: 24,
  hdrMetadataType: 'smpteSt2086',
  colorGamut: 'rec2020',
  transferFunction: 'pq',
};


promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo());
}, "Test that decodingInfo rejects if it doesn't get a configuration");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({}));
}, "Test that decodingInfo rejects if the MediaConfiguration isn't valid");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    video: minimalVideoConfiguration,
    audio: minimalAudioConfiguration,
  }));
}, "Test that decodingInfo rejects if the MediaConfiguration does not have a type");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
  }));
}, "Test that decodingInfo rejects if the configuration doesn't have an audio or video field");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: -1,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration has a negative framerate");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 0,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration has a framerate set to 0");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: Infinity,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration has a framerate set to Infinity");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'fgeoa',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration contentType doesn't parse");

// See https://mimesniff.spec.whatwg.org/#example-valid-mime-type-string
promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm;',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration contentType is not a valid MIME type string");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'audio/fgeoa',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration contentType isn't of type video");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'application/ogg; codec=vorbis',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration contentType is of type audio");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: {
      contentType: 'application/ogg; codec=theora',
      channels: 2,
    },
  }));
}, "Test that decodingInfo rejects if the audio configuration contentType is of type video");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"; foo="bar"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration contentType has more than one parameter");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; foo="bar"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration contentType has one parameter that isn't codecs");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
    },
  }));
}, "Test that decodingInfo rejects if the video configuration contentType does not imply a single media codec but has no codecs parameter");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08, vp8"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
    }
  }));
}, "Test that decodingInfo rejects if the video configuration contentType has a codecs parameter that indicates multiple video codecs");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08, opus"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
    }
  }));
}, "Test that decodingInfo rejects if the video configuration contentType has a codecs parameter that indicates both an audio and a video codec");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: '24000/1001',
    }
  }));
}, "Test that decodingInfo rejects framerate in the form of x/y");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: '24000/0',
    }
  }));
}, "Test that decodingInfo rejects framerate in the form of x/0");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: '0/10001',
    }
  }));
}, "Test that decodingInfo rejects framerate in the form of 0/y");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: '-24000/10001',
    }
  }));
}, "Test that decodingInfo rejects framerate in the form of -x/y");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: '24000/-10001',
    }
  }));
}, "Test that decodingInfo rejects framerate in the form of x/-y");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: '24000/',
    }
  }));
}, "Test that decodingInfo rejects framerate in the form of x/");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: '1/3x',
    }
  }));
}, "Test that decodingInfo rejects framerate with trailing unallowed characters");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: { contentType: 'fgeoa' },
  }));
}, "Test that decodingInfo rejects if the audio configuration contentType doesn't parse");

// See https://mimesniff.spec.whatwg.org/#example-valid-mime-type-string
promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: { contentType: 'audio/mpeg;' },
  }));
}, "Test that decodingInfo rejects if the audio configuration contentType is not a valid MIME type string");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: { contentType: 'video/fgeoa' },
  }));
}, "Test that decodingInfo rejects if the audio configuration contentType isn't of type audio");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: { contentType: 'audio/webm; codecs="opus"; foo="bar"' },
  }));
}, "Test that decodingInfo rejects if the audio configuration contentType has more than one parameter");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: { contentType: 'audio/webm; foo="bar"' },
  }));
}, "Test that decodingInfo rejects if the audio configuration contentType has one parameter that isn't codecs");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: { contentType: 'audio/webm' },
  }));
}, "Test that decodingInfo rejects if the audio configuration contentType does not imply a single media codec but has no codecs parameter");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: { contentType: 'audio/webm; codecs="vorbis, opus"' },
  }));
}, "Test that decodingInfo rejects if the audio configuration contentType has a codecs parameter that indicates multiple audio codecs");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: { contentType: 'audio/webm; codecs="vp09.00.10.08, opus"' },
  }));
}, "Test that decodingInfo rejects if the audio configuration contentType has a codecs parameter that indicates both an audio and a video codec");

promise_test(t => {
  return navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: minimalVideoConfiguration,
    audio: minimalAudioConfiguration,
  }).then(ability => {
    assert_equals(typeof ability.supported, "boolean");
    assert_equals(typeof ability.smooth, "boolean");
    assert_equals(typeof ability.powerEfficient, "boolean");
    assert_equals(typeof ability.keySystemAccess, "object");
  });
}, "Test that decodingInfo returns a valid MediaCapabilitiesInfo objects");

async_test(t => {
  var validTypes = [ 'file', 'media-source' ];
  var invalidTypes = [ undefined, null, '', 'foobar', 'mse', 'MediaSource',
                       'record', 'transmission' ];

  var validPromises = [];
  var invalidCaught = 0;

  validTypes.forEach(type => {
    validPromises.push(navigator.mediaCapabilities.decodingInfo({
      type: type,
      video: minimalVideoConfiguration,
      audio: minimalAudioConfiguration,
    }));
  });

  // validTypes are tested via Promise.all(validPromises) because if one of the
  // promises fail, Promise.all() will reject. This mechanism can't be used for
  // invalid types which will be tested individually and increment invalidCaught
  // when rejected until the amount of rejection matches the expectation.
  Promise.all(validPromises).then(t.step_func(() => {
    for (var i = 0; i < invalidTypes.length; ++i) {
      navigator.mediaCapabilities.decodingInfo({
        type: invalidTypes[i],
        video: minimalVideoConfiguration,
        audio: minimalAudioConfiguration,
      }).then(t.unreached_func(), t.step_func(e => {
        assert_equals(e.name, 'TypeError');
        ++invalidCaught;
        if (invalidCaught == invalidTypes.length)
          t.done();
      }));
    }
  }), t.unreached_func('Promise.all should not reject for valid types'));
}, "Test that decodingInfo rejects if the MediaConfiguration does not have a valid type");

promise_test(t => {
  return navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    audio: audioConfigurationWithSpatialRendering,
  }).then(ability => {
    assert_equals(typeof ability.supported, "boolean");
    assert_equals(typeof ability.smooth, "boolean");
    assert_equals(typeof ability.powerEfficient, "boolean");
    assert_equals(typeof ability.keySystemAccess, "object");
  });
}, "Test that decodingInfo with spatialRendering set returns a valid MediaCapabilitiesInfo objects");

promise_test(t => {
  return navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: videoConfigurationWithDynamicRange,
  }).then(ability => {
    assert_equals(typeof ability.supported, "boolean");
    assert_equals(typeof ability.smooth, "boolean");
    assert_equals(typeof ability.powerEfficient, "boolean");
    assert_equals(typeof ability.keySystemAccess, "object");
  });
}, "Test that decodingInfo with hdrMetadataType, colorGamut, and transferFunction set returns a valid MediaCapabilitiesInfo objects");

promise_test(t => {
  // VP9 has a default color space of BT.709 in the codec string. So this will
  // mismatch against the provided colorGamut and transferFunction.
  let bt709Config = videoConfigurationWithDynamicRange;
  bt709Config.contentType = 'video/webm; codecs="vp09.00.10.08"';
  return navigator.mediaCapabilities
      .decodingInfo({
        type: 'file',
        video: bt709Config,
      })
      .then(ability => {
        assert_equals(typeof ability.supported, 'boolean');
        assert_equals(typeof ability.smooth, 'boolean');
        assert_equals(typeof ability.powerEfficient, 'boolean');
        assert_equals(typeof ability.keySystemAccess, 'object');
        assert_false(ability.supported);
      });
}, 'Test that decodingInfo with mismatched codec color space is unsupported');

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
      hdrMetadataType: ""
    },
  }));
}, "Test that decodingInfo rejects if the video configuration has an empty hdrMetadataType");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
      colorGamut: true
    },
  }));
}, "Test that decodingInfo rejects if the video configuration has a colorGamut set to true");

promise_test(t => {
  return promise_rejects_js(t, TypeError, navigator.mediaCapabilities.decodingInfo({
    type: 'file',
    video: {
      contentType: 'video/webm; codecs="vp09.00.10.08"',
      width: 800,
      height: 600,
      bitrate: 3000,
      framerate: 24,
      transferFunction: 3
    },
  }));
}, "Test that decodingInfo rejects if the video configuration has a transferFunction set to 3");
