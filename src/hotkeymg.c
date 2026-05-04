#include "server.h"

/*-----------------------------------------------------------------------------
 * Misra-Gries summary for hot key detection — flat-array variant
 *
 * Uses a fixed-size array of k entries instead of a dict. For small k
 * (default 16), linear scan is faster than dict hash+pointer-chase and
 * the entire working set fits in L1 cache (~1KB).
 *----------------------------------------------------------------------------*/

/* ---- Misra-Gries summary operations ---- */

hotkeyMGSummary *hotkeyMGSummaryNew(int max_keys) {
    hotkeyMGSummary *s = zcalloc(sizeof(hotkeyMGSummary));
    s->entries = zcalloc(max_keys * sizeof(hotkeyMGEntry));
    s->max_keys = max_keys;
    s->size = 0;
    s->total = 0;
    return s;
}

void hotkeyMGSummaryFree(hotkeyMGSummary *s) {
    if (!s) return;
    for (int i = 0; i < s->size; i++) {
        if (s->entries[i].key) sdsfree(s->entries[i].key);
    }
    zfree(s->entries);
    zfree(s);
}

void hotkeyMGSummaryReset(hotkeyMGSummary *s) {
    if (!s) return;
    for (int i = 0; i < s->size; i++) {
        if (s->entries[i].key) sdsfree(s->entries[i].key);
        s->entries[i].key = NULL;
        s->entries[i].count = 0;
    }
    s->size = 0;
    s->total = 0;
}

void hotkeyMGSummaryAddTyped(hotkeyMGSummary *s, robj *key, int val_type) {
    if (!s || !key || !objectGetVal(key)) return;

    s->total++;
    const char *k = objectGetVal(key);
    size_t klen = sdslen(objectGetVal(key));

    /* Case 1: key already tracked — linear scan */
    for (int i = 0; i < s->size; i++) {
        if (s->entries[i].key && sdslen(s->entries[i].key) == klen &&
            memcmp(s->entries[i].key, k, klen) == 0) {
            s->entries[i].count++;
            s->entries[i].val_type = val_type;
            return;
        }
    }

    /* Case 2: room available — append */
    if (s->size < s->max_keys) {
        s->entries[s->size].key = sdsnewlen(k, klen);
        s->entries[s->size].count = 1;
        s->entries[s->size].val_type = val_type;
        s->size++;
        return;
    }

    /* Case 3: full — decrement all, compact out zeros */
    int write = 0;
    for (int i = 0; i < s->size; i++) {
        s->entries[i].count--;
        if (s->entries[i].count > 0) {
            if (write != i) {
                s->entries[write] = s->entries[i];
            }
            write++;
        } else {
            sdsfree(s->entries[i].key);
            s->entries[i].key = NULL;
        }
    }
    s->size = write;
}

/* ---- Manager lifecycle ---- */

hotkeyMGManager *hotkeyMGManagerInit(int max_keys) {
    UNUSED(max_keys);
    hotkeyMGManager *m = zcalloc(sizeof(hotkeyMGManager));
    m->history_dict = dictCreate(&hotkeyMGHistoryDictType);
    m->history_lru = zcalloc(sizeof(hotkeyLRU));
    return m;
}

void hotkeyMGManagerFree(hotkeyMGManager *m) {
    if (!m) return;
    for (int i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->read_summaries[i]) hotkeyMGSummaryFree(m->read_summaries[i]);
        if (m->write_summaries[i]) hotkeyMGSummaryFree(m->write_summaries[i]);
    }
    if (m->history_dict) dictRelease(m->history_dict);
    if (m->history_lru) zfree(m->history_lru);
    zfree(m);
}

void hotkeyMGManagerReset(hotkeyMGManager *m) {
    if (!m) return;
    for (int i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->read_summaries[i]) hotkeyMGSummaryReset(m->read_summaries[i]);
        if (m->write_summaries[i]) hotkeyMGSummaryReset(m->write_summaries[i]);
    }
}

/* ---- Per-slot detection hooks ---- */

