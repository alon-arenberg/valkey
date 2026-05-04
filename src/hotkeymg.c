#include "server.h"

/*-----------------------------------------------------------------------------
 * Misra-Gries summary for hot key detection — per-slot variant
 *
 * Each slot gets its own lazily-allocated read/write MG summary.
 * History is global (single LRU) with slot annotation on each entry.
 *----------------------------------------------------------------------------*/

/* ---- Misra-Gries summary operations ---- */

hotkeyMGSummary *hotkeyMGSummaryNew(int max_keys) {
    hotkeyMGSummary *s = zcalloc(sizeof(hotkeyMGSummary));
    s->counters = dictCreate(&hotKeyMGDictType);
    s->max_keys = max_keys;
    s->total = 0;
    return s;
}

void hotkeyMGSummaryFree(hotkeyMGSummary *s) {
    if (!s) return;
    if (s->counters) dictRelease(s->counters);
    zfree(s);
}

void hotkeyMGSummaryReset(hotkeyMGSummary *s) {
    if (!s) return;
    dictEmpty(s->counters, NULL);
    s->total = 0;
}

void hotkeyMGSummaryAdd(hotkeyMGSummary *s, robj *key) {
    if (!s || !key || !objectGetVal(key)) return;
    s->total++;
    const char *k = objectGetVal(key);

    dictEntry *de = dictFind(s->counters, k);
    if (de) { ((hotkeyMGEntry *)dictGetVal(de))->count++; return; }

    if ((long long)dictSize(s->counters) < s->max_keys) {
        hotkeyMGEntry *e = zcalloc(sizeof(hotkeyMGEntry));
        e->count = 1;
        e->val_type = OBJ_STRING;
        dictAdd(s->counters, sdsnew(k), e);
        return;
    }

    dictIterator *di = dictGetSafeIterator(s->counters);
    while ((de = dictNext(di)) != NULL) {
        hotkeyMGEntry *e = dictGetVal(de);
        if (--e->count == 0) dictDelete(s->counters, dictGetKey(de));
    }
    dictReleaseIterator(di);
}

void hotkeyMGSummaryAddTyped(hotkeyMGSummary *s, robj *key, int val_type) {
    if (!s || !key || !objectGetVal(key)) return;
    s->total++;
    const char *k = objectGetVal(key);

    dictEntry *de = dictFind(s->counters, k);
    if (de) {
        hotkeyMGEntry *e = dictGetVal(de);
        e->count++;
        e->val_type = val_type;
        return;
    }

    if ((long long)dictSize(s->counters) < s->max_keys) {
        hotkeyMGEntry *e = zcalloc(sizeof(hotkeyMGEntry));
        e->count = 1;
        e->val_type = val_type;
        dictAdd(s->counters, sdsnew(k), e);
        return;
    }

    dictIterator *di = dictGetSafeIterator(s->counters);
    while ((de = dictNext(di)) != NULL) {
        hotkeyMGEntry *e = dictGetVal(de);
        if (--e->count == 0) dictDelete(s->counters, dictGetKey(de));
    }
    dictReleaseIterator(di);
}

/* ---- Manager lifecycle ---- */

hotkeyMGManager *hotkeyMGManagerInit(int max_keys) {
    UNUSED(max_keys);
    hotkeyMGManager *m = zcalloc(sizeof(hotkeyMGManager));
    /* Per-slot arrays are zero-initialized (NULL) by zcalloc — lazy allocation */
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
    node->key = key;
    node->entry = entry;
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
    h->peak_qps = qps;
    h->first_detected = now;
    h->last_detected = now;
    h->is_read = is_read;
    h->duration = server.hotkey_window_seconds;
    h->val_type = val_type;
    h->slot = slot;

    sds ks = sdsnew(key_str);
    hotkeyLRUNode *node = mgLRUAddToHead(m->history_lru, ks, h);
    if (dictAdd(m->history_dict, ks, node) != DICT_OK) {
        if (m->history_lru->head == node) {
            m->history_lru->head = node->next;
            if (node->next) node->next->prev = NULL;
            else m->history_lru->tail = NULL;
            m->history_lru->size--;
        }
        sdsfree(ks);
        zfree(h);
        zfree(node);
    }
}

