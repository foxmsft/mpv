/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include <strings.h>
#include <assert.h>

#include <libavutil/common.h>
#include "osdep/io.h"

#include "mpv_talloc.h"

#include "config.h"

#include "common/common.h"
#include "common/global.h"
#include "misc/bstr.h"
#include "misc/thread_tools.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "options/options.h"
#include "options/path.h"
#include "osdep/timer.h"
#include "stream.h"

#include "options/m_option.h"
#include "options/m_config.h"

extern const stream_info_t stream_info_cdda;
extern const stream_info_t stream_info_dvb;
extern const stream_info_t stream_info_smb;
extern const stream_info_t stream_info_null;
extern const stream_info_t stream_info_memory;
extern const stream_info_t stream_info_mf;
extern const stream_info_t stream_info_ffmpeg;
extern const stream_info_t stream_info_ffmpeg_unsafe;
extern const stream_info_t stream_info_avdevice;
extern const stream_info_t stream_info_file;
extern const stream_info_t stream_info_ifo_dvdnav;
extern const stream_info_t stream_info_dvdnav;
extern const stream_info_t stream_info_bdmv_dir;
extern const stream_info_t stream_info_bluray;
extern const stream_info_t stream_info_bdnav;
extern const stream_info_t stream_info_edl;
extern const stream_info_t stream_info_libarchive;
extern const stream_info_t stream_info_cb;

static const stream_info_t *const stream_list[] = {
#if HAVE_CDDA
    &stream_info_cdda,
#endif
    &stream_info_ffmpeg,
    &stream_info_ffmpeg_unsafe,
    &stream_info_avdevice,
#if HAVE_DVBIN
    &stream_info_dvb,
#endif
#if HAVE_LIBSMBCLIENT
    &stream_info_smb,
#endif
#if HAVE_DVDNAV
    &stream_info_ifo_dvdnav,
    &stream_info_dvdnav,
#endif
#if HAVE_LIBBLURAY
    &stream_info_bdmv_dir,
    &stream_info_bluray,
    &stream_info_bdnav,
#endif
#if HAVE_LIBARCHIVE
    &stream_info_libarchive,
#endif
    &stream_info_memory,
    &stream_info_null,
    &stream_info_mf,
    &stream_info_edl,
    &stream_info_file,
    &stream_info_cb,
    NULL
};

struct stream_opts {
    int64_t buffer_size;
};

#define OPT_BASE_STRUCT struct stream_opts

const struct m_sub_options stream_conf = {
    .opts = (const struct m_option[]){
        OPT_BYTE_SIZE("stream-buffer-size", buffer_size, 0,
                      STREAM_FIXED_BUFFER_SIZE, 512 * 1024 * 1024),
        {0}
    },
    .size = sizeof(struct stream_opts),
    .defaults = &(const struct stream_opts){
        .buffer_size = STREAM_FIXED_BUFFER_SIZE,
    },
};

// return -1 if not hex char
static int hex2dec(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return 10 + c - 'A';
    if (c >= 'a' && c <= 'f')
        return 10 + c - 'a';
    return -1;
}

// Replace escape sequences in an URL (or a part of an URL)
void mp_url_unescape_inplace(char *url)
{
    for (int len = strlen(url), i = 0, o = 0; i <= len;) {
        if ((url[i] != '%') || (i > len - 3)) {  // %NN can't start after len-3
            url[o++] = url[i++];
            continue;
        }

        int msd = hex2dec(url[i + 1]),
            lsd = hex2dec(url[i + 2]);

        if (msd >= 0 && lsd >= 0) {
            url[o++] = 16 * msd + lsd;
            i += 3;
        } else {
            url[o++] = url[i++];
            url[o++] = url[i++];
            url[o++] = url[i++];
        }
    }
}

static const char hex_digits[] = "0123456789ABCDEF";


static const char url_default_ok[] = "abcdefghijklmnopqrstuvwxyz"
                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                     "0123456789"
                                     "-._~";

