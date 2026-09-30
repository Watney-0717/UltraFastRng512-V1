#include "UltraFastRng512_MC_Finance_Early_v1.h"

#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

#ifndef UFR_FINANCE_EARLY_MISSING_SENTINEL
#define UFR_FINANCE_EARLY_MISSING_SENTINEL (-1.0e300)
#endif

static inline double ufr_fe_payoff(double s, double k, ufr_finance_early_option_t opt)
{
    const double d = (opt == UFR_FINANCE_EARLY_CALL) ? (s - k) : (k - s);
    return d > 0.0 ? d : 0.0;
}

static int ufr_fe_is_exercise(const ufr_finance_early_cfg_t *c, unsigned t)
{
    if (!c || t == 0u || t > c->time_steps) return 0;
    return (c->exercise_mask >> (t - 1u)) & 1u;
}

void ufr_finance_early_cfg_default(ufr_finance_early_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->api_version = UFR_FINANCE_EARLY_API_VERSION;
    cfg->option = UFR_FINANCE_EARLY_PUT;
    cfg->style = UFR_FINANCE_EARLY_AMERICAN;
    cfg->time_steps = 64u;
    cfg->exercise_mask = UINT64_MAX;
    cfg->spot = 100.0f;
    cfg->strike = 100.0f;
    cfg->rate = 0.05f;
    cfg->vol = 0.20f;
    cfg->maturity = 1.0f;
    cfg->regression_ridge = 1.0e-12;
    cfg->min_regression_paths = 32u;
}

int ufr_finance_early_cfg_validate(const ufr_finance_early_cfg_t *c)
{
    if (!c) return -1;
    if (c->api_version != UFR_FINANCE_EARLY_API_VERSION) return -2;
    if (c->option != UFR_FINANCE_EARLY_CALL && c->option != UFR_FINANCE_EARLY_PUT) return -3;
    if (c->style != UFR_FINANCE_EARLY_BERMUDAN && c->style != UFR_FINANCE_EARLY_AMERICAN) return -4;
    if (c->time_steps < 2u || c->time_steps > UFR_FINANCE_EARLY_MAX_STEPS) return -5;
    if (!isfinite(c->spot) || !isfinite(c->strike) || !isfinite(c->rate) ||
        !isfinite(c->vol) || !isfinite(c->maturity)) return -6;
    if (!(c->spot > 0.0f) || !(c->strike > 0.0f) || c->vol < 0.0f || !(c->maturity > 0.0f)) return -7;
    if (!isfinite(c->regression_ridge) || c->regression_ridge < 0.0) return -8;
    if (c->min_regression_paths < 3u) return -9;
    if (!ufr_fe_is_exercise(c, c->time_steps)) return -10;
    if (c->style == UFR_FINANCE_EARLY_AMERICAN) {
        const uint64_t need = (c->time_steps == 64u) ? UINT64_MAX : ((UINT64_C(1) << c->time_steps) - 1u);
        if ((c->exercise_mask & need) != need) return -11;
    }
    return 0;
}

static int ufr_fe_solve3(double a[3][3], double b[3], double x[3])
{
    for (unsigned k = 0; k < 3u; ++k) {
        unsigned piv = k;
        double best = fabs(a[k][k]);
        for (unsigned r = k + 1u; r < 3u; ++r) {
            const double v = fabs(a[r][k]);
            if (v > best) { best = v; piv = r; }
        }
        if (!(best > 1.0e-18) || !isfinite(best)) return -1;
        if (piv != k) {
            for (unsigned j = k; j < 3u; ++j) {
                const double tmp = a[k][j]; a[k][j] = a[piv][j]; a[piv][j] = tmp;
            }
            const double tb = b[k]; b[k] = b[piv]; b[piv] = tb;
        }
        const double inv = 1.0 / a[k][k];
        for (unsigned j = k; j < 3u; ++j) a[k][j] *= inv;
        b[k] *= inv;
        for (unsigned r = 0; r < 3u; ++r) {
            if (r == k) continue;
            const double f = a[r][k];
            if (f == 0.0) continue;
            for (unsigned j = k; j < 3u; ++j) a[r][j] -= f * a[k][j];
            b[r] -= f * b[k];
        }
    }
    x[0] = b[0]; x[1] = b[1]; x[2] = b[2];
    return 0;
}

