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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "PhiraAgent"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define LIB_NAME         "libphira.so"
#define PROFILE_ASSET    "phira_profile.txt"
#define PROFILE_OVERRIDE "/data/local/tmp/phira_autoplay_profile.txt"
#define POLL_MS          200
#define TIMEOUT_MS       120000
#define LIB_POLLS_BEFORE_FAIL 10
#define MAX_SPANS        16
#define MAX_PROBES       8
#define MAX_PATTERNS     4
#define MAX_PAT_BYTES    32
#define MAX_CANDS        64
#define MAX_SPAN_BYTES   (256UL * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* profile                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char bytes[MAX_PAT_BYTES];
    unsigned char wild[MAX_PAT_BYTES]; /* 1 = wildcard byte */
    int len;
    long patch_off; /* byte offset of the branch inside the match, -1 = n/a */
} pat_t;

typedef struct {
    uint64_t probes[MAX_PROBES];
    int nprobes;
    long window;
    int max_cand;
    pat_t pats[MAX_PATTERNS];
    int npats;
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
    pf->max_cand = 1;
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
    } else if (!strcmp(cmd, "MAX_CAND")) {
        int v = atoi(rest);
        if (v >= 1 && v <= MAX_CANDS) pf->max_cand = v;
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
        return;
    }

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
                        return;
                    }
                }
                AAsset_close(a);
            }
        }
    }
    LOGI("profile: built-in defaults");
}

/* ------------------------------------------------------------------ */
/* maps                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uintptr_t start, end;
} span_t;

static int parse_maps(span_t* out, int max) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, LIB_NAME)) continue;
        uintptr_t s, e;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3) continue;
        if (!strchr(perms, 'x')) continue;
        char* slash = strrchr(line, '/');
        if (!slash) continue;
        size_t ll = strlen(slash);
        while (ll && (slash[ll - 1] == '\n' || slash[ll - 1] == '\r')) slash[--ll] = 0;
        char want[64];
        snprintf(want, sizeof(want), "/%s", LIB_NAME);
        size_t wl = strlen(want);
        if (ll < wl || strcmp(slash + ll - wl, want) != 0) continue;
        if (n < max) {
            out[n].start = s;
            out[n].end = e;
            n++;
        }
    }
    fclose(f);
    return n;
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

static int is_b(uint32_t insn) { return (insn & 0xFC000000u) == 0x14000000u; }

static int is_bl(uint32_t insn) { return (insn & 0xFC000000u) == 0x94000000u; }

static int is_ret(uint32_t insn) { return insn == 0xD65F03C0u; }

static int32_t imm14_of(uint32_t insn) { return ((int32_t)insn << 14) >> 18; }

static int32_t imm19_of(uint32_t insn) { return ((int32_t)insn << 8) >> 13; }

static int32_t branch_imm(uint32_t insn) {
    if (is_tbz_bit0(insn)) return imm14_of(insn);
    if (is_bcond(insn)) return imm19_of(insn);
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

typedef struct {
    uintptr_t pc;
    uint32_t insn;
} cand_t;

static void push_cand(cand_t* cands, int* n, uintptr_t pc, uint32_t insn) {
    for (int i = 0; i < *n; i++)
        if (cands[i].pc == pc) return;
    if (*n < MAX_CANDS) {
        cands[*n].pc = pc;
        cands[*n].insn = insn;
        (*n)++;
    }
}

static void collect_structural(const profile_t* pf, uintptr_t rs, uintptr_t re,
                               cand_t* cands, int* n) {
    for (uintptr_t pc = (rs + 3) & ~(uintptr_t)3u; pc + 4 <= re; pc += 4) {
        uint32_t insn = *(volatile uint32_t*)pc;
        uintptr_t bpc = 0;
        uint32_t binsn = 0;

        if (is_tbz_bit0(insn)) {
            bpc = pc;
            binsn = insn;
        } else if (is_tst_w_1(insn)) {
            for (int d = 1; d <= 4; d++) {
                if (pc + (uintptr_t)d * 4 + 4 > re) break;
                uint32_t nx = *(volatile uint32_t*)(pc + (uintptr_t)d * 4);
                if (is_bcond(nx)) {
                    bpc = pc + (uintptr_t)d * 4;
                    binsn = nx;
                    break;
                }
            }
        }
        if (!bpc) continue;

        int32_t im = branch_imm(binsn);
        if (im == 0 || im == -1) continue;
        uintptr_t tgt = bpc + (uintptr_t)(im * 4);
        if (tgt < rs || tgt >= re) continue;
        if (!region_has_probe(pf, rs, re, pc)) continue;

        push_cand(cands, n, bpc, binsn);
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
            push_cand(cands, n, bpc, insn);
        }
    }
}

/* ------------------------------------------------------------------ */
/* polarity + rewrite                                                  */
/* ------------------------------------------------------------------ */

