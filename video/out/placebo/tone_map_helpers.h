#ifndef DMPV_PLACEBO_TONE_MAP_HELPERS_H
#define DMPV_PLACEBO_TONE_MAP_HELPERS_H

#include <stdbool.h>

typedef struct pl_options_t *pl_options;
struct pl_color_map_params;
struct pl_tone_map_params;
struct pl_tone_map_function;

bool pltm_build_from_opts(const char *func_name, double curve_param,
                          struct pl_tone_map_params *out);
bool pltm_try_set_options_string(pl_options pars, double curve_param);
bool pltm_apply_to_color_map(struct pl_color_map_params *cm,
                             const struct pl_tone_map_params *tm);

#endif
