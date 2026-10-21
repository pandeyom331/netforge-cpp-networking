
#include "netforge/frame_parser.hpp"

#include <new>

namespace netforge {

bool FrameParser::append(
    const std::uint8_t* data,
    std::size_t size,
    std::string& error)
{
    // Start with an empty error message.
    error.clear();

    // Appending zero bytes is a valid no-op.
    if (size == 0) {
        return true;
    }

    // A non-empty input must have a valid data pointer.
    if (data == nullptr) {
        error = "Input data pointer is null";
        return false;
    }

    // Reject unexpectedly large chunks.
    if (size > MAX_INPUT_CHUNK_SIZE) {
        error = "Input chunk exceeds maximum allowed size";
        return false;
    }

    // Remove bytes belonging to frames already consumed.
    // Keep all remaining, unprocessed bytes.
    if (read_offset_ > 0) {
        buffer_.erase(
            buffer_.begin(),
            buffer_.begin() +
                static_cast<
                    std::vector<std::uint8_t>::difference_type
                >(read_offset_)
        );

        read_offset_ = 0;
    }

    // Check the limit without calculating a potentially
    // overflowing buffer_.size() + size expression.
    if (size > MAX_BUFFERED_BYTES - buffer_.size()) {
        error = "Parser buffer limit exceeded";
        return false;
    }

    // Append the new bytes to the existing buffered data.
    try {
        buffer_.insert(
            buffer_.end(),
            data,
            data + size
        );
    } catch (const std::bad_alloc&) {
        error = "Insufficient memory to append input";
        return false;
    }

    return true;
}

} // namespace netforge


namespace netforge {

DecodeResult FrameParser::next_frame()
{
    // Calculate how many bytes have not yet been consumed.
    const std::size_t available =
        buffer_.size() - read_offset_;

    // An empty buffer has no frame to decode.
    if (available == 0) {
        return {
            DecodeStatus::NeedMoreData,
            {},
            0,
            {}
        };
    }

    // Try to decode one frame starting at the first
    // unconsumed byte.
    DecodeResult result = decode_frame(
        buffer_.data() + read_offset_,
        available
    );

    // Only advance the read position when a complete
    // frame has been decoded successfully.
    if (result.status == DecodeStatus::Complete) {
        // Defensive check: a decoded frame must consume
        // a non-zero number of available bytes.
        if (result.bytes_consumed == 0 ||
            result.bytes_consumed > available) {
            return {
                DecodeStatus::Invalid,
                {},
                0,
                "Invalid decoder byte count"
            };
        }

        read_offset_ += result.bytes_consumed;

        // If every buffered byte has been consumed,
        // clear the buffer and reset the offset.
        if (read_offset_ == buffer_.size()) {
            buffer_.clear();
            read_offset_ = 0;
        }
    }

    return result;
}

std::size_t FrameParser::buffered_bytes() const noexcept
{
    return buffer_.size() - read_offset_;
}

void FrameParser::reset() noexcept
{
    buffer_.clear();
    read_offset_ = 0;
}

} // namespace netforge
