#include "server.h"

/* ---------------------------------------------------------------------------
 * Hot key detection — WavingSketch (Li et al., KDD 2020).
 *
 * Reference: "WavingSketch: An Unbiased and Generic Sketch for Finding Top-k
 *            Items in Data Streams" (https://dl.acm.org/doi/10.1145/3394486.3403208)
 *
 * Algorithm matches §3.1 of the paper exactly. See server.h for the prose
 * description; the implementation comments below cite Algorithm 1.
 *
 * Hashing: Bob Jenkins' lookup3 (http://burtleburtle.net/bob/hash/evahash.html),
 * the same hash family the paper authors used. We compute one 32-bit hash per
 * key to pick the bucket and one bit of a second 32-bit hash to pick the sign.
 * --------------------------------------------------------------------------*/

/* ===========================================================================
 * Bob Jenkins lookup3 hash
 * (Adapted from http://burtleburtle.net/bob/hash/evahash.html, public domain)
 * ==========================================================================*/

#define rot(x, k) (((x) << (k)) | ((x) >> (32 - (k))))

#define mix(a, b, c) \
    do { \
        a -= c; a ^= rot(c, 4);  c += b; \
        b -= a; b ^= rot(a, 6);  a += c; \
        c -= b; c ^= rot(b, 8);  b += a; \
        a -= c; a ^= rot(c, 16); c += b; \
        b -= a; b ^= rot(a, 19); a += c; \
        c -= b; c ^= rot(b, 4);  b += a; \
    } while (0)

#define final(a, b, c) \
    do { \
        c ^= b; c -= rot(b, 14); \
        a ^= c; a -= rot(c, 11); \
        b ^= a; b -= rot(a, 25); \
        c ^= b; c -= rot(b, 16); \
        a ^= c; a -= rot(c, 4);  \
        b ^= a; b -= rot(a, 14); \
        c ^= b; c -= rot(b, 24); \
    } while (0)

/* hashlittle2: produces two independent 32-bit hashes (`*pc` is primary,
 * `*pb` is secondary). Same byte-order behavior as Bob's reference code on
 * little-endian machines. */
static void hashlittle2(const void *key, size_t length,
                        uint32_t *pc, uint32_t *pb) {
    uint32_t a, b, c;

    a = b = c = 0xdeadbeef + ((uint32_t)length) + (*pc);
    c += (*pb);

    const uint8_t *k = (const uint8_t *)key;
    while (length > 12) {
        a += ((uint32_t)k[0])       | ((uint32_t)k[1]  << 8)
           | ((uint32_t)k[2]  << 16) | ((uint32_t)k[3]  << 24);
        b += ((uint32_t)k[4])       | ((uint32_t)k[5]  << 8)
           | ((uint32_t)k[6]  << 16) | ((uint32_t)k[7]  << 24);
        c += ((uint32_t)k[8])       | ((uint32_t)k[9]  << 8)
           | ((uint32_t)k[10] << 16) | ((uint32_t)k[11] << 24);
        mix(a, b, c);
        length -= 12;
        k += 12;
    }

    /* Last block: handle remaining bytes 1..12. */
    switch (length) {
        case 12: c += ((uint32_t)k[11]) << 24; /* fallthrough */
        case 11: c += ((uint32_t)k[10]) << 16; /* fallthrough */
        case 10: c += ((uint32_t)k[9])  << 8;  /* fallthrough */
        case 9:  c += k[8];                    /* fallthrough */
        case 8:  b += ((uint32_t)k[7])  << 24; /* fallthrough */
        case 7:  b += ((uint32_t)k[6])  << 16; /* fallthrough */
        case 6:  b += ((uint32_t)k[5])  << 8;  /* fallthrough */
        case 5:  b += k[4];                    /* fallthrough */
        case 4:  a += ((uint32_t)k[3])  << 24; /* fallthrough */
        case 3:  a += ((uint32_t)k[2])  << 16; /* fallthrough */
        case 2:  a += ((uint32_t)k[1])  << 8;  /* fallthrough */
        case 1:  a += k[0]; break;
        case 0:  *pc = c; *pb = b; return;
    }
    final(a, b, c);
    *pc = c;
    *pb = b;
}

