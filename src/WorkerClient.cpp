#include "WorkerClient.h"
#include <cstring>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

//==============================================================================
#if JUCE_WINDOWS

struct WorkerClient::Pimpl
{
    ~Pimpl() { closeAll(); }

    void terminateProcess()
    {
        if (hProcess)
            TerminateProcess (hProcess, 1);
    }

    void closeAll()
    {
        if (hChildStdinWrite) { CloseHandle (hChildStdinWrite); hChildStdinWrite = nullptr; }
        if (hChildStdoutRead) { CloseHandle (hChildStdoutRead); hChildStdoutRead = nullptr; }
        if (hProcess) { TerminateProcess (hProcess, 1); CloseHandle (hProcess); hProcess = nullptr; }
        if (hThread)  { CloseHandle (hThread); hThread = nullptr; }
    }

    bool startProcess (const juce::String& commandLine, const juce::File& stderrLog, juce::String& error)
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof (sa);
        sa.bInheritHandle = TRUE;

        HANDLE hChildStdoutWrite = nullptr;
        HANDLE hChildStdinRead   = nullptr;

        if (! CreatePipe (&hChildStdoutRead, &hChildStdoutWrite, &sa, 0))
        {
            error = "CreatePipe(stdout) failed";
            return false;
        }
        if (! CreatePipe (&hChildStdinRead, &hChildStdinWrite, &sa, 0))
        {
            error = "CreatePipe(stdin) failed";
            CloseHandle (hChildStdoutRead); hChildStdoutRead = nullptr;
            CloseHandle (hChildStdoutWrite);
            return false;
        }

