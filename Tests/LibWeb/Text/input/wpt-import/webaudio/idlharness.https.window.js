// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: timeout=long

// https://webaudio.github.io/web-audio-api/

'use strict';

idl_test(
  ['webaudio'],
  ['cssom', 'uievents', 'mediacapture-streams', 'html', 'dom'],
  async idl_array => {
    idl_array.add_untested_idls('interface SVGElement {};');

    idl_array.add_objects({
      BaseAudioContext: [],
      AudioContext: ['context'],
      OfflineAudioContext: ['new OfflineAudioContext(1, 1, sample_rate)'],
      OfflineAudioCompletionEvent: [
        'new OfflineAudioCompletionEvent("", {renderedBuffer: buffer})'
      ],
      AudioBuffer: ['buffer'],
      AudioNode: [],
      AudioParam: ['new AudioBufferSourceNode(context).playbackRate'],
      AudioScheduledSourceNode: [],
      AnalyserNode: ['new AnalyserNode(context)'],
      AudioBufferSourceNode: ['new AudioBufferSourceNode(context)'],
      AudioDestinationNode: ['context.destination'],
      AudioListener: ['context.listener'],
      AudioProcessingEvent: [`new AudioProcessingEvent('', {
        playbackTime: 0, inputBuffer: buffer, outputBuffer: buffer
      })`],
      BiquadFilterNode: ['new BiquadFilterNode(context)'],
      ChannelMergerNode: ['new ChannelMergerNode(context)'],
      ChannelSplitterNode: ['new ChannelSplitterNode(context)'],
      ConstantSourceNode: ['new ConstantSourceNode(context)'],
      ConvolverNode: ['new ConvolverNode(context)'],
      DelayNode: ['new DelayNode(context)'],
      DynamicsCompressorNode: ['new DynamicsCompressorNode(context)'],
      GainNode: ['new GainNode(context)'],
      IIRFilterNode: [
        'new IIRFilterNode(context, {feedforward: [1], feedback: [1]})'
      ],
      MediaElementAudioSourceNode: [
        'new MediaElementAudioSourceNode(context, {mediaElement: new Audio})'
      ],
      MediaStreamAudioDestinationNode: [
        'new MediaStreamAudioDestinationNode(context)'
      ],
      MediaStreamAudioSourceNode: [],
      MediaStreamTrackAudioSourceNode: [],
      OscillatorNode: ['new OscillatorNode(context)'],
      PannerNode: ['new PannerNode(context)'],
      PeriodicWave: ['new PeriodicWave(context)'],
      ScriptProcessorNode: ['context.createScriptProcessor()'],
      StereoPannerNode: ['new StereoPannerNode(context)'],
      WaveShaperNode: ['new WaveShaperNode(context)'],
      AudioWorklet: ['context.audioWorklet'],
      AudioWorkletGlobalScope: [],
      AudioParamMap: ['worklet_node.parameters'],
      AudioWorkletNode: ['worklet_node'],
      AudioWorkletProcessor: [],
    });

    self.sample_rate = 44100;
    self.context = new AudioContext;
    self.buffer = new AudioBuffer({length: 1, sampleRate: sample_rate});
    await context.audioWorklet.addModule(
      'the-audio-api/the-audioworklet-interface/processors/dummy-processor.js');
    self.worklet_node = new AudioWorkletNode(context, 'dummy');
  }
);
