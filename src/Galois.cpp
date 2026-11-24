#include "Galois.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qqr
{

namespace
{

constexpr unsigned primitive_polynomial = 0x11DU;

struct GaloisTables
{
    std::array<std::uint8_t, 512> exp;
    std::array<int, 256> log;
};

GaloisTables MakeGaloisTables()
{
    GaloisTables tables{};
    unsigned value = 1;
    for (int i = 0; i < 255; ++i)
    {
        tables.exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value);
        tables.log[value] = i;
        value <<= 1;
        if ((value & 0x100U) != 0)
        {
            value ^= primitive_polynomial;
        }
    }
    for (std::size_t i = 255; i < tables.exp.size(); ++i)
    {
        tables.exp[i] = tables.exp[i - 255];
    }
    return tables;
}

const GaloisTables galois = MakeGaloisTables();

std::array<std::uint8_t, 256> MultiplyRow(std::uint8_t factor)
{
    std::array<std::uint8_t, 256> row{};
    for (std::size_t i = 1; i < row.size(); ++i)
    {
        row[i] = GfMul(static_cast<std::uint8_t>(i), factor);
    }
    return row;
}

} // namespace

std::uint8_t GfMul(std::uint8_t a, std::uint8_t b)
{
    if (a == 0 || b == 0)
    {
        return 0;
    }
    return galois.exp[static_cast<std::size_t>(galois.log[a] + galois.log[b])];
}

std::uint8_t GfDiv(std::uint8_t a, std::uint8_t b)
{
    if (a == 0)
    {
        return 0;
    }
    return galois.exp[static_cast<std::size_t>((galois.log[a] + 255 - galois.log[b]) % 255)];
}

std::uint8_t GfInverse(std::uint8_t a)
{
    return galois.exp[static_cast<std::size_t>(255 - galois.log[a])];
}

std::uint8_t GfPow2(int power)
{
    int reduced = power % 255;
    if (reduced < 0)
    {
        reduced += 255;
    }
    return galois.exp[static_cast<std::size_t>(reduced)];
}

void GfMulAdd(std::span<std::uint8_t> target, std::span<const std::uint8_t> source, std::uint8_t factor)
{
    if (factor == 0)
    {
        return;
    }
    if (factor == 1)
    {
        for (std::size_t i = 0; i < target.size(); ++i)
        {
            target[i] ^= source[i];
        }
        return;
    }
    const std::array<std::uint8_t, 256> row = MultiplyRow(factor);
    for (std::size_t i = 0; i < target.size(); ++i)
    {
        target[i] ^= row[source[i]];
    }
}

void GfScale(std::span<std::uint8_t> target, std::uint8_t factor)
{
    if (factor == 1)
    {
        return;
    }
    const std::array<std::uint8_t, 256> row = MultiplyRow(factor);
    for (std::uint8_t& value : target)
    {
        value = row[value];
    }
}

} // namespace qqr