// Escape according to http://tools.ietf.org/html/rfc3986#section-2.1
// Only unreserved characters are not escaped.
// The argument ok (if not NULL) is as follows:
//      ok[0] != '~': additional characters that are not escaped
//      ok[0] == '~': do not escape anything but these characters
//                    (can't override the unreserved characters, which are
//                     never escaped)
char *mp_url_escape(void *talloc_ctx, const char *url, const char *ok)
{
    char *rv = talloc_size(talloc_ctx, strlen(url) * 3 + 1);
    char *out = rv;
    bool negate = ok && ok[0] == '~';

    for (char c; (c = *url); url++) {
        bool as_is = negate ? !strchr(ok + 1, c)
                            : (strchr(url_default_ok, c) || (ok && strchr(ok, c)));
        if (as_is) {
            *out++ = c;
        } else {
            unsigned char v = c;
            *out++ = '%';
            *out++ = hex_digits[v / 16];
            *out++ = hex_digits[v % 16];
        }
    }

    *out = 0;
    return rv;
}

static const char *match_proto(const char *url, const char *proto)
{
    int l = strlen(proto);
    if (l > 0) {
        if (strncasecmp(url, proto, l) == 0 && strncmp("://", url + l, 3) == 0)
            return url + l + 3;
    } else if (!mp_is_url(bstr0(url))) {
        return url; // pure filenames
    }
    return NULL;
}

// Read len bytes from the start position, and wrap around as needed. Limit the
// a actually read data to the size of the buffer. Return amount of copied bytes.
//  len: max bytes to copy to dst
//  pos: index into s->buffer[], e.g. s->buf_start is byte 0
//  returns: bytes copied to dst (limited by len and available buffered data)
static int ring_copy(struct stream *s, void *dst, int len, int pos)
{
    if (pos < s->buf_start || pos > s->buf_end)
        return 0;

    int avail = s->buf_end - pos;
    int copied = 0;
    len = MPMIN(len, avail);

    if (len && pos <= s->buffer_mask) {
        int copy = MPMIN(len, s->buffer_mask + 1 - pos);
        memcpy(dst, &s->buffer[pos], copy);
        copied += copy;
        len -= copy;
        pos += copy;
    }

    if (len) {
        memcpy((char *)dst + copied, &s->buffer[pos & s->buffer_mask], len);
        copied += len;
    }

    return copied;
}

// Return 0 if none found.
// round_to_next_power_of_2(65) == 128
// round_to_next_power_of_2(64) == 64
// round_to_next_power_of_2(0 or negative) == 1
static int round_to_next_power_of_2(int v)
{
    for (int n = 0; n < 30; n++) {
        if ((1 << n) >= v)
            return 1 << n;
    }
    return 0;
}

// Resize the current stream buffer. Uses a larger size if needed to keep data.
// Does nothing if the size is adequate. Calling this with 0 ensures it uses the
// default buffer size if possible.
// The caller must check whether enough data was really allocated.
// Returns false if buffer allocation failed.
static bool stream_resize_buffer(struct stream *s, int new)
{
    // Keep all valid buffer.
    int old_used_len = s->buf_end - s->buf_start;
    int old_pos = s->buf_cur - s->buf_start;
    new = MPMAX(new, old_used_len);

    new = MPMAX(new, s->requested_buffer_size);

    // This much is always required.
    new = MPMAX(new, STREAM_FIXED_BUFFER_SIZE);

    new = round_to_next_power_of_2(new);
    if (!new)
        return false;

    if (new == s->buffer_mask + 1)
        return true;

    MP_DBG(s, "resize stream to %d bytes\n", new);

    uint8_t *nbuf = s->buffer_inline;
    if (new > STREAM_FIXED_BUFFER_SIZE) {
        nbuf = ta_alloc_size(s, new);
    } else {
        static_assert(MP_IS_POWER_OF_2(STREAM_FIXED_BUFFER_SIZE), "");
        assert(new == STREAM_FIXED_BUFFER_SIZE);
    }
    assert(nbuf != s->buffer);

    if (!nbuf)
        return false; // oom; tolerate it, caller needs to check if required

    int new_len = 0;
    if (s->buffer)
        new_len = ring_copy(s, nbuf, new, s->buf_start);
    assert(new_len == old_used_len);
    assert(old_pos <= old_used_len);
    s->buf_start = 0;
    s->buf_cur = old_pos;
    s->buf_end = new_len;

    if (s->buffer != s->buffer_inline)
        ta_free(s->buffer);

    s->buffer = nbuf;
    s->buffer_mask = new - 1;

    return true;
}

