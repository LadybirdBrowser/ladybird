let sampleRate = 44100.0;

let renderLengthSeconds = 4;
let delayTimeSeconds = 0.5;
let toneLengthSeconds = 2;

function createToneBuffer(context, frequency, numberOfCycles, sampleRate) {
  let duration = numberOfCycles / frequency;
  let sampleFrameLength = duration * sampleRate;

  let audioBuffer = context.createBuffer(1, sampleFrameLength, sampleRate);

  let n = audioBuffer.length;
  let data = audioBuffer.getChannelData(0);

  for (let i = 0; i < n; ++i)
    data[i] = Math.sin(frequency * 2.0 * Math.PI * i / sampleRate);

  return audioBuffer;
}

function checkDelayedResult(renderedBuffer, toneBuffer, should) {
  let sourceData = toneBuffer.getChannelData(0);
  let renderedData = renderedBuffer.getChannelData(0);

  let delayTimeFrames = delayTimeSeconds * sampleRate;
  let toneLengthFrames = toneLengthSeconds * sampleRate;

  let success = true;

  let n = renderedBuffer.length;

  for (let i = 0; i < n; ++i) {
    if (i < delayTimeFrames) {
      // Check that initial portion is 0 (since signal is delayed).
      if (renderedData[i] != 0) {
        should(
            renderedData[i], 'Initial portion expected to be 0 at frame ' + i)
            .beEqualTo(0);
        success = false;
        break;
      }
    } else if (i >= delayTimeFrames && i < delayTimeFrames + toneLengthFrames) {
      // Make sure that the tone data is delayed by exactly the expected number
      // of frames.
      let j = i - delayTimeFrames;
      if (renderedData[i] != sourceData[j]) {
        should(renderedData[i], 'Actual data at frame ' + i)
            .beEqualTo(sourceData[j]);
        success = false;
        break;
      }
    } else {
      // Make sure we have silence after the delayed tone.
      if (renderedData[i] != 0) {
        should(renderedData[j], 'Final portion at frame ' + i).beEqualTo(0);
        success = false;
        break;
      }
    }
  }

  should(
      success, 'Delaying test signal by ' + delayTimeSeconds + ' sec was done')
      .message('correctly', 'incorrectly')
}
