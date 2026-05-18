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


/* ===========================================================================
 * Misra-Gries summary operations
 * ==========================================================================*/

hotkeyMGSummary *hotkeyMGSummaryNew(int max_keys) {
    hotkeyMGSummary *s = zcalloc(sizeof(hotkeyMGSummary));
    s->keys = zcalloc(max_keys * sizeof(sds));
    s->counters = zcalloc(max_keys * sizeof(uint64_t));
    s->decrements = zcalloc(max_keys * sizeof(uint64_t));
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
    zfree(s->counters);
    zfree(s->decrements);
    zfree(s);
}

static void hotkeyMGSummaryReset(hotkeyMGSummary *s) {
    int i;

    if (!s) return;
    for (i = 0; i < s->size; i++) {
        if (s->keys[i]) sdsfree(s->keys[i]);
        s->keys[i] = NULL;
        s->counters[i] = 0;
        s->decrements[i] = 0;
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
static void hotkeyMGSummaryAdd(hotkeyMGSummary *s, robj *key) {
    const char *k;
    size_t klen;
    int i, empty_slot;

    if (!s || !key || !objectGetVal(key)) return;

    s->total++;
    k = objectGetVal(key);
    klen = sdslen(objectGetVal(key));

    /* Case 1: key already tracked — increment its access counter.
     * Also track first empty slot we pass for potential insertion. */
    empty_slot = -1;
    for (i = 0; i < s->size; i++) {
        if (!s->keys[i]) {
            if (empty_slot == -1) empty_slot = i;
            continue;
        }
        if (sdslen(s->keys[i]) == klen &&
            memcmp(s->keys[i], k, klen) == 0) {
            s->counters[i]++;
            return;
        }
    }

    /* Case 2: empty slot available. */
    if (s->size < s->max_keys) {
        s->keys[s->size] = sdsnewlen(k, klen);
        s->counters[s->size] = 1;
        s->decrements[s->size] = 0;
        s->size++;
        return;
    }
    if (empty_slot != -1) {
        s->keys[empty_slot] = sdsnewlen(k, klen);
        s->counters[empty_slot] = 1;
        s->decrements[empty_slot] = 0;
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
    hotkeyManager *m;

    UNUSED(max_keys);
    m = zcalloc(sizeof(hotkeyManager));
    return m;
}

void hotkeyManagerFree(hotkeyManager *m) {
    int i;

    if (!m) return;
    for (i = 0; i < HOTKEY_SLOTS; i++) {
        if (m->read_summaries[i]) hotkeyMGSummaryFree(m->read_summaries[i]);
        if (m->write_summaries[i]) hotkeyMGSummaryFree(m->write_summaries[i]);
    }
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

    UNUSED(val_type);
    if (!m || !key) return;
    if (slot < 0 || slot >= HOTKEY_SLOTS) return;
    server.hotkey_runtime_total_sampled++;
    if (!m->read_summaries[slot])
        m->read_summaries[slot] = hotkeyMGSummaryNew(server.hotkey_max_keys);
    hotkeyMGSummaryAdd(m->read_summaries[slot], key);
}

void writeHotKeyDetection(robj *key, int val_type, int slot) {
    hotkeyManager *m = server.hotkey_manager;

    UNUSED(val_type);
    if (!m || !key) return;
    if (slot < 0 || slot >= HOTKEY_SLOTS) return;
    server.hotkey_runtime_total_sampled++;
    if (!m->write_summaries[slot])
        m->write_summaries[slot] = hotkeyMGSummaryNew(server.hotkey_max_keys);
    hotkeyMGSummaryAdd(m->write_summaries[slot], key);
}


/* ===========================================================================
 * HOTKEYS commands
 * ==========================================================================*/

/* Entry used for collecting and sorting active MG slots. */
typedef struct {
    sds key;
    uint64_t count;
    int slot;
    int is_read;
} hotkeyMGCollected;

static int hotkeyMGCollectedCmp(const void *a, const void *b) {
    const hotkeyMGCollected *ea = a;
    const hotkeyMGCollected *eb = b;
    if (eb->count > ea->count) return 1;
    if (eb->count < ea->count) return -1;
    return 0;
}

void hotkeysGetCommand(client *c) {
    int filter_type, filter_slot, slot, i, n, limit;
    hotkeyManager *m;
    hotkeyMGSummary *s;
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
    if (!parseHotkeyFilterArgs(c, 2, &filter_type, &filter_slot)) return;

    /* First pass: count active entries to allocate exactly. */
    count = 0;
    for (slot = 0; slot < HOTKEY_SLOTS; slot++) {
        if (filter_slot != -1 && slot != filter_slot) continue;
        if (filter_type == -1 || filter_type == 1) {
            s = m->read_summaries[slot];
            if (s) {
                for (i = 0; i < s->size; i++)
                    if (s->keys[i]) count++;
            }
        }
        if (filter_type == -1 || filter_type == 0) {
            s = m->write_summaries[slot];
            if (s) {
                for (i = 0; i < s->size; i++)
                    if (s->keys[i]) count++;
            }
        }
    }

    if (count == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    collected = zmalloc(count * sizeof(hotkeyMGCollected));
    n = 0;

    for (slot = 0; slot < HOTKEY_SLOTS; slot++) {
        if (filter_slot != -1 && slot != filter_slot) continue;

        /* Read summaries. */
        if (filter_type == -1 || filter_type == 1) {
            s = m->read_summaries[slot];
            if (s) {
                for (i = 0; i < s->size; i++) {
                    if (!s->keys[i]) continue;
                    collected[n].key = s->keys[i];
                    collected[n].count = s->counters[i];
                    collected[n].slot = slot;
                    collected[n].is_read = 1;
                    n++;
                }
            }
        }

        /* Write summaries. */
        if (filter_type == -1 || filter_type == 0) {
            s = m->write_summaries[slot];
            if (s) {
                for (i = 0; i < s->size; i++) {
                    if (!s->keys[i]) continue;
                    collected[n].key = s->keys[i];
                    collected[n].count = s->counters[i];
                    collected[n].slot = slot;
                    collected[n].is_read = 0;
                    n++;
                }
            }
        }
    }

    /* Sort by counter descending. */
    if (n > 0) qsort(collected, n, sizeof(hotkeyMGCollected), hotkeyMGCollectedCmp);

    /* Return top K results. */
    limit = n < server.hotkey_max_keys ? n : server.hotkey_max_keys;
    addReplyArrayLen(c, limit);
    for (i = 0; i < limit; i++) {
        addReplyArrayLen(c, 8);
        addReplyBulkCString(c, "key");
        addReplyBulkCBuffer(c, collected[i].key, sdslen(collected[i].key));
        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, collected[i].is_read ? "read" : "write");
        addReplyBulkCString(c, "slot");
        addReplyLongLong(c, collected[i].slot);
        addReplyBulkCString(c, "count");
        addReplyLongLong(c, collected[i].count);
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

void hotkeyPurgeSlot(int slot) {
    hotkeyManager *m = server.hotkey_manager;

    if (!m) return;

    if (m->read_summaries[slot]) {
        hotkeyMGSummaryFree(m->read_summaries[slot]);
        m->read_summaries[slot] = NULL;
    }
    if (m->write_summaries[slot]) {
        hotkeyMGSummaryFree(m->write_summaries[slot]);
        m->write_summaries[slot] = NULL;
    }
}

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
}
