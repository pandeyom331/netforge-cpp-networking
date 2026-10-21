
#include "netforge/frame_parser.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace netforge;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        std::cerr << "[FAIL] " << description << '\n';
        ++failures;
    }
}

Frame make_frame(
    std::uint32_t request_id,
    std::vector<std::uint8_t> payload)
{
    Frame frame;
    frame.type = MessageType::Chat;
    frame.request_id = request_id;
    frame.payload = std::move(payload);
    return frame;
}

// Test 1: A frame arrives in two separate chunks.
void test_fragmented_frame()
{
    Frame original = make_frame(100, {'H', 'e', 'l', 'l', 'o'});
    const auto bytes = encode_frame(original);

    FrameParser parser;
    std::string error;

    // First chunk: header plus two payload bytes.
    const std::size_t split = HEADER_SIZE + 2;

    check(parser.append(bytes.data(), split, error),
          "First chunk accepted");

    DecodeResult first = parser.next_frame();

    check(first.status == DecodeStatus::NeedMoreData,
          "Incomplete frame requests more data");

    check(parser.buffered_bytes() == split,
          "Incomplete bytes are retained");

    // Second chunk: the rest of the frame.
    check(parser.append(bytes.data() + split,
                        bytes.size() - split, error),
          "Second chunk accepted");

    DecodeResult second = parser.next_frame();

    check(second.status == DecodeStatus::Complete,
          "Frame completes after second chunk");

    if (second.status == DecodeStatus::Complete) {
        check(second.frame.request_id == 100,
              "Decoded request ID is correct");

        check(second.frame.payload == original.payload,
              "Decoded payload is correct");
    }

    check(parser.buffered_bytes() == 0,
          "Completed frame leaves no unconsumed bytes");
}

// Test 2: Two frames arrive in one input chunk.
void test_multiple_frames()
{
    const auto first_bytes =
        encode_frame(make_frame(101, {'A'}));

    const auto second_bytes =
        encode_frame(make_frame(102, {'B', 'C'}));

    std::vector<std::uint8_t> combined = first_bytes;

    combined.insert(combined.end(),
                    second_bytes.begin(),
                    second_bytes.end());

    FrameParser parser;
    std::string error;

    check(parser.append(combined.data(), combined.size(), error),
          "Combined input accepted");

    DecodeResult first = parser.next_frame();

    check(first.status == DecodeStatus::Complete,
          "First frame decoded");

    check(parser.buffered_bytes() == second_bytes.size(),
          "Second frame remains buffered");

    DecodeResult second = parser.next_frame();

    check(second.status == DecodeStatus::Complete,
          "Second frame decoded without another recv");

    if (first.status == DecodeStatus::Complete &&
        second.status == DecodeStatus::Complete) {
        check(first.frame.request_id == 101,
              "First frame has correct request ID");

        check(second.frame.request_id == 102,
              "Second frame has correct request ID");

        check(second.frame.payload ==
                  std::vector<std::uint8_t>({'B', 'C'}),
              "Second frame payload is preserved");
    }

    check(parser.buffered_bytes() == 0,
          "Both frames are consumed");
}

// Test 3: Invalid input must be reported, not silently discarded.
void test_invalid_frame()
{
    auto bytes = encode_frame(make_frame(103, {'X'}));
    bytes[0] = 'X'; // Corrupt the magic byte.

    FrameParser parser;
    std::string error;

    check(parser.append(bytes.data(), bytes.size(), error),
          "Bytes can be buffered before validation");

    DecodeResult result = parser.next_frame();

    check(result.status == DecodeStatus::Invalid,
          "Invalid frame is rejected");

    check(parser.buffered_bytes() == bytes.size(),
          "Invalid data is not silently consumed");

    parser.reset();

    check(parser.buffered_bytes() == 0,
          "Reset clears buffered data");
}

// Test 4: Reject an oversized input chunk.
void test_input_chunk_limit()
{
    FrameParser parser;
    std::string error;

    std::vector<std::uint8_t> oversized(
        FrameParser::MAX_INPUT_CHUNK_SIZE + 1, 0
    );

    const bool accepted = parser.append(
        oversized.data(), oversized.size(), error
    );

    check(!accepted,
          "Oversized input chunk is rejected");

    check(!error.empty(),
          "Rejection supplies an error message");

    check(parser.buffered_bytes() == 0,
          "Rejected chunk does not enter the buffer");
}

} // namespace

int main()
{
    test_fragmented_frame();
    test_multiple_frames();
    test_invalid_frame();
    test_input_chunk_limit();

    std::cout << "\nTest failures: " << failures << '\n';

    return failures == 0 ? 0 : 1;
}
