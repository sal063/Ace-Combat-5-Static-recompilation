#include "rn_vp_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SH_MAX 1024u
#define SH_TRIS_PER_POINT 7u

static u64 st_batches, st_declined, st_tris, st_full, st_room;

static int mesh_wanted(void) {
    static int sw = -1;
    if (sw < 0) {
        const char *s = getenv("PS2_RN_SHADOW_MESH");
        sw = !(s && *s == '0');
    }
    return sw && vp_mesh_wanted();
}

static int cross_z_negative(const float *p0, const float *p1, const float *p2, const float *p3) {
    float d1x = p1[0] - p0[0], d1y = p1[1] - p0[1];
    float d2x = p3[0] - p2[0], d2y = p3[1] - p2[1];
    float acc = d1x * d2y;
    float z = acc - d2x * d1y;
    return signbit(z) != 0;
}

typedef struct { u16 pt; u8 ext; } sh_ref;

typedef struct {
    sh_ref v[16];
    u32 col[16];
    u32 n;
} sh_strip;

static void strip_col(sh_strip *s, u32 addr) { s->col[s->n] = addr; }
static void strip_vtx(sh_strip *s, u16 pt, u8 ext) {
    s->v[s->n].pt = pt;
    s->v[s->n].ext = ext;
    s->n++;
    s->col[s->n] = 0;
}

