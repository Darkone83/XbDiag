// StressTestGPU.cpp
// XbDiag - GPU Stress Test card (CARD_GPU)
//
// CrystalScene (crystalline grotto demo) is inlined directly here —
// no CrystalScene.h / CrystalScene.cpp dependency. All scene code
// is file-scope static; the three entry points are renamed:
//   GPU_SceneInit()     — formerly CrystalScene_Init()
//   GPU_SceneShutdown() — formerly CrystalScene_Shutdown()
//   GPU_SceneRender()   — formerly CrystalScene_Render()
//
// Stress card behaviour:
//   Scene loops every 20s until [Back]+[A] held 5s.
//   Overlay strip at bottom: ELAPSED / LOOPS / FPS / PEAK / MIN / CPU / FAN.
//   Abort hold bar drawn above the strip.

#include "StressTestGPU.h"
#include "StressTest.h"
#include "font.h"
#include "input.h"
#include <xtl.h>
#include <xgraphics.h>
#include <d3dx8.h>
#include <math.h>
#include <string.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if ((p)) { (p)->Release(); (p) = NULL; } } while (0)
#endif

// ============================================================================
// CrystalScene internals (all static — no external linkage)
// ============================================================================

static const float CS_SW = 640.f;
static const float CS_SH = 480.f;
static const float CS_PI = 3.14159265f;
static const float CS_TAU = 6.28318530f;
static const DWORD SCENE_LOOP_MS = 20000;

static bool  cs_initOK = false;
static char  cs_initErr[128] = { 0 };
static DWORD cs_startTicks = 0;
static DWORD cs_lastTick = 0;
static float cs_fps = 0.f;
static float cs_frameMs = 16.f;
static int   cs_drawCalls = 0;

static void CS_SetInitError(const char* msg)
{
    cs_initOK = false;
    if (!msg) msg = "unknown";
    strncpy(cs_initErr, msg, sizeof(cs_initErr) - 1);
    cs_initErr[sizeof(cs_initErr) - 1] = 0;
}

// --- Helpers -----------------------------------------------------------------
static __forceinline int   CS_Ftoi(float f) { int i; __asm { fld f } __asm { fistp i } return i; }
static __forceinline BYTE  CS_ClampB(int v) { return v < 0 ? 0 : v > 255 ? 255 : (BYTE)v; }
static __forceinline float CS_Clamp01(float f) { return f < 0.f ? 0.f : f > 1.f ? 1.f : f; }

static const int CS_LUT_N = 1024;
static float cs_sin[CS_LUT_N], cs_cos[CS_LUT_N];
static bool  cs_lutReady = false;

static void CS_BuildLUT()
{
    if (cs_lutReady) return;
    for (int i = 0; i < CS_LUT_N; ++i) {
        float a = (float)i * CS_TAU / (float)CS_LUT_N;
        cs_sin[i] = sinf(a);
        cs_cos[i] = cosf(a);
    }
    cs_lutReady = true;
}

static __forceinline float CS_LSin(float r)
{
    int i = CS_Ftoi(r * CS_LUT_N / CS_TAU);
    return cs_sin[((i % CS_LUT_N) + CS_LUT_N) & (CS_LUT_N - 1)];
}

// --- Vertex shader -----------------------------------------------------------
static DWORD cs_vsHandle = 0;
static const DWORD cs_vsDecl[] = {
    D3DVSD_STREAM(0),
    D3DVSD_REG(D3DVSDE_POSITION,  D3DVSDT_FLOAT3),
    D3DVSD_REG(D3DVSDE_NORMAL,    D3DVSDT_FLOAT3),
    D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT2),
    D3DVSD_END()
};
static const char cs_vsSource[] =
"vs.1.1\n"
"dp4 r0.x, v0, c4\ndp4 r0.y, v0, c5\ndp4 r0.z, v0, c6\nmov r0.w, c13.z\n"
"dp4 oPos.x, v0, c0\ndp4 oPos.y, v0, c1\ndp4 oPos.z, v0, c2\ndp4 oPos.w, v0, c3\n"
"dp3 r1.x, v1, c4\ndp3 r1.y, v1, c5\ndp3 r1.z, v1, c6\n"
"dp3 r2.x, r1, r1\nrsq r2.x, r2.x\nmul r1, r1, r2.xxxx\n"
"dp3 r3.x, r1, c7\nmax r3.x, r3.x, c13.x\nmul r4, c9, r3.xxxx\n"
"dp3 r3.y, r1, c8\nmax r3.y, r3.y, c13.x\nmul r5, c10, r3.yyyy\n"
"add r4, r4, c11\nadd r4, r4, r5\nmin r4, r4, c13.zzzz\nmov r4.w, c13.z\nmov oD0, r4\n"
"sub r6, c12, r0\ndp3 r2.x, r6, r6\nrsq r2.x, r2.x\nmul r6, r6, r2.xxxx\n"
"dp3 r2.x, r1, r6\nadd r2.x, r2.x, r2.x\nmul r7, r1, r2.xxxx\nsub r7, r7, r6\n"
"mov oT1, r7\nmov oT0, v2\n";

static void CS_CreateVS()
{
    LPXGBUFFER pCode = NULL, pErr = NULL;
    if (SUCCEEDED(XGAssembleShader("CrystalVS", cs_vsSource,
        (UINT)strlen(cs_vsSource), 0, NULL, &pCode, &pErr, NULL, NULL, NULL, NULL)))
    {
        g_pDevice->CreateVertexShader(cs_vsDecl,
            (const DWORD*)pCode->GetBufferPointer(), &cs_vsHandle, 0);
        pCode->Release();
    }
    if (pErr) pErr->Release();
}

static void CS_DeleteVS()
{
    if (cs_vsHandle) { g_pDevice->DeleteVertexShader(cs_vsHandle); cs_vsHandle = 0; }
}

// --- Vertex formats ----------------------------------------------------------
struct CS_CVtx { float x, y, z, nx, ny, nz, u, v; };
struct CS_GVtx { float x, y, z; DWORD color; };
struct CS_CaveVtx { float x, y, z, nx, ny, nz, u, v; };
static const DWORD CS_GVFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;
static const DWORD CS_CAVEFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

// --- V3 math -----------------------------------------------------------------
struct CS_V3 { float x, y, z; };

static CS_V3 CS_V3Sub(CS_V3 a, CS_V3 b)
{
    CS_V3 r = { a.x - b.x, a.y - b.y, a.z - b.z };
    return r;
}
static CS_V3 CS_V3Mid(CS_V3 a, CS_V3 b)
{
    CS_V3 r = { (a.x + b.x) * .5f, (a.y + b.y) * .5f, (a.z + b.z) * .5f };
    return r;
}
static CS_V3 CS_V3Cross(CS_V3 a, CS_V3 b)
{
    CS_V3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}
static float CS_V3Len(CS_V3 v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }
static CS_V3 CS_V3Norm(CS_V3 v)
{
    float l = CS_V3Len(v);
    CS_V3 r = l > 1e-5f ? CS_V3{ v.x / l, v.y / l, v.z / l } : CS_V3{ 0, 1, 0 };
    return r;
}
static CS_V3 CS_XformV3(CS_V3 p, float sc,
    float rx, float ry, float rz, float tx, float ty, float tz)
{
    float x = p.x * sc, y = p.y * sc, z = p.z * sc;
    float y2 = y * cosf(rx) - z * sinf(rx), z2 = y * sinf(rx) + z * cosf(rx); y = y2; z = z2;
    float x2 = x * cosf(ry) + z * sinf(ry); z2 = -x * sinf(ry) + z * cosf(ry); x = x2; z = z2;
    x2 = x * cosf(rz) - y * sinf(rz); y2 = x * sinf(rz) + y * cosf(rz); x = x2; y = y2;
    CS_V3 r = { x + tx, y + ty, z + tz };
    return r;
}

// --- Subdivision -------------------------------------------------------------
struct CS_Tri { CS_V3 a, b, c; };

static int CS_SubdivideTris(const CS_Tri* src, int n, CS_Tri* dst)
{
    int out = 0;
    for (int i = 0; i < n; ++i) {
        CS_V3 ab = CS_V3Mid(src[i].a, src[i].b);
        CS_V3 bc = CS_V3Mid(src[i].b, src[i].c);
        CS_V3 ca = CS_V3Mid(src[i].c, src[i].a);
        CS_Tri t0 = { src[i].a, ab, ca }; dst[out++] = t0;
        CS_Tri t1 = { ab, src[i].b, bc }; dst[out++] = t1;
        CS_Tri t2 = { ca, bc, src[i].c }; dst[out++] = t2;
        CS_Tri t3 = { ab, bc, ca };        dst[out++] = t3;
    }
    return out;
}

