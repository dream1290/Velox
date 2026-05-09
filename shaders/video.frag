#version 330 core
/* video.frag — Video frame fragment shader
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * glimagesink writes RGBA frames directly into the texture so no
 * YUV→RGB conversion is needed here — that's done in hardware by
 * the GStreamer videoconvert element upstream.
 *
 * The shader is kept intentionally minimal.  Colour correction
 * (brightness/contrast/saturation) is applied via uniform offsets
 * when vlx_settings enables it.
 */

in  vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uTexture;

/* Optional colour adjustments (0.0 = identity) */
uniform float uBrightness; /* -1.0 .. +1.0, default 0.0 */
uniform float uContrast;   /*  0.0 .. +2.0, default 1.0 */
uniform float uSaturation; /*  0.0 .. +2.0, default 1.0 */

vec3 adjustSaturation(vec3 rgb, float sat)
{
    float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(lum), rgb, sat);
}

void main()
{
    vec4 color = texture(uTexture, vTexCoord);

    /* Brightness */
    color.rgb += uBrightness;

    /* Contrast  */
    color.rgb = (color.rgb - 0.5) * uContrast + 0.5;

    /* Saturation */
    color.rgb = adjustSaturation(color.rgb, uSaturation);

    fragColor = clamp(color, 0.0, 1.0);
}