/* Flush all per-slot summaries into global history. Called from serverCron. */
void addHotkeyMGToHistory(hotkeyMGManager *m) {
    if (!m) return;

    for (int slot = 0; slot < HOTKEY_SLOTS; slot++) {
        if (m->read_summaries[slot] && dictSize(m->read_summaries[slot]->counters) > 0) {
            dictIterator *di = dictGetIterator(m->read_summaries[slot]->counters);
            dictEntry *de;
            while ((de = dictNext(di)) != NULL) {
                hotkeyMGEntry *e = dictGetVal(de);
                mgAddSingleToHistory(m, dictGetKey(de), e->count, e->val_type, 1, slot);
                server.hotkey_mg_runtime_read_count++;
            }
            dictReleaseIterator(di);
        }
        if (m->write_summaries[slot] && dictSize(m->write_summaries[slot]->counters) > 0) {
            dictIterator *di = dictGetIterator(m->write_summaries[slot]->counters);
            dictEntry *de;
            while ((de = dictNext(di)) != NULL) {
                hotkeyMGEntry *e = dictGetVal(de);
                mgAddSingleToHistory(m, dictGetKey(de), e->count, e->val_type, 0, slot);
                server.hotkey_mg_runtime_write_count++;
            }
            dictReleaseIterator(di);
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
            if (cur->prev) cur->prev->next = cur->next;
            else m->history_lru->head = cur->next;
            if (cur->next) cur->next->prev = cur->prev;
            else m->history_lru->tail = cur->prev;
            m->history_lru->size--;
            if (cur->key) dictDelete(m->history_dict, cur->key);
        } else {
            break;
        }
        cur = prev;
    }
    server.hotkey_mg_runtime_history_count = m->history_lru->size;
}

/* ---- Commands ---- */

/* Declared in hotkey.c — shared filter parser */
extern int parseHotkeyFilterArgs(client *c, int start_idx, int *filter_type, int *filter_slot);
extern int matchesFilter(hotkeyHistoryEntry *e, int filter_type, int filter_slot);
extern void replyWithHotkeyEntry(client *c, hotkeyLRUNode *node);

/* HOTKEYS MG [SLOT <n>] [TYPE {read|write|all}] */
void hotkeysMGGetCommand(client *c) {
    if (!server.hotkey_mg_enabled) {
        addReplyError(c, "Hotkey MG detection is disabled");
        return;
    }
    if (!server.hotkey_mg_manager || !server.hotkey_mg_manager->history_lru) {
        addReplyArrayLen(c, 0);
        return;
    }

    int filter_type, filter_slot;
    if (!parseHotkeyFilterArgs(c, 2, &filter_type, &filter_slot)) return;

    expireHotkeyMGHistory(server.hotkey_mg_manager);

    if (!server.hotkey_mg_manager || !server.hotkey_mg_manager->history_lru) {
        addReplyArrayLen(c, 0);
        return;
    }

    int count = 0;
    hotkeyLRUNode *cur = server.hotkey_mg_manager->history_lru->head;
    while (cur) {
        if (matchesFilter(cur->entry, filter_type, filter_slot)) count++;
        cur = cur->next;
    }

    addReplyArrayLen(c, count);
    cur = server.hotkey_mg_manager->history_lru->head;
    while (cur) {
        if (cur->entry && cur->key && matchesFilter(cur->entry, filter_type, filter_slot)) {
            replyWithHotkeyEntry(c, cur);
        }
        cur = cur->next;
    }
}

/* HOTKEYS MGRESET */
void hotkeysMGResetCommand(client *c) {
    if (!server.hotkey_mg_enabled) {
        addReplyError(c, "Hotkey MG detection is disabled");
        return;
    }
    if (server.hotkey_mg_manager) {
        if (server.hotkey_mg_manager->history_dict)
            dictEmpty(server.hotkey_mg_manager->history_dict, NULL);
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
        if (server.hotkey_mg_manager) {
            hotkeyMGManagerFree(server.hotkey_mg_manager);
            server.hotkey_mg_manager = NULL;
        }
    }
    return 1;
}

int hotKeyMGMaxKeysCallback(const char **err) {
    UNUSED(err);
    if (!server.hotkey_mg_enabled) return 1;
    if (server.hotkey_mg_manager) {
        hotkeyMGManagerFree(server.hotkey_mg_manager);
        server.hotkey_mg_manager = NULL;
    }
    server.hotkey_mg_manager = hotkeyMGManagerInit(server.hotkey_mg_max_keys);
    return 1;
}
