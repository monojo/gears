/* Copyright 2008, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. Neither the name of Google Inc. nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"
#include "http_connection.h"

#include "apr_lib.h"
#include "apr_strings.h"

#include <stdio.h>

module AP_MODULE_DECLARE_DATA resume_module;

typedef enum {
    STREAM,
    HOLD_AND_FORWARD
} streaming_mode;

/* from mod_disk_cache */
typedef struct {
    const char* cache_root;

    apr_time_t expiration;
    apr_size_t min_checkpoint;
    apr_size_t min_filesize;
    apr_size_t max_filesize;

    streaming_mode initial_mode;
    streaming_mode resume_mode;
} resume_config;

#define DEFAULT_MIN_CHECKPOINT 1024*1024
#define DEFAULT_MAX_FILESIZE 1024*1024*1024
#define DEFAULT_EXPIRATION 60*60*24
#define DEFAULT_INITIAL_MODE STREAM
#define DEFAULT_RESUME_MODE HOLD_AND_FORWARD

typedef struct {
    int input_initialized; /* has input initialization happened yet? */
    int expecting_103; /* was an Expect: 103-continue header received? */
    const char *etag; /* the resume ETag in the Expect header */

    /* derived from the bytes parameter in the Expect header */
    apr_off_t offset;
    apr_off_t expected_length; /* UNKNOWN_OFFSET if not specified */
    apr_off_t instance_length; /* UNKNOWN_OFFSET if '*' */

    /* info about the cache file we are writing */
    char *filename;
    apr_file_t *fd;
    apr_time_t mtime;
    apr_off_t file_size;

    apr_off_t max_filesize; /* instance_length or conf->max_filesize */
    apr_off_t received_length; /* number of bytes actually received */
    apr_off_t skip; /* redundant bytes in the incoming stream to be skipped */
    apr_off_t unacked; /* number of bytes we haven't acked yet */

    streaming_mode mode; /* HOLD_AND_FORWARD or STREAM mode */
    apr_bucket_brigade *bb; /* only use for checkpoints */
} resume_request_rec;

#define ETAG_LENGTH 6
const apr_off_t UNKNOWN_OFFSET = (apr_off_t)-1;

/* Handles for resume filters, resolved at startup to eliminate a
 * name-to-function mapping on each request.
 */
static ap_filter_rec_t *resume_input_filter_handle;
static ap_filter_rec_t *resume_save_filter_handle;
static ap_filter_rec_t *resume_out_filter_handle;

/********************************************************************************
 * Shared methods
 *******************************************************************************/

static int validate_etag(const char *etag)
{
    int i;
    if (!etag) {
        return 0;
    }
    for (i = 0; i < ETAG_LENGTH; i++) {
        if (!apr_isalnum(etag[i])) {
            return 0;
        }
    }
    return (etag[ETAG_LENGTH] == '\0');
}

/********************************************************************************
 * Handler methods
 *******************************************************************************/

