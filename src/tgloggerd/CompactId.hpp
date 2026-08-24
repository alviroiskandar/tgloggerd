// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_COMPACT_ID_HPP
#define TGLOGGERD_COMPACT_ID_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>

/*
 * Reversible, compact text encoding of a 64-bit id.
 *
 * encode(v) is base64 of v's minimal big-endian byte string with the '='
 * padding removed; decode() is its exact inverse, so an id embedded in a
 * human-readable label can always be recovered.
 *
 * "Minimal" means leading zero bytes are dropped, so the length is
 * floor(log256(v)) + 1 -- the fewest bytes that can hold v. That count is
 * identical whether the bytes are read big- or little-endian (little-endian
 * would instead drop the *trailing* zero bytes for the same length), so the
 * choice of endianness does not affect storage size at all. Big-endian is used
 * because it is the canonical network byte order: the encoded form sorts the
 * same way the integers do, and decoding is a plain shift-and-or with no byte
 * reversal.
 *
 * v == 0 encodes as a single zero byte ("AA"), never the empty string, so
 * every id has a non-empty, unambiguous form.
 *
 * Standard base64 alphabet (A-Za-z0-9+/). Padding is omitted, which is
 * unambiguous: the byte count is recoverable from the character count, and a
 * length that is 1 (mod 4) -- impossible for real base64 -- is rejected.
 */
namespace tgloggerd::compactid {

inline constexpr char kAlphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string encode(uint64_t v)
{
	/* Minimal big-endian bytes: how many, then fill most-significant first. */
	unsigned char be[8];
	int n;
	if (v == 0) {
		be[0] = 0;
		n = 1;
	} else {
		n = 0;
		for (uint64_t t = v; t; t >>= 8)
			n++;
		for (int i = 0; i < n; i++)
			be[n - 1 - i] = (unsigned char)((v >> (8 * i)) & 0xFF);
	}

	std::string out;
	out.reserve((size_t)((n * 4 + 2) / 3));

	int i = 0;
	for (; i + 3 <= n; i += 3) {
		uint32_t b = ((uint32_t)be[i] << 16) |
			     ((uint32_t)be[i + 1] << 8) | be[i + 2];
		out += kAlphabet[(b >> 18) & 0x3F];
		out += kAlphabet[(b >> 12) & 0x3F];
		out += kAlphabet[(b >> 6) & 0x3F];
		out += kAlphabet[b & 0x3F];
	}
	if (n - i == 1) {
		uint32_t b = (uint32_t)be[i] << 16;
		out += kAlphabet[(b >> 18) & 0x3F];
		out += kAlphabet[(b >> 12) & 0x3F];
	} else if (n - i == 2) {
		uint32_t b = ((uint32_t)be[i] << 16) | ((uint32_t)be[i + 1] << 8);
		out += kAlphabet[(b >> 18) & 0x3F];
		out += kAlphabet[(b >> 12) & 0x3F];
		out += kAlphabet[(b >> 6) & 0x3F];
	}
	return out;
}

inline std::optional<uint64_t> decode(std::string_view s)
{
	auto sextet = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1;
	};

	/* Padding-free base64 never has a length of 1 (mod 4). */
	if (s.empty() || s.size() % 4 == 1)
		return std::nullopt;

	unsigned char bytes[8];
	int nb = 0;
	uint32_t acc = 0;
	int bits = 0;
	for (char c : s) {
		int d = sextet(c);
		if (d < 0)
			return std::nullopt;
		acc = (acc << 6) | (uint32_t)d;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (nb >= 8)
				return std::nullopt; /* > 8 bytes: overflows u64 */
			bytes[nb++] = (unsigned char)((acc >> bits) & 0xFF);
		}
	}
	/* Any bits left over must be zero -- reject non-canonical encodings. */
	if (bits > 0 && (acc & ((1u << bits) - 1)) != 0)
		return std::nullopt;
	if (nb == 0)
		return std::nullopt;

	uint64_t v = 0;
	for (int i = 0; i < nb; i++)
		v = (v << 8) | bytes[i];
	return v;
}

} /* namespace tgloggerd::compactid */

#endif /* TGLOGGERD_COMPACT_ID_HPP */