static int ufr_fe_regress3(
    const double *spot_t,
    const double *cont_t,
    const uint8_t *alive,
    uint64_t n,
    double strike,
    double ridge,
    uint32_t min_paths,
    double beta[3])
{
    double ata[3][3] = {{0}};
    double aty[3] = {0,0,0};
    uint64_t used = 0u;
    for (uint64_t p = 0; p < n; ++p) {
        if (!alive[p]) continue;
        const double s = spot_t[p];
        const double y = cont_t[p];
        if (!isfinite(s) || !isfinite(y)) continue;
        /* The caller already filters to ITM paths by setting alive. */
        const double x = s / strike;
        const double f0 = 1.0, f1 = x, f2 = x * x;
        const double f[3] = {f0, f1, f2};
        for (unsigned i = 0; i < 3u; ++i) {
            aty[i] += f[i] * y;
            for (unsigned j = 0; j < 3u; ++j) ata[i][j] += f[i] * f[j];
        }
        ++used;
    }
    if (used < min_paths) return -1;
    ata[0][0] += ridge;
    ata[1][1] += ridge;
    ata[2][2] += ridge;
    return ufr_fe_solve3(ata, aty, beta);
}

static void ufr_fe_finalize_stats(
    const double *pv, uint64_t n, uint64_t invalid,
    uint64_t exercise_count, uint64_t regression_count,
    uint64_t regression_fallbacks, uint32_t steps,
    ufr_finance_early_result_t *out)
{
    double mean = 0.0, m2 = 0.0;
    uint64_t used = 0u;
    for (uint64_t i = 0; i < n; ++i) {
        if (!isfinite(pv[i])) continue;
        ++used;
        const double delta = pv[i] - mean;
        mean += delta / (double)used;
        m2 += delta * (pv[i] - mean);
    }
    memset(out, 0, sizeof(*out));
    out->price = mean;
    out->variance = used > 1u ? m2 / (double)(used - 1u) : NAN;
    out->sem = (used > 1u && isfinite(out->variance)) ? sqrt(out->variance / (double)used) : NAN;
    out->path_count = n;
    out->exercise_count = exercise_count;
    out->regression_count = regression_count;
    out->regression_fallback_count = regression_fallbacks;
    out->invalid_path_count = invalid;
    out->time_steps = steps;
}