static int parse_expect(request_rec *r, resume_request_rec *ctx,
                        resume_config *conf)
{
    /* TODO(fry):
     * const char *expect = apr_table_get(r->headers_in, "Expect");
     */
    const char *expect = apr_table_get(r->headers_in, "Pragma");

    if (!expect || !*expect) {
        /* Ignore absense of expect header */
        return 1;
    }

    /* from mod_negotiation */
    const char *name = ap_get_token(r->pool, &expect, 0);
    if (!name || strcasecmp(name, "103-checkpoint")) {
        /* Ignore expects that aren't for us */
        return 1;
    }
    ctx->expecting_103 = 1;
    ctx->expected_length = 0;

    while (*expect++ == ';') {
        /* Parameters ... */
        char *parm;
        char *cp;
        char *end;
        apr_off_t last_byte_pos;

        parm = ap_get_token(r->pool, &expect, 1);

        /* Look for 'var = value' */

        for (cp = parm; (*cp && !apr_isspace(*cp) && *cp != '='); ++cp);

        if (!*cp) {
            continue;           /* No '='; just ignore it. */
        }

        *cp++ = '\0';           /* Delimit var */
        while (*cp && (apr_isspace(*cp) || *cp == '=')) {
            ++cp;
        }

        if (*cp == '"') {
            ++cp;
            for (end = cp;
                 (*end && *end != '\n' && *end != '\r' && *end != '\"');
                 end++);
        }
        else {
            for (end = cp; (*end && !apr_isspace(*end)); end++);
        }
        if (*end) {
            *end = '\0';        /* strip ending quote or return */
        }

        if (!strcmp(parm, "resume")) {
            if (!validate_etag(cp)) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "resume: Invalid etag \"%s\"", ctx->etag);
                return 0;
            }
            ctx->etag = cp;
        }
        else if (!strcmp(parm, "bytes")) {
            /* from byterange_filter */
            ctx->mode = conf->resume_mode;
            /* must have first_byte_pos (which is inclusive) */
            if (apr_strtoff(&ctx->offset, cp, &end, 10) || *end != '-') {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "resume: Invalid first_byte_pos \"%s\"", cp);
                return 0;
            }
            cp = end + 1;

            /* may have last_byte_pos (which is inclusive) */
            if (*cp == '/') {
                last_byte_pos = UNKNOWN_OFFSET;
            }
            else if (apr_strtoff(&last_byte_pos, cp, &end, 10) || *end != '/') {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "resume: Invalid last_byte_pos \"%s\"", cp);
                return 0;
            }
            cp = end + 1;

            /* must have either instance_length or "*" */
            if (*cp == '*' && *(cp + 1) == '\0') {
                ctx->instance_length = UNKNOWN_OFFSET; /* currently redundant */
            }
            else if (apr_strtoff(&ctx->instance_length, cp, &end, 10) || *end) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "resume: Invalid instance_length \"%s\"", cp);
                return 0;
            }

            /* syntactic checks of byte ranges */

            /* ctx->offset */
            if (ctx->offset < 0) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "resume: Invalid first_byte_pos \"%s\"", cp);
                return 0;
            }

            /* ctx->expected_length */
            if (last_byte_pos == UNKNOWN_OFFSET) {
                ctx->expected_length = UNKNOWN_OFFSET;
                if (!r->read_chunked) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                                 "resume: Content-Length specified "
                                 "without last_byte_pos \"%s\"", cp);
                    return 0;
                }
            }
            else if (ctx->offset > last_byte_pos
                     || last_byte_pos >= conf->max_filesize) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "resume: Invalid last_byte_pos \"%s\"", cp);
                return 0;
            }
            else if (r->read_chunked) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "resume: last_byte_pos specified "
                             "without Content-Length \"%s\"", cp);
                return 0;
            }
            else {
                /* convert from last_byte_pos to expected_length */
                ctx->expected_length = last_byte_pos - ctx->offset + 1;
            }

            /* ctx->instance_length */
            if (ctx->instance_length == UNKNOWN_OFFSET) {
                /* this space intentionally left blank */
            }
            else if (ctx->expected_length == UNKNOWN_OFFSET) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "resume: instance_length specified "
                             "without last_byte_pos \"%s\"", cp);
                return 0;
            }
            else if (ctx->instance_length > conf->max_filesize ||
                     ctx->offset + ctx->expected_length > ctx->instance_length)
            {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                             "resume: Invalid instance_length \"%s\"", cp);
                return 0;
            }
            else {
                ctx->max_filesize = ctx->instance_length;
            }
        }
    }

    /* cross-parameter validation */
    if (!ctx->etag && ctx->offset > 0) { /* implicit && ctx->expecting_103 */
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "resume: Non-contiguous first_byte_pos \"%s\"",
                     ctx->filename);
        return 0;
    }

    return 1;
}

static int send_cached_response(request_rec *r)
{
    apr_bucket_brigade *bb;
    apr_status_t rv;

    /* from mod_cache */
    ap_filter_t *next;

    /* Remove all filters that are before the resume_out filter. This
     * ensures that we kick off the filter stack with our resume_out
     * filter being the first in the chain. This make sense because we
     * want to restore things in the same manner as we saved them.
     * There may be filters before our resume_out filter, because:
     *     1. We call ap_set_content_type during cache_select. This
     *        causes Content-Type specific filters to be added.
     *     2. We call the insert_filter hook. This causes filters like
     *        the ones set with SetOutputFilter to be added.
     */
    next = r->output_filters;
    while (next && (next->frec != resume_out_filter_handle)) {
        ap_remove_output_filter(next);
        next = next->next;
    }

    /* kick off the filter stack */
    bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    rv = ap_pass_brigade(r->output_filters, bb);
    if (rv != APR_SUCCESS) {
        if (rv != AP_FILTER_ERROR) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, r->server,
                         "resume: error returned while trying to "
                         "return %s cached data", "TODO");
                         /* TODO(fry): cache->provider_name); */
        }
        return rv;
    }

    return OK;
}

