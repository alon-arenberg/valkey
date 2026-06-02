#include "server.h"

/* ===========================================================================
 * Count-Min Sketch
 * ==========================================================================*/

uint32_t murmurHash2(const void *key, int len, uint32_t seed) {
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = seed ^ len;
    const unsigned char *data = (const unsigned char *)key;

    while (len >= 4) {
        uint32_t k = *(uint32_t *)data;
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
        data += 4;
        len -= 4;
    }
    switch (len) {
    case 3: h ^= data[2] << 16; /* fallthrough */
    case 2: h ^= data[1] << 8; /* fallthrough */
    case 1:
        h ^= data[0];
        h *= m;
    };
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

static size_t nextPowerOfTwo(size_t n) {
    if (n == 0) return 1;
    if ((n & (n - 1)) == 0) return n;
    size_t power = 1;
    while (power < n) power <<= 1;
    return power;
}

hotkeyCMS *newHotkeyCMS(size_t width, size_t depth) {
    serverAssert(width > 0 && depth > 0);
    hotkeyCMS *cms = zcalloc(sizeof(hotkeyCMS));
    size_t adjusted_width = nextPowerOfTwo(width);
    cms->width = adjusted_width;
    cms->depth = depth;
    cms->counter = 0;
    cms->width_mask = adjusted_width - 1;
    cms->array = zcalloc(adjusted_width * depth * sizeof(uint32_t));
    return cms;
}

void freeHotkeyCMS(hotkeyCMS *cms) {
    if (!cms) return;
    if (cms->array) zfree(cms->array);
    zfree(cms);
}

size_t hotkeyCMSUpdate(hotkeyCMS *cms, robj *key) {
    if (!cms || !key || !objectGetVal(key)) return 0;
    size_t len = stringObjectLen(key);
    if (len == 0) return 0;

    size_t minCount = (size_t)-1;
    for (size_t i = 0; i < cms->depth; ++i) {
        uint32_t hash = murmurHash2(objectGetVal(key), len, i);
        size_t loc = (hash & cms->width_mask) + (i * cms->width);
        cms->array[loc]++;
        minCount = min(minCount, cms->array[loc]);
    }
    cms->counter++;
    return minCount;
}

void hotkeyCMSReset(hotkeyCMS *cms) {
    if (!cms || !cms->array) return;
    memset(cms->array, 0, (size_t)cms->width * cms->depth * sizeof(uint32_t));
    cms->counter = 0;
}

/* ===========================================================================
 * Top-K min-heap tracker
 *
 * Min-ordered by count: root has the smallest count. When full and a new key
 * has a higher count than the root, evict the root. Linear scan for key
 * lookup (K is small), heap sift for structural maintenance.
 * ==========================================================================*/

static hotkeyTopK *hotkeyTopKNew(int capacity) {
    hotkeyTopK *tk = zcalloc(sizeof(hotkeyTopK));
    tk->entries = zcalloc(capacity * sizeof(hotkeyTopKEntry));
    tk->capacity = capacity;
    tk->size = 0;
    return tk;
}

static void hotkeyTopKFree(hotkeyTopK *tk) {
    if (!tk) return;
    for (int i = 0; i < tk->size; i++) {
        if (tk->entries[i].key) sdsfree(tk->entries[i].key);
    }
    zfree(tk->entries);
    zfree(tk);
}

static void hotkeyTopKReset(hotkeyTopK *tk) {
    if (!tk) return;
    for (int i = 0; i < tk->size; i++) {
        if (tk->entries[i].key) sdsfree(tk->entries[i].key);
        tk->entries[i].key = NULL;
        tk->entries[i].count = 0;
    }
    tk->size = 0;
}

static void heapSwap(hotkeyTopK *tk, int a, int b) {
    hotkeyTopKEntry tmp = tk->entries[a];
    tk->entries[a] = tk->entries[b];
    tk->entries[b] = tmp;
}

static void heapSiftUp(hotkeyTopK *tk, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (tk->entries[idx].count < tk->entries[parent].count) {
            heapSwap(tk, idx, parent);
            idx = parent;
        } else {
            break;
        }
    }
}

static void heapSiftDown(hotkeyTopK *tk, int idx) {
    while (1) {
        int smallest = idx;
        int left = 2 * idx + 1;
        int right = 2 * idx + 2;
        if (left < tk->size && tk->entries[left].count < tk->entries[smallest].count)
            smallest = left;
        if (right < tk->size && tk->entries[right].count < tk->entries[smallest].count)
            smallest = right;
        if (smallest == idx) break;
        heapSwap(tk, idx, smallest);
        idx = smallest;
    }
}

/* Find (key, dbid) index in heap by linear scan. Returns -1 if not found. */
static int hotkeyTopKFind(hotkeyTopK *tk, const char *k, size_t klen, int dbid) {
    for (int i = 0; i < tk->size; i++) {
        if (tk->entries[i].key &&
            tk->entries[i].dbid == dbid &&
            sdslen(tk->entries[i].key) == klen &&
            memcmp(tk->entries[i].key, k, klen) == 0) {
            return i;
        }
    }
    return -1;
}

