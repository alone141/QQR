#include "ReedSolomon.hpp"
#include "Galois.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace qqr
{

namespace
{

using Poly = std::vector<std::uint8_t>;

std::uint8_t Mul(std::uint8_t a, std::uint8_t b)
{
    return GfMul(a, b);
}

std::uint8_t Div(std::uint8_t a, std::uint8_t b)
{
    return GfDiv(a, b);
}

std::uint8_t Pow2(int power)
{
    return GfPow2(power);
}

std::uint8_t Inverse(std::uint8_t a)
{
    return GfInverse(a);
}

Poly PolyScale(const Poly& poly, std::uint8_t factor)
{
    Poly result(poly.size());
    for (std::size_t i = 0; i < poly.size(); ++i)
    {
        result[i] = Mul(poly[i], factor);
    }
    return result;
}

Poly PolyAdd(const Poly& p, const Poly& q)
{
    const std::size_t size = std::max(p.size(), q.size());
    Poly result(size, 0);
    for (std::size_t i = 0; i < p.size(); ++i)
    {
        result[i + size - p.size()] = p[i];
    }
    for (std::size_t i = 0; i < q.size(); ++i)
    {
        result[i + size - q.size()] ^= q[i];
    }
    return result;
}

Poly PolyMul(const Poly& p, const Poly& q)
{
    Poly result(p.size() + q.size() - 1, 0);
    for (std::size_t j = 0; j < q.size(); ++j)
    {
        for (std::size_t i = 0; i < p.size(); ++i)
        {
            result[i + j] ^= Mul(p[i], q[j]);
        }
    }
    return result;
}

std::uint8_t PolyEval(std::span<const std::uint8_t> poly, std::uint8_t x)
{
    std::uint8_t y = poly[0];
    for (std::size_t i = 1; i < poly.size(); ++i)
    {
        y = static_cast<std::uint8_t>(Mul(y, x) ^ poly[i]);
    }
    return y;
}

bool ComputeSyndromes(std::span<const std::uint8_t> codeword, int parityBytes, Poly& syndromes)
{
    syndromes.assign(static_cast<std::size_t>(parityBytes) + 1, 0);
    bool clean = true;
    for (int i = 0; i < parityBytes; ++i)
    {
        const std::uint8_t value = PolyEval(codeword, Pow2(i));
        syndromes[static_cast<std::size_t>(i) + 1] = value;
        clean = clean && value == 0;
    }
    return clean;
}

} // namespace

ReedSolomon::ReedSolomon(int parityBytes)
    : parityBytes{parityBytes}
    , generator{1}
{
    for (int i = 0; i < parityBytes; ++i)
    {
        generator = PolyMul(generator, Poly{1, Pow2(i)});
    }
}

int ReedSolomon::ParityBytes() const
{
    return parityBytes;
}

RsCode ReedSolomon::Encode(std::span<const std::uint8_t> data, std::span<std::uint8_t> parity) const
{
    if (data.size() + static_cast<std::size_t>(parityBytes) > rs_max_codeword_bytes ||
        parity.size() != static_cast<std::size_t>(parityBytes))
    {
        return RsCode::BadLength;
    }

    Poly work(data.size() + parity.size(), 0);
    std::copy(data.begin(), data.end(), work.begin());
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        const std::uint8_t coefficient = work[i];
        if (coefficient == 0)
        {
            continue;
        }
        for (std::size_t j = 1; j < generator.size(); ++j)
        {
            work[i + j] ^= Mul(generator[j], coefficient);
        }
    }
    std::copy(work.begin() + static_cast<std::ptrdiff_t>(data.size()), work.end(), parity.begin());
    return RsCode::Ok;
}

