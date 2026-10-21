
#include "netforge/protocol.hpp"

#include <stdexcept>
#include <utility>

namespace netforge {
namespace {

constexpr std::uint8_t MAGIC_0 = 'N';
constexpr std::uint8_t MAGIC_1 = 'F';

// Check whether a numeric message type is defined by our protocol.
bool is_valid_message_type(std::uint8_t type)
{
    return type >= static_cast<std::uint8_t>(MessageType::Hello) &&
           type <= static_cast<std::uint8_t>(MessageType::Pong);
}

// Append a 32-bit number in big-endian (network byte) order.
void append_u32_be(std::vector<std::uint8_t>& output,
                   std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 24));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

// Read a 32-bit big-endian number from four bytes.
std::uint32_t read_u32_be(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8)  |
            static_cast<std::uint32_t>(data[3]);
}

DecodeResult need_more_data()
{
    return {DecodeStatus::NeedMoreData, {}, 0, {}};
}

DecodeResult invalid_frame(const char* reason)
{
    return {DecodeStatus::Invalid, {}, 0, reason};
}

} // unnamed namespace

std::vector<std::uint8_t> encode_frame(const Frame& frame)
{
    // Reject payloads that exceed our protocol limit.
    if (frame.payload.size() > MAX_PAYLOAD_SIZE) {
        throw std::length_error("Payload exceeds maximum size");
    }

    const auto type = static_cast<std::uint8_t>(frame.type);

    if (!is_valid_message_type(type)) {
        throw std::invalid_argument("Unknown message type");
    }

    std::vector<std::uint8_t> output;
    output.reserve(HEADER_SIZE + frame.payload.size());

    // Bytes 0-1: Magic bytes.
    output.push_back(MAGIC_0);
    output.push_back(MAGIC_1);

    // Byte 2: Protocol version.
    output.push_back(PROTOCOL_VERSION);

    // Byte 3: Message type.
    output.push_back(type);

    // Bytes 4-7: Payload length.
    append_u32_be(
        output,
        static_cast<std::uint32_t>(frame.payload.size())
    );

    // Bytes 8-11: Request ID.
    append_u32_be(output, frame.request_id);

    // Bytes 12 onward: Payload.
    output.insert(
        output.end(),
        frame.payload.begin(),
        frame.payload.end()
    );

    return output;
}

DecodeResult decode_frame(const std::uint8_t* data,
                          std::size_t size)
{
    if (size == 0) {
        return need_more_data();
    }

    if (data == nullptr) {
        return invalid_frame("Null data pointer");
    }

    // Validate whatever magic-byte information is already available.
    if (data[0] != MAGIC_0) {
        return invalid_frame("Invalid magic byte");
    }

    if (size < 2) {
        return need_more_data();
    }

    if (data[1] != MAGIC_1) {
        return invalid_frame("Invalid magic bytes");
    }

    // We cannot parse header fields until all 12 header bytes exist.
    if (size < HEADER_SIZE) {
        return need_more_data();
    }

    if (data[2] != PROTOCOL_VERSION) {
        return invalid_frame("Unsupported protocol version");
    }

    const std::uint8_t raw_type = data[3];

    if (!is_valid_message_type(raw_type)) {
        return invalid_frame("Unknown message type");
    }

    const std::uint32_t payload_length = read_u32_be(data + 4);
    const std::uint32_t request_id = read_u32_be(data + 8);

    // Validate the declared length before waiting for or copying the payload.
    if (payload_length > MAX_PAYLOAD_SIZE) {
        return invalid_frame("Payload exceeds maximum size");
    }

    const std::size_t total_size =
        HEADER_SIZE + static_cast<std::size_t>(payload_length);

    // The header is valid, but the complete payload has not arrived.
    if (size < total_size) {
        return need_more_data();
    }

    Frame frame;
    frame.type = static_cast<MessageType>(raw_type);
    frame.request_id = request_id;

    frame.payload.assign(
        data + HEADER_SIZE,
        data + total_size
    );

    // Consume exactly one frame; later bytes belong to subsequent frames.
    return {
        DecodeStatus::Complete,
        std::move(frame),
        total_size,
        {}
    };
}

} // namespace netforge
