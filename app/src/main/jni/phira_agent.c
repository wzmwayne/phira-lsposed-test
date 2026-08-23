/*
 * PhiraAgent - Direction-1 native agent.
 *
 * Locates the autoplay conditional branch inside Judge::update of libphira.so
 * and rewrites it so the native auto_play_update path is always taken, while
 * config.mods stays untouched (no AUTOPLAY marker, normal scoring pipeline).
 *
 * Location strategy (no hard-coded offsets):
 *   1. structural scan  - conditional branches testing bit #0 of a register
 *                         (tbz/tbnz) or "tst wn, #1" followed by b.cond;
 *   2. fingerprint gate - an f64 literal from the prpr judge constants
 *                         (LIMIT_PERFECT/GOOD) must exist within WINDOW bytes;
 *   3. polarity resolve - decide which side of the branch is the autoplay
 *                         block (cond-code heuristic, then [bl ... b] shape);
 *   4. explicit PATTERNs from the editable profile as an escape hatch.
 *
 * Fail-safe: any ambiguity or validation miss => nothing is written.
 */

#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TAG "PhiraAgent"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define DEFAULT_LIB_NAME "libphira.so"
#define PROFILE_ASSET    "phira_profile.txt"
#define PROFILE_OVERRIDE "/sdcard/Documents/phira_autoplay_profile.txt"
#define POLL_MS          200
#define TIMEOUT_MS       120000
#define LIB_POLLS_BEFORE_FAIL 10
#define MAX_SPANS        32
#define MAX_PROBE_ADDRS  128
#define MAX_USE_SITES    256
#define MAX_PROBES       8
#define MAX_PATTERNS     4
#define MAX_PAT_BYTES    32
#define MAX_PATCH_ATS    4
#define JITTER_CAVE_NEED 192 /* trampoline + literals + strings image size */
#define MAX_CANDS        128
#define MAX_PATCH_GROUP  4
#define MAX_SPAN_BYTES   (256UL * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* profile                                                             */
/* ------------------------------------------------------------------ */

static const char* g_lib_name = DEFAULT_LIB_NAME;

static void report(JNIEnv* env, const char* fmt, ...); /* fwd: defined later */
static void status(JNIEnv* env, const char* fmt, ...);  /* fwd: defined later */

typedef struct {
    unsigned char bytes[MAX_PAT_BYTES];
    unsigned char wild[MAX_PAT_BYTES]; /* 1 = wildcard byte */
    int len;
    long patch_off; /* byte offset of the branch inside the match, -1 = n/a */
} pat_t;

typedef struct {
    long off;        /* span-relative byte offset (must be 4-aligned) */
    uint32_t expect; /* required current word - refuse to touch on mismatch */
    uint32_t repl;   /* replacement word, 0 = derive from expect (tbnz->B) */
} patch_at_t;

/* auto-play timing-offset jitter: the auto commit passes diff=0.0 for every
 * note, so the std (无暇度) aggregate stays 0 and the server rejects the
 * record as non-human. JITTER patches the auto commit's diff slot to feed a
 * per-note pseudo-random offset in [-R,+R] (R = sigma*sqrt(3)) so the
 * game's own sumsq accumulator yields std ~= sigma. */
typedef struct {
    long off;        /* span-relative offset of the movi d0,#0.0 slot */
    uint32_t expect; /* required current word (e.g. 0x2F00E400 movi d0,#0) */
    int sigma_ms;    /* target std in ms, 0 = disabled */
    uint32_t seed;   /* hash seed, 0 = random at patch time */
    int enabled;
} jitter_t;

typedef struct {
    uint64_t probes[MAX_PROBES];
    int nprobes;
    long window;
    long xref_win; /* proximity to adrp+ldr use sites */
    int max_cand;
    int best_of_n; /* allow ranked best-of-N selection when N != max_cand */
    pat_t pats[MAX_PATTERNS];
    int npats;
    patch_at_t patch_ats[MAX_PATCH_ATS];
    int npatch_ats;
    jitter_t jit;
} profile_t;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void profile_defaults(profile_t* pf) {
    memset(pf, 0, sizeof(*pf));
    pf->probes[pf->nprobes++] = 0x3FB47AE147AE147BULL; /* f64 0.08 */
    pf->probes[pf->nprobes++] = 0x3FC47AE147AE147BULL; /* f64 0.16 */
    pf->window = 4096;
    pf->xref_win = 8192; /* real branch sits 4776B from nearest const site */
    pf->max_cand = 1;
    pf->best_of_n = 1;
    /* built-in insurance: current build (Phira_0.8.2) judge::update branch at
     * vaddr 0xdc92d0 = span+0x6D92D0, tbnz w8,#0 -> B +0xa8 (0x1400002A).
     * Guarantees injection even when the APK asset fails to load. Update this
     * when targeting a different libphira.so build (see docs/re-judge-update.md). */
    pf->patch_ats[pf->npatch_ats].off = 0x6D92D0L;
    pf->patch_ats[pf->npatch_ats].expect = 0x37000548u;
    pf->patch_ats[pf->npatch_ats].repl = 0u; /* derive: tbnz -> B same target */
    pf->npatch_ats++;
    /* built-in jitter insurance: same build (Phira_0.8.2) auto commit diff
     * slot at vaddr 0xdc96f8 = span+0x6D96F8, `movi d0,#0.0`. Patch it to a
     * trampoline that feeds per-note pseudo-random offsets so std ~= 20ms
     * (human-plausible) instead of 0. Update with the build (see
     * docs/judge-error-std-manual-analysis.md). */
    pf->jit.off = 0x6D96F8L;
    pf->jit.expect = 0x2F00E400u; /* movi d0, #0.0 */
    pf->jit.sigma_ms = 20;
    pf->jit.seed = 0; /* random per launch */
    pf->jit.enabled = 1;
}

static void profile_parse(profile_t* pf, char* buf);

