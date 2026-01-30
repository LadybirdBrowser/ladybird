/*
  Returns an array (typed or not), of the passed array with removed trailing and ending
  zero-valued elements
 */
function trimEmptyElements(array) {
  var start = 0;
  var end = array.length;

  while (start < array.length) {
    if (array[start] !== 0) {
      break;
    }
    start++;
  }

  while (end > 0) {
    end--;
    if (array[end] !== 0) {
      break;
    }
  }
  return array.subarray(start, end);
}


function fuzzyCompare(a, b) {
  return Math.abs(a - b) < 9e-3;
}

function compareChannels(buf1, buf2,
                        /*optional*/ length,
                        /*optional*/ sourceOffset,
                        /*optional*/ destOffset,
                        /*optional*/ skipLengthCheck) {
  if (!skipLengthCheck) {
    assert_equals(buf1.length, buf2.length, "Channels must have the same length");
  }
  sourceOffset = sourceOffset || 0;
  destOffset = destOffset || 0;
  if (length == undefined) {
    length = buf1.length - sourceOffset;
  }
  var difference = 0;
  var maxDifference = 0;
  var firstBadIndex = -1;
  for (var i = 0; i < length; ++i) {
    if (!fuzzyCompare(buf1[i + sourceOffset], buf2[i + destOffset])) {
      difference++;
      maxDifference = Math.max(maxDifference, Math.abs(buf1[i + sourceOffset] - buf2[i + destOffset]));
      if (firstBadIndex == -1) {
        firstBadIndex = i;
      }
    }
  };

  assert_equals(difference, 0, "maxDifference: " + maxDifference +
     ", first bad index: " + firstBadIndex + " with test-data offset " +
     sourceOffset + " and expected-data offset " + destOffset +
     "; corresponding values " + buf1[firstBadIndex + sourceOffset] + " and " +
     buf2[firstBadIndex + destOffset] + " --- differences");
}

function compareBuffers(got, expected) {
  if (got.numberOfChannels != expected.numberOfChannels) {
    assert_equals(got.numberOfChannels, expected.numberOfChannels,
                  "Correct number of buffer channels");
    return;
  }
  if (got.length != expected.length) {
    assert_equals(got.length, expected.length,
                  "Correct buffer length");
    return;
  }
  if (got.sampleRate != expected.sampleRate) {
    assert_equals(got.sampleRate, expected.sampleRate,
                  "Correct sample rate");
    return;
  }

  for (var i = 0; i < got.numberOfChannels; ++i) {
    compareChannels(got.getChannelData(i), expected.getChannelData(i),
                    got.length, 0, 0, true);
  }
}

/**
 * This function assumes that the test is a "single page test" [0], and defines a
 * single gTest variable with the following properties and methods:
 *
 * + numberOfChannels: optional property which specifies the number of channels
 *                     in the output.  The default value is 2.
 * + createGraph: mandatory method which takes a context object and does
 *                everything needed in order to set up the Web Audio graph.
 *                This function returns the node to be inspected.
 * + createGraphAsync: async version of createGraph.  This function takes
 *                     a callback which should be called with an argument
 *                     set to the node to be inspected when the callee is
 *                     ready to proceed with the test.  Either this function
 *                     or createGraph must be provided.
 * + createExpectedBuffers: optional method which takes a context object and
 *                          returns either one expected buffer or an array of
 *                          them, designating what is expected to be observed
 *                          in the output.  If omitted, the output is expected
 *                          to be silence.  All buffers must have the same
 *                          length, which must be a bufferSize supported by
 *                          ScriptProcessorNode.  This function is guaranteed
 *                          to be called before createGraph.
 * + length: property equal to the total number of frames which we are waiting
 *           to see in the output, mandatory if createExpectedBuffers is not
 *           provided, in which case it must be a bufferSize supported by
 *           ScriptProcessorNode (256, 512, 1024, 2048, 4096, 8192, or 16384).
 *           If createExpectedBuffers is provided then this must be equal to
 *           the number of expected buffers * the expected buffer length.
 *
 * + skipOfflineContextTests: optional. when true, skips running tests on an offline
 *                            context by circumventing testOnOfflineContext.
 *
 * [0]: https://web-platform-tests.org/writing-tests/testharness-api.html#single-page-tests
 */