// --- Write triangle ----------------------------------------------------------
static CS_CVtx* CS_WriteTri(CS_CVtx* dst, CS_V3 a, CS_V3 b, CS_V3 c)
{
    CS_V3 n = CS_V3Norm(CS_V3Cross(CS_V3Sub(b, a), CS_V3Sub(c, a)));
    CS_V3 pts[3] = { a, b, c };
    for (int k = 0; k < 3; ++k) {
        dst->x = pts[k].x; dst->y = pts[k].y; dst->z = pts[k].z;
        dst->nx = n.x; dst->ny = n.y; dst->nz = n.z;
        float len = CS_V3Len(pts[k]);
        float nx_ = len > 1e-5f ? pts[k].x / len : 0.f;
        float ny_ = len > 1e-5f ? pts[k].y / len : 0.f;
        float nz_ = len > 1e-5f ? pts[k].z / len : 1.f;
        dst->u = atan2f(nx_, nz_) / CS_TAU + 0.5f;
        dst->v = acosf(CS_Clamp01(ny_)) / CS_PI;
        dst++;
    }
    return dst;
}

// --- Prism definitions -------------------------------------------------------
struct CS_CDef { float shH, capH, rad, sc, rx, ry, rz, tx, ty, tz; };
static const int CS_ND = 14;
static const CS_CDef cs_def[CS_ND] = {
    {1.8f,0.70f,0.35f,1.f, 0.10f, 0.30f, 0.00f, 0.00f,-0.90f, 0.00f},
    {1.5f,0.60f,0.28f,1.f,-0.15f, 0.80f, 0.10f,-0.10f,-0.85f, 0.15f},
    {1.2f,0.50f,0.24f,1.f, 0.20f,-0.40f, 0.05f, 0.15f,-0.80f,-0.10f},
    {0.9f,0.40f,0.18f,1.f, 1.10f, 0.00f, 0.00f, 0.50f,-0.60f, 0.20f},
    {0.8f,0.35f,0.16f,1.f,-0.90f, 0.50f, 0.20f,-0.45f,-0.50f, 0.10f},
    {1.0f,0.40f,0.20f,1.f, 0.80f, 1.20f,-0.30f, 0.20f,-0.70f, 0.50f},
    {0.7f,0.30f,0.15f,1.f,-0.70f,-0.80f, 0.40f,-0.30f,-0.40f,-0.50f},
    {0.85f,0.35f,0.17f,1.f,1.30f, 0.30f, 0.50f, 0.00f,-0.80f,-0.60f},
    {0.6f,0.28f,0.14f,1.f, 0.60f,-1.10f,-0.20f,-0.60f,-0.50f, 0.30f},
    {0.75f,0.32f,0.15f,1.f,1.50f,-0.30f, 0.60f, 0.80f,-0.35f, 0.10f},
    {0.70f,0.30f,0.14f,1.f,-1.40f, 0.60f,-0.40f,-0.75f,-0.30f,-0.20f},
    {0.65f,0.28f,0.13f,1.f, 0.50f, 1.60f, 0.30f, 0.10f,-0.45f, 0.80f},
    {0.60f,0.26f,0.12f,1.f,-0.60f,-1.50f,-0.50f,-0.15f,-0.35f,-0.80f},
    {0.40f,0.20f,0.09f,1.f, 1.80f, 0.40f, 0.20f, 0.70f,-0.30f, 0.00f},
};

// --- Build prism -------------------------------------------------------------
static int CS_BuildPrism(CS_CVtx* dst,
    float shH, float capH, float rad, float sc,
    float rx, float ry, float rz,
    float tx, float ty, float tz)
{
    const int SIDES = 6;
    CS_V3 ring[6], top[6];
    for (int i = 0; i < SIDES; ++i) {
        float a = CS_PI / 6.f + i * CS_TAU / SIDES;
        CS_V3 rp = { cosf(a) * rad, 0.f, sinf(a) * rad };
        CS_V3 tp = { cosf(a) * rad, shH, sinf(a) * rad };
        ring[i] = CS_XformV3(rp, sc, rx, ry, rz, tx, ty, tz);
        top[i] = CS_XformV3(tp, sc, rx, ry, rz, tx, ty, tz);
    }
    CS_V3 apexP = { 0.f, shH + capH, 0.f };
    CS_V3 baseP = { 0.f, 0.f, 0.f };
    CS_V3 apex = CS_XformV3(apexP, sc, rx, ry, rz, tx, ty, tz);
    CS_V3 base = CS_XformV3(baseP, sc, rx, ry, rz, tx, ty, tz);

    static CS_Tri bA[24], bufA[1536], bufB[1536];
    int bn = 0;
    for (int i = 0; i < SIDES; ++i) {
        int j = (i + 1) % SIDES;
        CS_Tri t0 = { ring[i], top[i],  ring[j] }; bA[bn++] = t0;
        CS_Tri t1 = { ring[j], top[i],  top[j] }; bA[bn++] = t1;
        CS_Tri t2 = { top[i],  apex,    top[j] }; bA[bn++] = t2;
        CS_Tri t3 = { base,    ring[j], ring[i] }; bA[bn++] = t3;
    }
    CS_Tri* src = bufA;
    CS_Tri* tmp = bufB;
    for (int i = 0; i < bn; ++i) src[i] = bA[i];
    int n = bn;
    for (int p = 0; p < 3; ++p)
    {
        n = CS_SubdivideTris(src, n, tmp);
        CS_Tri* sw = src; src = tmp; tmp = sw;
    }
    CS_CVtx* dp = dst;
    for (int i = 0; i < n; ++i)
        dp = CS_WriteTri(dp, src[i].a, src[i].b, src[i].c);
    return n;
}

// --- Geometry state ----------------------------------------------------------
static LPDIRECT3DVERTEXBUFFER8 cs_crystalVB = NULL;
static LPDIRECT3DVERTEXBUFFER8 cs_fogVB = NULL;
static int cs_crystalTris = 0;
static int cs_fogTris = 0;

static const int CS_FOG_PUFFS = 120;
static const int CS_FOG_VERTS = CS_FOG_PUFFS * 12;

static LPDIRECT3DTEXTURE8     cs_normalMap = NULL;
static LPDIRECT3DCUBETEXTURE8 cs_cubeMap = NULL;

static const int CS_CAVE_SEGS = 192;
static const int CS_CAVE_RINGS = 96;
static LPDIRECT3DVERTEXBUFFER8 cs_caveVB = NULL;
static LPDIRECT3DINDEXBUFFER8  cs_caveIB = NULL;
static LPDIRECT3DTEXTURE8      cs_caveTex = NULL;
static bool                    cs_rockDDSOK = false;
static int cs_caveVerts = 0;
static int cs_cavePrims = 0;

