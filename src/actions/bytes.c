/**
 * Byte-array and crypto helpers for plugins: building and parsing binary frames (BLE protocols),
 * hex conversion (keys stored in plugin config), SHA-256 / HMAC-SHA256 and random bytes.
 *
 * "data" arguments accept a string or a byte array; "out" arguments are byte-array variables that
 * are written in place, up to their declared capacity. No permissions are needed: these touch
 * nothing outside the plugin.
 */
#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mt_actions.h"
#include <ctype.h>
#include <string.h>

static const char* TAG = "actions:bytes";

// Kept small: driver plugins run on the 4 KB plugin task stack.
#define BYTES_DATA_MAX 128
#define BYTES_KEY_MAX 64

static int copy_data(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index, uint8_t* buf, int max,
    const char* fn
) {
    int len = mta_arg_copy_bytes(plugin, scope, args, index, buf, max);
    if (len < 0) {
        return mta_raise(plugin, MTA_RT_ERR_TYPE_MISMATCH, "%s: expected a string or byte array", fn);
    }
    return len;
}

static int write_out(mta_array_t* out, const uint8_t* data, int len) {
    int n = len < out->capacity ? len : out->capacity;
    for (int i = 0; i < n; i++) out->data[i] = data[i];
    out->length = n;
    return n;
}

/**
 * SHA-256 of a string or byte array.
 *
 * @plugin sha256
 * @module bytes
 * @arg data:any String or byte array (up to 128 bytes)
 * @arg out:array Receives the 32-byte digest (array capacity >= 32)
 * @returns int 32
 */
static int host_sha256(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "data", MTA_KIND_ANY }, { "out", MTA_KIND_ARRAY } };
    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "sha256");
    if (ret != 0) return ret;

    if (v[1].val.arr->capacity < 32) {
        return mta_raise(plugin, MTA_RT_ERR_TYPE_MISMATCH, "sha256: out needs capacity 32");
    }

    uint8_t data[BYTES_DATA_MAX];
    int len = mta_arg_copy_bytes(plugin, scope, args, 0, data, sizeof(data));
    if (len < 0) return copy_data(plugin, scope, args, 0, data, sizeof(data), "sha256");

    uint8_t digest[32];
    mbedtls_sha256_ret(data, len, digest, 0);
    write_out(v[1].val.arr, digest, 32);
    return mta_return_int(plugin, scope, 32);
}

/**
 * HMAC-SHA256.
 *
 * @plugin hmac_sha256
 * @module bytes
 * @arg key:any Key, string or byte array (up to 64 bytes)
 * @arg data:any Message, string or byte array (up to 128 bytes)
 * @arg out:array Receives the 32-byte MAC (array capacity >= 32)
 * @returns int 32
 */
static int
host_hmac_sha256(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "key", MTA_KIND_ANY },
        { "data", MTA_KIND_ANY },
        { "out", MTA_KIND_ARRAY },
    };
    mta_argv_t v[3];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 3, v, "hmac_sha256");
    if (ret != 0) return ret;

    if (v[2].val.arr->capacity < 32) {
        return mta_raise(plugin, MTA_RT_ERR_TYPE_MISMATCH, "hmac_sha256: out needs capacity 32");
    }

    uint8_t key[BYTES_KEY_MAX];
    uint8_t data[BYTES_DATA_MAX];
    int key_len = mta_arg_copy_bytes(plugin, scope, args, 0, key, sizeof(key));
    int data_len = mta_arg_copy_bytes(plugin, scope, args, 1, data, sizeof(data));
    if (key_len < 0) return copy_data(plugin, scope, args, 0, key, sizeof(key), "hmac_sha256");
    if (data_len < 0) return copy_data(plugin, scope, args, 1, data, sizeof(data), "hmac_sha256");

    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md, key, key_len, data, data_len, mac) != 0) {
        return mta_raise(plugin, MTA_RT_ERR_HOST_DISPATCH_FAILED, "hmac_sha256: mbedtls failed");
    }

    write_out(v[2].val.arr, mac, 32);
    return mta_return_int(plugin, scope, 32);
}

/**
 * Fill a byte array with cryptographically random bytes (hardware RNG; strongest while Wi-Fi or
 * Bluetooth is on).
 *
 * @plugin random_bytes
 * @module bytes
 * @arg out:array Destination array
 * @arg count:int Number of bytes (up to the array's capacity)
 * @returns int Bytes written
 */