static int send_418(request_rec *r)
{
    apr_bucket_brigade *bb;
    apr_status_t rv;
    int seen_eos;

    /* Partial request, necessitating a 418.
     *
     * It is _incorrect_ to allow downstream handlers to process this request as
     * they will incorrectly interpret the input EOS. So instead we read all of
     * the bytes ourself, ignoring them as the input filter is already storing
     * them.
     */

    /* from mod_cgi */
    bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    seen_eos = 0;
    do {
        apr_bucket *bucket;
        rv = ap_get_brigade(r->input_filters, bb, AP_MODE_READBYTES,
                            APR_BLOCK_READ, HUGE_STRING_LEN);
        if (rv != APR_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
                          "Error reading request entity data");
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        /* Search for EOS (even though we only expect a single bucket) */
        for (bucket = APR_BRIGADE_FIRST(bb);
             bucket != APR_BRIGADE_SENTINEL(bb);
             bucket = APR_BUCKET_NEXT(bucket))
        {
            if (APR_BUCKET_IS_EOS(bucket)) {
                seen_eos = 1;
                break;
            }
        }
        apr_brigade_cleanup(bb);
    } while (!seen_eos);

    return 418;
}

static int create_cache_file(request_rec *r, resume_config *conf,
                             resume_request_rec *ctx)
{
    apr_status_t rv;

    ctx->filename = apr_pstrcat(r->pool, conf->cache_root,
                                "/mod_resume.XXXXXX", NULL);
    ctx->file_size = 0;
    ctx->mtime = apr_time_now();
    rv = apr_file_mktemp(&ctx->fd, ctx->filename,
                         APR_CREATE | APR_EXCL
                         | APR_READ | APR_WRITE | APR_APPEND
                         | APR_BINARY | APR_BUFFERED,
                         r->pool);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "resume: Error creating byte archive \"%s\"",
                     ctx->filename);
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    /* ETag is XXXXXX from filename. */
    /* TODO(fry): longer etag please!
     *            also cryptographically random?
     *            use etag_uint64_to_hex and http_etag
     */
    ctx->etag = ap_strrchr(ctx->filename, '.');
    if (!ctx->etag++ || !validate_etag(ctx->etag)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "resume: Invalid etag \"%s\"", ctx->etag);
        return HTTP_BAD_REQUEST;
    }
    return OK;
}

static int open_cache_file(request_rec *r, resume_config *conf,
                           resume_request_rec *ctx)
{
    apr_status_t rv;
    apr_finfo_t finfo;

    ctx->filename = apr_pstrcat(r->pool, conf->cache_root, "/mod_resume.",
                                ctx->etag, NULL);
    rv = apr_file_open(&ctx->fd, ctx->filename,
                       APR_READ | APR_WRITE | APR_APPEND
                       | APR_BINARY | APR_BUFFERED, 0, r->pool);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "resume: Error creating byte archive \"%s\"",
                     ctx->filename);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    rv = apr_file_info_get(&finfo, APR_FINFO_MTIME | APR_FINFO_SIZE, ctx->fd);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "resume: Error stating byte archive \"%s\"",
                     ctx->filename);
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    ctx->mtime = finfo.mtime;
    ctx->file_size = finfo.size;
    if (ctx->offset > ctx->file_size) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "resume: Non-contiguous first_byte_pos \"%s\"",
                     ctx->filename);
        return HTTP_BAD_REQUEST;
    }
    /* skip re-transmitted bytes */
    ctx->skip = ctx->file_size - ctx->offset;
    return OK;
}