static int stream_create_instance(const stream_info_t *sinfo,
                                  struct stream_open_args *args,
                                  struct stream **ret)
{
    const char *url = args->url;
    int flags = args->flags;

    *ret = NULL;

    if (!sinfo->is_safe && (flags & STREAM_SAFE_ONLY))
        return STREAM_UNSAFE;
    if (!sinfo->is_network && (flags & STREAM_NETWORK_ONLY))
        return STREAM_UNSAFE;

    const char *path = url;
    for (int n = 0; sinfo->protocols && sinfo->protocols[n]; n++) {
        path = match_proto(url, sinfo->protocols[n]);
        if (path)
            break;
    }

    if (!path)
        return STREAM_NO_MATCH;

    struct stream_opts *opts =
        mp_get_config_group(NULL, args->global, &stream_conf);

    stream_t *s = talloc_zero(NULL, stream_t);
    s->global = args->global;
    if (flags & STREAM_SILENT) {
        s->log = mp_null_log;
    } else {
        s->log = mp_log_new(s, s->global->log, sinfo->name);
    }
    s->info = sinfo;
    s->cancel = args->cancel;
    s->url = talloc_strdup(s, url);
    s->path = talloc_strdup(s, path);
    s->is_network = sinfo->is_network;
    s->mode = flags & (STREAM_READ | STREAM_WRITE);
    s->requested_buffer_size = opts->buffer_size;

    int opt;
    mp_read_option_raw(s->global, "access-references", &m_option_type_flag, &opt);
    s->access_references = opt;

    talloc_free(opts);

    MP_VERBOSE(s, "Opening %s\n", url);

    if (strlen(url) > INT_MAX / 8) {
        MP_ERR(s, "URL too large.\n");
        talloc_free(s);
        return STREAM_ERROR;
    }

    if ((s->mode & STREAM_WRITE) && !sinfo->can_write) {
        MP_DBG(s, "No write access implemented.\n");
        talloc_free(s);
        return STREAM_NO_MATCH;
    }

    int r = STREAM_UNSUPPORTED;
    if (sinfo->open2) {
        r = sinfo->open2(s, args);
    } else if (!args->special_arg) {
        r = (sinfo->open)(s);
    }
    if (r != STREAM_OK) {
        talloc_free(s);
        return r;
    }

    if (!s->read_chunk)
        s->read_chunk = 4 * STREAM_BUFFER_SIZE;

    if (!stream_resize_buffer(s, 0)) {
        free_stream(s);
        return STREAM_ERROR;
    }

    assert(s->seekable == !!s->seek);

    if (s->mime_type)
        MP_VERBOSE(s, "Mime-type: '%s'\n", s->mime_type);

    MP_DBG(s, "Stream opened successfully.\n");

    *ret = s;
    return STREAM_OK;
}

int stream_create_with_args(struct stream_open_args *args, struct stream **ret)

{
    assert(args->url);

    int r = STREAM_NO_MATCH;
    *ret = NULL;

    // Open stream proper
    if (args->sinfo) {
        r = stream_create_instance(args->sinfo, args, ret);
    } else {
        for (int i = 0; stream_list[i]; i++) {
            r = stream_create_instance(stream_list[i], args, ret);
            if (r == STREAM_OK)
                break;
            if (r == STREAM_NO_MATCH || r == STREAM_UNSUPPORTED)
                continue;
            if (r == STREAM_UNSAFE)
                continue;
            break;
        }
    }

    if (!*ret && !(args->flags & STREAM_SILENT) && !mp_cancel_test(args->cancel))
    {
        struct mp_log *log = mp_log_new(NULL, args->global->log, "!stream");

        if (r == STREAM_UNSAFE) {
            mp_err(log, "\nRefusing to load potentially unsafe URL from a playlist.\n"
                   "Use --playlist=file or the --load-unsafe-playlists option to "
                   "load it anyway.\n\n");
        } else if (r == STREAM_NO_MATCH || r == STREAM_UNSUPPORTED) {
            mp_err(log, "No protocol handler found to open URL %s\n", args->url);
            mp_err(log, "The protocol is either unsupported, or was disabled "
                        "at compile-time.\n");
        } else {
            mp_err(log, "Failed to open %s.\n", args->url);
        }

        talloc_free(log);
    }

    return r;
}

