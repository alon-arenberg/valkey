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
 * Top-K tracker (flat array, linear scan)
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

/* Insert or update a key in the top-K using linear scan.
 * If key exists: update count.
 * If room: append.
 * If full: evict the entry with the smallest count if new count is larger. */
static void hotkeyTopKAdd(hotkeyTopK *tk, robj *key, uint64_t count, int val_type) {
    const char *k = objectGetVal(key);
    size_t klen = sdslen(objectGetVal(key));

    /* Linear scan: check if key already tracked */
    for (int i = 0; i < tk->size; i++) {
        if (tk->entries[i].key &&
            sdslen(tk->entries[i].key) == klen &&
            memcmp(tk->entries[i].key, k, klen) == 0) {
            tk->entries[i].count = count;
            tk->entries[i].val_type = val_type;
            return;
        }
    }

    /* Room available */
    if (tk->size < tk->capacity) {
        tk->entries[tk->size].key = sdsnewlen(k, klen);
        tk->entries[tk->size].count = count;
        tk->entries[tk->size].val_type = val_type;
        tk->size++;
        return;
    }

    /* Full: find minimum entry */
    int min_idx = 0;
    for (int i = 1; i < tk->size; i++) {
        if (tk->entries[i].count < tk->entries[min_idx].count)
            min_idx = i;
    }

    /* Evict if new count is larger than the minimum */
    if (count > tk->entries[min_idx].count) {
        sdsfree(tk->entries[min_idx].key);
        tk->entries[min_idx].key = sdsnewlen(k, klen);
        tk->entries[min_idx].count = count;
        tk->entries[min_idx].val_type = val_type;
    }
}

/* ===========================================================================
 * Per-slot state management
 * ==========================================================================*/

static hotkeySlotState *hotkeySlotStateNew(void) {
    hotkeySlotState *s = zcalloc(sizeof(hotkeySlotState));
    s->read_cms = newHotkeyCMS(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    s->read_topk = hotkeyTopKNew(server.hotkey_top_k);
    s->write_cms = newHotkeyCMS(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    s->write_topk = hotkeyTopKNew(server.hotkey_top_k);
    return s;
}

static void hotkeySlotStateFree(hotkeySlotState *s) {
    if (!s) return;
    if (s->read_cms) freeHotkeyCMS(s->read_cms);
    if (s->read_topk) hotkeyTopKFree(s->read_topk);
    if (s->write_cms) freeHotkeyCMS(s->write_cms);
    if (s->write_topk) hotkeyTopKFree(s->write_topk);
    zfree(s);
}

static void hotkeySlotStateReset(hotkeySlotState *s) {
    if (!s) return;
    if (s->read_cms) hotkeyCMSReset(s->read_cms);
    if (s->read_topk) hotkeyTopKReset(s->read_topk);
    if (s->write_cms) hotkeyCMSReset(s->write_cms);
    if (s->write_topk) hotkeyTopKReset(s->write_topk);
}

/* ===========================================================================
 * Hotkey manager lifecycle
 * ==========================================================================*/

hotkeyManager *hotkeyManagerInit(size_t cms_width, size_t cms_depth) {
    UNUSED(cms_width);
    UNUSED(cms_depth);
    hotkeyManager *m = zcalloc(sizeof(hotkeyManager));
    return m;
}

void hotkeyManagerFree(hotkeyManager *m) {
    if (!m) return;
    for (int i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->slots[i]) hotkeySlotStateFree(m->slots[i]);
    }
    zfree(m);
}

void hotkeyManagerReset(hotkeyManager *m) {
    if (!m) return;
    for (int i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->slots[i]) hotkeySlotStateReset(m->slots[i]);
    }
}

/* ===========================================================================
 * Per-access detection hooks
 * ==========================================================================*/

void readHotKeyDetection(robj *key, int val_type, int slot) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    if (slot < 0 || slot >= HOTKEY_SLOTS) return;
    server.hotkey_runtime_total_sampled++;

    if (!m->slots[slot])
        m->slots[slot] = hotkeySlotStateNew();

    size_t count = hotkeyCMSUpdate(m->slots[slot]->read_cms, key);
    hotkeyTopKAdd(m->slots[slot]->read_topk, key, count, val_type);
}

