/* Декодеры картинок из PDF (1.9): CCITT Fax, JBIG2, JPEG 2000. В форматах книг такие картинки
   показать нельзя, поэтому они распаковываются здесь и дальше пишутся как PNG. */
#ifndef FB2C_IMGCODEC_H
#define FB2C_IMGCODEC_H

#include <string>
#include <vector>

/* Строки упакованы по 1 биту на пиксель с выравниванием на байт, 0 — чёрный (как в PDF). */
struct CcittParams {
    int k;               /* <0 — G4, 0 — G3 одномерный, >0 — G3 смешанный */
    int columns, rows;   /* rows может быть 0 — тогда до конца данных */
    bool blackIs1, byteAlign, endOfLine, endOfBlock;
    CcittParams() : k(0), columns(1728), rows(0), blackIs1(false), byteAlign(false), endOfLine(false), endOfBlock(true) {}
};
bool ccittDecode(const std::string& data, const CcittParams& p, std::string& out);

/* JBIG2 внутри PDF. globals может быть пустой. Выход упакован как выше: 0 — чёрный. */
bool jbig2Decode(const std::string& data, const std::string& globals, int width, int height, std::string& out);

struct JpxImage {
    enum { CS_UNKNOWN, CS_GRAY, CS_RGB, CS_CMYK };
    int w, h, comps, colorSpace;
    std::string pixels;  /* по 8 бит на канал, каналы подряд */
    JpxImage() : w(0), h(0), comps(0), colorSpace(CS_UNKNOWN) {}
};
bool jpxDecode(const std::string& data, JpxImage& out);

/* Арифметический MQ-декодер: общий для JBIG2 и JPEG 2000 (Annex E и Annex C стандартов) */
class MqDecoder {
public:
    MqDecoder(const unsigned char* data, size_t start, size_t end);
    int bit(unsigned char& cx) {
        static const unsigned short QE[47] = {
            0x5601,0x3401,0x1801,0x0AC1,0x0521,0x0221,0x5601,0x5401,0x4801,0x3801,0x3001,0x2401,0x1C01,0x1601,
            0x5601,0x5401,0x5101,0x4801,0x3801,0x3401,0x3001,0x2801,0x2401,0x2201,0x1C01,0x1801,0x1601,0x1401,
            0x1201,0x1101,0x0AC1,0x09C1,0x08A1,0x0521,0x0441,0x02A1,0x0221,0x0141,0x0111,0x0085,0x0049,0x0025,
            0x0015,0x0009,0x0005,0x0001,0x5601};
        static const unsigned char NMPS[47] = {
            1,2,3,4,5,38,7,8,9,10,11,12,13,29,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,45,46};
        static const unsigned char NLPS[47] = {
            1,6,9,12,29,33,6,14,14,14,17,18,20,21,14,14,15,16,17,18,19,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,46};
        static const unsigned char SW[47] = {
            1,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
        unsigned idx = cx >> 1, mps = cx & 1, qe = QE[idx], d;
        unsigned a = a_ - qe;
        if (chigh_ < qe) {
            if (a < qe) { d = mps; idx = NMPS[idx]; }
            else { d = 1 ^ mps; if (SW[idx]) mps = d; idx = NLPS[idx]; }
            a = qe;
        } else {
            chigh_ -= qe;
            if (a & 0x8000) { a_ = a; return (int)mps; }
            if (a < qe) { d = 1 ^ mps; if (SW[idx]) mps = d; idx = NLPS[idx]; }
            else { d = mps; idx = NMPS[idx]; }
        }
        do {
            if (ct_ == 0) byteIn();
            a <<= 1;
            chigh_ = ((chigh_ << 1) & 0xFFFF) | ((clow_ >> 15) & 1);
            clow_ = (clow_ << 1) & 0xFFFF;
            ct_--;
        } while (!(a & 0x8000));
        a_ = a;
        cx = (unsigned char)((idx << 1) | mps);
        return (int)d;
    }
    size_t position() const { return bp_; }

private:
    void byteIn();
    const unsigned char* d_;
    size_t bp_, end_;
    unsigned chigh_, clow_, a_;
    int ct_;
};

#endif
