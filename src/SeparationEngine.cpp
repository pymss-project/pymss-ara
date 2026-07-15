#include "SeparationEngine.h"

SeparationEngine::SeparationEngine (WorkerClient& clientToUse)
    : juce::Thread ("pymss-separation"), worker (clientToUse)
{
    worker.addListener (this);
}

SeparationEngine::~SeparationEngine()
{
    worker.removeListener (this);
    cancelRequested = true;
    signalThreadShouldExit();
    resultEvent.signal();
    stopThread (2000);
}

void SeparationEngine::setHostSampleRate (double sr)
{
    hostSampleRate = sr;
}

void SeparationEngine::setStemMuted (int index, bool muted)
{
    if (index >= 0 && index < (int) stemMuted.size())
        stemMuted[(size_t) index].store (muted, std::memory_order_relaxed);
}

void SeparationEngine::setStemSolo (int index, bool solo)
{
    if (index >= 0 && index < (int) stemSolo.size())
        stemSolo[(size_t) index].store (solo, std::memory_order_relaxed);
}

bool SeparationEngine::isStemMuted (int index) const
{
    return (index >= 0 && index < (int) stemMuted.size())
               ? stemMuted[(size_t) index].load (std::memory_order_relaxed)
               : false;
}

bool SeparationEngine::isStemSolo (int index) const
{
    return (index >= 0 && index < (int) stemSolo.size())
               ? stemSolo[(size_t) index].load (std::memory_order_relaxed)
               : false;
}

bool SeparationEngine::isStemActive (int index) const
{
    bool anySolo = false;
    for (const auto& s : stemSolo)
        if (s.load (std::memory_order_relaxed)) { anySolo = true; break; }

    if (anySolo)
        return isStemSolo (index);

    return ! isStemMuted (index);
}

juce::String SeparationEngine::setStaticStatus (const juce::String& s)
{
    // Keep a stable pointer for the atomic status field.
    juce::ScopedLock sl (stringLock);
    statusStrings.add (s);
    return statusStrings.getReference (statusStrings.size() - 1);
}

bool SeparationEngine::requestSeparation (SourceReader sourceReader,
                                          const juce::String& audioSourcePersistentId,
                                          const juce::String& model,
                                          const juce::String& modelDir,
                                          const SeparationParams& params,
                                          double hostSampleRateIn)
{
    if (isThreadRunning())
        return false;

    {
        juce::ScopedLock sl (jobLock);
        currentJob = std::make_unique<Job>();
        currentJob->sourceReader = std::move (sourceReader);
        currentJob->sourceId = audioSourcePersistentId;
        currentJob->model = model;
        currentJob->modelDir = modelDir;
        currentJob->params = params;
        currentJob->hostSampleRate = hostSampleRateIn > 0 ? hostSampleRateIn : hostSampleRate.load();
    }

    cancelRequested = false;
    pendingTag = -1;
    progressDone = 0;
    progressTotal = 1;
    {
        juce::ScopedLock el (errorLock);
        errorMessage.clear();
    }

    state = State::reading;
    statusMessage = setStaticStatus ("Reading audio source...").toRawUTF8();
    sendChangeMessage();
    startThread();
    return true;
}

void SeparationEngine::cancel()
{
    cancelRequested = true;
    if (pendingTag.load() >= 0)
        worker.cancelSeparation();
    resultEvent.signal();
}

float SeparationEngine::getProgress() const
{
    const int total = progressTotal.load();
    if (total <= 0)
        return 0.0f;
    return juce::jlimit (0.0f, 1.0f, (float) progressDone.load() / (float) total);
}

juce::String SeparationEngine::getProgressText() const
{
    const int total = juce::jmax (1, progressTotal.load());
    const int done = juce::jmin (progressDone.load(), total);
    return juce::String (done) + " / " + juce::String (total);
}

juce::String SeparationEngine::getStatusMessage() const
{
    return juce::String (statusMessage.load());
}

juce::String SeparationEngine::getErrorMessage() const
{
    juce::ScopedLock sl (errorLock);
    return errorMessage;
}

std::shared_ptr<StemSet> SeparationEngine::getStemsForSource (const juce::String& id) const
{
    juce::ScopedLock sl (cacheLock);
    auto it = stemCache.find (id);
    return it != stemCache.end() ? it->second : nullptr;
}

bool SeparationEngine::hasStemsForSource (const juce::String& id) const
{
    juce::ScopedLock sl (cacheLock);
    return stemCache.find (id) != stemCache.end();
}

void SeparationEngine::invalidateSource (const juce::String& id)
{
    juce::ScopedLock sl (cacheLock);
    stemCache.erase (id);
}

//==============================================================================
void SeparationEngine::separationProgress (int tag, int done, int total, const juce::String& message)
{
    if (tag != pendingTag.load() && pendingTag.load() >= 0)
        return;

    progressDone = done;
    progressTotal = juce::jmax (1, total);
    if (message.isNotEmpty())
        statusMessage = setStaticStatus (message).toRawUTF8();
    state = State::separating;
    sendChangeMessage();
}

