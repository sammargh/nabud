/*-
 * Copyright (c) 2022 Jason R. Thorpe.
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

#ifndef fileio_h_included
#define	fileio_h_included

#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct fileio;

struct fileio_attrs {
	off_t	size;
	time_t	mtime;		/* mod time */
	time_t	btime;		/* birth time */
	bool	is_directory;
	bool	is_writable;
	bool	is_seekable;
};

struct fileio *	fileio_open(const char *, int, struct fileio_attrs *);
void		fileio_close(struct fileio *);
ssize_t		fileio_read(struct fileio *, void *, size_t);
ssize_t		fileio_write(struct fileio *, const void *, size_t);
ssize_t		fileio_pread(struct fileio *, void *, size_t, off_t);
ssize_t		fileio_pwrite(struct fileio *, const void *, size_t, off_t);
bool		fileio_getattr(struct fileio *, struct fileio_attrs *);
const char *	fileio_location(struct fileio *);

#define	FILEIO_O_RDONLY		0x00
#define	FILEIO_O_RDWR		0x01

void	*fileio_load_file(struct fileio *, struct fileio_attrs *, size_t,
			  size_t, size_t *filesizep);

void	*fileio_load_file_from_location(const char *, size_t, size_t,
					size_t *);

#endif /* fileio_h_included */
