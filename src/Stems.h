#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/** Shared data structures passed between the worker client, separation engine,
    playback renderer and editor.
*/

/** One model entry as returned by the worker's list_models command. */
struct ModelInfo
{
    juce::String name;
    juce::String stem;
    juce::String architecture;
    juce::String category;
    juce::String targetStem;
    bool installed = false;
    juce::int64 sizeBytes = 0;
};

/** Detailed model information as returned by the worker's model_info command. */
struct ModelInfoDetail
{
    bool found = false;
    juce::String name;
    juce::String stem;
    juce::String architecture;
    juce::String category;
    juce::String targetStem;
    juce::String instruments;
    juce::String intro;
    bool installed = false;
};

/** Inference parameters exposed in the UI. A value of 0 means "use the model
    default", which the worker translates to None. */
struct SeparationParams
{
    int batchSize = 0;
    int overlapSize = 0;
    int chunkSize = 0;
    bool normalize = false;

    bool operator== (const SeparationParams& o) const
    {
        return batchSize == o.batchSize && overlapSize == o.overlapSize
               && chunkSize == o.chunkSize && normalize == o.normalize;
    }
};

/** A single separated stem (channel-major audio buffer). */
struct StemBuffer
{
    juce::String name;
    int channels = 0;
    juce::int64 frames = 0;
    juce::AudioBuffer<float> data; // channels x frames
};

/** A complete separation result for one audio source. */
struct StemSet
{
    juce::String audioSourcePersistentId;   // ARA audio source this belongs to
    juce::String modelName;
    juce::String modelDir;
    SeparationParams params;
    double sampleRate = 0.0;                // host rate the stem buffers are stored at
    double sourceNativeSampleRate = 0.0;    // native rate of the ARA source (for offset math)
    juce::StringArray stemNames;
    std::vector<StemBuffer> stems;

    bool isEmpty() const { return stems.empty(); }
    int getNumStems() const { return (int) stems.size(); }
};
