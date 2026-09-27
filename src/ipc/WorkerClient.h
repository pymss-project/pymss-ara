#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <map>

#include "../core/Stems.h"
#include "WorkerProtocol.h"

/** Asynchronous client that talks to python/worker.py over a stdin/stdout
    control protocol. Separation audio is transferred through named shared
    memory, while JSON commands, progress and errors remain pipe-framed.

    On Windows the Python process is launched with Win32 CreateProcess using
    anonymous pipes for stdin/stdout. A dedicated reader thread blocks on the
    stdout pipe, parses frames and dispatches events to the registered
    listeners (all callbacks fire on the reader thread, never the message
    thread; implementations must be thread-safe or hop threads themselves).
*/
class WorkerClient : private juce::Thread,
                     private juce::Timer
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;

        /** Emitted once after the worker process announces readiness. */
        virtual void workerReady (bool, const juce::String&, const juce::String&) {}

        virtual void pymssCheckResult (int, bool, const juce::String&, const juce::String&) {}

        virtual void modelListResult (int, const juce::Array<juce::var>&) {}

        virtual void modelInfoResult (int, const juce::var&) {}

        virtual void modelInfoFailed (int, const juce::String&) {}

        virtual void modelDownloadProgress (int, juce::int64, juce::int64, const juce::String&) {}

        virtual void modelDownloadDone (int, const juce::String&, const juce::var&) {}

        virtual void modelDownloadFailed (int, const juce::String&) {}

        virtual void separationProgress (int, int, int, const juce::String&) {}

        virtual void separationDone (int, std::shared_ptr<StemSet>) {}

        virtual void separationFailed (int, const juce::String&, bool) {}

        /** The worker process died unexpectedly (crash, EOF, failed launch). */
        virtual void workerDied (const juce::String&) {}
    };

    WorkerClient();
    ~WorkerClient() override;

    void addListener (Listener* l)     { listeners.add (l); }
    void removeListener (Listener* l)  { listeners.remove (l); }

    /** Launch the worker process. Returns false (and notifies workerDied) if
        the process could not be started. Safe to call again after stop(). */
    bool start (const juce::String& pythonExe, const juce::File& workerScript,
                const juce::File& stderrLogFile);

    /** Shut the worker down gracefully (sends "shutdown") then kills it. */
    void stop (bool notifyActiveRequest = false);

    /** Restart with the most recently supplied executable, script and log path. */
    bool restart();

    bool isRunning() const;
    bool isReady() const;

    /** Resolve the resource directory containing worker.py for the currently
        running plugin bundle. */
    static juce::File findWorkerScript();

    //----------------------------------------------------------------------
    // Request API. Each call returns a tag that identifies matching replies.

    int checkPymss();
    int requestModelList (const juce::String& modelDir);
    bool requestModelInfo (int tag, const juce::String& modelName, const juce::String& modelDir);
    bool requestModelDownload (int tag, const juce::String& modelName, const juce::String& modelDir);

    /** Reserve a request tag so listeners can publish it before any response
        can arrive from the worker. */
    int reserveRequestTag() noexcept { return nextTag.fetch_add (1); }

    /** Request separation with a previously reserved tag. The audio buffer is
        exposed to the worker as planar float32 shared memory. */
    bool requestSeparation (int tag,
                            const juce::String& model,
                            const juce::String& modelDir,
                            const SeparationParams& params,
                            const juce::AudioBuffer<float>& audio,
                            double sampleRate,
                            int channels);

    void cancelSeparation (int tag);

private:
    enum class StartupState { stopped, starting, ready };

    void run() override;
    void timerCallback() override;
    bool sendFrame (const pymss_protocol::Header& header, const void* body, juce::uint32 bodySize);
    void handleFrame (const juce::var& header, const juce::MemoryBlock& body);
    bool readSharedMemoryResult (int tag,
                                 const juce::var& header,
                                 std::shared_ptr<StemSet>& stems,
                                 juce::String& error);
    void releasePendingInput (int tag);
    bool releaseAllPendingInputs();
    void sendReleaseBuffer (int tag);

    void notifyDied (const juce::String& reason, bool forceNotification = false);

    juce::ListenerList<Listener> listeners;
    std::atomic<int> nextTag { 1 };

    juce::CriticalSection writeLock;
    juce::CriticalSection processLock;
    juce::CriticalSection sharedMemoryLock;
    std::map<int, std::unique_ptr<pymss_shm::SharedMemoryRegion>> pendingInputMappings;

    std::atomic<bool> started { false };
    std::atomic<bool> protocolCompatible { false };
    std::atomic<bool> runtimeReady { false };
    std::atomic<StartupState> startupState { StartupState::stopped };
    juce::String pythonExecutable;
    juce::File script;
    juce::File stderrLog;

    static constexpr int workerReadyTimeoutMs = 10000;

    //----------------------------------------------------------------------
    // Platform process state. Defined in the platform .cpp.
    struct Pimpl;
    std::unique_ptr<Pimpl> pimpl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WorkerClient)
};
