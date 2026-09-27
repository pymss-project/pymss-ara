#include "SharedMemoryRegion.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <utility>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace pymss_shm
{
namespace
{
constexpr std::size_t magicOffset = 0;
constexpr std::size_t versionOffset = 4;
constexpr std::size_t headerSizeOffset = 8;
constexpr std::size_t roleOffset = 12;
constexpr std::size_t requestIdOffset = 16;
constexpr std::size_t payloadBytesOffset = 24;
constexpr std::size_t framesOffset = 32;
constexpr std::size_t channelsOffset = 40;
constexpr std::size_t stateOffset = 44;
constexpr std::size_t identityOffset = 48;

void writeU32 (void* base, const std::size_t offset, const std::uint32_t value) noexcept
{
    auto* p = static_cast<std::uint8_t*> (base) + offset;
    p[0] = static_cast<std::uint8_t> (value);
    p[1] = static_cast<std::uint8_t> (value >> 8);
    p[2] = static_cast<std::uint8_t> (value >> 16);
    p[3] = static_cast<std::uint8_t> (value >> 24);
}

void writeU64 (void* base, const std::size_t offset, const std::uint64_t value) noexcept
{
    auto* p = static_cast<std::uint8_t*> (base) + offset;
    for (std::size_t i = 0; i < sizeof (value); ++i)
        p[i] = static_cast<std::uint8_t> (value >> (i * 8));
}

std::uint32_t readU32 (const void* base, const std::size_t offset) noexcept
{
    const auto* p = static_cast<const std::uint8_t*> (base) + offset;
    return static_cast<std::uint32_t> (p[0])
         | (static_cast<std::uint32_t> (p[1]) << 8)
         | (static_cast<std::uint32_t> (p[2]) << 16)
         | (static_cast<std::uint32_t> (p[3]) << 24);
}

std::uint64_t readU64 (const void* base, const std::size_t offset) noexcept
{
    const auto* p = static_cast<const std::uint8_t*> (base) + offset;
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < sizeof (result); ++i)
        result |= static_cast<std::uint64_t> (p[i]) << (i * 8);
    return result;
}

bool validateSize (const std::uint64_t size, std::string& error) noexcept
{
    if (size < headerBytes || size > maxMappingBytes)
    {
        error = "Shared memory size is outside the supported range";
        return false;
    }

    if (size > static_cast<std::uint64_t> (std::numeric_limits<std::size_t>::max()))
    {
        error = "Shared memory size exceeds this process address space";
        return false;
    }

    return true;
}

#if defined(_WIN32)
std::wstring utf8ToWide (const std::string& text)
{
    if (text.empty())
        return {};

    const int length = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (length <= 0)
        return {};

    std::wstring result (static_cast<std::size_t> (length), L'\0');
    if (MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, result.data(), length) <= 0)
        return {};
    result.pop_back();
    return result;
}

std::string windowsError (const char* action)
{
    return std::string (action) + " failed (Windows error " + std::to_string (GetLastError()) + ")";
}
#endif
} // namespace

struct SharedMemoryRegion::Impl
{
#if defined(_WIN32)
    HANDLE handle = nullptr;
#endif
    void* view = nullptr;
    std::uint64_t mappedSize = 0;
    std::string mappingName;

    ~Impl()
    {
#if defined(_WIN32)
        if (view != nullptr)
            UnmapViewOfFile (view);
        if (handle != nullptr)
            CloseHandle (handle);
#endif
    }
};

SharedMemoryRegion::SharedMemoryRegion (std::unique_ptr<Impl> implementation)
    : impl (std::move (implementation))
{
}

SharedMemoryRegion::~SharedMemoryRegion() = default;
SharedMemoryRegion::SharedMemoryRegion (SharedMemoryRegion&&) noexcept = default;
SharedMemoryRegion& SharedMemoryRegion::operator= (SharedMemoryRegion&&) noexcept = default;

