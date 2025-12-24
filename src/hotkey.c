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

/* Parse [SLOT <n>] [TYPE {read|write|all}] arguments in any order.
 * Returns 1 on success, 0 on error (error reply already sent). */
static int parseHotkeyFilterArgs(client *c, int start_idx, int *filter_type, int *filter_slot) {
    int i = start_idx;

    *filter_type = -1;
    *filter_slot = -1;

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
        } else if (!strcasecmp(objectGetVal(c->argv[i]), "SLOT") && i + 1 < c->argc) {
            long long slot;
            if (getLongLongFromObject(c->argv[i + 1], &slot) != C_OK ||
                slot < 0 || slot >= HOTKEY_SLOTS) {
                addReplyErrorFormat(c, "Invalid slot number. Must be 0-%d",
                                   HOTKEY_SLOTS - 1);
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

/* Check if a history entry matches the given type and slot filters. */
static int matchesFilter(hotkeyHistoryEntry *e, int filter_type, int filter_slot) {
    if (!e) return 0;
    if (filter_type != -1 && e->is_read != filter_type) return 0;
    if (filter_slot != -1 && e->slot != filter_slot) return 0;
    return 1;
}

/* Format and send a single hotkey history entry to the client. */
static void replyWithHotkeyEntry(client *c, hotkeyLRUNode *node) {
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

/* ===========================================================================
 * Misra-Gries summary operations
 * ==========================================================================*/

hotkeyMGSummary *hotkeyMGSummaryNew(int max_keys) {
    hotkeyMGSummary *s = zcalloc(sizeof(hotkeyMGSummary));
    s->entries = zcalloc(max_keys * sizeof(hotkeyMGEntry));
    s->max_keys = max_keys;
    s->size = 0;
    s->total = 0;
    return s;
}

void hotkeyMGSummaryFree(hotkeyMGSummary *s) {
    int i;

    if (!s) return;
    for (i = 0; i < s->size; i++) {
        if (s->entries[i].key) sdsfree(s->entries[i].key);
    }
    zfree(s->entries);
    zfree(s);
}

static void hotkeyMGSummaryReset(hotkeyMGSummary *s) {
    int i;

    if (!s) return;
    for (i = 0; i < s->size; i++) {
        if (s->entries[i].key) sdsfree(s->entries[i].key);
        s->entries[i].key = NULL;
        s->entries[i].count = 0;
    }
    s->size = 0;
    s->total = 0;
}

/* Add a key observation to the Misra-Gries summary.
 *
 * Three cases:
 * 1. Key already tracked: increment its counter.
 * 2. Room available (size < k): insert new entry.
 * 3. Full: decrement all counters, compact out zeros. */
static void hotkeyMGSummaryAdd(hotkeyMGSummary *s, robj *key, int val_type) {
    const char *k;
    size_t klen;
    int i, write_pos;

    if (!s || !key || !objectGetVal(key)) return;

    s->total++;
    k = objectGetVal(key);
    klen = sdslen(objectGetVal(key));

    /* Case 1: key already tracked. */
    for (i = 0; i < s->size; i++) {
        if (s->entries[i].key &&
            sdslen(s->entries[i].key) == klen &&
            memcmp(s->entries[i].key, k, klen) == 0) {
            s->entries[i].count++;
            s->entries[i].val_type = val_type;
            return;
        }
    }

    /* Case 2: room available. */
    if (s->size < s->max_keys) {
        s->entries[s->size].key = sdsnewlen(k, klen);
        s->entries[s->size].count = 1;
        s->entries[s->size].val_type = val_type;
        s->size++;
        return;
    }

    /* Case 3: full — decrement all, compact out zeros. */
    write_pos = 0;
    for (i = 0; i < s->size; i++) {
        s->entries[i].count--;
        if (s->entries[i].count > 0) {
            if (write_pos != i) {
                s->entries[write_pos] = s->entries[i];
            }
            write_pos++;
        } else {
            sdsfree(s->entries[i].key);
            s->entries[i].key = NULL;
        }
    }
    s->size = write_pos;
}

/* ===========================================================================
 * Hotkey manager lifecycle
 * ==========================================================================*/

hotkeyManager *hotkeyManagerInit(int max_keys) {
    hotkeyManager *m;

    UNUSED(max_keys);
    m = zcalloc(sizeof(hotkeyManager));
    m->history_dict = dictCreate(&hotkeyHistoryDictType);
    m->history_lru = zcalloc(sizeof(hotkeyLRU));
    return m;
}

void hotkeyManagerFree(hotkeyManager *m) {
    int i;

    if (!m) return;
    for (i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->read_summaries[i]) hotkeyMGSummaryFree(m->read_summaries[i]);
        if (m->write_summaries[i]) hotkeyMGSummaryFree(m->write_summaries[i]);
    }
    if (m->history_dict) dictRelease(m->history_dict);
    if (m->history_lru) zfree(m->history_lru);
    zfree(m);
}

void hotkeyManagerReset(hotkeyManager *m) {
    int i;

    if (!m) return;
    for (i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->read_summaries[i]) hotkeyMGSummaryReset(m->read_summaries[i]);
        if (m->write_summaries[i]) hotkeyMGSummaryReset(m->write_summaries[i]);
    }
}

/* ===========================================================================
 * Per-access detection hooks (called from lookupKey)
 * ==========================================================================*/

void readHotKeyDetection(robj *key, int val_type, int slot) {
    hotkeyManager *m = server.hotkey_manager;

    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    if (!m->read_summaries[slot])
        m->read_summaries[slot] = hotkeyMGSummaryNew(server.hotkey_max_keys);
    hotkeyMGSummaryAdd(m->read_summaries[slot], key, val_type);
}

void writeHotKeyDetection(robj *key, int val_type, int slot) {
    hotkeyManager *m = server.hotkey_manager;

    if (!m || !key) return;
    server.hotkey_runtime_total_sampled++;
    if (!m->write_summaries[slot])
        m->write_summaries[slot] = hotkeyMGSummaryNew(server.hotkey_max_keys);
    hotkeyMGSummaryAdd(m->write_summaries[slot], key, val_type);
}

/* ===========================================================================
 * LRU history list helpers
 * ==========================================================================*/

static void hotkeyLRUMoveToHead(hotkeyLRU *lru, hotkeyLRUNode *node) {
    if (!lru || !node || lru->head == node) return;

    /* Unlink from current position. */
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (lru->tail == node) lru->tail = node->prev;

    /* Insert at head. */
    node->prev = NULL;
    node->next = lru->head;
    if (lru->head) lru->head->prev = node;
    lru->head = node;
    if (!lru->tail) lru->tail = node;
}

static hotkeyLRUNode *hotkeyLRUAddToHead(hotkeyLRU *lru, sds key, hotkeyHistoryEntry *entry) {
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

static hotkeyLRUNode *hotkeyLRURemoveTail(hotkeyLRU *lru) {
    hotkeyLRUNode *tail;

    if (!lru || !lru->tail) return NULL;
    tail = lru->tail;
    if (tail->prev) {
        tail->prev->next = NULL;
        lru->tail = tail->prev;
    } else {
        lru->head = NULL;
        lru->tail = NULL;
    }
    lru->size--;
    return tail;
}

/* Evict the oldest (tail) entry from the history LRU. */
static void evictLRUHistoryEntry(hotkeyManager *m) {
    hotkeyLRUNode *tail = hotkeyLRURemoveTail(m->history_lru);

    if (!tail) return;
    if (tail->key && m->history_dict) {
        dictDelete(m->history_dict, tail->key);
    } else {
        if (tail->key) sdsfree(tail->key);
        if (tail->entry) zfree(tail->entry);
        zfree(tail);
    }
}

/* ===========================================================================
 * History management
 * ==========================================================================*/

/* Extrapolate QPS from the sampled count, sampling ratio, and window size. */
static uint64_t calculateQPS(uint64_t count) {
    if (server.hotkey_sampling_ratio <= 0 || server.hotkey_window_seconds <= 0)
        return 0;
    return (count * 100 / server.hotkey_sampling_ratio) / server.hotkey_window_seconds;
}

/* Upsert a single key into the global history. If the key already exists,
 * update its peak QPS and timestamps; otherwise create a new entry (possibly
 * evicting the oldest one). */
static void addSingleToHistory(hotkeyManager *m, const char *key_str,
                               uint64_t count, int val_type,
                               int is_read, int slot) {
    time_t now;
    uint64_t qps;
    dictEntry *de;
    hotkeyHistoryEntry *h;
    hotkeyLRUNode *node;
    sds ks;

    if (!m || !key_str) return;
    now = time(NULL);
    qps = calculateQPS(count);

    /* If key already in history, update and move to head. */
    de = dictFind(m->history_dict, key_str);
    if (de) {
        node = dictGetVal(de);
        if (!node || !node->entry) return;
        h = node->entry;
        if (h->peak_qps < qps) h->peak_qps = qps;
        h->last_detected = now;
        h->duration += server.hotkey_window_seconds;
        h->val_type = val_type;
        h->slot = slot;
        hotkeyLRUMoveToHead(m->history_lru, node);
        return;
    }

    /* Evict oldest entries if we are at capacity. */
    while (m->history_lru->size >= (size_t)server.hotkey_history_max_count) {
        size_t old = m->history_lru->size;
        evictLRUHistoryEntry(m);
        if (m->history_lru->size >= old) break;
    }

    /* Create new history entry. */
    h = zcalloc(sizeof(hotkeyHistoryEntry));
    h->peak_qps = qps;
    h->first_detected = now;
    h->last_detected = now;
    h->is_read = is_read;
    h->duration = server.hotkey_window_seconds;
    h->val_type = val_type;
    h->slot = slot;

    ks = sdsnew(key_str);
    node = hotkeyLRUAddToHead(m->history_lru, ks, h);
    if (dictAdd(m->history_dict, ks, node) != DICT_OK) {
        /* Dict add failed — undo the LRU insertion. */
        if (m->history_lru->head == node) {
            m->history_lru->head = node->next;
            if (node->next)
                node->next->prev = NULL;
            else
                m->history_lru->tail = NULL;
            m->history_lru->size--;
        }
        sdsfree(ks);
        zfree(h);
        zfree(node);
    }
}

/* Flush all per-slot MG summaries into the global history.
 * Called periodically from serverCron. */
void addHotkeyToHistory(hotkeyManager *m) {
    int slot, i;
    hotkeyMGSummary *rs, *ws;

    if (!m) return;

    for (slot = 0; slot < HOTKEY_SLOTS; slot++) {
        rs = m->read_summaries[slot];
        if (rs && rs->size > 0) {
            for (i = 0; i < rs->size; i++) {
                if (!rs->entries[i].key) continue;
                addSingleToHistory(m, rs->entries[i].key,
                                   rs->entries[i].count,
                                   rs->entries[i].val_type, 1, slot);
                server.hotkey_runtime_read_count++;
            }
        }
        ws = m->write_summaries[slot];
        if (ws && ws->size > 0) {
            for (i = 0; i < ws->size; i++) {
                if (!ws->entries[i].key) continue;
                addSingleToHistory(m, ws->entries[i].key,
                                   ws->entries[i].count,
                                   ws->entries[i].val_type, 0, slot);
                server.hotkey_runtime_write_count++;
            }
        }
    }

    server.hotkey_runtime_history_count = m->history_lru->size;
}

/* Remove history entries whose last_detected time is older than the TTL. */
void expireHotkeyHistory(hotkeyManager *m) {
    time_t expire;
    hotkeyLRUNode *cur, *prev;

    if (!m || !m->history_lru || !m->history_dict) return;
    expire = time(NULL) - server.hotkey_history_ttl;
    cur = m->history_lru->tail;

    while (cur) {
        prev = cur->prev;
        if (cur->entry && cur->entry->last_detected < expire) {
            /* Unlink node. */
            if (cur->prev)
                cur->prev->next = cur->next;
            else
                m->history_lru->head = cur->next;
            if (cur->next)
                cur->next->prev = cur->prev;
            else
                m->history_lru->tail = cur->prev;
            m->history_lru->size--;
            if (cur->key) dictDelete(m->history_dict, cur->key);
        } else {
            break;
        }
        cur = prev;
    }
    server.hotkey_runtime_history_count = m->history_lru->size;
}

/* ===========================================================================
 * HOTKEYS commands
 * ==========================================================================*/

void hotkeysGetCommand(client *c) {
    int filter_type, filter_slot, count;
    hotkeyLRUNode *cur;

    if (!server.hotkey_enabled) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }
    if (!server.hotkey_manager || !server.hotkey_manager->history_lru) {
        addReplyArrayLen(c, 0);
        return;
    }
    if (!parseHotkeyFilterArgs(c, 2, &filter_type, &filter_slot)) return;

    expireHotkeyHistory(server.hotkey_manager);
    if (!server.hotkey_manager || !server.hotkey_manager->history_lru) {
        addReplyArrayLen(c, 0);
        return;
    }

    /* Count matching entries. */
    count = 0;
    cur = server.hotkey_manager->history_lru->head;
    while (cur) {
        if (matchesFilter(cur->entry, filter_type, filter_slot)) count++;
        cur = cur->next;
    }

    /* Reply with matching entries. */
    addReplyArrayLen(c, count);
    cur = server.hotkey_manager->history_lru->head;
    while (cur) {
        if (cur->entry && cur->key &&
            matchesFilter(cur->entry, filter_type, filter_slot)) {
            replyWithHotkeyEntry(c, cur);
        }
        cur = cur->next;
    }
}

void hotkeysResetCommand(client *c) {
    if (!server.hotkey_enabled) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }
    if (server.hotkey_manager) {
        if (server.hotkey_manager->history_dict)
            dictEmpty(server.hotkey_manager->history_dict, NULL);
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

void hotkeysPurgeCommand(client *c) {
    long long slot;

    if (!server.hotkey_enabled) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }
    if (c->argc != 4 || strcasecmp(objectGetVal(c->argv[2]), "SLOT")) {
        addReplyError(c, "Syntax error. Usage: HOTKEYS PURGE SLOT <slot>");
        return;
    }
    if (getLongLongFromObject(c->argv[3], &slot) != C_OK ||
        slot < 0 || slot >= HOTKEY_SLOTS) {
        addReplyErrorFormat(c, "Invalid slot number. Must be 0-%d",
                           HOTKEY_SLOTS - 1);
        return;
    }
    hotkeyPurgeSlot((int)slot);
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
    else if (!strcasecmp(subcmd, "purge"))
        hotkeysPurgeCommand(c);
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
 * Slot purge (triggered on cluster slot migration / flush / reset)
 * ==========================================================================*/

/* Purge all detection state and history entries for a single slot. */
void hotkeyPurgeSlot(int slot) {
    hotkeyManager *m = server.hotkey_manager;
    hotkeyLRUNode *cur, *next;

    if (!m) return;

    /* Free per-slot summaries. */
    if (m->read_summaries[slot]) {
        hotkeyMGSummaryFree(m->read_summaries[slot]);
        m->read_summaries[slot] = NULL;
    }
    if (m->write_summaries[slot]) {
        hotkeyMGSummaryFree(m->write_summaries[slot]);
        m->write_summaries[slot] = NULL;
    }

    /* Remove history entries belonging to this slot. */
    if (m->history_lru && m->history_dict) {
        cur = m->history_lru->head;
        while (cur) {
            next = cur->next;
            if (cur->entry && cur->entry->slot == slot) {
                if (cur->prev)
                    cur->prev->next = cur->next;
                else
                    m->history_lru->head = cur->next;
                if (cur->next)
                    cur->next->prev = cur->prev;
                else
                    m->history_lru->tail = cur->prev;
                m->history_lru->size--;
                if (cur->key) dictDelete(m->history_dict, cur->key);
            }
            cur = next;
        }
    }
    server.hotkey_runtime_history_count =
        m->history_lru ? m->history_lru->size : 0;
}

/* Purge all detection state across all slots. */
void hotkeyPurgeAll(void) {
    hotkeyManager *m = server.hotkey_manager;
    int i;

    if (!m) return;

    for (i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->read_summaries[i]) {
            hotkeyMGSummaryFree(m->read_summaries[i]);
            m->read_summaries[i] = NULL;
        }
        if (m->write_summaries[i]) {
            hotkeyMGSummaryFree(m->write_summaries[i]);
            m->write_summaries[i] = NULL;
        }
    }
    if (m->history_dict) dictEmpty(m->history_dict, NULL);
    if (m->history_lru) {
        m->history_lru->head = NULL;
        m->history_lru->tail = NULL;
        m->history_lru->size = 0;
    }
    server.hotkey_runtime_history_count = 0;
}
