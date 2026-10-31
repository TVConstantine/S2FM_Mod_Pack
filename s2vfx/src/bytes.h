// Little endian reading and writing of byte buffers.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

struct FormatError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class Reader
{
public:
    Reader(const Bytes& data, size_t pos = 0) : b(data), p(pos) {}

    template <typename T> T take()
    {
        need(sizeof(T));
        T v;
        std::memcpy(&v, b.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }

    Bytes raw(size_t n)
    {
        need(n);
        Bytes v(b.begin() + p, b.begin() + p + n);
        p += n;
        return v;
    }

    std::string cstr()
    {
        size_t e = p;
        while (e < b.size() && b[e] != 0)
            e++;
        if (e >= b.size())
            throw FormatError("string runs past the end");
        std::string v(b.begin() + p, b.begin() + e);
        p = e + 1;
        return v;
    }

    size_t pos() const { return p; }
    size_t size() const { return b.size(); }

private:
    void need(size_t n) const
    {
        if (p + n > b.size())
            throw FormatError("read past the end");
    }

    const Bytes& b;
    size_t p;
};

class Writer
{
public:
    template <typename T> void put(T v)
    {
        const auto* s = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), s, s + sizeof(T));
    }

    void raw(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
    void raw(const uint8_t* v, size_t n) { b.insert(b.end(), v, v + n); }

    void cstr(const std::string& s)
    {
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }

    size_t size() const { return b.size(); }
    Bytes& bytes() { return b; }

private:
    Bytes b;
};
