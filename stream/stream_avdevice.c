/*
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

#include "stream.h"

static int open_f(stream_t *stream)
{
    stream->demuxer = "lavf";

    return STREAM_OK;
}

const stream_info_t stream_info_avdevice = {
    .name = "avdevice",
    .open = open_f,
    .protocols = (const char*const[]){ "avdevice", "av", NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};
