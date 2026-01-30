function createTestBuffer(context, sampleFrameLength) {
  let audioBuffer =
      context.createBuffer(1, sampleFrameLength, context.sampleRate);
  let channelData = audioBuffer.getChannelData(0);

  // Create a simple linear ramp starting at zero, with each value in the buffer
  // equal to its index position.
  for (let i = 0; i < sampleFrameLength; ++i)
    channelData[i] = i;

  return audioBuffer;
}

function checkSingleTest(renderedBuffer, i, should) {
  let renderedData = renderedBuffer.getChannelData(0);
  let offsetFrame = i * testSpacingFrames;

  let test = tests[i];
  let expected = test.expected;
  let description;

  if (test.description) {
    description = test.description;
  } else {
    // No description given, so create a basic one from the given test
    // parameters.
    description =
        'loop from ' + test.loopStartFrame + ' -> ' + test.loopEndFrame;
    if (test.offsetFrame)
      description += ' with offset ' + test.offsetFrame;
    if (test.playbackRate && test.playbackRate != 1)
      description += ' with playbackRate of ' + test.playbackRate;
  }

  let framesToTest;

  if (test.renderFrames)
    framesToTest = test.renderFrames;
  else if (test.durationFrames)
    framesToTest = test.durationFrames;

  // Verify that the output matches
  let prefix = 'Case ' + i + ': ';
  should(
      renderedData.slice(offsetFrame, offsetFrame + framesToTest),
      prefix + description)
      .beEqualToArray(expected);

  // Verify that we get all zeroes after the buffer (or duration) has passed.
  should(
      renderedData.slice(
          offsetFrame + framesToTest, offsetFrame + testSpacingFrames),
      prefix + description + ': tail')
      .beConstantValueOf(0);
}

function checkAllTests(renderedBuffer, should) {
  for (let i = 0; i < tests.length; ++i)
    checkSingleTest(renderedBuffer, i, should);
}


// Create the actual result by modulating playbackRate or detune AudioParam of
// ABSN. |modTarget| is a string of AudioParam name, |modOffset| is the offset
// (anchor) point of modulation, and |modRange| is the range of modulation.
//
//   createSawtoothWithModulation(context, 'detune', 440, 1200);
//
// The above will perform a modulation on detune within the range of
// [1200, -1200] around the sawtooth waveform on 440Hz.
function createSawtoothWithModulation(context, modTarget, modOffset, modRange) {
  let lfo = context.createOscillator();
  let amp = context.createGain();

  // Create a sawtooth generator with the signal range of [0, 1].
  let phasor = context.createBufferSource();
  let phasorBuffer = context.createBuffer(1, sampleRate, sampleRate);
  let phasorArray = phasorBuffer.getChannelData(0);
  let phase = 0, phaseStep = 1 / sampleRate;
  for (let i = 0; i < phasorArray.length; i++) {
    phasorArray[i] = phase % 1.0;
    phase += phaseStep;
  }
  phasor.buffer = phasorBuffer;
  phasor.loop = true;

  // 1Hz for audible (human-perceivable) parameter modulation by LFO.
  lfo.frequency.value = 1.0;

  amp.gain.value = modRange;
  phasor.playbackRate.value = modOffset;

  // The oscillator output should be amplified accordingly to drive the
  // modulation within the desired range.
  lfo.connect(amp);
  amp.connect(phasor[modTarget]);

  phasor.connect(context.destination);

  lfo.start();
  phasor.start();
}