        // Parent's ends must not be inherited by the child.
        SetHandleInformation (hChildStdoutRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation (hChildStdinWrite, HANDLE_FLAG_INHERIT, 0);

        // Redirect stderr to a log file (or NUL if that fails) so Python has a
        // valid stderr handle and its tracebacks are recoverable.
        HANDLE hStdErr = nullptr;
        if (stderrLog != juce::File())
            hStdErr = CreateFileW (stderrLog.getFullPathName().toWideCharPointer(),
                                   GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (! hStdErr)
            hStdErr = CreateFileW (L"NUL", GENERIC_WRITE, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof (si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hChildStdinRead;
        si.hStdOutput = hChildStdoutWrite;
        si.hStdError = hStdErr;

        PROCESS_INFORMATION pi{};

        // CreateProcess may write into the command line buffer.
        std::vector<wchar_t> cmdBuf (commandLine.toWideCharPointer(),
                                     commandLine.toWideCharPointer() + commandLine.length() + 1);

        const auto ok = CreateProcessW (nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

        // The child has its own copies now; close the parent's duplicate ends so
        // that ReadFile will return EOF once the child exits.
        CloseHandle (hChildStdoutWrite);
        CloseHandle (hChildStdinRead);
        if (hStdErr) CloseHandle (hStdErr);

        if (! ok)
        {
            error = "CreateProcessW failed (error " + juce::String ((int) GetLastError()) + ")";
            CloseHandle (hChildStdoutRead); hChildStdoutRead = nullptr;
            CloseHandle (hChildStdinWrite); hChildStdinWrite = nullptr;
            return false;
        }

        hProcess = pi.hProcess;
        hThread  = pi.hThread;
        return true;
    }

    bool readExact (void* dest, size_t n)
    {
        auto* p = static_cast<juce::uint8*> (dest);
        while (n > 0)
        {
            DWORD got = 0;
            if (! ReadFile (hChildStdoutRead, p, (DWORD) n, &got, nullptr) || got == 0)
                return false;
            p += got;
            n -= got;
        }
        return true;
    }

    bool writeAll (const void* src, size_t n)
    {
        const auto* p = static_cast<const juce::uint8*> (src);
        while (n > 0)
        {
            DWORD wrote = 0;
            if (! WriteFile (hChildStdinWrite, p, (DWORD) n, &wrote, nullptr))
                return false;
            p += wrote;
            n -= wrote;
        }
        return true;
    }

    bool isAlive()
    {
        if (! hProcess) return false;
        DWORD code = 0;
        if (GetExitCodeProcess (hProcess, &code))
            return code == STILL_ACTIVE;
        return false;
    }

    HANDLE hChildStdinWrite = nullptr;
    HANDLE hChildStdoutRead = nullptr;
    HANDLE hProcess = nullptr;
    HANDLE hThread  = nullptr;
};

#else
// Non-Windows: not yet implemented (Windows-first per project scope).
struct WorkerClient::Pimpl
{
    bool startProcess (const juce::String&, const juce::File&, juce::String& error)
    {
        error = "WorkerClient only supports Windows currently";
        return false;
    }
    bool readExact (void*, size_t) { return false; }
    bool writeAll (const void*, size_t) { return false; }
    bool isAlive() { return false; }
    void terminateProcess() {}
};
#endif

//==============================================================================
WorkerClient::WorkerClient()
    : juce::Thread ("pymss-worker-reader"), pimpl (std::make_unique<Pimpl>())
{
}

WorkerClient::~WorkerClient()
{
    stop();
}

bool WorkerClient::start (const juce::String& pythonExe, const juce::File& workerScript,
                          const juce::File& stderrLogFile)
{
    stop();

    juce::ScopedLock sl (processLock);
    pythonExecutable = pythonExe;
    script = workerScript;

    juce::StringArray args;
    args.add (juce::String (pythonExe).quoted());
    args.add (workerScript.getFullPathName().quoted());
    // Keep the worker from opening a console window / interactive prompts.
    args.add ("-u"); // unbuffered stdout/stderr

    juce::String error;
    if (! pimpl->startProcess (args.joinIntoString (" "), stderrLogFile, error))
    {
        started = false;
        notifyDied (error.isEmpty() ? "failed to start worker process" : error);
        return false;
    }

    started = true;
    startThread(); // Thread::startThread
    return true;
}

void WorkerClient::stop()
{
    if (isThreadRunning())
    {
        // Suppress the workerDied notification during an explicit, orderly stop.
        started = false;

        if (pimpl && pimpl->isAlive())
        {
            pymss_protocol::HeaderPtr h = pymss_protocol::makeHeader();
            h->setProperty ("id", 0);
            h->setProperty ("cmd", "shutdown");
            sendFrame (*h, nullptr, 0);
        }

        signalThreadShouldExit();

        // Kill the child first so the reader's blocking ReadFile returns EOF;
        // only then is it safe to close the parent's pipe handles.
        pimpl->terminateProcess();
        stopThread (2000);
    }
    else
    {
        started = false;
    }

    juce::ScopedLock sl (processLock);
    if (pimpl)
        pimpl->closeAll();
}

bool WorkerClient::isRunning() const
{
    return started && pimpl && pimpl->isAlive();
}

//==============================================================================
void WorkerClient::sendFrame (const pymss_protocol::Header& header, const void* body, juce::uint32 bodySize)
{
    juce::ScopedLock sl (writeLock);
    if (! pimpl) return;
    auto frame = pymss_protocol::buildFrame (header, body, bodySize);
    pimpl->writeAll (frame.getData(), frame.getSize());
}

int WorkerClient::checkPymss()
{
    auto tag = nextTag.fetch_add (1);
    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", tag);
    h->setProperty ("cmd", "check_pymss");
    sendFrame (*h, nullptr, 0);
    return tag;
}

int WorkerClient::requestModelList (const juce::String& modelDir)
{
    auto tag = nextTag.fetch_add (1);
    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", tag);
    h->setProperty ("cmd", "list_models");
    h->setProperty ("model_dir", modelDir);
    sendFrame (*h, nullptr, 0);
    return tag;
}

int WorkerClient::requestModelInfo (const juce::String& modelName, const juce::String& modelDir)
{
    auto tag = nextTag.fetch_add (1);
    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", tag);
    h->setProperty ("cmd", "model_info");
    h->setProperty ("model", modelName);
    h->setProperty ("model_dir", modelDir);
    sendFrame (*h, nullptr, 0);
    return tag;
}

int WorkerClient::requestSeparation (const juce::String& model,
                                     const juce::String& modelDir,
                                     const SeparationParams& params,
                                     const juce::AudioBuffer<float>& audio,
                                     double sampleRate,
                                     int channels)
{
    if (! isRunning())
        return -1;

    auto tag = nextTag.fetch_add (1);

    // Build interleaved float32 body from the channel-major buffer.
    const int frames = audio.getNumSamples();
    const juce::uint32 bodySize = (juce::uint32) ((size_t) frames * (size_t) channels * sizeof (float));
    juce::HeapBlock<float> interleaved (bodySize > 0 ? (size_t) frames * (size_t) channels : 1, true);

    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            interleaved[(size_t) i * channels + c] = audio.getSample (c, i);

    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", tag);
    h->setProperty ("cmd", "separate");
    h->setProperty ("model", model);
    h->setProperty ("model_dir", modelDir);
    h->setProperty ("sample_rate", (int) juce::roundToInt (sampleRate));
    h->setProperty ("channels", channels);
    h->setProperty ("batch_size", params.batchSize);
    h->setProperty ("overlap_size", params.overlapSize);
    h->setProperty ("chunk_size", params.chunkSize);
    h->setProperty ("normalize", params.normalize);

    sendFrame (*h, interleaved, bodySize);
    return tag;
}

void WorkerClient::cancelSeparation()
{
    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", 0);
    h->setProperty ("cmd", "cancel");
    sendFrame (*h, nullptr, 0);
}

//==============================================================================
void WorkerClient::run()
{
    using namespace pymss_protocol;

    while (! threadShouldExit())
    {
        juce::uint8 prefix[8];
        if (! pimpl->readExact (prefix, 8))
            break;

        const juce::uint32 headerLen = (juce::uint32) prefix[0]
                                     | ((juce::uint32) prefix[1] << 8)
                                     | ((juce::uint32) prefix[2] << 16)
                                     | ((juce::uint32) prefix[3] << 24);
        const juce::uint32 bodyLen = (juce::uint32) prefix[4]
                                   | ((juce::uint32) prefix[5] << 8)
                                   | ((juce::uint32) prefix[6] << 16)
                                   | ((juce::uint32) prefix[7] << 24);

        if (headerLen > (8u * 1024u * 1024u) || bodyLen > (2048u * 1024u * 1024u))
            break; // sanity guard

        juce::HeapBlock<char> headerBuf (headerLen + 1, true);
        if (headerLen > 0 && ! pimpl->readExact (headerBuf, headerLen))
            break;
        headerBuf[headerLen] = 0;

        juce::MemoryBlock body;
        if (bodyLen > 0)
        {
            body.ensureSize (bodyLen);
            if (! pimpl->readExact (body.getData(), bodyLen))
                break;
        }

        auto header = juce::JSON::parse (juce::String (headerBuf.getData()));
        if (! header.isObject())
            continue;

        handleFrame (header, body);
    }

    notifyDied ("worker stream ended");
}

void WorkerClient::handleFrame (const juce::var& header, const juce::MemoryBlock& body)
{
    const auto type = header.getProperty ("type", "").toString();
    const int id = (int) header.getProperty ("id", 0);

    if (type == "ready")
    {
        listeners.call ([&] (Listener& l)
        {
            l.workerReady ((bool) header.getProperty ("ok", false),
                           header.getProperty ("version", "").toString(),
                           header.getProperty ("message", "").toString());
        });
        return;
    }

    if (type == "progress")
    {
        const int done = (int) header.getProperty ("done", 0);
        const int total = (int) header.getProperty ("total", 1);
        const auto msg = header.getProperty ("message", "").toString();
        listeners.call ([&] (Listener& l) { l.separationProgress (id, done, total, msg); });
        return;
    }

    if (type == "error")
    {
        const auto errorType = header.getProperty ("error_type", "").toString();
        const bool cancelled = errorType == "cancelled";
        const auto msg = header.getProperty ("message", "unknown error").toString();
        listeners.call ([&] (Listener& l) { l.separationFailed (id, msg, cancelled); });
        return;
    }

    if (type == "result")
    {
        if (header.hasProperty ("pong"))
            return;

        if (header.hasProperty ("models"))
        {
            listeners.call ([&] (Listener& l)
            {
                if (auto* arr = header.getProperty ("models", juce::var()).getArray())
                    l.modelListResult (id, *arr);
                else
                    l.modelListResult (id, {});
            });
            return;
        }

        if (header.hasProperty ("found")) // model_info reply
        {
            listeners.call ([&] (Listener& l) { l.modelInfoResult (id, header); });
            return;
        }

        if (header.hasProperty ("stems")) // separation result
        {
            auto stems = std::make_shared<StemSet>();
            stems->sampleRate = (double) header.getProperty ("sample_rate", 44100);

            if (auto* stemArray = header.getProperty ("stems", juce::var()).getArray())
            {
                const float* src = static_cast<const float*> (body.getData());
                const size_t totalFloats = body.getSize() / sizeof (float);
                size_t offset = 0;

                for (auto& stemVar : *stemArray)
                {
                    StemBuffer buf;
                    buf.name = stemVar.getProperty ("name", "").toString();
                    buf.channels = (int) stemVar.getProperty ("channels", 1);
                    buf.frames = (juce::int64) stemVar.getProperty ("frames", 0);

                    const size_t count = (size_t) buf.channels * (size_t) buf.frames;
                    if (offset + count > totalFloats)
                        break;

                    buf.data.setSize (buf.channels, (int) buf.frames);
                    for (int c = 0; c < buf.channels; ++c)
                        for (juce::int64 i = 0; i < buf.frames; ++i)
                            buf.data.setSample (c, (int) i, src[offset + (size_t) i * buf.channels + c]);

                    offset += count;
                    stems->stemNames.add (buf.name);
                    stems->stems.push_back (std::move (buf));
                }
            }

            listeners.call ([&] (Listener& l) { l.separationDone (id, stems); });
            return;
        }

        if (header.hasProperty ("ok")) // check_pymss reply
        {
            listeners.call ([&] (Listener& l)
            {
                l.pymssCheckResult (id,
                                    (bool) header.getProperty ("ok", false),
                                    header.getProperty ("version", "").toString(),
                                    header.getProperty ("message", "").toString());
            });
            return;
        }
    }
}

void WorkerClient::notifyDied (const juce::String& reason)
{
    if (! started)
        return;
    started = false;
    listeners.call ([&] (Listener& l) { l.workerDied (reason); });
}

//==============================================================================
juce::File WorkerClient::findWorkerScript()
{
    // currentApplicationFile is the plugin binary itself. On Windows VST3 the
    // binary is <bundle>.vst3/Contents/<arch>/Binary.vst3 (note: the inner
    // binary file also has a .vst3 extension, which previously fooled a naive
    // "walk up to the .vst3 directory" lookup). worker.py ships in
    // <bundle>/Contents/Resources/, so walk up to the "Contents" directory.
    auto p = juce::File::getSpecialLocation (juce::File::currentApplicationFile);

    for (int i = 0; i < 8; ++i)
    {
        if (p.isDirectory() && p.getFileName() == "Contents")
        {
            auto res = p.getChildFile ("Resources").getChildFile ("worker.py");
            if (res.existsAsFile())
                return res;
            break;
        }
        p = p.getParentDirectory();
    }

    // Fallback for dev builds: <project>/python/worker.py (CWD-relative).
    auto devRes = juce::File::getCurrentWorkingDirectory().getChildFile ("python").getChildFile ("worker.py");
    if (devRes.existsAsFile())
        return devRes;

    return {};
}