int ufr_finance_early_price(
    const float *normals,
    uint64_t path_count,
    const ufr_finance_early_cfg_t *cfg,
    ufr_finance_early_result_t *out)
{
    if (!normals || !out || path_count == 0u || path_count > UFR_FINANCE_EARLY_MAX_PATHS) return -1;
    if (ufr_finance_early_cfg_validate(cfg) != 0) return -2;
    const unsigned T = cfg->time_steps;
    const size_t state_count = (size_t)path_count * (size_t)(T + 1u);
    double *S = (double *)malloc(state_count * sizeof(double));
    double *cash = (double *)malloc((size_t)path_count * sizeof(double));
    unsigned *etime = (unsigned *)malloc((size_t)path_count * sizeof(unsigned));
    double *spot_t = (double *)malloc((size_t)path_count * sizeof(double));
    double *cont_t = (double *)malloc((size_t)path_count * sizeof(double));
    uint8_t *itm = (uint8_t *)malloc((size_t)path_count);
    double *pv = (double *)malloc((size_t)path_count * sizeof(double));
    if (!S || !cash || !etime || !spot_t || !cont_t || !itm || !pv) {
        free(S); free(cash); free(etime); free(spot_t); free(cont_t); free(itm); free(pv);
        return -3;
    }

    const double dt = (double)cfg->maturity / (double)T;
    const double drift = ((double)cfg->rate - 0.5 * (double)cfg->vol * (double)cfg->vol) * dt;
    const double vol_sqrt_dt = (double)cfg->vol * sqrt(dt);
    const double log_spot = log((double)cfg->spot);
    uint64_t invalid = 0u;

    for (uint64_t p = 0; p < path_count; ++p) {
        S[p * (T + 1u)] = (double)cfg->spot;
        double logS = log_spot;
        int path_bad = 0;
        for (unsigned t = 1u; t <= T; ++t) {
            const float zf = normals[p * (uint64_t)T + (t - 1u)];
            if (!isfinite(zf)) { path_bad = 1; logS = NAN; S[p*(T+1u)+t] = NAN; continue; }
            logS += drift + vol_sqrt_dt * (double)zf;
            const double st = exp(logS);
            S[p * (T + 1u) + t] = isfinite(st) ? st : NAN;
            if (!isfinite(st)) path_bad = 1;
        }
        if (path_bad) {
            ++invalid;
            cash[p] = NAN;
        } else {
            cash[p] = ufr_fe_payoff(S[p*(T+1u)+T], cfg->strike, cfg->option);
        }
        etime[p] = T;
    }

    uint64_t exercise_count = 0u, regression_count = 0u, regression_fallbacks = 0u;
    for (int t = (int)T - 1; t >= 1; --t) {
        if (!ufr_fe_is_exercise(cfg, (unsigned)t)) continue;
            for (uint64_t p = 0; p < path_count; ++p) {
            const double future = cash[p];
            const unsigned te = etime[p];
            cont_t[p] = isfinite(future) ? future * exp(-(double)cfg->rate * dt * (double)(te - (unsigned)t)) : NAN;
            spot_t[p] = S[p * (T + 1u) + (unsigned)t];
            itm[p] = (uint8_t)(isfinite(spot_t[p]) &&
                ((cfg->option == UFR_FINANCE_EARLY_CALL && spot_t[p] > cfg->strike) ||
                 (cfg->option == UFR_FINANCE_EARLY_PUT && spot_t[p] < cfg->strike)));
        }
        double beta[3] = {0,0,0};
        int reg_rc = ufr_fe_regress3(spot_t, cont_t, itm, path_count,
                                     cfg->strike, cfg->regression_ridge,
                                     cfg->min_regression_paths, beta);
        ++regression_count;
        if (reg_rc != 0) {
            ++regression_fallbacks;
            double fallback = 0.0;
            uint64_t nfb = 0u;
            for (uint64_t p = 0; p < path_count; ++p) {
                if (!itm[p] || !isfinite(cont_t[p])) continue;
                fallback += cont_t[p]; ++nfb;
            }
            fallback = nfb ? fallback / (double)nfb : 0.0;
            beta[0] = fallback; beta[1] = beta[2] = 0.0;
        }
        for (uint64_t p = 0; p < path_count; ++p) {
            if (!itm[p]) continue;
            const double s = spot_t[p];
            if (!isfinite(s)) continue;
            const double x = s / (double)cfg->strike;
            const double continuation = beta[0] + beta[1] * x + beta[2] * x * x;
            const double immediate = ufr_fe_payoff(s, cfg->strike, cfg->option);
            if (immediate > continuation) {
                cash[p] = immediate;
                etime[p] = (unsigned)t;
                ++exercise_count;
            }
        }
    }

    for (uint64_t p = 0; p < path_count; ++p) {
        if (!isfinite(cash[p]) || etime[p] > T) pv[p] = NAN;
        else pv[p] = cash[p] * exp(-(double)cfg->rate * dt * (double)etime[p]);
    }

    ufr_fe_finalize_stats(pv, path_count, invalid, exercise_count,
                          regression_count, regression_fallbacks, T, out);
    free(S); free(cash); free(etime); free(spot_t); free(cont_t); free(itm); free(pv);
    return isfinite(out->price) ? 0 : -4;
}

static int ufr_fe_run_bump(
    const float *normals, uint64_t n,
    const ufr_finance_early_cfg_t *base,
    double spot, double vol, double rate, double maturity,
    ufr_finance_early_result_t *out)
{
    ufr_finance_early_cfg_t c = *base;
    c.spot = (float)spot;
    c.vol = (float)vol;
    c.rate = (float)rate;
    c.maturity = (float)maturity;
    return ufr_finance_early_price(normals, n, &c, out);
}

