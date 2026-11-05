/* Шифры для чтения защищённых PDF с пустым паролем (1.9): MD5, RC4, AES-128/256, SHA-256/384/512.
   Свои реализации, чтобы не зависеть от CryptoAPI и набора его провайдеров в конкретной Windows.
   Пароль здесь известен (пустой), поэтому от кода не требуется стойкость к сторонним каналам. */
#include "pdfcrypt.h"
#include <string.h>

namespace {
inline unsigned rol32(unsigned x, int n) { return (x << n) | (x >> (32 - n)); }
inline unsigned ror32(unsigned x, int n) { return (x >> n) | (x << (32 - n)); }
inline unsigned long long ror64(unsigned long long x, int n) { return (x >> n) | (x << (64 - n)); }
}

std::string md5(const std::string& input) {
    static const unsigned K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int S[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                              4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    std::string m = input;
    unsigned long long bits = (unsigned long long)input.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 0; i < 8; i++) m += (char)((bits >> (8 * i)) & 0xFF);
    unsigned h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t off = 0; off < m.size(); off += 64) {
        unsigned w[16];
        for (int i = 0; i < 16; i++) {
            const unsigned char* p = (const unsigned char*)m.data() + off + i * 4;
            w[i] = p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
        }
        unsigned a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            unsigned f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            unsigned t = d;
            d = c;
            c = b;
            b = b + rol32(a + f + K[i] + w[g], S[i]);
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    std::string out;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) out += (char)((h[i] >> (8 * k)) & 0xFF);
    return out;
}

std::string rc4(const std::string& key, const std::string& data) {
    unsigned char st[256];
    for (int i = 0; i < 256; i++) st[i] = (unsigned char)i;
    if (key.empty()) return data;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + st[i] + (unsigned char)key[(size_t)i % key.size()]) & 255;
        unsigned char t = st[i]; st[i] = st[j]; st[j] = t;
    }
    std::string out(data);
    int a = 0, b = 0;
    for (size_t k = 0; k < out.size(); k++) {
        a = (a + 1) & 255;
        b = (b + st[a]) & 255;
        unsigned char t = st[a]; st[a] = st[b]; st[b] = t;
        out[k] = (char)((unsigned char)out[k] ^ st[(st[a] + st[b]) & 255]);
    }
    return out;
}

