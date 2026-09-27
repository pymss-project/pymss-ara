#include "WorkerClient.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{
bool parseUnsigned64 (const juce::var& value, std::uint64_t& result)
{
    const auto text = value.toString().trim();
    if (text.isEmpty())
        return false;

    std::uint64_t parsed = 0;
    for (int index = 0; index < text.length(); ++index)
    {
        const auto character = text[index];
        if (character < '0' || character > '9')
            return false;
        const auto digit = static_cast<std::uint64_t> (character - '0');
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }

    result = parsed;
    return true;
}
} // namespace

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
        if (! SetHandleInformation (hChildStdoutRead, HANDLE_FLAG_INHERIT, 0)
            || ! SetHandleInformation (hChildStdinWrite, HANDLE_FLAG_INHERIT, 0))
        {
            error = "Could not restrict worker pipe inheritance (Windows error "
                  + juce::String ((int) GetLastError()) + ")";
            CloseHandle (hChildStdoutWrite);
            CloseHandle (hChildStdinRead);
            CloseHandle (hChildStdoutRead); hChildStdoutRead = nullptr;
            CloseHandle (hChildStdinWrite); hChildStdinWrite = nullptr;
            return false;
        }

        // Redirect stderr to a log file (or NUL if that fails) so Python has a
        // valid stderr handle and its tracebacks are recoverable.
        HANDLE hStdErr = INVALID_HANDLE_VALUE;
        if (stderrLog != juce::File())
            hStdErr = CreateFileW (stderrLog.getFullPathName().toWideCharPointer(),
                                   GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hStdErr == INVALID_HANDLE_VALUE)
            hStdErr = CreateFileW (L"NUL", GENERIC_WRITE, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);

        if (hStdErr == INVALID_HANDLE_VALUE)
        {
            error = "Could not open the worker error stream (Windows error "
                  + juce::String ((int) GetLastError()) + ")";
            CloseHandle (hChildStdoutWrite);
            CloseHandle (hChildStdinRead);
            CloseHandle (hChildStdoutRead); hChildStdoutRead = nullptr;
            CloseHandle (hChildStdinWrite); hChildStdinWrite = nullptr;
            return false;
        }

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof (si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = hChildStdinRead;
        si.StartupInfo.hStdOutput = hChildStdoutWrite;
        si.StartupInfo.hStdError = hStdErr;

        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList (nullptr, 1, 0, &attributeBytes);
        std::vector<juce::uint8> attributeStorage (attributeBytes);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST> (attributeStorage.data());
        const HANDLE inheritedHandles[] { hChildStdinRead, hChildStdoutWrite, hStdErr };
        const bool attributeListInitialized = attributeBytes > 0
            && InitializeProcThreadAttributeList (si.lpAttributeList, 1, 0, &attributeBytes);
        const bool attributesReady = attributeListInitialized
            && UpdateProcThreadAttribute (si.lpAttributeList,
                                          0,
                                          PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                          const_cast<HANDLE*> (inheritedHandles),
                                          sizeof (inheritedHandles),
                                          nullptr,
                                          nullptr);
        if (! attributesReady)
        {
            error = "Could not configure worker handle inheritance (Windows error "
                  + juce::String ((int) GetLastError()) + ")";
            if (attributeListInitialized)
                DeleteProcThreadAttributeList (si.lpAttributeList);
            CloseHandle (hChildStdoutWrite);
            CloseHandle (hChildStdinRead);
            CloseHandle (hStdErr);
            CloseHandle (hChildStdoutRead); hChildStdoutRead = nullptr;
            CloseHandle (hChildStdinWrite); hChildStdinWrite = nullptr;
            return false;
        }

        PROCESS_INFORMATION pi{};

        // CreateProcess may write into the command line buffer.
        std::vector<wchar_t> cmdBuf (commandLine.toWideCharPointer(),
                                     commandLine.toWideCharPointer() + commandLine.length() + 1);

        const auto ok = CreateProcessW (nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr, nullptr, &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList (si.lpAttributeList);

        // The child has its own copies now; close the parent's duplicate ends so
        // that ReadFile will return EOF once the child exits.
        CloseHandle (hChildStdoutWrite);
        CloseHandle (hChildStdinRead);
        CloseHandle (hStdErr);

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
    protocolCompatible = false;
    runtimeReady = false;
    startupState = StartupState::stopped;

    juce::ScopedLock sl (processLock);
    pythonExecutable = pythonExe;
    script = workerScript;
    stderrLog = stderrLogFile;

    juce::StringArray args;
    args.add (juce::String (pythonExe).quoted());
    args.add ("-u"); // unbuffered stdout/stderr
    args.add (workerScript.getFullPathName().quoted());

    juce::String error;
    if (! pimpl->startProcess (args.joinIntoString (" "), stderrLogFile, error))
    {
        started = false;
        notifyDied (error.isEmpty() ? "failed to start worker process" : error, true);
        return false;
    }

    started = true;
    startupState = StartupState::starting;
    startTimer (workerReadyTimeoutMs);
    startThread(); // Thread::startThread
    return true;
}

void WorkerClient::stop (const bool notifyActiveRequest)
{
    stopTimer();
    startupState = StartupState::stopped;
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

    bool hadActiveRequest = false;
    {
        juce::ScopedLock sl (processLock);
        if (pimpl)
            pimpl->closeAll();
        hadActiveRequest = releaseAllPendingInputs();
        protocolCompatible = false;
        runtimeReady = false;
    }

    if (notifyActiveRequest && hadActiveRequest)
        listeners.call ([] (Listener& listener)
        {
            listener.workerDied ("Python worker was restarted while a separation request was active.");
        });
}

bool WorkerClient::restart()
{
    juce::String python;
    juce::File workerScript;
    juce::File logFile;
    {
        juce::ScopedLock sl (processLock);
        python = pythonExecutable;
        workerScript = script;
        logFile = stderrLog;
    }

    if (python.isEmpty() || ! workerScript.existsAsFile())
        return false;
    return start (python, workerScript, logFile);
}

bool WorkerClient::isRunning() const
{
    return started && pimpl && pimpl->isAlive();
}

bool WorkerClient::isReady() const
{
    return isRunning()
        && startupState.load (std::memory_order_acquire) == StartupState::ready
        && protocolCompatible.load (std::memory_order_acquire)
        && runtimeReady.load (std::memory_order_acquire);
}

//==============================================================================
bool WorkerClient::sendFrame (const pymss_protocol::Header& header, const void* body, juce::uint32 bodySize)
{
    juce::ScopedLock sl (writeLock);
    if (! pimpl)
        return false;
    auto frame = pymss_protocol::buildFrame (header, body, bodySize);
    return pimpl->writeAll (frame.getData(), frame.getSize());
}

void WorkerClient::timerCallback()
{
    stopTimer();
    auto expected = StartupState::starting;
    if (! startupState.compare_exchange_strong (expected, StartupState::stopped))
        return;

    notifyDied ("Python worker did not complete its startup handshake within 10 seconds.");
    if (pimpl)
        pimpl->terminateProcess();
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

bool WorkerClient::requestModelInfo (const int tag, const juce::String& modelName,
                                     const juce::String& modelDir)
{
    if (tag <= 0 || ! isReady() || modelName.isEmpty())
        return false;

    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", tag);
    h->setProperty ("cmd", "model_info");
    h->setProperty ("model", modelName);
    h->setProperty ("model_dir", modelDir);
    return sendFrame (*h, nullptr, 0);
}

bool WorkerClient::requestModelDownload (const int tag, const juce::String& modelName,
                                         const juce::String& modelDir)
{
    if (tag <= 0 || ! isReady() || modelName.isEmpty())
        return false;

    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", tag);
    h->setProperty ("cmd", "download_model");
    h->setProperty ("model", modelName);
    h->setProperty ("model_dir", modelDir);
    return sendFrame (*h, nullptr, 0);
}

bool WorkerClient::requestSeparation (const int tag,
                                      const juce::String& model,
                                      const juce::String& modelDir,
                                      const SeparationParams& params,
                                      const juce::AudioBuffer<float>& audio,
                                      double sampleRate,
                                      int channels)
{
    if (tag <= 0 || ! isRunning() || ! protocolCompatible.load())
        return false;

    const int frames = audio.getNumSamples();
    if (frames <= 0 || channels <= 0 || channels > audio.getNumChannels()
        || channels > static_cast<int> (pymss_shm::maxChannels))
        return false;

    std::uint64_t payloadBytes = 0;
    if (! pymss_shm::checkedPayloadSize (static_cast<std::uint64_t> (frames),
                                         static_cast<std::uint32_t> (channels),
                                         payloadBytes))
        return false;

    const auto mappingBytes = pymss_shm::headerBytes + payloadBytes;
    const auto mappingName = pymss_shm::makeUniqueName (static_cast<std::uint64_t> (tag), "input");
    std::string mappingError;
    auto mapping = pymss_shm::SharedMemoryRegion::create (mappingName, mappingBytes, mappingError);
    if (mapping == nullptr)
        return false;

    if (! pymss_shm::writeHeader (mapping->data(), mapping->size(),
                                  pymss_shm::MappingRole::input,
                                  static_cast<std::uint64_t> (tag),
                                  payloadBytes,
                                  static_cast<std::uint64_t> (frames),
                                  static_cast<std::uint32_t> (channels),
                                  pymss_shm::MappingState::writing,
                                  mappingError))
        return false;

    auto* destination = static_cast<juce::uint8*> (mapping->data()) + pymss_shm::headerBytes;
    const auto channelBytes = static_cast<std::size_t> (frames) * sizeof (float);
    for (int channel = 0; channel < channels; ++channel)
        std::memcpy (destination + static_cast<std::size_t> (channel) * channelBytes,
                     audio.getReadPointer (channel),
                     channelBytes);

    if (! pymss_shm::markReady (mapping->data(), mapping->size(), mappingError))
        return false;

    {
        juce::ScopedLock sl (sharedMemoryLock);
        pendingInputMappings[tag] = std::move (mapping);
    }

    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", tag);
    h->setProperty ("cmd", "separate");
    h->setProperty ("protocol_version", (int) pymss_protocol::controlProtocolVersion);
    h->setProperty ("transport", pymss_protocol::sharedMemoryTransport);
    h->setProperty ("model", model);
    h->setProperty ("model_dir", modelDir);
    h->setProperty ("model_architecture", params.isVrModel ? "vr" : "mss");
    h->setProperty ("batch_size", params.batchSize);
    h->setProperty ("overlap_size", params.overlapSize);
    h->setProperty ("chunk_size", params.chunkSize);
    h->setProperty ("window_size", params.windowSize);
    h->setProperty ("aggression", params.aggression);
    h->setProperty ("post_process_threshold", params.postProcessThreshold);
    h->setProperty ("enable_tta", params.enableTta);
    h->setProperty ("standardize", params.standardize);
    h->setProperty ("high_end_process", params.highEndProcess);
    h->setProperty ("enable_post_process", params.enablePostProcess);
    h->setProperty ("normalize", params.normalize);

    auto input = std::make_unique<juce::DynamicObject>();
    input->setProperty ("name", juce::String::fromUTF8 (mappingName.c_str()));
    input->setProperty ("mapping_bytes", juce::String (static_cast<juce::int64> (mappingBytes)));
    input->setProperty ("payload_offset", (int) pymss_shm::headerBytes);
    input->setProperty ("payload_bytes", juce::String (static_cast<juce::int64> (payloadBytes)));
    input->setProperty ("format", "float32_le");
    input->setProperty ("layout", "planar");
    input->setProperty ("frames", juce::String (frames));
    input->setProperty ("channels", channels);
    input->setProperty ("sample_rate", (int) juce::roundToInt (sampleRate));
    h->setProperty ("input", juce::var (input.release()));

    if (! sendFrame (*h, nullptr, 0))
    {
        releasePendingInput (tag);
        return false;
    }
    return true;
}

void WorkerClient::cancelSeparation (const int tag)
{
    if (tag <= 0)
        return;

    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", tag);
    h->setProperty ("cmd", "cancel");
    sendFrame (*h, nullptr, 0);
}

void WorkerClient::releasePendingInput (const int tag)
{
    juce::ScopedLock sl (sharedMemoryLock);
    pendingInputMappings.erase (tag);
}

bool WorkerClient::releaseAllPendingInputs()
{
    juce::ScopedLock sl (sharedMemoryLock);
    const bool hadPendingInputs = ! pendingInputMappings.empty();
    pendingInputMappings.clear();
    return hadPendingInputs;
}

void WorkerClient::sendReleaseBuffer (const int tag)
{
    auto h = pymss_protocol::makeHeader();
    h->setProperty ("id", tag);
    h->setProperty ("cmd", "release_buffer");
    sendFrame (*h, nullptr, 0);
}

bool WorkerClient::readSharedMemoryResult (const int tag,
                                           const juce::var& header,
                                           std::shared_ptr<StemSet>& stems,
                                           juce::String& error)
{
    if ((int) header.getProperty ("protocol_version", 0)
            != static_cast<int> (pymss_protocol::controlProtocolVersion)
        || header.getProperty ("transport", "").toString() != pymss_protocol::sharedMemoryTransport)
    {
        error = "Worker returned an unsupported separation transport";
        return false;
    }

    const auto output = header.getProperty ("output", juce::var());
    if (! output.isObject())
    {
        error = "Worker result is missing shared memory metadata";
        return false;
    }

    const auto mappingName = output.getProperty ("name", "").toString();
    std::uint64_t mappingBytes = 0;
    std::uint64_t payloadBytes = 0;
    const auto payloadOffset = (int) output.getProperty ("payload_offset", 0);
    if (! mappingName.startsWith ("Local\\PyMSS_") || mappingName.length() > 240
        || ! parseUnsigned64 (output.getProperty ("mapping_bytes", juce::var()), mappingBytes)
        || ! parseUnsigned64 (output.getProperty ("payload_bytes", juce::var()), payloadBytes)
        || payloadOffset != static_cast<int> (pymss_shm::headerBytes)
        || mappingBytes != pymss_shm::headerBytes + payloadBytes)
    {
        error = "Worker returned invalid shared memory metadata";
        return false;
    }

    if (payloadBytes > pymss_shm::maxResultPayloadBytes)
    {
        error = "Worker result exceeds the supported 2 GiB payload limit";
        return false;
    }

#if JUCE_WINDOWS
    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof (memoryStatus);
    if (GlobalMemoryStatusEx (&memoryStatus)
        && payloadBytes > memoryStatus.ullAvailPhys / 2)
    {
        error = "Insufficient available memory to copy the worker result safely";
        return false;
    }
#endif

    std::string mappingError;
    auto mapping = pymss_shm::SharedMemoryRegion::openReadOnly (mappingName.toStdString(),
                                                                mappingBytes,
                                                                mappingError);
    if (mapping == nullptr)
    {
        error = juce::String::fromUTF8 (mappingError.c_str());
        return false;
    }

    pymss_shm::HeaderInfo mappingHeader;
    if (! pymss_shm::readAndValidateHeader (mapping->data(),
                                            mapping->size(),
                                            pymss_shm::MappingRole::output,
                                            static_cast<std::uint64_t> (tag),
                                            mappingHeader,
                                            mappingError)
        || mappingHeader.payloadBytes != payloadBytes
        || mappingHeader.frames != 0
        || mappingHeader.channels != 0)
    {
        if (mappingError.empty())
            error = "Shared memory payload metadata does not match";
        else
            error = juce::String::fromUTF8 (mappingError.c_str());
        return false;
    }

    auto* stemArray = header.getProperty ("stems", juce::var()).getArray();
    if (stemArray == nullptr || stemArray->isEmpty()
        || stemArray->size() > static_cast<int> (pymss_shm::maxStems))
    {
        error = "Worker returned an invalid stem list";
        return false;
    }

    struct StemDescriptor
    {
        juce::String name;
        std::uint64_t offset = 0;
        std::uint64_t frames = 0;
        std::uint32_t channels = 0;
        std::uint64_t bytes = 0;
    };

    std::vector<StemDescriptor> descriptors;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    descriptors.reserve (static_cast<std::size_t> (stemArray->size()));
    ranges.reserve (static_cast<std::size_t> (stemArray->size()));

    for (const auto& stemVar : *stemArray)
    {
        StemDescriptor descriptor;
        descriptor.name = stemVar.getProperty ("name", "").toString();
        descriptor.channels = static_cast<std::uint32_t> ((int) stemVar.getProperty ("channels", 0));
        if (descriptor.name.isEmpty() || descriptor.name.length() > 256
            || stemVar.getProperty ("layout", "").toString() != "planar"
            || ! parseUnsigned64 (stemVar.getProperty ("offset_bytes", juce::var()), descriptor.offset)
            || ! parseUnsigned64 (stemVar.getProperty ("frames", juce::var()), descriptor.frames)
            || descriptor.frames > static_cast<std::uint64_t> ((std::numeric_limits<int>::max)())
            || ! pymss_shm::checkedPayloadSize (descriptor.frames, descriptor.channels, descriptor.bytes)
            || descriptor.offset % sizeof (float) != 0
            || descriptor.offset > payloadBytes
            || descriptor.bytes > payloadBytes - descriptor.offset)
        {
            error = "Worker returned invalid stem buffer metadata";
            return false;
        }

        ranges.emplace_back (descriptor.offset, descriptor.offset + descriptor.bytes);
        descriptors.push_back (std::move (descriptor));
    }

    std::sort (ranges.begin(), ranges.end());
    for (std::size_t index = 1; index < ranges.size(); ++index)
    {
        if (ranges[index].first < ranges[index - 1].second)
        {
            error = "Worker returned overlapping stem buffers";
            return false;
        }
    }

    try
    {
        auto result = std::make_shared<StemSet>();
        result->sampleRate = (double) header.getProperty ("sample_rate", 0.0);
        if (result->sampleRate <= 0.0 || result->sampleRate > 768000.0)
        {
            error = "Worker returned an invalid sample rate";
            return false;
        }

        const auto* payload = static_cast<const juce::uint8*> (mapping->data()) + pymss_shm::headerBytes;
        for (const auto& descriptor : descriptors)
        {
            StemBuffer buffer;
            buffer.name = descriptor.name;
            buffer.channels = static_cast<int> (descriptor.channels);
            buffer.frames = static_cast<juce::int64> (descriptor.frames);
            buffer.data.setSize (buffer.channels, static_cast<int> (descriptor.frames));

            const auto channelBytes = descriptor.frames * sizeof (float);
            for (int channel = 0; channel < buffer.channels; ++channel)
            {
                const auto channelOffset = descriptor.offset
                                         + static_cast<std::uint64_t> (channel) * channelBytes;
                const auto* source = reinterpret_cast<const float*> (
                    payload + static_cast<std::size_t> (channelOffset));
                buffer.data.copyFrom (channel, 0, source, static_cast<int> (descriptor.frames));
            }

            result->stemNames.add (buffer.name);
            result->stems.push_back (std::move (buffer));
        }

        stems = std::move (result);
    }
    catch (const std::bad_alloc&)
    {
        error = "Insufficient memory to allocate separated stem buffers";
        return false;
    }
    error.clear();
    return true;
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

        if (headerLen > (8u * 1024u * 1024u) || bodyLen > (1024u * 1024u))
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
        const int protocolVersion = (int) header.getProperty ("protocol_version", 0);
        bool supportsSharedMemory = false;
        if (auto* capabilities = header.getProperty ("capabilities", juce::var()).getArray())
            for (const auto& capability : *capabilities)
                supportsSharedMemory = supportsSharedMemory
                                    || capability.toString() == pymss_protocol::sharedMemoryTransport;

        if (protocolVersion != static_cast<int> (pymss_protocol::controlProtocolVersion)
            || ! supportsSharedMemory)
        {
            auto expected = StartupState::starting;
            if (! startupState.compare_exchange_strong (expected, StartupState::stopped))
                return;
            stopTimer();
            protocolCompatible = false;
            runtimeReady = false;
            notifyDied ("Worker protocol or shared-memory capability mismatch (expected version "
                        + juce::String ((int) pymss_protocol::controlProtocolVersion)
                        + ", received " + juce::String (protocolVersion) + ")");
            if (pimpl)
                pimpl->terminateProcess();
            return;
        }

        auto expected = StartupState::starting;
        if (! startupState.compare_exchange_strong (expected, StartupState::ready))
            return;
        stopTimer();
        protocolCompatible = true;
        const bool workerRuntimeReady = (bool) header.getProperty ("ok", false);
        runtimeReady = workerRuntimeReady;
        listeners.call ([&] (Listener& l)
        {
            l.workerReady (workerRuntimeReady,
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

    if (type == "download_progress")
    {
        const auto done = (juce::int64) header.getProperty ("done", 0);
        const auto total = (juce::int64) header.getProperty ("total", 0);
        const auto msg = header.getProperty ("message", "").toString();
        listeners.call ([&] (Listener& l) { l.modelDownloadProgress (id, done, total, msg); });
        return;
    }

    if (type == "error")
    {
        const auto errorType = header.getProperty ("error_type", "").toString();
        if (errorType == "download_failed")
        {
            const auto msg = header.getProperty ("message", "Model download failed").toString();
            listeners.call ([&] (Listener& l) { l.modelDownloadFailed (id, msg); });
            return;
        }

        if (errorType == "model_info_failed")
        {
            const auto msg = header.getProperty ("message", "Could not load model information").toString();
            listeners.call ([&] (Listener& l) { l.modelInfoFailed (id, msg); });
            return;
        }

        releasePendingInput (id);
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
            releasePendingInput (id);
            std::shared_ptr<StemSet> stems;
            juce::String sharedMemoryError;
            const bool ok = body.getSize() == 0
                         && readSharedMemoryResult (id, header, stems, sharedMemoryError);
            sendReleaseBuffer (id);

            if (! ok)
            {
                const auto message = body.getSize() == 0 ? sharedMemoryError
                                                         : juce::String ("Worker returned an unexpected pipe payload");
                listeners.call ([&] (Listener& l) { l.separationFailed (id, message, false); });
                return;
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

        if (header.hasProperty ("downloaded_model"))
        {
            const auto modelName = header.getProperty ("downloaded_model", "").toString();
            const auto info = header.getProperty ("info", juce::var());
            listeners.call ([&] (Listener& l) { l.modelDownloadDone (id, modelName, info); });
            return;
        }

        releasePendingInput (id);
    }
}

void WorkerClient::notifyDied (const juce::String& reason, const bool forceNotification)
{
    const bool wasStarted = started.exchange (false);
    if (! wasStarted && ! forceNotification)
        return;
    stopTimer();
    startupState = StartupState::stopped;
    releaseAllPendingInputs();
    protocolCompatible = false;
    runtimeReady = false;
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
