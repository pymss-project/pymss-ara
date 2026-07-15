#pragma once

#include <juce_core/juce_core.h>
#include <cstring>

/** Binary framing helpers shared between the C++ WorkerClient and the Python
    worker (see python/worker.py for the matching implementation).

    Frame layout (little-endian):
        uint32  headerLen   length of the JSON header in bytes
        uint32  bodyLen     length of the binary body in bytes
        bytes   headerLen   UTF-8 JSON header (single-line)
        bytes   bodyLen     raw binary payload

    Every header carries an integer "id" used to match responses to requests and
    to attribute progress events.
*/

namespace pymss_protocol
{
using Header = juce::DynamicObject;     // a JSON object
using HeaderPtr = juce::ReferenceCountedObjectPtr<Header>;

inline HeaderPtr makeHeader()
{
    return new Header();
}

/** Serialize a header object to a single-line UTF-8 JSON MemoryBlock. */
inline juce::MemoryBlock headerToBlock (const Header& header)
{
    const auto jsonStr = juce::JSON::toString (juce::var (&const_cast<Header&> (header)), true);
    return { jsonStr.toRawUTF8(), jsonStr.getNumBytesAsUTF8() };
}

/** Build a complete frame (8-byte prefix + header + body). */
inline juce::MemoryBlock buildFrame (const Header& header, const void* bodyData, juce::uint32 bodySize)
{
    const auto headerBlock = headerToBlock (header);
    const juce::uint32 headerLen = (juce::uint32) headerBlock.getSize();

    juce::MemoryBlock frame;
    frame.setSize ((size_t) 8 + headerLen + bodySize);
    auto* base = (juce::uint8*) frame.getData();

    auto writeU32LE = [] (void* dst, juce::uint32 v)
    {
        auto* p = static_cast<juce::uint8*> (dst);
        p[0] = (juce::uint8) (v & 0xFF);
        p[1] = (juce::uint8) ((v >> 8) & 0xFF);
        p[2] = (juce::uint8) ((v >> 16) & 0xFF);
        p[3] = (juce::uint8) ((v >> 24) & 0xFF);
    };

    writeU32LE (base, headerLen);
    writeU32LE (base + 4, bodySize);
    std::memcpy (base + 8, headerBlock.getData(), headerLen);
    if (bodySize > 0)
        std::memcpy (base + 8 + headerLen, bodyData, bodySize);
    return frame;
}

inline juce::MemoryBlock buildFrame (const Header& header)
{
    return buildFrame (header, nullptr, 0);
}

/** Parse a single-line JSON header string into a var. */
inline juce::var parseHeader (const void* data, juce::uint32 headerLen)
{
    juce::String s (static_cast<const char*> (data), (size_t) headerLen);
    return juce::JSON::parse (s);
}
} // namespace pymss_protocol
