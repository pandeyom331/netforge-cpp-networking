#pragma once

#include "netforge/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace netforge {

class FrameParser {
public:
    // Maximum input chunk accepted by a single append() call.
    static constexpr std::size_t MAX_INPUT_CHUNK_SIZE =
        64U * 1024U;

    // Enough for one maximum-sized frame and one input chunk.
    static constexpr std::size_t MAX_BUFFERED_BYTES =
        HEADER_SIZE +
        static_cast<std::size_t>(MAX_PAYLOAD_SIZE) +
        MAX_INPUT_CHUNK_SIZE;

    // Append newly received bytes to the internal buffer.
    // Returns false if the input cannot be accepted.
    bool append(
        const std::uint8_t* data,
        std::size_t size,
        std::string& error
    );

    // Attempt to decode the next frame from buffered data.
    // A completed frame is consumed automatically.
    DecodeResult next_frame();

    // Number of bytes not yet consumed by the parser.
    std::size_t buffered_bytes() const noexcept;

    // Discard buffered data and return to the initial state.
    void reset() noexcept;

private:
    std::vector<std::uint8_t> buffer_;

    // Index of the first unconsumed byte in buffer_.
    std::size_t read_offset_{0};
};

} // namespace netforge
