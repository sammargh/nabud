/*-
 * Copyright (c) 2022, 2023 Jason R. Thorpe.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR  
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES  
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, 
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Support for the NABU HCCA Application Communication Protocol.
 *
 *    https://github.com/hanshuebner/nabu-figforth/blob/main/nabu-comms.md
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define	NABU_PROTO_INLINES

#include "libnabud/fileio.h"
#include "libnabud/log.h"
#include "libnabud/missing.h"
#include "libnabud/nabu_proto.h"
#include "libnabud/nhacp_proto.h"
#include "libnabud/nbsd_queue.h"

#include "conn.h"
#include "nhacp.h"

/* 1MB limit on shadow file length. */
#define	MAX_SHADOW_LENGTH	(1U * 1024 * 1024)

/* 32-bit limit on fileio file length (due to wire protocol). */
#define	MAX_FILEIO_LENGTH	UINT32_MAX

struct stext_context {
	struct nabu_connection *conn;
	LIST_HEAD(, stext_file) files;
};

struct nhacp_context {
	struct stext_context stext;

	union {
		struct nhacp_request request;
		struct nhacp_response reply;
	};
};

struct stext_file {
	LIST_ENTRY(stext_file) link;
	uint8_t		slot;
	const struct stext_fileops *ops;

	union {
		struct {
			struct fileio	*fileio;
		} fileio;
		struct {
			uint8_t		*data;
			size_t		length;
		} shadow;
	};
};

struct stext_fileops {
	int	(*file_read)(struct stext_file *, void *, uint32_t, uint16_t *);
	int	(*file_write)(struct stext_file *, const void *, uint32_t,
		    uint16_t);
	void	(*file_close)(struct stext_file *);
};

/*
 * The messages that contain a variable-sized data payload need to
 * sanity check that payload's length against how much of the max
 * payload was consumed by the protocol message itself.
 */
#define	MAX_STORAGE_GET_LENGTH	(NHACP_MAX_MESSAGELEN -			\
				 sizeof(struct nhacp_response_data_buffer))
#define	MAX_STORAGE_PUT_LENGTH	(NHACP_MAX_MESSAGELEN -			\
				 sizeof(struct nhacp_request_storage_put))

/*
 * stext_file_insert --
 *	Insert a file into the list, allocating a slot number if
 *	necessary.  This always succeeds, and returns the old
 *	file object that needs to be freed if there's a collision.
 */
static bool
stext_file_insert(struct stext_context *ctx, struct stext_file *f,
    uint8_t reqslot, struct stext_file **oldfp)
{
	struct stext_file *lf, *prevf = NULL;
	uint8_t slot;

	*oldfp = NULL;

	if (reqslot == 0xff) {
		/*
		 * We're being asked to allocate a slot #.  Find the
		 * lowest slot number and use that.
		 */
		slot = 0;
		LIST_FOREACH(lf, &ctx->files, link) {
			assert(lf->slot != 0xff);
			assert(slot <= lf->slot);
			if (slot < lf->slot) {
				f->slot = slot;
				LIST_INSERT_BEFORE(lf, f, link);
				goto success;
			}
			slot = lf->slot + 1;
			if (slot == 0xff) {
				/* File table is full. */
				return false;
			}
			prevf = lf;
		}
		f->slot = slot;
		goto insert_after;
	}

	/*
	 * We're being asked to allocate a specific slot, possibly
	 * replacing another file.
	 */
	slot = f->slot = reqslot;
	LIST_FOREACH(lf, &ctx->files, link) {
		if (slot > lf->slot) {
			prevf = lf;
			continue;
		}
		if (slot == lf->slot) {
			LIST_REMOVE(lf, link);
			*oldfp = lf;
			goto insert_after;
		}
		if (slot < lf->slot) {
			LIST_INSERT_BEFORE(lf, f, link);
			goto success;
		}
	}
 insert_after:
	if (prevf != NULL) {
		LIST_INSERT_AFTER(prevf, f, link);
	} else {
		LIST_INSERT_HEAD(&ctx->files, f, link);
	}
 success:
	assert(f->slot != 0xff);
	assert((lf = LIST_NEXT(f, link)) == NULL || lf->slot > f->slot);
	return true;
}