/* This is the resume handler, in charge of installing all filters necessary
 * for this request.
 *
 * Note that this is very similar to mod_cache, and we only avoid conflicting
 * with them because mod_cache operates on GET commands, and we operate on
 * POST/PUT commands.
 */
static int resume_handler(request_rec *r)
{
    resume_config *conf;
    resume_request_rec *ctx;
    apr_status_t rv;

    /* Delay initialization until we know we are handling a POST/PUT */
    if (r->method_number != M_POST && r->method_number != M_PUT) {
        return DECLINED;
    }

    conf = ap_get_module_config(r->server->module_config, &resume_module);
    ctx = ap_get_module_config(r->request_config, &resume_module);
    if (!ctx) { /* TODO(fry): is this ever already set? */
        ctx = apr_pcalloc(r->pool, sizeof(resume_request_rec));
        ap_set_module_config(r->request_config, &resume_module, ctx);
    }

    ctx->input_initialized = 0;
    ctx->expecting_103 = 0;
    ctx->etag = NULL;
    ctx->offset = 0;
    ctx->expected_length = UNKNOWN_OFFSET;
    ctx->instance_length = UNKNOWN_OFFSET;
    ctx->max_filesize = conf->max_filesize;
    ctx->received_length = 0;
    ctx->skip = 0;
    ctx->unacked = 0;
    ctx->mode = conf->initial_mode;
    ctx->bb = NULL; /* delay instantiation */

    /* read Content-Length and Transfer-Encoding, but don't trust them
     * populates r->remaining, r->read_chunked */
    rv = ap_setup_client_block(r, REQUEST_CHUNKED_DECHUNK);
    if (rv != OK) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "resume: Error reading length and encoding \"%s\"",
                     ctx->filename);
        return rv;
    }
    if (!parse_expect(r, ctx, conf)) {
        return HTTP_BAD_REQUEST;
    }

    if (ctx->etag) {
        /* If a final response was already generated, return it again. */
        if (0 /* TODO(fry): we have a cached response */) {
            ap_add_output_filter_handle(resume_out_filter_handle, NULL, r,
                                        r->connection);
            return send_cached_response(r);
        }

        rv = open_cache_file(r, conf, ctx);
        if (rv != OK) {
            return rv;
        }
    }
    else {
        /* look for excuses not to enable resumability */
        if (!ctx->expecting_103 && !r->read_chunked
            && (r->remaining < conf->min_filesize ||
                r->remaining > conf->max_filesize))
        {
            /* Note that this is an optimistic decision made based on
             * Content-Length, which may be incorrect. See:
             * http://mail-archives.apache.org/mod_mbox/httpd-modules-dev/200802.mbox/%3c003701c86ff6$4f2b39d0$6501a8c0@T60%3e
             */
            return DECLINED;
        }

        rv = create_cache_file(r, conf, ctx);
        if (rv != OK) {
            return rv;
        }
    }

    /* TODO(fry): could use ap_set_module_config instead of passing ctx directly
     *       into handler if we wanted users to be able to use input filter
     *       without the handler.
     */
    ap_add_input_filter_handle(resume_input_filter_handle, ctx, r,
                               r->connection);
    if (!ctx->expecting_103
        || ctx->skip + ctx->expected_length == ctx->instance_length
        || (ctx->expected_length == UNKNOWN_OFFSET
            && ctx->instance_length == UNKNOWN_OFFSET)) {
        /* All bytes are present, no 418 required.
         * We expect a final response, so install the save output filter.
         */
        ap_add_output_filter_handle(resume_save_filter_handle, NULL, r,
                                    r->connection);
        return DECLINED;
    }

    /* no reason to stream as we aren't reading bytes */
    ctx->mode = HOLD_AND_FORWARD;
    return send_418(r);
}

/********************************************************************************
 * Input Filter methods
 *******************************************************************************/

/* TODO use this to generate etags */
/* from http_etag */
/* Generate the human-readable hex representation of an apr_uint64_t
 * (basically a faster version of 'sprintf("%llx")')
 */