// --- State helpers -----------------------------------------------------------
static void CS_DisableFrom(int s)
{
    for (int i = s; i < 4; ++i) {
        g_pDevice->SetTexture(i, NULL);
        g_pDevice->SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(i, D3DTSS_TEXCOORDINDEX, i);
        g_pDevice->SetTextureStageState(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
}

static void CS_SetLinear(int s)
{
    g_pDevice->SetTextureStageState(s, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(s, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(s, D3DTSS_MIPFILTER, D3DTEXF_NONE);
}

static void CS_SetDOT3TFactor(DWORD tMs)
{
    float t = (float)tMs * 0.001f;
    float la = t * 0.22f;
    float dlx = -cosf(la), dly = -0.6f - 0.3f * sinf(t * 0.17f), dlz = -sinf(la);
    float len = sqrtf(dlx * dlx + dly * dly + dlz * dlz);
    if (len < 1e-4f) len = 1.f;
    dlx /= len; dly /= len; dlz /= len;
    BYTE r = CS_ClampB(CS_Ftoi((dlx + 1.f) * 127.5f));
    BYTE g = CS_ClampB(CS_Ftoi((-dly + 1.f) * 127.5f));
    BYTE b = CS_ClampB(CS_Ftoi((dlz + 1.f) * 127.5f));
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, r, g, b));
}

// --- Build cave --------------------------------------------------------------
static void CS_BuildCave()
{
    SAFE_RELEASE(cs_caveVB); SAFE_RELEASE(cs_caveIB); SAFE_RELEASE(cs_caveTex);
    const int   SEGS = CS_CAVE_SEGS, RINGS = CS_CAVE_RINGS;
    const float RADIUS = 6.0f, DEPTH = 14.0f, ZNEAR = -2.0f;
    cs_caveVerts = (SEGS + 1) * (RINGS + 1);
    cs_cavePrims = SEGS * RINGS * 2;

    HRESULT hrV = g_pDevice->CreateVertexBuffer(
        cs_caveVerts * sizeof(CS_CaveVtx), D3DUSAGE_WRITEONLY, 0,
        D3DPOOL_MANAGED, &cs_caveVB);
    if (FAILED(hrV) || !cs_caveVB)
        hrV = g_pDevice->CreateVertexBuffer(
            cs_caveVerts * sizeof(CS_CaveVtx), D3DUSAGE_WRITEONLY, 0,
            D3DPOOL_DEFAULT, &cs_caveVB);
    if (FAILED(hrV) || !cs_caveVB) return;

    HRESULT hrI = g_pDevice->CreateIndexBuffer(
        cs_cavePrims * 3 * sizeof(WORD), D3DUSAGE_WRITEONLY,
        D3DFMT_INDEX16, D3DPOOL_MANAGED, &cs_caveIB);
    if (FAILED(hrI) || !cs_caveIB)
        hrI = g_pDevice->CreateIndexBuffer(
            cs_cavePrims * 3 * sizeof(WORD), D3DUSAGE_WRITEONLY,
            D3DFMT_INDEX16, D3DPOOL_DEFAULT, &cs_caveIB);
    if (FAILED(hrI) || !cs_caveIB) { SAFE_RELEASE(cs_caveVB); return; }

    CS_CaveVtx* vb = NULL;
    cs_caveVB->Lock(0, 0, (BYTE**)&vb, 0);
    for (int ring = 0; ring <= RINGS; ++ring) {
        float frac = (float)ring / (float)RINGS;
        float z = ZNEAR + frac * DEPTH;
        for (int seg = 0; seg <= SEGS; ++seg) {
            float a = (float)seg * CS_TAU / (float)SEGS;
            unsigned int h = (unsigned int)(seg * 1664525 + ring * 22695477 + 1013904223);
            h ^= (h >> 16); h *= 0x45d9f3b; h ^= (h >> 16);
            float jag = RADIUS + ((h >> 24) & 0x0F) * 0.04f;
            float cx = cosf(a) * jag, cy = sinf(a) * jag;
            float nl = sqrtf(cx * cx + cy * cy);
            CS_CaveVtx& v = vb[ring * (SEGS + 1) + seg];
            v.x = cx; v.y = cy; v.z = z;
            v.nx = -(cx / nl); v.ny = -(cy / nl); v.nz = 0.f;
            v.u = (float)seg / (float)SEGS * 6.f;
            v.v = frac * 4.f;
        }
    }
    cs_caveVB->Unlock();

    WORD* ib = NULL;
    cs_caveIB->Lock(0, 0, (BYTE**)&ib, 0);
    int idx = 0;
    for (int ring = 0; ring < RINGS; ++ring)
        for (int seg = 0; seg < SEGS; ++seg) {
            WORD tl = (WORD)(ring * (SEGS + 1) + seg), tr = tl + 1;
            WORD bl = (WORD)((ring + 1) * (SEGS + 1) + seg), br = bl + 1;
            ib[idx++] = tl; ib[idx++] = bl; ib[idx++] = tr;
            ib[idx++] = tr; ib[idx++] = bl; ib[idx++] = br;
        }
    cs_caveIB->Unlock();

    cs_rockDDSOK = SUCCEEDED(D3DXCreateTextureFromFileA(
        g_pDevice, "D:\\tex\\rock.dds", &cs_caveTex)) && cs_caveTex;
    if (!cs_rockDDSOK) {
        g_pDevice->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &cs_caveTex);
        if (cs_caveTex) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(cs_caveTex->LockRect(0, &lr, NULL, 0)) && lr.pBits) {
                DWORD* px = (DWORD*)lr.pBits;
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        px[y * 8 + x] = ((x + y) & 1)
                        ? D3DCOLOR_ARGB(255, 255, 100, 255)
                        : D3DCOLOR_ARGB(255, 100, 255, 100);
                cs_caveTex->UnlockRect(0);
            }
        }
    }
}

// --- Draw cave ---------------------------------------------------------------
static void CS_DrawCave(DWORD tMs, float alpha)
{
    if (!cs_caveVB || !cs_caveIB || !cs_caveTex || cs_cavePrims <= 0) return;
    float t = (float)tMs * 0.001f;

    D3DXVECTOR3 eye(0.f, 0.3f, -5.f), at(0.f, 0.f, 0.f), up(0.f, 1.f, 0.f);
    D3DXMATRIX view, proj, mWorld;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, CS_PI / 3.2f, CS_SW / CS_SH, 0.1f, 50.f);
    D3DXMatrixRotationY(&mWorld, t * 0.03f);
    g_pDevice->SetTransform(D3DTS_VIEW, &view);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &proj);
    g_pDevice->SetTransform(D3DTS_WORLD, &mWorld);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

    int gph = (tMs / 12u) & (CS_LUT_N - 1);
    float glow = 0.18f + cs_sin[gph] * 0.07f;
    BYTE gR = CS_ClampB(CS_Ftoi(glow * 60.f * alpha));
    BYTE gG = CS_ClampB(CS_Ftoi(glow * 30.f * alpha));
    BYTE gB = CS_ClampB(CS_Ftoi(glow * 80.f * alpha));
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(gR, gG, gB));

    g_pDevice->SetTexture(0, cs_caveTex);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    g_pDevice->SetTexture(1, NULL);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    CS_DisableFrom(2);

    g_pDevice->SetVertexShader(CS_CAVEFVF);
    g_pDevice->SetStreamSource(0, cs_caveVB, sizeof(CS_CaveVtx));
    g_pDevice->SetIndices(cs_caveIB, 0);
    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0,
        cs_caveVerts, 0, cs_cavePrims);

    // Stalactites / stalagmites
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    CS_DisableFrom(1);

    struct CV { float x, y, z; DWORD color; };
    const float ZN = -2.f, DP = 14.f, RD = 6.f;

    // Stalactites — 32, violet/blue
    for (int i = 0; i < 32; ++i) {
        float ang = (float)i * 0.71f;
        float zp = ZN + 0.8f + (float)i * (DP / 32.f);
        float wx = cosf(ang) * RD, wy = sinf(ang) * RD;
        float nx2 = -cosf(ang), ny2 = -sinf(ang);
        float sLen = 0.25f + (float)(i % 4) * 0.18f + (float)(i % 3) * 0.10f;
        float cLen = 0.20f + (float)(i % 3) * 0.12f;
        float rad = 0.06f + (float)(i % 4) * 0.025f;
        float bx = -ny2, by = nx2;
        int iph = (i * 97 + (tMs / 25u)) & (CS_LUT_N - 1);
        float br = 0.70f + cs_sin[iph] * 0.30f;
        DWORD cBase = D3DCOLOR_XRGB(CS_ClampB(CS_Ftoi(br * 80.f * alpha)), CS_ClampB(CS_Ftoi(br * 30.f * alpha)), CS_ClampB(CS_Ftoi(br * 140.f * alpha)));
        DWORD cMid = D3DCOLOR_XRGB(CS_ClampB(CS_Ftoi(br * 180.f * alpha)), CS_ClampB(CS_Ftoi(br * 80.f * alpha)), CS_ClampB(CS_Ftoi(br * 255.f * alpha)));
        DWORD cTip = D3DCOLOR_XRGB(CS_ClampB(CS_Ftoi(br * 220.f * alpha)), CS_ClampB(CS_Ftoi(br * 180.f * alpha)), CS_ClampB(CS_Ftoi(br * 255.f * alpha)));
        CV base[6], mid[6];
        for (int k = 0; k < 6; ++k) {
            float a2 = (float)k / 6.f * CS_TAU;
            float tx2 = bx * cosf(a2) * rad, ty2 = by * cosf(a2) * rad, tz2 = sinf(a2) * rad;
            CV b2 = { wx + tx2, wy + ty2, zp + tz2, cBase }; base[k] = b2;
            CV m2 = { wx + tx2 * 0.6f + nx2 * sLen, wy + ty2 * 0.6f + ny2 * sLen, zp + tz2 * 0.6f, cMid }; mid[k] = m2;
        }
        CV tip = { wx + nx2 * (sLen + cLen), wy + ny2 * (sLen + cLen), zp, cTip };
        for (int k = 0; k < 6; ++k) {
            int nn = (k + 1) % 6;
            CV q[6] = { base[k],base[nn],mid[k],base[nn],mid[nn],mid[k] };
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(CV));
        }
        for (int k = 0; k < 6; ++k) {
            int nn = (k + 1) % 6;
            CV t3[3] = { mid[k], mid[nn], tip };
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t3, sizeof(CV));
        }
    }

    // Stalagmites — 24, pink/magenta
    for (int i = 0; i < 24; ++i) {
        float ang = (float)i * 0.83f + CS_PI * 0.5f;
        float zp = ZN + 0.8f + (float)i * (DP / 24.f);
        float wx = cosf(ang) * RD, wy = sinf(ang) * RD;
        float nx2 = -cosf(ang), ny2 = -sinf(ang);
        float sLen = 0.22f + (float)(i % 3) * 0.16f + (float)(i % 5) * 0.09f;
        float cLen = 0.18f + (float)(i % 4) * 0.10f;
        float rad = 0.05f + (float)(i % 3) * 0.025f;
        float bx = -ny2, by = nx2;
        int iph = (i * 61 + (tMs / 30u)) & (CS_LUT_N - 1);
        float br = 0.65f + cs_sin[iph] * 0.35f;
        DWORD cBase = D3DCOLOR_XRGB(CS_ClampB(CS_Ftoi(br * 140.f * alpha)), CS_ClampB(CS_Ftoi(br * 20.f * alpha)), CS_ClampB(CS_Ftoi(br * 100.f * alpha)));
        DWORD cMid = D3DCOLOR_XRGB(CS_ClampB(CS_Ftoi(br * 255.f * alpha)), CS_ClampB(CS_Ftoi(br * 80.f * alpha)), CS_ClampB(CS_Ftoi(br * 200.f * alpha)));
        DWORD cTip = D3DCOLOR_XRGB(CS_ClampB(CS_Ftoi(br * 255.f * alpha)), CS_ClampB(CS_Ftoi(br * 200.f * alpha)), CS_ClampB(CS_Ftoi(br * 255.f * alpha)));
        CV base[6], mid[6];
        for (int k = 0; k < 6; ++k) {
            float a2 = (float)k / 6.f * CS_TAU;
            float tx2 = bx * cosf(a2) * rad, ty2 = by * cosf(a2) * rad, tz2 = sinf(a2) * rad;
            CV b2 = { wx + tx2, wy + ty2, zp + tz2, cBase }; base[k] = b2;
            CV m2 = { wx + tx2 * 0.6f + nx2 * sLen, wy + ty2 * 0.6f + ny2 * sLen, zp + tz2 * 0.6f, cMid }; mid[k] = m2;
        }
        CV tip = { wx + nx2 * (sLen + cLen), wy + ny2 * (sLen + cLen), zp, cTip };
        for (int k = 0; k < 6; ++k) {
            int nn = (k + 1) % 6;
            CV q[6] = { base[k],base[nn],mid[k],base[nn],mid[nn],mid[k] };
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(CV));
        }
        for (int k = 0; k < 6; ++k) {
            int nn = (k + 1) % 6;
            CV t3[3] = { mid[k], mid[nn], tip };
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t3, sizeof(CV));
        }
    }

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    CS_DisableFrom(0);
    D3DXMATRIX id; D3DXMatrixIdentity(&id);
    g_pDevice->SetTransform(D3DTS_WORLD, &id);
}