static void hotkeyTopKAdd(hotkeyTopK *tk, robj *key, uint64_t count, int dbid, int slot) {
    sds k = objectGetVal(key);
    size_t klen = sdslen(k);

    /* (Key, dbid) already in heap — update count and fix heap order */
    int idx = hotkeyTopKFind(tk, k, klen, dbid);
    if (idx >= 0) {
        tk->entries[idx].count = count;
        tk->entries[idx].dbid = dbid;
        tk->entries[idx].slot = slot;
        heapSiftDown(tk, idx);
        return;
    }

    /* Room available — insert at end and sift up */
    if (tk->size < tk->capacity) {
        idx = tk->size;
        tk->entries[idx].key = sdsnewlen(k, klen);
        tk->entries[idx].count = count;
        tk->entries[idx].dbid = dbid;
        tk->entries[idx].slot = slot;
        tk->size++;
        heapSiftUp(tk, idx);
        return;
    }

    /* Full — evict root if new count is larger */
    if (count > tk->entries[0].count) {
        sdsfree(tk->entries[0].key);
        tk->entries[0].key = sdsnewlen(k, klen);
        tk->entries[0].count = count;
        tk->entries[0].dbid = dbid;
        tk->entries[0].slot = slot;
        heapSiftDown(tk, 0);
    }
}

/* ===========================================================================
 * Hotkey manager lifecycle
 * ==========================================================================*/

hotkeyManager *hotkeyManagerInit(size_t cms_width, size_t cms_depth) {
    hotkeyManager *m = zcalloc(sizeof(hotkeyManager));
    m->read_cms = newHotkeyCMS(cms_width, cms_depth);
    m->read_topk = hotkeyTopKNew(server.hotkey_top_k);
    m->write_cms = newHotkeyCMS(cms_width, cms_depth);
    m->write_topk = hotkeyTopKNew(server.hotkey_top_k);
    return m;
}

void hotkeyManagerFree(hotkeyManager *m) {
    if (!m) return;
    if (m->read_cms) freeHotkeyCMS(m->read_cms);
    if (m->read_topk) hotkeyTopKFree(m->read_topk);
    if (m->write_cms) freeHotkeyCMS(m->write_cms);
    if (m->write_topk) hotkeyTopKFree(m->write_topk);
    zfree(m);
}

void hotkeyManagerReset(hotkeyManager *m) {
    if (!m) return;
    if (m->read_cms) hotkeyCMSReset(m->read_cms);
    if (m->read_topk) hotkeyTopKReset(m->read_topk);
    if (m->write_cms) hotkeyCMSReset(m->write_cms);
    if (m->write_topk) hotkeyTopKReset(m->write_topk);
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

    size_t count = hotkeyCMSUpdate(m->read_cms, key);
    hotkeyTopKAdd(m->read_topk, key, count, dbid, slot);
}

void writeHotKeyDetection(robj *key, int slot, int dbid) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;

    size_t count = hotkeyCMSUpdate(m->write_cms, key);
    hotkeyTopKAdd(m->write_topk, key, count, dbid, slot);
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

static int hotkeyCollectFromTopK(hotkeyTopK *tk, hotkeyCollected *arr, int n, int is_read) {
    if (!tk) return n;
    for (int j = 0; j < tk->size; j++) {
        if (!tk->entries[j].key) continue;
        arr[n].key = tk->entries[j].key;
        arr[n].qps = hotkeyEstimateQPS(tk->entries[j].count);
        arr[n].is_read = is_read;
        arr[n].dbid = tk->entries[j].dbid;
        arr[n].slot = tk->entries[j].slot;
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

    /* Count total entries to allocate */
    int count = 0;
    if (filter_type != 0 && m->read_topk) count += m->read_topk->size;
    if (filter_type != 1 && m->write_topk) count += m->write_topk->size;

    if (count == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    hotkeyCollected *arr = zmalloc(count * sizeof(hotkeyCollected));
    int n = 0;
    if (filter_type != 0) n = hotkeyCollectFromTopK(m->read_topk, arr, n, 1);
    if (filter_type != 1) n = hotkeyCollectFromTopK(m->write_topk, arr, n, 0);

    qsort(arr, n, sizeof(hotkeyCollected), hotkeyCollectedCmpDesc);

    /* Cap output at top_k */
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
            server.hotkey_manager = hotkeyManagerInit(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    } else {
        if (server.hotkey_manager) {
            hotkeyManagerFree(server.hotkey_manager);
            server.hotkey_manager = NULL;
        }
    }
    return 1;
}

int hotKeyCMSBucketSizeCallback(const char **err) {
    UNUSED(err);
    if (!server.hotkey_enabled) return 1;
    if (server.hotkey_manager) {
        hotkeyManagerFree(server.hotkey_manager);
        server.hotkey_manager = NULL;
    }
    server.hotkey_manager = hotkeyManagerInit(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    return 1;
}

int hotKeyCMSDepthCallback(const char **err) {
    UNUSED(err);
    if (!server.hotkey_enabled) return 1;
    if (server.hotkey_manager) {
        hotkeyManagerFree(server.hotkey_manager);
        server.hotkey_manager = NULL;
    }
    server.hotkey_manager = hotkeyManagerInit(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    return 1;
}