struct stream *stream_create(const char *url, int flags,
                             struct mp_cancel *c, struct mpv_global *global)
{
    struct stream_open_args args = {
        .global = global,
        .cancel = c,
        .flags = flags,
        .url = url,
    };
    struct stream *s;
    stream_create_with_args(&args, &s);
    return s;
}

struct stream *stream_open(const char *filename, struct mpv_global *global)
{
    return stream_create(filename, STREAM_READ, NULL, global);
}

stream_t *open_output_stream(const char *filename, struct mpv_global *global)
{
    return stream_create(filename, STREAM_WRITE, NULL, global);
}

// Read function bypassing the local stream buffer. This will not write into
// s->buffer, but into buf[0..len] instead.
// Returns 0 on error or EOF, and length of bytes read on success.
// Partial reads are possible, even if EOF is not reached.
static int stream_read_unbuffered(stream_t *s, void *buf, int len)
{
    assert(len >= 0);
    if (len <= 0)
        return 0;

    int res = 0;
    // we will retry even if we already reached EOF previously.
    if (s->fill_buffer && !mp_cancel_test(s->cancel))
        res = s->fill_buffer(s, buf, len);
    if (res <= 0) {
        s->eof = 1;
        return 0;
    }
    // When reading succeeded we are obviously not at eof.
    s->eof = 0;
    s->pos += res;
    s->total_unbuffered_read_bytes += res;
    return res;
}

// Ask for having at most "forward" bytes ready to read in the buffer.
// To read everything, you may have to call this in a loop.
// keep_old==false still keeps _some_ data for seek back.
//  forward: desired amount of bytes in buffer after s->cur_pos
//  keep_old: if true, keep all old data (s->buf_start .. s->buf_cur)
//  returns: progress (false on EOF or memory allocation failure)
static bool stream_read_more(struct stream *s, int forward, bool keep_old)
{
    assert(forward >= 0);

    int avail = s->buf_end - s->buf_cur;
    if (avail < forward) {
        // Avoid that many small reads will lead to many low-level read calls.
        forward = MPMAX(forward, s->requested_buffer_size / 2);

        int buf_old = s->buf_cur - s->buf_start;
        // Keep guaranteed seek-back.
        if (!keep_old)
            buf_old = MPMIN(s->requested_buffer_size / 2, buf_old);

        if (!stream_resize_buffer(s, buf_old + forward))
            return false;

        int buf_alloc = s->buffer_mask + 1;

        assert(s->buf_start <= s->buf_cur);
        assert(s->buf_cur <= s->buf_end);
        assert(s->buf_cur < buf_alloc * 2);
        assert(s->buf_end < buf_alloc * 2);
        assert(s->buf_start < buf_alloc);

        // Note: read as much as possible, even if forward is much smaller. Do
        // this because the stream buffer is supposed to set an approx. minimum
        // read size on it.
        int read = buf_alloc - buf_old - avail; // free buffer past end

        int pos = s->buf_end & s->buffer_mask;
        read = MPMIN(read, buf_alloc - pos);

        // Note: if wrap-around happens, we need to make two calls. This may
        // affect latency (e.g. waiting for new data on a socket), so do only
        // 1 read call always.
        read = stream_read_unbuffered(s, &s->buffer[pos], read);

        s->buf_end += read;

        // May have overwritten old data.
        if (s->buf_end - s->buf_start >= buf_alloc) {
            assert(s->buf_end >= buf_alloc);

            s->buf_start = s->buf_end - buf_alloc;

            assert(s->buf_start <= s->buf_cur);
            assert(s->buf_cur <= s->buf_end);

            if (s->buf_start >= buf_alloc) {
                s->buf_start -= buf_alloc;
                s->buf_cur -= buf_alloc;
                s->buf_end -= buf_alloc;
            }
        }

        // Must not have overwritten guaranteed old data.
        assert(s->buf_cur - s->buf_start >= buf_old);

        if (s->buf_end > s->buf_cur)
            s->eof = 0;

        return !!read;
    }

    return true;
}