static struct stext_file *
stext_file_find(struct stext_context *ctx, uint8_t slot)
{
	struct stext_file *f;

	if (slot == 0xff) {
		return NULL;
	}

	LIST_FOREACH(f, &ctx->files, link) {
		/* The list is sorted. */
		if (f->slot > slot) {
			break;
		}
		if (f->slot == slot) {
			return f;
		}
	}
	return NULL;
}

static void
stext_file_free(struct stext_file *f)
{
	if (f->ops != NULL) {
		(*f->ops->file_close)(f);
	}
	free(f);
}

/*
 * stext_context_init --
 *	Initlaize a storage extension context.
 */
static void
stext_context_init(struct stext_context *ctx, struct nabu_connection *conn)
{
	LIST_INIT(&ctx->files);
	ctx->conn = conn;
}

/*
 * stext_context_fini --
 *	Close all files associated with this connection.
 */
static void
stext_context_fini(struct stext_context *ctx)
{
	struct stext_file *f;

	while ((f = LIST_FIRST(&ctx->files)) != NULL) {
		log_debug("[%s] Freeing file at slot %u.", conn_name(ctx->conn),
		    f->slot);
		LIST_REMOVE(f, link);
		stext_file_free(f);
	}
}

/*
 * nhacp_send_reply --
 *	Convenience function to send an NHACP reply.
 */
static void
nhacp_send_reply(struct nhacp_context *ctx, uint8_t type, uint16_t length)
{
	nabu_set_uint16(ctx->reply.length, length);
	ctx->reply.generic.type = type;
	conn_send(ctx->stext.conn, &ctx->reply,
	    length + sizeof(ctx->reply.length));
}

/* Handy error strings. */
static const char error_message_eio[] = "I/O ERROR";
static const char error_message_einval[] = "BAD REQUEST";
static const char error_message_ebadf[] = "INVALID FILE";
static const char error_message_efbig[] = "FILE TOO BIG";
static const char error_message_enomem[] = "OUT OF MEMORY";
static const char error_message_enoent[] = "NO SUCH FILE";

static const char *
nhacp_errno_to_message(int error)
{
	switch (error) {
	default:	/* FALLTHROUGH */
	case EIO:	return error_message_eio;
	case EINVAL:	return error_message_einval;
	case EBADF:	return error_message_ebadf;
	case EFBIG:	return error_message_efbig;
	case ENOMEM:	return error_message_enomem;
	case ENOENT:	return error_message_enoent;
	}
}

/*
 * nhacp_send_error --
 *	Convenience function to send an NHACP error.
 */
static void
nhacp_send_error(struct nhacp_context *ctx, uint16_t code,
    const char *error_message)
{
	size_t message_length = strlen(error_message);
	if (message_length > 255) {
		message_length = 255;
	}
	nabu_set_uint16(ctx->reply.error.code, code);
	ctx->reply.error.message_length = (uint8_t)message_length;
	strncpy((char *)ctx->reply.error.message, error_message, 255);

	nhacp_send_reply(ctx, NHACP_RESP_ERROR,
	    sizeof(ctx->reply.error) + message_length);
}

/*
 * nhacp_send_ok --
 *	Convenience function to send an NHACP OK response.
 */
static void
nhacp_send_ok(struct nhacp_context *ctx)
{
	nhacp_send_reply(ctx, NHACP_RESP_OK, sizeof(ctx->reply.ok));
}

/*
 * nhacp_send_data_buffer --
 *	Convenience function to send a DATA-BUFFER response.
 */
static void
nhacp_send_data_buffer(struct nhacp_context *ctx, uint16_t length)
{
	nabu_set_uint16(ctx->reply.data_buffer.length, length);
	nhacp_send_reply(ctx, NHACP_RESP_DATA_BUFFER,
	    sizeof(ctx->reply.data_buffer) + length);
}

/*****************************************************************************
 * File ops for live read/write files.
 *****************************************************************************/

