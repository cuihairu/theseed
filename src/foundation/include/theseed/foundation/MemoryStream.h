#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace theseed::foundation {

class MemoryStream final {
public:
    explicit MemoryStream(std::size_t initialCapacity = 256);

    MemoryStream(const MemoryStream&) = delete;
    MemoryStream& operator=(const MemoryStream&) = delete;

    MemoryStream(MemoryStream&& other) noexcept;
    MemoryStream& operator=(MemoryStream&& other) noexcept;

    // Write operations
    void writeInt8(std::int8_t value);
    void writeInt16(std::int16_t value);
    void writeInt32(std::int32_t value);
    void writeInt64(std::int64_t value);
    void writeUint8(std::uint8_t value);
    void writeUint16(std::uint16_t value);
    void writeUint32(std::uint32_t value);
    void writeUint64(std::uint64_t value);
    void writeFloat(float value);
    void writeDouble(double value);
    void writeBool(bool value);
    void writeString(std::string_view value);
    void writeBytes(const void* data, std::size_t size);

    // Read operations
    std::int8_t readInt8();
    std::int16_t readInt16();
    std::int32_t readInt32();
    std::int64_t readInt64();
    std::uint8_t readUint8();
    std::uint16_t readUint16();
    std::uint32_t readUint32();
    std::uint64_t readUint64();
    float readFloat();
    double readDouble();
    bool readBool();
    std::string readString();
    void readBytes(void* out, std::size_t size);

    // State
    std::size_t readPos() const;
    std::size_t writePos() const;
    std::size_t size() const;
    std::size_t capacity() const;
    std::size_t readRemaining() const;

    void readSkip(std::size_t count);
    void writeSkip(std::size_t count);

    void clear();
    void resetRead();
    void resetWrite();

    const std::byte* data() const;
    std::byte* data();
    const std::byte* readPtr() const;
    const std::byte* writePtr() const;

private:
    void ensureWriteCapacity(std::size_t additionalBytes);
    void checkReadBounds(std::size_t count) const;

    std::vector<std::byte> buffer_;
    std::size_t rpos_ = 0;
    std::size_t wpos_ = 0;
};

}  // namespace theseed::foundation