#undef rot
#undef mix
#undef final

/* ===========================================================================
 * Command argument parsing helpers
 * ==========================================================================*/
#define HOTKEY_FILTER_ALL   -1
#define HOTKEY_FILTER_WRITE  0
#define HOTKEY_FILTER_READ   1

/* Parse [TYPE {read|write|all}] argument.
 * Returns C_OK on success, C_ERR on error (error reply already sent). */
static int parseHotkeyFilterArgs(client *c, int start_idx, int *filter_type) {
    int i = start_idx;

    *filter_type = HOTKEY_FILTER_ALL;

    while (i < c->argc) {
        if (!strcasecmp(objectGetVal(c->argv[i]), "TYPE") && i + 1 < c->argc) {
            char *t = objectGetVal(c->argv[i + 1]);
            if (!strcasecmp(t, "read"))
                *filter_type = HOTKEY_FILTER_READ;
            else if (!strcasecmp(t, "write"))
                *filter_type = HOTKEY_FILTER_WRITE;
            else if (!strcasecmp(t, "all"))
                *filter_type = HOTKEY_FILTER_ALL;
            else {
                addReplyError(c, "Invalid type. Use 'read', 'write', or 'all'");
                return C_ERR;
            }
            i += 2;
        } else {
            addReplyError(c, "Syntax error. Use [TYPE {read|write|all}]");
            return C_ERR;
        }
    }
    return C_OK;
}

/* ===========================================================================
 * WavingSketch geometry
 *
 * We split the user-configured K = max_keys into B buckets of L cells each.
 * The paper uses L=8 in its evaluation (see §4); we follow that choice.
 * B is derived as ceil(K / L) so the user's K request is honored.
 * ==========================================================================*/
#define HOTKEY_WS_CELLS_PER_BUCKET 8

/* Compute the bucket index and sign for a (key, dbid) pair using lookup3.
 *
 * We feed `dbid` into the hash by initializing the secondary seed (`*pb`) so
 * that the same key bytes in different databases produce different hashes.
 *
 * `*out_bucket` ∈ [0, b);  `*out_sign` ∈ {+1, -1}.
 */
static void hotkeyWSHash(const void *kbytes, size_t klen, int dbid, int b,
                         int *out_bucket, int *out_sign) {
    uint32_t pc = 0;
    uint32_t pb = (uint32_t)dbid;
    hashlittle2(kbytes, klen, &pc, &pb);
    /* `pc` selects the bucket. `pb` provides an independent stream of bits
     * for the sign — bit 0 of `pb` gives ±1 with equal probability. */
    *out_bucket = (int)(pc % (uint32_t)b);
    *out_sign = (pb & 1u) ? 1 : -1;
}

/* ===========================================================================
 * WavingSketch structure operations
 * ==========================================================================*/

static hotkeyWS *hotkeyWSNew(int max_keys) {
    int l = HOTKEY_WS_CELLS_PER_BUCKET;
    int b = (max_keys + l - 1) / l;
    if (b < 1) b = 1;

    hotkeyWS *s = zcalloc(sizeof(hotkeyWS));
    s->b = b;
    s->l = l;
    s->total = 0;
    s->buckets = zcalloc((size_t)b * sizeof(hotkeyWSBucket));
    for (int i = 0; i < b; i++) {
        s->buckets[i].cells = zcalloc((size_t)l * sizeof(hotkeyWSCell));
        s->buckets[i].count = 0;
        /* Empty cells start with flag=true (frequency=0 is the unbiased truth). */
        for (int j = 0; j < l; j++) s->buckets[i].cells[j].flag = 1;
    }
    return s;
}