static int
stext_fileop_read_fileio(struct stext_file *f, void *vbuf, uint32_t offset,
    uint16_t *lengthp)
{
	uint8_t *buf = vbuf;
	size_t resid = *lengthp;
	ssize_t actual;

	if (resid > MAX_FILEIO_LENGTH - offset) {
		resid = MAX_FILEIO_LENGTH - offset;
	}

	while (resid != 0) {
		actual = fileio_pread(f->fileio.fileio, buf, resid, offset);
		if (actual < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno;
		}
		if (actual == 0) {
			/* EOF. */
			break;
		}
		buf += actual;
		offset += actual;
		resid -= actual;
	}
	*lengthp -= resid;
	return 0;
}

static int
stext_fileop_write_fileio(struct stext_file *f, const void *vbuf,
    uint32_t offset, uint16_t length)
{
	const uint8_t *buf = vbuf;
	size_t resid = length;
	ssize_t actual;

	if (resid > MAX_FILEIO_LENGTH - offset) {
		return EFBIG;
	}

	while (resid != 0) {
		actual = fileio_pwrite(f->fileio.fileio, buf, resid, offset);
		if (actual <= 0) {
			if (actual < 0 && errno == EINTR) {
				continue;
			}
			return errno;
		}
		buf += actual;
		offset += actual;
		resid -= actual;
	}
	return 0;
}

static void
stext_fileop_close_fileio(struct stext_file *f)
{
	if (f->fileio.fileio != NULL) {
		fileio_close(f->fileio.fileio);
	}
}

static const struct stext_fileops stext_fileops_fileio = {
	.file_read	= stext_fileop_read_fileio,
	.file_write	= stext_fileop_write_fileio,
	.file_close	= stext_fileop_close_fileio,
};

/*****************************************************************************
 * File ops for shadow buffered files.
 *****************************************************************************/

static int
stext_fileop_read_shadow(struct stext_file *f, void *vbuf, uint32_t offset,
    uint16_t *lengthp)
{
	uint16_t length = *lengthp;

	if (offset >= f->shadow.length) {
		length = 0;
	} else if (length > f->shadow.length - offset) {
		length = f->shadow.length - offset;
	}
	if (length != 0) {
		memcpy(vbuf, f->shadow.data + offset, length);
	}
	*lengthp = length;
	return 0;
}

static int
stext_fileop_write_shadow(struct stext_file *f, const void *vbuf,
    uint32_t offset, uint16_t length)
{
	if (length > MAX_SHADOW_LENGTH - offset) {
		return EFBIG;
	}

	if (offset + length > f->shadow.length) {
		uint8_t *newbuf = realloc(f->shadow.data, offset + length);
		if (newbuf == NULL) {
			return EIO;
		}
		memset(newbuf + f->shadow.length, 0,
		    offset + length - f->shadow.length);
		if (newbuf != f->shadow.data) {
			free(f->shadow.data);
			f->shadow.data = newbuf;
		}
		f->shadow.length = offset + length;
	}
	memcpy(f->shadow.data + offset, vbuf, length);
	return 0;
}

static void
stext_fileop_close_shadow(struct stext_file *f)
{
	if (f->shadow.data != NULL) {
		free(f->shadow.data);
	}
}

static const struct stext_fileops stext_fileops_shadow = {
	.file_read	= stext_fileop_read_shadow,
	.file_write	= stext_fileop_write_shadow,
	.file_close	= stext_fileop_close_shadow,
};

/*****************************************************************************
 * Request handling
 *****************************************************************************/

/*
 * nhacp_req_storage_open --
 *	Handle the STORAGE-OPEN request.
 */
