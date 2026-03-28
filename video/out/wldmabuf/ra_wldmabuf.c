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

#include "video/out/wayland_common.h"
#include "video/out/gpu/ra.h"
#include "ra_wldmabuf.h"

struct priv {
    struct vo *vo;
};

static void destroy(struct ra *ra)
{
    talloc_free(ra->priv);
}

bool ra_compatible_format(struct ra* ra, uint32_t drm_format, uint64_t modifier)
{
    struct priv* p = ra->priv;
    struct vo_wayland_state *wl = p->vo->wl;

    if (!wl_list_empty(&wl->tranche_list)) {
        struct vo_wayland_tranche *tranche;
        wl_list_for_each(tranche, &wl->tranche_list, link) {
            for (int i = 0; i < tranche->num_formats; i++) {
                if (tranche->formats[i].format == drm_format &&
                    tranche->formats[i].modifier == modifier)
                    return true;
            }
        }
        return false;
    }

    const wayland_format *formats = wl->format_map;
    for (int i = 0; i < (int)(wl->format_size / sizeof(wayland_format)); i++) {
        if (formats[i].format == drm_format && formats[i].modifier == modifier)
            return true;
    }
    return false;
}

static struct ra_fns ra_fns_wldmabuf = {
    .destroy                = destroy,
};

struct ra *ra_create_wayland(struct mp_log *log, struct vo* vo)
{
    struct ra *ra =  talloc_zero(NULL, struct ra);

    ra->fns = &ra_fns_wldmabuf;
    ra->log = log;
    ra_add_native_resource(ra, "wl", vo->wl->display);
    ra->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ra->priv;
    p->vo = vo;

    return ra;
}

bool ra_is_wldmabuf(struct ra *ra)
{
    return (ra->fns == &ra_fns_wldmabuf);
}
