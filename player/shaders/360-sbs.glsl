//!PARAM yaw
//!DESC Horizontal view angle (degrees, positive = right)
//!TYPE DYNAMIC float
//!MINIMUM -180.0
//!MAXIMUM 180.0
0.0

//!PARAM pitch
//!DESC Vertical view angle (degrees, positive = up)
//!TYPE DYNAMIC float
//!MINIMUM -90.0
//!MAXIMUM 90.0
0.0

//!PARAM hfov
//!DESC Horizontal field of view (degrees)
//!TYPE DYNAMIC float
//!MINIMUM 10.0
//!MAXIMUM 170.0
90.0

//!HOOK MAIN
//!BIND HOOKED
//!DESC 360 SBS equirectangular projection
//!WIDTH OUTPUT.w
//!HEIGHT OUTPUT.h

#define PI 3.14159265358979323846

vec4 hook() {
    vec2 ndc = HOOKED_pos * 2.0 - 1.0;
    ndc.y    = -ndc.y;

    float aspect = target_size.x / target_size.y;

    float hfov_rad = hfov * (PI / 180.0);
    float vlon = ndc.x * hfov_rad * 0.5;
    float vlat = ndc.y * hfov_rad * 0.5 / aspect;

    vec3 ray = vec3(sin(vlon) * cos(vlat),
                    sin(vlat),
                    cos(vlon) * cos(vlat));

    float p  = pitch * (PI / 180.0);
    float cp = cos(p), sp = sin(p);
    mat3 Rx = mat3(
        1.0, 0.0,  0.0,   // col 0
        0.0,  cp,  -sp,   // col 1
        0.0,  sp,   cp    // col 2
    );

    float ya = yaw * (PI / 180.0);
    float cy = cos(ya), sy = sin(ya);
    mat3 Ry = mat3(
         cy, 0.0, -sy,    // col 0
        0.0, 1.0,  0.0,   // col 1
         sy, 0.0,  cy     // col 2
    );

    vec3 dir = Ry * Rx * ray;

    float lon = atan(dir.x, dir.z);
    float lat = asin(clamp(dir.y, -1.0, 1.0));
    float u   = lon / (2.0 * PI) + 0.5;
    float v   = 0.5 - lat / PI;

    return HOOKED_tex(vec2(u, v));
}
