// Byte-level encoding shared by the transport and the script glue.
//
// A script message is: u16 type, then tagged fields ('i' int32, 'f' float32, 's' u16 length + bytes).
// Both peers are x64 Windows, so integers are written in native (little-endian) order.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace coopnet
{

class Writer
{
  public:
    void U8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
    void U16(uint16_t v) { Raw(&v, sizeof v); }
    void U32(uint32_t v) { Raw(&v, sizeof v); }
    void U64(uint64_t v) { Raw(&v, sizeof v); }
    void Raw(const void *p, size_t n) { buf_.append(static_cast<const char *>(p), n); }
    void Str(const std::string &s)
    {
        size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
        U16(static_cast<uint16_t>(n));
        buf_.append(s.data(), n);
    }

    std::string &Data() { return buf_; }
    void Clear() { buf_.clear(); }

  private:
    std::string buf_;
};

class Reader
{
  public:
    Reader() = default;
    explicit Reader(const std::string &s) : p_(s.data()), n_(s.size()) {}
    Reader(const char *p, size_t n) : p_(p), n_(n) {}

    bool U8(uint8_t &v) { return Raw(&v, sizeof v); }
    bool U16(uint16_t &v) { return Raw(&v, sizeof v); }
    bool U32(uint32_t &v) { return Raw(&v, sizeof v); }
    bool U64(uint64_t &v) { return Raw(&v, sizeof v); }
    bool Raw(void *out, size_t n)
    {
        if (n_ - pos_ < n)
            return false;
        std::memcpy(out, p_ + pos_, n);
        pos_ += n;
        return true;
    }
    bool Str(std::string &s)
    {
        uint16_t n;
        if (!U16(n) || n_ - pos_ < n)
            return false;
        s.assign(p_ + pos_, n);
        pos_ += n;
        return true;
    }
    // Consumes `tag` if it is the next byte.
    bool Expect(uint8_t tag)
    {
        if (pos_ >= n_ || static_cast<uint8_t>(p_[pos_]) != tag)
            return false;
        ++pos_;
        return true;
    }
    std::string Rest()
    {
        std::string s(p_ + pos_, n_ - pos_);
        pos_ = n_;
        return s;
    }
    size_t Left() const { return n_ - pos_; }

  private:
    const char *p_ = nullptr;
    size_t n_ = 0;
    size_t pos_ = 0;
};

enum : uint8_t
{
    kTagInt = 'i',
    kTagFloat = 'f',
    kTagString = 's',
};

inline void WriteInt(Writer &w, int32_t v)
{
    w.U8(kTagInt);
    w.Raw(&v, sizeof v);
}
inline void WriteFloat(Writer &w, float v)
{
    w.U8(kTagFloat);
    w.Raw(&v, sizeof v);
}
inline void WriteString(Writer &w, const std::string &s)
{
    w.U8(kTagString);
    w.Str(s);
}

// Each returns false (reader untouched) when the next field is missing or of another type.
inline bool ReadInt(Reader &r, int32_t &v) { return r.Expect(kTagInt) && r.Raw(&v, sizeof v); }
inline bool ReadFloat(Reader &r, float &v) { return r.Expect(kTagFloat) && r.Raw(&v, sizeof v); }
inline bool ReadString(Reader &r, std::string &s) { return r.Expect(kTagString) && r.Str(s); }

} // namespace coopnet