static void profile_line(profile_t* pf, char* s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#' || *s == '\0' || *s == '\r' || *s == '\n') return;

    char cmd[32];
    if (sscanf(s, "%31s", cmd) != 1) return;
    char* rest = s + strlen(cmd);

    if (!strcmp(cmd, "WINDOW")) {
        long v = atol(rest);
        if (v >= 256 && v <= 65536) pf->window = v;
    } else if (!strcmp(cmd, "XREF_WIN")) {
        long v = atol(rest);
        if (v >= 64 && v <= 65536) pf->xref_win = v;
    } else if (!strcmp(cmd, "MAX_CAND")) {
        int v = atoi(rest);
        if (v >= 1 && v <= MAX_CANDS) pf->max_cand = v;
    } else if (!strcmp(cmd, "BEST_OF_N")) {
        pf->best_of_n = atoi(rest) != 0;
    } else if (!strcmp(cmd, "LIB")) {
        char name[128];
        if (sscanf(rest, "%127s", name) == 1 && name[0] == '/') {
            g_lib_name = strdup(name); /* full-path suffix match */
        } else if (sscanf(rest, "%127s", name) == 1 && strchr(name, '.')) {
            static char buf[128];
            snprintf(buf, sizeof(buf), "/%s", name);
            g_lib_name = buf; /* bare file-name match */
        }
    } else if (!strcmp(cmd, "PATCH_AT")) {
        if (pf->npatch_ats >= MAX_PATCH_ATS) return;
        patch_at_t* pa = &pf->patch_ats[pf->npatch_ats];
        unsigned long off;
        unsigned int expect, repl = 0;
        int got = sscanf(rest, "%lx %x %x", &off, &expect, &repl);
        if (got < 2) {
            LOGE("PATCH_AT: need <hex_off> <hex_expect> [hex_repl]");
            return;
        }
        if (off & 3u) return;
        /* dedupe by offset: built-in defaults already carry the current
         * build's entry, and a duplicate would fail its post-write verify
         * (word no longer equals expect) and abort the whole attempt. */
        for (int i = 0; i < pf->npatch_ats; i++) {
            if (pf->patch_ats[i].off == (long)off) {
                LOGW("PATCH_AT @+%lx duplicate (already set) - skipped", off);
                return;
            }
        }
        pa->off = (long)off;
        pa->expect = (uint32_t)expect;
        pa->repl = (uint32_t)repl; /* 0 => derive */
        pf->npatch_ats++;
    } else if (!strcmp(cmd, "JITTER")) {
        if (pf->jit.enabled) {
            LOGW("JITTER duplicate (already set) - keep first");
            return;
        }
        unsigned long off;
        unsigned int expect;
        int sigma = 20;
        unsigned int seed = 0;
        int got = sscanf(rest, "%lx %x %d %x", &off, &expect, &sigma, &seed);
        if (got < 2) {
            LOGE("JITTER: need <hex_off> <hex_expect> [sigma_ms] [seed]");
            return;
        }
        if (off & 3u) return;
        if (sigma < 0) sigma = 0;
        if (sigma > 40) sigma = 40; /* R=sigma*sqrt(3) must stay < 80ms window */
        pf->jit.off = (long)off;
        pf->jit.expect = (uint32_t)expect;
        pf->jit.sigma_ms = sigma;
        pf->jit.seed = seed;
        pf->jit.enabled = 1;
    } else if (!strcmp(cmd, "PROBE")) {
        char hex[32];
        if (pf->nprobes >= MAX_PROBES) return;
        if (sscanf(rest, "%16s", hex) != 1 || strlen(hex) != 16) return;
        uint64_t v = 0;
        for (int i = 0; i < 16; i++) {
            int h = hexval(hex[i]);
            if (h < 0) return;
            v = (v << 4) | (uint64_t)h;
        }
        pf->probes[pf->nprobes++] = v;
    } else if (!strcmp(cmd, "PATTERN")) {
        if (pf->npats >= MAX_PATTERNS) return;
        pat_t* p = &pf->pats[pf->npats];
        memset(p, 0, sizeof(*p));
        p->patch_off = -1;
        int blen = 0;
        int bad = 0;
        char* tok = strtok(rest, " \t\r\n");
        while (tok) {
            size_t tl = strlen(tok);
            int digits_only = 1;
            for (size_t i = 0; i < tl; i++) {
                if (tok[i] < '0' || tok[i] > '9') { digits_only = 0; break; }
            }
            if (digits_only) {
                p->patch_off = atol(tok);
                break;
            }
            if (tl == 0 || (tl % 2) != 0 || blen + (int)(tl / 2) > MAX_PAT_BYTES) {
                bad = 1;
                break;
            }
            for (size_t i = 0; i < tl; i += 2) {
                if (tok[i] == '.' && tok[i + 1] == '.') {
                    p->wild[blen] = 1;
                    p->bytes[blen] = 0;
                    blen++;
                } else {
                    int hi = hexval(tok[i]), lo = hexval(tok[i + 1]);
                    if (hi < 0 || lo < 0) { bad = 1; break; }
                    p->bytes[blen] = (unsigned char)((hi << 4) | lo);
                    p->wild[blen] = 0;
                    blen++;
                }
            }
            if (bad) break;
            tok = strtok(NULL, " \t\r\n");
        }
        if (!bad && blen > 0) {
            p->len = blen;
            pf->npats++;
        }
    }
}

static void profile_parse(profile_t* pf, char* buf) {
    char* save = NULL;
    for (char* line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        profile_line(pf, line);
    }
}

static void profile_load(profile_t* pf, JNIEnv* env, jobject assets) {
    profile_defaults(pf);

    FILE* f = fopen(PROFILE_OVERRIDE, "r");
    if (f) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        profile_parse(pf, buf);
        LOGI("profile: override %s", PROFILE_OVERRIDE);
        report(env, "profile: source=override %s", PROFILE_OVERRIDE);
        return;
    }
    LOGW("profile: override %s not readable (%s)", PROFILE_OVERRIDE,
         strerror(errno));
    report(env, "profile: override %s not readable (%s)", PROFILE_OVERRIDE,
           strerror(errno));

    if (env && assets) {
        AAssetManager* mgr = AAssetManager_fromJava(env, assets);
        if (mgr) {
            AAsset* a = AAssetManager_open(mgr, PROFILE_ASSET, AASSET_MODE_BUFFER);
            if (a) {
                off_t len = AAsset_getLength(a);
                if (len > 0 && len < 65536) {
                    char* buf = (char*)malloc((size_t)len + 1);
                    if (buf) {
                        AAsset_read(a, buf, (size_t)len);
                        buf[len] = 0;
                        profile_parse(pf, buf);
                        free(buf);
                        AAsset_close(a);
                        LOGI("profile: asset %s", PROFILE_ASSET);
                        report(env, "profile: source=asset %s", PROFILE_ASSET);
                        return;
                    }
                    LOGE("profile: asset %s malloc failed", PROFILE_ASSET);
                    report(env, "profile: asset %s malloc failed", PROFILE_ASSET);
                } else {
                    LOGE("profile: asset %s bad length %lld", PROFILE_ASSET,
                         (long long)len);
                    report(env, "profile: asset %s bad length %lld",
                           PROFILE_ASSET, (long long)len);
                }
                AAsset_close(a);
            } else {
                LOGE("profile: AAssetManager_open(%s) failed", PROFILE_ASSET);
                report(env, "profile: AAssetManager_open(%s) failed",
                       PROFILE_ASSET);
            }
        } else {
            LOGE("profile: AAssetManager_fromJava failed");
            report(env, "profile: AAssetManager_fromJava failed");
        }
    } else {
        report(env, "profile: no assets handle (env=%p assets=%p)",
               (void*)env, (void*)assets);
    }
    LOGI("profile: built-in defaults");
    report(env, "profile: source=builtin");
}

/* ------------------------------------------------------------------ */
/* maps                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uintptr_t start, end;
    int exec;
} span_t;

static int parse_maps(span_t* out, int max) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, g_lib_name)) continue;
        uintptr_t s, e;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3) continue;
        char* slash = strrchr(line, '/');
        if (!slash) continue;
        size_t ll = strlen(slash);
        while (ll && (slash[ll - 1] == '\n' || slash[ll - 1] == '\r')) slash[--ll] = 0;
        size_t wl = strlen(g_lib_name);
        if (ll < wl || strcmp(slash + ll - wl, g_lib_name) != 0) continue;
        /* merge with previous span if adjacent */
        if (n > 0 && out[n - 1].end == s) {
            out[n - 1].end = e;
            out[n - 1].exec |= strchr(perms, 'x') != NULL;
            continue;
        }
        if (n < max) {
            out[n].start = s;
            out[n].end = e;
            out[n].exec = strchr(perms, 'x') != NULL;
            n++;
        }
    }
    fclose(f);
    return n;
}