std::string sha256(const std::string& input) {
    static const unsigned K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::string m = input;
    unsigned long long bits = (unsigned long long)input.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)((bits >> (8 * i)) & 0xFF);
    unsigned h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (size_t off = 0; off < m.size(); off += 64) {
        unsigned w[64];
        for (int i = 0; i < 16; i++) {
            const unsigned char* p = (const unsigned char*)m.data() + off + i * 4;
            w[i] = ((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        }
        for (int i = 16; i < 64; i++) {
            unsigned s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
            unsigned s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            unsigned t1 = hh + (ror32(e,6)^ror32(e,11)^ror32(e,25)) + ((e&f)^(~e&g)) + K[i] + w[i];
            unsigned t2 = (ror32(a,2)^ror32(a,13)^ror32(a,22)) + ((a&b)^(a&c)^(b&c));
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    std::string out;
    for (int i = 0; i < 8; i++)
        for (int k = 3; k >= 0; k--) out += (char)((h[i] >> (8 * k)) & 0xFF);
    return out;
}

/* SHA-384 и SHA-512 — один алгоритм с разными начальными значениями */
static std::string sha512core(const std::string& input, const unsigned long long* iv, size_t outLen) {
    static const unsigned long long K[80] = {
        0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,
        0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,0xd807aa98a3030242ULL,0x12835b0145706fbeULL,
        0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,
        0xc19bf174cf692694ULL,0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,0x983e5152ee66dfabULL,
        0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,
        0x06ca6351e003826fULL,0x142929670a0e6e70ULL,0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,
        0x53380d139d95b3dfULL,0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,
        0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,
        0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,
        0x682e6ff3d6b2b8a3ULL,0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,0xca273eceea26619cULL,
        0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,
        0x113f9804bef90daeULL,0x1b710b35131c471bULL,0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,
        0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};
    std::string m = input;
    unsigned long long bits = (unsigned long long)input.size() * 8;
    m += (char)0x80;
    while (m.size() % 128 != 112) m += (char)0;
    for (int i = 0; i < 8; i++) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)((bits >> (8 * i)) & 0xFF);
    unsigned long long h[8];
    for (int i = 0; i < 8; i++) h[i] = iv[i];
    for (size_t off = 0; off < m.size(); off += 128) {
        unsigned long long w[80];
        for (int i = 0; i < 16; i++) {
            unsigned long long v = 0;
            for (int k = 0; k < 8; k++) v = (v << 8) | (unsigned char)m[off + i * 8 + k];
            w[i] = v;
        }
        for (int i = 16; i < 80; i++) {
            unsigned long long s0 = ror64(w[i-15],1) ^ ror64(w[i-15],8) ^ (w[i-15] >> 7);
            unsigned long long s1 = ror64(w[i-2],19) ^ ror64(w[i-2],61) ^ (w[i-2] >> 6);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned long long a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 80; i++) {
            unsigned long long t1 = hh + (ror64(e,14)^ror64(e,18)^ror64(e,41)) + ((e&f)^(~e&g)) + K[i] + w[i];
            unsigned long long t2 = (ror64(a,28)^ror64(a,34)^ror64(a,39)) + ((a&b)^(a&c)^(b&c));
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    std::string out;
    for (int i = 0; i < 8; i++)
        for (int k = 7; k >= 0; k--) out += (char)((h[i] >> (8 * k)) & 0xFF);
    out.resize(outLen);
    return out;
}

std::string sha384(const std::string& input) {
    static const unsigned long long iv[8] = {
        0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL};
    return sha512core(input, iv, 48);
}

std::string sha512(const std::string& input) {
    static const unsigned long long iv[8] = {
        0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    return sha512core(input, iv, 64);
}

/* ------------------------------- AES ------------------------------------ */

namespace {
unsigned char sbox[256], inv[256];
bool aesReady = false;

inline unsigned char xt(unsigned char x) { return (unsigned char)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

unsigned char mul(unsigned char a, unsigned char b) {
    unsigned char r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

/* таблицы считаются при первом вызове: так их нельзя случайно испортить опечаткой */
void aesInit() {
    if (aesReady) return;
    unsigned char p = 1, q = 1;
    do {
        p = (unsigned char)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (unsigned char)(q << 1);
        q ^= (unsigned char)(q << 2);
        q ^= (unsigned char)(q << 4);
        if (q & 0x80) q ^= 0x09;
        unsigned char x = (unsigned char)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^
                                          ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        sbox[p] = (unsigned char)(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) inv[sbox[i]] = (unsigned char)i;
    aesReady = true;
}

struct AesKey {
    unsigned char rk[240];
    int rounds;
    explicit AesKey(const std::string& key) {
        aesInit();
        int nk = (int)key.size() / 4;
        rounds = nk + 6;
        memcpy(rk, key.data(), key.size());
        unsigned char rcon = 1;
        for (int i = nk; i < 4 * (rounds + 1); i++) {
            unsigned char t[4];
            memcpy(t, rk + 4 * (i - 1), 4);
            if (i % nk == 0) {
                unsigned char f = t[0];
                t[0] = (unsigned char)(sbox[t[1]] ^ rcon);
                t[1] = sbox[t[2]];
                t[2] = sbox[t[3]];
                t[3] = sbox[f];
                rcon = xt(rcon);
            } else if (nk > 6 && i % nk == 4) {
                for (int k = 0; k < 4; k++) t[k] = sbox[t[k]];
            }
            for (int k = 0; k < 4; k++) rk[4 * i + k] = (unsigned char)(rk[4 * (i - nk) + k] ^ t[k]);
        }
    }
    void addKey(unsigned char* s, int r) const {
        for (int i = 0; i < 16; i++) s[i] ^= rk[16 * r + i];
    }
    void encrypt(unsigned char* s) const {
        addKey(s, 0);
        for (int r = 1; r <= rounds; r++) {
            unsigned char t[16];
            for (int c = 0; c < 4; c++)
                for (int w = 0; w < 4; w++) t[4 * c + w] = sbox[s[4 * ((c + w) % 4) + w]];
            if (r < rounds) {
                for (int c = 0; c < 4; c++) {
                    unsigned char* a = t + 4 * c;
                    unsigned char b0 = a[0], b1 = a[1], b2 = a[2], b3 = a[3];
                    a[0] = (unsigned char)(xt(b0) ^ xt(b1) ^ b1 ^ b2 ^ b3);
                    a[1] = (unsigned char)(b0 ^ xt(b1) ^ xt(b2) ^ b2 ^ b3);
                    a[2] = (unsigned char)(b0 ^ b1 ^ xt(b2) ^ xt(b3) ^ b3);
                    a[3] = (unsigned char)(xt(b0) ^ b0 ^ b1 ^ b2 ^ xt(b3));
                }
            }
            memcpy(s, t, 16);
            addKey(s, r);
        }
    }
    void decrypt(unsigned char* s) const {
        addKey(s, rounds);
        for (int r = rounds - 1; r >= 0; r--) {
            unsigned char t[16];
            for (int c = 0; c < 4; c++)
                for (int w = 0; w < 4; w++) t[4 * ((c + w) % 4) + w] = inv[s[4 * c + w]];
            memcpy(s, t, 16);
            addKey(s, r);
            if (r > 0) {
                for (int c = 0; c < 4; c++) {
                    unsigned char* a = s + 4 * c;
                    unsigned char b0 = a[0], b1 = a[1], b2 = a[2], b3 = a[3];
                    a[0] = (unsigned char)(mul(b0, 14) ^ mul(b1, 11) ^ mul(b2, 13) ^ mul(b3, 9));
                    a[1] = (unsigned char)(mul(b0, 9) ^ mul(b1, 14) ^ mul(b2, 11) ^ mul(b3, 13));
                    a[2] = (unsigned char)(mul(b0, 13) ^ mul(b1, 9) ^ mul(b2, 14) ^ mul(b3, 11));
                    a[3] = (unsigned char)(mul(b0, 11) ^ mul(b1, 13) ^ mul(b2, 9) ^ mul(b3, 14));
                }
            }
        }
    }
};
} /* namespace */

std::string aesCbcEncrypt(const std::string& key, const std::string& iv, const std::string& data) {
    AesKey k(key);
    std::string out(data.size() - data.size() % 16, '\0');
    unsigned char prev[16];
    memcpy(prev, iv.data(), 16);
    for (size_t off = 0; off + 16 <= data.size(); off += 16) {
        unsigned char b[16];
        for (int i = 0; i < 16; i++) b[i] = (unsigned char)((unsigned char)data[off + i] ^ prev[i]);
        k.encrypt(b);
        memcpy(&out[off], b, 16);
        memcpy(prev, b, 16);
    }
    return out;
}

std::string aesCbcDecrypt(const std::string& key, const std::string& iv, const std::string& data) {
    AesKey k(key);
    std::string out(data.size() - data.size() % 16, '\0');
    unsigned char prev[16];
    memcpy(prev, iv.data(), 16);
    for (size_t off = 0; off + 16 <= data.size(); off += 16) {
        unsigned char b[16], c[16];
        memcpy(c, data.data() + off, 16);
        memcpy(b, c, 16);
        k.decrypt(b);
        for (int i = 0; i < 16; i++) out[off + i] = (char)(b[i] ^ prev[i]);
        memcpy(prev, c, 16);
    }
    return out;
}