#define HEX_DIGITS "0123456789abcdef"
static char *etag_uint64_to_hex(char *next, apr_uint64_t u)
{
    int printing = 0;
    int shift = sizeof(apr_uint64_t) * 8 - 4;
    do {
        unsigned short next_digit = (unsigned short)
                                    ((u >> shift) & (apr_uint64_t)0xf);
        if (next_digit) {
            *next++ = HEX_DIGITS[next_digit];
            printing = 1;
        }
        else if (printing) {
            *next++ = HEX_DIGITS[next_digit];
        }
        shift -= 4;
    } while (shift);
    *next++ = HEX_DIGITS[u & (apr_uint64_t)0xf];
    return next;
}

/* send 103 (Checkpoint) */
static void send_103(request_rec *r, resume_config *conf,
                     resume_request_rec *ctx)
{
    apr_bucket_brigade *bb = ctx->bb;
    ap_filter_t *of = r->connection->output_filters;

    /* flush the bytes which we will now ack */
    apr_file_flush(ctx->fd);

    /* Copied from ap_send_interim_response to avoid saving status. */
    ap_fputstrs(of, bb, AP_SERVER_PROTOCOL, " ", "103 Checkpoint", CRLF, NULL);
    ap_fputstrs(of, bb, "ETag: \"", ctx->etag, "\"", CRLF, NULL);

    if (ctx->file_size > 0) {
        ap_fprintf(of, bb, "Range: 0-%d", ctx->file_size - 1);
        ap_fputs(of, bb, CRLF);
    }
    if (conf->expiration > 0) {
        apr_time_t expires = ctx->mtime + conf->expiration;
        char *timestr = apr_palloc(r->pool, APR_RFC822_DATE_LEN);
        apr_rfc822_date(timestr, expires);
        ap_fputstrs(of, bb, "Expires: ", timestr, CRLF, NULL);
    }

    ap_fputs(of, bb, CRLF);
    ap_fflush(of, bb);
    ctx->unacked = 0;
}

static apr_status_t read_bytes(request_rec *r, apr_bucket_brigade *bb ,
                               resume_config *conf, resume_request_rec *ctx,
                               int *seen_eos)
{
    apr_bucket *bucket;
    apr_status_t rv;

    for (bucket = APR_BRIGADE_FIRST(bb);
         bucket != APR_BRIGADE_SENTINEL(bb);
         bucket = APR_BUCKET_NEXT(bucket))
    {
        const char *str;
        apr_size_t length;

        if (APR_BUCKET_IS_EOS(bucket)) {
            *seen_eos = 1;
            return APR_SUCCESS;
        }
        if (APR_BUCKET_IS_FLUSH(bucket)) {
            continue;
        }

        rv = apr_bucket_read(bucket, &str, &length, APR_BLOCK_READ);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "resume: Error reading bytes for \"%s\"",
                         ctx->etag);
            return rv;
        }
        /* received_length includes skipped bytes */
        ctx->received_length += length;
        if (ctx->expected_length != UNKNOWN_OFFSET
            && ctx->received_length > ctx->expected_length)
        {
            apr_file_close(ctx->fd);
            apr_file_remove(ctx->filename, r->pool);
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "resume: Received incorrect number of bytes for "
                         "\"%s\"", ctx->etag);
            return APR_EGENERAL;
        }

        if (ctx->skip > 0) {
            /* skip bytes we've already received */
            if (ctx->skip >= length) {
                ctx->skip -= length;
                continue;
            }

            str += ctx->skip;
            length -= ctx->skip;
            ctx->skip = 0;
        }
        ctx->mtime = apr_time_now();
        rv = apr_file_write_full(ctx->fd, str, length, NULL);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "resume: Error archiving bytes for \"%s\"",
                         ctx->etag);
            return rv;
        }
        ctx->unacked += length;
        ctx->file_size += length;
        if (ctx->file_size > ctx->max_filesize) {
            /* This is just too many bytes. We'd have noticed it earlier in
             * non-chunked mode, but now that we know we must disapprove. */
            apr_file_close(ctx->fd);
            apr_file_remove(ctx->filename, r->pool);
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "resume: Received too many bytes for \"%s\"",
                         ctx->etag);
            return APR_EGENERAL;
        }
        if (ctx->unacked >= conf->min_checkpoint) {
            send_103(r, conf, ctx);
        }
    }
    return APR_SUCCESS;
}