void SeparationEngine::separationDone (int tag, std::shared_ptr<StemSet> stems)
{
    if (tag != pendingTag.load())
        return;

    Job job;
    {
        juce::ScopedLock sl (jobLock);
        if (currentJob)
            job = *currentJob;
    }

    if (stems)
    {
        stems->audioSourcePersistentId = job.sourceId;
        stems->modelName = job.model;
        stems->modelDir = job.modelDir;
        stems->params = job.params;
        stems->sourceNativeSampleRate = lastNativeSampleRate.load();

        juce::ScopedLock sl (cacheLock);
        stemCache[job.sourceId] = stems;
        lastSourceIdRaw = job.sourceId;
        lastSourceId = lastSourceIdRaw.toRawUTF8();
        lastSourceIdValid = true;
    }

    state = State::done;
    statusMessage = setStaticStatus ("Separation complete").toRawUTF8();
    pendingTag = -1;
    resultEvent.signal();
    sendChangeMessage();
}

void SeparationEngine::separationFailed (int tag, const juce::String& message, bool cancelled)
{
    if (tag != pendingTag.load() && ! cancelled)
        return;

    {
        juce::ScopedLock sl (errorLock);
        errorMessage = message;
    }

    if (cancelled)
    {
        state = State::cancelled;
        statusMessage = setStaticStatus ("Cancelled").toRawUTF8();
    }
    else
    {
        state = State::failed;
        statusMessage = setStaticStatus ("Failed").toRawUTF8();
    }
    pendingTag = -1;
    resultEvent.signal();
    sendChangeMessage();
}

void SeparationEngine::workerDied (const juce::String& reason)
{
    // If a separation is in flight, fail it so the engine thread doesn't hang.
    const auto tag = pendingTag.load();
    if (tag < 0)
        return;

    separationFailed (tag, "Worker process stopped: " + reason, false);
}

//==============================================================================
static juce::AudioBuffer<float> resampleBuffer (const juce::AudioBuffer<float>& src, double fromSR, double toSR)
{
    if (fromSR <= 0 || toSR <= 0 || std::abs (fromSR - toSR) < 0.5)
        return src;

    const double ratio = fromSR / toSR; // input samples per output sample
    const int numOut = juce::roundToInt (src.getNumSamples() * toSR / fromSR);
    const int channels = src.getNumChannels();

    juce::AudioBuffer<float> out (channels, juce::jmax (1, numOut));
    for (int c = 0; c < channels; ++c)
    {
        juce::LagrangeInterpolator interp;
        interp.reset();
        interp.process (ratio, src.getReadPointer (c), out.getWritePointer (c), out.getNumSamples());
    }
    return out;
}

void SeparationEngine::run()
{
    Job job;
    {
        juce::ScopedLock sl (jobLock);
        if (! currentJob) { state = State::idle; return; }
        job = *currentJob;
    }

    // 1) Read the ARA audio source.
    juce::AudioBuffer<float> sourceAudio;
    double nativeSR = 0.0;

    if (! job.sourceReader || ! job.sourceReader (sourceAudio, nativeSR) || sourceAudio.getNumSamples() == 0)
    {
        juce::ScopedLock el (errorLock);
        errorMessage = "Failed to read audio source from the host.";
        state = State::failed;
        statusMessage = setStaticStatus ("Failed").toRawUTF8();
        sendChangeMessage();
        return;
    }

    lastNativeSampleRate = nativeSR;

    if (threadShouldExit() || cancelRequested.load())
    {
        state = State::cancelled;
        statusMessage = setStaticStatus ("Cancelled").toRawUTF8();
        sendChangeMessage();
        return;
    }

    // 2) Resample to the host sample rate.
    const double targetSR = job.hostSampleRate > 0 ? job.hostSampleRate : hostSampleRate.load();
    if (targetSR > 0 && std::abs (targetSR - nativeSR) > 0.5)
        sourceAudio = resampleBuffer (sourceAudio, nativeSR, targetSR);

    if (! worker.isRunning())
    {
        juce::ScopedLock el (errorLock);
        errorMessage = "Python worker is not running. Check the python_path setting and that pymss is installed.";
        state = State::failed;
        statusMessage = setStaticStatus ("Failed").toRawUTF8();
        sendChangeMessage();
        return;
    }

    // 3) Dispatch separation and wait for the worker result.
    state = State::separating;
    statusMessage = setStaticStatus ("Loading model / separating...").toRawUTF8();
    sendChangeMessage();

    resultEvent.reset();
    const int tag = worker.requestSeparation (job.model, job.modelDir, job.params,
                                              sourceAudio, targetSR, sourceAudio.getNumChannels());
    pendingTag = tag;

    if (tag < 0)
    {
        juce::ScopedLock el (errorLock);
        errorMessage = "Could not send separation request to the worker.";
        state = State::failed;
        statusMessage = setStaticStatus ("Failed").toRawUTF8();
        sendChangeMessage();
        return;
    }

    // Wait until the worker reports done/failed, or we are cancelled.
    while (! threadShouldExit())
    {
        if (resultEvent.wait (200))
            break;
        if (cancelRequested.load())
            worker.cancelSeparation();
    }

    // Engine state has been updated by separationDone/Failed callback.
    sendChangeMessage();
}