// Read between 1..buf_size bytes of data, return how much data has been read.
// Return 0 on EOF, error, or if buf_size was 0.
int stream_read_partial(stream_t *s, char *buf, int buf_size)
{
    assert(s->buf_cur <= s->buf_end);
    assert(buf_size >= 0);
    if (s->buf_cur == s->buf_end && buf_size > 0) {
        if (buf_size > (s->buffer_mask + 1) / 2) {
            // Direct read if the buffer is too small anyway.
            stream_drop_buffers(s);
            return stream_read_unbuffered(s, buf, buf_size);
        }
        stream_read_more(s, 1, false);
    }
    int res = ring_copy(s, buf, buf_size, s->buf_cur);
    s->buf_cur += res;
    return res;
}

int stream_read(stream_t *s, char *mem, int total)
{
    int len = total;
    while (len > 0) {
        int read = stream_read_partial(s, mem, len);
        if (read <= 0)
            break; // EOF
        mem += read;
        len -= read;
    }
    total -= len;
    if (total > 0)
        s->eof = 0;
    return total;
}

int stream_read_char_fallback(stream_t *s)
{
    uint8_t c;
    return stream_read(s, &c, 1) ? c : -256;
}

static void get_buffers(struct stream *s, bstr *a, bstr *b)
{
    int len = s->buf_end - s->buf_start;

    a->start = &s->buffer[s->buf_cur & s->buffer_mask];
    a->len = MPMIN(len, s->buffer_mask + 1 - (s->buf_cur & s->buffer_mask));

    b->start = &s->buffer[0];
    b->len = len - a->len;

    // For convenience, so *a has always valid data,
    if (a->len == 0)
        MPSWAP(bstr, *a, *b);
}

// Read ahead at most len bytes without changing the read position. Return a
// pointer to the internal buffer, starting from the current read position.
// Reading ahead may require memory allocation. If allocation fails, read ahead
// is silently limited to the last successful allocation.
// The returned buffer becomes invalid on the next stream call, and you must
// not write to it.
struct bstr stream_peek(stream_t *s, int len)
{
    assert(len >= 0);

    while (s->buf_end - s->buf_cur < len) {
        if (!stream_read_more(s, len, true))
            break;
    }

    struct bstr a, b;
    get_buffers(s, &a, &b);

    // stream_peek() was written when this was _not_ a ringbuffer that could
    // wrap around. Since this function is now only used by demuxers which need
    // to probe headers when opening a stream, this function is now defined to
    // be used only at the start of a stream.
    if (b.len)
        MP_WARN(s, "Using stream_peek() in the middle of a stream.\n");

    a.len = MPMIN(a.len, len);

    return a;
}

// Peek the current buffer. This will return at least 1 byte, unless EOF was
// reached. If data is returned, the length is essentially random.
struct bstr stream_peek_buffer(stream_t *s)
{
    stream_read_more(s, 1, false);
    bstr a, b;
    get_buffers(s, &a, &b);
    return a;
}

int stream_write_buffer(stream_t *s, unsigned char *buf, int len)
{
    if (!s->write_buffer)
        return -1;
    int orig_len = len;
    while (len) {
        int w = s->write_buffer(s, buf, len);
        if (w <= 0)
            return -1;
        s->pos += w;
        buf += w;
        len -= w;
    }
    return orig_len;
}