static void hotkeyWSFreeBucketContents(hotkeyWS *s, int bucket_idx) {
    hotkeyWSBucket *bucket = &s->buckets[bucket_idx];
    for (int j = 0; j < s->l; j++) {
        if (bucket->cells[j].key) {
            sdsfree(bucket->cells[j].key);
            bucket->cells[j].key = NULL;
        }
        bucket->cells[j].frequency = 0;
        bucket->cells[j].key_len = 0;
        bucket->cells[j].dbid = 0;
        bucket->cells[j].slot = 0;
        bucket->cells[j].sign = 0;
        /* Empty cells start with flag=true: frequency=0 trivially equals true=0. */
        bucket->cells[j].flag = 1;
    }
    bucket->count = 0;
}

static void hotkeyWSFree(hotkeyWS *s) {
    if (!s) return;
    for (int i = 0; i < s->b; i++) {
        hotkeyWSFreeBucketContents(s, i);
        zfree(s->buckets[i].cells);
    }
    zfree(s->buckets);
    zfree(s);
}

static void hotkeyWSReset(hotkeyWS *s) {
    if (!s) return;
    for (int i = 0; i < s->b; i++) {
        hotkeyWSFreeBucketContents(s, i);
    }
    s->total = 0;
}

/* Find the heavy cell with the smallest (signed) frequency in this bucket.
 * Per the paper, biased cells (flag=false) can store a negative frequency,
 * so the comparison must be signed. */
static int hotkeyWSBucketMinCell(hotkeyWS *s, hotkeyWSBucket *bucket,
                                 int64_t *out_min_freq) {
    int min_idx = 0;
    int64_t min_freq = bucket->cells[0].frequency;
    for (int j = 1; j < s->l; j++) {
        if (bucket->cells[j].frequency < min_freq) {
            min_freq = bucket->cells[j].frequency;
            min_idx = j;
        }
    }
    *out_min_freq = min_freq;
    return min_idx;
}

/* Add an observation of (key, dbid) to the sketch — Algorithm 1 from §3.1
 * of the WavingSketch paper, line-by-line.
 *
 *  1: f_i_estimated = B[h(e_i)].count * s(e_i)
 *  2: if e_i in the Heavy Part of B[h(e_i)]:
 *  3:     update e_i's frequency in the Heavy Part   (frequency += 1)
 *  4:     if flag of e_i is false:
 *  5:         B[h(e_i)].count += s(e_i)
 *  6: else if the Heavy Part is not full:
 *  7:     insert e_i with <frequency=1, flag=true>
 *  8: else:
 *  9:     B[h(e_i)].count += s(e_i)
 * 10:     e_r = item with smallest frequency
 * 11:     f_r_estimated = frequency of e_r
 * 12:     if f_i_estimated >= f_r_estimated:
 * 13:         if flag of e_r is true:
 * 14:             B[h(e_i)].count += f_r_estimated * s(e_r)
 * 15:         replace e_r with <e_i, frequency = f_i_estimated + 1, flag=false>
 *
 * `slot` is metadata stored on the heavy cell for reporting only; it does
 * not affect the algorithm. */
