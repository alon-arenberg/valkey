#include "server.h"

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
 * The structure is organized as a min-heap by `count` so the eviction
 * candidate (root) is found in O(1). Membership check is a linear scan
 * since K is small (default 16).
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
        ss->entries[i].count = 0;
        ss->entries[i].error = 0;
    }
    ss->size = 0;
}

static void heapSwap(hotkeySS *ss, int a, int b) {
    hotkeySSEntry tmp = ss->entries[a];
    ss->entries[a] = ss->entries[b];
    ss->entries[b] = tmp;
}

static void heapSiftUp(hotkeySS *ss, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (ss->entries[idx].count < ss->entries[parent].count) {
            heapSwap(ss, idx, parent);
            idx = parent;
        } else {
            break;
        }
    }
}

static void heapSiftDown(hotkeySS *ss, int idx) {
    while (1) {
        int smallest = idx;
        int left = 2 * idx + 1;
        int right = 2 * idx + 2;
        if (left < ss->size && ss->entries[left].count < ss->entries[smallest].count)
            smallest = left;
        if (right < ss->size && ss->entries[right].count < ss->entries[smallest].count)
            smallest = right;
        if (smallest == idx) break;
        heapSwap(ss, idx, smallest);
        idx = smallest;
    }
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

/* Add a single observation of `key` to the summary. */
static void hotkeySSAdd(hotkeySS *ss, robj *key, int dbid, int slot) {
    sds k = objectGetVal(key);
    size_t klen = sdslen(k);

    /* Case 1: key already tracked — increment its count. */
    int idx = hotkeySSFind(ss, k, klen, dbid);
    if (idx >= 0) {
        ss->entries[idx].count++;
        ss->entries[idx].slot = slot;
        heapSiftDown(ss, idx);
        return;
    }

    /* Case 2: room available — insert with count=1, error=0. */
    if (ss->size < ss->capacity) {
        idx = ss->size;
        ss->entries[idx].key = sdsnewlen(k, klen);
        ss->entries[idx].count = 1;
        ss->entries[idx].error = 0;
        ss->entries[idx].dbid = dbid;
        ss->entries[idx].slot = slot;
        ss->size++;
        heapSiftUp(ss, idx);
        return;
    }

    /* Case 3: full — replace min entry (root). New entry inherits the evicted
     * count + 1, and `error` records the maximum overestimate. */
    uint64_t min_count = ss->entries[0].count;
    sdsfree(ss->entries[0].key);
    ss->entries[0].key = sdsnewlen(k, klen);
    ss->entries[0].count = min_count + 1;
    ss->entries[0].error = min_count;
    ss->entries[0].dbid = dbid;
    ss->entries[0].slot = slot;
    heapSiftDown(ss, 0);
}

/* ===========================================================================
 * Hotkey manager lifecycle
 * ==========================================================================*/

hotkeyManager *hotkeyManagerInit(int top_k) {
    hotkeyManager *m = zcalloc(sizeof(hotkeyManager));
    m->read_ss = hotkeySSNew(top_k);
    m->write_ss = hotkeySSNew(top_k);
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
    hotkeySSAdd(m->read_ss, key, dbid, slot);
}

void writeHotKeyDetection(robj *key, int slot, int dbid) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
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

/* Extrapolate QPS from sampled count:
 * qps = count * (100 / sampling_ratio) / window_seconds */
static uint64_t hotkeyEstimateQPS(uint64_t count) {
    if (server.hotkey_sampling_ratio <= 0 || server.hotkey_window_seconds <= 0) return 0;
    return (count * 100 / server.hotkey_sampling_ratio) / server.hotkey_window_seconds;
}

static int hotkeyCollectFromSS(hotkeySS *ss, hotkeyCollected *arr, int n, int is_read) {
    if (!ss) return n;
    for (int j = 0; j < ss->size; j++) {
        if (!ss->entries[j].key) continue;
        arr[n].key = ss->entries[j].key;
        arr[n].qps = hotkeyEstimateQPS(ss->entries[j].count);
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