std::unique_ptr<SharedMemoryRegion> SharedMemoryRegion::create (const std::string& name,
                                                                const std::uint64_t size,
                                                                std::string& error)
{
    if (! validateSize (size, error))
        return {};

#if defined(_WIN32)
    const auto wideName = utf8ToWide (name);
    if (wideName.empty())
    {
        error = "Shared memory name is not valid UTF-8";
        return {};
    }

    auto implementation = std::make_unique<Impl>();
    implementation->mappingName = name;
    implementation->mappedSize = size;
    implementation->handle = CreateFileMappingW (INVALID_HANDLE_VALUE,
                                                   nullptr,
                                                   PAGE_READWRITE,
                                                   static_cast<DWORD> (size >> 32),
                                                   static_cast<DWORD> (size & 0xffffffffu),
                                                   wideName.c_str());
    if (implementation->handle == nullptr)
    {
        error = windowsError ("CreateFileMappingW");
        return {};
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        error = "Shared memory name already exists";
        return {};
    }

    implementation->view = MapViewOfFile (implementation->handle,
                                           FILE_MAP_ALL_ACCESS,
                                           0,
                                           0,
                                           static_cast<SIZE_T> (size));
    if (implementation->view == nullptr)
    {
        error = windowsError ("MapViewOfFile");
        return {};
    }

    std::memset (implementation->view, 0, static_cast<std::size_t> (headerBytes));
    error.clear();
    return std::unique_ptr<SharedMemoryRegion> (new SharedMemoryRegion (std::move (implementation)));
#else
    (void) name;
    error = "Named shared memory is only supported on Windows";
    return {};
#endif
}

std::unique_ptr<SharedMemoryRegion> SharedMemoryRegion::openReadOnly (const std::string& name,
                                                                      const std::uint64_t size,
                                                                      std::string& error)
{
    if (! validateSize (size, error))
        return {};

#if defined(_WIN32)
    const auto wideName = utf8ToWide (name);
    if (wideName.empty())
    {
        error = "Shared memory name is not valid UTF-8";
        return {};
    }

    auto implementation = std::make_unique<Impl>();
    implementation->mappingName = name;
    implementation->mappedSize = size;
    implementation->handle = OpenFileMappingW (FILE_MAP_READ, FALSE, wideName.c_str());
    if (implementation->handle == nullptr)
    {
        error = windowsError ("OpenFileMappingW");
        return {};
    }

    implementation->view = MapViewOfFile (implementation->handle,
                                           FILE_MAP_READ,
                                           0,
                                           0,
                                           static_cast<SIZE_T> (size));
    if (implementation->view == nullptr)
    {
        error = windowsError ("MapViewOfFile");
        return {};
    }

    error.clear();
    return std::unique_ptr<SharedMemoryRegion> (new SharedMemoryRegion (std::move (implementation)));
#else
    (void) name;
    error = "Named shared memory is only supported on Windows";
    return {};
#endif
}

void* SharedMemoryRegion::data() noexcept                     { return impl != nullptr ? impl->view : nullptr; }
const void* SharedMemoryRegion::data() const noexcept         { return impl != nullptr ? impl->view : nullptr; }
std::uint64_t SharedMemoryRegion::size() const noexcept       { return impl != nullptr ? impl->mappedSize : 0; }
const std::string& SharedMemoryRegion::name() const noexcept
{
    static const std::string empty;
    return impl != nullptr ? impl->mappingName : empty;
}

