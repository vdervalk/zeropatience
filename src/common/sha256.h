// sha256.h - compacte SHA-256, alleen gebruikt om een build-vingerafdruk van
// de game-executable te maken. Header-only zodat er niets te linken valt.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace zp {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        len_ = 0; bufLen_ = 0;
        static const uint32_t kInit[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(h_, kInit, sizeof(h_));
    }

    void update(const uint8_t* p, size_t n) {
        len_ += n;
        while (n) {
            size_t take = 64 - bufLen_;
            if (take > n) take = n;
            memcpy(buf_ + bufLen_, p, take);
            bufLen_ += take; p += take; n -= take;
            if (bufLen_ == 64) { block(buf_); bufLen_ = 0; }
        }
    }

    std::string hex() {
        uint64_t bits = len_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (bufLen_ != 56) update(&zero, 1);
        uint8_t be[8];
        for (int i = 0; i < 8; ++i) be[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(be, 8);

        char out[65];
        for (int i = 0; i < 8; ++i) snprintf(out + i * 8, 9, "%08x", h_[i]);
        return std::string(out, 64);
    }

private:
    static uint32_t ror(uint32_t v, int s) { return (v >> s) | (v << (32 - s)); }

    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
            0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
            0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
            0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
            0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
            0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
            0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
            0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                   ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],hh=h_[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d;
        h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=hh;
    }

    uint32_t h_[8];
    uint8_t  buf_[64];
    size_t   bufLen_ = 0;
    uint64_t len_ = 0;
};

inline std::string sha256File(const std::string& path, uint64_t* sizeOut) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    Sha256 s;
    uint8_t buf[64 * 1024];
    uint64_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) { s.update(buf, n); total += n; }
    fclose(f);
    if (sizeOut) *sizeOut = total;
    return s.hex();
}

} // namespace zp
