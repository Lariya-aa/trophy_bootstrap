/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bootstrap.h"
#include "sha256.h"
#ifdef TB_VITA
#include <libk/string.h>
#include <libk/stdio.h>
#else
#include <string.h>
#include <stdio.h>
#endif
static int digit(char c) {
    return c >= '0' && c <= '9';
}
int tb_hex_valid(const char * s, size_t n) {
    if (strlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++) if (!digit(s[i]) &&(s[i] < 'a' || s[i] > 'f')) return 0;
    return 1;
}
int tb_npwr_valid(const char * s) {
    if (strlen(s) != 12 || memcmp(s, "NPWR", 4) || s[9] != '_') return 0;
    for (unsigned i = 4; i < 12; i++) if (i != 9 && !digit(s[i])) return 0;
    return 1;
}
static int title_valid(const char * s) {
    if (strlen(s) != 9 || !memcmp(s, "NPXS", 4)) return 0;
    for (unsigned i = 0; i < 9; i++) if (!digit(s[i]) &&(s[i] < 'A' || s[i] > 'Z')) return 0;
    return 1;
}
static int line(char * * cursor, char * * out) {
    if (!* * cursor) return 0;
    * out = * cursor;
    char * end = strchr(* cursor, '\n');
    if (!end) return - 1;
    * end = 0;
    * cursor = end + 1;
    return 1;
}
int tb_queue_parse(const char * bytes, size_t size, TBQueue * q) {
    if (!size || size >= TB_QUEUE_BYTES || memchr(bytes, 0, size) || memchr(bytes, '\r', size)) return TB_ERROR;
    char copy[TB_QUEUE_BYTES], * p = copy, * s;
    memcpy(copy, bytes, size);
    copy[size] = 0;
    memset(q, 0, sizeof(* q));
    if (line(& p, & s) != 1 || strcmp(s, "TROPHY_BOOTSTRAP_QUEUE_V1")) return TB_ERROR;
    if (line(& p, & s) != 1 || strncmp(s, "batch=", 6) || !tb_hex_valid(s + 6, 32)) return TB_ERROR;
    memcpy(q->batch, s + 6, 33);
    if (line(& p, & s) != 1 || strncmp(s, "user=", 5) || strlen(s + 5) != 2 || !digit(s[5]) || !digit(s[6])) return TB_ERROR;
    memcpy(q->user, s + 5, 3);
    if (line(& p, & s) != 1) return TB_ERROR;
    q->min_ms = 5000;
    if (!strcmp(s, "window_ms=5000,10000")) q->max_ms = 10000;
    else if (!strcmp(s, "window_ms=5000,30000")) q->max_ms = 30000;
    else return TB_ERROR;
    int r;
    while ((r = line(& p, & s)) == 1) {
        if (q->count == TB_MAX) return TB_ERROR;
        TBItem * it = & q->items[q->count];
        int used = 0;
        if (sscanf(s, "%9s %12s%n", it->title, it->npwr, & used) != 2 || s[used] || !title_valid(it->title) || !tb_npwr_valid(it->npwr)) return TB_ERROR;
        for (unsigned i = 0; i < q->count; i++) if (!strcmp(q->items[i].title, it->title) || !strcmp(q->items[i].npwr, it->npwr)) return TB_ERROR;
        q->count++;
    }
    return r < 0 || !q->count ? TB_ERROR : 0;
}
static int save(TBState * s, const TBOps * o, void * c) {
    s->sequence++;
    trophy_sha256(s, offsetof(TBState, checksum), s->checksum);
    if (o->save(c, s) < 0) {
        s->phase = TB_PAUSED;
        s->error = TB_STORAGE;
        return TB_STORAGE;
    }
    return 0;
}
static int phase(TBState * s, unsigned p, uint64_t now, const TBOps * o, void * c) {
    s->phase = p;
    s->phase_at = now;
    return save(s, o, c);
}
static int pause_at(TBState * s, int error, uint64_t now, const TBOps * o, void * c) {
    s->error = error;
    int r = phase(s, TB_PAUSED, now, o, c);
    return r < 0 ? r : error;
}
int tb_state_valid(const TBState * s) {
    char hash[65];
    if (s->magic != TB_MAGIC || s->version != 1 || !s->sequence || s->queue_sha[64] || s->batch[32] || s->user[2] || s->checksum[64] || !tb_hex_valid(s->queue_sha, 64) || !tb_hex_valid(s->batch, 32) || !digit(s->user[0]) || !digit(s->user[1]) || !s->count || s->count > TB_MAX || s->index > s->count || s->phase > TB_PAUSED || s->owned > 1 || s->cancel > 1 || s->halt > 1) return 0;
    for (unsigned i = 0; i < s->count; i++) if (s->results[i] > TB_RECOVERED_PAIR) return 0;
    if (s->index == s->count && s->phase != TB_GENERATED && s->phase != TB_CANCELLED && s->phase != TB_PAUSED) return 0;
    if (s->owned && s->pid <= 0) return 0;
    trophy_sha256(s, offsetof(TBState, checksum), hash);
    return !strcmp(hash, s->checksum);
}
int tb_terminal(const TBState * s) {
    return s->phase == TB_GENERATED || s->phase == TB_CANCELLED;
}
const char * tb_phase_name(unsigned p) {
    static const char * n[] = {
        "IDLE", "PREFLIGHT", "LAUNCH", "WAIT_PROCESS", "RUN_WINDOW", "STOP", "WAIT_EXIT", "CHECK_PAIR", "COOLDOWN", "RECOVER_WAIT", "BATCH_GENERATED", "CANCELLED", "PAUSED"
    };
    return p <= TB_PAUSED ? n[p] : "INVALID";
}
const char * tb_result_name(unsigned p) {
    static const char * n[] = {
        "pending", "existing_skipped", "pair_generated_waiting_decrypted_export", "missing_pair", "incomplete_pair", "launch_failed", "cancelled", "pair_recovered_waiting_decrypted_export"
    };
    return p <= TB_RECOVERED_PAIR ? n[p] : "invalid";
}
int tb_start(TBState * s, const TBQueue * q, const char * hash, uint64_t now, const TBOps * o, void * c) {
    if (!q->count || q->count > TB_MAX || !tb_hex_valid(hash, 64)) return TB_ERROR;
    uint64_t previous = tb_state_valid(s) ? s->sequence : 0;
    memset(s, 0, sizeof(* s));
    s->sequence = previous;
    s->magic = TB_MAGIC;
    s->version = 1;
    s->count = q->count;
    s->pid = - 1;
    strcpy(s->queue_sha, hash);
    strcpy(s->batch, q->batch);
    strcpy(s->user, q->user);
    return phase(s, TB_PREFLIGHT, now, o, c);
}
int tb_resume(TBState * s, const TBQueue * q, uint64_t now, const TBOps * o, void * c) {
    if (s->count != q->count || strcmp(s->batch, q->batch) || strcmp(s->user, q->user) || tb_terminal(s)) return TB_ERROR;
    /* Prior process IDs are never authority after a plugin restart. */
    s->owned = 0;
    s->pid = - 1;
    s->ready_at = 0;
    s->error = 0;
    s->halt = 0;
    return phase(s, TB_RECOVER_WAIT, now, o, c);
}
static int advance(TBState * s, unsigned result, uint64_t now, const TBOps * o, void * c) {
    s->results[s->index] = result;
    s->index++;
    s->owned = 0;
    s->pid = - 1;
    s->ready_at = 0;
    if (s->cancel) return phase(s, TB_CANCELLED, now, o, c);
    if (s->halt) return phase(s, TB_PAUSED, now, o, c);
    return phase(s, s->index == s->count ? TB_GENERATED : TB_COOLDOWN, now, o, c);
}
int tb_step(TBState * s, const TBQueue * q, uint64_t now, int cancel, const TBOps * o, void * c) {
    if (s->count != q->count || s->index > s->count || strcmp(s->batch, q->batch)) return TB_ERROR;
    if (tb_terminal(s)) return 0;
    if (cancel && !s->cancel) {
        s->cancel = 1;
        if (save(s, o, c) < 0) return TB_STORAGE;
    }
    if (s->index == s->count) return phase(s, s->cancel ? TB_CANCELLED : TB_GENERATED, now, o, c);
    const TBItem * it = & q->items[s->index];
    int32_t pid = - 1;
    int r, exists;
    if (s->cancel &&(s->phase == TB_PREFLIGHT || s->phase == TB_LAUNCH || s->phase == TB_COOLDOWN)) return phase(s, TB_CANCELLED, now, o, c);
    if (s->phase == TB_PAUSED) {
        if (s->cancel && s->owned) return phase(s, TB_STOP, now, o, c);
        if (s->cancel) {
            r = o->process(c, it->title, & pid);
            if (r < 0) return r;
            if (r) return TB_BUSY;
            return phase(s, TB_CANCELLED, now, o, c);
        }
        return 0;
    }
    switch (s->phase) {
        case TB_PREFLIGHT :
        case TB_LAUNCH :
        exists = o->pair(c, q->user, it->npwr);
        if (exists < 0) return pause_at(s, exists, now, o, c);
        if (exists) return advance(s, exists == 1 ? TB_EXISTING : TB_INCOMPLETE_PAIR, now, o, c);
        /* Defer directory scans while another application owns the slot. */
        r = o->process(c, it->title, & pid);
        if (r < 0) return pause_at(s, r, now, o, c);
        if (r) return pause_at(s, TB_BUSY, now, o, c);
        r = o->busy(c, - 1);
        if (r < 0) return pause_at(s, r, now, o, c);
        if (r) return 0;
        r = o->registered(c, q->user);
        if (r < 0) return pause_at(s, r, now, o, c);
        if (r >= TB_MAX) return pause_at(s, TB_CAPACITY, now, o, c);
        int registered = r;
        r = o->game(c, it);
        if (r <= 0) {
            s->error = r < 0 ? r : TB_ERROR;
            return advance(s, TB_LAUNCH_FAILED, now, o, c);
        }
        if (r > TB_MAX - registered) return pause_at(s, TB_CAPACITY, now, o, c);
        if (s->phase == TB_PREFLIGHT) return phase(s, TB_LAUNCH, now, o, c);
        /* LAUNCH journal is durable before the first process side effect. */
        r = o->launch(c, it->title);
        if (r < 0) {
            s->error = r;
            return advance(s, TB_LAUNCH_FAILED, now, o, c);
        }
        return phase(s, TB_WAIT_PROCESS, now, o, c);
        case TB_WAIT_PROCESS :
        r = o->process(c, it->title, & pid);
        if (r < 0) return pause_at(s, r, now, o, c);
        /* Initial matching PID can be replaced during game startup. Ownership
           requires an uninterrupted stable observation, never a RUN-time rebind. */
        if (now - s->phase_at >= 30000) return pause_at(s, TB_TIMEOUT, now, o, c);
        if (r != TB_PROCESS_RUNNING) {
            s->pid = -1;
            s->ready_at = 0;
            return 0;
        }
        if (pid != s->pid || !s->ready_at) {
            s->pid = pid;
            s->ready_at = now;
            return 0;
        }
        if (now - s->ready_at < 1000) return 0;
        s->owned = 1;
        s->ready_at = 0;
        return phase(s, s->cancel ? TB_STOP : TB_RUN, now, o, c);
        case TB_RUN :
        r = o->process(c, it->title, & pid);
        if (r < 0) return pause_at(s, r, now, o, c);
        if (!r) {
            s->owned = 0;
            return phase(s, TB_CHECK_PAIR, now, o, c);
        }
        if (r != TB_PROCESS_RUNNING || pid != s->pid) {
            s->owned = 0;
            return pause_at(s, TB_IDENTITY, now, o, c);
        }
        r = o->busy(c, s->pid);
        if (r < 0 || r > 0) {
            s->halt = 1;
            s->error = r < 0 ? r : TB_BUSY;
            return phase(s, TB_STOP, now, o, c);
        }
        exists = o->pair(c, q->user, it->npwr);
        if (exists == 1) {
            if (!s->ready_at) s->ready_at = now;
        }
        else s->ready_at = 0;
        if (exists < 0) {
            s->halt = 1;
            s->error = exists;
            return phase(s, TB_STOP, now, o, c);
        }
        if (s->cancel || now - s->phase_at >= q->max_ms ||(now - s->phase_at >= q->min_ms && s->ready_at && now - s->ready_at >= 500)) return phase(s, TB_STOP, now, o, c);
        return 0;
        case TB_STOP :
        r = o->process(c, it->title, & pid);
        if (r < 0) return pause_at(s, r, now, o, c);
        if (!r) {
            s->owned = 0;
            return phase(s, TB_CHECK_PAIR, now, o, c);
        }
        if (r != TB_PROCESS_RUNNING || !s->owned || pid != s->pid) {
            s->owned = 0;
            return pause_at(s, TB_IDENTITY, now, o, c);
        }
        r = o->stop(c, it->title, s->pid);
        if (r < 0) return pause_at(s, r, now, o, c);
        return phase(s, TB_WAIT_EXIT, now, o, c);
        case TB_WAIT_EXIT :
        r = o->process(c, it->title, & pid);
        if (r < 0) return pause_at(s, r, now, o, c);
        if (!r) {
            s->owned = 0;
            return phase(s, TB_CHECK_PAIR, now, o, c);
        }
        if (pid != s->pid) {
            s->owned = 0;
            return pause_at(s, TB_IDENTITY, now, o, c);
        }
        if (now - s->phase_at >= 10000) return pause_at(s, TB_TIMEOUT, now, o, c);
        return 0;
        case TB_CHECK_PAIR :
        if (now - s->phase_at < 1500) return 0;
        exists = o->pair(c, q->user, it->npwr);
        if (exists < 0) return pause_at(s, exists, now, o, c);
        return advance(s, exists == 1 ? TB_PAIR_CREATED :(s->cancel ? TB_ITEM_CANCELLED :(exists == 2 ? TB_INCOMPLETE_PAIR : TB_MISSING_PAIR)), now, o, c);
        case TB_COOLDOWN :
        if (now - s->phase_at < 1500) return 0;
        return phase(s, TB_PREFLIGHT, now, o, c);
        case TB_RECOVER_WAIT :
        r = o->process(c, it->title, & pid);
        if (r < 0) return pause_at(s, r, now, o, c);
        if (r) return pause_at(s, TB_BUSY, now, o, c);
        if (now - s->phase_at < 3000) return 0;
        exists = o->pair(c, q->user, it->npwr);
        if (exists < 0) return pause_at(s, exists, now, o, c);
        if (exists) return advance(s, exists == 1 ? TB_RECOVERED_PAIR : TB_INCOMPLETE_PAIR, now, o, c);
        return phase(s, s->cancel ? TB_CANCELLED : TB_PREFLIGHT, now, o, c);
        default :
        return TB_ERROR;
    }
}