RsCode ReedSolomon::Decode(std::span<std::uint8_t> codeword, int& correctedBytes) const
{
    correctedBytes = 0;
    const std::size_t length = codeword.size();
    if (length > rs_max_codeword_bytes || length <= static_cast<std::size_t>(parityBytes))
    {
        return RsCode::BadLength;
    }

    Poly syndromes;
    if (ComputeSyndromes(codeword, parityBytes, syndromes))
    {
        return RsCode::Ok;
    }

    // Berlekamp-Massey: find the error locator polynomial.
    Poly errorLocator{1};
    Poly oldLocator{1};
    for (int i = 0; i < parityBytes; ++i)
    {
        const std::size_t k = static_cast<std::size_t>(i) + 1;
        std::uint8_t delta = syndromes[k];
        for (std::size_t j = 1; j < errorLocator.size(); ++j)
        {
            delta ^= Mul(errorLocator[errorLocator.size() - 1 - j], syndromes[k - j]);
        }
        oldLocator.push_back(0);
        if (delta != 0)
        {
            if (oldLocator.size() > errorLocator.size())
            {
                Poly newLocator = PolyScale(oldLocator, delta);
                oldLocator = PolyScale(errorLocator, Inverse(delta));
                errorLocator = newLocator;
            }
            errorLocator = PolyAdd(errorLocator, PolyScale(oldLocator, delta));
        }
    }
    std::size_t leading = 0;
    while (leading < errorLocator.size() && errorLocator[leading] == 0)
    {
        ++leading;
    }
    errorLocator.erase(errorLocator.begin(), errorLocator.begin() + static_cast<std::ptrdiff_t>(leading));
    if (errorLocator.empty())
    {
        return RsCode::TooManyErrors;
    }
    const std::size_t errorCount = errorLocator.size() - 1;
    if (errorCount * 2 > static_cast<std::size_t>(parityBytes))
    {
        return RsCode::TooManyErrors;
    }

    // Chien search: find the error positions.
    const Poly reversedLocator(errorLocator.rbegin(), errorLocator.rend());
    std::vector<std::size_t> errorPositions;
    for (std::size_t i = 0; i < length; ++i)
    {
        if (PolyEval(reversedLocator, Pow2(static_cast<int>(i))) == 0)
        {
            errorPositions.push_back(length - 1 - i);
        }
    }
    if (errorPositions.size() != errorCount)
    {
        return RsCode::TooManyErrors;
    }

    // Forney: compute the error magnitudes.
    std::vector<int> coefficientPositions;
    Poly errataLocator{1};
    for (const std::size_t position : errorPositions)
    {
        const int coefficientPosition = static_cast<int>(length - 1 - position);
        coefficientPositions.push_back(coefficientPosition);
        errataLocator = PolyMul(errataLocator, PolyAdd(Poly{1}, Poly{Pow2(coefficientPosition), 0}));
    }
    const Poly reversedSyndromes(syndromes.rbegin(), syndromes.rend());
    const Poly product = PolyMul(reversedSyndromes, errataLocator);
    const std::size_t evaluatorLength = errataLocator.size();
    const Poly errorEvaluator(product.end() - static_cast<std::ptrdiff_t>(evaluatorLength), product.end());

    std::vector<std::uint8_t> locations;
    for (const int coefficientPosition : coefficientPositions)
    {
        locations.push_back(Pow2(coefficientPosition));
    }

    std::vector<std::uint8_t> original;
    for (const std::size_t position : errorPositions)
    {
        original.push_back(codeword[position]);
    }

    for (std::size_t i = 0; i < locations.size(); ++i)
    {
        const std::uint8_t location = locations[i];
        const std::uint8_t locationInverse = Inverse(location);
        std::uint8_t locatorPrime = 1;
        for (std::size_t j = 0; j < locations.size(); ++j)
        {
            if (j != i)
            {
                locatorPrime = Mul(locatorPrime, static_cast<std::uint8_t>(1 ^ Mul(locationInverse, locations[j])));
            }
        }
        if (locatorPrime == 0)
        {
            for (std::size_t r = 0; r < errorPositions.size(); ++r)
            {
                codeword[errorPositions[r]] = original[r];
            }
            return RsCode::TooManyErrors;
        }
        const std::uint8_t y = Mul(location, PolyEval(errorEvaluator, locationInverse));
        codeword[errorPositions[i]] ^= Div(y, locatorPrime);
    }

    if (!ComputeSyndromes(codeword, parityBytes, syndromes))
    {
        for (std::size_t r = 0; r < errorPositions.size(); ++r)
        {
            codeword[errorPositions[r]] = original[r];
        }
        return RsCode::TooManyErrors;
    }

    correctedBytes = static_cast<int>(errorCount);
    return RsCode::Ok;
}

} // namespace qqr
