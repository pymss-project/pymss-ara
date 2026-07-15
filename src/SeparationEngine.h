#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <map>
#include <memory>
#include <functional>

#include "Stems.h"
#include "WorkerClient.h"

/** Orchestrates separation jobs on a background thread.

    Threading model:
      - requestSeparation() starts an internal thread that reads the ARA audio
        source (via the callback supplied by the document controller), resamples
        it to the host sample rate, then dispatches the actual separation to the
        WorkerClient.
      - WorkerClient callbacks (progress / done / failed) fire on the worker's
        reader thread; the engine stores results under atomics / a lock and
        broadcasts a ChangeBroadcaster notification so the editor can poll.
*/
class SeparationEngine : public WorkerClient::Listener,
                         public juce::ChangeBroadcaster,
                         private juce::Thread
{
public:
    enum class State { idle, reading, downloading, separating, done, failed, cancelled };

    /** sourceReader must fill `out` with the full audio source (channel-major)
        at the source's native sample rate, returning true on success. */
    using SourceReader = std::function<bool (juce::AudioBuffer<float>& out, double& nativeSampleRate)>;

    SeparationEngine (WorkerClient& clientToUse);
    ~SeparationEngine() override;

    /** Set the host playback sample rate (from PlaybackRenderer::prepareToPlay). */
    void setHostSampleRate (double sr);

    /** Kick off a separation job. No-op if a job is already running. */
    bool requestSeparation (SourceReader sourceReader,
                            const juce::String& audioSourcePersistentId,
                            const juce::String& model,
                            const juce::String& modelDir,
                            const SeparationParams& params,
                            double hostSampleRate);

    void cancel();

    //----------------------------------------------------------------------
    // Read-only accessors for the editor (thread-safe).
    State getState() const               { return state.load(); }
    bool isBusy() const                  { return getState() == State::reading
                                                || getState() == State::downloading
                                                || getState() == State::separating; }
    float getProgress() const;
    juce::String getProgressText() const;
    juce::String getStatusMessage() const;
    juce::String getErrorMessage() const;

    /** Retrieve cached stems for an audio source, or nullptr. Thread-safe. */
    std::shared_ptr<StemSet> getStemsForSource (const juce::String& audioSourcePersistentId) const;
    bool hasStemsForSource (const juce::String& audioSourcePersistentId) const;
    juce::String getLastSeparatedSourceId() const  { return lastSourceId.load(); }

    /** Invalidate cached stems (e.g. when the source content changes). */
    void invalidateSource (const juce::String& audioSourcePersistentId);

    //----------------------------------------------------------------------
    // Per-stem mute / solo (used by the renderer to build the monitor mix).
    // Stem slots are indexed 0..7. State is stored in atomics so the audio
    // thread can read it without locking while the UI thread writes.
    void setStemMuted (int index, bool muted);
    void setStemSolo  (int index, bool solo);
    bool isStemMuted (int index) const;
    bool isStemSolo  (int index) const;
    /** A stem is audible if either no stem is soloed and it isn't muted, or
        some stem is soloed and this one is soloed (solo overrides mute). */
    bool isStemActive (int index) const;

    //----------------------------------------------------------------------
    // WorkerClient::Listener (called on the worker reader thread).
    void separationProgress (int tag, int done, int total, const juce::String& message) override;
    void separationDone (int tag, std::shared_ptr<StemSet> stems) override;
    void separationFailed (int tag, const juce::String& message, bool cancelled) override;
    void workerDied (const juce::String& reason) override;

private:
    void run() override;

    WorkerClient& worker;

    // Per-stem mute/solo state (slot 0..7), lock-free for audio-thread reads.
    mutable std::array<std::atomic<bool>, 8> stemMuted {};
    mutable std::array<std::atomic<bool>, 8> stemSolo {};

    juce::WaitableEvent resultEvent;

    std::atomic<State> state { State::idle };
    std::atomic<int> progressDone { 0 };
    std::atomic<int> progressTotal { 1 };
    std::atomic<double> hostSampleRate { 0.0 };
    std::atomic<double> lastNativeSampleRate { 0.0 };
    std::atomic<juce::int64> pendingTag { -1 };
    std::atomic<bool> cancelRequested { false };
    std::atomic<const char*> statusMessage { "Idle" };

    mutable juce::CriticalSection cacheLock;
    std::map<juce::String, std::shared_ptr<StemSet>> stemCache;
    juce::String lastSourceIdRaw;
    std::atomic<bool> lastSourceIdValid { false };
    std::atomic<const char*> lastSourceId { "" };

    mutable juce::CriticalSection errorLock;
    juce::String errorMessage;

    // Job context (accessed only on the engine thread unless noted).
    struct Job
    {
        SourceReader sourceReader;
        juce::String sourceId;
        juce::String model;
        juce::String modelDir;
        SeparationParams params;
        double hostSampleRate = 0.0;
    };
    std::unique_ptr<Job> currentJob;
    juce::CriticalSection jobLock;

    // scratch storage for status strings (keeps atomic pointers valid)
    juce::CriticalSection stringLock;
    juce::StringArray statusStrings;
    juce::String setStaticStatus (const juce::String& s);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeparationEngine)
};