static void hotkeyWSAdd(hotkeyWS *s, robj *key, int dbid, int slot) {
    if (!s || !key || !objectGetVal(key)) return;
    s->total++;

    const char *k = objectGetVal(key);
    uint32_t klen = (uint32_t)sdslen(objectGetVal(key));

    int bidx, sign_i;  /* sign_i = s(e_i) */
    hotkeyWSHash(k, (size_t)klen, dbid, s->b, &bidx, &sign_i);
    hotkeyWSBucket *bucket = &s->buckets[bidx];

    /* Prefetch keys in this bucket that pass the cheap fast-reject. */
    for (int j = 0; j < s->l; j++) {
        if (bucket->cells[j].key &&
            bucket->cells[j].key_len == klen &&
            bucket->cells[j].dbid == dbid) {
            valkey_prefetch(bucket->cells[j].key);
        }
    }

    /* Line 1: f_i_estimated = B[h(e_i)].count * s(e_i). */
    int64_t f_i_estimated = bucket->count * (int64_t)sign_i;

    /* Lines 2-5: e_i is in the Heavy Part. */
    int empty_idx = -1;
    for (int j = 0; j < s->l; j++) {
        if (!bucket->cells[j].key) {
            if (empty_idx == -1) empty_idx = j;
            continue;
        }
        if (bucket->cells[j].key_len != klen || bucket->cells[j].dbid != dbid) continue;
        if (memcmp(bucket->cells[j].key, k, klen) == 0) {
            /* Line 3: update e_i's frequency. */
            bucket->cells[j].frequency++;
            /* Lines 4-5: if flag is false, also update the bucket's counter. */
            if (!bucket->cells[j].flag) {
                bucket->count += sign_i;
            }
            return;
        }
    }

    /* Lines 6-7: Heavy Part is not full. Install <frequency=1, flag=true>. */
    if (empty_idx != -1) {
        bucket->cells[empty_idx].key = sdsnewlen(k, klen);
        bucket->cells[empty_idx].key_len = klen;
        bucket->cells[empty_idx].frequency = 1;
        bucket->cells[empty_idx].dbid = dbid;
        bucket->cells[empty_idx].slot = slot;
        bucket->cells[empty_idx].flag = 1;
        bucket->cells[empty_idx].sign = sign_i;
        return;
    }

    /* Lines 8-15: Heavy Part is full and e_i is not in it. */

    /* Line 9: B[h(e_i)].count += s(e_i). */
    bucket->count += sign_i;

    /* Line 10-11: find e_r — the cell with the smallest (signed) frequency. */
    int64_t f_r_estimated = 0;
    int min_idx = hotkeyWSBucketMinCell(s, bucket, &f_r_estimated);

    /* Line 12: if f_i_estimated >= f_r_estimated. Both are signed; biased
     * cells (flag=false) may store a negative frequency. */
    if (f_i_estimated >= f_r_estimated) {
        /* Lines 13-14: if e_r's flag is true (unbiased cell), absorb its
         * frequency into the bucket counter using e_r's cached sign. This
         * preserves the martingale property of the bucket counter when an
         * unbiased cell is displaced. */
        if (bucket->cells[min_idx].flag) {
            bucket->count += f_r_estimated * (int64_t)bucket->cells[min_idx].sign;
        }
        /* Line 15: replace e_r with <e_i, frequency = f_i_estimated + 1, flag=false>.
         * Note: bucket->count is NOT reset — it carries the accumulated bucket
         * history across displacements. */
        if (bucket->cells[min_idx].key) sdsfree(bucket->cells[min_idx].key);
        bucket->cells[min_idx].key = sdsnewlen(k, klen);
        bucket->cells[min_idx].key_len = klen;
        bucket->cells[min_idx].frequency = f_i_estimated + 1;
        bucket->cells[min_idx].dbid = dbid;
        bucket->cells[min_idx].slot = slot;
        bucket->cells[min_idx].flag = 0;
        bucket->cells[min_idx].sign = sign_i;
    }
}

/* ===========================================================================
 * Hotkey manager lifecycle
 * ==========================================================================*/

hotkeyManager *hotkeyManagerInit(int max_keys) {
    hotkeyManager *m = zcalloc(sizeof(hotkeyManager));
    m->read_summary = hotkeyWSNew(max_keys);
    m->write_summary = hotkeyWSNew(max_keys);
    return m;
}

void hotkeyManagerFree(hotkeyManager *m) {
    if (!m) return;
    if (m->read_summary) hotkeyWSFree(m->read_summary);
    if (m->write_summary) hotkeyWSFree(m->write_summary);
    zfree(m);
}

void hotkeyManagerReset(hotkeyManager *m) {
    if (!m) return;
    if (m->read_summary) hotkeyWSReset(m->read_summary);
    if (m->write_summary) hotkeyWSReset(m->write_summary);
}

/* ===========================================================================
 * Per-access detection hooks (called from lookupKey)
 * ==========================================================================*/

void readHotKeyDetection(robj *key, int slot, int dbid) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    hotkeyWSAdd(m->read_summary, key, dbid, slot);
}

void writeHotKeyDetection(robj *key, int slot, int dbid) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    hotkeyWSAdd(m->write_summary, key, dbid, slot);
}