int ufr_finance_early_greeks(
    const float *normals,
    uint64_t path_count,
    const ufr_finance_early_cfg_t *cfg,
    const double spot_bump_rel,
    const double vol_bump_abs,
    const double rate_bump_abs,
    const double maturity_bump_rel,
    ufr_finance_early_greeks_result_t *out)
{
    if (!out || !normals || path_count == 0u || ufr_finance_early_cfg_validate(cfg) != 0) return -1;
    if (!(spot_bump_rel > 0.0) || !(vol_bump_abs > 0.0) || !(rate_bump_abs > 0.0) || !(maturity_bump_rel > 0.0)) return -2;
    const double s = cfg->spot, v = cfg->vol, r = cfg->rate, t = cfg->maturity;
    const double ds = s * spot_bump_rel;
    const double dv = vol_bump_abs;
    const double dr = rate_bump_abs;
    const double dtm = t * maturity_bump_rel;

    ufr_finance_early_result_t b, sm, sp, vm, vp, rm, rp, tm, tp;
    if (ufr_fe_run_bump(normals,path_count,cfg,s,v,r,t,&b)!=0 ||
        ufr_fe_run_bump(normals,path_count,cfg,s-ds,v,r,t,&sm)!=0 ||
        ufr_fe_run_bump(normals,path_count,cfg,s+ds,v,r,t,&sp)!=0) return -3;
    if (v - dv < 0.0 ||
        ufr_fe_run_bump(normals,path_count,cfg,s,v-dv,r,t,&vm)!=0 ||
        ufr_fe_run_bump(normals,path_count,cfg,s,v+dv,r,t,&vp)!=0) return -4;
    if (ufr_fe_run_bump(normals,path_count,cfg,s,v,r-dr,t,&rm)!=0 ||
        ufr_fe_run_bump(normals,path_count,cfg,s,v,r+dr,t,&rp)!=0) return -5;
    if (t - dtm <= 0.0 ||
        ufr_fe_run_bump(normals,path_count,cfg,s,v,r,t-dtm,&tm)!=0 ||
        ufr_fe_run_bump(normals,path_count,cfg,s,v,r,t+dtm,&tp)!=0) return -6;

    memset(out,0,sizeof(*out));
    out->base=b;
    out->price_spot_minus=sm.price; out->price_spot_plus=sp.price;
    out->price_vol_minus=vm.price; out->price_vol_plus=vp.price;
    out->price_rate_minus=rm.price; out->price_rate_plus=rp.price;
    out->price_maturity_minus=tm.price; out->price_maturity_plus=tp.price;
    out->delta=(sp.price-sm.price)/(2.0*ds);
    out->gamma=(sp.price-2.0*b.price+sm.price)/(ds*ds);
    out->vega=(vp.price-vm.price)/(2.0*dv);
    out->rho=(rp.price-rm.price)/(2.0*dr);
    /* Conventional option theta is time decay dV/d(calendar time).
     * Maturity T moves opposite to elapsed calendar time, hence -dV/dT. */
    out->theta=(tm.price-tp.price)/(2.0*dtm);
    out->spot_bump=ds; out->vol_bump=dv; out->rate_bump=dr; out->maturity_bump=dtm;
    out->crn_common_normals=1u; out->central_difference=1u;
    return 0;
}

#ifdef UFR_FINANCE_EARLY_ENGINE_INTEGRATION

/* Internal engine bridge. The translation unit that includes this block
 * already defines ufr_mc_run(), ufr_mc_cultivation_engine_t, and UF_NORMAL_SCALE. */
typedef struct {
    float *normals;
    uint64_t capacity_paths;
    uint64_t path_count;
    uint32_t time_steps;
    int error;
} ufr_fe_collect_ctx_t;

