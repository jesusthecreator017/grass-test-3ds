// Mini procedural grass scene for the 3DS, after ../Rust/wgpu-test (grass-test).
//
// The PC version builds blades on the GPU every frame with a compute shader.
// The PICA200 has no compute, so here every blade is still a pure function of
// its grid cell hash, generated once at startup; each frame the CPU bends the
// blades for wind and the cube "pusher", and a vertex shader does transform + fog.

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern const u8 shader_shbin[];
extern const u32 shader_shbin_size;

#define CLEAR_COLOR 0x85B8ECFF
#define SKY_R 0x85 / 255.0f
#define SKY_G 0xB8 / 255.0f
#define SKY_B 0xEC / 255.0f

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// Tuning knobs.
#define FIELD      7.0f    // grass covers [-FIELD, FIELD] on x and z
#define SPACING    0.17f   // metres between grass cells
#define MAX_VERTS  60000   // u16 indices, so stay under 65536
#define MAX_IDX    120000
#define MAX_BLADES 9000
#define PUSH_R     0.6f    // cube pushes grass within this radius
#define TERRAIN_N  64      // ground grid cells per side
#define TERRAIN_SZ 90.0f   // ground covers [-SZ, SZ]

typedef struct { float x, y, z, r, g, b; } Vertex;

typedef struct {
    float rx, ry, rz, h, phase, stiff;
    u16 first, count;
} Blade;

static Vertex *base, *grassVbo, *groundVbo, *cubeVbo;
static u16 *grassIdx, *groundIdx;
static Blade blades[MAX_BLADES];
static int nBlades, nVerts, nIdx, nGroundIdx;

// ---------------------------------------------------------------- noise

static u32 hash2(int x, int y, u32 seed) {
    u32 h = (u32)x * 0x8da6b343u ^ (u32)y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}
