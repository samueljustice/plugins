#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// What travels over the wire to Pro Tools' scripting link, written and read by hand: the few
// protocol buffer messages it takes and gives (a header and a JSON string each way), and the
// gRPC framing around them. Plain C++ with no other library.
//
// No field number is written here. Every number comes in a Schema, which the client fills from
// the header generated from the SDK at build time, and which the tests fill with numbers of
// their own.
namespace tt::ptsl::wire
{

//==============================================================================
// Protocol buffers: varints and length-delimited fields.
enum WireType : int
{
    varintType = 0,
    fixed64Type = 1,
    lengthType = 2,
    fixed32Type = 5
};

class Writer
{
public:
    void varint (std::uint64_t value);
    void tag (int field, WireType type);

    // An int32 or enum field. Negative values take ten bytes, as protocol buffers write them.
    // A zero is left out, as proto3 leaves out a default value.
    void intField (int field, std::int64_t value);

    // A string field. An empty string is left out.
    void stringField (int field, std::string_view value);

    // An embedded message, written even when it is empty.
    void messageField (int field, std::string_view encoded);

    const std::string& bytes() const        { return out; }

private:
    std::string out;
};

struct Field
{
    int number = 0;
    WireType type = varintType;
    std::uint64_t value = 0;        // a varint, fixed32 or fixed64 field
    std::string_view bytes;         // a length-delimited field
};

// Reads the fields of one message in order. Fields of a type it does not know end the reading
// with failed() set.
class Reader
{
public:
    explicit Reader (std::string_view message) : data (message) {}

    // False at the end of the message, or when it is not well formed (then failed() is true).
    bool next (Field& field);
    bool failed() const                     { return bad; }

private:
    std::string_view data;
    std::size_t at = 0;
    bool bad = false;

    bool readVarint (std::uint64_t& value);
};

//==============================================================================
// gRPC framing: a byte that says whether the message is compressed, its length as four bytes
// big-endian, then the message.
std::string frame (std::string_view message);

enum class Unframed
{
    message,        // one whole message was taken from the front of the buffer
    needMore,       // the buffer holds only part of a message
    compressed      // the message is compressed, which this client never asks for
};

// Takes the first message from `buffer`. `used` is how many bytes of the buffer it took.
Unframed unframe (std::string_view buffer, std::string& message, std::size_t& used);

// grpc-message is percent-encoded.
std::string percentDecode (std::string_view text);

// How a unary call ended, from the HTTP/2 status, the gRPC status and message (from the trailers,
// or from the headers when the answer is trailers only) and the body.
struct CallResult
{
    bool ok = false;
    std::string message;            // the response message, unframed
    int grpcStatus = -1;            // -1: none was given
    std::string error;              // why the call failed, one sentence
};

CallResult finishCall (int httpStatus, int grpcStatus, std::string_view grpcMessage, std::string_view body);

//==============================================================================
// The request and response of the unary call.
struct Schema
{
    int requestHeader = 0, requestBodyJson = 0;
    int requestHeaderCommand = 0, requestHeaderVersion = 0, requestHeaderSessionId = 0;
    int requestHeaderVersionMinor = 0, requestHeaderVersionRevision = 0;

    int responseHeader = 0, responseBodyJson = 0, responseErrorJson = 0;
    int responseHeaderTaskId = 0, responseHeaderCommand = 0, responseHeaderStatus = 0;
};

struct Request
{
    int command = 0;
    std::string sessionId;
    int version = 0, versionMinor = 0, versionRevision = 0;
    std::string bodyJson;
};

struct Response
{
    std::string taskId;
    int command = 0;
    int status = 0;
    std::string bodyJson;
    std::string errorJson;
};

std::string encodeRequest (const Schema& schema, const Request& request);

// False when the bytes are not a well-formed response.
bool decodeResponse (const Schema& schema, std::string_view bytes, Response& response);

} // namespace tt::ptsl::wire
