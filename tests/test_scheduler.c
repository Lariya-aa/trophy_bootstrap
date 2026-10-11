/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bootstrap.h"
#include "sha256.h"
#include "app_status.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    TBState * s;
    TBQueue * q;
    uint64_t now, launch_at, stop_at;
    int pair[2], pid[2], count, launches, stops, saves, busy, ready_ms, slots;
    int launch_error, stop_error, process_error, save_error, no_pid, exit_stuck;
    int registered_calls, game_calls, process_starting, no_pair_on_stop;
    TBState durable;
}
Fake;
static int item(Fake * f, const char * name, int npwr) {
    for (unsigned i = 0; i < f->q->count; i++) if (!strcmp(npwr ? f->q->items[i].npwr : f->q->items[i].title, name)) return(int) i;
    assert(0);
    return - 1;
}
static int pair(void * v, const char * u, const char * n) {
    Fake * f = v;
    assert(!strcmp(u, "00"));
    int i = item(f, n, 1);
    if (f->pid[i] && f->ready_ms >= 0 && f->now - f->launch_at >=(unsigned) f->ready_ms && !f->pair[i]) {
        f->pair[i] = 1;
        f->count++;
    }
    return f->pair[i];
}
static int count(void * v, const char * u) {
    ((Fake *) v)->registered_calls++;
    return((Fake *) v)->count;
}
static int game(void * v, const TBItem * i) {
    ((Fake *) v)->game_calls++;
    return((Fake *) v)->slots;
}
static int process(void * v, const char * t, int32_t * out) {
    Fake * f = v;
    if (f->process_error) return f->process_error;
    int i = item(f, t, 0);
    if (!f->pid[i]) return 0;
    * out = f->pid[i];
    return f->process_starting ? TB_PROCESS_STARTING : TB_PROCESS_RUNNING;
}
static int busy(void * v, int32_t p) {
    return((Fake *) v)->busy;
}
static int launch(void * v, const char * t) {
    Fake * f = v;
    assert(f->durable.phase == TB_LAUNCH && tb_state_valid(& f->durable));
    f->launches++;
    if (f->launch_error) return f->launch_error;
    int i = item(f, t, 0);
    f->launch_at = f->now;
    if (!f->no_pid) f->pid[i] = 100 + i;
    return 0;
}
static int stop(void * v, const char * t, int32_t p) {
    Fake * f = v;
    int i = item(f, t, 0);
    assert(f->durable.phase == TB_STOP && tb_state_valid(& f->durable));
    assert(f->pid[i] == p && p == f->s->pid);
    f->stops++;
    f->stop_at = f->now;
    if (f->stop_error) return f->stop_error;
    if (!f->exit_stuck) f->pid[i] = 0;
    if (!f->pair[i] && !f->no_pair_on_stop) {
        f->pair[i] = 1;
        f->count++;
    }
    return 0;
}
static int save(void * v, const TBState * s) {
    Fake * f = v;
    f->saves++;
    if (f->save_error) return - 9;
    assert(tb_state_valid(s));
    f->durable = * s;
    return 0;
}
static const TBOps ops = {
    pair, count, game, process, busy, launch, stop, save
};
static TBQueue queue(void) {
    TBQueue q;
    const char * b = "TROPHY_BOOTSTRAP_QUEUE_V1\nbatch=0123456789abcdef0123456789abcdef\nuser=00\nwindow_ms=5000,10000\nPCSG00122 NPWR03833_00\nPCSE00001 NPWR00001_00\n";
    assert(!tb_queue_parse(b, strlen(b), & q));
    return q;
}
static Fake init(TBState * s, TBQueue * q) {
    memset(s, 0, sizeof(* s));
    Fake f = {
        0
    };
    f.s = s;
    f.q = q;
    f.ready_ms = - 1;
    f.slots = 1;
    f.count = 3;
    return f;
}
static void start(Fake * f) {
    char hash[65];
    trophy_sha256("queue", 5, hash);
    assert(!tb_start(f->s, f->q, hash, f->now, & ops, f));
}
static int step(Fake * f, int cancel) {
    f->now += 200;
    return tb_step(f->s, f->q, f->now, cancel, & ops, f);
}
static void until(Fake * f, unsigned phase) {
    for (unsigned i = 0; i < 400 && f->s->phase != phase; i++) {
        int r = step(f, 0);
        if (r < 0) break;
    }
    assert(f->s->phase == phase);
}
int main(void) {
    TBQueue q = queue();
    TBState s;
    Fake f;
    /* Real queue shape, parsing must fail rather than silently truncate or merge. */
    TBQueue bad;
    char data[256];
    const char * prefix = "TROPHY_BOOTSTRAP_QUEUE_V1\nbatch=0123456789abcdef0123456789abcdef\nuser=00\nwindow_ms=5000,10000\n";
    snprintf(data, sizeof(data), "%sPCSG00122 NPWR03833_00\nPCSG00122 NPWR00001_00\n", prefix);
    assert(tb_queue_parse(data, strlen(data), & bad) < 0);
    snprintf(data, sizeof(data), "%sNPXS10015 NPWR03833_00\n", prefix);
    assert(tb_queue_parse(data, strlen(data), & bad) < 0);
    snprintf(data, sizeof(data), "%sPCSG00122 NPWR03833_00 extra\n", prefix);
    assert(tb_queue_parse(data, strlen(data), & bad) < 0);
    assert(tb_queue_parse(data, strlen(data) - 1, & bad) < 0);
    /* Extended launch window: delayed initialization, finite missing timeout,
       early completion and cancellation must all retain their timing. */
    const char *slow_bytes = "TROPHY_BOOTSTRAP_QUEUE_V1\nbatch=0123456789abcdef0123456789abcdef\nuser=00\nwindow_ms=5000,30000\nPCSG00122 NPWR03833_00\nPCSE00001 NPWR00001_00\n";
    TBQueue slow;
    assert(!tb_queue_parse(slow_bytes, strlen(slow_bytes), &slow));
    assert(slow.min_ms == 5000 && slow.max_ms == 30000);
    const char *invalid_window = "TROPHY_BOOTSTRAP_QUEUE_V1\nbatch=0123456789abcdef0123456789abcdef\nuser=00\nwindow_ms=5000,30001\nPCSG00122 NPWR03833_00\n";
    assert(tb_queue_parse(invalid_window, strlen(invalid_window), &bad) < 0);
    f = init(&s, &slow);f.ready_ms = 16000;f.no_pair_on_stop = 1;
    start(&f);until(&f, TB_RUN);
    uint64_t slow_began = s.phase_at;
    while (f.now - slow_began < 10000) assert(!step(&f, 0));
    assert(s.phase == TB_RUN && !f.stops);
    until(&f, TB_CHECK_PAIR);
    assert(f.stop_at - slow_began > 10000 && f.stop_at - slow_began < 20000 && f.pair[0]);
    until(&f, TB_COOLDOWN);assert(s.results[0] == TB_PAIR_CREATED);
    f = init(&s, &slow);f.no_pair_on_stop = 1;
    start(&f);until(&f, TB_RUN);slow_began = s.phase_at;
    until(&f, TB_CHECK_PAIR);
    assert(f.stop_at - slow_began >= 30000 && f.stop_at - slow_began <= 30400);
    until(&f, TB_COOLDOWN);assert(s.results[0] == TB_MISSING_PAIR);
    f = init(&s, &slow);f.ready_ms = 500;start(&f);
    until(&f, TB_RUN);slow_began = s.phase_at;until(&f, TB_CHECK_PAIR);
    assert(f.stop_at - slow_began >= 5000 && f.stop_at - slow_began <= 5400);
    f = init(&s, &slow);start(&f);until(&f, TB_RUN);slow_began = s.phase_at;
    assert(!step(&f, 1));until(&f, TB_CANCELLED);
    assert(f.stop_at - slow_began < 1000 && f.stops == 1);
    f = init(& s, & q);
    start(& f);
    until(& f, TB_GENERATED);
    assert(f.launches == 2 && f.stops == 2 && s.results[0] == TB_PAIR_CREATED && s.results[1] == TB_PAIR_CREATED);
    f = init(& s, & q);
    f.ready_ms = 500;
    start(& f);
    until(& f, TB_RUN);
    uint64_t began = s.phase_at;
    until(& f, TB_CHECK_PAIR);
    assert(f.stop_at - began >= 5000 && f.stop_at - began <= 5400);
    f = init(& s, & q);
    f.pair[0] = 1;
    start(& f);
    until(& f, TB_GENERATED);
    assert(f.launches == 1 && s.results[0] == TB_EXISTING);
    f = init(& s, & q);
    f.pair[0] = 2;
    start(& f);
    until(& f, TB_GENERATED);
    assert(f.launches == 1 && s.results[0] == TB_INCOMPLETE_PAIR);
    f = init(& s, & q);
    f.count = 100;
    start(& f);
    until(& f, TB_PAUSED);
    assert(!f.launches && s.error == TB_CAPACITY);
    f = init(& s, & q);
    f.count = 99;
    f.slots = 2;
    start(& f);
    until(& f, TB_PAUSED);
    assert(!f.launches && s.error == TB_CAPACITY);
    f = init(& s, & q);
    f.count = 99;
    start(& f);
    until(& f, TB_PAUSED);
    assert(f.launches == 1 && s.results[0] == TB_PAIR_CREATED && s.index == 1);
    f = init(& s, & q);
    f.busy = 1;
    start(& f);
    for (unsigned i = 0; i < 20; i++) assert(!step(& f, 0));
    assert(!f.launches && s.phase == TB_PREFLIGHT);
    assert(!f.registered_calls && !f.game_calls);
    f.busy = 0;
    until(& f, TB_RUN);
    /* Replay a middleware appearing during RUN: it must not prematurely stop
       the target; an unknown system app still stops and preserves the reason. */
    TBAppStatus background={0};strcpy(background.title,"NPXS10063");
    background.pid=6957101;background.app_id=921606;background.started=1;
    f.busy=tb_status_background(&background)?0:1;
    for (unsigned i=0;i<10;i++) assert(!step(&f,0));
    assert(s.phase==TB_RUN && !f.stops && !s.error);
    until(&f,TB_COOLDOWN);assert(s.results[0]==TB_PAIR_CREATED && !s.halt);
    f=init(&s,&q);f.no_pair_on_stop=1;start(&f);until(&f,TB_RUN);
    strcpy(background.title,"NPXS10065");f.busy=tb_status_background(&background)?0:1;
    until(&f,TB_PAUSED);
    assert(f.stops==1 && f.launches==1 && s.index==1 && s.halt && s.error==TB_BUSY && s.results[0]==TB_MISSING_PAIR);
    assert(f.durable.error==TB_BUSY && tb_state_valid(&f.durable));
    f = init(& s, & q);
    f.pid[0] = 999;
    start(& f);
    until(& f, TB_PAUSED);
    assert(!f.launches && !f.stops && s.error == TB_BUSY);
    f = init(& s, & q);
    f.process_error = - 44;
    start(& f);
    until(& f, TB_PAUSED);
    assert(!f.launches && !f.stops);
    /* A same-title startup context stable for >1s cannot start the run timer. */
    f=init(&s,&q);start(&f);until(&f,TB_WAIT_PROCESS);f.process_starting=1;
    for (int i=0;i<15;i++) assert(!step(&f,0));
    assert(s.phase==TB_WAIT_PROCESS && !s.owned && !f.stops && !s.ready_at);
    f.pid[0]=777;f.process_starting=0;assert(!step(&f,0));uint64_t actual_started=f.now;
    until(&f,TB_RUN);assert(s.pid==777 && s.phase_at-actual_started>=1000);
    until(&f,TB_CHECK_PAIR);assert(f.stops==1);
    /* A user-started startup context also blocks PREFLIGHT launch. */
    f=init(&s,&q);f.pid[0]=999;f.process_starting=1;start(&f);until(&f,TB_PAUSED);
    assert(s.error==TB_BUSY && !f.launches && !f.stops);
    /* Running status lost after ownership cannot become kill authority. */
    f=init(&s,&q);start(&f);until(&f,TB_RUN);f.process_starting=1;until(&f,TB_PAUSED);
    assert(s.error==TB_IDENTITY && !s.owned && !f.stops);
    /* Startup PID replacement resets stabilization without granting ownership. */
    f = init(&s, &q);
    start(&f);
    until(&f, TB_WAIT_PROCESS);
    assert(!step(&f, 0));
    assert(s.phase == TB_WAIT_PROCESS && !s.owned && s.pid == 100);
    for (int i = 0; i < 3; i++) assert(!step(&f, 0));
    f.pid[0] = 777;
    assert(!step(&f, 0));
    uint64_t stable_at = f.now;
    assert(s.pid == 777 && !s.owned && s.phase == TB_WAIT_PROCESS);
    until(&f, TB_RUN);
    assert(s.owned && s.pid == 777 && s.phase_at - stable_at >= 1000);
    uint64_t run_at = s.phase_at;
    until(&f, TB_CHECK_PAIR);
    assert(f.stops == 1 && f.stop_at - run_at >= 5000 && f.stop_at - run_at <= 10400);
    /* Disappearance resets observation; a late PID must earn a full interval. */
    f = init(&s, &q);start(&f);until(&f, TB_WAIT_PROCESS);
    assert(!step(&f, 0));f.pid[0] = 0;
    assert(!step(&f, 0));assert(s.pid == -1 && !s.ready_at && !s.owned);
    f.pid[0] = 888;assert(!step(&f, 0));stable_at = f.now;
    until(&f, TB_RUN);assert(s.phase_at - stable_at >= 1000 && s.pid == 888);
    /* Continual churn times out, and cannot kill an unconfirmed PID. */
    f = init(&s, &q);start(&f);until(&f, TB_WAIT_PROCESS);
    while (s.phase == TB_WAIT_PROCESS) {
        f.pid[0]++;step(&f, 0);
    }
    assert(s.phase == TB_PAUSED && s.error == TB_TIMEOUT && !s.owned && !f.stops);
    f = init(& s, & q);
    start(& f);
    until(& f, TB_RUN);
    f.pid[0] = 777;
    until(& f, TB_PAUSED);
    assert(!f.stops && s.error == TB_IDENTITY);
    f = init(& s, & q);
    start(& f);
    until(& f, TB_RUN);
    assert(!step(& f, 1));
    until(& f, TB_CANCELLED);
    assert(f.stops == 1 && f.launches == 1 && f.pair[0]);
    f = init(& s, & q);
    start(& f);
    assert(!step(& f, 1));
    assert(tb_terminal(& s) && !f.launches);
    f = init(& s, & q);
    f.no_pid = 1;
    start(& f);
    until(& f, TB_PAUSED);
    assert(s.error == TB_TIMEOUT && f.launches == 1 && !f.stops);
    f = init(& s, & q);
    f.exit_stuck = 1;
    start(& f);
    until(& f, TB_PAUSED);
    assert(s.error == TB_TIMEOUT && f.launches == 1 && f.stops == 1);
    f = init(& s, & q);
    f.stop_error = - 66;
    start(& f);
    until(& f, TB_PAUSED);
    assert(f.launches == 1 && s.owned);
    f = init(& s, & q);
    start(& f);
    f.save_error = 1;
    assert(step(& f, 0) == TB_STORAGE);
    assert(!f.launches);
    f = init(& s, & q);
    start(& f);
    until(& f, TB_RUN);
    TBState before = s;
    s = before;
    assert(!tb_resume(& s, & q, f.now, & ops, & f));
    until(& f, TB_PAUSED);
    assert(!f.stops && s.error == TB_BUSY && !s.owned);
    f.pid[0] = 0;
    f.pair[0] = 1;
    assert(!tb_resume(& s, & q, f.now, & ops, & f));
    until(& f, TB_COOLDOWN);
    assert(s.results[0] == TB_RECOVERED_PAIR && f.launches == 1);
    f = init(& s, & q);
    start(& f);
    until(& f, TB_GENERATED);
    uint64_t old = s.sequence;
    strcpy(q.batch, "abcdef0123456789abcdef0123456789");
    start(& f);
    assert(s.sequence > old);
    TBState broken = s;
    broken.index = 101;
    assert(!tb_state_valid(& broken));
    broken = s;
    broken.results[0] ^= 1;
    assert(!tb_state_valid(& broken));
    puts("scheduler checks passed: two targets, 5-second readiness, existing/partial skip, live capacity and multi-set reservation, busy defers directory scans, foreign/PID guards, cancellation, launch/exit/stop errors, durable-before-effect, restart recovery, monotonic journal, parser and checksum rejection");
    return 0;
}