/* ===========================================================================
 * HOTKEYS commands
 * ==========================================================================*/

typedef struct {
    sds key;
    int64_t count;   /* Signed; biased cells may store a negative frequency. */
    int slot;
    int dbid;
    int is_read;
} hotkeyWSCollected;

static int hotkeyWSCollectedCmp(const void *a, const void *b) {
    const hotkeyWSCollected *ea = a;
    const hotkeyWSCollected *eb = b;
    /* Sort by descending count. The paper's report (§3.1, line 1) uses
     *   f_estimated = B[h(e)].count * s(e)
     * for sorting; here we report each cell's stored frequency directly,
     * which is the unbiased estimate for flag=true cells and an upper-bound
     * surrogate for flag=false cells. */
    if (eb->count > ea->count) return 1;
    if (eb->count < ea->count) return -1;
    return 0;
}

static long long hotkeyEstimateQPS(int64_t count) {
    if (server.hotkey_window_seconds <= 0) return (long long)count;
    if (server.hotkey_sampling_ratio <= 0) return 0;
    long long scaled = (long long)count * 100LL;
    scaled /= server.hotkey_sampling_ratio;
    scaled /= server.hotkey_window_seconds;
    return scaled;
}

static int hotkeyWSCollectInto(hotkeyWS *s, hotkeyWSCollected *arr, int n, int is_read) {
    if (!s) return n;
    for (int i = 0; i < s->b; i++) {
        hotkeyWSBucket *bucket = &s->buckets[i];
        for (int j = 0; j < s->l; j++) {
            hotkeyWSCell *cell = &bucket->cells[j];
            if (!cell->key) continue;
            arr[n].key = cell->key;
            arr[n].count = cell->frequency;
            arr[n].slot = cell->slot;
            arr[n].dbid = cell->dbid;
            arr[n].is_read = is_read;
            n++;
        }
    }
    return n;
}

void hotkeysGetCommand(client *c) {
    int filter_type;
    if (parseHotkeyFilterArgs(c, 2, &filter_type) != C_OK) return;

    hotkeyManager *m = server.hotkey_manager;
    if (!m) {
        addReplyArrayLen(c, 0);
        return;
    }

    int cap = 0;
    if (filter_type != HOTKEY_FILTER_WRITE && m->read_summary)
        cap += m->read_summary->b * m->read_summary->l;
    if (filter_type != HOTKEY_FILTER_READ && m->write_summary)
        cap += m->write_summary->b * m->write_summary->l;
    if (cap == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    hotkeyWSCollected *arr = zmalloc((size_t)cap * sizeof(hotkeyWSCollected));
    int n = 0;
    if (filter_type != HOTKEY_FILTER_WRITE)
        n = hotkeyWSCollectInto(m->read_summary, arr, n, 1);
    if (filter_type != HOTKEY_FILTER_READ)
        n = hotkeyWSCollectInto(m->write_summary, arr, n, 0);

    if (n > 1) qsort(arr, n, sizeof(hotkeyWSCollected), hotkeyWSCollectedCmp);

    addReplyArrayLen(c, n);
    for (int i = 0; i < n; i++) {
        addReplyArrayLen(c, 10);
        addReplyBulkCString(c, "key");
        addReplyBulkCBuffer(c, arr[i].key, sdslen(arr[i].key));
        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, arr[i].is_read ? "read" : "write");
        addReplyBulkCString(c, "db");
        addReplyLongLong(c, arr[i].dbid);
        addReplyBulkCString(c, "slot");
        addReplyLongLong(c, arr[i].slot);
        addReplyBulkCString(c, "qps");
        addReplyLongLong(c, hotkeyEstimateQPS(arr[i].count));
    }
    zfree(arr);
}

void hotkeysResetCommand(client *c) {
    if (server.hotkey_manager) hotkeyManagerReset(server.hotkey_manager);
    addReply(c, shared.ok);
}

