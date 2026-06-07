#include "server.h"
#include <math.h>

/* ---------------------------------------------------------------------------
 * Lazy time-anchored exponential decay
 *
 * Instead of resetting the Space-Saving summaries on a fixed window boundary,
 * (count, error) decay exponentially toward zero over time. Decay is applied
 * lazily (at most once per tick) on access and on query, anchored to the
 * monotonic clock — so a stalled cron has no effect on accuracy. A key
 * arriving at a steady sampled rate r converges to count = r/lambda, so the
 * access rate is recovered as: qps = count * lambda * (100 / sampling_ratio).
 * --------------------------------------------------------------------------*/

/* ln(2), used to convert a half-life into a decay rate lambda = ln2/half_life. */
#define HOTKEY_LN2 0.6931471805599453
/* Amortize the O(K) decay sweep: advance decay at most once per this interval. */
#define HOTKEY_DECAY_TICK_US 100000 /* 100ms */
/* Evict an entry once its decayed count falls below this floor (less than half
 * a sampled hit) so faded keys release their slot. */
#define HOTKEY_MIN_COUNT 0.5

/* ===========================================================================
 * Space-Saving algorithm for Top-K heavy hitters
 *
 * Reference: Metwally, Agrawal, El Abbadi (2005),
 *   "Efficient Computation of Frequent and Top-k Elements in Data Streams".
 *
 * Maintains K (key, count, error) entries. Per observation:
 *   1. If key already tracked: count++.
 *   2. Else if room available (size < K): insert with count=1, error=0.
 *   3. Else: replace the entry with the smallest count; new entry takes
 *      count = min_count + 1, error = min_count.
 *
 * Guarantees:
 *   - For any tracked key: true_count is in [count - error, count].
 *   - Any key with true frequency f > N/K is guaranteed to be tracked.
 *
 * Storage: a flat unordered array of `capacity` entries. K is small
 * (default 16) so a linear scan finds membership / min entry in a few
 * cache lines. Avoiding the heap removes sift-down on every hit (the
 * common path) at the cost of an O(K) min-find on the rarer eviction path.
 * ==========================================================================*/

static hotkeySS *hotkeySSNew(int capacity) {
    hotkeySS *ss = zcalloc(sizeof(hotkeySS));
    ss->entries = zcalloc(capacity * sizeof(hotkeySSEntry));
    ss->capacity = capacity;
    ss->size = 0;
    return ss;
}

static void hotkeySSFree(hotkeySS *ss) {
    if (!ss) return;
    for (int i = 0; i < ss->size; i++) {
        if (ss->entries[i].key) sdsfree(ss->entries[i].key);
    }
    zfree(ss->entries);
    zfree(ss);
}

static void hotkeySSReset(hotkeySS *ss) {
    if (!ss) return;
    for (int i = 0; i < ss->size; i++) {
        if (ss->entries[i].key) sdsfree(ss->entries[i].key);
        ss->entries[i].key = NULL;
        ss->entries[i].count = 0.0;
        ss->entries[i].error = 0.0;
    }
    ss->size = 0;
}

/* Find (key, dbid) index in the summary by linear scan. -1 if not found. */
static int hotkeySSFind(hotkeySS *ss, const char *k, size_t klen, int dbid) {
    for (int i = 0; i < ss->size; i++) {
        if (ss->entries[i].key &&
            ss->entries[i].dbid == dbid &&
            sdslen(ss->entries[i].key) == klen &&
            memcmp(ss->entries[i].key, k, klen) == 0) {
            return i;
        }
    }
    return -1;
}

/* Find the index of the entry with the smallest count by linear scan. */
static int hotkeySSMinIdx(hotkeySS *ss) {
    int min_idx = 0;
    double min_count = ss->entries[0].count;
    for (int i = 1; i < ss->size; i++) {
        if (ss->entries[i].count < min_count) {
            min_count = ss->entries[i].count;
            min_idx = i;
        }
    }
    return min_idx;
}

/* Add a single observation of `key` to the summary. */
static void hotkeySSAdd(hotkeySS *ss, robj *key, int dbid, int slot) {
    sds k = objectGetVal(key);
    size_t klen = sdslen(k);

    /* Case 1: key already tracked — increment its count. */
    int idx = hotkeySSFind(ss, k, klen, dbid);
    if (idx >= 0) {
        ss->entries[idx].count += 1.0;
        ss->entries[idx].slot = slot;
        return;
    }

    /* Case 2: room available — append with count=1, error=0. */
    if (ss->size < ss->capacity) {
        idx = ss->size;
        ss->entries[idx].key = sdsnewlen(k, klen);
        ss->entries[idx].count = 1.0;
        ss->entries[idx].error = 0.0;
        ss->entries[idx].dbid = dbid;
        ss->entries[idx].slot = slot;
        ss->size++;
        return;
    }

    /* Case 3: full — replace the entry with the smallest count. The new
     * entry inherits the evicted count + 1; `error` records the maximum
     * possible overestimate vs. true count. */
    int min_idx = hotkeySSMinIdx(ss);
    double min_count = ss->entries[min_idx].count;
    sdsfree(ss->entries[min_idx].key);
    ss->entries[min_idx].key = sdsnewlen(k, klen);
    ss->entries[min_idx].count = min_count + 1.0;
    ss->entries[min_idx].error = min_count;
    ss->entries[min_idx].dbid = dbid;
    ss->entries[min_idx].slot = slot;
}

