function make_audio_data(timestamp, channels, sampleRate, frames) {
  let data = new Float32Array(frames*channels);

  // This generates samples in a planar format.
  for (var channel = 0; channel < channels; channel++) {
    let hz = 100 + channel * 50; // sound frequency
    let base_index = channel * frames;
    for (var i = 0; i < frames; i++) {
      let t = (i / sampleRate) * hz * (Math.PI * 2);
      data[base_index + i] = Math.sin(t);
    }
  }

  return new AudioData({
    timestamp: timestamp,
    data: data,
    numberOfChannels: channels,
    numberOfFrames: frames,
    sampleRate: sampleRate,
    format: "f32-planar",
  });
}

function makeOffscreenCanvas(width, height, options) {
  let canvas = new OffscreenCanvas(width, height);
  let ctx = canvas.getContext('2d', options);
  ctx.fillStyle = 'rgba(50, 100, 150, 255)';
  ctx.fillRect(0, 0, width, height);
  return canvas;
}

function makeImageBitmap(width, height) {
  return makeOffscreenCanvas(width, height).transferToImageBitmap();
}

// Gives a chance to pending output and error callbacks to complete before
// resolving.
function endAfterEventLoopTurn() {
  return new Promise(resolve => step_timeout(resolve, 0));
}

// Returns a codec initialization with callbacks that expected to not be called.
function getDefaultCodecInit(test) {
  return {
    output: test.unreached_func("unexpected output"),
    error: test.unreached_func("unexpected error"),
  }
}

// Checks that codec can be configured, reset, reconfigured, and that incomplete
// or invalid configs throw errors immediately.
function testConfigurations(codec, validConfig, unsupportedCodecsList) {
  assert_equals(codec.state, "unconfigured");

  const requiredConfigPairs = validConfig;
  let incrementalConfig = {};

  for (let key in requiredConfigPairs) {
    // Configure should fail while required keys are missing.
    assert_throws_js(TypeError, () => { codec.configure(incrementalConfig); });
    incrementalConfig[key] = requiredConfigPairs[key];
    assert_equals(codec.state, "unconfigured");
  }

  // Configure should pass once incrementalConfig meets all requirements.
  codec.configure(incrementalConfig);
  assert_equals(codec.state, "configured");

  // We should be able to reconfigure the codec.
  codec.configure(incrementalConfig);
  assert_equals(codec.state, "configured");

  let config = incrementalConfig;

  unsupportedCodecsList.forEach(unsupportedCodec => {
    // Invalid codecs should fail.
    config.codec = unsupportedCodec;
    assert_throws_dom('NotSupportedError', () => {
      codec.configure(config);
    }, unsupportedCodec);
  });

  // The failed configures should not affect the current config.
  assert_equals(codec.state, "configured");

  // Test we can configure after a reset.
  codec.reset()
  assert_equals(codec.state, "unconfigured");

  codec.configure(validConfig);
  assert_equals(codec.state, "configured");
}

// Performs an encode or decode with the provided input, depending on whether
// the passed codec is an encoder or a decoder.
function encodeOrDecodeShouldThrow(codec, input) {
  // We are testing encode/decode on codecs in invalid states.
  assert_not_equals(codec.state, "configured");

  if (codec.decode) {
    assert_throws_dom("InvalidStateError",
                      () => codec.decode(input),
                      "decode");
  } else if (codec.encode) {
    // Encoders consume frames, so clone it to be safe.
    assert_throws_dom("InvalidStateError",
                  () => codec.encode(input.clone()),
                  "encode");

  } else {
    assert_unreached("Codec should have encode or decode function");
  }
}

// Makes sure that we cannot close, configure, reset, flush, decode or encode a
// closed codec.
function testClosedCodec(test, codec, validconfig, codecInput) {
  assert_equals(codec.state, "unconfigured");

  codec.close();
  assert_equals(codec.state, "closed");

  assert_throws_dom("InvalidStateError",
                    () => codec.configure(validconfig),
                    "configure");
  assert_throws_dom("InvalidStateError",
                    () => codec.reset(),
                    "reset");
  assert_throws_dom("InvalidStateError",
                    () => codec.close(),
                    "close");

  encodeOrDecodeShouldThrow(codec, codecInput);

  return promise_rejects_dom(test, 'InvalidStateError', codec.flush(), 'flush');
}

// Makes sure we cannot flush, encode or decode with an unconfigured coded, and
// that reset is a valid no-op.
function testUnconfiguredCodec(test, codec, codecInput) {
  assert_equals(codec.state, "unconfigured");

  // Configure() and Close() are valid operations that would transition us into
  // a different state.

  // Resetting an unconfigured encoder is a no-op.
  codec.reset();
  assert_equals(codec.state, "unconfigured");

  encodeOrDecodeShouldThrow(codec, codecInput);

  return promise_rejects_dom(test, 'InvalidStateError', codec.flush(), 'flush');
}

// Reference values generated by:
// https://fiddle.skia.org/c/f100d4d5f085a9e09896aabcbc463868

const kSRGBPixel = [50, 100, 150, 255];
const kP3Pixel = [62, 99, 146, 255];

function testCanvas(ctx, width, height, expected_pixel, imageSetting, assert_compares) {
  // The dup getImageData is to workaournd crbug.com/1100233
  let imageData = ctx.getImageData(0, 0, width, height, imageSetting);
  let colorData = ctx.getImageData(0, 0, width, height, imageSetting).data;
  const kMaxPixelToCheck = 128 * 96;
  let step = width * height / kMaxPixelToCheck;
  step = Math.round(step);
  step = (step < 1) ? 1 : step;
  for (let i = 0; i < 4 * width * height; i += (4 * step)) {
    assert_compares(colorData[i], expected_pixel[0]);
    assert_compares(colorData[i + 1], expected_pixel[1]);
    assert_compares(colorData[i + 2], expected_pixel[2]);
    assert_compares(colorData[i + 3], expected_pixel[3]);
  }
}

function makeDetachedArrayBuffer() {
  const buffer = new ArrayBuffer(10);
  const view = new Uint8Array(buffer);
  new MessageChannel().port1.postMessage(buffer, [buffer]);
  return view;
}

function isFrameClosed(frame) {
  return frame.format == null && frame.codedWidth == 0 &&
         frame.codedHeight == 0 && frame.displayWidth == 0 &&
         frame.displayHeight == 0 && frame.codedRect == null &&
         frame.visibleRect == null;
}

function testImageBitmapToAndFromVideoFrame(
    width, height, expectedPixel, canvasOptions, imageBitmapOptions,
    imageSetting) {
  let canvas = new OffscreenCanvas(width, height);
  let ctx = canvas.getContext('2d', canvasOptions);
  ctx.fillStyle = 'rgb(50, 100, 150)';
  ctx.fillRect(0, 0, width, height);
  testCanvas(ctx, width, height, expectedPixel, imageSetting, assert_equals);

  return createImageBitmap(canvas, imageBitmapOptions)
      .then((fromImageBitmap) => {
        let videoFrame = new VideoFrame(fromImageBitmap, {timestamp: 0});
        return createImageBitmap(videoFrame, imageBitmapOptions);
      })
      .then((toImageBitmap) => {
        let myCanvas = new OffscreenCanvas(width, height);
        let myCtx = myCanvas.getContext('2d', canvasOptions);
        myCtx.drawImage(toImageBitmap, 0, 0);
        let tolerance = 2;
        testCanvas(
            myCtx, width, height, expectedPixel, imageSetting,
            (actual, expected) => {
              assert_approx_equals(actual, expected, tolerance);
            });
      });
}