/* This is the resume filter, whose responsibilities are to:
 *  1) periodically send 100 (Continue) responses to the client
 *  2) persist the incoming byte stream in case of failure
 *  3) intercept resumes, injecting previous data
 */
static apr_status_t resume_input_filter(ap_filter_t *f,
                                        apr_bucket_brigade *bb,
                                        ap_input_mode_t mode,
                                        apr_read_type_e block,
                                        apr_off_t readbytes)
{
    resume_config *conf = ap_get_module_config(f->r->server->module_config,
                                               &resume_module);
    resume_request_rec *ctx = f->ctx;
    request_rec *r = f->r;
    apr_status_t rv;
    int seen_eos;

    /* just get out of the way of things we don't want. */
    if (mode != AP_MODE_READBYTES) {
        return ap_get_brigade(f->next, bb, mode, block, readbytes);
    }
    if (!ctx) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "resume: RESUME input filter enabled unexpectedly");
        ap_remove_input_filter(f);
        return ap_get_brigade(f->next, bb, mode, block, readbytes);
    }

    if (!ctx->input_initialized) {
        ctx->input_initialized = 1;
        ctx->bb = apr_brigade_create(r->pool, f->c->bucket_alloc);

        /* Always send a 103 as soon as we're committed to supporting
         * resumability.
         */
        send_103(r, conf, ctx);

        if (ctx->mode == STREAM && ctx->file_size > 0) {
            /* Stream any previous bytes in streaming mode */
            apr_bucket *bucket;
            bucket = apr_bucket_file_create(ctx->fd, 0,
                                            ctx->file_size,
                                            r->pool,
                                            bb->bucket_alloc);
            APR_BRIGADE_INSERT_TAIL(bb, bucket);
            /* delay reading more data until previous bytes are re-sent */
            return APR_SUCCESS;
        }
    }

    do {
        /* TODO(fry): get non-blocking
         *  if no data then 103 and get blocking
         *
         *  if not blocking then count bytes, otherwise always ack before
         *  blocking
         */

        /* Might read less than requested in order to ack progressively. */
        rv = ap_get_brigade(f->next, bb, mode, block,
                            (readbytes < conf->min_checkpoint ?
                             readbytes : conf->min_checkpoint));
        if (rv != APR_SUCCESS) {
            return rv;
        }

        rv = read_bytes(r, bb, conf, ctx, &seen_eos);
        if (rv != APR_SUCCESS) {
            return rv;
        }

        if (ctx->mode == HOLD_AND_FORWARD) {
            apr_brigade_cleanup(bb);
        }
        else if (!seen_eos) { /* STREAM mode */
            return APR_SUCCESS;
        }
    } while (!seen_eos);

    /* eos */
    if (ctx->unacked > 0) {
        /* ack any remaining bytes as the response might be slow in coming */
        send_103(r, conf, ctx);
    }

    if ((ctx->expected_length != UNKNOWN_OFFSET
         && ctx->received_length != ctx->expected_length)
        || (ctx->instance_length != UNKNOWN_OFFSET
            && ctx->file_size < ctx->instance_length))
        /* Already checked above for ctx->file_size > ctx->instance_length */
    {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                     "resume: Received insufficient bytes for \"%s\"",
                     ctx->etag);
        return APR_EGENERAL;
    }

    if (ctx->mode == HOLD_AND_FORWARD) {
        apr_bucket *bucket;
        apr_off_t offset = 0;
        apr_file_seek(ctx->fd, APR_SET, &offset);
        /* bb was just cleaned above */
        bucket = apr_bucket_file_create(ctx->fd, 0,
                                        (apr_size_t) ctx->file_size,
                                        r->pool, bb->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(bb, bucket);
        bucket = apr_bucket_eos_create(f->c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(bb, bucket);
    }
    return APR_SUCCESS;
}

/********************************************************************************
 * Output Filter methods
 *******************************************************************************/

/* from mod_cache */
/* Cache server response. */
static int resume_save_filter(ap_filter_t *f, apr_bucket_brigade *in)
{
    /* TODO(fry): import relevant bits from cache_save_filter */
    return ap_pass_brigade(f->next, in);
}

/* from mod_cache */
/* Deliver cached responses up the stack. */
static int resume_out_filter(ap_filter_t *f, apr_bucket_brigade *bb)
{
    /* TODO(fry): import relevant bits from cache_out_filter */
    /* TODO(fry): ensure that bb is empty */
    return ap_pass_brigade(f->next, bb);
}

/********************************************************************************
 * Configuration methods
 *******************************************************************************/

static int resume_post_config(apr_pool_t *p, apr_pool_t *plog,
                              apr_pool_t *ptemp, server_rec *s)
{
    /* TODO(fry): initial garbage collection of file and etag
     *            also perform regular cleanup
     */
    return OK;
}

static void *create_resume_config(apr_pool_t *p, server_rec *s)
{
    resume_config *conf = apr_pcalloc(p, sizeof(resume_config));

    /* set default values */
    conf->cache_root = NULL;
    conf->min_checkpoint = DEFAULT_MIN_CHECKPOINT;
    conf->min_filesize = DEFAULT_MIN_CHECKPOINT;
    conf->max_filesize = DEFAULT_MAX_FILESIZE;
    conf->expiration = apr_time_from_sec(DEFAULT_EXPIRATION);
    conf->initial_mode = DEFAULT_INITIAL_MODE;
    conf->resume_mode = DEFAULT_RESUME_MODE;

    return conf;
}

static const char *set_cache_root(cmd_parms *parms, void *in_struct_ptr,
                                  const char *root)
{
    resume_config *c = ap_get_module_config(parms->server->module_config,
                                             &resume_module);
    /* TODO(fry): check existence of root */
    c->cache_root = root;
    return NULL;
}


static const char *set_min_checkpoint(cmd_parms *parms, void *in_struct_ptr,
                                  const char *min)
{
    resume_config *conf = ap_get_module_config(parms->server->module_config,
                                             &resume_module);
    int n = atoi(min);
    if (n <= 0) {
        return "ResumeCheckpointBytes must be positive";
    }
    conf->min_checkpoint = n;
    return NULL;
}

static const char *set_min_filesize(cmd_parms *parms, void *in_struct_ptr,
                                    const char *min)
{
    resume_config *conf = ap_get_module_config(parms->server->module_config,
                                             &resume_module);
    int n = atoi(min);
    if (n <= 0) {
        return "ResumeMinBytes must be positive";
    }
    conf->min_filesize = n;
    return NULL;
}

static const char *set_max_filesize(cmd_parms *parms, void *in_struct_ptr,
                                    const char *max)
{
    resume_config *conf = ap_get_module_config(parms->server->module_config,
                                             &resume_module);
    int n = atoi(max);
    if (n <= 0) {
        return "ResumeMaxBytes must be positive";
    }
    conf->max_filesize = n;
    return NULL;
}

static const char *set_expiration(cmd_parms *parms, void *in_struct_ptr,
                                  const char *expiration)
{
    resume_config *conf = ap_get_module_config(parms->server->module_config,
                                             &resume_module);
    int n = atoi(expiration);
    conf->expiration = apr_time_from_sec(n);
    return NULL;
}

static int get_mode(const char *str, streaming_mode *mode)
{
    if (!strcasecmp(str, "STREAM")) {
        *mode = STREAM;
        return 1;
    }
    else if (!strcasecmp(str, "HOLD_AND_FORWARD")) {
        *mode = HOLD_AND_FORWARD;
        return 1;
    }
    return 0;
}

static const char *set_streaming(cmd_parms *parms, void *in_struct_ptr,
                                  const char *initial, const char *resume)
{
    resume_config *conf = ap_get_module_config(parms->server->module_config,
                                             &resume_module);

    if (!get_mode(initial, &conf->initial_mode)) {
        return "Invalid ResumeStreamingMode; must be one of: "
            "STREAM, HOLD_AND_FORWARD";
    }
    if (!get_mode(resume, &conf->resume_mode)) {
        return "Invalid ResumeStreamingMode; must be one of: "
            "STREAM, HOLD_AND_FORWARD";
    }

    return NULL;
}


static const command_rec resume_cmds[] =
{
    AP_INIT_TAKE1("ResumeCacheRoot", set_cache_root, NULL, RSRC_CONF,
                  "The directory to store resume cache files"),
    AP_INIT_TAKE1("ResumeCheckpointBytes", set_min_checkpoint, NULL, RSRC_CONF,
                  "The minimum number of bytes to read between checkpoints."),
    AP_INIT_TAKE1("ResumeMinBytes", set_min_filesize, NULL, RSRC_CONF,
                  "The minimum number of bytes for a request to be resumable."),
    AP_INIT_TAKE1("ResumeMaxBytes", set_max_filesize, NULL, RSRC_CONF,
                  "The maximum number of bytes in a resumable request."),
    AP_INIT_TAKE1("ResumeExpiration", set_expiration, NULL, RSRC_CONF,
                  "The number of seconds until a resumed operation expires."),
    AP_INIT_TAKE2("ResumeStreamingMode", set_streaming, NULL, RSRC_CONF,
                   "The mode for handling bytes as they arrive. "
                   "The first parameter specifies how the initial request "
                   "should be dealt with; the second parameter applies to "
                   "resume requests. Valid values for both parameters are: "
                   "STREAM, HOLD_AND_FORWARD. The default values are STREAM "
                   "for initial requests and HOLD_AND_FORWARD for resume "
                   "requests."),
    {NULL}
};

static void resume_register_hooks(apr_pool_t *p)
{
    /* from mod_cache */

    /* TODO(fry): should we use quick_handler like mod_cache?
     *            see http_config.h for quick_handler overview
     *            I don't currently think so since it optimizes for the
     *            uncommon case (all bytes transfered, but response not
     *            received), and adds complexity of needing to deal with an
     *            incomplete handler environment (e.g. no output filters set)
     */
    ap_hook_handler(resume_handler, NULL, NULL, APR_HOOK_FIRST);

    /*
     * Note that input filters are pull-based, and output filters are push-based.
     *
     * Handler <- InputFilter0  <- InputFilter1  <- Network
     * Handler -> OutputFilter0 -> OutputFilter1 -> Network
     *
     * However they are grouped in priority based on their proximity to the
     * handler, as defined in util_filter.h:
     *
     * Handler <-> AP_FTYPE_RESOURCE (10)   <-> AP_FTYPE_CONTENT_SET (20)
     *         <-> AP_FTYPE_PROTOCOL (30)   <-> AP_FTYPE_TRANSCODE (40)
     *         <-> AP_FTYPE_CONNECTION (50) <-> AP_FTYPE_NETWORK (60)
     */

    /* RESUME must go in the filter chain before a possible DEFLATE filter in
     * order in order to make resumes independent of compression. In other
     * words, we want our input filter to see bytes that have already been
     * inflated if mod_deflate's input filter is in the chain.
     * Decrementing filter type by 1 ensures this happens.
     */
    resume_input_filter_handle =
            ap_register_input_filter("RESUME", resume_input_filter, NULL,
                                     AP_FTYPE_CONTENT_SET-1);

    /* TODO(fry): Once our mod_cache dependencies are understood, it may be
     * necessary to use AP_FTYPE_CONTENT_SET+1, and the following comment:
     * RESUME_SAVE must go into the filter chain after a possible DEFLATE
     * filter to ensure that already compressed cache objects do not
     * get compressed again. In other words, we want our save output filter to
     * save bytes that have already run through mod_deflate's output filter if
     * it is in the chain.
     * Incrementing filter type by 1 ensures this happens.
     */
    resume_save_filter_handle =
            ap_register_output_filter("RESUME_SAVE", resume_save_filter, NULL,
                                      AP_FTYPE_CONTENT_SET);

    /* RESUME_OUT must be at the same priority in the filter chain as
     * RESUME_SAVE in order to ensure that the replayed output is the identical
     * to the original output.
     */
    resume_out_filter_handle =
            ap_register_output_filter("RESUME_OUT", resume_out_filter, NULL,
                                      AP_FTYPE_CONTENT_SET);

    ap_hook_post_config(resume_post_config, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA resume_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,                  /* per-directory config creator */
    NULL,                  /* dir config merger -- default is to override */
    create_resume_config,  /* server config creator */
    NULL,                  /* server config merger */
    resume_cmds,           /* command table */
    resume_register_hooks, /* set up other request processing hooks */
};