static void
nhacp_req_storage_open(struct nhacp_context *ctx)
{
	struct stext_file *f = NULL, *of = NULL;
	struct fileio_attrs attrs;
	struct fileio *fileio = NULL;
	const char *filename;
	bool need_shadow = false;

	/*
	 * The requested URL is no more than 255 bytes long, and we
	 * know the buffer it's in is longer than the maximum size
	 * STORAGE-OPEN request, so we can simply NUL-terminate in
	 * situ.
	 */
	ctx->request.storage_open.url_string[
	    ctx->request.storage_open.url_length] = '\0';
	filename = (const char *)ctx->request.storage_open.url_string;

	f = calloc(1, sizeof(*f));
	if (f == NULL) {
		log_error("[%s] Unable to allocate NHACP file object for '%s'",
		    conn_name(ctx->stext.conn), filename);
		nhacp_send_error(ctx, 0, error_message_enomem);
		goto out;
	}

	log_debug("[%s] Opening '%s'", conn_name(ctx->stext.conn), filename);
	fileio = fileio_open(filename,
	    FILEIO_O_CREAT | FILEIO_O_LOCAL_ROOT | FILEIO_O_RDWR,
	    ctx->stext.conn->file_root, &attrs);
	if (fileio == NULL) {
		/*
		 * Try opening read-only.  If that succeeds, then we just
		 * allocate a shadow file.
		 */
		fileio = fileio_open(filename,
		    FILEIO_O_LOCAL_ROOT | FILEIO_O_RDONLY,
		    ctx->stext.conn->file_root, &attrs);
		if (fileio != NULL) {
			log_debug("[%s] Need R/W shadow buffer for '%s'",
			    conn_name(ctx->stext.conn), filename);
			need_shadow = true;
		}
	}
	if (fileio == NULL) {
		log_error("[%s] Unable to open file '%s': %s",
		    conn_name(ctx->stext.conn), filename, strerror(errno));
		nhacp_send_error(ctx, 0, error_message_enoent);
		goto out;
	}

	/* Opening directories is not allowed. */
	if (attrs.is_directory) {
		log_error("[%s] '%s': Opening directories is not permitted.",
		    conn_name(ctx->stext.conn), fileio_location(fileio));
		nhacp_send_error(ctx, 0, error_message_einval);
		goto out;
	}

	/*
	 * If the underlying file object is not seekable, then we need
	 * to allocate a shadow file, because the NHACP API has only
	 * positional I/O.
	 */
	if (! attrs.is_seekable) {
		log_debug("[%s] Need seekable shadow buffer for '%s'",
		    conn_name(ctx->stext.conn), fileio_location(fileio));
		need_shadow = true;
	}

	if (need_shadow) {
		if (attrs.size > MAX_SHADOW_LENGTH) {
			log_debug("[%s] '%s' exceeds maximum shadow length %u.",
			    conn_name(ctx->stext.conn),
			    fileio_location(fileio),
			    MAX_SHADOW_LENGTH);
			nhacp_send_error(ctx, 0, error_message_efbig);
			goto out;
		}

		f->shadow.data = fileio_load_file(fileio, &attrs,
		    0 /*extra*/, 0 /*maxsize XXX*/, &f->shadow.length);
		f->ops = &stext_fileops_shadow;
	} else {
		if (attrs.size > MAX_FILEIO_LENGTH) {
			log_debug("[%s] '%s' exceeds maximum file size %u.",
			    conn_name(ctx->stext.conn),
			    fileio_location(f->fileio.fileio),
			    MAX_FILEIO_LENGTH);
			nhacp_send_error(ctx, 0, error_message_efbig);
			goto out;
		}
		f->fileio.fileio = fileio;
		fileio = NULL;		/* file owns it now */
		f->ops = &stext_fileops_fileio;
	}

	if (! stext_file_insert(&ctx->stext, f,
				ctx->request.storage_open.req_slot, &of)) {
		log_error("[%s] Unable to insert %s at requsted slot %u.",
		    conn_name(ctx->stext.conn), filename,
		    ctx->request.storage_open.req_slot);
		goto out;
	}
	uint8_t slot = f->slot;
	f = NULL;

	ctx->reply.storage_loaded.slot = slot;
	nabu_set_uint32(ctx->reply.storage_loaded.length, (uint32_t)attrs.size);
	nhacp_send_reply(ctx, NHACP_RESP_STORAGE_LOADED,
	    sizeof(ctx->reply.storage_loaded));

 out:
	if (fileio != NULL) {
		fileio_close(fileio);
	}
	if (f != NULL) {
		stext_file_free(f);
	}
	if (of != NULL) {
		stext_file_free(of);
	}
}