static int ufr_fe_collect_qblock(const int32_t *q256,
                                 uint64_t generation_id,
                                 uint32_t view_id,
                                 uint32_t block_id,
                                 void *vctx)
{
    (void)generation_id; (void)view_id; (void)block_id;
    ufr_fe_collect_ctx_t *c = (ufr_fe_collect_ctx_t *)vctx;
    if (!c || !q256 || c->time_steps != 64u || c->path_count + 4u > c->capacity_paths) {
        if (c) c->error = 1;
        return -1;
    }
    float *dst = c->normals + c->path_count * 64u;
    for (unsigned i = 0; i < 256u; ++i)
        dst[i] = (float)((double)q256[i] * (double)UF_NORMAL_SCALE);
    c->path_count += 4u;
    return 0;
}

int ufr_paperview_mc_run_finance_early(
    struct ufr_paperview_mc_engine *engine,
    const ufr_finance_early_cfg_t *cfg,
    uint64_t chunks,
    uint64_t max_paths,
    ufr_finance_early_result_t *out)
{
    if (!engine || !cfg || !out || chunks == 0u) return -1;
    if (!engine->started) return -2;
    if (cfg->time_steps != 64u) return -3;
    if (ufr_finance_early_cfg_validate(cfg) != 0) return -4;
    if (max_paths == 0u || max_paths > UFR_FINANCE_EARLY_MAX_PATHS) max_paths = UFR_FINANCE_EARLY_MAX_PATHS;
    const uint64_t views = engine->impl.cfg.views_per_chunk;
    if (views == 0u) return -5;
    if (chunks > UINT64_MAX / views || chunks * views > UINT64_MAX / 64u ||
        chunks * views * 64u > UINT64_MAX / 4u) return -6;
    const uint64_t capacity = chunks * views * 64u * 4u;
    if (capacity == 0u || capacity > max_paths) return -7;
    float *normals = (float *)malloc((size_t)capacity * 64u * sizeof(float));
    if (!normals) return -8;
    ufr_fe_collect_ctx_t cc = {normals, capacity, 0u, 64u, 0};
    const int rc = ufr_mc_run(&engine->impl, chunks, ufr_fe_collect_qblock, &cc);
    if (rc != 0 || cc.error || cc.path_count == 0u) { free(normals); return -9; }
    const int prc = ufr_finance_early_price(normals, cc.path_count, cfg, out);
    free(normals);
    return prc;
}

int ufr_paperview_mc_run_finance_early_greeks(
    struct ufr_paperview_mc_engine *engine,
    const ufr_finance_early_cfg_t *cfg,
    uint64_t chunks,
    uint64_t max_paths,
    double spot_bump_rel,
    double vol_bump_abs,
    double rate_bump_abs,
    double maturity_bump_rel,
    ufr_finance_early_greeks_result_t *out)
{
    if (!engine || !cfg || !out || chunks == 0u) return -1;
    if (!engine->started) return -2;
    if (cfg->time_steps != 64u || ufr_finance_early_cfg_validate(cfg) != 0) return -3;
    if (max_paths == 0u || max_paths > UFR_FINANCE_EARLY_MAX_PATHS) max_paths = UFR_FINANCE_EARLY_MAX_PATHS;
    const uint64_t views = engine->impl.cfg.views_per_chunk;
    if (views == 0u || chunks > UINT64_MAX / views || chunks * views > UINT64_MAX / 64u ||
        chunks * views * 64u > UINT64_MAX / 4u) return -4;
    const uint64_t capacity = chunks * views * 64u * 4u;
    if (capacity == 0u || capacity > max_paths) return -5;
    float *normals = (float *)malloc((size_t)capacity * 64u * sizeof(float));
    if (!normals) return -6;
    ufr_fe_collect_ctx_t cc = {normals, capacity, 0u, 64u, 0};
    const int rc = ufr_mc_run(&engine->impl, chunks, ufr_fe_collect_qblock, &cc);
    if (rc != 0 || cc.error || cc.path_count == 0u) { free(normals); return -7; }
    const int grc = ufr_finance_early_greeks(normals, cc.path_count, cfg,
                                             spot_bump_rel, vol_bump_abs,
                                             rate_bump_abs, maturity_bump_rel,
                                             out);
    free(normals);
    return grc;
}

#endif /* UFR_FINANCE_EARLY_ENGINE_INTEGRATION */
