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

#pragma once

#include <libavutil/channel_layout.h>

#include "config.h"

#include "chmap.h"

bool mp_chmap_from_av_layout(struct mp_chmap *dst, const AVChannelLayout *src);

void mp_chmap_to_av_layout(AVChannelLayout *dst, const struct mp_chmap *src);