void hotkeysCommand(client *c) {
    char *subcmd = objectGetVal(c->argv[1]);
    if (!strcasecmp(subcmd, "get"))
        hotkeysGetCommand(c);
    else if (!strcasecmp(subcmd, "reset"))
        hotkeysResetCommand(c);
    else
        addReplySubcommandSyntaxError(c);
}

/* ===========================================================================
 * Invalidation hooks (cluster slot migration / FLUSH).
 *
 * The WavingSketch is a global structure (B buckets × L cells), but each
 * cell stores the key's `slot` and `dbid` as metadata. To selectively purge
 * entries belonging to a specific slot or db, we walk every cell and clear
 * the ones that match.
 *
 * When a heavy cell is cleared we reset it to the empty state (key=NULL,
 * frequency=0, flag=true). The bucket counter is left intact: that signed
 * counter accumulates ±1 contributions from many keys, including non-cleared
 * ones, so wiping it would lose information that's still meaningful for the
 * remaining cells in the same bucket.
 * ==========================================================================*/

/* Reset a single cell to its empty (post-allocation) state. */
static void hotkeyWSCellClear(hotkeyWSCell *cell) {
    if (cell->key) sdsfree(cell->key);
    cell->key = NULL;
    cell->key_len = 0;
    cell->frequency = 0;
    cell->dbid = 0;
    cell->slot = 0;
    cell->sign = 0;
    cell->flag = 1;
}

/* Walk every cell in `s` and clear those for which `match(cell, ctx)` returns 1.
 * `ctx` is an opaque caller-supplied pointer (e.g. address of a target int,
 * a struct holding multiple criteria, etc.). */
typedef int (*hotkeyWSCellMatch)(hotkeyWSCell *cell, void *ctx);
static void hotkeyWSInvalidateMatching(hotkeyWS *s,
                                       hotkeyWSCellMatch match,
                                       void *ctx) {
    if (!s) return;
    for (int i = 0; i < s->b; i++) {
        hotkeyWSBucket *bucket = &s->buckets[i];
        for (int j = 0; j < s->l; j++) {
            hotkeyWSCell *cell = &bucket->cells[j];
            if (!cell->key) continue;
            if (match(cell, ctx)) hotkeyWSCellClear(cell);
        }
    }
}

static int hotkeyWSCellMatchSlot(hotkeyWSCell *cell, void *ctx) {
    return cell->slot == *(int *)ctx;
}
static int hotkeyWSCellMatchDb(hotkeyWSCell *cell, void *ctx) {
    return cell->dbid == *(int *)ctx;
}

void hotkeyPurgeSlot(int slot) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m) return;
    hotkeyWSInvalidateMatching(m->read_summary, hotkeyWSCellMatchSlot, &slot);
    hotkeyWSInvalidateMatching(m->write_summary, hotkeyWSCellMatchSlot, &slot);
}

void hotkeyPurgeDb(int dbid) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m) return;
    hotkeyWSInvalidateMatching(m->read_summary, hotkeyWSCellMatchDb, &dbid);
    hotkeyWSInvalidateMatching(m->write_summary, hotkeyWSCellMatchDb, &dbid);
}

void hotkeyPurgeAll(void) {
    if (server.hotkey_manager) hotkeyManagerReset(server.hotkey_manager);
}

/* ===========================================================================
 * Config callbacks
 * ==========================================================================*/

int hotKeyEnabledCallback(const char **err) {
    UNUSED(err);
    if (!server.hotkey_enabled) {
        if (server.hotkey_manager) {
            hotkeyManagerFree(server.hotkey_manager);
            server.hotkey_manager = NULL;
        }
        return 1;
    }
    if (!server.hotkey_manager) {
        server.hotkey_manager = hotkeyManagerInit(server.hotkey_max_keys);
    }
    return 1;
}

int hotKeyMaxKeysCallback(const char **err) {
    UNUSED(err);
    if (server.hotkey_manager) {
        hotkeyManagerFree(server.hotkey_manager);
        server.hotkey_manager = hotkeyManagerInit(server.hotkey_max_keys);
    }
    return 1;
}