// --- Fog puffs ---------------------------------------------------------------
struct CS_FogPuff { float x, y, z, radius; BYTE alpha; };
static CS_FogPuff cs_puffs[CS_FOG_PUFFS];

static void CS_BuildFogDisc()
{
    SAFE_RELEASE(cs_fogVB); cs_fogTris = 0;
    if (FAILED(g_pDevice->CreateVertexBuffer(CS_FOG_VERTS * sizeof(CS_GVtx),
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &cs_fogVB))) return;
    int pi = 0;
    for (int p = 0; p < CS_FOG_PUFFS; ++p) {
        const CS_CDef& c = cs_def[pi % CS_ND]; pi++;
        float ang = (float)p * 2.3998f;
        float dist = c.rad * (1.0f + (float)(p % 3) * 0.6f);
        cs_puffs[p].x = c.tx + cosf(ang) * dist;
        cs_puffs[p].y = c.ty + 0.05f + (float)(p % 4) * 0.06f;
        cs_puffs[p].z = c.tz + sinf(ang) * dist;
        cs_puffs[p].radius = 0.28f + c.rad * 1.80f + (float)(p % 4) * 0.09f;
        cs_puffs[p].alpha = (BYTE)(110 + (p % 5) * 14);
    }
}

static void CS_UpdateFogVB(const D3DXMATRIX& view, DWORD tMs)
{
    if (!cs_fogVB) return;
    float rx = view._11, ry = view._21, rz = view._31;
    float ux = view._12, uy = view._22, uz = view._32;
    CS_GVtx* vb = NULL;
    if (FAILED(cs_fogVB->Lock(0, 0, (BYTE**)&vb, 0))) return;
    CS_GVtx* dst = vb; cs_fogTris = 0;
    const BYTE fR = 40, fG = 15, fB = 80;
    for (int p = 0; p < CS_FOG_PUFFS; ++p) {
        const CS_FogPuff& pf = cs_puffs[p];
        int ph = ((tMs / 8u) + p * 37) & (CS_LUT_N - 1);
        float pulse = 0.80f + cs_sin[ph] * 0.20f;
        BYTE a = (BYTE)CS_Ftoi((float)pf.alpha * pulse);
        DWORD cCore = D3DCOLOR_ARGB(a, fR, fG, fB);
        DWORD cEdge = D3DCOLOR_ARGB(0, fR, fG, fB);
        float r = pf.radius, cx = pf.x, cy = pf.y, cz = pf.z;
        float rpx = rx * r, rpy = ry * r, rpz = rz * r;
        float upx = ux * r, upy = uy * r, upz = uz * r;
        float tx2 = cx + upx, ty_ = cy + upy, tz2 = cz + upz;
        float bx2 = cx - upx, by2 = cy - upy, bz2 = cz - upz;
        float lx = cx - rpx, ly = cy - rpy, lz = cz - rpz;
        float wrx = cx + rpx, wry = cy + rpy, wrz = cz + rpz;
        dst->x = cx;  dst->y = cy;  dst->z = cz;  dst->color = cCore; dst++;
        dst->x = tx2; dst->y = ty_; dst->z = tz2; dst->color = cEdge; dst++;
        dst->x = lx;  dst->y = ly;  dst->z = lz;  dst->color = cEdge; dst++;
        dst->x = cx;  dst->y = cy;  dst->z = cz;  dst->color = cCore; dst++;
        dst->x = wrx; dst->y = wry; dst->z = wrz; dst->color = cEdge; dst++;
        dst->x = tx2; dst->y = ty_; dst->z = tz2; dst->color = cEdge; dst++;
        dst->x = cx;  dst->y = cy;  dst->z = cz;  dst->color = cCore; dst++;
        dst->x = bx2; dst->y = by2; dst->z = bz2; dst->color = cEdge; dst++;
        dst->x = wrx; dst->y = wry; dst->z = wrz; dst->color = cEdge; dst++;
        dst->x = cx;  dst->y = cy;  dst->z = cz;  dst->color = cCore; dst++;
        dst->x = lx;  dst->y = ly;  dst->z = lz;  dst->color = cEdge; dst++;
        dst->x = bx2; dst->y = by2; dst->z = bz2; dst->color = cEdge; dst++;
        cs_fogTris += 4;
    }
    cs_fogVB->Unlock();
}

// --- Build geometry ----------------------------------------------------------
static bool CS_BuildGeometry()
{
    SAFE_RELEASE(cs_crystalVB); cs_crystalTris = 0;
    if (!g_pDevice) { CS_SetInitError("BuildGeometry: no device"); return false; }
    const int TRIS_PER_PRISM = 24 * 64;
    const int TOTAL_TRIS = CS_ND * TRIS_PER_PRISM;
    const int TOTAL_VERTS = TOTAL_TRIS * 3;
    const int VB_BYTES = TOTAL_VERTS * (int)sizeof(CS_CVtx);

    HRESULT hr = g_pDevice->CreateVertexBuffer(VB_BYTES, D3DUSAGE_WRITEONLY,
        0, D3DPOOL_MANAGED, &cs_crystalVB);
    if (FAILED(hr) || !cs_crystalVB)
        hr = g_pDevice->CreateVertexBuffer(VB_BYTES, D3DUSAGE_WRITEONLY,
            0, D3DPOOL_DEFAULT, &cs_crystalVB);
    if (FAILED(hr) || !cs_crystalVB)
    {
        CS_SetInitError("BuildGeometry: CreateVB failed");
        return false;
    }
    CS_CVtx* cvb = NULL;
    hr = cs_crystalVB->Lock(0, 0, (BYTE**)&cvb, 0);
    if (FAILED(hr) || !cvb)
    {
        CS_SetInitError("BuildGeometry: Lock failed");
        SAFE_RELEASE(cs_crystalVB);
        return false;
    }
    CS_CVtx* cdst = cvb;
    for (int i = 0; i < CS_ND; ++i) {
        const CS_CDef& c = cs_def[i];
        int n = CS_BuildPrism(cdst, c.shH, c.capH, c.rad, c.sc,
            c.rx, c.ry, c.rz, c.tx, c.ty, c.tz);
        cs_crystalTris += n;
        cdst += n * 3;
    }
    cs_crystalVB->Unlock();
    return true;
}

