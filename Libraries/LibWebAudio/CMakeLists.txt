compile_ipc(ToSessionClientFromWebAudioWorker.ipc ${CMAKE_BINARY_DIR}/Libraries/LibWebAudio/ToSessionClientFromWebAudioWorkerEndpoint.h)
compile_ipc(ToWebAudioWorkerFromSessionClient.ipc ${CMAKE_BINARY_DIR}/Libraries/LibWebAudio/ToWebAudioWorkerFromSessionClientEndpoint.h)
compile_ipc(ToBrokerFromWebAudioWorker.ipc ${CMAKE_BINARY_DIR}/Libraries/LibWebAudio/ToBrokerFromWebAudioWorkerEndpoint.h)
compile_ipc(ToWebAudioWorkerFromBroker.ipc ${CMAKE_BINARY_DIR}/Libraries/LibWebAudio/ToWebAudioWorkerFromBrokerEndpoint.h)

set(SOURCES
    BrokerOfWebAudioWorker.cpp
    Debug.cpp
    LibWebAudio.cpp
    RingStream.cpp
    SessionClientOfWebAudioWorker.cpp
    SharedAudioBuffer.cpp
    SharedBufferStream.cpp
    
    Engine/FrequencyAnalysis.cpp
    Engine/GraphCodec.cpp
    Engine/GraphCompiler.cpp
    Engine/GraphExecutor.cpp
    Engine/Mixing.cpp
    Engine/AudioContextPlaybackPolicy.cpp
    Engine/SincResampler.cpp
    GraphNodes/AnalyserGraphNode.cpp
    GraphNodes/AudioListenerGraphNode.cpp
    GraphNodes/AudioWorkletGraphNode.cpp
    GraphNodes/BiquadFilterGraphNode.cpp
    GraphNodes/AudioBufferSourceGraphNode.cpp
    GraphNodes/ChannelMergerGraphNode.cpp
    GraphNodes/ChannelSplitterGraphNode.cpp
    GraphNodes/ConstantSourceGraphNode.cpp
    GraphNodes/ConvolverGraphNode.cpp
    GraphNodes/IIRFilterGraphNode.cpp
    GraphNodes/DelayGraphNode.cpp
    GraphNodes/DestinationGraphNode.cpp
    GraphNodes/DynamicsCompressorGraphNode.cpp
    GraphNodes/GainGraphNode.cpp
    GraphNodes/MediaElementAudioSourceGraphNode.cpp
    GraphNodes/MediaStreamAudioDestinationGraphNode.cpp
    GraphNodes/MediaStreamAudioSourceGraphNode.cpp
    GraphNodes/OhNoesGraphNode.cpp
    GraphNodes/OscillatorGraphNode.cpp
    GraphNodes/PannerGraphNode.cpp
    GraphNodes/ScriptProcessorGraphNode.cpp
    GraphNodes/StereoPannerGraphNode.cpp
    GraphNodes/WaveShaperGraphNode.cpp
    RenderNodes/AnalyserRenderNode.cpp
    RenderNodes/AudioListenerRenderNode.cpp
    RenderNodes/AudioBufferSourceRenderNode.cpp
    RenderNodes/AudioWorkletRenderNode.cpp
    RenderNodes/BiquadFilterRenderNode.cpp
    RenderNodes/ChannelMergerRenderNode.cpp
    RenderNodes/ChannelSplitterRenderNode.cpp
    RenderNodes/ConstantSourceRenderNode.cpp
    RenderNodes/ConvolverRenderNode.cpp
    RenderNodes/IIRFilterRenderNode.cpp
    RenderNodes/DelayRenderNode.cpp
    RenderNodes/DestinationRenderNode.cpp
    RenderNodes/DynamicsCompressorRenderNode.cpp
    RenderNodes/GainRenderNode.cpp
    RenderNodes/MediaElementAudioSourceProvider.cpp
    RenderNodes/MediaElementAudioSourceRenderNode.cpp
    RenderNodes/MediaStreamAudioDestinationRenderNode.cpp
    RenderNodes/MediaStreamAudioSourceRenderNode.cpp
    RenderNodes/OhNoesRenderNode.cpp
    RenderNodes/OscillatorRenderNode.cpp
    RenderNodes/PannerRenderNode.cpp
    RenderNodes/ScriptProcessorRenderNode.cpp
    RenderNodes/WaveShaperRenderNode.cpp
    RenderNodes/StereoPannerRenderNode.cpp
    Script/ScriptProcessorRequestPump.cpp
    Script/ScriptProcessorStreamBindings.cpp
)

set(GENERATED_SOURCES
    ../../Libraries/LibWebAudio/ToBrokerFromWebAudioWorkerEndpoint.h
    ../../Libraries/LibWebAudio/ToWebAudioWorkerFromBrokerEndpoint.h
    ../../Libraries/LibWebAudio/ToSessionClientFromWebAudioWorkerEndpoint.h
    ../../Libraries/LibWebAudio/ToWebAudioWorkerFromSessionClientEndpoint.h
)

ladybird_lib(LibWebAudio webaudio)
target_link_libraries(LibWebAudio PRIVATE LibCore LibIPC LibAudioServer)
