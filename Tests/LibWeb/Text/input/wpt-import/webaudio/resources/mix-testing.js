let toneLengthSeconds = 1;

// Create a buffer with multiple channels.
// The signal frequency in each channel is the multiple of that in the first
// channel.
function createToneBuffer(context, frequency, duration, numberOfChannels) {
  let sampleRate = context.sampleRate;
  let sampleFrameLength = duration * sampleRate;

  let audioBuffer =
      context.createBuffer(numberOfChannels, sampleFrameLength, sampleRate);

  let n = audioBuffer.length;

  for (let k = 0; k < numberOfChannels; ++k) {
    let data = audioBuffer.getChannelData(k);

    for (let i = 0; i < n; ++i)
      data[i] = Math.sin(frequency * (k + 1) * 2.0 * Math.PI * i / sampleRate);
  }

  return audioBuffer;
}