/* Decay every entry's (count, error) by f, drop entries that fade below the
 * eviction floor, and compact the array. */
static void hotkeySSDecayAndPrune(hotkeySS *ss, double f) {
    if (!ss) return;
    int w = 0;
    for (int i = 0; i < ss->size; i++) {
        if (!ss->entries[i].key) continue;
        double cnt = ss->entries[i].count * f;
        double err = ss->entries[i].error * f;
        if (cnt < HOTKEY_MIN_COUNT) {
            sdsfree(ss->entries[i].key);
            ss->entries[i].key = NULL;
            continue;
        }
        if (w != i) ss->entries[w] = ss->entries[i];
        ss->entries[w].count = cnt;
        ss->entries[w].error = err;
        w++;
    }
    ss->size = w;
}

/* ===========================================================================
 * Hotkey manager lifecycle
 * ==========================================================================*/

hotkeyManager *hotkeyManagerInit(int top_k) {
    hotkeyManager *m = zcalloc(sizeof(hotkeyManager));
    m->read_ss = hotkeySSNew(top_k);
    m->write_ss = hotkeySSNew(top_k);
    m->last_decay_us = getMonotonicUs();
    return m;
}

void hotkeyManagerFree(hotkeyManager *m) {
    if (!m) return;
    if (m->read_ss) hotkeySSFree(m->read_ss);
    if (m->write_ss) hotkeySSFree(m->write_ss);
    zfree(m);
}

void hotkeyManagerReset(hotkeyManager *m) {
    if (!m) return;
    if (m->read_ss) hotkeySSReset(m->read_ss);
    if (m->write_ss) hotkeySSReset(m->write_ss);
    m->last_decay_us = getMonotonicUs();
}

/* Lazy, time-anchored exponential decay of both summaries.
 *
 * Advances (count, error) to `now_us` by multiplying by f = exp(-lambda *
 * elapsed_seconds), at most once per tick so the O(K) sweep is amortized and
 * immune to cron stalls. Faded entries are pruned. Multiplying every entry by
 * the same factor preserves the min-heap order. */
static void hotkeyManagerDecayToNow(hotkeyManager *m, uint64_t now_us) {
    uint64_t dt;
    double f, lambda;

    if (!m || server.hotkey_half_life_seconds <= 0) return;
    if (now_us <= m->last_decay_us) return; /* monotonic guard */
    dt = now_us - m->last_decay_us;
    if (dt < HOTKEY_DECAY_TICK_US) return; /* amortize */

    lambda = HOTKEY_LN2 / (double)server.hotkey_half_life_seconds;
    f = exp(-lambda * ((double)dt / 1e6));
    hotkeySSDecayAndPrune(m->read_ss, f);
    hotkeySSDecayAndPrune(m->write_ss, f);
    m->last_decay_us = now_us;
}

void hotkeyPurgeAll(void) {
    hotkeyManagerReset(server.hotkey_manager);
}

/* ===========================================================================
 * Per-access detection hooks
 * ==========================================================================*/

void readHotKeyDetection(robj *key, int slot, int dbid) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    hotkeyManagerDecayToNow(m, getMonotonicUs());
    hotkeySSAdd(m->read_ss, key, dbid, slot);
}

void writeHotKeyDetection(robj *key, int slot, int dbid) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    hotkeyManagerDecayToNow(m, getMonotonicUs());
    hotkeySSAdd(m->write_ss, key, dbid, slot);
}

/* ===========================================================================
 * HOTKEYS commands
 * ==========================================================================*/

typedef struct {
    sds key;
    uint64_t qps;
    int is_read;
    int dbid;
    int slot;
} hotkeyCollected;

static int hotkeyCollectedCmpDesc(const void *a, const void *b) {
    const hotkeyCollected *ea = a;
    const hotkeyCollected *eb = b;
    if (eb->qps > ea->qps) return 1;
    if (eb->qps < ea->qps) return -1;
    return 0;
}

/* Recover access rate from a decayed Space-Saving entry using the midpoint of
 * the [count-error, count] band:
 *   qps = (count - error/2) * lambda * (100 / sampling_ratio)
 * where lambda = ln2 / half_life. A key arriving at a steady sampled rate r
 * settles at count = r/lambda, so this returns the true rate. */
static uint64_t hotkeyEstimateQPS(double count, double error) {
    if (server.hotkey_sampling_ratio <= 0 || server.hotkey_half_life_seconds <= 0) return 0;
    double midpoint = count - error / 2.0;
    if (midpoint <= 0.0) return 0;
    double lambda = HOTKEY_LN2 / (double)server.hotkey_half_life_seconds;
    double scale = 100.0 / (double)server.hotkey_sampling_ratio;
    return (uint64_t)(midpoint * lambda * scale + 0.5);
}