/* Diagnostic: list every executable mapping so mismatches are visible. */
static void dump_loaded_libs(void) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    const char* last = NULL;
    char lastbuf[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char perms[8] = {0};
        char* slash = strrchr(line, '/');
        if (!slash || !strstr(slash, ".so")) continue;
        if (sscanf(line, "%*x-%*x %7s", perms) != 1) continue;
        if (!strchr(perms, 'x')) continue;
        size_t ll = strlen(slash);
        while (ll && (slash[ll - 1] == '\n' || slash[ll - 1] == '\r')) slash[--ll] = 0;
        if (last && strcmp(last, slash) == 0) continue;
        strncpy(lastbuf, slash, sizeof(lastbuf) - 1);
        last = lastbuf;
        LOGI("loaded-lib: %s", slash);
        if (++count >= 40) break;
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* arm64 helpers                                                       */
/* ------------------------------------------------------------------ */

static int is_tbz_bit0(uint32_t insn) {
    uint32_t op = insn >> 24;
    if (op != 0x36 && op != 0x37) return 0; /* TBZ / TBNZ */
    return ((insn >> 19) & 0x1Fu) == 0;     /* tests bit #0 */
}

static int is_tst_w_1(uint32_t insn) {
    /* TST Wn, #1 : ANDS immediate, sf=0 opc=11 N=0 imm12=1 Rt=31 */
    return (insn & 0x7FC0001Fu) == 0x7100001Fu &&
           ((insn >> 10) & 0xFFFu) == 1;
}

static int is_bcond(uint32_t insn) { return (insn >> 24) == 0x54; }

static int is_cbz_w(uint32_t insn) {
    uint32_t op = insn >> 24;
    return op == 0x34 || op == 0x35; /* CBZ / CBNZ on Wn */
}

static int is_and_w_1(uint32_t insn) {
    /* AND Wn, Wm, #1 : logical immediate, N/immr/imms all zero */
    return ((insn >> 23) & 0x1FFu) == 0x24u &&
           (insn & 0x003FFC00u) == 0;
}

static int is_b(uint32_t insn) { return (insn & 0xFC000000u) == 0x14000000u; }

static int is_bl(uint32_t insn) { return (insn & 0xFC000000u) == 0x94000000u; }

static int is_ret(uint32_t insn) { return insn == 0xD65F03C0u; }

static int32_t imm14_of(uint32_t insn) {
    /* signed imm14 at bits[18:5] (TBZ/TBNZ) */
    return ((int32_t)insn << 13) >> 18;
}

static int32_t imm19_of(uint32_t insn) { return ((int32_t)insn << 8) >> 13; }

static int32_t branch_imm(uint32_t insn) {
    if (is_tbz_bit0(insn)) return imm14_of(insn);
    if (is_bcond(insn) || is_cbz_w(insn)) return imm19_of(insn);
    return 0;
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */
/* ------------------------------------------------------------------ */

static int region_has_probe(const profile_t* pf, uintptr_t rs, uintptr_t re,
                            uintptr_t pc) {
    uintptr_t lo = (uintptr_t)pf->window < pc ? pc - (uintptr_t)pf->window : pc;
    uintptr_t hi = pc + (uintptr_t)pf->window;
    if (lo < rs) lo = rs;
    if (hi > re) hi = re;
    if (hi <= lo || hi - lo < 8) return 0;
    const unsigned char* p = (const unsigned char*)lo;
    size_t len = (size_t)(hi - lo);
    for (int k = 0; k < pf->nprobes; k++) {
        uint64_t v = pf->probes[k];
        for (size_t i = 0; i + 8 <= len; i++) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            if (w == v) return 1;
        }
    }
    return 0;
}

static long count_probe(const profile_t* pf, uintptr_t rs, uintptr_t re,
                        uint64_t v) {
    const unsigned char* p = (const unsigned char*)rs;
    size_t len = (size_t)(re - rs);
    long hits = 0;
    for (size_t i = 0; i + 8 <= len; i++) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        if (w == v) hits++;
    }
    return hits;
}

/* ------------------------------------------------------------------ */
/* probe cross-reference                                               */
/*                                                                     */
/* The judge constants live in .rodata (non-executable LOAD segment),  */
/* so searching for them near code is useless. Instead: locate them    */
/* anywhere in the module, then find ADRP+LDR pairs in .text that      */
/* reference their pages - those LDR sites are the real fingerprint.   */
/* ------------------------------------------------------------------ */

static uintptr_t g_probe_addrs[MAX_PROBE_ADDRS];
static int g_nprobe_addrs = 0;
static uintptr_t g_probe_pages[16];
static int g_nprobe_pages = 0;
static uintptr_t g_use_sites[MAX_USE_SITES];
static int g_nuse_sites = 0;
static int g_nuse_sites_total = 0;
static int g_xref_done = 0;

static void push_site(uintptr_t pc) {
    g_nuse_sites_total++;
    for (int i = 0; i < g_nuse_sites; i++)
        if (g_use_sites[i] == pc) return;
    if (g_nuse_sites < MAX_USE_SITES) g_use_sites[g_nuse_sites++] = pc;
}

static int is_adrp(uint32_t insn) {
    return (insn & 0x9F000000u) == 0x90000000u;
}

static uintptr_t adrp_page(uintptr_t pc, uint32_t insn) {
    int32_t immlo = (int32_t)((insn >> 29) & 0x3u);
    int32_t immhi = (int32_t)((insn >> 5) & 0x7FFFFu);
    int32_t imm = (immhi << 2) | immlo; /* 21-bit signed */
    if (imm & 0x100000) imm -= 0x200000;
    return (pc & ~(uintptr_t)0xFFFu) + ((uintptr_t)(uint64_t)imm << 12);
}

static int is_ldr_imm(uint32_t insn) {
    uint32_t t = insn & 0xFFC00000u;
    return t == 0xF9400000u || /* x  */
           t == 0xB9400000u || /* w  */
           t == 0xFD400000u || /* d  */
           t == 0xBD400000u;   /* s  */
}

static uint32_t reg_rn(uint32_t insn) { return (insn >> 5) & 0x1Fu; }

static int is_add_imm(uint32_t insn) {
    return ((insn >> 23) & 0x3Fu) == 0x22u && !((insn >> 29) & 0x3u) &&
           !(insn & 0x400000u); /* sh = 0 */
}

static uint32_t ldr_pimm(uint32_t insn) {
    uint32_t t = insn & 0xFFC00000u;
    uint32_t scale = (t == 0xF9400000u || t == 0xFD400000u) ? 8u : 4u;
    return ((insn >> 10) & 0xFFFu) * scale;
}

static uint32_t add_imm12(uint32_t insn) { return (insn >> 10) & 0xFFFu; }

/* human-readable form of a candidate branch, for panel diagnostics */
static const char* form_name(uint32_t insn) {
    if (is_tbz_bit0(insn)) return (insn >> 24) == 0x37 ? "tbnz#0" : "tbz#0";
    if (is_bcond(insn)) {
        switch (insn & 0xFu) {
            case 0: return "b.eq";
            case 1: return "b.ne";
            case 2: return "b.cs";
            case 3: return "b.cc";
            case 4: return "b.mi";
            case 5: return "b.pl";
            case 6: return "b.vs";
            case 7: return "b.vc";
            case 8: return "b.hi";
            case 9: return "b.ls";
            case 10: return "b.ge";
            case 11: return "b.lt";
            case 12: return "b.gt";
            case 13: return "b.le";
            default: return "b.?";
        }
    }
    if (is_cbz_w(insn)) return (insn >> 24) == 0x35 ? "cbnz" : "cbz";
    return "?";
}