void readHotKeyMGDetection(robj *key, int val_type, int slot) {
    hotkeyMGManager *m = server.hotkey_mg_manager;
    if (!m || !key) return;
    server.hotkey_mg_runtime_total_sampled++;
    if (!m->read_summaries[slot])
        m->read_summaries[slot] = hotkeyMGSummaryNew(server.hotkey_mg_max_keys);
    hotkeyMGSummaryAddTyped(m->read_summaries[slot], key, val_type);
}

void writeHotKeyMGDetection(robj *key, int val_type, int slot) {
    hotkeyMGManager *m = server.hotkey_mg_manager;
    if (!m || !key) return;
    server.hotkey_mg_runtime_total_sampled++;
    if (!m->write_summaries[slot])
        m->write_summaries[slot] = hotkeyMGSummaryNew(server.hotkey_mg_max_keys);
    hotkeyMGSummaryAddTyped(m->write_summaries[slot], key, val_type);
}

/* ---- LRU helpers ---- */

static void mgLRUMoveToHead(hotkeyLRU *lru, hotkeyLRUNode *node) {
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

static hotkeyLRUNode *mgLRUAddToHead(hotkeyLRU *lru, sds key, hotkeyHistoryEntry *entry) {
    hotkeyLRUNode *node = zcalloc(sizeof(hotkeyLRUNode));
    node->key = key; node->entry = entry;
    node->next = lru->head;
    if (lru->head) lru->head->prev = node;
    lru->head = node;
    if (!lru->tail) lru->tail = node;
    lru->size++;
    return node;
}

static hotkeyLRUNode *mgLRURemoveTail(hotkeyLRU *lru) {
    if (!lru || !lru->tail) return NULL;
    hotkeyLRUNode *tail = lru->tail;
    if (tail->prev) { tail->prev->next = NULL; lru->tail = tail->prev; }
    else { lru->head = NULL; lru->tail = NULL; }
    lru->size--;
    return tail;
}

static void mgEvictLRU(hotkeyMGManager *m) {
    hotkeyLRUNode *tail = mgLRURemoveTail(m->history_lru);
    if (!tail) return;
    if (tail->key && m->history_dict) {
        dictDelete(m->history_dict, tail->key);
    } else {
        if (tail->key) sdsfree(tail->key);
        if (tail->entry) zfree(tail->entry);
        zfree(tail);
    }
}

static uint64_t mgCalculateQPS(uint64_t count) {
    if (server.hotkey_mg_sampling_ratio <= 0 || server.hotkey_window_seconds <= 0) return 0;
    return (count * 100 / server.hotkey_mg_sampling_ratio) / server.hotkey_window_seconds;
}

static void mgAddSingleToHistory(hotkeyMGManager *m, const char *key_str, uint64_t count, int val_type, int is_read, int slot) {
    if (!m || !key_str) return;
    time_t now = time(NULL);
    uint64_t qps = mgCalculateQPS(count);

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
        mgLRUMoveToHead(m->history_lru, node);
        return;
    }

    while (m->history_lru->size >= (size_t)server.hotkey_mg_history_max_count) {
        size_t old = m->history_lru->size;
        mgEvictLRU(m);
        if (m->history_lru->size >= old) break;
    }

    hotkeyHistoryEntry *h = zcalloc(sizeof(hotkeyHistoryEntry));
    h->peak_qps = qps; h->first_detected = now; h->last_detected = now;
    h->is_read = is_read; h->duration = server.hotkey_window_seconds;
    h->val_type = val_type; h->slot = slot;

    sds ks = sdsnew(key_str);
    hotkeyLRUNode *node = mgLRUAddToHead(m->history_lru, ks, h);
    if (dictAdd(m->history_dict, ks, node) != DICT_OK) {
        if (m->history_lru->head == node) {
            m->history_lru->head = node->next;
            if (node->next) node->next->prev = NULL; else m->history_lru->tail = NULL;
            m->history_lru->size--;
        }
        sdsfree(ks); zfree(h); zfree(node);
    }
}

