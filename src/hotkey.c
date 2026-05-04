#include "server.h"

/* ---- Argument parsing helper for HOTKEYS GET / MG ----
 * Parses [SLOT <n>] [TYPE {read|write|all}] in any order.
 * Returns 1 on success, 0 on error (error reply already sent). */
int parseHotkeyFilterArgs(client *c, int start_idx, int *filter_type, int *filter_slot) {
    *filter_type = -1;
    *filter_slot = -1;
    int i = start_idx;
    while (i < c->argc) {
        if (!strcasecmp(objectGetVal(c->argv[i]), "TYPE") && i + 1 < c->argc) {
            char *t = objectGetVal(c->argv[i + 1]);
            if (!strcasecmp(t, "read")) *filter_type = 1;
            else if (!strcasecmp(t, "write")) *filter_type = 0;
            else if (!strcasecmp(t, "all")) *filter_type = -1;
            else { addReplyError(c, "Invalid type. Use 'read', 'write', or 'all'"); return 0; }
            i += 2;
        } else if (!strcasecmp(objectGetVal(c->argv[i]), "SLOT") && i + 1 < c->argc) {
            long long slot;
            if (getLongLongFromObject(c->argv[i + 1], &slot) != C_OK || slot < 0 || slot >= HOTKEY_SLOTS) {
                addReplyErrorFormat(c, "Invalid slot number. Must be 0-%d", HOTKEY_SLOTS - 1);
                return 0;
            }
            *filter_slot = (int)slot;
            i += 2;
        } else {
            addReplyError(c, "Syntax error. Use [SLOT <n>] [TYPE {read|write|all}]");
            return 0;
        }
    }
    return 1;
}

int matchesFilter(hotkeyHistoryEntry *e, int filter_type, int filter_slot) {
    if (!e) return 0;
    if (filter_type != -1 && e->is_read != filter_type) return 0;
    if (filter_slot != -1 && e->slot != filter_slot) return 0;
    return 1;
}

void replyWithHotkeyEntry(client *c, hotkeyLRUNode *node) {
    addReplyArrayLen(c, 14);
    addReplyBulkCString(c, "key");
    addReplyBulkCString(c, node->key);
    addReplyBulkCString(c, "type");
    addReplyBulkCString(c, node->entry->is_read ? "read" : "write");
    addReplyBulkCString(c, "slot");
    addReplyLongLong(c, node->entry->slot);
    addReplyBulkCString(c, "peak_qps");
    addReplyLongLong(c, node->entry->peak_qps);
    addReplyBulkCString(c, "first_detected");
    addReplyLongLong(c, node->entry->first_detected);
    addReplyBulkCString(c, "last_detected");
    addReplyLongLong(c, node->entry->last_detected);
    addReplyBulkCString(c, "duration");
    addReplyLongLong(c, node->entry->duration);
}

/* ---- HOTKEYS GET / RESET / dispatcher ---- */

void hotkeysGetCommand(client *c) {
    if (!server.hotkey_enabled) { addReplyError(c, "Hotkey detection is disabled"); return; }
    if (!server.hotkey_manager || !server.hotkey_manager->history_lru) { addReplyArrayLen(c, 0); return; }
    int filter_type, filter_slot;
    if (!parseHotkeyFilterArgs(c, 2, &filter_type, &filter_slot)) return;
    expireHotkeyHistory(server.hotkey_manager);
    if (!server.hotkey_manager || !server.hotkey_manager->history_lru) { addReplyArrayLen(c, 0); return; }
    int count = 0;
    hotkeyLRUNode *cur = server.hotkey_manager->history_lru->head;
    while (cur) { if (matchesFilter(cur->entry, filter_type, filter_slot)) count++; cur = cur->next; }
    addReplyArrayLen(c, count);
    cur = server.hotkey_manager->history_lru->head;
    while (cur) {
        if (cur->entry && cur->key && matchesFilter(cur->entry, filter_type, filter_slot))
            replyWithHotkeyEntry(c, cur);
        cur = cur->next;
    }
}

void hotkeysResetCommand(client *c) {
    if (!server.hotkey_enabled) { addReplyError(c, "Hotkey detection is disabled"); return; }
    if (server.hotkey_manager) {
        if (server.hotkey_manager->history_dict) dictEmpty(server.hotkey_manager->history_dict, NULL);
        if (server.hotkey_manager->history_lru) {
            server.hotkey_manager->history_lru->head = NULL;
            server.hotkey_manager->history_lru->tail = NULL;
            server.hotkey_manager->history_lru->size = 0;
        }
        hotkeyManagerReset(server.hotkey_manager);
        server.hotkey_runtime_history_count = 0;
    }
    addReply(c, shared.ok);
}

