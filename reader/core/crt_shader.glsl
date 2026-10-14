// Reader CRT: the picture as a cathode-ray tube would show it. One pass, the same text in
// every reader fork and for every graphics API.
//
// The including shader defines, before this text:
//   CRT_TEX(uv)   the source picture's colour (vec4) at texture coordinate uv, bilinear
//   CRT_U(i)      vec4 number i of the eight the reader's core packs (Pack in reader/core/crt.cpp)
//   CRT_QUALITY   0: phones and handhelds, 1: computers
// and calls ReaderCrt(p), p being the place in the picture: (0,0) its top left corner,
// (1,1) its bottom right one, as it lies on the screen.
//
// Written in what GLSL ES 1.00, GLSL 3.30, Vulkan GLSL and HLSL (through the defines of
// crt.cpp's preambles) have in common: no vector made from a single number, no integer
// arithmetic, loops of constant length.

// Two of the eight vectors hold two numbers in one (128 bytes is all that some graphics
// APIs hand a shader without a buffer): the lines with, behind the point, how much of its
// own detail the source keeps; the phosphor's pattern with, behind the point, its size.
#define CRT_LINES floor(CRT_U(2).z)
#define CRT_OWN (fract(CRT_U(2).z) / 0.99)
#define CRT_MASK_KIND floor(CRT_U(3).y)
#define CRT_MASK_SIZE floor(fract(CRT_U(3).y) * 10.0 + 0.5)

#if CRT_QUALITY >= 1
#define CRT_HN 2
#define CRT_GN 3
#define CRT_GSTEP 1.6
#else
#define CRT_HN 1
#define CRT_GN 2
#define CRT_GSTEP 2.2
#endif