/*
 * nhacp_req_storage_get --
 *	Handle the STORAGE-GET request.
 */
static void
nhacp_req_storage_get(struct nhacp_context *ctx)
{
	struct stext_file *f;

	f = stext_file_find(&ctx->stext, ctx->request.storage_get.slot);
	if (f == NULL) {
		log_debug("[%s] No file for slot %u.",
		    conn_name(ctx->stext.conn), ctx->request.storage_get.slot);
		nhacp_send_error(ctx, 0, error_message_ebadf);
		return;
	}

	uint32_t offset = nabu_get_uint32(ctx->request.storage_get.offset);
	uint16_t length = nabu_get_uint16(ctx->request.storage_get.length);

	log_debug("[%s] slot %u offset %u length %u",
	    conn_name(ctx->stext.conn), ctx->request.storage_put.slot,
	    offset, length);

	if (length > MAX_STORAGE_GET_LENGTH) {
		nhacp_send_error(ctx, 0, error_message_einval);
		return;
	}

	int error = (*f->ops->file_read)(f, ctx->reply.data_buffer.data,
	    offset, &length);
	if (error == 0) {
		nhacp_send_data_buffer(ctx, length);
	} else {
		nhacp_send_error(ctx, 0, nhacp_errno_to_message(error));
	}
}

/*
 * nhacp_req_storage_put --
 *	Handle the STORAGE-PUT request.
 */
static void
nhacp_req_storage_put(struct nhacp_context *ctx)
{
	struct stext_file *f;

	f = stext_file_find(&ctx->stext, ctx->request.storage_put.slot);
	if (f == NULL) {
		log_debug("[%s] No file for slot %u.",
		    conn_name(ctx->stext.conn), ctx->request.storage_put.slot);
		nhacp_send_error(ctx, 0, error_message_ebadf);
		return;
	}

	uint32_t offset = nabu_get_uint32(ctx->request.storage_put.offset);
	uint16_t length = nabu_get_uint16(ctx->request.storage_put.length);

	log_debug("[%s] slot %u offset %u length %u",
	    conn_name(ctx->stext.conn), ctx->request.storage_put.slot,
	    offset, length);

	if (length > MAX_STORAGE_PUT_LENGTH) {
		nhacp_send_error(ctx, 0, error_message_einval);
		return;
	}

	int error = (*f->ops->file_write)(f, ctx->request.storage_put.data,
	    offset, length);
	if (error == 0) {
		nhacp_send_ok(ctx);
	} else {
		nhacp_send_error(ctx, 0, nhacp_errno_to_message(error));
	}
}

/*
 * nhacp_req_get_date_time --
 *	Handle the GET-DATE-TIME request.
 */
static void
nhacp_req_get_date_time(struct nhacp_context *ctx)
{
	struct tm tm_store, *tm;
	time_t now = time(NULL);

	if (now == (time_t)-1) {
		log_error("[%s] unable to get current time: %s",
		    conn_name(ctx->stext.conn), strerror(errno));
		memset(&tm_store, 0, sizeof(tm_store));
		tm = &tm_store;
	} else {
		tm = localtime_r(&now, &tm_store);
	}

	/*
	 * The date and time portions of the DATE-TIME response
	 * are adjacent to each other with no intervening NUL
	 * provision, and we know there is room at the end of
	 * the response for a NUL terminator, so we can do this
	 * with a single strftime() that emits:
	 *
	 *	YYYYMMDDHHMMSS\0
	 */
	strftime((char *)ctx->reply.date_time.yyyymmdd,
	    sizeof(ctx->reply.date_time.yyyymmdd) +
	    sizeof(ctx->reply.date_time.hhmmss) + 1,
	    "%Y%m%d%H%M%S", tm);

	nhacp_send_reply(ctx, NHACP_RESP_DATE_TIME,
	    sizeof(ctx->reply.date_time));
}

/*
 * nhacp_req_storage_close --
 *	Handle the STORAGE-CLOSE request.
 */
