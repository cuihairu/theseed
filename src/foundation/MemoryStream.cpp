#include "theseed/foundation/MemoryStream.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace theseed::foundation {

MemoryStream::MemoryStream(std::size_t initialCapacity)
    : buffer_(initialCapacity) {}

MemoryStream::MemoryStream(MemoryStream&& other) noexcept
    : buffer_(std::move(other.buffer_)),
      rpos_(std::exchange(other.rpos_, 0)),
      wpos_(std::exchange(other.wpos_, 0)) {}

MemoryStream& MemoryStream::operator=(MemoryStream&& other) noexcept {
    if (this != &other) {
        buffer_ = std::move(other.buffer_);
        rpos_ = std::exchange(other.rpos_, 0);
        wpos_ = std::exchange(other.wpos_, 0);
    }
    return *this;
}

void MemoryStream::ensureWriteCapacity(std::size_t additionalBytes) {
    const auto needed = wpos_ + additionalBytes;
    if (needed <= buffer_.size()) return;

    auto newCap = buffer_.size();
    while (newCap < needed) {
        newCap = newCap == 0 ? additionalBytes : newCap * 2;
    }
    buffer_.resize(newCap);
}

void MemoryStream::checkReadBounds(std::size_t count) const {
    if (rpos_ + count > wpos_) {
        throw std::runtime_error("MemoryStream read overflow");
    }
}

void MemoryStream::writeInt8(std::int8_t value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeInt16(std::int16_t value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeInt32(std::int32_t value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeInt64(std::int64_t value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeUint8(std::uint8_t value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeUint16(std::uint16_t value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeUint32(std::uint32_t value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeUint64(std::uint64_t value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeFloat(float value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeDouble(double value) {
    writeBytes(&value, sizeof(value));
}

void MemoryStream::writeBool(bool value) {
    writeUint8(value ? 1 : 0);
}

void MemoryStream::writeString(std::string_view value) {
    writeUint32(static_cast<std::uint32_t>(value.size()));
    if (!value.empty()) {
        writeBytes(value.data(), value.size());
    }
}

void MemoryStream::writeBytes(const void* data, std::size_t size) {
    if (size == 0) return;
    ensureWriteCapacity(size);
    std::memcpy(buffer_.data() + wpos_, data, size);
    wpos_ += size;
}

std::int8_t MemoryStream::readInt8() {
    std::int8_t value;
    readBytes(&value, sizeof(value));
    return value;
}

std::int16_t MemoryStream::readInt16() {
    std::int16_t value;
    readBytes(&value, sizeof(value));
    return value;
}

std::int32_t MemoryStream::readInt32() {
    std::int32_t value;
    readBytes(&value, sizeof(value));
    return value;
}

std::int64_t MemoryStream::readInt64() {
    std::int64_t value;
    readBytes(&value, sizeof(value));
    return value;
}

std::uint8_t MemoryStream::readUint8() {
    std::uint8_t value;
    readBytes(&value, sizeof(value));
    return value;
}

std::uint16_t MemoryStream::readUint16() {
    std::uint16_t value;
    readBytes(&value, sizeof(value));
    return value;
}

std::uint32_t MemoryStream::readUint32() {
    std::uint32_t value;
    readBytes(&value, sizeof(value));
    return value;
}

std::uint64_t MemoryStream::readUint64() {
    std::uint64_t value;
    readBytes(&value, sizeof(value));
    return value;
}

float MemoryStream::readFloat() {
    float value;
    readBytes(&value, sizeof(value));
    return value;
}

double MemoryStream::readDouble() {
    double value;
    readBytes(&value, sizeof(value));
    return value;
}

bool MemoryStream::readBool() {
    return readUint8() != 0;
}

std::string MemoryStream::readString() {
    const auto len = readUint32();
    if (len == 0) return {};
    std::string result(len, '\0');
    readBytes(result.data(), len);
    return result;
}

void MemoryStream::readBytes(void* out, std::size_t size) {
    if (size == 0) return;
    checkReadBounds(size);
    std::memcpy(out, buffer_.data() + rpos_, size);
    rpos_ += size;
}

std::size_t MemoryStream::readPos() const { return rpos_; }
std::size_t MemoryStream::writePos() const { return wpos_; }
std::size_t MemoryStream::size() const { return wpos_; }
std::size_t MemoryStream::capacity() const { return buffer_.size(); }
std::size_t MemoryStream::readRemaining() const {
    return wpos_ >= rpos_ ? wpos_ - rpos_ : 0;
}

void MemoryStream::readSkip(std::size_t count) {
    checkReadBounds(count);
    rpos_ += count;
}

void MemoryStream::writeSkip(std::size_t count) {
    ensureWriteCapacity(count);
    std::memset(buffer_.data() + wpos_, 0, count);
    wpos_ += count;
}

void MemoryStream::clear() {
    rpos_ = 0;
    wpos_ = 0;
}

void MemoryStream::resetRead() { rpos_ = 0; }
void MemoryStream::resetWrite() { wpos_ = 0; }

const std::byte* MemoryStream::data() const { return buffer_.data(); }
std::byte* MemoryStream::data() { return buffer_.data(); }
const std::byte* MemoryStream::readPtr() const { return buffer_.data() + rpos_; }
const std::byte* MemoryStream::writePtr() const { return buffer_.data() + wpos_; }

}  // namespace theseed::foundation