function runTest(name)
{
  function runTestFunction () {
    if (!gTest.numberOfChannels) {
      gTest.numberOfChannels = 2; // default
    }

    var testLength;

    function runTestOnContext(context, callback, testOutput) {
      if (!gTest.createExpectedBuffers) {
        // Assume that the output is silence
        var expectedBuffers = getEmptyBuffer(context, gTest.length);
      } else {
        var expectedBuffers = gTest.createExpectedBuffers(context);
      }
      if (!(expectedBuffers instanceof Array)) {
        expectedBuffers = [expectedBuffers];
      }
      var expectedFrames = 0;
      for (var i = 0; i < expectedBuffers.length; ++i) {
        assert_equals(expectedBuffers[i].numberOfChannels, gTest.numberOfChannels,
                      "Correct number of channels for expected buffer " + i);
        expectedFrames += expectedBuffers[i].length;
      }
      if (gTest.length && gTest.createExpectedBuffers) {
        assert_equals(expectedFrames,
                      gTest.length, "Correct number of expected frames");
      }

      if (gTest.createGraphAsync) {
        gTest.createGraphAsync(context, function(nodeToInspect) {
          testOutput(nodeToInspect, expectedBuffers, callback);
        });
      } else {
        testOutput(gTest.createGraph(context), expectedBuffers, callback);
      }
    }

    function testOnNormalContext(callback) {
      function testOutput(nodeToInspect, expectedBuffers, callback) {
        testLength = 0;
        var sp = context.createScriptProcessor(expectedBuffers[0].length, gTest.numberOfChannels, 1);
        nodeToInspect.connect(sp).connect(context.destination);
        sp.onaudioprocess = function(e) {
          var expectedBuffer = expectedBuffers.shift();
          testLength += expectedBuffer.length;
          compareBuffers(e.inputBuffer, expectedBuffer);
          if (expectedBuffers.length == 0) {
            sp.onaudioprocess = null;
            callback();
          }
        };
      }
      var context = new AudioContext();
      runTestOnContext(context, callback, testOutput);
    }

    function testOnOfflineContext(callback, sampleRate) {
      function testOutput(nodeToInspect, expectedBuffers, callback) {
        nodeToInspect.connect(context.destination);
        context.oncomplete = function(e) {
          var samplesSeen = 0;
          while (expectedBuffers.length) {
            var expectedBuffer = expectedBuffers.shift();
            assert_equals(e.renderedBuffer.numberOfChannels, expectedBuffer.numberOfChannels,
                          "Correct number of input buffer channels");
            for (var i = 0; i < e.renderedBuffer.numberOfChannels; ++i) {
              compareChannels(e.renderedBuffer.getChannelData(i),
                             expectedBuffer.getChannelData(i),
                             expectedBuffer.length,
                             samplesSeen,
                             undefined,
                             true);
            }
            samplesSeen += expectedBuffer.length;
          }
          callback();
        };
        context.startRendering();
      }

      var context = new OfflineAudioContext(gTest.numberOfChannels, testLength, sampleRate);
      runTestOnContext(context, callback, testOutput);
    }

    testOnNormalContext(function() {
      if (!gTest.skipOfflineContextTests) {
        testOnOfflineContext(function() {
          testOnOfflineContext(done, 44100);
        }, 48000);
      } else {
        done();
      }
    });
  };

  runTestFunction();
}

// Simpler than audit.js, but still logs the message. Requires
// `setup("explicit_done": true)` if testing code that runs after the "load"
// event.
function equals(a, b, msg) {
  test(function() {
    assert_equals(a, b);
  }, msg);
}
function is_true(a, msg) {
  test(function() {
    assert_true(a);
  }, msg);
}

// This allows writing AudioWorkletProcessor code in the same file as the rest
// of the test, for quick one off AudioWorkletProcessor testing.
function URLFromScriptsElements(ids)
{
  var scriptTexts = [];
  for (let id of ids) {

    const e = document.querySelector("script#"+id)
    if (!e) {
      throw id+" is not the id of a <script> tag";
    }
    scriptTexts.push(e.innerText);
  }
  const blob = new Blob(scriptTexts, {type: "application/javascript"});

  return URL.createObjectURL(blob);
}
