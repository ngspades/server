// Copyright CircumScriptor and DarkNeutrino 2021
#include <Util/DataStream.h>
#include <Util/Log.h>
#include <Util/Alloc.h>
#include <stdlib.h>
#include <string.h>

/* A stream is a bounds checked cursor over a byte buffer. A reader that does
   not fit leaves the cursor untouched and yields zero, which is what the
   legacy packet handlers expect of a short packet; every value is read and
   written little endian. */

static uint8_t* stream_reserve(stream_t* stream, uint32_t size)
{
    if (stream->pos + size > stream->length) {
        return NULL;
    }
    uint8_t* start = stream->data + stream->pos;
    stream->pos += size;
    return start;
}

static uint32_t stream_read_le(stream_t* stream, uint32_t size)
{
    uint8_t* bytes = stream_reserve(stream, size);
    if (bytes == NULL) {
        return 0;
    }
    uint32_t value = 0;
    for (uint32_t i = 0; i < size; i++) {
        value |= (uint32_t) bytes[i] << (8 * i);
    }
    return value;
}

static void stream_write_le(stream_t* stream, uint32_t value, uint32_t size)
{
    uint8_t* bytes = stream_reserve(stream, size);
    if (bytes == NULL) {
        return;
    }
    for (uint32_t i = 0; i < size; i++) {
        bytes[i] = (uint8_t) (value >> (8 * i));
    }
}

void stream_create(stream_t* stream, uint32_t length)
{
    stream->data   = spadesx_malloc(length);
    stream->length = length;
    stream->pos    = 0;
}

void stream_free(stream_t* stream)
{
    free(stream->data);
}

uint32_t stream_left(stream_t* stream)
{
    return (stream->pos < stream->length) ? stream->length - stream->pos : 0;
}

void stream_skip(stream_t* stream, uint32_t skip)
{
    uint32_t target = stream->pos + skip;
    stream->pos     = (target < stream->length) ? target : stream->length;
}

uint8_t stream_read_u8(stream_t* stream)
{
    return (uint8_t) stream_read_le(stream, 1);
}

uint16_t stream_read_u16(stream_t* stream)
{
    return (uint16_t) stream_read_le(stream, 2);
}

uint32_t stream_read_u32(stream_t* stream)
{
    return stream_read_le(stream, 4);
}

float stream_read_f(stream_t* stream)
{
    union
    {
        float    f;
        uint32_t v;
    } u;
    u.v = stream_read_u32(stream);
    return u.f;
}

color_t stream_read_color_rgb(stream_t* stream)
{
    color_t color = {.raw = 0};

    uint8_t* bytes = stream_reserve(stream, 3);
    if (bytes == NULL) {
        return color;
    }
    color.b = bytes[0];
    color.g = bytes[1];
    color.r = bytes[2];
    color.a = 0x0;
    return color;
}

color_t stream_read_color_argb(stream_t* stream)
{
    color_t color = {.raw = 0};

    uint8_t* bytes = stream_reserve(stream, 4);
    if (bytes == NULL) {
        return color;
    }
    color.b = bytes[0];
    color.g = bytes[1];
    color.r = bytes[2];
    color.a = bytes[3];
    return color;
}

void stream_read_array(stream_t* stream, void* output, uint32_t length)
{
    uint8_t* bytes = stream_reserve(stream, length);
    if (bytes == NULL) {
        return;
    }
    memcpy(output, bytes, length);
}

void stream_write_array(stream_t* stream, const void* array, uint32_t length)
{
    uint8_t* bytes = stream_reserve(stream, length);
    if (bytes == NULL) {
        return;
    }
    memcpy(bytes, array, length);
}

void stream_write_u8(stream_t* stream, uint8_t value)
{
    stream_write_le(stream, value, 1);
}

void stream_write_u16(stream_t* stream, uint16_t value)
{
    stream_write_le(stream, value, 2);
}

void stream_write_u32(stream_t* stream, uint32_t value)
{
    stream_write_le(stream, value, 4);
}

void stream_write_f(stream_t* stream, float value)
{
    union
    {
        float    f;
        uint32_t v;
    } u;
    u.f = value;
    stream_write_u32(stream, u.v);
}

void stream_write_vector3f(stream_t* stream, vector3f_t vector)
{
    stream_write_f(stream, vector.x);
    stream_write_f(stream, vector.y);
    stream_write_f(stream, vector.z);
}

void stream_write_color_rgb(stream_t* stream, color_t color)
{
    uint8_t* bytes = stream_reserve(stream, 3);
    if (bytes == NULL) {
        return;
    }
    bytes[0] = color.b;
    bytes[1] = color.g;
    bytes[2] = color.r;
}

void stream_write_color_argb(stream_t* stream, color_t color)
{
    uint8_t* bytes = stream_reserve(stream, 4);
    if (bytes == NULL) {
        return;
    }
    bytes[0] = color.b;
    bytes[1] = color.g;
    bytes[2] = color.r;
    bytes[3] = color.a;
}

void stream_write_color_3u8(stream_t* stream, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t* bytes = stream_reserve(stream, 3);
    if (bytes == NULL) {
        return;
    }
    bytes[0] = b;
    bytes[1] = g;
    bytes[2] = r;
}

void stream_write_color_4u8(stream_t* stream, uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t* bytes = stream_reserve(stream, 4);
    if (bytes == NULL) {
        return;
    }
    bytes[0] = b;
    bytes[1] = g;
    bytes[2] = r;
    bytes[3] = a;
}
