#pragma once

#include <stdint.h>

// The canonical value wire is intentionally independent of the host C/C++
// object layout.  Writers and readers are bounded cursors; callers must check
// good() before publishing or consuming a value.
namespace naos::canonical
{
class writer
{
  public:
    writer(uint8_t *buffer, uint64_t capacity)
        : buffer_(buffer)
        , capacity_(capacity)
    {
    }

    bool good() const { return good_; }
    uint64_t size() const { return offset_; }

    void put_u8(uint8_t value) { put_bytes(&value, sizeof(value)); }
    void put_u16(uint16_t value)
    {
        uint8_t bytes[sizeof(value)] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
        put_bytes(bytes, sizeof(bytes));
    }
    void put_u32(uint32_t value)
    {
        uint8_t bytes[sizeof(value)];
        for (uint64_t i = 0; i < sizeof(value); i++)
            bytes[i] = static_cast<uint8_t>(value >> (i * 8));
        put_bytes(bytes, sizeof(bytes));
    }
    void put_u64(uint64_t value)
    {
        uint8_t bytes[sizeof(value)];
        for (uint64_t i = 0; i < sizeof(value); i++)
            bytes[i] = static_cast<uint8_t>(value >> (i * 8));
        put_bytes(bytes, sizeof(bytes));
    }
    void put_i64(int64_t value) { put_u64(static_cast<uint64_t>(value)); }
    void put_bytes(const void *source, uint64_t size)
    {
        if (!good_ || size > capacity_ - offset_ || (size != 0 && source == nullptr))
        {
            good_ = false;
            return;
        }
        const auto *bytes = static_cast<const uint8_t *>(source);
        for (uint64_t i = 0; i < size; i++)
            buffer_[offset_ + i] = bytes[i];
        offset_ += size;
    }

  private:
    uint8_t *buffer_;
    uint64_t capacity_;
    uint64_t offset_ = 0;
    bool good_ = buffer_ != nullptr || capacity_ == 0;
};

class reader
{
  public:
    reader(const uint8_t *buffer, uint64_t size)
        : buffer_(buffer)
        , size_(size)
    {
    }

    bool good() const { return good_; }
    uint64_t remaining() const { return offset_ <= size_ ? size_ - offset_ : 0; }
    uint64_t offset() const { return offset_; }
    uint8_t get_u8()
    {
        uint8_t value = 0;
        get_bytes(&value, sizeof(value));
        return value;
    }
    uint16_t get_u16()
    {
        uint8_t bytes[sizeof(uint16_t)] = {};
        get_bytes(bytes, sizeof(bytes));
        return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8;
    }
    uint32_t get_u32()
    {
        uint8_t bytes[sizeof(uint32_t)] = {};
        get_bytes(bytes, sizeof(bytes));
        uint32_t value = 0;
        for (uint64_t i = 0; i < sizeof(value); i++)
            value |= static_cast<uint32_t>(bytes[i]) << (i * 8);
        return value;
    }
    uint64_t get_u64()
    {
        uint8_t bytes[sizeof(uint64_t)] = {};
        get_bytes(bytes, sizeof(bytes));
        uint64_t value = 0;
        for (uint64_t i = 0; i < sizeof(value); i++)
            value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
        return value;
    }
    int64_t get_i64() { return static_cast<int64_t>(get_u64()); }
    bool get_bytes(void *destination, uint64_t size)
    {
        if (!good_ || size > this->remaining() || (size != 0 && destination == nullptr))
        {
            good_ = false;
            return false;
        }
        auto *bytes = static_cast<uint8_t *>(destination);
        for (uint64_t i = 0; i < size; i++)
            bytes[i] = buffer_[offset_ + i];
        offset_ += size;
        return true;
    }

  private:
    const uint8_t *buffer_;
    uint64_t size_;
    uint64_t offset_ = 0;
    bool good_ = buffer_ != nullptr || size_ == 0;
};
} // namespace naos::canonical
