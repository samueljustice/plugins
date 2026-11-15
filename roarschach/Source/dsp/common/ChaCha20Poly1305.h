#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace roar::crypto
{
    /**
     * ChaCha20 and Poly1305, and their AEAD construction, written from RFC 8439 (sections 2.3,
     * 2.4, 2.5 and 2.8) and checked against its test vectors (RoarschachTests "chamber").
     * Public domain style, dependency free, portable (32-bit limbs, 64-bit products).
     *
     * Used to keep CHAMBER's bundled room data out of plain sight in the binary: it is decrypted
     * in memory when CHAMBER loads, never written anywhere.
     */
    using Key = std::array<std::uint8_t, 32>;
    using Nonce = std::array<std::uint8_t, 12>;
    using Tag = std::array<std::uint8_t, 16>;

    namespace detail
    {
        inline std::uint32_t load32 (const std::uint8_t* p)
        {
            return static_cast<std::uint32_t> (p[0]) | (static_cast<std::uint32_t> (p[1]) << 8)
                 | (static_cast<std::uint32_t> (p[2]) << 16) | (static_cast<std::uint32_t> (p[3]) << 24);
        }

        inline void store32 (std::uint8_t* p, std::uint32_t v)
        {
            p[0] = static_cast<std::uint8_t> (v);
            p[1] = static_cast<std::uint8_t> (v >> 8);
            p[2] = static_cast<std::uint8_t> (v >> 16);
            p[3] = static_cast<std::uint8_t> (v >> 24);
        }

        inline std::uint32_t rotl (std::uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

        inline void quarterRound (std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d)
        {
            a += b; d ^= a; d = rotl (d, 16);
            c += d; b ^= c; b = rotl (b, 12);
            a += b; d ^= a; d = rotl (d, 8);
            c += d; b ^= c; b = rotl (b, 7);
        }
    }

    /** RFC 8439 2.3: one 64-byte key-stream block. */
    inline void chacha20Block (const Key& key, std::uint32_t counter, const Nonce& nonce, std::uint8_t out[64])
    {
        std::uint32_t s[16];
        s[0] = 0x61707865u;
        s[1] = 0x3320646eu;
        s[2] = 0x79622d32u;
        s[3] = 0x6b206574u;
        for (int i = 0; i < 8; ++i)
            s[4 + i] = detail::load32 (key.data() + 4 * i);
        s[12] = counter;
        for (int i = 0; i < 3; ++i)
            s[13 + i] = detail::load32 (nonce.data() + 4 * i);

        std::uint32_t w[16];
        std::memcpy (w, s, sizeof (w));
        for (int round = 0; round < 10; ++round)
        {
            detail::quarterRound (w[0], w[4], w[8], w[12]);
            detail::quarterRound (w[1], w[5], w[9], w[13]);
            detail::quarterRound (w[2], w[6], w[10], w[14]);
            detail::quarterRound (w[3], w[7], w[11], w[15]);
            detail::quarterRound (w[0], w[5], w[10], w[15]);
            detail::quarterRound (w[1], w[6], w[11], w[12]);
            detail::quarterRound (w[2], w[7], w[8], w[13]);
            detail::quarterRound (w[3], w[4], w[9], w[14]);
        }
        for (int i = 0; i < 16; ++i)
            detail::store32 (out + 4 * i, w[i] + s[i]);
    }

    /** RFC 8439 2.4: XOR with the key stream from block `counter` on (encrypts and decrypts). */
    inline void chacha20Xor (const Key& key, std::uint32_t counter, const Nonce& nonce, std::uint8_t* data, size_t size)
    {
        std::uint8_t block[64];
        for (size_t at = 0; at < size; at += 64, ++counter)
        {
            chacha20Block (key, counter, nonce, block);
            const size_t n = size - at < 64 ? size - at : 64;
            for (size_t i = 0; i < n; ++i)
                data[at + i] ^= block[i];
        }
    }

    /** RFC 8439 2.5: Poly1305 one-time authenticator (26-bit limbs). */
    class Poly1305
    {
    public:
        explicit Poly1305 (const std::uint8_t key[32])
        {
            using detail::load32;
            m_r[0] = (load32 (key + 0)) & 0x3ffffffu;
            m_r[1] = (load32 (key + 3) >> 2) & 0x3ffff03u;
            m_r[2] = (load32 (key + 6) >> 4) & 0x3ffc0ffu;
            m_r[3] = (load32 (key + 9) >> 6) & 0x3f03fffu;
            m_r[4] = (load32 (key + 12) >> 8) & 0x00fffffu;
            for (int i = 0; i < 4; ++i)
                m_pad[i] = load32 (key + 16 + 4 * i);
        }

        void update (const std::uint8_t* m, size_t size)
        {
            if (m_leftover > 0)
            {
                while (size > 0 && m_leftover < 16)
                {
                    m_buffer[m_leftover++] = *m++;
                    --size;
                }
                if (m_leftover < 16)
                    return;
                blocks (m_buffer, 16, false);
                m_leftover = 0;
            }
            const size_t whole = size & ~static_cast<size_t> (15);
            if (whole > 0)
            {
                blocks (m, whole, false);
                m += whole;
                size -= whole;
            }
            while (size-- > 0)
                m_buffer[m_leftover++] = *m++;
        }

        Tag finish()
        {
            if (m_leftover > 0)
            {
                m_buffer[m_leftover] = 1;
                for (size_t i = m_leftover + 1; i < 16; ++i)
                    m_buffer[i] = 0;
                blocks (m_buffer, 16, true);
            }

            std::uint32_t h0 = m_h[0], h1 = m_h[1], h2 = m_h[2], h3 = m_h[3], h4 = m_h[4];
            std::uint32_t c = h1 >> 26; h1 &= 0x3ffffffu;
            h2 += c; c = h2 >> 26; h2 &= 0x3ffffffu;
            h3 += c; c = h3 >> 26; h3 &= 0x3ffffffu;
            h4 += c; c = h4 >> 26; h4 &= 0x3ffffffu;
            h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu;
            h1 += c;

            // h - p, kept if it does not borrow.
            std::uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffffu;
            std::uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffffu;
            std::uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffffu;
            std::uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffffu;
            std::uint32_t g4 = h4 + c - (1u << 26);
            std::uint32_t mask = (g4 >> 31) - 1u;
            g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
            mask = ~mask;
            h0 = (h0 & mask) | g0;
            h1 = (h1 & mask) | g1;
            h2 = (h2 & mask) | g2;
            h3 = (h3 & mask) | g3;
            h4 = (h4 & mask) | g4;

            h0 = (h0 | (h1 << 26)) & 0xffffffffu;
            h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffu;
            h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffu;
            h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffu;

            std::uint64_t f = static_cast<std::uint64_t> (h0) + m_pad[0]; h0 = static_cast<std::uint32_t> (f);
            f = static_cast<std::uint64_t> (h1) + m_pad[1] + (f >> 32); h1 = static_cast<std::uint32_t> (f);
            f = static_cast<std::uint64_t> (h2) + m_pad[2] + (f >> 32); h2 = static_cast<std::uint32_t> (f);
            f = static_cast<std::uint64_t> (h3) + m_pad[3] + (f >> 32); h3 = static_cast<std::uint32_t> (f);

            Tag tag {};
            detail::store32 (tag.data() + 0, h0);
            detail::store32 (tag.data() + 4, h1);
            detail::store32 (tag.data() + 8, h2);
            detail::store32 (tag.data() + 12, h3);
            return tag;
        }

    private:
        void blocks (const std::uint8_t* m, size_t size, bool final)
        {
            using detail::load32;
            const std::uint32_t hibit = final ? 0u : (1u << 24);
            const std::uint64_t r0 = m_r[0], r1 = m_r[1], r2 = m_r[2], r3 = m_r[3], r4 = m_r[4];
            const std::uint64_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
            std::uint32_t h0 = m_h[0], h1 = m_h[1], h2 = m_h[2], h3 = m_h[3], h4 = m_h[4];

            for (; size >= 16; m += 16, size -= 16)
            {
                h0 += (load32 (m + 0)) & 0x3ffffffu;
                h1 += (load32 (m + 3) >> 2) & 0x3ffffffu;
                h2 += (load32 (m + 6) >> 4) & 0x3ffffffu;
                h3 += (load32 (m + 9) >> 6) & 0x3ffffffu;
                h4 += (load32 (m + 12) >> 8) | hibit;

                const std::uint64_t d0 = h0 * r0 + h1 * s4 + h2 * s3 + h3 * s2 + h4 * s1;
                std::uint64_t d1 = h0 * r1 + h1 * r0 + h2 * s4 + h3 * s3 + h4 * s2;
                std::uint64_t d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4 + h4 * s3;
                std::uint64_t d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4;
                std::uint64_t d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;

                std::uint32_t c = static_cast<std::uint32_t> (d0 >> 26); h0 = static_cast<std::uint32_t> (d0) & 0x3ffffffu;
                d1 += c; c = static_cast<std::uint32_t> (d1 >> 26); h1 = static_cast<std::uint32_t> (d1) & 0x3ffffffu;
                d2 += c; c = static_cast<std::uint32_t> (d2 >> 26); h2 = static_cast<std::uint32_t> (d2) & 0x3ffffffu;
                d3 += c; c = static_cast<std::uint32_t> (d3 >> 26); h3 = static_cast<std::uint32_t> (d3) & 0x3ffffffu;
                d4 += c; c = static_cast<std::uint32_t> (d4 >> 26); h4 = static_cast<std::uint32_t> (d4) & 0x3ffffffu;
                h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu;
                h1 += c;
            }

            m_h[0] = h0; m_h[1] = h1; m_h[2] = h2; m_h[3] = h3; m_h[4] = h4;
        }

        std::uint32_t m_r[5] {}, m_h[5] {}, m_pad[4] {};
        std::uint8_t m_buffer[16] {};
        size_t m_leftover = 0;
    };

    inline Tag poly1305 (const std::uint8_t key[32], const std::uint8_t* m, size_t size)
    {
        Poly1305 p (key);
        p.update (m, size);
        return p.finish();
    }

    namespace detail
    {
        inline Tag aeadTag (const Key& key, const Nonce& nonce, const std::uint8_t* aad, size_t aadSize,
                            const std::uint8_t* cipher, size_t cipherSize)
        {
            std::uint8_t block[64];
            chacha20Block (key, 0, nonce, block);   // the one-time Poly1305 key
            Poly1305 p (block);
            static const std::uint8_t zeros[16] {};
            p.update (aad, aadSize);
            p.update (zeros, (16 - aadSize % 16) % 16);
            p.update (cipher, cipherSize);
            p.update (zeros, (16 - cipherSize % 16) % 16);
            std::uint8_t lengths[16];
            for (int i = 0; i < 8; ++i)
            {
                lengths[i] = static_cast<std::uint8_t> (static_cast<std::uint64_t> (aadSize) >> (8 * i));
                lengths[8 + i] = static_cast<std::uint8_t> (static_cast<std::uint64_t> (cipherSize) >> (8 * i));
            }
            p.update (lengths, 16);
            std::memset (block, 0, sizeof (block));
            return p.finish();
        }
    }

    /** RFC 8439 2.8: encrypts data in place and returns the tag. */
    inline Tag aeadEncrypt (const Key& key, const Nonce& nonce, const std::uint8_t* aad, size_t aadSize, std::uint8_t* data, size_t size)
    {
        chacha20Xor (key, 1, nonce, data, size);
        return detail::aeadTag (key, nonce, aad, aadSize, data, size);
    }

    /** Verifies the tag (constant time) and only then decrypts data in place. False: data untouched. */
    inline bool aeadDecrypt (const Key& key, const Nonce& nonce, const std::uint8_t* aad, size_t aadSize, std::uint8_t* data, size_t size,
                             const Tag& tag)
    {
        const Tag expected = detail::aeadTag (key, nonce, aad, aadSize, data, size);
        std::uint8_t diff = 0;
        for (size_t i = 0; i < tag.size(); ++i)
            diff = static_cast<std::uint8_t> (diff | (expected[i] ^ tag[i]));
        if (diff != 0)
            return false;
        chacha20Xor (key, 1, nonce, data, size);
        return true;
    }
}
