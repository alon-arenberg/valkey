#include "server.h"

/* ---------------------------------------------------------------------------
 * Misra-Gries hot key detection
 *
 * Uses a fixed-size array of k entries per slot. For small k (default 16),
 * linear scan is faster than dict hash+pointer-chase and the entire working
 * set fits in L1 cache (~1KB).
 *
 * Theoretical guarantee: after N observations, any key with true frequency
 * f > N/(k+1) is guaranteed to be in the summary (no false negatives for
 * true heavy hitters).
 * --------------------------------------------------------------------------*/

/* ===========================================================================
 * Command argument parsing helpers
 * ==========================================================================*/

/* Parse [TYPE {read|write|all}] argument.
 * Returns 1 on success, 0 on error (error reply already sent). */
static int parseHotkeyFilterArgs(client *c, int start_idx, int *filter_type) {
    int i = start_idx;

    *filter_type = -1;

    while (i < c->argc) {
        if (!strcasecmp(objectGetVal(c->argv[i]), "TYPE") && i + 1 < c->argc) {
            char *t = objectGetVal(c->argv[i + 1]);
            if (!strcasecmp(t, "read"))
                *filter_type = 1;
            else if (!strcasecmp(t, "write"))
                *filter_type = 0;
            else if (!strcasecmp(t, "all"))
                *filter_type = -1;
            else {
                addReplyError(c, "Invalid type. Use 'read', 'write', or 'all'");
                return 0;
            }
            i += 2;
        } else {
            addReplyError(c, "Syntax error. Use [TYPE {read|write|all}]");
            return 0;
        }
    }
    return 1;
}


/* ===========================================================================
 * Misra-Gries summary operations
 * ==========================================================================*/

hotkeyMGSummary *hotkeyMGSummaryNew(int max_keys) {
    hotkeyMGSummary *s = zcalloc(sizeof(hotkeyMGSummary));
    s->keys = zcalloc(max_keys * sizeof(sds));
    s->key_lens = zcalloc(max_keys * sizeof(uint32_t));
    s->counters = zcalloc(max_keys * sizeof(uint64_t));
    s->decrements = zcalloc(max_keys * sizeof(uint64_t));
    s->dbs = zcalloc(max_keys * sizeof(int));
    s->slots = zcalloc(max_keys * sizeof(int));
    s->max_keys = max_keys;
    s->size = 0;
    s->total = 0;
    return s;
}

void hotkeyMGSummaryFree(hotkeyMGSummary *s) {
    int i;

    if (!s) return;
    for (i = 0; i < s->size; i++) {
        if (s->keys[i]) sdsfree(s->keys[i]);
    }
    zfree(s->keys);
    zfree(s->key_lens);
    zfree(s->counters);
    zfree(s->decrements);
    zfree(s->dbs);
    zfree(s->slots);
    zfree(s);
}

static void hotkeyMGSummaryReset(hotkeyMGSummary *s) {
    int i;

    if (!s) return;
    for (i = 0; i < s->size; i++) {
        if (s->keys[i]) sdsfree(s->keys[i]);
        s->keys[i] = NULL;
        s->key_lens[i] = 0;
        s->counters[i] = 0;
        s->decrements[i] = 0;
        s->dbs[i] = 0;
        s->slots[i] = 0;
    }
    s->size = 0;
    s->total = 0;
}

/* Add a key observation to the Misra-Gries summary.
 *
 * Three cases:
 * 1. Key already tracked: increment its counter.
 * 2. Empty slot available: insert new entry.
 * 3. No empty slot: increment decrements for all, evict any where
 *    counters[i] - decrements[i] == 0. */
static void hotkeyMGSummaryAdd(hotkeyMGSummary *s, robj *key, int dbid, int slot) {
    const char *k;
    uint32_t klen;
    int i, empty_idx;

    if (!s || !key || !objectGetVal(key)) return;

    s->total++;
    k = objectGetVal(key);
    klen = (uint32_t)sdslen(objectGetVal(key));

    /* Prefetch only keys that pass the fast-reject (matching length + db)
     * so we don't waste cache lines on entries we'll skip. */
    for (i = 0; i < s->size; i++) {
        if (s->keys[i] && s->key_lens[i] == klen && s->dbs[i] == dbid)
            valkey_prefetch(s->keys[i]);
    }

    /* Case 1: key already tracked — increment its access counter.
     * Uses cached key_lens for fast-reject to avoid unnecessary memcmp. */
    empty_idx = -1;
    for (i = 0; i < s->size; i++) {
        if (!s->keys[i]) {
            if (empty_idx == -1) empty_idx = i;
            continue;
        }
        /* Fast-reject: check cached length and db before memcmp. */
        if (s->key_lens[i] != klen || s->dbs[i] != dbid) continue;
        if (memcmp(s->keys[i], k, klen) == 0) {
            s->counters[i]++;
            return;
        }
    }

    /* Case 2: empty slot available. */
    if (s->size < s->max_keys) {
        s->keys[s->size] = sdsnewlen(k, klen);
        s->key_lens[s->size] = klen;
        s->counters[s->size] = 1;
        s->decrements[s->size] = 0;
        s->dbs[s->size] = dbid;
        s->slots[s->size] = slot;
        s->size++;
        return;
    }
    if (empty_idx != -1) {
        s->keys[empty_idx] = sdsnewlen(k, klen);
        s->key_lens[empty_idx] = klen;
        s->counters[empty_idx] = 1;
        s->decrements[empty_idx] = 0;
        s->dbs[empty_idx] = dbid;
        s->slots[empty_idx] = slot;
        return;
    }

    /* Case 3: no empty slot — increment decrements, evict zeros. */
    for (i = 0; i < s->size; i++) {
        s->decrements[i]++;
        if (s->counters[i] - s->decrements[i] == 0) {
            sdsfree(s->keys[i]);
            s->keys[i] = NULL;
        }
    }
}