static float rnd(int x, int y, u32 seed) { return (hash2(x, y, seed) & 0xffffff) / 16777216.0f; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float smooth(float a, float b, float v) { float t = clampf((v - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }

// Value noise, for patches (dry, tall) and clump-ish variation.
static float vnoise(float x, float y, u32 seed) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = rnd(ix, iy, seed), b = rnd(ix + 1, iy, seed);
    float c = rnd(ix, iy + 1, seed), d = rnd(ix + 1, iy + 1, seed);
    return mixf(mixf(a, b, fx), mixf(c, d, fx), fy);
}

// Same terrain as terrain.rs / grass.wgsl in grass-test.
static float terrainH(float x, float z) {
    return 3.0f * sinf(x * 0.045f + 0.5f) * cosf(z * 0.038f - 1.0f)
         + 1.5f * sinf(x * 0.09f + z * 0.07f + 2.0f)
         + 0.6f * sinf(x * 0.21f - z * 0.17f);
}

// A dirt path winding across the field. Returns distance from its centre line.
static float pathDist(float x, float z) { return fabsf(z - (2.5f + 1.3f * sinf(x * 0.33f))); }
#define PATH_W 0.7f

// ---------------------------------------------------------------- geometry

static Vertex *addV(float dx, float h, float dz, float r, float g, float b) {
    Vertex *v = &base[nVerts++];
    v->x = dx; v->y = h; v->z = dz; v->r = r; v->g = g; v->b = b;
    return v;
}
static void tri(int a, int b, int c) { grassIdx[nIdx++] = a; grassIdx[nIdx++] = b; grassIdx[nIdx++] = c; }

// One blade: a tapered, bowed strip of 3 triangles. Offsets relative to root.
static void bladeStrip(float facing, float h, float w, float lean,
                       float rr, float rg, float rb, float tr, float tg, float tb) {
    float fx = cosf(facing), fz = sinf(facing);   // lean direction
    float sx = -fz, sz = fx;                       // width direction
    int v0 = nVerts;
    float ts[3] = {0.0f, 0.55f, 1.0f}, ws[3] = {0.5f, 0.32f, 0.0f};
    for (int i = 0; i < 3; i++) {
        float t = ts[i], l = lean * t * t * h;
        float r = mixf(rr, tr, t), g = mixf(rg, tg, t), b = mixf(rb, tb, t);
        if (i < 2) {
            addV(fx * l - sx * w * ws[i], h * t, fz * l - sz * w * ws[i], r, g, b);
            addV(fx * l + sx * w * ws[i], h * t, fz * l + sz * w * ws[i], r, g, b);
        } else {
            addV(fx * l, h, fz * l, r, g, b);
        }
    }
    tri(v0, v0 + 1, v0 + 2); tri(v0 + 1, v0 + 3, v0 + 2); tri(v0 + 2, v0 + 3, v0 + 4);
}

// Iris head: three drooping petals and three upright ones around the stem tip.
static void irisHead(float h, float tipx, float tipz, float spin, float r, float g, float b) {
    for (int i = 0; i < 6; i++) {
        float a = spin + i * (6.2831853f / 6), up = (i & 1) ? 0.05f : -0.025f;
        float len = (i & 1) ? 0.05f : 0.075f, c = cosf(a), s = sinf(a);
        float k = (i & 1) ? 1.1f : 0.85f;
        int v0 = nVerts;
        addV(tipx, h, tipz, r * 0.7f, g * 0.6f, b * 0.8f);
        addV(tipx + c * len - s * 0.02f, h + up, tipz + s * len + c * 0.02f, r * k, g * k, b * k);
        addV(tipx + c * len + s * 0.02f, h + up, tipz + s * len - c * 0.02f, r * k, g * k, b * k);
        tri(v0, v0 + 1, v0 + 2);
    }
}

static void buildGrass(void) {
    int n = (int)(2 * FIELD / SPACING);
    for (int iz = 0; iz < n && nBlades < MAX_BLADES; iz++)
    for (int ix = 0; ix < n && nBlades < MAX_BLADES; ix++) {
        if (nVerts + 32 > MAX_VERTS || nIdx + 48 > MAX_IDX) return;
        float x = -FIELD + (ix + rnd(ix, iz, 1)) * SPACING;
        float z = -FIELD + (iz + rnd(ix, iz, 2)) * SPACING;

        // Thin out towards the field edge and next to the dirt path.
        float edge = 1 - smooth(FIELD - 1.5f, FIELD, fmaxf(fabsf(x), fabsf(z)));
        float dirt = smooth(PATH_W, PATH_W + 0.6f, pathDist(x, z));
        float keep = edge * dirt;
        if (keep < 0.05f || rnd(ix, iz, 3) > keep + 0.1f) continue;

        // Patches and clumps: tall tussocks, sparse bits, dry areas.
        float tall  = vnoise(x * 0.35f, z * 0.35f, 10);
        float dry   = smooth(0.6f, 0.8f, vnoise(x * 0.22f + 7, z * 0.22f, 11));
        float clump = rnd((int)floorf(x / 0.7f), (int)floorf(z / 0.7f), 12);
        float h = (0.22f + 0.45f * tall * tall + 0.15f * clump) * (0.75f + 0.5f * rnd(ix, iz, 4)) * mixf(0.4f, 1, keep);
        if (rnd(ix, iz, 5) < 0.04f) h *= 1.6f;                     // stragglers

        float facing = rnd(ix, iz, 6) * 6.2831853f;
        float w = 0.035f + 0.02f * rnd(ix, iz, 7);
        float lean = 0.25f + 0.55f * rnd(ix, iz, 8);

        // Colour: dark root, lighter tip, clump tint, mixed towards straw when dry.
        float cv = 0.85f + 0.3f * clump;
        float rr = 0.10f * cv, rg = 0.20f * cv, rb = 0.06f * cv;
        float tr = mixf(0.30f, 0.38f, clump), tg = mixf(0.52f, 0.62f, clump), tb = 0.16f;
        rr = mixf(rr, 0.32f, dry); rg = mixf(rg, 0.30f, dry); rb = mixf(rb, 0.12f, dry);
        tr = mixf(tr, 0.74f, dry); tg = mixf(tg, 0.66f, dry); tb = mixf(tb, 0.36f, dry);

        Blade *bl = &blades[nBlades++];
        bl->rx = x; bl->rz = z; bl->ry = terrainH(x, z);
        bl->phase = rnd(ix, iz, 9) * 6.2831853f;
        bl->first = nVerts;

        bool flower = dry < 0.3f && rnd(ix, iz, 13) < 0.012f;
        if (flower) {
            h = 0.45f + 0.15f * rnd(ix, iz, 14);
            bladeStrip(facing, h, 0.012f, 0.1f, 0.12f, 0.25f, 0.08f, 0.2f, 0.4f, 0.12f);
            Vertex *tip = &base[nVerts - 1];
            float pick = rnd(ix, iz, 15);
            float pr = pick < 0.7f ? 0.56f : 0.75f, pg = pick < 0.7f ? 0.45f : 0.68f, pb = pick < 0.7f ? 0.86f : 0.90f;
            irisHead(h, tip->x, tip->z, facing, pr, pg, pb);
            bl->stiff = 0.5f;
        } else {
            bladeStrip(facing, h, w, lean, rr, rg, rb, tr, tg, tb);
            bl->stiff = 1.0f;
        }
        bl->h = h;
        bl->count = nVerts - bl->first;
    }
}

static void buildGround(void) {
    int n = TERRAIN_N + 1;
    for (int iz = 0; iz < n; iz++)
    for (int ix = 0; ix < n; ix++) {
        // Denser near the middle so the grassy area matches the blades' roots.
        float u = ix / (float)TERRAIN_N * 2 - 1, v = iz / (float)TERRAIN_N * 2 - 1;
        float x = u * fabsf(u) * TERRAIN_SZ, z = v * fabsf(v) * TERRAIN_SZ;
        float d = smooth(PATH_W - 0.2f, PATH_W + 0.5f, pathDist(x, z));
        if (fmaxf(fabsf(x), fabsf(z)) > FIELD + 1) d = 1;  // path only runs through the field
        float k = 0.85f + 0.3f * vnoise(x * 0.5f, z * 0.5f, 20);
        Vertex *g = &groundVbo[iz * n + ix];
        g->x = x; g->y = terrainH(x, z) - 0.01f; g->z = z;
        g->r = mixf(0.45f, 0.17f * k, d); g->g = mixf(0.33f, 0.33f * k, d); g->b = mixf(0.20f, 0.10f * k, d);
    }
    for (int iz = 0; iz < TERRAIN_N; iz++)
    for (int ix = 0; ix < TERRAIN_N; ix++) {
        u16 a = iz * n + ix, b = a + 1, c = a + n, e = c + 1;
        u16 *o = &groundIdx[nGroundIdx]; nGroundIdx += 6;
        o[0] = a; o[1] = c; o[2] = b; o[3] = b; o[4] = c; o[5] = e;
    }
}

static void buildCube(void) {
    static const float face[6][4][3] = {
        {{-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}}, {{ 1,-1,-1},{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1}},
        {{ 1,-1, 1},{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1}}, {{-1,-1,-1},{-1,-1, 1},{-1, 1, 1},{-1, 1,-1}},
        {{-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1},{-1, 1,-1}}, {{-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1}},
    };
    static const float shade[6] = {0.75f, 0.55f, 0.65f, 0.6f, 1.0f, 0.4f};
    static const int q[6] = {0, 1, 2, 0, 2, 3};
    for (int f = 0; f < 6; f++)
    for (int i = 0; i < 6; i++) {
        const float *p = face[f][q[i]];
        Vertex *v = &cubeVbo[f * 6 + i];
        v->x = p[0] * 0.25f; v->y = p[1] * 0.25f + 0.25f; v->z = p[2] * 0.25f;
        v->r = 0.28f * shade[f]; v->g = 0.48f * shade[f]; v->b = 0.32f * shade[f];
    }
}

// ---------------------------------------------------------------- per frame

// Bend every blade for wind and the cube, writing world-space vertices.
static void updateGrass(float t, float cx, float cz) {
    const float wx = 0.8f, wz = 0.6f;   // wind direction
    for (int i = 0; i < nBlades; i++) {
        Blade *b = &blades[i];
        // Gusts roll across the field (position term) plus a per-blade flutter.
        float w = (0.22f + 0.16f * sinf(t * 1.3f - b->rx * 0.45f - b->rz * 0.3f)
                   + 0.05f * sinf(t * 3.1f + b->phase)) * b->stiff;
        float bx = wx * w, bz = wz * w;
        float px = b->rx - cx, pz = b->rz - cz, d2 = px * px + pz * pz;
        if (d2 < PUSH_R * PUSH_R) {
            float d = sqrtf(d2) + 1e-3f, k = (PUSH_R - d) / PUSH_R * 3.0f;
            bx += px / d * k; bz += pz / d * k;
        }
        float b2 = bx * bx + bz * bz, invH = 1.0f / b->h;
        const Vertex *s = &base[b->first];
        Vertex *o = &grassVbo[b->first];
        for (int j = 0; j < b->count; j++) {
            float f = s[j].y * invH;
            float sc = 1.0f / sqrtf(1 + b2 * f * f);   // keep blade length roughly constant
            float l = s[j].y * f * sc;
            o[j].x = b->rx + s[j].x + bx * l;
            o[j].y = b->ry + s[j].y * sc;
            o[j].z = b->rz + s[j].z + bz * l;
        }
    }
    GSPGPU_FlushDataCache(grassVbo, nVerts * sizeof(Vertex));
}

static void bindVbo(void *vbo) {
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, vbo, sizeof(Vertex), 2, 0x10);
}

int main(void) {
    osSetSpeedupEnable(true);  // 804 MHz on New 3DS
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    C3D_RenderTarget *target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    DVLB_s *dvlb = DVLB_ParseFile((u32 *)shader_shbin, shader_shbin_size);
    shaderProgram_s prog;
    shaderProgramInit(&prog);
    shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
    C3D_BindProgram(&prog);
    int uProj = shaderInstanceGetUniformLocation(prog.vertexShader, "projection");
    int uView = shaderInstanceGetUniformLocation(prog.vertexShader, "modelView");
    int uFog  = shaderInstanceGetUniformLocation(prog.vertexShader, "fog");
    int uSky  = shaderInstanceGetUniformLocation(prog.vertexShader, "sky");

    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 3);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_CullFace(GPU_CULL_NONE);  // blades are double-sided

    printf("Building meadow...\n");
    int gN = (TERRAIN_N + 1) * (TERRAIN_N + 1);
    base      = malloc(MAX_VERTS * sizeof(Vertex));
    grassVbo  = linearAlloc(MAX_VERTS * sizeof(Vertex));
    grassIdx  = linearAlloc(MAX_IDX * sizeof(u16));
    groundVbo = linearAlloc(gN * sizeof(Vertex));
    groundIdx = linearAlloc(TERRAIN_N * TERRAIN_N * 6 * sizeof(u16));
    cubeVbo   = linearAlloc(36 * sizeof(Vertex));
    if (!base || !grassVbo || !grassIdx || !groundVbo || !groundIdx || !cubeVbo) {
        printf("Out of memory. START to exit.\n");
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
        gfxExit();
        return 1;
    }
    buildGrass();
    buildGround();
    buildCube();
    for (int i = 0; i < nVerts; i++) grassVbo[i] = base[i];  // colours never change
    GSPGPU_FlushDataCache(grassIdx, nIdx * sizeof(u16));
    GSPGPU_FlushDataCache(groundVbo, gN * sizeof(Vertex));
    GSPGPU_FlushDataCache(groundIdx, nGroundIdx * sizeof(u16));
    GSPGPU_FlushDataCache(cubeVbo, 36 * sizeof(Vertex));

    printf("\x1b[2J");
    printf("grass-test, 3DS edition\n\n");
    printf("%d blades, %d tris\n\n", nBlades, nIdx / 3);
    printf("Circle Pad  move the cube\n");
    printf("L / R       orbit camera\n");
    printf("C-Stick     orbit / tilt\n");
    printf("D-Pad       zoom\n");
    printf("START       exit\n");

    float cubeX = 0, cubeZ = 0, yaw = 0.6f, pitch = 0.18f, dist = 3.2f;
    u64 start = osGetTime(), last = start;
    int frames = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 held = hidKeysHeld();
        if (hidKeysDown() & KEY_START) break;

        u64 now = osGetTime();
        float dt = clampf((now - last) / 1000.0f, 0, 0.1f), t = (now - start) / 1000.0f;
        last = now;

        circlePosition cp, cs;
        hidCircleRead(&cp);
        hidCstickRead(&cs);
        if (held & KEY_L) yaw -= 1.5f * dt;
        if (held & KEY_R) yaw += 1.5f * dt;
        yaw += cs.dx / 156.0f * 2.0f * dt;
        pitch = clampf(pitch + cs.dy / 156.0f * dt, 0.05f, 1.2f);
        if (held & KEY_DUP)   dist = clampf(dist - 3 * dt, 1.2f, 10);
        if (held & KEY_DDOWN) dist = clampf(dist + 3 * dt, 1.2f, 10);

        // Move the cube relative to where the camera looks.
        float fx = -sinf(yaw), fz = -cosf(yaw);
        float mx = cp.dx / 156.0f, my = cp.dy / 156.0f;
        if (fabsf(mx) < 0.1f) mx = 0;
        if (fabsf(my) < 0.1f) my = 0;
        cubeX = clampf(cubeX + (fx * my - fz * mx) * 2.0f * dt, -FIELD + 1, FIELD - 1);
        cubeZ = clampf(cubeZ + (fz * my + fx * mx) * 2.0f * dt, -FIELD + 1, FIELD - 1);
        float cubeY = terrainH(cubeX, cubeZ);

        float ex = cubeX - fx * dist * cosf(pitch), ez = cubeZ - fz * dist * cosf(pitch);
        float ey = fmaxf(cubeY + 0.3f + dist * sinf(pitch), terrainH(ex, ez) + 0.25f);

        C3D_Mtx proj, view, model;
        Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(55), C3D_AspectRatioTop, 0.05f, 150, false);
        Mtx_LookAt(&view, FVec3_New(ex, ey, ez), FVec3_New(cubeX, cubeY + 0.3f, cubeZ), FVec3_New(0, 1, 0), false);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        updateGrass(t, cubeX, cubeZ);
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
        C3D_FrameDrawOn(target);

        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProj, &proj);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, uFog, 1.0f / 110, 0, 0, 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, uSky, SKY_R, SKY_G, SKY_B, 1);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uView, &view);

        bindVbo(groundVbo);
        C3D_DrawElements(GPU_TRIANGLES, nGroundIdx, C3D_UNSIGNED_SHORT, groundIdx);
        bindVbo(grassVbo);
        C3D_DrawElements(GPU_TRIANGLES, nIdx, C3D_UNSIGNED_SHORT, grassIdx);

        Mtx_Copy(&model, &view);
        Mtx_Translate(&model, cubeX, cubeY, cubeZ, true);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uView, &model);
        bindVbo(cubeVbo);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 36);

        C3D_FrameEnd(0);

        if (++frames % 30 == 0)
            printf("\x1b[12;0HFPS %4.1f  CPU %4.1fms  GPU %4.1fms ",
                   1.0f / fmaxf(dt, 1e-3f), C3D_GetProcessingTime(), C3D_GetDrawingTime());
    }

    linearFree(cubeVbo); linearFree(groundIdx); linearFree(groundVbo);
    linearFree(grassIdx); linearFree(grassVbo); free(base);
    shaderProgramFree(&prog);
    DVLB_Free(dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
