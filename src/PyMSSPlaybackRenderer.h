#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <map>
#include <memory>

#include "SeparationEngine.h"

/** ARA playback renderer for a 1-in / 1-out (stereo) plugin.

    Output policy per playback region:
      - Before separation: stream the original ("dry") source through a
        stateful resampler (source native SR -> host SR) so pitch is correct
        regardless of the host / source rate mismatch.
      - After separation: mix the active stems (mute/solo) into the output bus.

    Rendering only occurs while the host is playing; when stopped the output is
    silenced (otherwise the frozen playhead would repeat a slice and buzz).
*/
class PyMSSPlaybackRenderer : public juce::ARAPlaybackRenderer
{
public:
    PyMSSPlaybackRenderer (ARA::PlugIn::DocumentController* dc, SeparationEngine& engineToUse)
        : juce::ARAPlaybackRenderer (dc), engine (engineToUse) {}

    void prepareToPlay (double sampleRateIn,
                        int maximumSamplesPerBlockIn,
                        int numChannelsIn,
                        juce::AudioProcessor::ProcessingPrecision precision,
                        juce::ARARenderer::AlwaysNonRealtime alwaysNonRealtime) override;

    void releaseResources() override;

    bool processBlock (juce::AudioBuffer<float>& buffer,
                       juce::AudioProcessor::Realtime realtime,
                       const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;

    using juce::ARAPlaybackRenderer::processBlock;

private:
    struct DryState
    {
        std::unique_ptr<juce::ARAAudioSourceReader> reader;
        juce::LagrangeInterpolator resampler[2];
        juce::int64 lastSongPos = -1;       // last host song position rendered
        int lastNumOut = 0;                 // number of host samples rendered then
        juce::int64 nextNativeReadPos = 0;  // next native input sample to feed
        bool primed = false;
    };

    SeparationEngine& engine;

    double sampleRate = 48000.0;
    int maximumSamplesPerBlock = 512;
    int numChannels = 2;

    std::map<juce::ARAAudioSource*, std::unique_ptr<DryState>> dryStates;
    juce::AudioBuffer<float> dryInBuf;   // native-rate scratch
    juce::AudioBuffer<float> dryOutBuf;  // host-rate scratch
};
