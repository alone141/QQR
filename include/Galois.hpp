#pragma once

#include <cstdint>
#include <span>

namespace qqr
{

/**
 * @brief Multiply in GF(256) with the polynomial 0x11D.
 * @param a First factor.
 * @param b Second factor.
 * @return The product.
 */
std::uint8_t GfMul(std::uint8_t a, std::uint8_t b);

/**
 * @brief Divide in GF(256).
 * @param a Dividend.
 * @param b Divisor. Must not be zero.
 * @return a / b.
 */
std::uint8_t GfDiv(std::uint8_t a, std::uint8_t b);

/**
 * @brief Invert in GF(256).
 * @param a Value to invert. Must not be zero.
 * @return 1 / a.
 */
std::uint8_t GfInverse(std::uint8_t a);

/**
 * @brief Raise the generator 2 to a power in GF(256).
 * @param power Exponent; may be negative.
 * @return 2 to the power @p power.
 */
std::uint8_t GfPow2(int power);

/**
 * @brief Add a multiple of one byte row to another: target[i] ^= factor * source[i].
 * @param target Row to change.
 * @param source Row to add; at least as long as @p target.
 * @param factor Multiplier.
 */
void GfMulAdd(std::span<std::uint8_t> target, std::span<const std::uint8_t> source, std::uint8_t factor);

/**
 * @brief Multiply every byte of a row in place.
 * @param target Row to scale.
 * @param factor Multiplier.
 */
void GfScale(std::span<std::uint8_t> target, std::uint8_t factor);

} // namespace qqr