/* ===========================================================================
 * Hotkey manager lifecycle
 * ==========================================================================*/

hotkeyManager *hotkeyManagerInit(int max_keys) {
    hotkeyManager *m = zcalloc(sizeof(hotkeyManager));
    m->read_summary = hotkeyMGSummaryNew(max_keys);
    m->write_summary = hotkeyMGSummaryNew(max_keys);
    return m;
}

void hotkeyManagerFree(hotkeyManager *m) {
    if (!m) return;
    if (m->read_summary) hotkeyMGSummaryFree(m->read_summary);
    if (m->write_summary) hotkeyMGSummaryFree(m->write_summary);
    zfree(m);
}

void hotkeyManagerReset(hotkeyManager *m) {
    if (!m) return;
    if (m->read_summary) hotkeyMGSummaryReset(m->read_summary);
    if (m->write_summary) hotkeyMGSummaryReset(m->write_summary);
}

/* ===========================================================================
 * Per-access detection hooks (called from lookupKey)
 * ==========================================================================*/

void readHotKeyDetection(robj *key, int slot, int dbid) {
    hotkeyManager *m = server.hotkey_manager;

    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    hotkeyMGSummaryAdd(m->read_summary, key, dbid, slot);
}

void writeHotKeyDetection(robj *key, int slot, int dbid) {
    hotkeyManager *m = server.hotkey_manager;

    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    hotkeyMGSummaryAdd(m->write_summary, key, dbid, slot);
}


/* ===========================================================================
 * HOTKEYS commands
 * ==========================================================================*/

/* Entry used for collecting and sorting active MG slots. */
typedef struct {
    sds key;
    uint64_t qps;
    int slot;
    int is_read;
    int dbid;
} hotkeyMGCollected;

static int hotkeyMGCollectedCmp(const void *a, const void *b) {
    const hotkeyMGCollected *ea = a;
    const hotkeyMGCollected *eb = b;
    if (eb->qps > ea->qps) return 1;
    if (eb->qps < ea->qps) return -1;
    return 0;
}

/* Extrapolate QPS from sampled count:
 * qps = count * (100 / sampling_ratio) / window_seconds */
static uint64_t hotkeyEstimateQPS(uint64_t count) {
    if (server.hotkey_sampling_ratio <= 0 || server.hotkey_window_seconds <= 0) return 0;
    return (count * 100 / server.hotkey_sampling_ratio) / server.hotkey_window_seconds;
}

static int hotkeyCountActive(hotkeyMGSummary *s) {
    int count = 0, i;
    if (!s) return 0;
    for (i = 0; i < s->size; i++) {
        if (s->keys[i]) count++;
    }
    return count;
}

static int hotkeyCollectEntries(hotkeyMGSummary *s, hotkeyMGCollected *arr, int n, int is_read) {
    int i;
    if (!s) return n;
    for (i = 0; i < s->size; i++) {
        if (!s->keys[i]) continue;
        arr[n].key = s->keys[i];
        arr[n].qps = hotkeyEstimateQPS(s->counters[i]);
        arr[n].slot = s->slots[i];
        arr[n].is_read = is_read;
        arr[n].dbid = s->dbs[i];
        n++;
    }
    return n;
}

