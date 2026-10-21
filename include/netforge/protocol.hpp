
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace netforge {

inline constexpr std::uint8_t PROTOCOL_VERSION = 1;

inline constexpr std::size_t HEADER_SIZE = 12;

inline constexpr std::uint32_t MAX_PAYLOAD_SIZE =
    1024U * 1024U; // 1 MiB

enum class MessageType : std::uint8_t {
    Hello     = 1,
    Chat      = 2,
    ListUsers = 3,
    FileBegin = 4,
    FileChunk = 5,
    FileEnd   = 6,
    Error     = 7,
    Ping      = 8,
    Pong      = 9
};

struct Frame {
    MessageType type{MessageType::Error};
    std::uint32_t request_id{0};
    std::vector<std::uint8_t> payload;
};

enum class DecodeStatus {
    Complete,
    NeedMoreData,
    Invalid
};

struct DecodeResult {
    DecodeStatus status;
    Frame frame;
    std::size_t bytes_consumed{0};
    std::string error;
};

std::vector<std::uint8_t> encode_frame(const Frame& frame);

DecodeResult decode_frame(
    const std::uint8_t* data,
    std::size_t size
);

} // namespace netforge
