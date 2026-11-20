#include "Receiver.hpp"
#include "FrameLayout.hpp"
#include "Packet.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <span>
#include <vector>

namespace vfdt
{

Receiver::Receiver(int bitsPerCell, int threads)
    : readers(static_cast<std::size_t>(std::max(1, threads)), FrameReader{bitsPerCell})
    , decoder{}
    , stats{}
{
}

ReadCode Receiver::AcceptImage(const Image& image)
{
    FrameRead frame{};
    const ReadCode code = readers.front().ReadFrame(image, frame);
    AcceptFrame(code, frame);
    return code;
}

void Receiver::AcceptImages(std::span<const Image> images)
{
    std::vector<FrameRead> frames(readers.size());
    std::vector<std::future<ReadCode>> results;
    for (std::size_t start = 0; start < images.size(); start += readers.size())
    {
        const std::size_t count = std::min(readers.size(), images.size() - start);
        results.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            results.push_back(std::async(std::launch::async, [this, &images, &frames, start, i]()
            {
                return readers[i].ReadFrame(images[start + i], frames[i]);
            }));
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            AcceptFrame(results[i].get(), frames[i]);
        }
    }
}

void Receiver::AcceptFrame(ReadCode code, const FrameRead& frame)
{
    ++stats.images;
    switch (code)
    {
    case ReadCode::Ok:
        ++stats.framesRead;
        break;
    case ReadCode::FindersNotFound:
        ++stats.findersNotFound;
        return;
    case ReadCode::UnsupportedGrid:
        ++stats.unsupportedGrid;
        return;
    case ReadCode::LowContrast:
        ++stats.lowContrast;
        return;
    case ReadCode::BlendedFrame:
        ++stats.blended;
        return;
    case ReadCode::TornFrame:
        ++stats.torn;
        return;
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