/* Flush all per-slot summaries into global history. Called from serverCron. */
void addHotkeyMGToHistory(hotkeyMGManager *m) {
    if (!m) return;

    for (int slot = 0; slot < HOTKEY_SLOTS; slot++) {
        hotkeyMGSummary *rs = m->read_summaries[slot];
        if (rs && rs->size > 0) {
            for (int i = 0; i < rs->size; i++) {
                if (!rs->entries[i].key) continue;
                mgAddSingleToHistory(m, rs->entries[i].key, rs->entries[i].count, rs->entries[i].val_type, 1, slot);
                server.hotkey_mg_runtime_read_count++;
            }
        }
        hotkeyMGSummary *ws = m->write_summaries[slot];
        if (ws && ws->size > 0) {
            for (int i = 0; i < ws->size; i++) {
                if (!ws->entries[i].key) continue;
                mgAddSingleToHistory(m, ws->entries[i].key, ws->entries[i].count, ws->entries[i].val_type, 0, slot);
                server.hotkey_mg_runtime_write_count++;
            }
        }
    }

    server.hotkey_mg_runtime_history_count = m->history_lru->size;
}

void expireHotkeyMGHistory(hotkeyMGManager *m) {
    if (!m || !m->history_lru || !m->history_dict) return;
    time_t expire = time(NULL) - server.hotkey_mg_history_ttl;
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
    server.hotkey_mg_runtime_history_count = m->history_lru->size;
}

/* ---- Commands ---- */

extern int parseHotkeyFilterArgs(client *c, int start_idx, int *filter_type, int *filter_slot);
extern int matchesFilter(hotkeyHistoryEntry *e, int filter_type, int filter_slot);
extern void replyWithHotkeyEntry(client *c, hotkeyLRUNode *node);

void hotkeysMGGetCommand(client *c) {
    if (!server.hotkey_mg_enabled) { addReplyError(c, "Hotkey MG detection is disabled"); return; }
    if (!server.hotkey_mg_manager || !server.hotkey_mg_manager->history_lru) { addReplyArrayLen(c, 0); return; }
    int filter_type, filter_slot;
    if (!parseHotkeyFilterArgs(c, 2, &filter_type, &filter_slot)) return;
    expireHotkeyMGHistory(server.hotkey_mg_manager);
    if (!server.hotkey_mg_manager || !server.hotkey_mg_manager->history_lru) { addReplyArrayLen(c, 0); return; }
    int count = 0;
    hotkeyLRUNode *cur = server.hotkey_mg_manager->history_lru->head;
    while (cur) { if (matchesFilter(cur->entry, filter_type, filter_slot)) count++; cur = cur->next; }
    addReplyArrayLen(c, count);
    cur = server.hotkey_mg_manager->history_lru->head;
    while (cur) {
        if (cur->entry && cur->key && matchesFilter(cur->entry, filter_type, filter_slot))
            replyWithHotkeyEntry(c, cur);
        cur = cur->next;
    }
}

void hotkeysMGResetCommand(client *c) {
    if (!server.hotkey_mg_enabled) { addReplyError(c, "Hotkey MG detection is disabled"); return; }
    if (server.hotkey_mg_manager) {
        if (server.hotkey_mg_manager->history_dict) dictEmpty(server.hotkey_mg_manager->history_dict, NULL);
        if (server.hotkey_mg_manager->history_lru) {
            server.hotkey_mg_manager->history_lru->head = NULL;
            server.hotkey_mg_manager->history_lru->tail = NULL;
            server.hotkey_mg_manager->history_lru->size = 0;
        }
        hotkeyMGManagerReset(server.hotkey_mg_manager);
        server.hotkey_mg_runtime_history_count = 0;
    }
    addReply(c, shared.ok);
}

/* ---- Config callbacks ---- */

int hotKeyMGEnabledCallback(const char **err) {
    UNUSED(err);
    if (server.hotkey_mg_enabled) {
        if (!server.hotkey_mg_manager)
            server.hotkey_mg_manager = hotkeyMGManagerInit(server.hotkey_mg_max_keys);
    } else {
        if (server.hotkey_mg_manager) { hotkeyMGManagerFree(server.hotkey_mg_manager); server.hotkey_mg_manager = NULL; }
    }
    return 1;
}

int hotKeyMGMaxKeysCallback(const char **err) {
    UNUSED(err);
    if (!server.hotkey_mg_enabled) return 1;
    if (server.hotkey_mg_manager) { hotkeyMGManagerFree(server.hotkey_mg_manager); server.hotkey_mg_manager = NULL; }
    server.hotkey_mg_manager = hotkeyMGManagerInit(server.hotkey_mg_max_keys);
    return 1;
}