static int hotkeyCollectFromSS(hotkeySS *ss, hotkeyCollected *arr, int n, int is_read) {
    if (!ss) return n;
    for (int j = 0; j < ss->size; j++) {
        if (!ss->entries[j].key) continue;
        arr[n].key = ss->entries[j].key;
        arr[n].qps = hotkeyEstimateQPS(ss->entries[j].count, ss->entries[j].error);
        arr[n].is_read = is_read;
        arr[n].dbid = ss->entries[j].dbid;
        arr[n].slot = ss->entries[j].slot;
        n++;
    }
    return n;
}

void hotkeysGetCommand(client *c) {
    if (!server.hotkey_enabled) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }
    hotkeyManager *m = server.hotkey_manager;
    if (!m) {
        addReplyArrayLen(c, 0);
        return;
    }

    /* Advance decay so the reported counts reflect the current instant. */
    hotkeyManagerDecayToNow(m, getMonotonicUs());

    int filter_type = -1;
    int i = 2;
    while (i < c->argc) {
        if (!strcasecmp(objectGetVal(c->argv[i]), "TYPE") && i + 1 < c->argc) {
            char *t = objectGetVal(c->argv[i + 1]);
            if (!strcasecmp(t, "read"))
                filter_type = 1;
            else if (!strcasecmp(t, "write"))
                filter_type = 0;
            else if (!strcasecmp(t, "all"))
                filter_type = -1;
            else {
                addReplyError(c, "Invalid type. Use 'read', 'write', or 'all'");
                return;
            }
            i += 2;
        } else {
            addReplyError(c, "Syntax error. Usage: HOTKEYS GET [TYPE {read|write|all}]");
            return;
        }
    }

    int count = 0;
    if (filter_type != 0 && m->read_ss) count += m->read_ss->size;
    if (filter_type != 1 && m->write_ss) count += m->write_ss->size;

    if (count == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    hotkeyCollected *arr = zmalloc(count * sizeof(hotkeyCollected));
    int n = 0;
    if (filter_type != 0) n = hotkeyCollectFromSS(m->read_ss, arr, n, 1);
    if (filter_type != 1) n = hotkeyCollectFromSS(m->write_ss, arr, n, 0);

    qsort(arr, n, sizeof(hotkeyCollected), hotkeyCollectedCmpDesc);

    int limit = n < server.hotkey_top_k ? n : server.hotkey_top_k;
    addReplyArrayLen(c, limit);
    for (int j = 0; j < limit; j++) {
        addReplyArrayLen(c, 10);
        addReplyBulkCString(c, "key");
        addReplyBulkCBuffer(c, arr[j].key, sdslen(arr[j].key));
        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, arr[j].is_read ? "read" : "write");
        addReplyBulkCString(c, "db");
        addReplyLongLong(c, arr[j].dbid);
        addReplyBulkCString(c, "slot");
        addReplyLongLong(c, arr[j].slot);
        addReplyBulkCString(c, "qps");
        addReplyLongLong(c, arr[j].qps);
    }
    zfree(arr);
}

void hotkeysResetCommand(client *c) {
    if (!server.hotkey_enabled) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }
    if (server.hotkey_manager)
        hotkeyManagerReset(server.hotkey_manager);
    addReply(c, shared.ok);
}

void hotkeysCommand(client *c) {
    if (c->argc < 2) {
        addReplyError(c, "Wrong number of arguments for 'HOTKEYS' command");
        return;
    }
    char *subcmd = objectGetVal(c->argv[1]);
    if (!strcasecmp(subcmd, "get"))
        hotkeysGetCommand(c);
    else if (!strcasecmp(subcmd, "reset"))
        hotkeysResetCommand(c);
    else
        addReplyErrorFormat(c, "Unknown HOTKEYS subcommand '%s'", subcmd);
}

/* ===========================================================================
 * Config callbacks
 * ==========================================================================*/

int hotKeyEnabledCallback(const char **err) {
    UNUSED(err);
    if (server.hotkey_enabled) {
        if (!server.hotkey_manager)
            server.hotkey_manager = hotkeyManagerInit(server.hotkey_top_k);
    } else {
        if (server.hotkey_manager) {
            hotkeyManagerFree(server.hotkey_manager);
            server.hotkey_manager = NULL;
        }
    }
    return 1;
}

int hotKeyTopKCallback(const char **err) {
    UNUSED(err);
    if (!server.hotkey_enabled) return 1;
    /* Recreate the manager with the new K. */
    if (server.hotkey_manager) {
        hotkeyManagerFree(server.hotkey_manager);
        server.hotkey_manager = NULL;
    }
    server.hotkey_manager = hotkeyManagerInit(server.hotkey_top_k);
    return 1;
}
