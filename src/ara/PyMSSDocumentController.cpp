#include "PyMSSDocumentController.h"

bool PyMSSDocumentController::doRestoreObjectsFromStream (juce::ARAInputStream& input,
                                                          const juce::ARARestoreObjectsFilter* filter) noexcept
{
    // The plugin keeps no per-source persistent state.
    juce::ignoreUnused (input, filter);
    return true;
}

bool PyMSSDocumentController::doStoreObjectsToStream (juce::ARAOutputStream& output,
                                                      const juce::ARAStoreObjectsFilter* filter) noexcept
{
    juce::ignoreUnused (output, filter);
    return true;
}

bool PyMSSDocumentController::readAudioSource (juce::ARAAudioSource* source,
                                               juce::AudioBuffer<float>& out,
                                               double& nativeSampleRate)
{
    if (source == nullptr)
        return false;

    juce::ARAAudioSourceReader reader (source);
    if (! reader.isValid())
        return false;

    nativeSampleRate = reader.sampleRate;
    const int channels = (int) reader.numChannels;
    const int length = (int) reader.lengthInSamples;

    if (channels <= 0 || length <= 0)
        return false;

    out.setSize (channels, length);
    out.clear();

    // Read all source channels in one call: dest[c] receives source channel c.
    // (Reading one channel at a time previously read source ch0 into every
    // output channel, collapsing stereo to duplicated mono.)
    std::vector<float*> destPtrs ((size_t) channels);
    for (int c = 0; c < channels; ++c)
        destPtrs[(size_t) c] = out.getWritePointer (c);

    if (! reader.read (destPtrs.data(), channels, 0, length))
        return false;

    return true;
}

void PyMSSDocumentController::startWorker()
{
    const auto settings = settingsStore.load();
    const auto python = settings.resolvePythonExecutable();
    const auto script = WorkerClient::findWorkerScript();
    const auto logFile = settingsStore.getLogDir().getChildFile ("worker_stderr.txt");
    worker.start (python, script, logFile);
}

void PyMSSDocumentController::stopWorker()
{
    worker.stop();
}
