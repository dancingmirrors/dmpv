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

#include <string.h>

#include "misc/ctype.h"

#include "natural_sort.h"

// Comparison function for "natural" sort. Case insensitive (ASCII-only), and
// numbers are ordered by value (ignoring 00... prefix). If the two names
// end up equal, then it falls back to strcmp for higher-resolution order.
int mp_natural_sort_cmp(const char *name1, const char *name2)
{
    const char *name1_orig = name1, *name2_orig = name2;

    while (name1[0] && name2[0]) {
        if (mp_isdigit(name1[0]) && mp_isdigit(name2[0])) {
            while (name1[0] == '0')
                name1++;
            while (name2[0] == '0')
                name2++;
            const char *end1 = name1, *end2 = name2;
            while (mp_isdigit(*end1))
                end1++;
            while (mp_isdigit(*end2))
                end2++;
            // With padding stripped, a number with more digits is bigger.
            if ((end1 - name1) < (end2 - name2))
                return -1;
            if ((end1 - name1) > (end2 - name2))
                return 1;
            // Same length, lexicographical works.
            while (name1 < end1) {
                if (name1[0] < name2[0])
                    return -1;
                if (name1[0] > name2[0])
                    return 1;
                name1++;
                name2++;
            }
        } else {
            if (mp_tolower(name1[0]) < mp_tolower(name2[0]))
                return -1;
            if (mp_tolower(name1[0]) > mp_tolower(name2[0]))
                return 1;
            name1++;
            name2++;
        }
    }
    if (name2[0])
        return -1;
    if (name1[0])
        return 1;

    // equal as natural, fall back to strcmp
    return strcmp(name1_orig, name2_orig);
}