void hotkeysCommand(client *c) {
    if (c->argc < 2) { addReplyError(c, "Wrong number of arguments for 'HOTKEYS' command"); return; }
    char *subcmd = objectGetVal(c->argv[1]);
    if (!strcasecmp(subcmd, "get")) hotkeysGetCommand(c);
    else if (!strcasecmp(subcmd, "reset")) hotkeysResetCommand(c);
    else if (!strcasecmp(subcmd, "mg")) hotkeysMGGetCommand(c);
    else if (!strcasecmp(subcmd, "mgreset")) hotkeysMGResetCommand(c);
    else addReplyErrorFormat(c, "Unknown HOTKEYS subcommand '%s'", subcmd);
}

/* ---- Hash / CMS primitives ---- */

uint32_t murmurHash2(const void *key, int len, uint32_t seed) {
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = seed ^ len;
    const unsigned char *data = (const unsigned char *)key;
    while (len >= 4) {
        uint32_t k = *(uint32_t *)data;
        k *= m; k ^= k >> r; k *= m; h *= m; h ^= k;
        data += 4; len -= 4;
    }
    switch (len) {
    case 3: h ^= data[2] << 16; /* fallthrough */
    case 2: h ^= data[1] << 8;  /* fallthrough */
    case 1: h ^= data[0]; h *= m;
    };
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

static size_t nextPowerOfTwo(size_t n) {
    if (n == 0) return 1;
    if ((n & (n - 1)) == 0) return n;
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

hotkeyCMS *newHotkeyCMS(size_t width, size_t depth) {
    serverAssert(width > 0 && depth > 0);
    hotkeyCMS *cms = zcalloc(sizeof(hotkeyCMS));
    size_t adj = nextPowerOfTwo(width);
    cms->width = adj;
    cms->depth = depth;
    cms->width_mask = adj - 1;
    cms->array = zcalloc(adj * depth * sizeof(uint32_t));
    return cms;
}

void freeHotkeyCMS(hotkeyCMS *cms) {
    if (!cms) return;
    zfree(cms->array);
    zfree(cms);
}

void hotkeyCMSReset(hotkeyCMS *cms) {
    if (!cms || !cms->array) return;
    memset(cms->array, 0, (size_t)cms->width * cms->depth * sizeof(uint32_t));
}

size_t hotkeyCMSUpdate(hotkeyCMS *cms, robj *key) {
    if (!cms || !key || !objectGetVal(key)) return 0;
    size_t len = stringObjectLen(key);
    if (len == 0) return 0;
    size_t minCount = SIZE_MAX;
    for (size_t i = 0; i < cms->depth; ++i) {
        uint32_t hash = murmurHash2(objectGetVal(key), len, i);
        size_t loc = (hash & cms->width_mask) + (i * cms->width);
        cms->array[loc]++;
        minCount = min(minCount, cms->array[loc]);
    }
    return minCount;
}

/* ---- Min-heap for top-k ---- */
hotkeyHeap *hotkeyHeapNew(int capacity) {
    hotkeyHeap *h = zcalloc(sizeof(hotkeyHeap));
    h->entries = zcalloc(capacity * sizeof(hotkeyHeapEntry));
    h->capacity = capacity;
    h->size = 0;
    h->keys = dictCreate(&heapKeysDictType);
    return h;
}

void hotkeyHeapFree(hotkeyHeap *h) {
    if (!h) return;
    for (int i = 0; i < h->size; i++) {
        if (h->entries[i].key) sdsfree(h->entries[i].key);
    }
    zfree(h->entries);
    if (h->keys) dictRelease(h->keys);
    zfree(h);
}

void hotkeyHeapReset(hotkeyHeap *h) {
    if (!h) return;
    for (int i = 0; i < h->size; i++) {
        if (h->entries[i].key) sdsfree(h->entries[i].key);
        h->entries[i].key = NULL;
    }
    h->size = 0;
    if (h->keys) dictEmpty(h->keys, NULL);
}

static void heapSwap(hotkeyHeap *h, int a, int b) {
    hotkeyHeapEntry tmp = h->entries[a];
    h->entries[a] = h->entries[b];
    h->entries[b] = tmp;
    /* Update index in dict */
    dictEntry *da = dictFind(h->keys, h->entries[a].key);
    dictEntry *db = dictFind(h->keys, h->entries[b].key);
    if (da) *(int *)dictGetVal(da) = a;
    if (db) *(int *)dictGetVal(db) = b;
}

static void heapSiftUp(hotkeyHeap *h, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (h->entries[i].count < h->entries[parent].count) {
            heapSwap(h, i, parent);
            i = parent;
        } else break;
    }
}

static void heapSiftDown(hotkeyHeap *h, int i) {
    while (1) {
        int smallest = i;
        int left = 2 * i + 1, right = 2 * i + 2;
        if (left < h->size && h->entries[left].count < h->entries[smallest].count) smallest = left;
        if (right < h->size && h->entries[right].count < h->entries[smallest].count) smallest = right;
        if (smallest == i) break;
        heapSwap(h, i, smallest);
        i = smallest;
    }
}

void hotkeyHeapInsert(hotkeyHeap *h, const char *key, uint64_t count, int val_type) {
    if (!h || !key) return;

    /* If key already in heap, update its count if larger */
    dictEntry *existing = dictFind(h->keys, key);
    if (existing) {
        int idx = *(int *)dictGetVal(existing);
        if (count > h->entries[idx].count) {
            h->entries[idx].count = count;
            h->entries[idx].val_type = val_type;
            heapSiftDown(h, idx); /* Count increased, may need to push down in min-heap */
        }
        return;
    }

    /* If heap not full, add directly */
    if (h->size < h->capacity) {
        int idx = h->size;
        h->entries[idx].key = sdsnew(key);
        h->entries[idx].count = count;
        h->entries[idx].val_type = val_type;
        int *idxp = zmalloc(sizeof(int));
        *idxp = idx;
        dictAdd(h->keys, sdsnew(key), idxp);
        h->size++;
        heapSiftUp(h, idx);
        return;
    }

    /* Heap full — replace root (minimum) if new count is larger */
    if (count <= h->entries[0].count) return;

    /* Remove old root from dict */
    dictDelete(h->keys, h->entries[0].key);
    sdsfree(h->entries[0].key);

    /* Replace root */
    h->entries[0].key = sdsnew(key);
    h->entries[0].count = count;
    h->entries[0].val_type = val_type;
    int *idxp = zmalloc(sizeof(int));
    *idxp = 0;
    dictAdd(h->keys, sdsnew(key), idxp);
    heapSiftDown(h, 0);
}

/* ---- LRU helpers ---- */

static hotkeyLRU *hotkeyLRUInit(void) { return zcalloc(sizeof(hotkeyLRU)); }

static void hotkeyLRUMoveToHead(hotkeyLRU *lru, hotkeyLRUNode *node) {
    if (!lru || !node || lru->head == node) return;
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (lru->tail == node) lru->tail = node->prev;
    node->prev = NULL;
    node->next = lru->head;
    if (lru->head) lru->head->prev = node;
    lru->head = node;
    if (!lru->tail) lru->tail = node;
}

static hotkeyLRUNode *hotkeyLRUAddToHead(hotkeyLRU *lru, sds key, hotkeyHistoryEntry *entry) {
    if (!lru) return NULL;
    hotkeyLRUNode *node = zcalloc(sizeof(hotkeyLRUNode));
    node->key = key; node->entry = entry;
    node->next = lru->head;
    if (lru->head) lru->head->prev = node;
    lru->head = node;
    if (!lru->tail) lru->tail = node;
    lru->size++;
    return node;
}

static hotkeyLRUNode *hotkeyLRURemoveTail(hotkeyLRU *lru) {
    if (!lru || !lru->tail) return NULL;
    hotkeyLRUNode *tail = lru->tail;
    if (tail->prev) { tail->prev->next = NULL; lru->tail = tail->prev; }
    else { lru->head = NULL; lru->tail = NULL; }
    lru->size--;
    return tail;
}

/* ---- Manager lifecycle ---- */

hotkeyManager *hotkeyManagerInit(size_t cms_width, size_t cms_depth) {
    UNUSED(cms_width); UNUSED(cms_depth);
    hotkeyManager *m = zcalloc(sizeof(hotkeyManager));
    m->history_dict = dictCreate(&hotkeyHistoryDictType);
    m->history_lru = hotkeyLRUInit();
    return m;
}

void hotkeyManagerFree(hotkeyManager *m) {
    if (!m) return;
    for (int i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->read_cms[i]) freeHotkeyCMS(m->read_cms[i]);
        if (m->read_topk[i]) hotkeyHeapFree(m->read_topk[i]);
        if (m->write_cms[i]) freeHotkeyCMS(m->write_cms[i]);
        if (m->write_topk[i]) hotkeyHeapFree(m->write_topk[i]);
    }
    if (m->history_dict) dictRelease(m->history_dict);
    if (m->history_lru) zfree(m->history_lru);
    zfree(m);
}

