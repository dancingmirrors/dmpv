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

#ifndef MP_JSON_H
#define MP_JSON_H

// We reuse dmpv_node.
#include "misc/client.h"

#define MAX_JSON_DEPTH 50

int json_parse(void *ta_parent, struct dmpv_node *dst, char **src, int max_depth);
void json_skip_whitespace(char **src);
int json_write(char **s, struct dmpv_node *src);
int json_write_pretty(char **s, struct dmpv_node *src);

#endif