void writeHotKeyDetection(robj *key, int val_type, int slot) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    if (slot < 0 || slot >= HOTKEY_SLOTS) return;
    server.hotkey_runtime_total_sampled++;

    if (!m->slots[slot])
        m->slots[slot] = hotkeySlotStateNew();

    size_t count = hotkeyCMSUpdate(m->slots[slot]->write_cms, key);
    hotkeyTopKAdd(m->slots[slot]->write_topk, key, count, val_type);
}

/* ===========================================================================
 * HOTKEYS commands
 * ==========================================================================*/

typedef struct {
    sds key;
    uint64_t count;
    int is_read;
    int slot;
} hotkeyCollected;

static int hotkeyCollectedCmpDesc(const void *a, const void *b) {
    const hotkeyCollected *ea = a;
    const hotkeyCollected *eb = b;
    if (eb->count > ea->count) return 1;
    if (eb->count < ea->count) return -1;
    return 0;
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
    int filter_slot = -1;
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
        } else if (!strcasecmp(objectGetVal(c->argv[i]), "SLOT") && i + 1 < c->argc) {
            long long slot;
            if (getLongLongFromObject(c->argv[i + 1], &slot) != C_OK ||
                slot < 0 || slot >= HOTKEY_SLOTS) {
                addReplyErrorFormat(c, "Invalid slot number. Must be 0-%d", HOTKEY_SLOTS - 1);
                return;
            }
            filter_slot = (int)slot;
            i += 2;
        } else {
            addReplyError(c, "Syntax error. Usage: HOTKEYS GET [TYPE {read|write|all}] [SLOT <n>]");
            return;
        }
    }

    /* Count total entries to allocate */
    int count = 0;
    for (int s = 0; s < HOTKEY_SLOTS; s++) {
        if (filter_slot != -1 && s != filter_slot) continue;
        if (!m->slots[s]) continue;
        if (filter_type != 0 && m->slots[s]->read_topk)
            count += m->slots[s]->read_topk->size;
        if (filter_type != 1 && m->slots[s]->write_topk)
            count += m->slots[s]->write_topk->size;
    }

    if (count == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    hotkeyCollected *arr = zmalloc(count * sizeof(hotkeyCollected));
    int n = 0;

    for (int s = 0; s < HOTKEY_SLOTS; s++) {
        if (filter_slot != -1 && s != filter_slot) continue;
        if (!m->slots[s]) continue;

        if (filter_type != 0 && m->slots[s]->read_topk) {
            hotkeyTopK *tk = m->slots[s]->read_topk;
            for (int j = 0; j < tk->size; j++) {
                if (!tk->entries[j].key) continue;
                arr[n].key = tk->entries[j].key;
                arr[n].count = tk->entries[j].count;
                arr[n].is_read = 1;
                arr[n].slot = s;
                n++;
            }
        }
        if (filter_type != 1 && m->slots[s]->write_topk) {
            hotkeyTopK *tk = m->slots[s]->write_topk;
            for (int j = 0; j < tk->size; j++) {
                if (!tk->entries[j].key) continue;
                arr[n].key = tk->entries[j].key;
                arr[n].count = tk->entries[j].count;
                arr[n].is_read = 0;
                arr[n].slot = s;
                n++;
            }
        }
    }

    qsort(arr, n, sizeof(hotkeyCollected), hotkeyCollectedCmpDesc);

    /* Cap output at top_k */
    int limit = n < server.hotkey_top_k ? n : server.hotkey_top_k;
    addReplyArrayLen(c, limit);
    for (int j = 0; j < limit; j++) {
        addReplyArrayLen(c, 8);
        addReplyBulkCString(c, "key");
        addReplyBulkCBuffer(c, arr[j].key, sdslen(arr[j].key));
        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, arr[j].is_read ? "read" : "write");
        addReplyBulkCString(c, "slot");
        addReplyLongLong(c, arr[j].slot);
        addReplyBulkCString(c, "count");
        addReplyLongLong(c, arr[j].count);
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