// --- Gen textures ------------------------------------------------------------
static void CS_GenTextures()
{
    SAFE_RELEASE(cs_normalMap); SAFE_RELEASE(cs_cubeMap);

    if (FAILED(D3DXCreateTextureFromFileA(g_pDevice,
        "D:\\tex\\crystal_n.dds", &cs_normalMap)) || !cs_normalMap)
    {
        g_pDevice->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &cs_normalMap);
        if (cs_normalMap) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(cs_normalMap->LockRect(0, &lr, NULL, 0)) && lr.pBits) {
                DWORD* p = (DWORD*)lr.pBits;
                for (int i = 0; i < 16; ++i)
                    p[i] = D3DCOLOR_ARGB(255, 128, 128, 255);
                cs_normalMap->UnlockRect(0);
            }
        }
    }

    if (FAILED(D3DXCreateCubeTextureFromFileA(g_pDevice,
        "D:\\tex\\crystal_cube.dds", &cs_cubeMap)) || !cs_cubeMap)
    {
        g_pDevice->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &cs_cubeMap);
        if (cs_cubeMap) {
            DWORD cols[6] = {
                0xFFD23CD2, 0xFF28DCFF, 0xFF821EDB,
                0xFFFF50B4, 0xFFC8DCFF, 0xFF050010
            };
            for (int face = 0; face < 6; ++face) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(cs_cubeMap->LockRect(
                    (D3DCUBEMAP_FACES)face, 0, &lr, NULL, 0)) && lr.pBits) {
                    DWORD* p = (DWORD*)lr.pBits;
                    for (int i = 0; i < 16; ++i) p[i] = cols[face];
                    cs_cubeMap->UnlockRect((D3DCUBEMAP_FACES)face, 0);
                }
            }
        }
    }
}

// --- Render cluster — all 16 passes ------------------------------------------
static void CS_RenderCluster(DWORD tMs, float alpha, const D3DXMATRIX& world)
{
    if (!cs_crystalVB || cs_crystalTris <= 0) return;
    float t = (float)tMs * 0.001f;

    D3DXVECTOR3 eye(0.f, 0.3f, -5.f), at(0.f, 0.f, 0.f), up(0.f, 1.f, 0.f);
    D3DXMATRIX view, proj;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, CS_PI / 3.2f, CS_SW / CS_SH, 0.1f, 50.f);
    g_pDevice->SetTransform(D3DTS_WORLD, &world);
    g_pDevice->SetTransform(D3DTS_VIEW, &view);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &proj);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetVertexShader(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
    g_pDevice->SetStreamSource(0, cs_crystalVB, sizeof(CS_CVtx));
    CS_SetLinear(0); CS_SetLinear(1);

    // PASS 1 — opaque DOT3 base
    CS_SetDOT3TFactor(tMs);
    {
        int phK = (tMs / 7u) & (CS_LUT_N - 1);
        float kp = 0.85f + cs_sin[phK] * 0.15f;
        BYTE kR = CS_ClampB(CS_Ftoi(kp * 140.f * alpha));
        BYTE kG = CS_ClampB(CS_Ftoi(kp * 40.f * alpha));
        BYTE kB = CS_ClampB(CS_Ftoi(kp * 140.f * alpha));
        g_pDevice->SetTexture(0, cs_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
        g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(kR, kG, kB));
        g_pDevice->SetTexture(1, NULL);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        CS_DisableFrom(2);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, cs_crystalTris); ++cs_drawCalls;
    }

    // Switch to additive for passes 2-16
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

#define CS_DOT3_PASS(la_expr, sx_expr, sy_expr, sz_expr, rR_expr, rG_expr, rB_expr) \
    { float la = (la_expr), sx = (sx_expr), sy = (sy_expr), sz = (sz_expr); \
      float sl = sqrtf(sx*sx+sy*sy+sz*sz); if(sl<1e-4f)sl=1.f; sx/=sl;sy/=sl;sz/=sl; \
      BYTE sR=(rR_expr),sG=(rG_expr),sB=(rB_expr); (void)la; \
      g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR,D3DCOLOR_XRGB(sR,sG,sB)); \
      g_pDevice->SetTexture(0,cs_normalMap); \
      g_pDevice->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_DOTPRODUCT3); \
      g_pDevice->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE); \
      g_pDevice->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_TFACTOR); \
      g_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_DISABLE); \
      g_pDevice->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0); \
      CS_DisableFrom(1); \
      g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST,0,cs_crystalTris); ++cs_drawCalls; }

#define CS_CUBE_PASS(ph_div, rp_base, rp_amp, rR_expr, rG_expr, rB_expr) \
    { int ph=(tMs/(ph_div##u))&(CS_LUT_N-1); float rp=(rp_base)+cs_sin[ph]*(rp_amp); \
      BYTE cR=(rR_expr),cG=(rG_expr),cB=(rB_expr); \
      g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR,D3DCOLOR_XRGB(cR,cG,cB)); \
      g_pDevice->SetTexture(0,cs_cubeMap); \
      g_pDevice->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE); \
      g_pDevice->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE); \
      g_pDevice->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_TFACTOR); \
      g_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_DISABLE); \
      g_pDevice->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR); \
      g_pDevice->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_COUNT3); \
      CS_DisableFrom(1); \
      g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST,0,cs_crystalTris); ++cs_drawCalls; \
      g_pDevice->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0); \
      g_pDevice->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE); }

    // PASS 2 — DOT3 cyan+violet averaged
    {
        float la = t * 0.22f + CS_PI, flx = cosf(la), fly = -0.5f, flz = sinf(la);
        float fl = sqrtf(flx * flx + fly * fly + flz * flz); flx /= fl; fly /= fl; flz /= fl;
        float la2 = t * 0.22f + CS_PI * 0.5f, sx = cosf(la2), sy = 0.7f, sz = sinf(la2);
        float sl = sqrtf(sx * sx + sy * sy + sz * sz); sx /= sl; sy /= sl; sz /= sl;
        BYTE fR = CS_ClampB(CS_Ftoi(((flx + 1.f) * .5f + (sx + 1.f) * .5f) * .5f * 255.f));
        BYTE fG = CS_ClampB(CS_Ftoi(((-fly + 1.f) * .5f + (-sy + 1.f) * .5f) * .5f * 180.f));
        BYTE fB = CS_ClampB(CS_Ftoi(((flz + 1.f) * .5f + (sz + 1.f) * .5f) * .5f * 255.f));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(fR, fG, fB));
        g_pDevice->SetTexture(0, cs_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        CS_DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, cs_crystalTris); ++cs_drawCalls;
    }

    // PASS 3 — cubemap reflection merged
    {
        int ph = (tMs / 6u) & (CS_LUT_N - 1);  float rp = 0.35f + cs_sin[ph] * 0.10f;
        int ph2 = (tMs / 13u) & (CS_LUT_N - 1); float rp2 = 0.25f + cs_sin[ph2] * 0.10f;
        int ph3 = (tMs / 9u) & (CS_LUT_N - 1); float rp3 = 0.20f + cs_sin[ph3] * 0.12f;
        BYTE cR = CS_ClampB(CS_Ftoi((rp * 255.f + rp2 * 130.f + rp3 * 255.f) / 3.f * alpha));
        BYTE cG = CS_ClampB(CS_Ftoi((rp * 255.f + rp2 * 30.f + rp3 * 80.f) / 3.f * alpha));
        BYTE cB = CS_ClampB(CS_Ftoi((rp * 255.f + rp2 * 220.f + rp3 * 180.f) / 3.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(cR, cG, cB));
        g_pDevice->SetTexture(0, cs_cubeMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        CS_DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, cs_crystalTris); ++cs_drawCalls;
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }

    // PASS 4 — rim DOT3 averaged
    {
        float la = t * 0.22f + CS_PI * 1.5f, rx = cosf(la), ry = -0.2f, rz = sinf(la);
        float rl = sqrtf(rx * rx + ry * ry + rz * rz); rx /= rl; ry /= rl; rz /= rl;
        float la2 = t * 0.31f + CS_PI * 0.75f, rx2 = cosf(la2), ry2 = 0.3f, rz2 = sinf(la2);
        float rl2 = sqrtf(rx2 * rx2 + ry2 * ry2 + rz2 * rz2); rx2 /= rl2; ry2 /= rl2; rz2 /= rl2;
        BYTE rR = CS_ClampB(CS_Ftoi(((rx + 1.f) * .5f + (rx2 + 1.f) * .5f) * .5f * 180.f));
        BYTE rG = CS_ClampB(CS_Ftoi(((-ry + 1.f) * .5f + (-ry2 + 1.f) * .5f) * .5f * 180.f));
        BYTE rB = CS_ClampB(CS_Ftoi(((rz + 1.f) * .5f + (rz2 + 1.f) * .5f) * .5f * 255.f));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(rR, rG, rB));
        g_pDevice->SetTexture(0, cs_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        CS_DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, cs_crystalTris); ++cs_drawCalls;
    }

    // PASS 5 — violet specular DOT3
    CS_DOT3_PASS(t * 0.13f + CS_PI * 1.25f, cosf(la), -0.5f, sinf(la),
        CS_ClampB(CS_Ftoi((sx + 1.f) * 100.f * alpha)),
        CS_ClampB(CS_Ftoi((-sy + 1.f) * 30.f * alpha)),
        CS_ClampB(CS_Ftoi((sz + 1.f) * 127.5f * alpha)))

        // PASS 6 — cyan glint cubemap
        CS_CUBE_PASS(4, 0.12f, 0.10f,
            CS_ClampB(CS_Ftoi(rp * 40.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 220.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 255.f * alpha)))

        // PASS 7 — hot pink DOT3
        CS_DOT3_PASS(t * 0.29f + CS_PI * 0.33f, cosf(la), 0.6f, sinf(la),
            CS_ClampB(CS_Ftoi((sx + 1.f) * 127.5f * alpha)),
            CS_ClampB(CS_Ftoi((-sy + 1.f) * 40.f * alpha)),
            CS_ClampB(CS_Ftoi((sz + 1.f) * 110.f * alpha)))

        // PASS 8 — violet cubemap sweep
        CS_CUBE_PASS(17, 0.20f, 0.15f,
            CS_ClampB(CS_Ftoi(rp * 130.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 30.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 220.f * alpha)))

        // PASS 9 — warm pink DOT3
        CS_DOT3_PASS(t * 0.17f + CS_PI * 0.6f, cosf(la), -0.8f, sinf(la),
            CS_ClampB(CS_Ftoi((sx + 1.f) * 127.5f * alpha)),
            CS_ClampB(CS_Ftoi((-sy + 1.f) * 50.f * alpha)),
            CS_ClampB(CS_Ftoi((sz + 1.f) * 90.f * alpha)))

        // PASS 10 — teal cubemap
        CS_CUBE_PASS(11, 0.18f, 0.12f,
            CS_ClampB(CS_Ftoi(rp * 20.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 200.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 180.f * alpha)))

        // PASS 11 — deep blue rim DOT3
        CS_DOT3_PASS(t * 0.41f + CS_PI * 1.1f, cosf(la), 0.5f, sinf(la),
            CS_ClampB(CS_Ftoi((sx + 1.f) * 40.f * alpha)),
            CS_ClampB(CS_Ftoi((-sy + 1.f) * 80.f * alpha)),
            CS_ClampB(CS_Ftoi((sz + 1.f) * 127.5f * alpha)))

        // PASS 12 — white shimmer cubemap
        CS_CUBE_PASS(5, 0.15f, 0.10f,
            CS_ClampB(CS_Ftoi(rp * 200.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 200.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 200.f * alpha)))

        // PASS 13 — green-blue DOT3
        CS_DOT3_PASS(t * 0.23f + CS_PI * 1.8f, cosf(la), 0.3f, sinf(la),
            CS_ClampB(CS_Ftoi((sx + 1.f) * 30.f * alpha)),
            CS_ClampB(CS_Ftoi((-sy + 1.f) * 127.5f * alpha)),
            CS_ClampB(CS_Ftoi((sz + 1.f) * 127.5f * alpha)))

        // PASS 14 — deep violet cubemap pulse
        CS_CUBE_PASS(19, 0.22f, 0.18f,
            CS_ClampB(CS_Ftoi(rp * 160.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 20.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 255.f * alpha)))

        // PASS 15 — orange-red DOT3
        CS_DOT3_PASS(t * 0.37f + CS_PI * 0.9f, cosf(la), -0.4f, sinf(la),
            CS_ClampB(CS_Ftoi((sx + 1.f) * 127.5f * alpha)),
            CS_ClampB(CS_Ftoi((-sy + 1.f) * 60.f * alpha)),
            CS_ClampB(CS_Ftoi((sz + 1.f) * 30.f * alpha)))

        // PASS 16 — gold cubemap sweep
        CS_CUBE_PASS(23, 0.20f, 0.15f,
            CS_ClampB(CS_Ftoi(rp * 255.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 160.f * alpha)),
            CS_ClampB(CS_Ftoi(rp * 30.f * alpha)))

