/*
 * unicode/utf-8 I/O helpers and wrappers
 *
 * This file is part of dmpv.
 *
 * dmpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * dmpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with dmpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MPLAYER_OSDEP_IO
#define MPLAYER_OSDEP_IO

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <locale.h>

#include <glob.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

// This is in POSIX.1-2008, but support outside of Linux is scarce.
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 0
#endif

bool mp_set_cloexec(int fd);
int mp_make_cloexec_pipe(int pipes[2]);
int mp_make_wakeup_pipe(int pipes[2]);
void mp_flush_wakeup_pipe(int pipe_end);

typedef uint64_t mp_ino_t_;

int mp_mkostemps(char *template, int suffixlen, int flags);

#endif