int rn_vp_shadow(vpx *x, u32 pc, u32 *kick_at) {
    static float P[SH_MAX][4], E[SH_MAX][4];
    static float obj[SH_MAX][4];
    static u8 recs[SH_MAX * 8u * 3u * 64u];
    static u32 idx[SH_MAX * 8u * 3u];
    u32 t, n, vi5, vi6, vi7 = 0, nt = 0, vbase, ifirst;
    int native, lit_prev = 0;
    u32 c26 = 0, c27 = 0, c28 = 0, c29 = 0;
    float *vf9;

    if (pc == 0x0000u) { x->tpc = 0x0010u; return 0; }
    if (pc != 0x0010u) { st_declined++; return -1; }
    t = x->top & VP_QW_MASK;
    n = vp_ilw(x, t, 0);
    if (n < 3u || n > SH_MAX) { st_declined++; return -1; }
    native = x->native && mesh_wanted();
    if (native && !ps2_vk_mesh_room((n - 2u) * SH_TRIS_PER_POINT * 3u,
                                    (n - 2u) * SH_TRIS_PER_POINT * 3u, 1u)) {
        st_room++;
        native = 0;
    }

    if ((s16)vp_ilw(x, 0x1B, 2) > 0) {
        float *v1, *v2, *v3;
        vp_isw(x, 0x1B, 2, 0);
        for (int k = 0; k < 4; k++) vp_lq(x, 14 + k, 0x28u + k, VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x10u + k, VP_MALL);
        for (int k = 0; k < 4; k++)
            vp_rows(vp_F(x, 5 + k), vp_F(x, 1), vp_F(x, 2), vp_F(x, 3), vp_F(x, 4),
                    vp_F(x, 14 + k)[0], vp_F(x, 14 + k)[1], vp_F(x, 14 + k)[2],
                    vp_F(x, 14 + k)[3], VP_MALL);
        vp_lq(x, 18, 0x1Cu, VP_MALL);
        v1 = vp_F(x, 1); v2 = vp_F(x, 2); v3 = vp_F(x, 3);
        for (int i = 0; i < 4; i++) {
            v1[i] = vp_F(x, 14)[i] * vp_F(x, 18)[i];
            v2[i] = vp_F(x, 15)[i] * vp_F(x, 18)[i];
            v3[i] = vp_F(x, 16)[i] * vp_F(x, 18)[i];
        }
        v1[0] = v1[0] + v1[1]; v2[1] = v2[1] + v2[0]; v3[2] = v3[2] + v3[0];
        v1[0] = v1[0] + v1[2]; v2[1] = v2[1] + v2[2]; v3[2] = v3[2] + v3[1];
        vf9 = vp_F(x, 9);
        vf9[0] = v1[0] + v1[3];
        vf9[1] = v2[1] + v2[3];
        vf9[2] = v3[2] + v3[3];
        vf9[3] = 0.0f;
    }
    vf9 = vp_F(x, 9);

    vi6 = (t + 1u + n + n - 2u) & VP_QW_MASK;
    vi5 = vi6;

    for (u32 i = 0; i < n; i++) {
        float v[4], e[4], q;
        vp_rd(x, t + 1u + i, v);
        memcpy(obj[i], v, 16);
        vp_rows(P[i], vp_F(x, 5), vp_F(x, 6), vp_F(x, 7), vp_F(x, 8), v[0], v[1], v[2], v[3],
                VP_MALL);
        q = 1.0f / P[i][3];
        P[i][0] *= q; P[i][1] *= q; P[i][2] *= q; P[i][3] = 0.0f;
        for (int k = 0; k < 4; k++) e[k] = v[k] + vf9[k];
        vp_rows(E[i], vp_F(x, 5), vp_F(x, 6), vp_F(x, 7), vp_F(x, 8), e[0], e[1], e[2], e[3],
                VP_MALL);
        q = 1.0f / E[i][3];
        E[i][0] *= q; E[i][1] *= q; E[i][2] *= q; E[i][3] = 0.0f;
        if (i < 2u) continue;
        {
            float nrm[4], d;
            int lit, first = i == 2u, change, base;
            int last = i == n - 1u;
            u32 a = i - 2u, b = i - 1u, c = i;
            sh_strip s1, s2;
            vp_rd(x, t + 1u + n + (i - 2u), nrm);
            d = nrm[0] * vf9[0];
            d = d + nrm[1] * vf9[1];
            d = d + nrm[2] * vf9[2];
            lit = signbit(d) != 0;
            change = !first && lit != lit_prev;
            lit_prev = lit;
            base = lit + (int)i;
            if (first || change) {
                int k = (base + !cross_z_negative(P[a], E[a], E[a], P[b])) & 1;
                c26 = first ? (k ? 0x05u : 0x07u) : (k ? 0x06u : 0x08u);
            }
            if (last) {
                int k = (base + !cross_z_negative(P[b], E[b], E[b], P[c])) & 1;
                c27 = k ? 0x05u : 0x07u;
            }
            {
                int k = (base + !cross_z_negative(P[c], E[c], E[c], P[a])) & 1;
                c28 = k ? 0x05u : 0x07u;
                k = (base + !cross_z_negative(E[a], E[b], E[b], E[c])) & 1;
                c29 = k ? 0x07u : 0x05u;
            }
            s1.n = 0; s1.col[0] = 0;
            s2.n = 0; s2.col[0] = 0;
            if ((first || change) && last) {
                strip_vtx(&s1, a, 0); strip_vtx(&s1, a, 1); strip_col(&s1, c26);
                strip_vtx(&s1, b, 0); strip_vtx(&s1, b, 1); strip_col(&s1, c27);
                strip_vtx(&s1, c, 0); strip_vtx(&s1, c, 1); strip_col(&s1, c28);
                strip_vtx(&s1, a, 0); strip_vtx(&s1, a, 1);
            } else if (first || change) {
                strip_vtx(&s1, c, 0); strip_vtx(&s1, c, 1); strip_col(&s1, c28);
                strip_vtx(&s1, a, 0); strip_vtx(&s1, a, 1); strip_col(&s1, c26);
                strip_vtx(&s1, b, 0); strip_vtx(&s1, b, 1);
            } else if (last) {
                strip_vtx(&s1, b, 0); strip_vtx(&s1, b, 1); strip_col(&s1, c27);
                strip_vtx(&s1, c, 0); strip_vtx(&s1, c, 1); strip_col(&s1, c28);
                strip_vtx(&s1, a, 0); strip_vtx(&s1, a, 1);
            } else {
                strip_vtx(&s1, c, 0); strip_vtx(&s1, c, 1); strip_col(&s1, c28);
                strip_vtx(&s1, a, 0); strip_vtx(&s1, a, 1);
            }
            strip_col(&s2, c29);
            strip_vtx(&s2, a, 1); strip_vtx(&s2, b, 1); strip_vtx(&s2, c, 1);

            if (native) {
                for (int si = 0; si < 2; si++) {
                    const sh_strip *s = si ? &s2 : &s1;
                    u32 col = 0;
                    for (u32 k = 0; k < s->n; k++) {
                        if (s->col[k]) col = s->col[k];
                        if (k < 2u || !col) continue;
                        if (nt >= SH_MAX * 8u) break;
                        for (u32 j = 0; j < 3u; j++) {
                            const sh_ref *r = &s->v[k - 2u + j];
                            float pos[4];
                            s32 rgba[4];
                            u8 *rec = recs + (nt * 3u + j) * 64u;
                            for (int l = 0; l < 3; l++)
                                pos[l] = obj[r->pt][l] + (r->ext ? vf9[l] : 0.0f);
                            pos[3] = 1.0f;
                            memcpy(rgba, vp_q(x, col), 16);
                            memset(rec, 0, 64);
                            memcpy(rec, pos, 16);
                            memcpy(rec + 32, rgba, 16);
                            idx[nt * 3u + j] = nt * 3u + j;
                        }
                        nt++;
                    }
                }
            } else {
                u32 hdr = (first || change) ? (last ? 0x00u : 0x01u) : (last ? 0x01u : 0x02u);
                u32 o = vi6 - vi5, k;
                pkt_putf(o++, (vp_lq(x, 1, hdr, VP_MALL), vp_F(x, 1)));
                for (k = 0; k < s1.n; k++) {
                    if (s1.col[k]) pkt_putf(o++, (vp_lq(x, 1, s1.col[k], VP_MALL), vp_F(x, 1)));
                    pkt_put_xyzf(o++, s1.v[k].ext ? E[s1.v[k].pt] : P[s1.v[k].pt], 0);
                }
                vi7 = vi5 + o;
                pkt_putf(o++, (vp_lq(x, 1, 0x03u, VP_MALL), vp_F(x, 1)));
                pkt_putf(o++, (vp_lq(x, 1, c29, VP_MALL), vp_F(x, 1)));
                for (k = 0; k < s2.n; k++)
                    pkt_put_xyzf(o++, E[s2.v[k].pt], 0);
                vi6 = vi5 + o;
            }
        }
    }
    x->tpc = 0x0010u;
    st_batches++;

    if (!native) {
        pkt_putf(vi7 - vi5, (vp_lq(x, 1, 0x04u, VP_MALL), vp_F(x, 1)));
        *kick_at = vi5;
        return 1;
    }
    if (!nt) return 0;
    if (ps2_vk_mesh_store(recs, nt * 3u, idx, nt * 3u, &vbase, &ifirst) != 0) {
        st_full++;
        return 0;
    }
    st_tris += nt;
    {
        ps2_vk_mesh_block b;
        ps2_vk_state st;
        u32 prim;
        rn_mesh_block_init(&b);
        for (int k = 0; k < 4; k++) memcpy(b.m + 4 * k, vp_F(x, 5 + k), 16);
        b.clip = 0u;
        ps2_gs_write_reg(0x00, vp_tag_prim(x, 0x02u, &prim) ? prim : 0x44u);
        ps2_gs_native_state(&st);
        ps2_vk_draw_mesh(&st, &b, vbase, ifirst, nt * 3u, 0);
    }
    return 0;
}

void rn_vp_shadow_report(void) {
    if (!st_batches && !st_declined) return;
    ps2_log("rn: native shadow volumes -- %llu batches (%llu triangles by mesh.vert), "
            "%llu declined; %llu drawn as the packet for mesh arena space, %llu "
            "with triangles not stored",
            (unsigned long long)st_batches, (unsigned long long)st_tris,
            (unsigned long long)st_declined, (unsigned long long)st_room,
            (unsigned long long)st_full);
}
