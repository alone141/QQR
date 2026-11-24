#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace qqr
{

constexpr int rs_max_codeword_bytes = 255;

enum class RsCode
{
    Ok,
    BadLength,
    TooManyErrors,
};

class ReedSolomon
{
public:
    /**
     * @brief Build a GF(256) Reed-Solomon code with @p parityBytes parity bytes per codeword.
     * @param parityBytes Parity bytes per codeword. Corrects up to half this many byte errors.
     */
    explicit ReedSolomon(int parityBytes);

    /**
     * @brief Return the parity length this code was built with.
     * @return Parity bytes per codeword.
     */
    int ParityBytes() const;

    /**
     * @brief Compute the parity bytes for @p data.
     * @param data Message bytes. data.size() + ParityBytes() must not exceed 255.
     * @param parity Receives exactly ParityBytes() bytes.
     * @return RsCode::Ok on success, RsCode::BadLength when the sizes do not fit.
     */
    RsCode Encode(std::span<const std::uint8_t> data, std::span<std::uint8_t> parity) const;

    /**
     * @brief Correct a codeword (message followed by parity) in place.
     * @param codeword Bytes to correct. Left unchanged when correction fails.
     * @param correctedBytes Receives the number of bytes that were corrected.
     * @return RsCode::Ok when the codeword is now valid, otherwise the failure.
     */
    RsCode Decode(std::span<std::uint8_t> codeword, int& correctedBytes) const;

private:
    int parityBytes;
    std::vector<std::uint8_t> generator;
};

} // namespace qqr