#undef CS_DOT3_PASS
#undef CS_CUBE_PASS

        // Fog puffs
        D3DXMATRIX view2;
    D3DXVECTOR3 eye2(0.f, 0.3f, -5.f), at2(0.f, 0.f, 0.f), up2(0.f, 1.f, 0.f);
    D3DXMatrixLookAtLH(&view2, &eye2, &at2, &up2);
    CS_UpdateFogVB(view2, tMs);
    if (cs_fogVB && cs_fogTris > 0) {
        D3DXMATRIX fogWorld; D3DXMatrixIdentity(&fogWorld);
        g_pDevice->SetTransform(D3DTS_WORLD, &fogWorld);
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_pDevice->SetTexture(0, NULL);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        CS_DisableFrom(1);
        g_pDevice->SetVertexShader(CS_GVFVF);
        g_pDevice->SetStreamSource(0, cs_fogVB, sizeof(CS_GVtx));
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, cs_fogTris); ++cs_drawCalls;
    }

    // Restore
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    CS_DisableFrom(0);
    D3DXMATRIX id; D3DXMatrixIdentity(&id);
    g_pDevice->SetTransform(D3DTS_WORLD, &id);
    g_pDevice->SetTransform(D3DTS_VIEW, &id);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &id);
}

// --- Scene entry points (renamed from CrystalScene_*) -----------------------
static void GPU_SceneInit()
{
    cs_startTicks = GetTickCount();
    cs_lastTick = 0;
    cs_fps = 0.f;
    cs_frameMs = 16.f;
    cs_initOK = false;
    cs_initErr[0] = 0;

    CS_BuildLUT();
    if (!g_pDevice) { CS_SetInitError("no device"); return; }
    if (!CS_BuildGeometry()) return;
    CS_BuildCave();
    CS_BuildFogDisc();
    CS_GenTextures();
    CS_CreateVS();
    cs_initOK = true;
}

static void GPU_SceneShutdown()
{
    CS_DeleteVS();
    SAFE_RELEASE(cs_crystalVB);
    SAFE_RELEASE(cs_fogVB);
    SAFE_RELEASE(cs_caveVB);
    SAFE_RELEASE(cs_caveIB);
    SAFE_RELEASE(cs_caveTex);
    SAFE_RELEASE(cs_normalMap);
    SAFE_RELEASE(cs_cubeMap);
    cs_initOK = false;
}

static void GPU_SceneRender()
{
    if (!g_pDevice) return;

    DWORD fNow = GetTickCount();
    if (cs_lastTick != 0) {
        float dt = (float)(fNow - cs_lastTick);
        if (dt > 0.f) {
            cs_frameMs = cs_frameMs * 0.85f + dt * 0.15f;
            cs_fps = cs_fps * 0.85f + (1000.f / dt) * 0.15f;
        }
    }
    else { cs_frameMs = 16.f; cs_fps = 60.f; }
    cs_lastTick = fNow;
    cs_drawCalls = 0;

    DWORD tMs = GetTickCount() - cs_startTicks;
    float t = (float)tMs * 0.001f;
    float tFull = (float)SCENE_LOOP_MS * 0.001f;
    float alpha = 1.f;
    if (t < 1.5f)              alpha = t / 1.5f;
    else if (t > tFull - 1.5f)      alpha = (tFull - t) / 1.5f;
    alpha = CS_Clamp01(alpha);

    float ry = t * (CS_TAU / 30.f);
    float rx = 0.15f * CS_LSin(t * 0.4f);
    D3DXMATRIX mRX, mRY, world;
    D3DXMatrixRotationX(&mRX, rx);
    D3DXMatrixRotationY(&mRY, ry);
    world = mRX * mRY;

    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);

    if (cs_initOK)
        CS_RenderCluster(tMs, alpha, world);

    CS_DrawCave(tMs, alpha);
    CS_DrawCave(tMs + 40, alpha * 0.4f);
    CS_DrawCave(tMs + 80, alpha * 0.2f);
}

// ============================================================================
// Stress card state — declared here so GpuStress_AutoRun and ST_GPU_Render
// can both access them without forward declaration issues.
// ============================================================================

StressState s_gpuState = SSTATE_IDLE;
bool        s_gpuAbortHold = false;
DWORD       s_gpuAbortHoldStart = 0;

static DWORD s_gpuStartMs = 0;
static int   s_gpuLoops = 0;
static float s_gpuPeakFPS = 0.f;
static float s_gpuMinFPS = 9999.f;
static float s_gpuLastFPS = 0.f;
static DWORD s_gpuLastTick = 0;
static DWORD s_gpuSceneStart = 0;