static long dist_to_sites(uintptr_t pc) {
    long best = -1;
    for (int i = 0; i < g_nuse_sites; i++) {
        long d = g_use_sites[i] > pc ? (long)(g_use_sites[i] - pc)
                                     : (long)(pc - g_use_sites[i]);
        if (best < 0 || d < best) best = d;
    }
    return best;
}

static int off_in_list(const uint32_t* los, int nlos, uint32_t off) {
    for (int q = 0; q < nlos; q++)
        if (los[q] == off) return 1;
    return 0;
}

static void build_xref(JNIEnv* env, const span_t* spans, int nspan,
                       const profile_t* pf) {
    long pcnt[2] = {-1, -1};
    for (int k = 0; k < pf->nprobes && k < 2; k++) {
        uint64_t v = pf->probes[k];
        long hits = 0;
        for (int i = 0; i < nspan; i++) {
            const unsigned char* p = (const unsigned char*)spans[i].start;
            size_t len = (size_t)(spans[i].end - spans[i].start);
            for (size_t o = 0; o + 8 <= len; o += 4) {
                uint64_t w;
                memcpy(&w, p + o, 8);
                if (w != v) continue;
                hits++;
                if (g_nprobe_addrs < MAX_PROBE_ADDRS)
                    g_probe_addrs[g_nprobe_addrs++] =
                        spans[i].start + (uintptr_t)o;
            }
        }
        pcnt[k] = hits;
    }
    for (int i = 0; i < g_nprobe_addrs; i++) {
        uintptr_t pg = g_probe_addrs[i] & ~(uintptr_t)0xFFFu;
        int seen = 0;
        for (int j = 0; j < g_nprobe_pages; j++)
            if (g_probe_pages[j] == pg) { seen = 1; break; }
        if (!seen && g_nprobe_pages < 16) g_probe_pages[g_nprobe_pages++] = pg;
    }

    for (int i = 0; i < nspan; i++) {
        if (!spans[i].exec) continue;
        for (uintptr_t pc = spans[i].start; pc + 4 <= spans[i].end; pc += 4) {
            uint32_t insn = *(volatile uint32_t*)pc;
            if (!is_adrp(insn)) continue;
            uintptr_t page = adrp_page(pc, insn) & ~(uintptr_t)0xFFFu;

            /* page offsets of OUR probes living on this adrp target page */
            uint32_t los[16];
            int nlos = 0;
            for (int j = 0; j < g_nprobe_addrs && nlos < 16; j++)
                if ((g_probe_addrs[j] & ~(uintptr_t)0xFFFu) == page)
                    los[nlos++] = (uint32_t)(g_probe_addrs[j] & 0xFFFu);
            if (!nlos) continue;

            uint32_t rd = insn & 0x1Fu;
            for (int d = 1; d <= 6; d++) {
                uintptr_t apc = pc + (uintptr_t)d * 4;
                if (apc + 4 > spans[i].end) break;
                uint32_t nx = *(volatile uint32_t*)apc;
                if (is_ldr_imm(nx) && reg_rn(nx) == rd) {
                    if (off_in_list(los, nlos, ldr_pimm(nx))) push_site(apc);
                    break; /* chain consumed by this ldr either way */
                }
                if (is_add_imm(nx) && reg_rn(nx) == rd) {
                    uint32_t imm = add_imm12(nx);
                    if (!off_in_list(los, nlos, imm)) continue;
                    uint32_t rd2 = nx & 0x1Fu;
                    for (int e = d + 1; e <= 6; e++) {
                        uintptr_t bpc = pc + (uintptr_t)e * 4;
                        if (bpc + 4 > spans[i].end) break;
                        uint32_t mx = *(volatile uint32_t*)bpc;
                        if (is_ldr_imm(mx) && reg_rn(mx) == rd2) {
                            push_site(bpc);
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }
    report(env, "probes p0=%ld p1=%ld addrs=%d pages=%d sites=%d/%d", pcnt[0],
           pcnt[1], g_nprobe_addrs, g_nprobe_pages, g_nuse_sites,
           g_nuse_sites_total);
}

static int near_use_site(uintptr_t pc, uintptr_t window) {
    for (int i = 0; i < g_nuse_sites; i++) {
        uintptr_t d =
            g_use_sites[i] > pc ? g_use_sites[i] - pc : pc - g_use_sites[i];
        if (d <= window) return 1;
    }
    return 0;
}

typedef struct {
    uintptr_t pc;
    uint32_t insn;
} cand_t;

static void push_cand(cand_t* cands, int* n, int* truncated, uintptr_t pc,
                      uint32_t insn) {
    for (int i = 0; i < *n; i++)
        if (cands[i].pc == pc) return;
    if (*n < MAX_CANDS) {
        cands[*n].pc = pc;
        cands[*n].insn = insn;
        (*n)++;
    } else {
        (*truncated)++;
    }
}

static void collect_structural(const profile_t* pf, uintptr_t rs, uintptr_t re,
                               cand_t* cands, int* n, int* truncated,
                               long* raw_tbz, long* raw_tst, long* raw_cbz,
                               long* gated_out) {
    for (uintptr_t pc = (rs + 3) & ~(uintptr_t)3u; pc + 4 <= re; pc += 4) {
        uint32_t insn = *(volatile uint32_t*)pc;
        uintptr_t bpc = 0;
        uint32_t binsn = 0;

        if (is_tbz_bit0(insn)) {
            (*raw_tbz)++;
            bpc = pc;
            binsn = insn;
        } else if (is_tst_w_1(insn)) {
            for (int d = 1; d <= 4; d++) {
                if (pc + (uintptr_t)d * 4 + 4 > re) break;
                uint32_t nx = *(volatile uint32_t*)(pc + (uintptr_t)d * 4);
                if (is_bcond(nx)) {
                    (*raw_tst)++;
                    bpc = pc + (uintptr_t)d * 4;
                    binsn = nx;
                    break;
                }
            }
        } else if (is_cbz_w(insn)) {
            /* cbz/cbnz wN preceded by "and wN, wX, #1" */
            uint32_t rt = insn & 0x1Fu;
            for (int d = 1; d <= 4; d++) {
                if (pc < rs + (uintptr_t)d * 4) break;
                uint32_t pv =
                    *(volatile uint32_t*)(pc - (uintptr_t)d * 4);
                if (is_and_w_1(pv) && (pv & 0x1Fu) == rt) {
                    (*raw_cbz)++;
                    bpc = pc;
                    binsn = insn;
                    break;
                }
            }
        }
        if (!bpc) continue;

        int32_t im = branch_imm(binsn);
        if (im == 0 || im == -1) continue;
        uintptr_t tgt = bpc + (uintptr_t)(im * 4);
        if (tgt <= bpc) continue; /* backward branch = loop, not our shape */
        if (tgt < rs || tgt >= re) continue;
        if (!region_has_probe(pf, rs, re, pc) &&
            !near_use_site(pc, (uintptr_t)pf->xref_win)) {
            (*gated_out)++;
            continue;
        }

        push_cand(cands, n, truncated, bpc, binsn);
    }
}

static void collect_patterns(const profile_t* pf, uintptr_t rs, uintptr_t re,
                             cand_t* cands, int* n) {
    for (int k = 0; k < pf->npats; k++) {
        const pat_t* p = &pf->pats[k];
        if (p->len <= 0 || p->patch_off < 0 ||
            p->patch_off + 4 > p->len)
            continue;
        for (uintptr_t pc = rs; pc + (uintptr_t)p->len <= re; pc += 4) {
            int match = 1;
            for (int i = 0; i < p->len; i++) {
                uint32_t byte = *(volatile unsigned char*)(pc + (uintptr_t)i);
                if (!p->wild[i] && byte != p->bytes[i]) { match = 0; break; }
            }
            if (!match) continue;
            uintptr_t bpc = pc + (uintptr_t)p->patch_off;
            if (bpc & 3u) continue;
            if (bpc + 4 > re) continue;
            uint32_t insn = *(volatile uint32_t*)bpc;
            if (!is_tbz_bit0(insn) && !is_bcond(insn)) continue;
            int dummy_trunc = 0;
            push_cand(cands, n, &dummy_trunc, bpc, insn);
        }
    }
}

/* ------------------------------------------------------------------ */
/* polarity + rewrite                                                  */
/* ------------------------------------------------------------------ */

/* strict call-then-jump shape: bl among the first few instructions,
 * followed by an unconditional b - characteristic of the
 * "auto_play_update(res, chart); return;" block (judge.rs:414-415). */
static int side_call_ret_shape(uintptr_t rs, uintptr_t re, uintptr_t from) {
    int seen_bl = -1;
    for (int i = 0; i < 16; i++) {
        uintptr_t pc = from + (uintptr_t)i * 4;
        if (pc + 4 > re) break;
        uint32_t insn = *(volatile uint32_t*)pc;
        if (is_ret(insn)) break;
        if (seen_bl < 0) {
            if (i >= 6) break; /* the call must come early */
            if (is_bl(insn)) seen_bl = i;
        } else if (is_b(insn)) {
            return 1;
        }
    }
    return 0;
}

/* 1 = taken side is autoplay (rewrite branch -> B target),
 * 2 = fall-through side is autoplay (rewrite branch -> NOP).
 * Wrong guesses degrade to "always manual", never crash.
 * heuristic: 2 = cond-code hint (tbnz / b.ne => taken, tbz / b.eq => not);
 * st/sf: strict call-ret shape on taken / fall-through side. */
typedef struct {
    int decision;
    int heuristic; /* 0/2 */
    int st, sf;    /* call-ret shape on taken / fall-through side */
} side_res_t;

static side_res_t resolve_auto_side_ex(uintptr_t rs, uintptr_t re,
                                       uintptr_t pc, uint32_t insn) {
    side_res_t r = {1, 0, 0, 0};
    if (is_tbz_bit0(insn)) {
        r.decision = (insn >> 24) == 0x37 ? 1 : 2;
        r.heuristic = 2;
    } else if (is_bcond(insn)) {
        uint32_t cond = insn & 0xFu;
        if (cond == 1) {
            r.decision = 1;
            r.heuristic = 2;
        } else if (cond == 0) {
            r.decision = 2;
            r.heuristic = 2;
        }
    }

    /* shape is computed for EVERY candidate and combined with heuristic */
    int32_t im = branch_imm(insn);
    uintptr_t tgt = pc + (uintptr_t)(im * 4);
    r.st = side_call_ret_shape(rs, re, tgt);
    r.sf = side_call_ret_shape(rs, re, pc + 4);

    if (!r.heuristic) {
        /* no cond-code hint: decide purely by shape */
        if (r.st && !r.sf)
            r.decision = 1;
        else if (r.sf && !r.st)
            r.decision = 2;
        else {
            LOGW("polarity unresolved @ %p, assuming taken side", (void*)pc);
            r.decision = 1;
        }
    }
    return r;
}

static int resolve_auto_side(uintptr_t rs, uintptr_t re, uintptr_t pc,
                             uint32_t insn) {
    return resolve_auto_side_ex(rs, re, pc, insn).decision;
}

static int cand_score(const side_res_t* s) {
    return s->heuristic + ((s->st ^ s->sf) ? 1 : 0);
}

/* raw word write with mprotect dance; caller verifies semantics first */
static int patch_word(uintptr_t pc, uint32_t nw) {
    uintptr_t page = pc & ~(uintptr_t)0xFFFu;
    if (mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect RW failed: %s", strerror(errno));
        return 0;
    }
    *(volatile uint32_t*)pc = nw;
    __builtin___clear_cache((char*)pc, (char*)pc + 4);
    mprotect((void*)page, 0x1000, PROT_READ | PROT_EXEC);
    if (*(volatile uint32_t*)pc != nw) {
        LOGE("verify failed @ %p", (void*)pc);
        return 0;
    }
    return 1;
}

/* same dance for an arbitrary byte buffer (up to two pages) */
static int patch_bytes(uintptr_t addr, const void* buf, size_t len) {
    uintptr_t pg0 = addr & ~(uintptr_t)0xFFFu;
    uintptr_t pg1 = (addr + len - 1) & ~(uintptr_t)0xFFFu;
    if (mprotect((void*)pg0, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect RW failed: %s", strerror(errno));
        return 0;
    }
    if (pg1 != pg0 &&
        mprotect((void*)pg1, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect RW(pg1) failed: %s", strerror(errno));
        mprotect((void*)pg0, 0x1000, PROT_READ | PROT_EXEC);
        return 0;
    }
    memcpy((void*)addr, buf, len);
    __builtin___clear_cache((char*)addr, (char*)addr + (long)len);
    mprotect((void*)pg0, 0x1000, PROT_READ | PROT_EXEC);
    if (pg1 != pg0) mprotect((void*)pg1, 0x1000, PROT_READ | PROT_EXEC);
    if (memcmp((void*)addr, buf, len) != 0) {
        LOGE("verify failed @ %p", (void*)addr);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* jitter: make the auto path feed plausible timing offsets             */
/*                                                                     */
/* auto_play_update commits every note as Perfect with diff=0.0, so     */
/* the std (无暇度) aggregate stays 0 and servers reject the record as  */
/* non-human. JITTER redirects the diff slot (movi d0,#0.0) to a       */
/* trampoline in a free code cave that computes a per-note              */
/* pseudo-random offset (hash of note id + seed) in [-R,+R] with        */
/* R = sigma*sqrt(3), i.e. std ~= sigma. judged states / mods / score   */
/* are untouched.                                                       */
/* ------------------------------------------------------------------ */

/* aarch64 unconditional branch (signed 64-bit offset, then low 26 bits) */
static uint32_t b_insn(uintptr_t from, uintptr_t to) {
    int64_t off = (int64_t)to - (int64_t)from;
    return 0x14000000u | ((uint32_t)(off >> 2) & 0x03FFFFFFu);
}

/* first aligned run of >= need bytes of NOP/zero padding in [rs,re) */
static uintptr_t find_cave(uintptr_t rs, uintptr_t re, size_t need) {
    uintptr_t run_start = 0;
    size_t run = 0;
    for (uintptr_t p = (rs + 3) & ~(uintptr_t)3u; p + 4 <= re; p += 4) {
        uint32_t w = *(volatile uint32_t*)p;
        if (w == 0xD503201Fu || w == 0u) { /* NOP or zero padding */
            if (run == 0) run_start = p;
            run++;
            if (run * 4 >= need) return run_start;
        } else {
            run = 0;
        }
    }
    return 0;
}

/* patch the auto-commit diff slot to route through a jitter trampoline.
 * Returns 1 patched, 0 nothing to do, -1 fail-safe abort. */
static int apply_jitter(JNIEnv* env, const profile_t* pf, uintptr_t rs,
                        uintptr_t re) {
    const jitter_t* jit = &pf->jit;
    LOGI("jitter: enabled=%d sigma=%dms off=+0x%lx expect=%08x", jit->enabled,
         jit->sigma_ms, jit->off, jit->expect);
    if (!jit->enabled || jit->sigma_ms <= 0) return 0;

    uintptr_t pc = rs + (uintptr_t)jit->off;
    if (pc < rs || pc + 4 > re) {
        LOGE("jitter @+%lx out of span - abort", jit->off);
        report(env, "JITTER @+%lx out of span - abort", jit->off);
        status(env, "越界放弃");
        return -1;
    }
    uint32_t cur = *(volatile uint32_t*)pc;
    if (cur != jit->expect) {
        LOGE("jitter @+%lx: %08x != expect %08x - abort", jit->off, cur,
             jit->expect);
        report(env, "JITTER @+%lx: %08x != expect %08x - abort", jit->off, cur,
               jit->expect);
        status(env, "校验不符");
        return -1;
    }

    /* locate the trampoline cave once per span */
    static uintptr_t cave_rs = 0, cave_re = 0, cave = 0;
    if (cave_rs != rs || cave_re != re) {
        cave = find_cave(rs, re, JITTER_CAVE_NEED);
        cave_rs = rs;
        cave_re = re;
        LOGI("jitter: cave @ +0x%lx", cave ? cave - rs : 0);
        if (cave) report(env, "JITTER: cave @ +0x%lx", cave - rs);
    }
    if (!cave) {
        LOGE("jitter: no code cave found - abort");
        report(env, "JITTER: no code cave found - abort");
        status(env, "写入失败");
        return -1;
    }
    if (cave + JITTER_CAVE_NEED > re) {
        report(env, "JITTER: cave out of span - abort");
        status(env, "越界放弃");
        return -1;
    }

    /* trampoline (offsets from cave):
     *  hash(note_id w25 * seed) >> 22 - 512 -> w5(hash) + scvtf -> *scale
     *  = offset s, stash offset in callee-saved d8 (dead at this point),
     *  log (note, hash, offset_us) via __android_log_print, restore d8,
     *  then replicate the original add/mov and return to the bl.
     *  literals: 0x5C scale f64, 0x64 1e6 f64, 0x6C logfn u64;
     *  strings:  0x78 tag, 0x88 fmt.
     *  Any seed is scrambled by the Knuth multiplier so that even tiny
     *  user seeds give id*seed high bits that vary with note id. */
    uint32_t seed = jit->seed ? jit->seed : (uint32_t)time(NULL);
    seed = seed * 0x9E3779B1u + 0x9E3779B1u;
    seed |= 1u; /* keep the multiplier odd for hash quality */
    unsigned char img[JITTER_CAVE_NEED];
    memset(img, 0, sizeof(img));
    uint32_t* w = (uint32_t*)img;
    int n = 0;
    w[n++] = 0x52800000u | ((seed & 0xFFFFu) << 5) | 9u;          /* movz w9,#lo */
    w[n++] = 0x72A00000u | (((seed >> 16) & 0xFFFFu) << 5) | 9u;  /* movk w9,#hi */
    w[n++] = 0x1B007C00u | (9u << 16) | (25u << 5) | 9u;          /* mul w9,w25,w9 */
    w[n++] = 0x53000000u | (22u << 16) | (31u << 10) | (9u << 5) |
             9u; /* lsr w9,w9,#22 (imms=31) */
    w[n++] = 0x51000000u | (512u << 10) | (9u << 5) | 9u;         /* sub w9,w9,#512 */
    w[n++] = 0x2A0903E5u;               /* mov w5,w9 (save hash) */
    w[n++] = 0x5E61D800u | (9u << 5);   /* scvtf d0,w9 */
    w[n++] = 0x5C000001u | (16u << 5);  /* ldr d1,[pc,#64] scale@0x5C */
    w[n++] = 0x1E610800u | (1u << 16);  /* fmul d0,d0,d1 -> offset seconds */
    w[n++] = 0x1E604008u;               /* fmov d8,d0 (stash, callee-saved) */
    w[n++] = 0x5C000001u | (15u << 5);  /* ldr d1,[pc,#60] 1e6@0x64 */
    w[n++] = 0x1E610800u | (1u << 16);  /* fmul d0,d0,d1 -> offset us */
    w[n++] = 0x1E780004u;               /* fcvtzs w4,d0 (int us) */
    w[n++] = 0x2A1903E3u;               /* mov w3,w25 (note id) */
    w[n++] = 0xD2800080u;               /* mov x0,#4 (ANDROID_LOG_INFO) */
    w[n++] = 0x100001E1u;               /* adr x1,tag@0x78 (imm21=15) */
    w[n++] = 0x10000242u;               /* adr x2,fmt@0x88 (imm21=18) */
    w[n++] = 0x58000149u;               /* ldr x9,[pc,#40] logfn@0x6C */
    w[n++] = 0xD63F0120u;               /* blr x9 (__android_log_print) */
    w[n++] = 0x1E604100u;               /* fmov d0,d8 (restore offset s) */
    w[n++] = 0x910162C0u;               /* add x0,x22,#0x58 */
    w[n++] = 0x2A1F03E1u;               /* mov w1,wzr */
    w[n++] = b_insn(cave + 0x58u, pc + 0x0Cu); /* b back to the bl */
    /* n == 23 instructions -> literals at 0x5C */
    double scale =
        (jit->sigma_ms / 1000.0) * 1.7320508075688772 / 512.0; /* R/512 */
    double e6 = 1.0e6;
    uintptr_t logfn = (uintptr_t)&__android_log_print;
    memcpy(img + 0x5C, &scale, 8);
    memcpy(img + 0x64, &e6, 8);
    memcpy(img + 0x6C, &logfn, 8);
    strcpy((char*)img + 0x78, "PhiraAgent");
    strcpy((char*)img + 0x88, "jitter-note: note=%d hash=%d off_us=%d");
    size_t img_len = 0x88u + sizeof("jitter-note: note=%d hash=%d off_us=%d");

    uint32_t b = b_insn(pc, cave);
    int64_t d1 = (int64_t)cave - (int64_t)pc;
    int64_t d2 = (int64_t)(pc + 0x0Cu) - (int64_t)(cave + 0x58u);
    if ((d1 & 3) || (d2 & 3) || d1 > 0x7FFFFFFLL || d1 < -0x8000000LL ||
        d2 > 0x7FFFFFFLL || d2 < -0x8000000LL) {
        report(env, "JITTER: branch out of range - abort");
        status(env, "越界放弃");
        return -1;
    }

    status(env, "校验注入点");
    if (!patch_bytes(cave, img, img_len)) {
        LOGE("jitter: trampoline write failed - abort");
        report(env, "JITTER: trampoline write failed - abort");
        status(env, "写入失败");
        return -1;
    }
    /* read back the cave image for byte-level confirmation */
    LOGI("jitter cave: %08x %08x %08x %08x %08x %08x %08x %08x",
         ((uint32_t*)cave)[0], ((uint32_t*)cave)[1], ((uint32_t*)cave)[2],
         ((uint32_t*)cave)[3], ((uint32_t*)cave)[4], ((uint32_t*)cave)[5],
         ((uint32_t*)cave)[6], ((uint32_t*)cave)[7]);
    if (!patch_word(pc, b)) {
        LOGE("jitter: branch patch failed - abort");
        report(env, "JITTER: branch patch failed - abort");
        status(env, "写入失败");
        return -1;
    }
    LOGI("jitter: std~=%dms seed=%08x cave=+0x%lx patched", jit->sigma_ms,
         seed, cave - rs);
    report(env, "JITTER: std~=%dms seed=%08x cave=+0x%lx", jit->sigma_ms, seed,
           cave - rs);
    status(env, "抖动注入");
    return 1;
}

/* bonus by distance from enclosing function entry (proxy: last `ret`,
 * prologue marker must follow within 6 insns). The autoplay check lives
 * in the first few hundred bytes of judge::update - noise candidates
 * usually do not. */
static int entry_bonus(uintptr_t rs, uintptr_t re, uintptr_t pc) {
    for (uintptr_t p = pc - 4; p >= rs && pc - p < 0x20000; p -= 4) {
        uint32_t w = *(volatile uint32_t*)p;
        if (w != 0xD65F03Cu) continue; /* ret */
        for (int k = 1; k <= 6; k++) {
            uintptr_t q = p + 4 * (uintptr_t)k;
            if (q + 4 > re || q >= pc) break;
            uint32_t w2 = *(volatile uint32_t*)q;
            int sub_sp = (w2 & 0xFFC003FFu) == 0xD10003FFu;
            int stp_fl = (w2 & 0xFFFFu) == 0x7BFDu && (w2 >> 24) == 0xA9u;
            if (sub_sp || stp_fl) {
                uintptr_t dist = pc - p - 4;
                return dist <= 512 ? 2 : (dist <= 4096 ? 1 : 0);
            }
        }
        break; /* ret without adjacent prologue: not a function boundary */
    }
    return 0;
}

/* +1 when a `ldrb wN,[xM,#imm]` sits within 8 insns before the branch -
 * the autoplay test loads config.mods as a byte right beforehand */
static int modload_bonus(uintptr_t rs, uintptr_t pc) {
    for (int k = 1; k <= 8; k++) {
        uintptr_t q = pc - 4 * (uintptr_t)k;
        if (q < rs) break;
        uint32_t w = *(volatile uint32_t*)q;
        if ((w >> 22) == 0x0E5u) return 1; /* ldrb imm12 form */
    }
    return 0;
}

static int rewrite_branch(uintptr_t rs, uintptr_t re, uintptr_t pc,
                          uint32_t insn, int decision) {
    uint32_t nw;

    if (decision == 2) {
        nw = 0xD503201Fu; /* NOP: fall through into the autoplay block */
    } else {
        int32_t im = branch_imm(insn);
        if (im == 0 || im == -1) {
            LOGE("degenerate branch offset @ %p", (void*)pc);
            return 0;
        }
        uintptr_t target = pc + (uintptr_t)(im * 4);
        if (target < rs || target >= re || (target & 3u)) {
            LOGE("target %p out of range", (void*)target);
            return 0;
        }
        nw = 0x14000000u | ((uint32_t)im & 0x03FFFFFFu);
    }

    if (!patch_word(pc, nw)) return 0;
    LOGI("PATCHED %08x -> %08x @ %p", insn, nw, (void*)pc);
    return 1;
}

/* ------------------------------------------------------------------ */
/* attempt                                                             */
/* ------------------------------------------------------------------ */

static int g_lib_polls = 0;
static int g_failed = 0;

/* panel-direct reporting: mirror progress onto the in-app overlay so
 * diagnosis never depends on logcat availability */
static jclass g_cls = NULL;
static jmethodID g_report_mid = NULL;
static jmethodID g_status_mid = NULL;

static void report(JNIEnv* env, const char* fmt, ...) {
    if (!env || !g_report_mid) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jstring s = (*env)->NewStringUTF(env, buf);
    if (!s) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallStaticVoidMethod(env, g_cls, g_report_mid, s);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, s);
}

/* short Chinese state label for the overlay title bar */
static void status(JNIEnv* env, const char* fmt, ...) {
    if (!env || !g_status_mid) return;
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jstring s = (*env)->NewStringUTF(env, buf);
    if (!s) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallStaticVoidMethod(env, g_cls, g_status_mid, s);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, s);
}

static int attempt(JNIEnv* env, const profile_t* pf, uintptr_t rs, uintptr_t re,
                   int verbose) {
    if (re <= rs || re - rs > MAX_SPAN_BYTES) return 0;

    /* explicit PATCH_AT entries run first: verify word, then write */
    if (pf->npatch_ats > 0) {
        status(env, "校验注入点");
        for (int i = 0; i < pf->npatch_ats; i++) {
            const patch_at_t* pa = &pf->patch_ats[i];
            uintptr_t pc = rs + (uintptr_t)pa->off;
            if (pc < rs || pc + 4 > re) {
                report(env, "PATCH_AT[%d] @+%lx out of span - abort", i,
                       pa->off);
                status(env, "越界放弃");
                return -1;
            }
            uint32_t cur = *(volatile uint32_t*)pc;
            if (cur != pa->expect) {
                report(env, "PATCH_AT[%d] @+%lx: %08x != expect %08x - abort",
                       i, pa->off, cur, pa->expect);
                status(env, "校验不符");
                return -1;
            }
            uint32_t nw = pa->repl;
            if (!nw) { /* derive: tbnz -> unconditional B to same target */
                int32_t im = branch_imm(pa->expect);
                nw = 0x14000000u | ((uint32_t)im & 0x03FFFFFFu);
            }
            if (!patch_word(pc, nw)) {
                report(env, "PATCH_AT[%d] write failed - abort", i);
                status(env, "写入失败");
                return -1;
            }
            LOGI("PATCH_AT[%d] %08x -> %08x @ %p", i, pa->expect, nw,
                 (void*)pc);
        }
        report(env, "PATCH_AT: %d site(s) patched", pf->npatch_ats);
        status(env, "注入成功");
        /* optional jitter: never fatal if it fails - the branch patch alone
         * is the core injection, jitter only makes the record plausible. */
        if (pf->jit.enabled) {
            int jr = apply_jitter(env, pf, rs, re);
            if (jr < 0) {
                LOGW("jitter skipped (fail-safe, PATCH_AT ok)");
                report(env, "JITTER skipped (fail-safe, PATCH_AT ok)");
            } else if (jr == 0) {
                LOGI("jitter disabled (sigma=0)");
            }
        }
        return 1;
    }

    /* no PATCH_AT entries: jitter alone is pointless without the branch
     * patch (auto never runs), so fall through to the scan path. */

    /* one-shot scan: the 16MB walk costs ~10s, cache results per span */
    static cand_t cands[MAX_CANDS];
    static int n;
    static long raw_tbz, raw_tst, raw_cbz, gated_out;
    static int truncated;
    static uintptr_t cached_rs, cached_re;
    if (cached_rs != rs || cached_re != re) {
        n = 0;
        truncated = 0;
        status(env, "扫描中");
        collect_structural(pf, rs, re, cands, &n, &truncated, &raw_tbz,
                           &raw_tst, &raw_cbz, &gated_out);
        collect_patterns(pf, rs, re, cands, &n);
        cached_rs = rs;
        cached_re = re;
    }

    if (verbose)
        report(env,
               "span %ldMB: raw tbz=%ld tst=%ld cbz=%ld gated=%ld "
               "cand=%d%s",
               (long)((re - rs) >> 20), raw_tbz, raw_tst, raw_cbz, gated_out,
               n, truncated ? "(truncated)" : "");

    if (n == 0) {
        if (verbose) report(env, "scan [%lx,%lx): no candidates", rs, re);
        status(env, "未找到候选");
        return 0;
    }
    LOGI("candidates in [%p,%p): %d (max_cand=%d)", (void*)rs, (void*)re, n,
         pf->max_cand);

    /* score every candidate once (+entry proximity, +mods-byte load nearby) */
    status(env, "计算评分");
    side_res_t res[MAX_CANDS];
    int scores[MAX_CANDS];
    for (int i = 0; i < n; i++) {
        res[i] = resolve_auto_side_ex(rs, re, cands[i].pc, cands[i].insn);
        scores[i] = cand_score(&res[i]) + entry_bonus(rs, re, cands[i].pc) +
                    modload_bonus(rs, cands[i].pc);
    }

    /* detailed candidate dump (up to 8): form, offset, insn, site dist,
     * branch target, polarity decision and score */
    int dump = n > 8 ? 8 : n;
    for (int i = 0; verbose && i < dump; i++) {
        int32_t im = branch_imm(cands[i].insn);
        report(env,
               "cand%d %s @+%lx %08x d=%ld tgt=%+ld dec=%d h=%d st%d sf%d",
               i, form_name(cands[i].insn), cands[i].pc - rs, cands[i].insn,
               dist_to_sites(cands[i].pc), (long)im * 4, res[i].decision,
               res[i].heuristic, res[i].st, res[i].sf);
    }

    if (!pf->best_of_n) {
        /* legacy strict mode: candidate count must match exactly */
        if (n != pf->max_cand) {
            LOGE("ambiguous (%d != %d) - refusing to patch", n, pf->max_cand);
            report(env, "ambiguous (%d != max_cand), BEST_OF_N off - abort", n);
            status(env, "数量不符");
            return -1;
        }
        for (int i = 0; i < n; i++)
            if (!rewrite_branch(rs, re, cands[i].pc, cands[i].insn,
                                res[i].decision))
                return -1;
        return 1;
    }

    /* group selection: patch ALL candidates sharing the strictly top score
     * when they are the same uniform idiom (compiler duplicates the check
     * via jump threading - judge.rs:413 appears as identical copies). */
    int top = 0;
    for (int i = 0; i < n; i++)
        if (scores[i] > scores[top]) top = i;
    int grp[MAX_CANDS], ng = 0;
    for (int i = 0; i < n && ng < MAX_CANDS; i++)
        if (scores[i] == scores[top]) grp[ng++] = i;

    int ok = scores[top] >= 4 && ng >= 1 && ng <= MAX_PATCH_GROUP;
    for (int i = 0; ok && i < ng; i++) {
        int j = grp[i];
        if (cands[j].insn != cands[grp[0]].insn ||
            res[j].decision != res[grp[0]].decision)
            ok = 0; /* mixed idiom inside the top group: refuse */
    }
    LOGI("group: score=%d members=%d ok=%d", scores[top], ng, ok);
    if (!ok) {
        report(env,
               "no confident group (%d cands, top score=%d, tied=%d%s) - abort",
               n, scores[top], ng,
               ng > MAX_PATCH_GROUP ? ", too many" : "");
        status(env, "歧义放弃");
        return -1;
    }
    for (int i = 0; i < ng; i++) {
        int j = grp[i];
        report(env, "patching cand%d (%s @+%lx -> %s)", j,
               form_name(cands[j].insn), cands[j].pc - rs,
               res[j].decision == 2 ? "NOP" : "B target");
        status(env, "注入+%lx", cands[j].pc - rs);
        if (!rewrite_branch(rs, re, cands[j].pc, cands[j].insn,
                            res[j].decision))
            return -1;
    }
    return 1;
}

/* status codes: 1 = patched; -1 = target lib never appeared;
 * -2 = lib seen but no structural/pattern match; -3 = ambiguous, aborted */
JNIEXPORT jint JNICALL
Java_cn_test_phirauto_PhiraAgent_arm(JNIEnv* env, jclass clazz, jobject assets) {
    g_cls = (jclass)(*env)->NewGlobalRef(env, clazz);
    g_report_mid = (*env)->GetStaticMethodID(env, clazz, "report",
                                             "(Ljava/lang/String;)V");
    g_status_mid = (*env)->GetStaticMethodID(env, clazz, "reportStatus",
                                             "(Ljava/lang/String;)V");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

    profile_t pf;
    profile_load(&pf, env, assets);
    LOGI("agent up: lib=%s window=%ld probes=%d patterns=%d max_cand=%d",
         g_lib_name, pf.window, pf.nprobes, pf.npats, pf.max_cand);
    report(env,
           "profile: lib=%s win=%ld probes=%d pats=%d max=%d ats=%d jit=%s",
           g_lib_name, pf.window, pf.nprobes, pf.npats, pf.max_cand,
           pf.npatch_ats,
           pf.jit.enabled ? "on" : "off");
    status(env, "自检");

    long waited = 0;
    int ever_seen = 0;
    while (waited < TIMEOUT_MS) {
        if (g_failed) {
            LOGE("giving up permanently - nothing was written");
            report(env, "ambiguous candidates - aborted, nothing written");
            status(env, "已放弃");
            return -3;
        }
        span_t spans[MAX_SPANS];
        int n = parse_maps(spans, MAX_SPANS);
        if (n > 0) {
            if (!ever_seen) {
                int xe = 0;
                for (int i = 0; i < n; i++) xe += spans[i].exec;
                report(env, "lib mapped: %d span(s), exec=%d", n, xe);
                status(env, "定位段");
            }
            {
                /* re-map / layout change => rebuild fingerprint + rescan */
                static uintptr_t last_base = 0;
                if (last_base != spans[0].start) {
                    last_base = spans[0].start;
                    g_xref_done = 0;
                }
            }
            if (!g_xref_done) {
                build_xref(env, spans, n, &pf);
                g_xref_done = 1;
            }
            ever_seen = 1;
            g_lib_polls++;
            int verbose = g_lib_polls <= 2;
            int done = 0, decisive_fail = 0;
            for (int i = 0; i < n && !done; i++) {
                if (!spans[i].exec) continue;
                int r =
                    attempt(env, &pf, spans[i].start, spans[i].end, verbose);
                if (r == 1) done = 1;
                else if (r < 0) decisive_fail = 1;
            }
            if (done) {
                LOGI("injection complete after %ld ms", waited);
                status(env, "注入成功");
                return 1;
            }
            if (decisive_fail && g_lib_polls >= LIB_POLLS_BEFORE_FAIL) {
                g_failed = 1;
            }
        } else {
            g_lib_polls = 0;
            status(env, "等待so");
        }
        if ((waited / POLL_MS) % 25 == 24)
            report(env, "waiting... %lds seen=%d polls=%d", waited / 1000,
                   ever_seen, g_lib_polls);
        usleep(POLL_MS * 1000);
        waited += POLL_MS;
    }
    LOGE("timeout after %ld ms - target '%s' %s, nothing was written", waited,
         g_lib_name, ever_seen ? "seen but not matched" : "never appeared");
    if (!ever_seen) {
        dump_loaded_libs();
        report(env, "timeout: '%s' never appeared (see logcat for lib list)",
               g_lib_name);
        status(env, "未找到so");
        return -1;
    }
    report(env, "timeout: lib seen but no match");
    status(env, "超时");
    return -2;
}
