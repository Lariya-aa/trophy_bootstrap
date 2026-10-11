/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <libk/stdio.h>
#include <libk/string.h>
#include "bootstrap.h"
#include "app_list.h"
#include "app_status.h"
extern int _sceAppMgrGetStatusById(int32_t pid, TBAppStatus *status);
static int running_apps(int32_t ids[TB_APP_LIST_MAX]);
#include "sha256.h"
#define ROOT "ux0:data/trophy_bootstrap"
#define NOENT ((int)0x80010002)
#define PROCESS_NOT_FOUND ((int)0x80802012)
#define MODE_DIR 0706
#define MODE_FILE 0606
static volatile int quitting;
static SceUID thread = - 1, self_pid = - 1;
static char session[33], queue_bytes[TB_QUEUE_BYTES];
static size_t queue_length;
static TBQueue queue;
static TBState state;
static int have_state, active, storage_bad, last_error;
static const TBOps ops;
static const char *last_operation = "none";
static char waiting_for[32];
static int status_dirty;
static int error_at(const char *operation, int result) {
    if (result < 0 && result != NOENT && result != PROCESS_NOT_FOUND)
        last_operation = operation;
    return result;
}
static uint64_t now_ms(void) {
    return sceKernelGetSystemTimeWide() / 1000;
}
static int read_file(const char * path, void * out, size_t cap, size_t * length) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return fd;
    SceOff n = sceIoLseek(fd, 0, SCE_SEEK_END);
    int r = 0;
    if (n < 0 ||(uint64_t) n > cap || sceIoLseek(fd, 0, SCE_SEEK_SET) < 0) r = TB_ERROR;
    size_t done = 0;
    while (!r && done <(size_t) n) {
        int k = sceIoRead(fd,(char *) out + done,(size_t) n - done);
        if (k <= 0) r = k < 0 ? k : TB_STORAGE;
        else done +=(size_t) k;
    }
    char extra;
    if (!r && sceIoRead(fd, & extra, 1) != 0) r = TB_STORAGE;
    int close = sceIoClose(fd);
    if (!r && close < 0) r = close;
    if (!r) * length = done;
    return r;
}
static int directory(const char * path) {
    SceIoStat st;
    int r = sceIoGetstat(path, & st);
    if (r >= 0) return SCE_S_ISDIR(st.st_mode) ? 0 : TB_ERROR;
    if (r != NOENT) return r;
    return sceIoMkdir(path, MODE_DIR);
}
static int prepare(void) {
    int r = directory("ux0:data");
    if (!r) r = directory(ROOT);
    if (!r) r = directory(ROOT "/jobs");
    return r;
}
static int write_atomic(const char * path, const void * data, size_t n) {
    if (n > 16384) return TB_ERROR;
    char temp[240];
    int k = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (k < 0 || k >=(int) sizeof(temp)) return TB_ERROR;
    int r = sceIoRemove(temp);
    if (r < 0 && r != NOENT) return r;
    SceUID fd = sceIoOpen(temp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, MODE_FILE);
    if (fd < 0) return fd;
    size_t done = 0;
    while (done < n) {
        int wrote = sceIoWrite(fd,(char *) data + done, n - done);
        if (wrote <= 0) {
            r = wrote < 0 ? wrote : TB_STORAGE;
            break;
        }
        done +=(size_t) wrote;
    }
    if (done == n) r = sceIoSyncByFd(fd, 0);
    int close = sceIoClose(fd);
    if (r >= 0 && close < 0) r = close;
    if (r < 0) return r;
    char check[16384];
    size_t used = 0;
    r = read_file(temp, check, sizeof(check), & used);
    if (r < 0) return r;
    if (used != n || memcmp(check, data, n)) return TB_STORAGE;
    /* Journal slots alternate; the other valid slot remains if rename is interrupted. */
    r = sceIoRemove(path);
    if (r < 0 && r != NOENT) return r;
    r = sceIoRename(temp, path);
    if (r >= 0) r = sceIoSync("ux0:", 0);
    return r;
}
static int append_log(const char * message) {
    SceUID fd = sceIoOpen(ROOT "/diagnostic.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, MODE_FILE);
    if (fd < 0) return fd;
    size_t size = strlen(message), done = 0;
    int r = 0;
    while (done < size) {
        int n = sceIoWrite(fd, message + done, size - done);
        if (n <= 0) {
            r = n < 0 ? n : TB_STORAGE;
            break;
        }
        done +=(size_t) n;
    }
    if (!r) r = sceIoSyncByFd(fd, 0);
    int closed = sceIoClose(fd);
    return r < 0 ? r :(closed < 0 ? closed : 0);
}
static int status_write(void) {
    char b[512];
    int reported_error = last_error < 0 ? last_error : (have_state ? state.error : 0);
    int n = snprintf(b, sizeof(b), "{\"schema\":1,\"plugin\":\"Trophy Bootstrap 0.1.0\",\"session\":\"%s\",\"phase\":\"%s\",\"active\":%s,\"needs_explicit_resume\":%s,\"batch\":\"%s\",\"queue_sha256\":\"%s\",\"index\":%u,\"count\":%u,\"error\":%d,\"operation\":\"%s\",\"waiting_for\":\"%s\",\"storage_error\":%s}\n", session, have_state ? tb_phase_name(state.phase) : "IDLE", active ? "true" : "false", have_state && !active && !tb_terminal(& state) ? "true" : "false", have_state ? state.batch : "", have_state ? state.queue_sha : "", have_state ? state.index : 0, have_state ? state.count : 0, reported_error, last_operation, waiting_for, storage_bad ? "true" : "false");
    return n < 0 || n >=(int) sizeof(b) ? TB_ERROR : write_atomic(ROOT "/status.json", b,(size_t) n);
}
static int result_write(const TBState * s) {
    char path[160], b[12288];
    int used = snprintf(b, sizeof(b), "{\"schema\":1,\"batch\":\"%s\",\"queue_sha256\":\"%s\",\"user\":\"%s\",\"phase\":\"%s\",\"DAT_modified_by_plugin\":false,\"decrypted_export_verified\":false,\"official_sync_verified\":false,\"items\":[", s->batch, s->queue_sha, s->user, tb_phase_name(s->phase));
    for (unsigned i = 0; i < s->count; i++) {
        if (used < 0 || used >=(int) sizeof(b)) return TB_ERROR;
        int n = snprintf(b + used, sizeof(b) - used, "%s{\"title_id\":\"%s\",\"npwr\":\"%s\",\"result\":\"%s\"}", i ? "," : "", queue.items[i].title, queue.items[i].npwr, tb_result_name(s->results[i]));
        if (n < 0 || n >=(int)(sizeof(b) - used)) return TB_ERROR;
        used += n;
    }
    if (used + 4 >=(int) sizeof(b)) return TB_ERROR;
    memcpy(b + used, "]}\n", 4);
    used += 3;
    snprintf(path, sizeof(path), ROOT "/jobs/%s/result.json", s->batch);
    return write_atomic(path, b,(size_t) used);
}
static int journal_save(void * ctx, const TBState * s) {
    char path[96], line[192];
    snprintf(path, sizeof(path), ROOT "/state-%u.bin",(unsigned)(s->sequence & 1));
    int r = write_atomic(path, s, sizeof(* s));
    if (r < 0) return r;
    snprintf(line, sizeof(line), "seq_hex=%08x%08x phase=%s item=%u/%u owned=%u pid=%d error=%08X operation=%s\n",(unsigned)(s->sequence >> 32),(unsigned) s->sequence, tb_phase_name(s->phase), s->index, s->count, s->owned, s->pid,(unsigned) s->error, last_operation);
    r = append_log(line);
    if (r < 0) return r;
    if (tb_terminal(s)) {
        r = result_write(s);
        if (r < 0) return r;
    }
    return 0;
}
static int pair(void * c, const char * user, const char * npwr) {
    char path[160];
    SceIoStat st;
    snprintf(path, sizeof(path), "ur0:user/%s/trophy/data/%s", user, npwr);
    int r = error_at("pair.getstat", sceIoGetstat(path, & st));
    if (r == NOENT) return 0;
    if (r < 0) return r;
    if (!SCE_S_ISDIR(st.st_mode)) return TB_ERROR;
    int complete = 0;
    const char * names[] = {
        "TRPTITLE.DAT", "TRPTRANS.DAT"
    };
    for (unsigned i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "ur0:user/%s/trophy/data/%s/%s", user, npwr, names[i]);
        r = error_at("pair.getstat", sceIoGetstat(path, & st));
        if (r == NOENT) continue;
        if (r < 0) return r;
        if (SCE_S_ISREG(st.st_mode) && st.st_size > 0) complete++;
    }
    return complete == 2 ? 1 : 2;
}
static int registered(void * c, const char * user) {
    char path[96];
    snprintf(path, sizeof(path), "ur0:user/%s/trophy/data", user);
    SceUID fd = error_at("registered.open", sceIoDopen(path));
    if (fd < 0) return fd == NOENT ? 0 : fd;
    int count = 0, r;
    SceIoDirent e;
    while (1) {
        memset(& e, 0, sizeof(e));
        r = error_at("registered.read", sceIoDread(fd, & e));
        if (r <= 0) break;
        if (tb_npwr_valid(e.d_name)) {
            if (!SCE_S_ISDIR(e.d_stat.st_mode)) {
                r = TB_ERROR;
                break;
            }
            count++;
        }
    }
    int close = error_at("registered.close", sceIoDclose(fd));
    return r < 0 ? r :(close < 0 ? close : count);
}
static uint32_t le32(const unsigned char * p) {
    return p[0] |(uint32_t) p[1] << 8 |(uint32_t) p[2] << 16 |(uint32_t) p[3] << 24;
}
static unsigned le16(const unsigned char * p) {
    return p[0] |(unsigned) p[1] << 8;
}
static int check_sfo(const TBItem * it) {
    char path[96];
    unsigned char b[4096];
    size_t n = 0;
    snprintf(path, sizeof(path), "ux0:app/%s/sce_sys/param.sfo", it->title);
    int r = error_at("game.read_sfo", read_file(path, b, sizeof(b), & n));
    if (r < 0) return r;
    if (n < 20 || memcmp(b, "\0PSF", 4)) return TB_ERROR;
    uint32_t keys = le32(b + 8), values = le32(b + 12), count = le32(b + 16);
    if (count >(n - 20) / 16 || keys >= n || values >= n) return TB_ERROR;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char * p = b + 20 + 16 * i;
        uint32_t off = le32(p + 12), size = le32(p + 4), capacity = le32(p + 8);
        uint64_t key =(uint64_t) keys + le16(p), value =(uint64_t) values + off;
        if (key >= n || !memchr(b + key, 0, n - key) || value + size > n || size > capacity) return TB_ERROR;
        if (!strcmp((char *) b + key, "TITLE_ID")) return le16(p + 2) == 0x204 && size >= 10 && !memcmp(b + value, it->title, 10) ? 0 : TB_IDENTITY;
    }
    return TB_IDENTITY;
}
static int game(void * c, const TBItem * it) {
    int r = check_sfo(it);
    if (r < 0) return r;
    char seen[100][13], path[176];
    unsigned count = 0;
    int mapped = 0;
    const char * roots[] = {
        "ux0:app", "ux0:patch", "ux0:rePatch"
    };
    for (unsigned root = 0; root < 3; root++) {
        snprintf(path, sizeof(path), "%s/%s/sce_sys/trophy", roots[root], it->title);
        SceUID fd = error_at("game.open", sceIoDopen(path));
        if (fd == NOENT) continue;
        if (fd < 0) return fd;
        SceIoDirent e;
        while (1) {
            memset(& e, 0, sizeof(e));
            r = error_at("game.read", sceIoDread(fd, & e));
            if (r <= 0) break;
            if (!tb_npwr_valid(e.d_name) || !SCE_S_ISDIR(e.d_stat.st_mode)) continue;
            unsigned j = 0;
            for (; j < count; j++) if (!strcmp(seen[j], e.d_name)) break;
            if (j < count) continue;
            if (count == 100) {
                r = TB_CAPACITY;
                break;
            }
            strcpy(seen[count++], e.d_name);
            if (!strcmp(e.d_name, it->npwr)) {
                SceIoStat st;
                snprintf(path, sizeof(path), "%s/%s/sce_sys/trophy/%.12s/TROPHY.TRP", roots[root], it->title, e.d_name);
                int found = error_at("game.trp_stat", sceIoGetstat(path, & st));
                if (found < 0) {
                    r = found;
                    break;
                }
                if (!SCE_S_ISREG(st.st_mode) || st.st_size <= 0) {
                    r = TB_ERROR;
                    break;
                }
                mapped = 1;
            }
        }
        int close = error_at("game.close", sceIoDclose(fd));
        if (r < 0) return r;
        if (close < 0) return close;
    }
    if (!mapped) return TB_IDENTITY;
    int slots = 0;
    for (unsigned i = 0; i < count; i++) {
        r = pair(c, queue.user, seen[i]);
        if (r < 0) return r;
        if (!r) slots++;
    }
    return slots;
}
static int process(void * c, const char * title, int32_t * pid) {
    SceUID id = - 1;
    int r = error_at("process.find", sceAppMgrGetIdByName(& id, title));
    if (r == PROCESS_NOT_FOUND) return 0;
    if (r < 0) return r;
    if (id <= 0) return TB_IDENTITY;
    char name[32] = {
        0
    };
    r = error_at("process.name", sceAppMgrGetNameById(id, name));
    if (r < 0) return r;
    if (strcmp(name, title)) {
        last_operation = "process.title_mismatch";
        return TB_IDENTITY;
    }
    TBAppStatus snapshot;
    memset(&snapshot,0,sizeof(snapshot));
    r = error_at("process.status", _sceAppMgrGetStatusById(id,&snapshot));
    if (r == PROCESS_NOT_FOUND) {
        *pid = id;
        return TB_PROCESS_STARTING; /* A concurrent startup/exit is not absence. */
    }
    if (r < 0) return r;
    int classification = tb_status_classify(&snapshot,id,title);
    static int previous_pid=-1, previous_app=-1, previous_started=-1, previous_type=-1;
    static char previous_title[10];
    if (previous_pid!=id || previous_app!=snapshot.app_id || previous_started!=(int)snapshot.started || previous_type!=snapshot.application_type || strcmp(previous_title,title)) {
        char line[224];
        snprintf(line,sizeof(line),"process title=%s phase=%s observed_pid=%d previous_pid=%d recorded_pid=%d owned=%u status_pid=%d app_id=%d started=%u type=%u\n",title,tb_phase_name(state.phase),id,previous_pid,state.pid,state.owned,snapshot.pid,snapshot.app_id,snapshot.started,(unsigned)snapshot.application_type);
        if (append_log(line)<0) return TB_STORAGE;
        previous_pid=id;previous_app=snapshot.app_id;previous_started=(int)snapshot.started;previous_type=snapshot.application_type;
        snprintf(previous_title,sizeof(previous_title),"%s",title);
    }
    *pid=id;
    if (classification<0) { last_operation="process.status_contract";return classification; }
    if (classification==TB_PROCESS_STARTING) { last_operation="process.not_started_game";return classification; }
    /* Confirm the status App ID/PID is also present in Shell's full app list. */
    SceInt32 ids[TB_APP_LIST_MAX];
    int n=running_apps(ids);
    if (n<0) return n;
    int listed=-1;
    for (int i=0;i<n;i++) {
        if (ids[i]!=snapshot.app_id) continue;
        listed=error_at("process.app_pid",sceAppMgrGetProcessIdByAppIdForShell(ids[i]));
        if (listed<0) return listed;
        break;
    }
    classification=tb_status_binding(&snapshot,listed);
    if (classification!=TB_PROCESS_RUNNING) {last_operation="process.app_pending";return classification;}
    if (state.owned && state.pid!=id) last_operation="process.pid_changed";
    return TB_PROCESS_RUNNING;
}
static int app_query(void *ctx, int32_t *ids, int capacity) {
    static const char *names[] = {"apps.list.1", "apps.list.2", "apps.list.4", "apps.list.8", "apps.list.16", "apps.list.32"};
    static int seen[6], previous[6];
    int index = 0;
    for (int k = capacity; k > 1; k /= 2) index++;
    int result = error_at(names[index], sceAppMgrGetRunningAppIdListForShell(ids, capacity));
    if (!seen[index] || previous[index] != result) {
        char line[112];
        int populated = 0;
        for (int i = 0; i < TB_APP_LIST_MAX; i++) if (ids[i]) populated++;
        snprintf(line, sizeof(line), "apps request=%d result=%08X populated=%d first_id=%d\n", capacity, (unsigned)result, populated, ids[0]);
        int log = append_log(line);
        if (log < 0) return TB_STORAGE;
        seen[index] = 1;
        previous[index] = result;
    }
    return result;
}
static int running_apps(int32_t ids[TB_APP_LIST_MAX]) {
    int result = tb_app_list(app_query, NULL, ids);
    if (result == TB_ERROR || result == TB_IDENTITY || result == TB_CAPACITY)
        last_operation = "apps.list.contract";
    return result;
}
static int busy(void * c, int32_t owned) {
    SceInt32 ids[TB_APP_LIST_MAX];
    int n = running_apps(ids);
    if (n < 0) return n;
    for (int i = 0; i < n; i++) {
        SceUID pid = error_at("apps.pid", sceAppMgrGetProcessIdByAppIdForShell(ids[i]));
        if (pid < 0) return pid;
        if (pid == 0 || pid == self_pid || pid == owned) continue;
        char name[32] = {
            0
        };
        int r = error_at("apps.name", sceAppMgrGetNameById(pid, name));
        if (r < 0) return r;
        /* Read-only ABI/permission check on the blocking app before a game
           is launched. The name/PID/App ID are independently known here. */
        TBAppStatus foreign;
        memset(&foreign,0,sizeof(foreign));
        r = error_at("apps.status",_sceAppMgrGetStatusById(pid,&foreign));
        if (r < 0) return r;
        if (!memchr(foreign.title,0,sizeof(foreign.title)) || strcmp(foreign.title,name) || foreign.pid!=pid || foreign.app_id!=ids[i] || foreign.application_type>2) {
            last_operation="apps.status_contract";
            return TB_IDENTITY;
        }
        if (tb_status_background(&foreign)) {
            /* Do not kill or wait for verified background services. */
            static int seen_background[2];
            unsigned slot = !strcmp(name,"NPXS10063") ? 0 : 1;
            if (!seen_background[slot]) {
                char line[128];
                snprintf(line,sizeof(line),"background_service title=%s pid=%d app_id=%d type=%u\n",name,pid,foreign.app_id,(unsigned)foreign.application_type);
                if (append_log(line)<0) return TB_STORAGE;
                seen_background[slot]=1;
            }
            continue;
        }
        static SceUID probed_pid=-1;
        if (probed_pid!=pid) {
            char probe[160];
            snprintf(probe,sizeof(probe),"blocking_status title=%s pid=%d app_id=%d started=%u type=%u\n",name,pid,foreign.app_id,foreign.started,(unsigned)foreign.application_type);
            if (append_log(probe)<0) return TB_STORAGE;
            probed_pid=pid;
        }
        if (strcmp(waiting_for, name)) {
            snprintf(waiting_for, sizeof(waiting_for), "%s", name);
            status_dirty = 1;
            char line[96];
            snprintf(line, sizeof(line), "waiting_for=%s pid=%d\n", name, pid);
            int logged = append_log(line);
            if (logged < 0) return TB_STORAGE;
        }
        return 1;
    }
    if (waiting_for[0]) {
        waiting_for[0] = 0;
        status_dirty = 1;
    }
    return 0;
}
static int launch(void * c, const char * title) {
    char uri[40];
    snprintf(uri, sizeof(uri), "psgm:play?titleid=%s", title);
    return error_at("game.launch", sceAppMgrLaunchAppByUri(0x20000, uri));
}
static int stop(void * c, const char * title, int32_t owned) {
    int32_t current = - 1;
    int r = process(c, title, & current);
    if (r != 1 || current != owned) return r < 0 ? r : TB_IDENTITY;
    SceInt32 ids[TB_APP_LIST_MAX];
    int n = running_apps(ids);
    if (n < 0) return n;
    for (int i = 0; i < n; i++) {
        SceUID pid = error_at("apps.pid", sceAppMgrGetProcessIdByAppIdForShell(ids[i]));
        if (pid != owned) continue;
        current = - 1;
        r = process(c, title, & current);
        if (r != 1 || current != owned) return r < 0 ? r : TB_IDENTITY;
        if (sceAppMgrGetProcessIdByAppIdForShell(ids[i]) != owned) return TB_IDENTITY;
        return error_at("game.destroy", sceAppMgrDestroyAppByAppId(ids[i]));
    }
    return TB_IDENTITY;
}
static const TBOps ops = {
    pair, registered, game, process, busy, launch, stop, journal_save
};
static int archive_queue(void) {
    char path[160];
    snprintf(path, sizeof(path), ROOT "/jobs/%s", queue.batch);
    int r = directory(path);
    if (r < 0) return r;
    snprintf(path, sizeof(path), ROOT "/jobs/%s/queue.txt", queue.batch);
    char check[TB_QUEUE_BYTES];
    size_t n = 0;
    r = read_file(path, check, sizeof(check), & n);
    if (r >= 0) return n == queue_length && !memcmp(check, queue_bytes, n) ? 0 : TB_IDENTITY;
    if (r != NOENT) return r;
    return write_atomic(path, queue_bytes, queue_length);
}
static int load_queue(const char * path, const char * expected) {
    size_t n = 0;
    int r = read_file(path, queue_bytes, sizeof(queue_bytes) - 1, & n);
    if (r < 0) return r;
    char hash[65];
    trophy_sha256(queue_bytes, n, hash);
    if (strcmp(hash, expected)) return TB_IDENTITY;
    r = tb_queue_parse(queue_bytes, n, & queue);
    if (r >= 0) queue_length = n;
    return r;
}
static int load_journal(void) {
    TBState latest = {
        0
    };
    int seen = 0, valid = 0;
    for (unsigned i = 0; i < 2; i++) {
        char path[96];
        TBState s;
        size_t n = 0;
        snprintf(path, sizeof(path), ROOT "/state-%u.bin", i);
        int r = read_file(path, & s, sizeof(s), & n);
        if (r == NOENT) continue;
        seen++;
        if (r < 0 || n != sizeof(s) || !tb_state_valid(& s)) continue;
        if (!valid || s.sequence > latest.sequence) {
            latest = s;
            valid = 1;
        }
    }
    if (seen && !valid) return TB_STORAGE;
    if (!valid) return 0;
    state = latest;
    have_state = 1;
    char path[160];
    snprintf(path, sizeof(path), ROOT "/jobs/%s/queue.txt", state.batch);
    int r = load_queue(path, state.queue_sha);
    if (r < 0) return r;
    if (queue.count != state.count || strcmp(queue.batch, state.batch) || strcmp(queue.user, state.user)) return TB_IDENTITY;
    return 0;
}
static int command(void) {
    char b[160], boot[33] = {
        0
    }, verb[9] = {
        0
    }, hash[65] = {
        0
    };
    size_t n = 0;
    int used = 0;
    int r = read_file(ROOT "/command.txt", b, sizeof(b) - 1, & n);
    if (r == NOENT) return 0;
    if (r < 0) return r;
    b[n] = 0;
    if (memchr(b, 0, n) || sscanf(b, "TROPHY_BOOTSTRAP_COMMAND_V1\n%32s %8s %64s\n%n", boot, verb, hash, & used) != 3 || used !=(int) n || !tb_hex_valid(boot, 32) || !tb_hex_valid(hash, 64)) r = TB_ERROR;
    else if (strcmp(boot, session)) r = TB_IDENTITY;
    else if (storage_bad) r = TB_STORAGE;
    else if (!strcmp(verb, "START")) {
        if (active ||(have_state && !tb_terminal(& state))) r = TB_BUSY;
        else {
            TBQueue old = queue;
            size_t oldlen = queue_length;
            char oldbytes[TB_QUEUE_BYTES];
            memcpy(oldbytes, queue_bytes, oldlen);
            r = load_queue(ROOT "/queue.txt", hash);
            if (r >= 0 && have_state && !strcmp(queue.batch, state.batch)) r = TB_IDENTITY;
            if (r >= 0) r = archive_queue();
            if (r >= 0) {
                have_state = 1;
                active = 1;
                r = tb_start(& state, & queue, hash, now_ms(), & ops, NULL);
                if (r < 0) active = 0;
            }
            else {
                queue = old;
                queue_length = oldlen;
                memcpy(queue_bytes, oldbytes, oldlen);
            }
        }
    }
    else if (!strcmp(verb, "RESUME")) {
        if (!have_state || active || tb_terminal(& state) || strcmp(hash, state.queue_sha)) r = TB_IDENTITY;
        else {
            r = tb_resume(& state, & queue, now_ms(), & ops, NULL);
            if (r >= 0) active = 1;
        }
    }
    else if (!strcmp(verb, "CANCEL")) {
        if (!have_state || strcmp(hash, state.queue_sha)) r = TB_IDENTITY;
        else {
            r = tb_step(& state, & queue, now_ms(), 1, & ops, NULL);
            if (r >= 0 && !tb_terminal(& state) && state.phase != TB_PAUSED) active = 1;
        }
    }
    else r = TB_ERROR;
    int remove = sceIoRemove(ROOT "/command.txt");
    if (remove < 0) return remove;
    int sync = sceIoSync("ux0:", 0);
    if (sync < 0) return sync;
    char log[128];
    snprintf(log, sizeof(log), "command result=%08X session_matched=%d\n",(unsigned) r, !strcmp(boot, session));
    int logged = append_log(log);
    if (logged < 0) return logged;
    last_error = r;
    if (r == TB_STORAGE) storage_bad = 1;
    return status_write();
}
static int in_shell(void) {
    SceUID list[256];
    SceSize count = 256;
    int r = sceKernelGetModuleList(0, list, & count);
    if (r < 0 || count > 256) return 0;
    for (unsigned i = 0; i < count; i++) {
        SceKernelModuleInfo info;
        memset(& info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(list[i], & info) >= 0 && !strcmp(info.module_name, "SceShell")) return 1;
    }
    return 0;
}
static int worker(SceSize args, void * arg) {
    while (!quitting && prepare() < 0) sceKernelDelayThread(1000000);
    if (quitting) return sceKernelExitThread(0);
    SceRtcTick rtc;
    if (sceRtcGetCurrentTick(& rtc) < 0) return sceKernelExitThread(TB_ERROR);
    uint64_t monotonic = sceKernelGetSystemTimeWide();
    /* libk va_arg advances by four-byte words, unlike ARM 64-bit alignment. */
    snprintf(session, sizeof(session), "%08x%08x%08x%08x",(unsigned)(rtc.tick >> 32),(unsigned) rtc.tick,(unsigned)(monotonic >> 32),(unsigned) monotonic);
    last_error = load_journal();
    if (last_error < 0) storage_bad = 1;
    /* Startup only publishes a new session; no prior command may restart a batch. */
    char ready[128];
    int n = snprintf(ready, sizeof(ready), "{\"schema\":1,\"session\":\"%s\",\"version\":\"0.1.0\"}\n", session);
    if (write_atomic(ROOT "/ready.json", ready,(size_t) n) < 0) return sceKernelExitThread(TB_STORAGE);
    if (status_write() < 0) return sceKernelExitThread(TB_STORAGE);
    sceKernelDelayThread(5000000);
    while (!quitting) {
        int r = command();
        if (r < 0) {
            last_error = r;
            active = 0;
            status_write();
        }
        if (active) {
            uint64_t before = state.sequence;
            last_operation = "none";
            r = tb_step(& state, & queue, now_ms(), 0, & ops, NULL);
            if (r < 0 || state.phase == TB_PAUSED || tb_terminal(& state)) active = 0;
            if (r < 0) last_error = r;
            if (r == TB_STORAGE) storage_bad = 1;
            if (state.sequence != before || r < 0 || status_dirty) {
                status_dirty = 0;
                if (status_write() < 0) {
                    active = 0;
                    last_error = TB_STORAGE;
                }
            }
            if (active) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        }
        sceKernelDelayThread(active ? 200000 : 1000000);
    }
    if (active && state.owned) {
        /* Normal unload asks the same owner-checked state machine to cancel. */
        for (unsigned i = 0; i < 75 && !tb_terminal(& state); i++) {
            int r = tb_step(& state, & queue, now_ms(), 1, & ops, NULL);
            if (r < 0 || state.phase == TB_PAUSED) break;
            sceKernelDelayThread(200000);
        }
    }
    active = 0;
    status_write();
    return sceKernelExitThread(0);
}
int _start(SceSize args, void * arg) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void * arg) {
    if (!in_shell()) return SCE_KERNEL_START_FAILED;
    self_pid = sceKernelGetProcessId();
    thread = sceKernelCreateThread("trophy_bootstrap", worker, 0x80, 0x30000, 0, 0, NULL);
    if (thread < 0) return SCE_KERNEL_START_FAILED;
    int r = sceKernelStartThread(thread, 0, NULL);
    if (r < 0) {
        sceKernelDeleteThread(thread);
        thread = - 1;
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize args, void * arg) {
    quitting = 1;
    if (thread >= 0) {
        SceUInt timeout = 20000000;
        int r = sceKernelWaitThreadEnd(thread, NULL, & timeout);
        if (r < 0) return SCE_KERNEL_STOP_CANCEL;
        sceKernelDeleteThread(thread);
        thread = - 1;
    }
    return SCE_KERNEL_STOP_SUCCESS;
}