static int
host_random_bytes(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "out", MTA_KIND_ARRAY }, { "count", MTA_KIND_INT } };
    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "random_bytes");
    if (ret != 0) return ret;

    mta_array_t* out = v[0].val.arr;
    int n = v[1].val.i;
    if (n < 0) n = 0;
    if (n > out->capacity) n = out->capacity;

    uint8_t buf[BYTES_DATA_MAX];
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    esp_fill_random(buf, n);
    write_out(out, buf, n);
    return mta_return_int(plugin, scope, n);
}

/**
 * Decode a hex string ("0a1B2c", spaces and colons ignored) into a byte array.
 *
 * @plugin hex_to_bytes
 * @module bytes
 * @arg hex:string Hex text
 * @arg out:array Destination array
 * @returns int Bytes written, or -1 if the text is not valid hex or does not fit
 */
static int
host_hex_to_bytes(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "hex", MTA_KIND_STRING }, { "out", MTA_KIND_ARRAY } };
    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "hex_to_bytes");
    if (ret != 0) return ret;

    const char* p = v[0].val.s ? v[0].val.s : "";
    mta_array_t* out = v[1].val.arr;
    int n = 0;
    int high = -1;

    for (; *p; p++) {
        if (*p == ' ' || *p == ':' || *p == '-') continue;
        if (!isxdigit((unsigned char)*p)) return mta_return_int(plugin, scope, -1);

        int nibble = isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10);

        if (high < 0) {
            high = nibble;
        } else {
            if (n >= out->capacity) return mta_return_int(plugin, scope, -1);
            out->data[n++] = (high << 4) | nibble;
            high = -1;
        }
    }

    if (high >= 0) return mta_return_int(plugin, scope, -1);

    out->length = n;
    return mta_return_int(plugin, scope, n);
}

/**
 * Encode a string or byte array as lowercase hex.
 *
 * @plugin bytes_to_hex
 * @module bytes
 * @arg data:any String or byte array (up to 64 bytes)
 * @returns string Hex text
 */
static int
host_bytes_to_hex(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "data", MTA_KIND_ANY } };
    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "bytes_to_hex");
    if (ret != 0) return ret;

    uint8_t data[64];
    int len = copy_data(plugin, scope, args, 0, data, sizeof(data), "bytes_to_hex");
    if (len < 0) return len;

    static const char digits[] = "0123456789abcdef";
    char hex[sizeof(data) * 2 + 1];

    for (int i = 0; i < len; i++) {
        hex[i * 2] = digits[data[i] >> 4];
        hex[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    hex[len * 2] = '\0';

    return mta_return_string(plugin, scope, hex);
}

/**
 * Append a string or byte array to the end of a byte array.
 *
 * @plugin bytes_append
 * @module bytes
 * @arg out:array Array to append to
 * @arg data:any String or byte array
 * @returns int New length, or -1 if it did not fit (nothing is appended)
 */
static int
host_bytes_append(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "out", MTA_KIND_ARRAY }, { "data", MTA_KIND_ANY } };
    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "bytes_append");
    if (ret != 0) return ret;

    uint8_t data[BYTES_DATA_MAX];
    int len = copy_data(plugin, scope, args, 1, data, sizeof(data), "bytes_append");
    if (len < 0) return len;

    mta_array_t* out = v[0].val.arr;
    if (out->length + len > out->capacity) return mta_return_int(plugin, scope, -1);

    for (int i = 0; i < len; i++) out->data[out->length + i] = data[i];
    out->length += len;
    return mta_return_int(plugin, scope, out->length);
}

/**
 * Append an integer as 1, 2 or 4 little-endian bytes (negative size: big-endian).
 *
 * @plugin bytes_append_int
 * @module bytes
 * @arg out:array Array to append to
 * @arg value:int Value
 * @arg size:int 1, 2 or 4 (little-endian), -2 or -4 (big-endian)
 * @returns int New length, or -1 if it did not fit or the size is invalid
 */
static int
host_bytes_append_int(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "out", MTA_KIND_ARRAY },
        { "value", MTA_KIND_INT },
        { "size", MTA_KIND_INT },
    };
    mta_argv_t v[3];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 3, v, "bytes_append_int");
    if (ret != 0) return ret;

    mta_array_t* out = v[0].val.arr;
    uint32_t value = (uint32_t)v[1].val.i;
    int size = v[2].val.i;
    bool big_endian = size < 0;
    if (big_endian) size = -size;

    if ((size != 1 && size != 2 && size != 4) || out->length + size > out->capacity) {
        return mta_return_int(plugin, scope, -1);
    }

    for (int i = 0; i < size; i++) {
        int shift = big_endian ? (size - 1 - i) * 8 : i * 8;
        out->data[out->length + i] = (value >> shift) & 0xff;
    }
    out->length += size;
    return mta_return_int(plugin, scope, out->length);
}