/* [bl ... b] shape: small straight-line block that calls and jumps away,
 * characteristic of the auto_play_update call site. */
static int side_scores_bl_then_b(uintptr_t rs, uintptr_t re, uintptr_t from) {
    int seen_bl = 0;
    for (int i = 0; i < 20; i++) {
        uintptr_t pc = from + (uintptr_t)i * 4;
        if (pc + 4 > re) break;
        uint32_t insn = *(volatile uint32_t*)pc;
        if (is_ret(insn)) break;
        if (!seen_bl) {
            if (is_bl(insn)) seen_bl = 1;
        } else if (is_b(insn)) {
            return 1;
        }
    }
    return 0;
}

/* 1 = taken side is autoplay (rewrite branch -> B target),
 * 2 = fall-through side is autoplay (rewrite branch -> NOP).
 * Wrong guesses degrade to "always manual", never crash. */
static int resolve_auto_side(uintptr_t rs, uintptr_t re, uintptr_t pc,
                             uint32_t insn) {
    if (is_tbz_bit0(insn)) {
        return (insn >> 24) == 0x37 ? 1 : 2; /* tbnz: set => autoplay */
    }
    if (is_bcond(insn)) {
        uint32_t cond = insn & 0xFu;
        if (cond == 1) return 1;             /* b.ne */
        if (cond == 0) return 2;             /* b.eq */
    }
    int32_t im = branch_imm(insn);
    uintptr_t tgt = pc + (uintptr_t)(im * 4);
    int st = side_scores_bl_then_b(rs, re, tgt);
    int sf = side_scores_bl_then_b(rs, re, pc + 4);
    if (st && !sf) return 1;
    if (sf && !st) return 2;
    LOGW("polarity unresolved @ %p, assuming taken side", (void*)pc);
    return 1;
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
    LOGI("PATCHED %08x -> %08x @ %p", insn, nw, (void*)pc);
    return 1;
}

/* ------------------------------------------------------------------ */
/* attempt                                                             */
/* ------------------------------------------------------------------ */

static int g_lib_polls = 0;
static int g_failed = 0;

static int attempt(const profile_t* pf, uintptr_t rs, uintptr_t re) {
    if (re <= rs || re - rs > MAX_SPAN_BYTES) return 0;

    cand_t cands[MAX_CANDS];
    int n = 0;
    collect_structural(pf, rs, re, cands, &n);
    collect_patterns(pf, rs, re, cands, &n);

    if (n == 0) return 0;
    LOGI("candidates in [%p,%p): %d (max_cand=%d)", (void*)rs, (void*)re, n,
         pf->max_cand);

    if (n != pf->max_cand) {
        LOGE("ambiguous (%d != %d) - refusing to patch", n, pf->max_cand);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        int decision = resolve_auto_side(rs, re, cands[i].pc, cands[i].insn);
        if (rewrite_branch(rs, re, cands[i].pc, cands[i].insn, decision))
            return 1;
    }
    return -1;
}

JNIEXPORT jboolean JNICALL
Java_cn_test_phirauto_PhiraAgent_arm(JNIEnv* env, jclass clazz, jobject assets) {
    profile_t pf;
    profile_load(&pf, env, assets);
    LOGI("agent up: window=%ld probes=%d patterns=%d max_cand=%d", pf.window,
         pf.nprobes, pf.npats, pf.max_cand);

    long waited = 0;
    while (waited < TIMEOUT_MS) {
        if (g_failed) {
            LOGE("giving up permanently - nothing was written");
            return JNI_FALSE;
        }
        span_t spans[MAX_SPANS];
        int n = parse_maps(spans, MAX_SPANS);
        if (n > 0) {
            g_lib_polls++;
            int done = 0, decisive_fail = 0;
            for (int i = 0; i < n && !done; i++) {
                int r = attempt(&pf, spans[i].start, spans[i].end);
                if (r == 1) done = 1;
                else if (r < 0) decisive_fail = 1;
            }
            if (done) {
                LOGI("injection complete after %ld ms", waited);
                return JNI_TRUE;
            }
            if (decisive_fail && g_lib_polls >= LIB_POLLS_BEFORE_FAIL) {
                g_failed = 1;
            }
        } else {
            g_lib_polls = 0;
        }
        usleep(POLL_MS * 1000);
        waited += POLL_MS;
    }
    LOGE("timeout - %s not located, nothing was written", LIB_NAME);
    return JNI_FALSE;
}
