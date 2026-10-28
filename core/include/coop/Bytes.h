#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop {

// Little-endian writer. Never fails; the caller enforces size limits.
class ByteWriter {
public:
    void U8(uint8_t p_Value) { m_Data.push_back(p_Value); }

    void U16(uint16_t p_Value) {
        U8(static_cast<uint8_t>(p_Value));
        U8(static_cast<uint8_t>(p_Value >> 8));
    }

    void U32(uint32_t p_Value) {
        U16(static_cast<uint16_t>(p_Value));
        U16(static_cast<uint16_t>(p_Value >> 16));
    }

    void U64(uint64_t p_Value) {
        U32(static_cast<uint32_t>(p_Value));
        U32(static_cast<uint32_t>(p_Value >> 32));
    }

    void I16(int16_t p_Value) { U16(static_cast<uint16_t>(p_Value)); }
    void I32(int32_t p_Value) { U32(static_cast<uint32_t>(p_Value)); }

    void F32(float p_Value) {
        uint32_t s_Bits;
        std::memcpy(&s_Bits, &p_Value, sizeof(s_Bits));
        U32(s_Bits);
    }

    void Bool(bool p_Value) { U8(p_Value ? 1 : 0); }

    // Length-prefixed (u16) string.
    void Str(const std::string& p_Value) {
        const auto s_Len = static_cast<uint16_t>(p_Value.size() > 0xFFFF ? 0xFFFF : p_Value.size());
        U16(s_Len);
        Raw(p_Value.data(), s_Len);
    }

    void Raw(const void* p_Data, size_t p_Size) {
        const auto* s_Bytes = static_cast<const uint8_t*>(p_Data);
        m_Data.insert(m_Data.end(), s_Bytes, s_Bytes + p_Size);
    }

    void PatchU16(size_t p_Offset, uint16_t p_Value) {
        m_Data[p_Offset] = static_cast<uint8_t>(p_Value);
        m_Data[p_Offset + 1] = static_cast<uint8_t>(p_Value >> 8);
    }

    const std::vector<uint8_t>& Data() const { return m_Data; }
    std::vector<uint8_t>& Data() { return m_Data; }
    size_t Size() const { return m_Data.size(); }
    void Clear() { m_Data.clear(); }

private:
    std::vector<uint8_t> m_Data;
};

// Bounds-checked little-endian reader. After the first failed read every
// further read fails too, so callers can read a whole struct and check Ok() once.
class ByteReader {
public:
    ByteReader(const uint8_t* p_Data, size_t p_Size) : m_Data(p_Data), m_Size(p_Size) {}
    explicit ByteReader(const std::vector<uint8_t>& p_Data) : ByteReader(p_Data.data(), p_Data.size()) {}

    bool U8(uint8_t& p_Out) {
        if (!Need(1))
            return false;

        p_Out = m_Data[m_Pos++];
        return true;
    }

    bool U16(uint16_t& p_Out) {
        if (!Need(2))
            return false;

        p_Out = static_cast<uint16_t>(m_Data[m_Pos] | (m_Data[m_Pos + 1] << 8));
        m_Pos += 2;
        return true;
    }

    bool U32(uint32_t& p_Out) {
        uint16_t s_Lo, s_Hi;

        if (!U16(s_Lo) || !U16(s_Hi))
            return false;

        p_Out = static_cast<uint32_t>(s_Lo) | (static_cast<uint32_t>(s_Hi) << 16);
        return true;
    }

    bool U64(uint64_t& p_Out) {
        uint32_t s_Lo, s_Hi;

        if (!U32(s_Lo) || !U32(s_Hi))
            return false;

        p_Out = static_cast<uint64_t>(s_Lo) | (static_cast<uint64_t>(s_Hi) << 32);
        return true;
    }

    bool I16(int16_t& p_Out) {
        uint16_t s_Value;

        if (!U16(s_Value))
            return false;

        p_Out = static_cast<int16_t>(s_Value);
        return true;
    }

    bool I32(int32_t& p_Out) {
        uint32_t s_Value;

        if (!U32(s_Value))
            return false;

        p_Out = static_cast<int32_t>(s_Value);
        return true;
    }

    // Rejects NaN and infinity, which would otherwise poison transforms.
    bool F32(float& p_Out) {
        uint32_t s_Bits;

        if (!U32(s_Bits))
            return false;

        if ((s_Bits & 0x7F800000u) == 0x7F800000u)
            return Fail();

        std::memcpy(&p_Out, &s_Bits, sizeof(p_Out));
        return true;
    }

    bool Bool(bool& p_Out) {
        uint8_t s_Value;

        if (!U8(s_Value))
            return false;

        if (s_Value > 1)
            return Fail();

        p_Out = s_Value != 0;
        return true;
    }

    bool Str(std::string& p_Out, size_t p_MaxLen) {
        uint16_t s_Len;

        if (!U16(s_Len))
            return false;

        if (s_Len > p_MaxLen || !Need(s_Len))
            return Fail();

        p_Out.assign(reinterpret_cast<const char*>(m_Data + m_Pos), s_Len);
        m_Pos += s_Len;
        return true;
    }

    bool Raw(void* p_Out, size_t p_Size) {
        if (!Need(p_Size))
            return false;

        std::memcpy(p_Out, m_Data + m_Pos, p_Size);
        m_Pos += p_Size;
        return true;
    }

    bool Skip(size_t p_Size) {
        if (!Need(p_Size))
            return false;

        m_Pos += p_Size;
        return true;
    }

    const uint8_t* Cursor() const { return m_Data + m_Pos; }
    size_t Remaining() const { return m_Ok ? m_Size - m_Pos : 0; }
    bool Ok() const { return m_Ok; }
    bool AtEnd() const { return m_Ok && m_Pos == m_Size; }

    bool Fail() {
        m_Ok = false;
        return false;
    }

private:
    bool Need(size_t p_Size) {
        if (!m_Ok || m_Size - m_Pos < p_Size)
            return Fail();

        return true;
    }

    const uint8_t* m_Data;
    size_t m_Size;
    size_t m_Pos = 0;
    bool m_Ok = true;
};

} // namespace coop
