#include "Receiver.hpp"
#include "FrameLayout.hpp"
#include "Packet.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace vfdt
{

Receiver::Receiver(int bitsPerCell)
    : reader{bitsPerCell}
    , decoder{}
    , stats{}
{
}

ReadCode Receiver::AcceptImage(const Image& image)
{
    ++stats.images;
    FrameRead frame{};
    const ReadCode code = reader.ReadFrame(image, frame);
    switch (code)
    {
    case ReadCode::Ok:
        ++stats.framesRead;
        break;
    case ReadCode::FindersNotFound:
        ++stats.findersNotFound;
        return code;
    case ReadCode::UnsupportedGrid:
        ++stats.unsupportedGrid;
        return code;
    case ReadCode::LowContrast:
        ++stats.lowContrast;
        return code;
    case ReadCode::BlendedFrame:
        ++stats.blended;
        return code;
    case ReadCode::TornFrame:
        ++stats.torn;
        return code;
    }
    stats.correctedBytes += static_cast<std::uint64_t>(frame.correctedBytes);
    stats.failedCodewords += static_cast<std::uint64_t>(frame.failedCodewords);

    const std::span<const std::uint8_t> payload = frame.payload;
    for (int slot = 0; slot < frame.packetsPerFrame; ++slot)
    {
        const std::size_t begin = static_cast<std::size_t>(slot) * frame.packetBytes;
        const std::size_t end = begin + frame.packetBytes;

        // Skip packets that touch a codeword Reed-Solomon could not correct.
        bool intact = true;
        for (std::size_t c = begin / rs_data_bytes; c <= (end - 1) / rs_data_bytes; ++c)
        {
            intact = intact && frame.codewordOk[c];
        }
        Packet packet{};
        if (!intact || ParsePacket(payload.subspan(begin, frame.packetBytes), packet) != PacketCode::Ok)
        {
            ++stats.packetsDamaged;
            continue;
        }
        if (decoder.AddPacket(packet) == AddCode::Ok)
        {
            ++stats.packetsAccepted;
        }
        else
        {
            ++stats.packetsRedundant;
        }
    }
    return ReadCode::Ok;
}

const ReceiverStats& Receiver::Stats() const
{
    return stats;
}

const FountainDecoder& Receiver::Decoder() const
{
    return decoder;
}

ReceiveCode Receiver::Unpack(UnpackedContent& out) const
{
    if (!decoder.IsComplete())
    {
        return ReceiveCode::Incomplete;
    }
    switch (UnpackContent(decoder.Content(), out))
    {
    case ManifestCode::Ok:
        return ReceiveCode::Ok;
    case ManifestCode::SizeMismatch:
        return ReceiveCode::SizeMismatch;
    case ManifestCode::HashMismatch:
        return ReceiveCode::HashMismatch;
    case ManifestCode::BadMagic:
    case ManifestCode::Truncated:
        break;
    }
    return ReceiveCode::BadManifest;
}

} // namespace vfdt