/**
 * Read an unsigned integer from 1, 2 or 4 little-endian bytes (negative size: big-endian).
 *
 * @plugin bytes_read_int
 * @module bytes
 * @arg data:any String or byte array
 * @arg offset:int Byte offset
 * @arg size:int 1, 2 or 4 (little-endian), -2 or -4 (big-endian)
 * @returns int Value, or -1 if out of range (4-byte values above 2^31 come back negative)
 */
static int
host_bytes_read_int(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "data", MTA_KIND_ANY },
        { "offset", MTA_KIND_INT },
        { "size", MTA_KIND_INT },
    };
    mta_argv_t v[3];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 3, v, "bytes_read_int");
    if (ret != 0) return ret;

    uint8_t data[BYTES_DATA_MAX];
    int len = copy_data(plugin, scope, args, 0, data, sizeof(data), "bytes_read_int");
    if (len < 0) return len;

    int offset = v[1].val.i;
    int size = v[2].val.i;
    bool big_endian = size < 0;
    if (big_endian) size = -size;

    if ((size != 1 && size != 2 && size != 4) || offset < 0 || offset + size > len) {
        return mta_return_int(plugin, scope, -1);
    }

    uint32_t value = 0;
    for (int i = 0; i < size; i++) {
        int shift = big_endian ? (size - 1 - i) * 8 : i * 8;
        value |= (uint32_t)data[offset + i] << shift;
    }
    return mta_return_int(plugin, scope, (int32_t)value);
}

/**
 * Empty a byte array (length 0).
 *
 * @plugin bytes_clear
 * @module bytes
 * @arg out:array Array to clear
 * @returns void
 */
static int
host_bytes_clear(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "out", MTA_KIND_ARRAY } };
    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "bytes_clear");
    if (ret != 0) return ret;

    v[0].val.arr->length = 0;
    return mta_return_void(plugin, scope);
}

/**
 * Compare the first n bytes of two strings or byte arrays (constant time).
 *
 * @plugin bytes_equal
 * @module bytes
 * @arg a:any String or byte array
 * @arg b:any String or byte array
 * @arg n:int Number of bytes to compare
 * @returns int 1 if equal (and both have at least n bytes), else 0
 */
static int
host_bytes_equal(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "a", MTA_KIND_ANY },
        { "b", MTA_KIND_ANY },
        { "n", MTA_KIND_INT },
    };
    mta_argv_t v[3];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 3, v, "bytes_equal");
    if (ret != 0) return ret;

    uint8_t a[BYTES_DATA_MAX / 2];
    uint8_t b[BYTES_DATA_MAX / 2];
    int a_len = copy_data(plugin, scope, args, 0, a, sizeof(a), "bytes_equal");
    if (a_len < 0) return a_len;
    int b_len = copy_data(plugin, scope, args, 1, b, sizeof(b), "bytes_equal");
    if (b_len < 0) return b_len;

    int n = v[2].val.i;
    if (n < 0 || n > a_len || n > b_len) return mta_return_int(plugin, scope, 0);

    uint8_t diff = 0;
    for (int i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return mta_return_int(plugin, scope, diff == 0);
}

void action_bytes_init(void) {
    mta_register_system_function("sha256", host_sha256, NULL);
    mta_register_system_function("hmac_sha256", host_hmac_sha256, NULL);
    mta_register_system_function("random_bytes", host_random_bytes, NULL);
    mta_register_system_function("hex_to_bytes", host_hex_to_bytes, NULL);
    mta_register_system_function("bytes_to_hex", host_bytes_to_hex, NULL);
    mta_register_system_function("bytes_append", host_bytes_append, NULL);
    mta_register_system_function("bytes_append_int", host_bytes_append_int, NULL);
    mta_register_system_function("bytes_read_int", host_bytes_read_int, NULL);
    mta_register_system_function("bytes_clear", host_bytes_clear, NULL);
    mta_register_system_function("bytes_equal", host_bytes_equal, NULL);
    ESP_LOGD(TAG, "Byte helpers registered");
}
