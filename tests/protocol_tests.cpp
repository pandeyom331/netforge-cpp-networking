
#include "netforge/protocol.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <utility>

using namespace netforge;

namespace {

int failures = 0;

// Record a test result without stopping at the first failure.
void check(bool condition, const char* description)
{
    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        std::cerr << "[FAIL] " << description << '\n';
        ++failures;
    }
}

Frame make_chat_frame(
    std::uint32_t request_id,
    std::vector<std::uint8_t> payload)
{
    Frame frame;
    frame.type = MessageType::Chat;
    frame.request_id = request_id;
    frame.payload = std::move(payload);
    return frame;
}

// Test 1: Exact wire layout and encode/decode round trip.
void test_round_trip()
{
    const Frame original = make_chat_frame(
        0x12345678U,
        {0x00, 0xFF, 0x41}
    );

    const auto bytes = encode_frame(original);

    check(bytes.size() == HEADER_SIZE + 3,
          "Encoded frame has correct total size");

    check(bytes[0] == 'N' && bytes[1] == 'F',
          "Magic bytes are correct");

    check(bytes[2] == PROTOCOL_VERSION,
          "Version is encoded correctly");

    check(bytes[3] ==
              static_cast<std::uint8_t>(MessageType::Chat),
          "Message type is encoded correctly");

    check(bytes[4] == 0x00 &&
          bytes[5] == 0x00 &&
          bytes[6] == 0x00 &&
          bytes[7] == 0x03,
          "Payload length uses big-endian byte order");

    check(bytes[8] == 0x12 &&
          bytes[9] == 0x34 &&
          bytes[10] == 0x56 &&
          bytes[11] == 0x78,
          "Request ID uses big-endian byte order");

    const DecodeResult decoded =
        decode_frame(bytes.data(), bytes.size());

    check(decoded.status == DecodeStatus::Complete,
          "Complete frame is decoded successfully");

    if (decoded.status == DecodeStatus::Complete) {
        check(decoded.bytes_consumed == bytes.size(),
              "Decoder reports correct bytes consumed");

        check(decoded.frame.type == original.type,
              "Decoded message type matches");

        check(decoded.frame.request_id == original.request_id,
              "Decoded request ID matches");

        check(decoded.frame.payload == original.payload,
              "Binary payload survives the round trip");
    }
}

// Test 2: An incomplete header must not be treated as invalid.
void test_partial_header()
{
    const auto bytes =
        encode_frame(make_chat_frame(1, {'H', 'i'}));

    const DecodeResult result =
        decode_frame(bytes.data(), HEADER_SIZE - 1);

    check(result.status == DecodeStatus::NeedMoreData,
          "Incomplete header requests more data");
}

// Test 3: A valid header with an incomplete payload.
void test_partial_payload()
{
    const auto bytes =
        encode_frame(make_chat_frame(2, {'H', 'e', 'l', 'l', 'o'}));

    const DecodeResult result =
        decode_frame(bytes.data(), HEADER_SIZE + 2);

    check(result.status == DecodeStatus::NeedMoreData,
          "Incomplete payload requests more data");
}

// Test 4: Two frames must be independently recoverable
// even when both are already present in one buffer.
void test_two_frames_in_one_buffer()
{
    const auto first =
        encode_frame(make_chat_frame(10, {'A'}));

    const auto second =
        encode_frame(make_chat_frame(20, {'B', 'C'}));

    std::vector<std::uint8_t> combined = first;
    combined.insert(
        combined.end(),
        second.begin(),
        second.end()
    );

    const DecodeResult result1 =
        decode_frame(combined.data(), combined.size());

    check(result1.status == DecodeStatus::Complete,
          "First frame decodes from combined input");

    if (result1.status != DecodeStatus::Complete) {
        return;
    }

    check(result1.bytes_consumed == first.size(),
          "First frame consumes only its own bytes");

    const std::size_t remaining =
        combined.size() - result1.bytes_consumed;

    const DecodeResult result2 = decode_frame(
        combined.data() + result1.bytes_consumed,
        remaining
    );

    check(result2.status == DecodeStatus::Complete,
          "Second frame decodes without another recv");

    if (result2.status == DecodeStatus::Complete) {
        check(result2.frame.request_id == 20,
              "Second frame retains its request ID");

        check(result2.frame.payload ==
                  std::vector<std::uint8_t>({'B', 'C'}),
              "Second frame retains its payload");
    }
}

// Test 5: Corrupted protocol fields must be rejected.
void test_invalid_headers()
{
    auto bad_magic =
        encode_frame(make_chat_frame(1, {}));
    bad_magic[0] = 'X';

    check(decode_frame(bad_magic.data(), bad_magic.size()).status ==
              DecodeStatus::Invalid,
          "Invalid magic bytes are rejected");

    auto bad_version =
        encode_frame(make_chat_frame(2, {}));
    bad_version[2] = PROTOCOL_VERSION + 1;

    check(decode_frame(bad_version.data(), bad_version.size()).status ==
              DecodeStatus::Invalid,
          "Unsupported version is rejected");

    auto bad_type =
        encode_frame(make_chat_frame(3, {}));
    bad_type[3] = 0xFF;

    check(decode_frame(bad_type.data(), bad_type.size()).status ==
              DecodeStatus::Invalid,
          "Unknown message type is rejected");
}

// Test 6: Reject a payload length above the protocol limit.
void test_oversized_payload_length()
{
    std::vector<std::uint8_t> bytes(HEADER_SIZE, 0);

    bytes[0] = 'N';
    bytes[1] = 'F';
    bytes[2] = PROTOCOL_VERSION;
    bytes[3] = static_cast<std::uint8_t>(MessageType::Chat);

    const std::uint32_t oversized = MAX_PAYLOAD_SIZE + 1;

    bytes[4] = static_cast<std::uint8_t>(oversized >> 24);
    bytes[5] = static_cast<std::uint8_t>(oversized >> 16);
    bytes[6] = static_cast<std::uint8_t>(oversized >> 8);
    bytes[7] = static_cast<std::uint8_t>(oversized);

    const DecodeResult result =
        decode_frame(bytes.data(), bytes.size());

    check(result.status == DecodeStatus::Invalid,
          "Oversized payload length is rejected");
}

// Test 7: The encoder must enforce the same payload limit.
void test_encoder_rejects_oversized_payload()
{
    Frame frame;
    frame.type = MessageType::Chat;
    frame.payload.resize(
        static_cast<std::size_t>(MAX_PAYLOAD_SIZE) + 1
    );

    bool threw = false;

    try {
        (void)encode_frame(frame);
    } catch (const std::length_error&) {
        threw = true;
    }

    check(threw,
          "Encoder rejects oversized payloads");
}

} // namespace

int main()
{
    test_round_trip();
    test_partial_header();
    test_partial_payload();
    test_two_frames_in_one_buffer();
    test_invalid_headers();
    test_oversized_payload_length();
    test_encoder_rejects_oversized_payload();

    std::cout << "\nTest failures: " << failures << '\n';

    return failures == 0 ? 0 : 1;
}
