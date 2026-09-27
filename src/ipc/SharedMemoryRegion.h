#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pymss_shm
{
constexpr std::uint32_t controlProtocolVersion = 4;
constexpr std::uint32_t mappingMagic = 0x534d5950; // "PYMS" in little-endian byte order.
constexpr std::uint32_t mappingProtocolVersion = 2;
inline constexpr std::array<std::uint8_t, 16> mappingIdentity {
    'P', 'Y', 'M', 'S', 'S', ':', ':', 'S', 'H', 'M', ':', ':', 'V', '2', 0, 0
};
constexpr std::uint64_t headerBytes = 64;
constexpr std::uint64_t maxMappingBytes = 8ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t maxResultPayloadBytes = 2ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint32_t maxChannels = 64;
constexpr std::uint32_t maxStems = 64;

enum class MappingRole : std::uint32_t
{
    input = 1,
    output = 2
};

enum class MappingState : std::uint32_t
{
    writing = 1,
    ready = 2
};

struct HeaderInfo
{
    MappingRole role = MappingRole::input;
    std::uint64_t requestId = 0;
    std::uint64_t payloadBytes = 0;
    std::uint64_t frames = 0;
    std::uint32_t channels = 0;
    MappingState state = MappingState::writing;
};

class SharedMemoryRegion
{
public:
    ~SharedMemoryRegion();

    SharedMemoryRegion (SharedMemoryRegion&&) noexcept;
    SharedMemoryRegion& operator= (SharedMemoryRegion&&) noexcept;

    static std::unique_ptr<SharedMemoryRegion> create (const std::string& name,
                                                       std::uint64_t size,
                                                       std::string& error);
    static std::unique_ptr<SharedMemoryRegion> openReadOnly (const std::string& name,
                                                             std::uint64_t size,
                                                             std::string& error);

    void* data() noexcept;
    const void* data() const noexcept;
    std::uint64_t size() const noexcept;
    const std::string& name() const noexcept;

private:
    struct Impl;
    explicit SharedMemoryRegion (std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> impl;

    SharedMemoryRegion (const SharedMemoryRegion&) = delete;
    SharedMemoryRegion& operator= (const SharedMemoryRegion&) = delete;
};

std::string makeUniqueName (std::uint64_t requestId, const char* direction);

bool checkedPayloadSize (std::uint64_t frames,
                         std::uint32_t channels,
                         std::uint64_t& payloadBytes) noexcept;

bool writeHeader (void* mapping,
                  std::uint64_t mappingSize,
                  MappingRole role,
                  std::uint64_t requestId,
                  std::uint64_t payloadBytes,
                  std::uint64_t frames,
                  std::uint32_t channels,
                  MappingState state,
                  std::string& error) noexcept;

bool markReady (void* mapping, std::uint64_t mappingSize, std::string& error) noexcept;

bool readAndValidateHeader (const void* mapping,
                            std::uint64_t mappingSize,
                            MappingRole expectedRole,
                            std::uint64_t expectedRequestId,
                            HeaderInfo& result,
                            std::string& error) noexcept;
} // namespace pymss_shm