std::string makeUniqueName (const std::uint64_t requestId, const char* direction)
{
    static std::atomic<std::uint64_t> counter {0};
    std::random_device random;
    const auto timestamp = static_cast<std::uint64_t> (
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto nonce = (static_cast<std::uint64_t> (random()) << 32)
                     ^ static_cast<std::uint64_t> (random())
                     ^ timestamp
                     ^ counter.fetch_add (1, std::memory_order_relaxed);

    std::ostringstream result;
    result << "Local\\PyMSS_";
#if defined(_WIN32)
    result << GetCurrentProcessId();
#else
    result << 0;
#endif
    result << '_' << requestId << '_' << std::hex << std::setfill ('0') << std::setw (16) << nonce
           << '_' << (direction != nullptr ? direction : "buffer");
    return result.str();
}

bool checkedPayloadSize (const std::uint64_t frames,
                         const std::uint32_t channels,
                         std::uint64_t& payloadBytes) noexcept
{
    payloadBytes = 0;
    if (frames == 0 || channels == 0 || channels > maxChannels)
        return false;

    constexpr auto bytesPerSample = static_cast<std::uint64_t> (sizeof (float));
    if (frames > (maxMappingBytes - headerBytes) / bytesPerSample / channels)
        return false;

    payloadBytes = frames * channels * bytesPerSample;
    return true;
}

bool writeHeader (void* mapping,
                  const std::uint64_t mappingSize,
                  const MappingRole role,
                  const std::uint64_t requestId,
                  const std::uint64_t payloadBytes,
                  const std::uint64_t frames,
                  const std::uint32_t channels,
                  const MappingState state,
                  std::string& error) noexcept
{
    if (mapping == nullptr || mappingSize < headerBytes || payloadBytes > mappingSize - headerBytes)
    {
        error = "Shared memory header does not fit the mapping";
        return false;
    }

    std::memset (mapping, 0, static_cast<std::size_t> (headerBytes));
    writeU32 (mapping, magicOffset, mappingMagic);
    writeU32 (mapping, versionOffset, mappingProtocolVersion);
    writeU32 (mapping, headerSizeOffset, static_cast<std::uint32_t> (headerBytes));
    writeU32 (mapping, roleOffset, static_cast<std::uint32_t> (role));
    writeU64 (mapping, requestIdOffset, requestId);
    writeU64 (mapping, payloadBytesOffset, payloadBytes);
    writeU64 (mapping, framesOffset, frames);
    writeU32 (mapping, channelsOffset, channels);
    writeU32 (mapping, stateOffset, static_cast<std::uint32_t> (state));
    std::memcpy (static_cast<std::uint8_t*> (mapping) + identityOffset,
                 mappingIdentity.data(), mappingIdentity.size());
    error.clear();
    return true;
}

bool markReady (void* mapping, const std::uint64_t mappingSize, std::string& error) noexcept
{
    if (mapping == nullptr || mappingSize < headerBytes)
    {
        error = "Shared memory header is unavailable";
        return false;
    }

    std::atomic_thread_fence (std::memory_order_release);
    writeU32 (mapping, stateOffset, static_cast<std::uint32_t> (MappingState::ready));
    error.clear();
    return true;
}

bool readAndValidateHeader (const void* mapping,
                            const std::uint64_t mappingSize,
                            const MappingRole expectedRole,
                            const std::uint64_t expectedRequestId,
                            HeaderInfo& result,
                            std::string& error) noexcept
{
    if (mapping == nullptr || mappingSize < headerBytes || mappingSize > maxMappingBytes)
    {
        error = "Shared memory mapping size is invalid";
        return false;
    }

    if (readU32 (mapping, magicOffset) != mappingMagic
        || readU32 (mapping, versionOffset) != mappingProtocolVersion
        || readU32 (mapping, headerSizeOffset) != headerBytes
        || std::memcmp (static_cast<const std::uint8_t*> (mapping) + identityOffset,
                        mappingIdentity.data(), mappingIdentity.size()) != 0)
    {
        error = "Shared memory protocol header is invalid";
        return false;
    }

    result.role = static_cast<MappingRole> (readU32 (mapping, roleOffset));
    result.requestId = readU64 (mapping, requestIdOffset);
    result.payloadBytes = readU64 (mapping, payloadBytesOffset);
    result.frames = readU64 (mapping, framesOffset);
    result.channels = readU32 (mapping, channelsOffset);
    result.state = static_cast<MappingState> (readU32 (mapping, stateOffset));

    if (result.role != expectedRole || result.requestId != expectedRequestId)
    {
        error = "Shared memory mapping does not match the request";
        return false;
    }
    if (result.state != MappingState::ready)
    {
        error = "Shared memory mapping is not ready";
        return false;
    }
    if (result.payloadBytes > mappingSize - headerBytes)
    {
        error = "Shared memory payload exceeds the mapping";
        return false;
    }

    error.clear();
    return true;
}
} // namespace pymss_shm