// ============================================================================
// GpuStress_AutoRun — headless GPU stress for XbSet automation
// Runs the CrystalScene for durationMs then writes results to hReport.
// No interactive UI — renders a minimal status screen while running.
// ============================================================================

static void GPU_WriteReportLine(HANDLE hf, const char* lbl, const char* val)
{
    char line[128]; DWORD w;
    StrCopy(line, sizeof(line), lbl);
    StrCat2(line, sizeof(line), line, val);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
}

void GpuStress_AutoRun(HANDLE hReport, DWORD durationMs)
{
    // Reset stats
    s_gpuLoops = 0;
    s_gpuPeakFPS = 0.f;
    s_gpuMinFPS = 9999.f;
    s_gpuLastFPS = 0.f;
    s_gpuLastTick = 0;

    GPU_SceneInit();

    DWORD startMs = GetTickCount();
    DWORD endMs = startMs + durationMs;
    DWORD sceneStart = startMs;
    DWORD nextStatus = 0;

    while (GetTickCount() < endMs)
    {
        DWORD now = GetTickCount();

        // Loop scene every SCENE_LOOP_MS
        if ((now - sceneStart) >= SCENE_LOOP_MS)
        {
            ++s_gpuLoops;
            GPU_SceneShutdown();
            GPU_SceneInit();
            sceneStart = GetTickCount();
        }

        // Frame timing
        if (s_gpuLastTick != 0)
        {
            float dt = (float)(now - s_gpuLastTick);
            if (dt > 0.f)
            {
                float fps = 1000.f / dt;
                s_gpuLastFPS = s_gpuLastFPS * 0.85f + fps * 0.15f;
                if (s_gpuLastFPS > s_gpuPeakFPS) s_gpuPeakFPS = s_gpuLastFPS;
                if (s_gpuLastFPS < s_gpuMinFPS && s_gpuLastFPS > 1.f)
                    s_gpuMinFPS = s_gpuLastFPS;
            }
        }
        else { s_gpuLastFPS = 60.f; }
        s_gpuLastTick = now;

        // Render scene frame
        if (g_pDevice)
        {
            g_pDevice->BeginScene();
            GPU_SceneRender();

            // Simple status overlay every ~500ms so screen doesn't appear frozen
            if (now >= nextStatus)
            {
                DWORD remain = (now < endMs) ? (endMs - now) / 1000 : 0;
                int   cfps = CS_Ftoi(s_gpuLastFPS);
                char  rm[8], fp[8];
                IntToStr((int)remain, rm, sizeof(rm));
                IntToStr(cfps, fp, sizeof(fp));

                // Restore 2D state for text
                g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                g_pDevice->SetTexture(0, NULL);
                g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
                g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
                g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

                char statBuf[64];
                statBuf[0] = 0;
                StrCat2(statBuf, sizeof(statBuf), statBuf, "GPU STRESS (AUTO)  Remaining: ");
                StrCat2(statBuf, sizeof(statBuf), statBuf, rm);
                StrCat2(statBuf, sizeof(statBuf), statBuf, "s   FPS: ");
                StrCat2(statBuf, sizeof(statBuf), statBuf, fp);
                DrawText(8.f, 8.f, statBuf, 1.3f, D3DCOLOR_XRGB(0, 220, 80));
                nextStatus = now + 500;
            }

            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
        }
    }

    GPU_SceneShutdown();

    if (!hReport || hReport == INVALID_HANDLE_VALUE) return;

    char t[16];
    DWORD totalSecs = durationMs / 1000;
    char mm2[8], ss2[8];
    IntToStr((int)(totalSecs / 60), mm2, sizeof(mm2));
    IntToStr((int)(totalSecs % 60), ss2, sizeof(ss2));
    StrCopy(t, sizeof(t), mm2);
    StrCat2(t, sizeof(t), t, "m ");
    StrCat2(t, sizeof(t), t, ss2);
    StrCat2(t, sizeof(t), t, "s");
    GPU_WriteReportLine(hReport, "Duration:      ", t);

    char lb[8]; IntToStr(s_gpuLoops, lb, sizeof(lb));
    GPU_WriteReportLine(hReport, "Scene loops:   ", lb);

    char pk[8]; IntToStr(CS_Ftoi(s_gpuPeakFPS), pk, sizeof(pk));
    StrCat2(pk, sizeof(pk), pk, " fps");
    GPU_WriteReportLine(hReport, "Peak FPS:      ", pk);

    char mn[8];
    IntToStr((s_gpuMinFPS < 9000.f) ? CS_Ftoi(s_gpuMinFPS) : 0, mn, sizeof(mn));
    StrCat2(mn, sizeof(mn), mn, " fps");
    GPU_WriteReportLine(hReport, "Min FPS:       ", mn);

    GPU_WriteReportLine(hReport, "Result:        ",
        (s_gpuMinFPS >= 20.f) ? "PASS" :
        (s_gpuMinFPS >= 10.f) ? "WARNING - min FPS below 20" :
        "FAIL - min FPS critically low");
}

DWORD GpuAutoRun_GetLoops() { return (DWORD)s_gpuLoops; }
float GpuAutoRun_GetPeakFPS() { return s_gpuPeakFPS; }
float GpuAutoRun_GetMinFPS() { return s_gpuMinFPS; }

// ============================================================================
// Public API
// ============================================================================

void ST_GPU_OnStart()
{
    s_gpuStartMs = 0;
    s_gpuLoops = 0;
    s_gpuPeakFPS = 0.f;
    s_gpuMinFPS = 9999.f;
    s_gpuLastFPS = 0.f;
    s_gpuLastTick = 0;
    s_gpuSceneStart = 0;
}

void ST_GPU_Stop()
{
    GPU_SceneShutdown();
    s_gpuState = SSTATE_IDLE;
    s_gpuAbortHold = false;
}