void hotkeysGetCommand(client *c) {
    int filter_type, i, n, limit;
    hotkeyManager *m;
    hotkeyMGCollected *collected;
    int count;

    if (!server.hotkey_enabled) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }
    m = server.hotkey_manager;
    if (!m) {
        addReplyArrayLen(c, 0);
        return;
    }
    if (!parseHotkeyFilterArgs(c, 2, &filter_type)) return;

    count = 0;
    if (filter_type == -1 || filter_type == 1)
        count += hotkeyCountActive(m->read_summary);
    if (filter_type == -1 || filter_type == 0)
        count += hotkeyCountActive(m->write_summary);

    if (count == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    collected = zmalloc(count * sizeof(hotkeyMGCollected));
    n = 0;
    if (filter_type == -1 || filter_type == 1)
        n = hotkeyCollectEntries(m->read_summary, collected, n, 1);
    if (filter_type == -1 || filter_type == 0)
        n = hotkeyCollectEntries(m->write_summary, collected, n, 0);

    if (n > 0) qsort(collected, n, sizeof(hotkeyMGCollected), hotkeyMGCollectedCmp);

    limit = n < server.hotkey_max_keys ? n : server.hotkey_max_keys;
    addReplyArrayLen(c, limit);
    for (i = 0; i < limit; i++) {
        addReplyArrayLen(c, 10);
        addReplyBulkCString(c, "key");
        addReplyBulkCBuffer(c, collected[i].key, sdslen(collected[i].key));
        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, collected[i].is_read ? "read" : "write");
        addReplyBulkCString(c, "db");
        addReplyLongLong(c, collected[i].dbid);
        addReplyBulkCString(c, "slot");
        addReplyLongLong(c, collected[i].slot);
        addReplyBulkCString(c, "qps");
        addReplyLongLong(c, collected[i].qps);
    }
    zfree(collected);
}

void hotkeysResetCommand(client *c) {
    if (!server.hotkey_enabled) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }
    if (server.hotkey_manager) {
        hotkeyManagerReset(server.hotkey_manager);
    }
    addReply(c, shared.ok);
}

/* HOTKEYS command dispatcher. */
void hotkeysCommand(client *c) {
    char *subcmd;

    if (c->argc < 2) {
        addReplyError(c, "Wrong number of arguments for 'HOTKEYS' command");
        return;
    }
    subcmd = objectGetVal(c->argv[1]);
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
            server.hotkey_manager = hotkeyManagerInit(server.hotkey_max_keys);
    } else {
        if (server.hotkey_manager) {
            hotkeyManagerFree(server.hotkey_manager);
            server.hotkey_manager = NULL;
        }
    }
    return 1;
}

int hotKeyMaxKeysCallback(const char **err) {
    UNUSED(err);
    if (!server.hotkey_enabled) return 1;
    if (server.hotkey_manager) {
        hotkeyManagerFree(server.hotkey_manager);
        server.hotkey_manager = NULL;
    }
    server.hotkey_manager = hotkeyManagerInit(server.hotkey_max_keys);
    return 1;
}

/* ===========================================================================
 * Invalidation (slot purge, flush, key deletion)
 * ==========================================================================*/

/* Invalidate entries matching a specific slot in a summary. */
static void hotkeyMGSummaryInvalidateSlot(hotkeyMGSummary *s, int slot) {
    int i;
    if (!s) return;
    for (i = 0; i < s->size; i++) {
        if (s->keys[i] && s->slots[i] == slot) {
            sdsfree(s->keys[i]);
            s->keys[i] = NULL;
        }
    }
}

/* Invalidate a specific key+db combination in a summary. */
static void hotkeyMGSummaryInvalidateKey(hotkeyMGSummary *s, const char *k, size_t klen, int dbid) {
    int i;
    if (!s) return;
    for (i = 0; i < s->size; i++) {
        if (s->keys[i] && s->dbs[i] == dbid &&
            sdslen(s->keys[i]) == klen &&
            memcmp(s->keys[i], k, klen) == 0) {
            sdsfree(s->keys[i]);
            s->keys[i] = NULL;
            return;
        }
    }
}

void hotkeyPurgeSlot(int slot) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m) return;
    hotkeyMGSummaryInvalidateSlot(m->read_summary, slot);
    hotkeyMGSummaryInvalidateSlot(m->write_summary, slot);
}

void hotkeyPurgeAll(void) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m) return;
    hotkeyMGSummaryReset(m->read_summary);
    hotkeyMGSummaryReset(m->write_summary);
}

static void hotkeyMGSummaryInvalidateDb(hotkeyMGSummary *s, int dbid) {
    int i;
    if (!s) return;
    for (i = 0; i < s->size; i++) {
        if (s->keys[i] && s->dbs[i] == dbid) {
            sdsfree(s->keys[i]);
            s->keys[i] = NULL;
        }
    }
}

void hotkeyPurgeDb(int dbid) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m) return;
    hotkeyMGSummaryInvalidateDb(m->read_summary, dbid);
    hotkeyMGSummaryInvalidateDb(m->write_summary, dbid);
}

void hotkeyInvalidateKey(robj *key, int dbid) {
    hotkeyManager *m = server.hotkey_manager;
    const char *k;
    size_t klen;

    if (!m || !key || !objectGetVal(key)) return;
    k = objectGetVal(key);
    klen = sdslen(objectGetVal(key));
    hotkeyMGSummaryInvalidateKey(m->read_summary, k, klen, dbid);
    hotkeyMGSummaryInvalidateKey(m->write_summary, k, klen, dbid);
}