// Drop len bytes form input, possibly reading more until all is skipped. If
// EOF or an error was encountered before all could be skipped, return false,
// otherwise return true.
static bool stream_skip_read(struct stream *s, int64_t len)
{
    while (len > 0) {
        unsigned int left = s->buf_end - s->buf_cur;
        if (!left) {
            if (!stream_read_more(s, 1, false))
                return false;
            continue;
        }
        unsigned skip = MPMIN(len, left);
        s->buf_cur += skip;
        len -= skip;
    }
    return true;
}

// Drop the internal buffer. Note that this will advance the stream position
// (as seen by stream_tell()), because the real stream position is ahead of the
// logical stream position by the amount of buffered but not yet read data.
void stream_drop_buffers(stream_t *s)
{
    s->pos = stream_tell(s);
    s->buf_start = s->buf_cur = s->buf_end = 0;
    s->eof = 0;
    stream_resize_buffer(s, 0);
}

// Seek function bypassing the local stream buffer.
static bool stream_seek_unbuffered(stream_t *s, int64_t newpos)
{
    if (newpos != s->pos) {
        MP_VERBOSE(s, "stream level seek from %" PRId64 " to %" PRId64 "\n",
                   s->pos, newpos);

        s->total_stream_seeks++;

        if (newpos > s->pos && !s->seekable) {
            MP_ERR(s, "Cannot seek forward in this stream\n");
            return false;
        }
        if (newpos < s->pos && !s->seekable) {
            MP_ERR(s, "Cannot seek backward in linear streams!\n");
            return false;
        }
        if (s->seek(s, newpos) <= 0) {
            int level = mp_cancel_test(s->cancel) ? MSGL_V : MSGL_ERR;
            MP_MSG(s, level, "Seek failed (to %lld, size %lld)\n",
                   (long long)newpos, (long long)stream_get_size(s));
            return false;
        }
        stream_drop_buffers(s);
        s->pos = newpos;
    }
    return true;
}

bool stream_seek(stream_t *s, int64_t pos)
{
    MP_DBG(s, "seek request from %" PRId64 " to %" PRId64 "\n", s->pos, pos);

    s->eof = 0; // eof should be set only on read; seeking always clears it

    if (pos < 0) {
        MP_ERR(s, "Invalid seek to negative position %lld!\n", (long long)pos);
        pos = 0;
    }

    if (pos == stream_tell(s))
        return true;

    if (pos < s->pos) {
        int64_t x = pos - (s->pos - (int)s->buf_end);
        if (x >= s->buf_start) {
            s->buf_cur = x;
            assert(s->buf_cur >= s->buf_start);
            assert(s->buf_cur <= s->buf_end);
            return true;
        }
    }

    if (s->mode == STREAM_WRITE)
        return s->seekable && s->seek(s, pos);

    if (pos >= s->pos &&
            ((!s->seekable && s->fast_skip) ||
             pos - s->pos <= s->requested_buffer_size))
    {
        // skipping is handled by generic code below
    } else if (!stream_seek_unbuffered(s, pos)) {
        return false;
    }

    return stream_skip_read(s, pos - stream_tell(s));
}

bool stream_skip(stream_t *s, int64_t len)
{
    int64_t target = stream_tell(s) + len;
    if (len < 0)
        return stream_seek(s, target);
    if (len > s->requested_buffer_size && s->seekable) {
        // Seek to 1 byte before target - this is the only way to distinguish
        // skip-to-EOF and skip-past-EOF in general. Successful seeking means
        // absolutely nothing, so test by doing a real read of the last byte.
        if (!stream_seek(s, target - 1))
            return false;
        stream_read_char(s);
        return !stream_eof(s) && stream_tell(s) == target;
    }
    return stream_skip_read(s, len);
}

int stream_control(stream_t *s, int cmd, void *arg)
{
    return s->control ? s->control(s, cmd, arg) : STREAM_UNSUPPORTED;
}

// Return the current size of the stream, or a negative value if unknown.
int64_t stream_get_size(stream_t *s)
{
    int64_t size = -1;
    if (stream_control(s, STREAM_CTRL_GET_SIZE, &size) != STREAM_OK)
        size = -1;
    return size;
}

