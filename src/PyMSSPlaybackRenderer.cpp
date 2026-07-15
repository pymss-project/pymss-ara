#include "PyMSSPlaybackRenderer.h"

#include <cmath>

void PyMSSPlaybackRenderer::prepareToPlay (double sampleRateIn,
                                           int maximumSamplesPerBlockIn,
                                           int numChannelsIn,
                                           juce::AudioProcessor::ProcessingPrecision,
                                           juce::ARARenderer::AlwaysNonRealtime)
{
    sampleRate = sampleRateIn;
    maximumSamplesPerBlock = maximumSamplesPerBlockIn;
    numChannels = juce::jmax (1, numChannelsIn);
    engine.setHostSampleRate (sampleRate);

    dryInBuf.setSize (2, maximumSamplesPerBlock * 4 + 64);
    dryOutBuf.setSize (2, maximumSamplesPerBlock);

    // One-time log of host vs source-native rate (not per-block -> no xruns).
    juce::File logFile = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
        .getChildFile (".pymss/logs/renderer.log");
    logFile.appendText (juce::Time::getCurrentTime().toISO8601 (false)
        + "  prepareToPlay hostSR=" + juce::String (sampleRate, 1) + "\n");

    for (const auto pr : getPlaybackRegions())
    {
        if (pr == nullptr || pr->getAudioModification() == nullptr)
            continue;
        auto* source = pr->getAudioModification()->getAudioSource();
        if (source == nullptr)
            continue;
        auto reader = std::make_unique<juce::ARAAudioSourceReader> (source);
        const double nativeSr = reader->isValid() ? reader->sampleRate : 0.0;
        logFile.appendText (juce::Time::getCurrentTime().toISO8601 (false)
            + "  source " + source->getPersistentID()
            + " nativeSR=" + juce::String (nativeSr, 1)
            + " hostSR=" + juce::String (sampleRate, 1) + "\n");
    }
}

void PyMSSPlaybackRenderer::releaseResources()
{
    dryStates.clear();
}

bool PyMSSPlaybackRenderer::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::AudioProcessor::Realtime realtime,
                                          const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    juce::ignoreUnused (realtime);
    buffer.clear();

    if (! positionInfo.getIsPlaying())
        return true;

    const auto numSamples = buffer.getNumSamples();
    const auto timeInSamples = positionInfo.getTimeInSamples().orFallback (0);
    const auto blockRange = juce::Range<juce::int64>::withStartAndLength (timeInSamples, numSamples);
    const int outCh = juce::jmin (numChannels, buffer.getNumChannels());

    for (const auto playbackRegion : getPlaybackRegions())
    {
        const auto playbackSampleRange = playbackRegion->getSampleRange (sampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
        auto renderRange = blockRange.getIntersectionWith (playbackSampleRange);
        if (renderRange.isEmpty())
            continue;

        const auto source = playbackRegion->getAudioModification()->getAudioSource();
        if (source == nullptr)
            continue;

        const juce::int64 playbackStart = playbackSampleRange.getStart();
        const juce::int64 modStart = playbackRegion->getStartInAudioModificationSamples();
        const juce::int64 songPos = renderRange.getStart();
        const juce::int64 songOffset = songPos - playbackStart;   // host-SR samples into region

        const int startInBuffer = (int) (songPos - blockRange.getStart());
        const int numOut = (int) renderRange.getLength();

        auto stems = engine.getStemsForSource (source->getPersistentID());

        if (stems != nullptr && stems->getNumStems() > 0)
        {
            // Stems are stored at host SR; map modification start (native SR)
            // into the host-SR stem domain.
            const double hostRate = stems->sampleRate > 0 ? stems->sampleRate : sampleRate;
            const double nativeRate = stems->sourceNativeSampleRate > 0 ? stems->sourceNativeSampleRate : hostRate;
            const double ratioN2H = hostRate / nativeRate;
            const juce::int64 startInStems = (juce::int64) juce::roundToIntAccurate ((double) modStart * ratioN2H) + songOffset;

            for (int i = 0; i < stems->getNumStems(); ++i)
            {
                if (! engine.isStemActive (i))
                    continue;

                const auto& stem = stems->stems[(size_t) i];
                const int stemCh = stem.data.getNumChannels();
                const juce::int64 stemFrames = stem.frames;

                for (int c = 0; c < outCh; ++c)
                {
                    const int srcCh = juce::jmin (c, stemCh - 1);
                    const float* src = stem.data.getReadPointer (srcCh);
                    for (int n = 0; n < numOut; ++n)
                    {
                        const juce::int64 idx = startInStems + n;
                        if (idx >= 0 && idx < stemFrames)
                            buffer.addSample (c, startInBuffer + n, src[idx]);
                    }
                }
            }
        }
        else
        {
            // Dry passthrough with streaming resampling (native SR -> host SR).
            auto& state = dryStates[source];
            if (! state)
                state = std::make_unique<DryState>();
            if (! state->reader)
                state->reader = std::make_unique<juce::ARAAudioSourceReader> (source);

            auto* reader = state->reader.get();
            if (! (reader && reader->isValid()))
                continue;

            const double nativeRate = reader->sampleRate;
            if (nativeRate <= 0.0)
                continue;

            const double speedRatio = nativeRate / sampleRate;   // native input per host output

            // Reset only on a real playhead discontinuity (seek / first block).
            // Continuous playback keeps resampler state -> no drift, no clicks.
            const bool seek = ! state->primed || songPos != state->lastSongPos + state->lastNumOut;
            if (seek)
            {
                for (int c = 0; c < 2; ++c)
                    state->resampler[c].reset();
                state->nextNativeReadPos = modStart
                    + (juce::int64) std::llround ((double) songOffset * nativeRate / sampleRate);
                state->primed = true;
            }

            const int readerCh = juce::jmax (1, (int) reader->numChannels);
            const int numIn = juce::jmax (8, (int) std::llround (std::ceil (numOut * speedRatio)) + 16);

            dryInBuf.setSize (readerCh, numIn, false);
            dryInBuf.clear();
            reader->read (&dryInBuf, 0, numIn, state->nextNativeReadPos, true, true);

            dryOutBuf.setSize (outCh, numOut, false);
            int used = numIn;
            for (int c = 0; c < outCh; ++c)
            {
                const int srcC = juce::jmin (c, readerCh - 1);
                const int thisUsed = state->resampler[c].process (speedRatio,
                                                                   dryInBuf.getReadPointer (srcC),
                                                                   dryOutBuf.getWritePointer (c),
                                                                   numOut);
                if (c == 0)
                    used = thisUsed;
            }
            state->nextNativeReadPos += used;
            state->lastSongPos = songPos;
            state->lastNumOut = numOut;

            for (int c = 0; c < outCh; ++c)
                buffer.addFrom (c, startInBuffer, dryOutBuf, c, 0, numOut);
        }
    }

    return true;
}
