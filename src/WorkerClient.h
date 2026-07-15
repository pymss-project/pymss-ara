#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Stems.h"
#include "WorkerProtocol.h"

/** Asynchronous client that talks to python/worker.py over a stdin/stdout
    binary framing protocol.

    On Windows the Python process is launched with Win32 CreateProcess using
    anonymous pipes for stdin/stdout. A dedicated reader thread blocks on the
    stdout pipe, parses frames and dispatches events to the registered
    listeners (all callbacks fire on the reader thread, never the message
    thread — implementations must be thread-safe or hop threads themselves).
*/
class WorkerClient : private juce::Thread
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;

        /** Emitted once after the worker process announces readiness. */
        virtual void workerReady (bool pymssOk, const juce::String& version, const juce::String& message) {}

        virtual void pymssCheckResult (int tag, bool ok, const juce::String& version, const juce::String& message) {}

        virtual void modelListResult (int tag, const juce::Array<juce::var>& models) {}

        virtual void modelInfoResult (int tag, const juce::var& info) {}

        virtual void separationProgress (int tag, int done, int total, const juce::String& message) {}

        virtual void separationDone (int tag, std::shared_ptr<StemSet> stems) {}

        virtual void separationFailed (int tag, const juce::String& message, bool cancelled) {}

        /** The worker process died unexpectedly (crash, EOF, failed launch). */
        virtual void workerDied (const juce::String& reason) {}
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
    void stop();

    bool isRunning() const;

    /** Resolve the resource directory containing worker.py for the currently
        running plugin bundle. */
    static juce::File findWorkerScript();

    //----------------------------------------------------------------------
    // Request API. Each call returns a tag that identifies matching replies.

    int checkPymss();
    int requestModelList (const juce::String& modelDir);
    int requestModelInfo (const juce::String& modelName, const juce::String& modelDir);

    /** Request separation. The audio buffer is sent as interleaved float32 in
        the body (channels as given). Returns the tag, or -1 if not running. */
    int requestSeparation (const juce::String& model,
                           const juce::String& modelDir,
                           const SeparationParams& params,
                           const juce::AudioBuffer<float>& audio,
                           double sampleRate,
                           int channels);

    void cancelSeparation();

private:
    void run() override;
    void sendFrame (const pymss_protocol::Header& header, const void* body, juce::uint32 bodySize);
    void handleFrame (const juce::var& header, const juce::MemoryBlock& body);

    void notifyDied (const juce::String& reason);

    juce::ListenerList<Listener> listeners;
    std::atomic<int> nextTag { 1 };

    juce::CriticalSection writeLock;
    juce::CriticalSection processLock;

    std::atomic<bool> started { false };
    juce::String pythonExecutable;
    juce::File script;

    //----------------------------------------------------------------------
    // Platform process state. Defined in the platform .cpp.
    struct Pimpl;
    std::unique_ptr<Pimpl> pimpl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WorkerClient)
};
