#include "host.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define HOST_PATH_CAPACITY 4096u

static bool host_path(Pocket *p, Value path, char out[HOST_PATH_CAPACITY])
{
    size_t i, written = 0u;
    if (p_type(path) != P_STRING) {
        (void)p_error(p, P_ERR_TYPE, "host path must be a string");
        return false;
    }
    for (i = 0; i < p_string_ptr(path)->length; ++i) {
        if (p_string_ptr(path)->data[i] == 0u) {
            (void)p_error(p, P_ERR_IO, "host path contains an embedded NUL");
            return false;
        }
    }
    if (!p_string_to_utf8(p, path, out, HOST_PATH_CAPACITY, &written))
        return false;
    if (written == 0u || written >= HOST_PATH_CAPACITY) {
        (void)p_error(p, P_ERR_RANGE, "host path is empty or too long");
        return false;
    }
    out[written] = '\0';
    return true;
}

bool p_host_file_size(Pocket *p, Value path, size_t *size)
{
    char filename[HOST_PATH_CAPACITY];
    FILE *file;
    long end;
    if (size == NULL) {
        (void)p_error(p, P_ERR_TYPE, "file-size result pointer is null");
        return false;
    }
    if (!host_path(p, path, filename))
        return false;
    file = fopen(filename, "rb");
    if (file == NULL) {
        (void)p_error(p, P_ERR_IO, "unable to open file for reading");
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (end = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        (void)p_error(p, P_ERR_IO, "unable to determine file size");
        return false;
    }
    *size = (size_t)end;
    if ((long)*size != end) {
        (void)fclose(file);
        (void)p_error(p, P_ERR_RANGE, "file is too large for this platform");
        return false;
    }
    (void)fclose(file);
    return true;
}

bool p_host_read_file(Pocket *p, Value path, Value bytes)
{
    char filename[HOST_PATH_CAPACITY];
    FILE *file;
    size_t length, read_count;
    PBytes *buffer;
    if (p_type(bytes) != P_BYTES) {
        (void)p_error(p, P_ERR_TYPE, "file read destination must be bytes");
        return false;
    }
    if (!host_path(p, path, filename))
        return false;
    buffer = (PBytes *)bytes;
    length = buffer->length;
    file = fopen(filename, "rb");
    if (file == NULL) {
        (void)p_error(p, P_ERR_IO, "unable to open file for reading");
        return false;
    }
    read_count = length == 0u ? 0u : fread(buffer->data, 1u, length, file);
    if (ferror(file)) {
        (void)fclose(file);
        (void)p_error(p, P_ERR_IO, "error while reading file");
        return false;
    }
    if (read_count != length && !feof(file)) {
        (void)fclose(file);
        (void)p_error(p, P_ERR_IO, "incomplete file read");
        return false;
    }
    if (read_count != length) {
        /* The file may have changed after the safe-point size check. A shrink
         * is harmless: the preallocated object's logical length can decrease. */
        buffer->length = read_count;
    } else if (fgetc(file) != EOF) {
        (void)fclose(file);
        (void)p_error(p, P_ERR_IO, "file grew after its size was checked");
        return false;
    }
    (void)fclose(file);
    return true;
}

bool p_host_write_file(Pocket *p, Value path, Value bytes)
{
    char filename[HOST_PATH_CAPACITY];
    FILE *file;
    size_t length, written;
    if (p_type(bytes) != P_BYTES) {
        (void)p_error(p, P_ERR_TYPE, "file write source must be bytes");
        return false;
    }
    if (!host_path(p, path, filename))
        return false;
    length = ((PBytes *)bytes)->length;
    file = fopen(filename, "wb");
    if (file == NULL) {
        (void)p_error(p, P_ERR_IO, "unable to open file for writing");
        return false;
    }
    written = length == 0u ? 0u : fwrite(((PBytes *)bytes)->data, 1u, length, file);
    if (written != length || fflush(file) != 0 || ferror(file)) {
        (void)fclose(file);
        (void)p_error(p, P_ERR_IO, "error while writing file");
        return false;
    }
    if (fclose(file) != 0) {
        (void)p_error(p, P_ERR_IO, "error while closing written file");
        return false;
    }
    return true;
}

static bool write_codepoint(FILE *file, uint32_t codepoint)
{
    unsigned char bytes[4];
    size_t length;
    if (codepoint < 0x80u) {
        bytes[0] = (unsigned char)codepoint;
        length = 1u;
    } else if (codepoint < 0x800u) {
        bytes[0] = (unsigned char)(0xc0u | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80u | (codepoint & 63u));
        length = 2u;
    } else if (codepoint < 0x10000u) {
        bytes[0] = (unsigned char)(0xe0u | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80u | ((codepoint >> 6) & 63u));
        bytes[2] = (unsigned char)(0x80u | (codepoint & 63u));
        length = 3u;
    } else {
        bytes[0] = (unsigned char)(0xf0u | (codepoint >> 18));
        bytes[1] = (unsigned char)(0x80u | ((codepoint >> 12) & 63u));
        bytes[2] = (unsigned char)(0x80u | ((codepoint >> 6) & 63u));
        bytes[3] = (unsigned char)(0x80u | (codepoint & 63u));
        length = 4u;
    }
    return fwrite(bytes, 1u, length, file) == length;
}

bool p_host_console_write_string(Pocket *p, Value string)
{
    size_t i;
    PString *text;
    if (p_type(string) != P_STRING) {
        (void)p_error(p, P_ERR_TYPE, "console output requires a string");
        return false;
    }
    text = p_string_ptr(string);
    for (i = 0; i < text->length; ++i) {
        if (!write_codepoint(stdout, text->data[i])) {
            (void)p_error(p, P_ERR_IO, "error while writing console output");
            return false;
        }
    }
    if (fflush(stdout) != 0) {
        (void)p_error(p, P_ERR_IO, "error while flushing console output");
        return false;
    }
    return true;
}

bool p_host_console_read_line(Pocket *p, char *out, size_t capacity,
                              size_t *written, bool *eof)
{
    char *result;
    if (out == NULL || capacity < 2u || written == NULL || eof == NULL) {
        (void)p_error(p, P_ERR_RANGE, "console input buffer is too small");
        return false;
    }
    *written = 0u;
    *eof = false;
    if (capacity > (size_t)INT_MAX)
        capacity = (size_t)INT_MAX;
    result = fgets(out, (int)capacity, stdin);
    if (result == NULL) {
        if (feof(stdin)) {
            *eof = true;
            return true;
        }
        (void)p_error(p, P_ERR_IO, "error while reading console input");
        return false;
    }
    *written = strlen(out);
    return true;
}
