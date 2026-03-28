#include "tone_map_helpers.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include <libplacebo/tone_mapping.h>
#include <libplacebo/options.h>
#include <libplacebo/shaders/colorspace.h>

/*
 * Heuristic mapping:
 *  - If function is "spline" or "auto": interpret curve_param as spline_contrast.
 *  - If function is "reinhard": interpret curve_param as reinhard_contrast.
 *  - Otherwise put the value in constants.exposure as a fallback.
 *
 * Always call pl_tone_map_params_infer() to clobber sensible defaults.
 */
bool pltm_build_from_opts(const char *func_name, double curve_param,
                          struct pl_tone_map_params *out)
{
    if (!out)
        return false;

    // Default function: auto -> spline
    const struct pl_tone_map_function *func = NULL;
    if (func_name && func_name[0]) {
        func = pl_find_tone_map_function(func_name);
    }
    if (!func)
        func = &pl_tone_map_spline; // default to spline if not found

    struct pl_tone_map_constants defaults = { PL_TONE_MAP_CONSTANTS };
    *out = (struct pl_tone_map_params){
        .function = func,
        .constants = defaults,
        .input_scaling = PL_HDR_PQ,
        .output_scaling = PL_HDR_PQ,
        .lut_size = 256,
        .input_min = 0.0f,
        .input_max = 1.0f,
        .input_avg = 0.0f,
        .output_min = 0.0f,
        .output_max = 1.0f,
    };

    if (isnan(curve_param))
        curve_param = 0.0;

    if (func == &pl_tone_map_spline) {
        // spline_contrast is [0,1.5] (see header). Clamp accordingly.
        double v = curve_param;
        if (v < 0.0) v = 0.0;
        if (v > 1.5) v = 1.5;
        out->constants.spline_contrast = (float)v;
    } else if (func == &pl_tone_map_reinhard) {
        double v = curve_param;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        out->constants.reinhard_contrast = (float)v;
    } else if (func == &pl_tone_map_mobius) {
        out->constants.exposure = (float)(1.0 + curve_param);
    } else {
        out->constants.exposure = (float)(1.0 + curve_param);
    }

    // Allow libplacebo to clamp/default other fields
    pl_tone_map_params_infer(out);
    return true;
}

bool pltm_try_set_options_string(pl_options pars, double curve_param)
{
    if (!pars)
        return false;

    char tmp[64];
    if (isnan(curve_param))
        curve_param = 0.0;
    snprintf(tmp, sizeof(tmp), "%g", curve_param);

    pl_options_set_str(pars, "tone-mapping-param", tmp);
    return true;
}

bool pltm_apply_to_color_map(struct pl_color_map_params *cm,
                             const struct pl_tone_map_params *tm)
{
    if (!cm || !tm)
        return false;

    cm->tone_mapping_function = tm->function;
    cm->tone_constants = tm->constants; // colorspace.h has `struct pl_tone_map_constants tone_constants;`

    return true;
}