void free_stream(stream_t *s)
{
    if (!s)
        return;

    if (s->close)
        s->close(s);
    talloc_free(s);
}

static const char *const bom[3] = {"\xEF\xBB\xBF", "\xFF\xFE", "\xFE\xFF"};

// Return utf16 argument for stream_read_line
int stream_skip_bom(struct stream *s)
{
    bstr data = stream_peek(s, 4);
    for (int n = 0; n < 3; n++) {
        if (bstr_startswith0(data, bom[n])) {
            stream_skip(s, strlen(bom[n]));
            return n;
        }
    }
    return -1; // default to 8 bit codepages
}

// Read the rest of the stream into memory (current pos to EOF), and return it.
//  talloc_ctx: used as talloc parent for the returned allocation
//  max_size: must be set to >0. If the file is larger than that, it is treated
//            as error. This is a minor robustness measure.
//  returns: stream contents, or .start/.len set to NULL on error
// If the file was empty, but no error happened, .start will be non-NULL and
// .len will be 0.
// For convenience, the returned buffer is padded with a 0 byte. The padding
// is not included in the returned length.
struct bstr stream_read_complete(struct stream *s, void *talloc_ctx,
                                 int max_size)
{
    if (max_size > 1000000000)
        abort();

    int bufsize;
    int total_read = 0;
    int padding = 1;
    char *buf = NULL;
    int64_t size = stream_get_size(s) - stream_tell(s);
    if (size > max_size)
        return (struct bstr){NULL, 0};
    if (size > 0)
        bufsize = size + padding;
    else
        bufsize = 1000;
    while (1) {
        buf = talloc_realloc_size(talloc_ctx, buf, bufsize);
        int readsize = stream_read(s, buf + total_read, bufsize - total_read);
        total_read += readsize;
        if (total_read < bufsize)
            break;
        if (bufsize > max_size) {
            talloc_free(buf);
            return (struct bstr){NULL, 0};
        }
        bufsize = FFMIN(bufsize + (bufsize >> 1), max_size + padding);
    }
    buf = talloc_realloc_size(talloc_ctx, buf, total_read + padding);
    memset(&buf[total_read], 0, padding);
    return (struct bstr){buf, total_read};
}

struct bstr stream_read_file(const char *filename, void *talloc_ctx,
                             struct mpv_global *global, int max_size)
{
    struct bstr res = {0};
    char *fname = mp_get_user_path(NULL, global, filename);
    stream_t *s = stream_open(fname, global);
    if (s) {
        res = stream_read_complete(s, talloc_ctx, max_size);
        free_stream(s);
    }
    talloc_free(fname);
    return res;
}

char **stream_get_proto_list(void)
{
    char **list = NULL;
    int num = 0;
    for (int i = 0; stream_list[i]; i++) {
        const stream_info_t *stream_info = stream_list[i];

        if (!stream_info->protocols)
            continue;

        for (int j = 0; stream_info->protocols[j]; j++) {
            if (*stream_info->protocols[j] == '\0')
               continue;

            MP_TARRAY_APPEND(NULL, list, num,
                                talloc_strdup(NULL, stream_info->protocols[j]));
        }
    }
    MP_TARRAY_APPEND(NULL, list, num, NULL);
    return list;
}

void stream_print_proto_list(struct mp_log *log)
{
    int count = 0;

    mp_info(log, "Protocols:\n\n");
    char **list = stream_get_proto_list();
    for (int i = 0; list[i]; i++) {
        mp_info(log, " %s://\n", list[i]);
        count++;
        talloc_free(list[i]);
    }
    talloc_free(list);
    mp_info(log, "\nTotal: %d protocols\n", count);
}

bool stream_has_proto(const char *proto)
{
    for (int i = 0; stream_list[i]; i++) {
        const stream_info_t *stream_info = stream_list[i];

        for (int j = 0; stream_info->protocols && stream_info->protocols[j]; j++) {
            if (strcmp(stream_info->protocols[j], proto) == 0)
                return true;
        }
    }

    return false;
}