float crtHash(vec2 p) {
    vec3 p3 = fract(vec3(p.x, p.y, p.x) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// The source at a place in the picture; its edge's colour beyond the edge.
vec3 crtSrc(vec2 p) {
    vec2 inset = 0.5 / CRT_U(1).zw;
    vec2 q = clamp(p, inset, 1.0 - inset);
    return CRT_TEX(CRT_U(0).xy + q.x * CRT_U(0).zw + q.y * CRT_U(1).xy).rgb;
}

vec3 crtLinear(vec3 c) {
    return pow(abs(c), vec3(2.4, 2.4, 2.4));
}

// One line of the tube: the source along a row, as wide a spot as `Sharpness` leaves, red
// and blue landing a little to the left and right of green (`Convergence`).
vec3 crtRow(vec2 p) {
    float hres = CRT_U(2).w;
    float sharp = CRT_U(4).x;
    float conv = CRT_U(7).w * 0.7;
    float sigma = mix(0.80, 0.12, sharp);
    float d = max(sigma * 0.75, conv * 0.5);
    float sk = max(sigma, d * 0.62);
    float inv = 0.5 / (sk * sk);
    vec3 sum = vec3(0.0, 0.0, 0.0);
    vec3 wsum = vec3(0.0, 0.0, 0.0);
    for (int k = -CRT_HN; k <= CRT_HN; k++) {
        float o = float(k) * d;
        vec3 e = vec3(o + conv, o, o - conv);
        vec3 w = exp(-e * e * inv);
        sum += w * crtLinear(crtSrc(vec2(p.x + o / hres, p.y)));
        wsum += w;
    }
    return sum / wsum;
}

// The phosphor at a pixel of the screen: which of the three colours is lit there.
vec3 crtMask(vec2 px, float kind) {
    vec3 m = vec3(1.0, 1.0, 1.0);
    if (kind < 0.5) {
        return m;
    }
    if (kind < 1.5) { // aperture grille: unbroken stripes
        float x = fract(px.x / 3.0);
        m = x < 0.333 ? vec3(1.0, 0.0, 0.0) : (x < 0.666 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
    } else if (kind < 2.5) { // slot mask: stripes cut into bricks, every other column half a brick down
        float x = fract(px.x / 3.0);
        m = x < 0.333 ? vec3(1.0, 0.0, 0.0) : (x < 0.666 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
        float odd = fract(px.x / 6.0) < 0.5 ? 0.0 : 2.0;
        if (fract((px.y + odd) / 4.0) < 0.25) {
            m *= 0.25;
        }
    } else if (kind < 3.5) { // shadow mask: dots in a honeycomb
        float x = fract((px.x + floor(px.y) * 3.0) / 6.0);
        m = x < 0.333 ? vec3(1.0, 0.0, 0.0) : (x < 0.666 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
    } else { // fine: two pixels a triad, for a screen with few of them
        m = fract(px.x / 2.0) < 0.5 ? vec3(1.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    }
    return m;
}

vec3 ReaderCrt(vec2 p) {
    vec2 src = CRT_U(1).zw;
    vec2 outp = CRT_U(2).xy;
    float lines = CRT_LINES;
    float time = CRT_U(3).x;
    float scan = CRT_U(3).z;
    float beam = CRT_U(3).w;
    float flick = CRT_U(7).y;
    float aspect = outp.x / outp.y;

    // The glass bulges: a place on the screen shows what lies further out in the picture.
    vec2 c = p * 2.0 - 1.0;
    float warp = CRT_U(4).z;
    vec2 cw = c * (1.0 + warp * vec2(0.062 * c.y * c.y, 0.082 * c.x * c.x));
    vec2 q = cw * 0.5 + 0.5;

    // Where the tube ends: the picture's edge, and the rounded corners.
    float corner = max(CRT_U(4).w * 0.16, 0.0015);
    vec2 cs = vec2(aspect, 1.0);
    vec2 dd = abs(cw) * cs - (cs - corner);
    float dist = length(max(dd, 0.0)) + min(max(dd.x, dd.y), 0.0) - corner;
    float inside = clamp(-dist * outp.y * 0.5, 0.0, 1.0);
    if (inside <= 0.0) {
        return vec3(0.0, 0.0, 0.0);
    }

    // Lines that do not all land where they should (`Flicker`).
    float ly = q.y * lines - 0.5;
    float li = floor(ly);
    float lf = ly - li;
    if (flick > 0.0) {
        q.x += (crtHash(vec2(li, floor(time * 60.0))) - 0.5) * 0.0012 * flick;
    }

    // A source of about one row a line is shown as its rows, each the middle of a line; a
    // finer one keeps its own detail and gets the lines laid over it (CRT_OWN says which).
    float own = CRT_OWN;
    float sharp = CRT_U(4).x;
    float tx = q.x * src.x - 0.5;
    float txi = floor(tx);
    float snap = mix(1.0, max(1.0, outp.x / src.x), sharp * sharp);
    q.x = (txi + clamp((tx - txi - 0.5) * snap + 0.5, 0.0, 1.0) + 0.5) / src.x;
    vec3 c1 = crtRow(vec2(q.x, mix((li + 0.5) / lines, q.y, own)));
    vec3 c2 = crtRow(vec2(q.x, mix((li + 1.5) / lines, q.y, own)));

    // The beam: a spot that is brightest in the middle of its line, and wider the brighter
    // it is, so that bright lines close up and dark ones stand apart.
    float sd = mix(0.42, 0.17, scan);
    float l1 = max(c1.r, max(c1.g, c1.b));
    float l2 = max(c2.r, max(c2.g, c2.b));
    float s1 = min(sd * (1.0 + beam * 1.3 * sqrt(l1)), 0.42);
    float s2 = min(sd * (1.0 + beam * 1.3 * sqrt(l2)), 0.42);
    float d1 = lf;
    float d2 = 1.0 - lf;
    float w1 = exp(-0.5 * d1 * d1 / (s1 * s1)) * smoothstep(1.0, 0.7, d1);
    float w2 = exp(-0.5 * d2 * d2 / (s2 * s2)) * smoothstep(1.0, 0.7, d2);
    vec3 col = c1 * w1 + c2 * w2;
    float lum = max(col.r, max(col.g, col.b));
    // What the gaps between the lines take away is given back, most of it.
    col /= mix(1.0, min(1.0, 2.5066 * 0.5 * (s1 + s2)), 0.78);

    // Light that spreads in the glass: a near blur (`Glow`) and a far one (`Halation`).
    float glow = CRT_U(5).y;
    float hal = CRT_U(5).z;
    vec3 gn = vec3(0.0, 0.0, 0.0);
    vec3 gw = vec3(0.0, 0.0, 0.0);
    if (glow + hal > 0.0) {
        float gs = CRT_GSTEP / 480.0;
        float wn = 0.0;
        float ww = 0.0;
        for (int j = -CRT_GN; j <= CRT_GN; j++) {
            for (int i = -CRT_GN; i <= CRT_GN; i++) {
                vec2 o = vec2(float(i), float(j)) * CRT_GSTEP;
                float r2 = dot(o, o);
                float a = exp(-r2 / 3.6);
                float b = exp(-r2 / 22.0);
                vec3 t = crtSrc(q + vec2(float(i) * gs / aspect, float(j) * gs));
                t *= t;
                gn += a * t;
                gw += b * t;
                wn += a;
                ww += b;
            }
        }
        gn /= wn;
        gw /= ww;
    }

    // The phosphor. Where the picture is bright its light runs over the mask's gaps (`Bloom`).
    float mstr = CRT_U(4).y;
    float bloom = CRT_U(5).w;
    vec3 mask = crtMask(floor(p * outp / CRT_MASK_SIZE), CRT_MASK_KIND);
    float mean = CRT_MASK_KIND > 3.5 ? 0.5 : (CRT_MASK_KIND > 1.5 && CRT_MASK_KIND < 2.5 ? 0.27 : 0.3333);
    float open = clamp(bloom * 1.2 * lum * lum, 0.0, 1.0);
    mask = mix(mask, vec3(1.0, 1.0, 1.0), open);
    mean = mix(mean, 1.0, open);
    col *= mix(vec3(1.0, 1.0, 1.0), mask, mstr) / mix(1.0, 1.0 - mstr * (1.0 - mean), 0.85);

    col = (col + glow * 0.55 * gn) / (1.0 + glow * 0.22);
    col += hal * 0.45 * (gw + gw * gw) * vec3(1.06, 0.97, 0.88);

    // The tube's colours: how bright, how warm its white, how strong its colours.
    col *= CRT_U(6).x;
    float warm = CRT_U(6).y;
    col *= vec3(1.0 + 0.14 * warm, 1.0 + 0.01 * warm, 1.0 - 0.20 * warm);
    float grey = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = max(mix(vec3(grey, grey, grey), col, CRT_U(6).z), 0.0);

    // Darker towards the corners.
    float vig = CRT_U(5).x;
    float vv = (1.0 - c.x * c.x) * (1.0 - c.y * c.y);
    col *= mix(1.0, pow(abs(vv), 0.35), vig);

    // The mains: a slow bar of brightness going up the picture, and a tremble.
    if (flick > 0.0) {
        float bar = sin((p.y * 1.3 - time * 0.37) * 6.2831853) * 0.035;
        float tremble = sin(time * 75.0) * 0.012 + sin(time * 119.0) * 0.008;
        col *= 1.0 + (bar + tremble) * flick;
    }

    // The room in the glass: a lamp above, and a little light everywhere.
    float glass = CRT_U(7).z;
    if (glass > 0.0) {
        vec2 lp = vec2((p.x - 0.64) * aspect * 0.75, p.y + 0.10);
        float lamp = exp(-dot(lp, lp) * 7.0);
        float rim = dot(c * c, c * c) * 0.25;
        col += glass * (0.012 + 0.11 * lamp + 0.03 * rim) * vec3(1.0, 0.96, 0.90);
    }

    // Highlights roll off instead of ending flat.
    vec3 over = max(col - 0.8, 0.0);
    col = min(col, 0.8) + over / (1.0 + over * 5.0);

    // To the screen's own curve, `Contrast` deepening it.
    float ga = CRT_U(6).w / 2.2;
    col = pow(abs(col), vec3(ga, ga, ga));

    // Snow (`Grain`), and always a grain too fine to see, against steps in the gradients.
    vec2 px = p * outp;
    float n = crtHash(px + vec2(fract(time * 7.13) * 311.0, fract(time * 3.71) * 173.0)) - 0.5;
    col += n * (CRT_U(7).x * 0.09 * (0.35 + 0.65 * (1.0 - abs(2.0 * grey - 1.0))));
    col += (crtHash(px) - 0.5) / 255.0;

    return clamp(col, 0.0, 1.0) * inside;
}
