#pragma once

#include "FountainDecoder.hpp"
#include "FrameReader.hpp"
#include "Image.hpp"
#include "Manifest.hpp"

#include <cstdint>

namespace vfdt
{

struct ReceiverStats
{
    std::uint64_t images{};
    std::uint64_t framesRead{};
    std::uint64_t findersNotFound{};
    std::uint64_t unsupportedGrid{};
    std::uint64_t lowContrast{};
    std::uint64_t blended{};
    std::uint64_t torn{};
    std::uint64_t correctedBytes{};
    std::uint64_t failedCodewords{};
    std::uint64_t packetsAccepted{};
    std::uint64_t packetsDamaged{};
    std::uint64_t packetsRedundant{};
};

enum class ReceiveCode
{
    Ok,
    Incomplete,
    BadManifest,
    SizeMismatch,
    HashMismatch,
};

class Receiver
{
public:
    /**
     * @brief Prepare to receive frames drawn with @p bitsPerCell bits per data cell.
     * @param bitsPerCell 1 or 2; must match the sender.
     */
    explicit Receiver(int bitsPerCell);

    /**
     * @brief Read one camera image and feed its packets to the decoder.
     * @param image Camera image.
     * @return ReadCode::Ok when the frame was read, otherwise why it was skipped.
     */
    ReadCode AcceptImage(const Image& image);

    /**
     * @brief Return counters for everything received so far.
     * @return The counters.
     */
    const ReceiverStats& Stats() const;

    /**
     * @brief Return the fountain decoder, for progress reporting.
     * @return The decoder.
     */
    const FountainDecoder& Decoder() const;

    /**
     * @brief Verify and split the recovered content.
     * @param out Receives the manifest and a view of the file bytes, valid while the receiver lives.
     * @return ReceiveCode::Ok when the file is complete and its SHA-256 matches, otherwise the failure.
     */
    ReceiveCode Unpack(UnpackedContent& out) const;

private:
    FrameReader reader;
    FountainDecoder decoder;
    ReceiverStats stats;
};

} // namespace vfdt