static void
nhacp_req_storage_close(struct nhacp_context *ctx)
{
	struct stext_file *f;

	f = stext_file_find(&ctx->stext, ctx->request.storage_close.slot);
	if (f == NULL) {
		log_debug("[%s] No file for slot %u.",
		    conn_name(ctx->stext.conn),
		    ctx->request.storage_close.slot);
		return;
	}
	log_debug("[%s] Freeing file at slot %u.", conn_name(ctx->stext.conn),
	    f->slot);
	LIST_REMOVE(f, link);
	stext_file_free(f);
}

#define	HANDLER_ENTRY(v, n)						\
	[(v)] = {							\
		.handler    = nhacp_req_ ## n ,				\
		.debug_desc = #v ,					\
		.min_reqlen = sizeof(struct nhacp_request_ ## n ),	\
	}

static const struct {
	void		(*handler)(struct nhacp_context *);
	const char	*debug_desc;
	ssize_t		min_reqlen;
} nhacp_request_types[] = {
	HANDLER_ENTRY(NHACP_REQ_STORAGE_OPEN,  storage_open),
	HANDLER_ENTRY(NHACP_REQ_STORAGE_GET,   storage_get),
	HANDLER_ENTRY(NHACP_REQ_STORAGE_PUT,   storage_put),
	HANDLER_ENTRY(NHACP_REQ_GET_DATE_TIME, get_date_time),
	HANDLER_ENTRY(NHACP_REQ_STORAGE_CLOSE, storage_close),
};
static const unsigned int nhacp_request_type_count =
    sizeof(nhacp_request_types) / sizeof(nhacp_request_types[0]);

#undef HANDLER_ENTRY

/*
 * nhacp_context_alloc --
 *	Allocate an NHACP context for the specified connection.
 */
static struct nhacp_context *
nhacp_context_alloc(struct nabu_connection *conn)
{
	struct nhacp_context *ctx = calloc(1, sizeof(*ctx));
	if (ctx != NULL) {
		stext_context_init(&ctx->stext, conn);
	}
	return ctx;
}

/*
 * nhacp_context_free --
 *	Free and NHACP context and all associated resources.
 */
static void
nhacp_context_free(struct nhacp_context *ctx)
{
	stext_context_fini(&ctx->stext);
	free(ctx);
}

/*
 * nhacp_request_check --
 *	Validates the incoming request.
 *
 *	Returns:	-1	request type unknown
 *			0	everything is OK
 *			other	the expected minimum size
 */
static ssize_t
nhacp_request_check(uint8_t req, uint16_t length)
{
	/* Max message length has already been checked. */
	assert(length <= NHACP_MAX_MESSAGELEN);

	if (req < nhacp_request_type_count) {
		if (nhacp_request_types[req].handler == NULL) {
			return -1;
		}
		if (length >= nhacp_request_types[req].min_reqlen) {
			return 0;
		}
		return nhacp_request_types[req].min_reqlen;
	}
	return -1;
}

/*
 * nhacp_request --
 *	Invoke the handler for the specified request.
 */
static inline void
nhacp_request(struct nhacp_context *ctx)
{
	log_debug("[%s] Got %s.", conn_name(ctx->stext.conn),
	    nhacp_request_types[ctx->request.generic.type].debug_desc);
	(*nhacp_request_types[ctx->request.generic.type].handler)(ctx);
}

/*
 * nhacp_start --
 *	Enter NHACP mode on this connection.
 */
void
nhacp_start(struct nabu_connection *conn)
{
	struct nhacp_context *ctx = nhacp_context_alloc(conn);
	extern char nabud_version[];
	uint16_t reqlen;
	ssize_t minlen;

	/*
	 * If we failed to allocate a context, just don't send
	 * a reply -- act as if this were an unrecognized message.
	 */
	if (ctx == NULL) {
		return;
	}

	/*
	 * Send a NHACP-STARTED response.  We know there's room at the end
	 * for a NUL terminator.
	 */
						/* XXX proto version */
	nabu_set_uint16(ctx->reply.nhacp_started.version, 0);
	snprintf((char *)ctx->reply.nhacp_started.adapter_id, 256, "%s-%s",
	    getprogname(), nabud_version);
	ctx->reply.nhacp_started.adapter_id_length =
	    (uint8_t)strlen((char *)ctx->reply.nhacp_started.adapter_id);
	log_debug("[%s] Sending server version: %s", conn_name(conn),
	    (char *)ctx->reply.nhacp_started.adapter_id);
	nhacp_send_reply(ctx, NHACP_RESP_NHACP_STARTED,
	    sizeof(ctx->reply.nhacp_started) +
	    ctx->reply.nhacp_started.adapter_id_length);

	/*
	 * Now enter NHACP mode until we are asked to exit or until
	 * we detect something is awry with the NABU.
	 */
	log_info("[%s] Entering NHACP mode.", conn_name(conn));

	for (;;) {
		/* We want to block "forever" waiting for requests. */
		conn_stop_watchdog(conn);

		/*
		 * Receive the first (LSB) byte of the length.  We need
		 * to do this to guard against a NABU that's been reset.
		 */
		log_debug("[%s] Waiting for NABU.", conn_name(conn));
		if (! conn_recv_byte(conn, &ctx->request.length[0])) {
 recv_failure:
			if (conn_state(conn) == CONN_STATE_EOF) {
				log_info("[%s] Peer disconnected.",
				    conn_name(conn));
				break;
			}
			if (conn_state(conn) == CONN_STATE_CANCELLED) {
				log_info("[%s] Received cancellation request.",
				    conn_name(conn));
				break;
			}
			if (conn_state(conn) == CONN_STATE_ABORTED) {
				log_error("[%s] Connection aborted.",
				    conn_name(conn));
				break;
			}
			log_error("[%s] conn_recv_byte() failed, "
			    "exiting event loop.", conn_name(conn));
			break;
		}

		/*
		 * Now that we have the first byte, enable the watchdog.
		 * The protocol says that each individual message transfer
		 * must complete within 1 second.
		 */
		conn_start_watchdog(conn, 1);

		/* Now receive the MSB of the length. */
		if (! conn_recv_byte(conn, &ctx->request.length[1])) {
			goto recv_failure;
		}
		reqlen = nabu_get_uint16(ctx->request.length);

		/*
		 * No legitimate NHACP message can have a length with
		 * the most significant bit set.  If we have one, we
		 * assume the NABU has reset and is sending legacy
		 * messages (see nabu_proto.h).
		 *
		 * XXX A zero-length message isn't legitimate, either.
		 * XXX We'll treat it the same way.
		 */
		if ((reqlen & 0x8000) != 0 || reqlen == 0) {
			log_error("[%s] Bogus request length: 0x%04x - "
			    "exiting NHACP mode.", conn_name(conn), reqlen);
			break;
		}

		/* Ok, receive the message. */
		if (! conn_recv(conn, &ctx->request.max_request, reqlen)) {
			goto recv_failure;
		}

		/*
		 * Check for END-PROTOCOL before we do anything else.
		 * There's no payload and no reply -- we just get out.
		 */
		if (ctx->request.generic.type == NHACP_REQ_END_PROTOCOL) {
			log_debug("[%s] Got NHACP_REQ_END_PROTOCOL.",
			    conn_name(conn));
			break;
		}

		/*
		 * Check that the client sent the bare-minimum for the
		 * request to be valid.
		 */
		minlen = nhacp_request_check(ctx->request.generic.type, reqlen);
		if (minlen == -1) {
			log_error("[%s] Unknown NHACP request: 0x%02x",
			    conn_name(conn), ctx->request.generic.type);
			/* Just skip it. */
			continue;
		} else if (minlen != 0) {
			log_error("[%s] Runt NHACP request: %u < %zd",
			    conn_name(conn), reqlen, minlen);
			/* Just skip it. */
			continue;
		}

		/* Everything checks out -- handle the request. */
		nhacp_request(ctx);
	}

	log_info("[%s] Exiting NHACP mode.", conn_name(conn));
	nhacp_context_free(ctx);
}
