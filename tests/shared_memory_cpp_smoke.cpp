#include "../src/ipc/SharedMemoryRegion.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
constexpr std::uint64_t frames = 5;
constexpr std::uint32_t channels = 2;
constexpr std::uint64_t inputRequestId = 7001;
constexpr std::uint64_t outputRequestId = 7002;
constexpr std::array<float, frames * channels> inputSamples {
    0.0f, 0.25f, 0.5f, 0.75f, 1.0f,
    -1.0f, -0.75f, -0.5f, -0.25f, 0.0f
};

std::unique_ptr<pymss_shm::SharedMemoryRegion> createInput (std::string& error)
{
    std::uint64_t payloadBytes = 0;
    if (! pymss_shm::checkedPayloadSize (frames, channels, payloadBytes))
        return {};

    const auto name = pymss_shm::makeUniqueName (inputRequestId, "native");
    auto owner = pymss_shm::SharedMemoryRegion::create (
        name, pymss_shm::headerBytes + payloadBytes, error);
    if (owner == nullptr)
        return {};

    if (! pymss_shm::writeHeader (owner->data(), owner->size(),
                                  pymss_shm::MappingRole::input,
                                  inputRequestId,
                                  payloadBytes,
                                  frames,
                                  channels,
                                  pymss_shm::MappingState::writing,
                                  error))
        return {};

    auto* payload = static_cast<std::uint8_t*> (owner->data()) + pymss_shm::headerBytes;
    std::memcpy (payload, inputSamples.data(), static_cast<std::size_t> (payloadBytes));
    if (! pymss_shm::markReady (owner->data(), owner->size(), error))
        return {};
    return owner;
}

int runSelfTest()
{
    std::string error;
    auto owner = createInput (error);
    if (owner == nullptr)
        return 1;

    auto reader = pymss_shm::SharedMemoryRegion::openReadOnly (owner->name(), owner->size(), error);
    if (reader == nullptr)
        return 2;

    pymss_shm::HeaderInfo header;
    if (! pymss_shm::readAndValidateHeader (reader->data(), reader->size(),
                                            pymss_shm::MappingRole::input,
                                            inputRequestId,
                                            header,
                                            error))
        return 3;

    const auto* payload = static_cast<const std::uint8_t*> (reader->data()) + pymss_shm::headerBytes;
    if (header.frames != frames || header.channels != channels
        || std::memcmp (payload, inputSamples.data(),
                        static_cast<std::size_t> (header.payloadBytes)) != 0)
        return 4;
    return 0;
}

int produceInput()
{
    std::string error;
    auto owner = createInput (error);
    if (owner == nullptr)
        return 10;

    std::cout << owner->name() << '\n' << owner->size() << std::endl;
    std::string release;
    if (! std::getline (std::cin, release))
        return 11;
    return 0;
}

int consumeOutput (const char* nameText, const char* mappingBytesText,
                   const char* payloadBytesText)
{
    const auto mappingBytes = std::stoull (mappingBytesText);
    const auto payloadBytes = std::stoull (payloadBytesText);
    std::string error;
    auto reader = pymss_shm::SharedMemoryRegion::openReadOnly (nameText, mappingBytes, error);
    if (reader == nullptr)
        return 20;

    pymss_shm::HeaderInfo header;
    if (! pymss_shm::readAndValidateHeader (reader->data(), reader->size(),
                                            pymss_shm::MappingRole::output,
                                            outputRequestId,
                                            header,
                                            error))
        return 21;
    if (header.payloadBytes != payloadBytes || header.frames != 0 || header.channels != 0)
        return 22;

    constexpr std::array<float, 6> expected {
        1.0f, 2.0f, 3.0f,
        10.0f, 20.0f, 30.0f
    };
    const auto* payload = static_cast<const std::uint8_t*> (reader->data()) + pymss_shm::headerBytes;
    if (payloadBytes < expected.size() * sizeof (float)
        || std::memcmp (payload, expected.data(), expected.size() * sizeof (float)) != 0)
        return 23;
    return 0;
}
} // namespace

int main (const int argc, char* argv[])
{
    try
    {
        if (argc == 1)
            return runSelfTest();
        if (argc == 2 && std::string (argv[1]) == "produce-input")
            return produceInput();
        if (argc == 5 && std::string (argv[1]) == "consume-output")
            return consumeOutput (argv[2], argv[3], argv[4]);
    }
    catch (...)
    {
        return 30;
    }

    return 31;
}
