#include "PtslWire.h"

namespace tt::ptsl::wire
{

//==============================================================================
void Writer::varint (std::uint64_t value)
{
    while (value >= 0x80)
    {
        out.push_back ((char) (std::uint8_t) ((value & 0x7f) | 0x80));
        value >>= 7;
    }

    out.push_back ((char) (std::uint8_t) value);
}

void Writer::tag (int field, WireType type)
{
    varint (((std::uint64_t) (std::uint32_t) field << 3) | (std::uint64_t) type);
}

void Writer::intField (int field, std::int64_t value)
{
    if (value == 0)
        return;

    tag (field, varintType);
    varint ((std::uint64_t) value);
}

void Writer::stringField (int field, std::string_view value)
{
    if (value.empty())
        return;

    messageField (field, value);
}

void Writer::messageField (int field, std::string_view encoded)
{
    tag (field, lengthType);
    varint (encoded.size());
    out.append (encoded.data(), encoded.size());
}

//==============================================================================
bool Reader::readVarint (std::uint64_t& value)
{
    value = 0;

    for (int shift = 0; shift < 64; shift += 7)
    {
        if (at >= data.size())
            return false;

        const auto byte = (std::uint8_t) data[at++];
        value |= (std::uint64_t) (byte & 0x7f) << shift;

        if ((byte & 0x80) == 0)
            return true;
    }

    return false;
}

bool Reader::next (Field& field)
{
    if (bad || at >= data.size())
        return false;

    std::uint64_t key = 0;

    if (! readVarint (key) || (key >> 3) == 0 || (key >> 3) > 0x1fffffff)
    {
        bad = true;
        return false;
    }

    field = {};
    field.number = (int) (key >> 3);
    field.type = (WireType) (key & 7);

    switch (field.type)
    {
        case varintType:
            if (! readVarint (field.value))
                break;

            return true;

        case fixed64Type:
        case fixed32Type:
        {
            const std::size_t size = field.type == fixed64Type ? 8 : 4;

            if (data.size() - at < size)
                break;

            for (std::size_t i = 0; i < size; ++i)
                field.value |= (std::uint64_t) (std::uint8_t) data[at + i] << (8 * i);

            at += size;
            return true;
        }

        case lengthType:
        {
            std::uint64_t size = 0;

            if (! readVarint (size) || size > data.size() - at)
                break;

            field.bytes = data.substr (at, (std::size_t) size);
            at += (std::size_t) size;
            return true;
        }

        default:
            break;
    }

    bad = true;
    return false;
}

//==============================================================================
std::string frame (std::string_view message)
{
    std::string out;
    out.reserve (message.size() + 5);
    out.push_back ('\0');

    const auto size = (std::uint32_t) message.size();

    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back ((char) (std::uint8_t) (size >> shift));

    out.append (message.data(), message.size());
    return out;
}

Unframed unframe (std::string_view buffer, std::string& message, std::size_t& used)
{
    used = 0;

    if (buffer.size() < 5)
        return Unframed::needMore;

    std::uint32_t size = 0;

    for (int i = 1; i <= 4; ++i)
        size = (size << 8) | (std::uint8_t) buffer[(std::size_t) i];

    if (buffer.size() - 5 < size)
        return Unframed::needMore;

    if (buffer[0] != '\0')
        return Unframed::compressed;

    message.assign (buffer.data() + 5, size);
    used = 5 + (std::size_t) size;
    return Unframed::message;
}

std::string percentDecode (std::string_view text)
{
    const auto hex = [] (char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve (text.size());

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size())
        {
            const int high = hex (text[i + 1]);
            const int low = hex (text[i + 2]);

            if (high >= 0 && low >= 0)
            {
                out.push_back ((char) (high * 16 + low));
                i += 2;
                continue;
            }
        }

        out.push_back (text[i]);
    }

    return out;
}

CallResult finishCall (int httpStatus, int grpcStatus, std::string_view grpcMessage, std::string_view body)
{
    CallResult result;
    result.grpcStatus = grpcStatus;

    if (httpStatus != 200)
    {
        result.error = httpStatus == 0 ? std::string ("the answer had no HTTP status")
                                       : "the answer had HTTP status " + std::to_string (httpStatus);
        return result;
    }

    if (grpcStatus < 0)
    {
        result.error = "the answer ended without a gRPC status";
        return result;
    }

    if (grpcStatus != 0)
    {
        const auto text = percentDecode (grpcMessage);
        result.error = "gRPC status " + std::to_string (grpcStatus) + (text.empty() ? std::string() : ": " + text);
        return result;
    }

    std::size_t used = 0;

    switch (unframe (body, result.message, used))
    {
        case Unframed::needMore:
            result.error = body.empty() ? "the answer held no message" : "the answer's message was cut short";
            return result;

        case Unframed::compressed:
            result.error = "the answer's message was compressed";
            return result;

        case Unframed::message:
            break;
    }

    if (used != body.size())
    {
        result.error = "the answer held more than one message";
        return result;
    }

    result.ok = true;
    return result;
}

//==============================================================================
std::string encodeRequest (const Schema& schema, const Request& request)
{
    Writer header;
    header.intField (schema.requestHeaderCommand, request.command);
    header.intField (schema.requestHeaderVersion, request.version);
    header.stringField (schema.requestHeaderSessionId, request.sessionId);
    header.intField (schema.requestHeaderVersionMinor, request.versionMinor);
    header.intField (schema.requestHeaderVersionRevision, request.versionRevision);

    Writer message;
    message.messageField (schema.requestHeader, header.bytes());
    message.stringField (schema.requestBodyJson, request.bodyJson);
    return message.bytes();
}

bool decodeResponse (const Schema& schema, std::string_view bytes, Response& response)
{
    response = {};
    Reader reader (bytes);
    Field field;

    while (reader.next (field))
    {
        if (field.number == schema.responseHeader && field.type == lengthType)
        {
            Reader header (field.bytes);
            Field inner;

            while (header.next (inner))
            {
                if (inner.number == schema.responseHeaderTaskId && inner.type == lengthType)
                    response.taskId.assign (inner.bytes);
                else if (inner.number == schema.responseHeaderCommand && inner.type == varintType)
                    response.command = (int) (std::int32_t) inner.value;
                else if (inner.number == schema.responseHeaderStatus && inner.type == varintType)
                    response.status = (int) (std::int32_t) inner.value;
            }

            if (header.failed())
                return false;
        }
        else if (field.number == schema.responseBodyJson && field.type == lengthType)
        {
            response.bodyJson.assign (field.bytes);
        }
        else if (field.number == schema.responseErrorJson && field.type == lengthType)
        {
            response.errorJson.assign (field.bytes);
        }
    }

    return ! reader.failed();
}

} // namespace tt::ptsl::wire