void ST_GPU_Render(const DiagLogo& logo)
{
    if (!g_pDevice) return;

    // ---- IDLE ---------------------------------------------------------------
    if (s_gpuState == SSTATE_IDLE)
    {
        g_pDevice->BeginScene();
        DrawPageChrome(logo, "STRESS TEST  -  GPU",
            "[Left/Right] Card    [A] Start    [B] Back");
        ST_DrawTabStrip(CARD_GPU);

        float y = ST_PANEL_TOP + 10.f;
        DrawText(LM, y, "GPU STRESS TEST", 1.5f, COL_YELLOW); y += LINE_H * 1.5f;
        DrawText(LM, y, "Runs the Crystalline Grotto scene continuously as a full GPU", 1.2f, COL_WHITE); y += LINE_H;
        DrawText(LM, y, "workload. Pushes NV2A fill-rate, texture bandwidth, and", 1.2f, COL_WHITE); y += LINE_H;
        DrawText(LM, y, "vertex throughput across 16 render passes per frame.", 1.2f, COL_WHITE); y += LINE_H * 1.5f;
        DrawText(LM, y, "Per-frame workload:", 1.2f, COL_CYAN); y += LINE_H;
        DrawText(LM + 16.f, y, "~344K tri submissions  (crystal cluster, 16 passes)", 1.15f, COL_GRAY); y += LINE_H;
        DrawText(LM + 16.f, y, "~110K tri submissions  (cave mesh x3)", 1.15f, COL_GRAY); y += LINE_H;
        DrawText(LM + 16.f, y, "120 billboarded fog quads  (CPU-updated)", 1.15f, COL_GRAY); y += LINE_H;
        DrawText(LM + 16.f, y, "DOT3 bump map + cubemap reflection passes", 1.15f, COL_GRAY); y += LINE_H * 1.5f;
        DrawText(LM, y, "Scene loops every 20s. Abort: hold [Back]+[A] for 5 seconds.", 1.2f, COL_WHITE); y += LINE_H * 1.5f;
        DrawText(LM, y, "Textures: D:\\tex\\crystal_n.dds  crystal_cube.dds  rock.dds", 1.1f, COL_GRAY);

        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    // ---- CONFIRM ------------------------------------------------------------
    if (s_gpuState == SSTATE_CONFIRM)
    {
        g_pDevice->BeginScene();
        DrawPageChrome(logo, "STRESS TEST  -  GPU",
            "Hold LT+RT to confirm    [B] Cancel");
        ST_DrawTabStrip(CARD_GPU);

        float y = ST_PANEL_TOP + 20.f;
        DrawText(LM, y, "GPU STRESS TEST  --  CONFIRM START", 1.4f, COL_YELLOW); y += LINE_H * 1.5f;
        DrawText(LM, y, "This test runs a demanding 3D scene continuously until aborted.", 1.2f, COL_WHITE); y += LINE_H;
        DrawText(LM, y, "The scene will take over the full screen.", 1.2f, COL_WHITE); y += LINE_H * 1.5f;
        DrawText(LM, y, "Hold LT + RT together to begin.", 1.3f, COL_CYAN); y += LINE_H;
        DrawText(LM, y, "[B] to cancel.", 1.2f, COL_GRAY);

        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    // ---- RUNNING ------------------------------------------------------------
    // First frame: init scene and record start
    if (s_gpuStartMs == 0)
    {
        s_gpuStartMs = GetTickCount();
        s_gpuSceneStart = s_gpuStartMs;
        GPU_SceneInit();
    }

    // Card-level frame timing (separate from scene-internal cs_fps)
    DWORD fNow = GetTickCount();
    if (s_gpuLastTick != 0)
    {
        float dt = (float)(fNow - s_gpuLastTick);
        if (dt > 0.f)
        {
            float fps = 1000.f / dt;
            s_gpuLastFPS = s_gpuLastFPS * 0.85f + fps * 0.15f;
            if (s_gpuLastFPS > s_gpuPeakFPS) s_gpuPeakFPS = s_gpuLastFPS;
            if (s_gpuLastFPS < s_gpuMinFPS && s_gpuLastFPS > 1.f)
                s_gpuMinFPS = s_gpuLastFPS;
        }
    }
    else { s_gpuLastFPS = 60.f; }
    s_gpuLastTick = fNow;

    // Loop: restart scene every SCENE_LOOP_MS
    if ((fNow - s_gpuSceneStart) >= SCENE_LOOP_MS)
    {
        ++s_gpuLoops;
        GPU_SceneShutdown();
        GPU_SceneInit();
        s_gpuSceneStart = GetTickCount();
    }

    // Render scene — owns Clear and draw calls, not BeginScene/EndScene
    g_pDevice->BeginScene();
    GPU_SceneRender();

    // ---- Overlay (drawn on top of scene in screen space) --------------------
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

    // Semi-transparent bottom strip — two rows tall
    {
        struct OV { float x, y, z, rhw; DWORD c; };
        DWORD bg = D3DCOLOR_ARGB(180, 8, 10, 24);
        const float BX = 0.f, BY = SY(BOT_BAR_Y - 38.f);
        const float BW = SX(SW), BH = SY(38.f);
        OV v[4] = {
            {BX,    BY,    0.f, 1.f, bg},
            {BX + BW, BY,    0.f, 1.f, bg},
            {BX,    BY + BH, 0.f, 1.f, bg},
            {BX + BW, BY + BH, 0.f, 1.f, bg}
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(OV));
    }

    // Abort hold bar — sits flush above the strip
    if (s_gpuAbortHold)
    {
        DWORD held = GetTickCount() - s_gpuAbortHoldStart;
        float frac = (held >= ABORT_HOLD_MS) ? 1.f
            : (float)held / (float)ABORT_HOLD_MS;
        struct OV { float x, y, z, rhw; DWORD c; };
        DWORD bg = D3DCOLOR_ARGB(160, 30, 10, 10);
        DWORD fg = D3DCOLOR_ARGB(220, 200, 30, 30);
        const float AX = 0.f, AY = SY(BOT_BAR_Y - 42.f);
        const float AW = SX(SW), AH = SY(4.f);
        OV track[4] = {
            {AX,    AY,    0,1,bg}, {AX + AW, AY,    0,1,bg},
            {AX,    AY + AH, 0,1,bg}, {AX + AW, AY + AH, 0,1,bg}
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, track, sizeof(OV));
        float fw = frac * AW;
        if (fw > 1.f)
        {
            OV fill[4] = {
                {AX,    AY,    0,1,fg}, {AX + fw, AY,    0,1,fg},
                {AX,    AY + AH, 0,1,fg}, {AX + fw, AY + AH, 0,1,fg}
            };
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, fill, sizeof(OV));
        }
    }

    // Restore for text
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    // ---- Info strip — two rows in design space (DrawText applies g_sx/g_sy) --
    // Row 1 (top): ELAPSED  LOOPS  FPS  PEAK  MIN
    // Row 2 (bot): CPU  FAN  [right-aligned] abort hint

    DWORD elSec = s_gpuStartMs > 0
        ? (GetTickCount() - s_gpuStartMs) / 1000 : 0;
    char mm[6], ss[4];
    IntToStr((int)(elSec / 60), mm, sizeof(mm));
    IntToStr((int)(elSec % 60), ss, sizeof(ss));
    char elBuf[20]; elBuf[0] = 0;
    StrCat2(elBuf, sizeof(elBuf), elBuf, mm);
    StrCat2(elBuf, sizeof(elBuf), elBuf, "m ");
    if ((elSec % 60) < 10) StrCat2(elBuf, sizeof(elBuf), elBuf, "0");
    StrCat2(elBuf, sizeof(elBuf), elBuf, ss);
    StrCat2(elBuf, sizeof(elBuf), elBuf, "s");

    int fps = CS_Ftoi(s_gpuLastFPS);
    int peak = CS_Ftoi(s_gpuPeakFPS);
    int minF = (s_gpuMinFPS < 9000.f) ? CS_Ftoi(s_gpuMinFPS) : 0;

    char fpsBuf[8], peakBuf[8], minBuf[8], loopBuf[8];
    char cpuBuf[8], fanBuf[8];
    IntToStr(fps, fpsBuf, sizeof(fpsBuf));
    IntToStr(peak, peakBuf, sizeof(peakBuf));
    IntToStr(minF, minBuf, sizeof(minBuf));
    IntToStr(s_gpuLoops, loopBuf, sizeof(loopBuf));
    IntToStr((int)s_curCPU, cpuBuf, sizeof(cpuBuf));
    IntToStr((int)s_curFan * 2, fanBuf, sizeof(fanBuf));

    // Scale: 1.0f keeps chars ~8px wide at 480p — fits comfortably on one row
    const float sc = 1.0f;
    const float row1 = BOT_BAR_Y - 35.f;  // upper row
    const float row2 = BOT_BAR_Y - 20.f;  // lower row
    const float pad = 6.f;               // gap between label+value pairs

    // Row 1: ELAPSED  LOOPS  FPS  PEAK  MIN  (left-anchored, flowing)
    float x = LM;
    DrawText(x, row1, "ELAPSED:", sc, COL_GRAY);
    x += TW("ELAPSED:", sc) + 2.f;
    DrawText(x, row1, elBuf, sc, COL_WHITE);
    x += TW(elBuf, sc) + pad;

    DrawText(x, row1, "LOOPS:", sc, COL_GRAY);
    x += TW("LOOPS:", sc) + 2.f;
    DrawText(x, row1, loopBuf, sc, COL_CYAN);
    x += TW(loopBuf, sc) + pad;

    DrawText(x, row1, "FPS:", sc, COL_GRAY);
    x += TW("FPS:", sc) + 2.f;
    DrawText(x, row1, fpsBuf, sc,
        fps >= 45 ? COL_GREEN : fps >= 30 ? COL_ORANGE : COL_RED);
    x += TW(fpsBuf, sc) + pad;

    DrawText(x, row1, "PEAK:", sc, COL_GRAY);
    x += TW("PEAK:", sc) + 2.f;
    DrawText(x, row1, peakBuf, sc, COL_GREEN);
    x += TW(peakBuf, sc) + pad;

    DrawText(x, row1, "MIN:", sc, COL_GRAY);
    x += TW("MIN:", sc) + 2.f;
    DrawText(x, row1, minBuf, sc,
        minF >= 30 ? COL_GREEN : minF >= 20 ? COL_ORANGE : COL_RED);

    // Row 2: CPU + FAN left, abort hint right-aligned
    if (s_sensorOK)
    {
        char cpuFull[10]; StrCopy(cpuFull, sizeof(cpuFull), cpuBuf);
        StrCat2(cpuFull, sizeof(cpuFull), cpuFull, "C");
        char fanFull[10]; StrCopy(fanFull, sizeof(fanFull), fanBuf);
        StrCat2(fanFull, sizeof(fanFull), fanFull, "%");
        DWORD cpuCol = ((int)s_curCPU > CPU_HOT) ? COL_RED :
            ((int)s_curCPU > CPU_WARN) ? COL_ORANGE : COL_GREEN;

        float x2 = LM;
        DrawText(x2, row2, "CPU:", sc, COL_GRAY);
        x2 += TW("CPU:", sc) + 2.f;
        DrawText(x2, row2, cpuFull, sc, cpuCol);
        x2 += TW(cpuFull, sc) + pad;
        DrawText(x2, row2, "FAN:", sc, COL_GRAY);
        x2 += TW("FAN:", sc) + 2.f;
        DrawText(x2, row2, fanFull, sc, COL_CYAN);
    }

    DrawTextR(SW - LM, row2,
        s_gpuAbortHold
        ? "ABORTING... RELEASE TO CANCEL"
        : "Hold [Back+A] 5s to Abort",
        sc, s_gpuAbortHold ? COL_RED : COL_DIM);

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}