void hotkeyManagerReset(hotkeyManager *m) {
    if (!m) return;
    for (int i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->read_cms[i]) hotkeyCMSReset(m->read_cms[i]);
        if (m->read_topk[i]) hotkeyHeapReset(m->read_topk[i]);
        if (m->write_cms[i]) hotkeyCMSReset(m->write_cms[i]);
        if (m->write_topk[i]) hotkeyHeapReset(m->write_topk[i]);
    }
}

/* ---- Per-slot detection hooks ---- */

void readHotKeyDetection(robj *key, int val_type, int slot) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    if (!m->read_cms[slot])
        m->read_cms[slot] = newHotkeyCMS(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    if (!m->read_topk[slot])
        m->read_topk[slot] = hotkeyHeapNew(server.hotkey_top_k);
    size_t count = hotkeyCMSUpdate(m->read_cms[slot], key);
    hotkeyHeapInsert(m->read_topk[slot], objectGetVal(key), count, val_type);
}

void writeHotKeyDetection(robj *key, int val_type, int slot) {
    hotkeyManager *m = server.hotkey_manager;
    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    if (!m->write_cms[slot])
        m->write_cms[slot] = newHotkeyCMS(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    if (!m->write_topk[slot])
        m->write_topk[slot] = hotkeyHeapNew(server.hotkey_top_k);
    size_t count = hotkeyCMSUpdate(m->write_cms[slot], key);
    hotkeyHeapInsert(m->write_topk[slot], objectGetVal(key), count, val_type);
}

/* ---- History management ---- */

static uint64_t calculateActualQPS(uint64_t count) {
    if (server.hotkey_sampling_ratio <= 0 || server.hotkey_window_seconds <= 0) return 0;
    return (count * 100 / server.hotkey_sampling_ratio) / server.hotkey_window_seconds;
}

static void evictLRUHistoryEntry(hotkeyManager *m) {
    if (!m || !m->history_lru || !m->history_dict || m->history_lru->size == 0) return;
    hotkeyLRUNode *tail = hotkeyLRURemoveTail(m->history_lru);
    if (!tail) return;
    if (tail->key && m->history_dict) dictDelete(m->history_dict, tail->key);
    else { if (tail->key) sdsfree(tail->key); if (tail->entry) zfree(tail->entry); zfree(tail); }
}

static void addSingleHotkeyToHistory(hotkeyManager *m, const char *key_str, uint64_t count, int val_type, int is_read, int slot) {
    if (!m || !key_str) return;
    time_t now = time(NULL);
    uint64_t qps = calculateActualQPS(count);

    dictEntry *de = dictFind(m->history_dict, key_str);
    if (de) {
        hotkeyLRUNode *node = dictGetVal(de);
        if (!node || !node->entry) return;
        hotkeyHistoryEntry *h = node->entry;
        if (h->peak_qps < qps) h->peak_qps = qps;
        h->last_detected = now;
        h->duration += server.hotkey_window_seconds;
        h->val_type = val_type;
        h->slot = slot;
        hotkeyLRUMoveToHead(m->history_lru, node);
        return;
    }

    while (m->history_lru->size >= (size_t)server.hotkey_history_max_count) {
        size_t old = m->history_lru->size;
        evictLRUHistoryEntry(m);
        if (m->history_lru->size >= old) break;
    }

    hotkeyHistoryEntry *h = zcalloc(sizeof(hotkeyHistoryEntry));
    h->peak_qps = qps; h->first_detected = now; h->last_detected = now;
    h->is_read = is_read; h->duration = server.hotkey_window_seconds;
    h->val_type = val_type; h->slot = slot;

    sds ks = sdsnew(key_str);
    hotkeyLRUNode *node = hotkeyLRUAddToHead(m->history_lru, ks, h);
    if (!node) { sdsfree(ks); zfree(h); return; }
    if (dictAdd(m->history_dict, ks, node) != DICT_OK) {
        if (m->history_lru->head == node) {
            m->history_lru->head = node->next;
            if (node->next) node->next->prev = NULL; else m->history_lru->tail = NULL;
            m->history_lru->size--;
        }
        sdsfree(ks); zfree(h); zfree(node);
    }
}

static void publishSingleHotkeyNotification(const char *key_str, uint64_t count, int val_type, int is_read, int slot) {
    uint64_t qps = calculateActualQPS(count);
    sds message = sdscatprintf(sdsempty(),
        "{\"key\":\"%s\",\"type\":\"%s\",\"qps\":%llu,\"val_type\":%d,\"slot\":%d,\"timestamp\":%ld}",
        key_str, is_read ? "read" : "write", (unsigned long long)qps, val_type, slot, (long)time(NULL));
    robj *msg = createObject(OBJ_STRING, message);
    if (shared.hotkey_notify_channel && msg) pubsubPublishMessage(shared.hotkey_notify_channel, msg, 0);
    if (msg) decrRefCount(msg);
}

static void flushHeapToHistory(hotkeyManager *m, hotkeyHeap *heap, int is_read, int slot) {
    if (!heap) return;
    for (int i = 0; i < heap->size; i++) {
        hotkeyHeapEntry *e = &heap->entries[i];
        if (!e->key) continue;
        addSingleHotkeyToHistory(m, e->key, e->count, e->val_type, is_read, slot);
        publishSingleHotkeyNotification(e->key, e->count, e->val_type, is_read, slot);
        if (is_read) server.hotkey_runtime_read_count++;
        else server.hotkey_runtime_write_count++;
    }
}

void addHotkeyToHistory(hotkeyManager *m) {
    if (!m) return;
    for (int slot = 0; slot < HOTKEY_SLOTS; slot++) {
        flushHeapToHistory(m, m->read_topk[slot], 1, slot);
        flushHeapToHistory(m, m->write_topk[slot], 0, slot);
    }
    server.hotkey_runtime_history_count = m->history_lru->size;
}

void expireHotkeyHistory(hotkeyManager *m) {
    if (!m || !m->history_lru || !m->history_dict) return;
    time_t expire = time(NULL) - server.hotkey_history_ttl;
    hotkeyLRUNode *cur = m->history_lru->tail;
    while (cur) {
        hotkeyLRUNode *prev = cur->prev;
        if (cur->entry && cur->entry->last_detected < expire) {
            if (cur->prev) cur->prev->next = cur->next; else m->history_lru->head = cur->next;
            if (cur->next) cur->next->prev = cur->prev; else m->history_lru->tail = cur->prev;
            m->history_lru->size--;
            if (cur->key) dictDelete(m->history_dict, cur->key);
        } else break;
        cur = prev;
    }
    server.hotkey_runtime_history_count = m->history_lru->size;
}

/* ---- Config callbacks ---- */

int hotKeyEnabledCallback(const char **err) {
    UNUSED(err);
    if (server.hotkey_enabled) {
        if (!server.hotkey_manager) {
            size_t orig = server.hotkey_cms_bucket_size;
            size_t adj = nextPowerOfTwo(orig);
            if (adj != orig) { server.hotkey_cms_bucket_size = adj; }
            server.hotkey_manager = hotkeyManagerInit(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
        }
    } else {
        if (server.hotkey_manager) { hotkeyManagerFree(server.hotkey_manager); server.hotkey_manager = NULL; }
    }
    return 1;
}

int hotKeyCMSBucketSizeCallback(const char **err) {
    UNUSED(err);
    if (!server.hotkey_enabled) return 1;
    size_t orig = server.hotkey_cms_bucket_size;
    size_t adj = nextPowerOfTwo(orig);
    if (adj != orig) server.hotkey_cms_bucket_size = adj;
    if (server.hotkey_manager) { hotkeyManagerFree(server.hotkey_manager); server.hotkey_manager = NULL; }
    server.hotkey_manager = hotkeyManagerInit(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    return 1;
}

int hotKeyCMSDepthCallback(const char **err) {
    UNUSED(err);
    if (!server.hotkey_enabled) return 1;
    if (server.hotkey_manager) { hotkeyManagerFree(server.hotkey_manager); server.hotkey_manager = NULL; }
    server.hotkey_manager = hotkeyManagerInit(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    return 1;
}
