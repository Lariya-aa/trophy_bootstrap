/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TB_BOOTSTRAP_H
#define TB_BOOTSTRAP_H
#include <stdint.h>
#include <stddef.h>
#define TB_MAX 100
#define TB_QUEUE_BYTES 8192
#define TB_MAGIC 0x54424931u
#define TB_ERROR (-0x7401)
#define TB_BUSY (-0x7402)
#define TB_CAPACITY (-0x7403)
#define TB_IDENTITY (-0x7404)
#define TB_TIMEOUT (-0x7405)
#define TB_STORAGE (-0x7406)
#define TB_PROCESS_RUNNING 1
#define TB_PROCESS_STARTING 2
typedef struct { char title[10], npwr[13]; } TBItem;
typedef struct {
 char batch[33], user[3]; unsigned count,min_ms,max_ms;
 TBItem items[TB_MAX];
} TBQueue;
typedef enum { TB_IDLE,TB_PREFLIGHT,TB_LAUNCH,TB_WAIT_PROCESS,TB_RUN,
 TB_STOP,TB_WAIT_EXIT,TB_CHECK_PAIR,TB_COOLDOWN,TB_RECOVER_WAIT,
 TB_GENERATED,TB_CANCELLED,TB_PAUSED } TBPhase;
typedef enum { TB_PENDING,TB_EXISTING,TB_PAIR_CREATED,TB_MISSING_PAIR,
 TB_INCOMPLETE_PAIR,TB_LAUNCH_FAILED,TB_ITEM_CANCELLED,TB_RECOVERED_PAIR } TBResult;
typedef struct {
 uint32_t magic,version; uint64_t sequence;
 char queue_sha[65],batch[33],user[3];
 uint32_t count,index,phase,cancel,owned,halt;
 int32_t pid,error;
 uint64_t phase_at,ready_at;
 uint8_t results[TB_MAX]; char checksum[65];
} TBState;
typedef struct {
 /* pair: 0 absent, 1 both present/nonempty, 2 partial container; <0 error.
    process: 0 absent, 1 running game, 2 present but starting; negative errors
    must not mean absent. A starting context blocks launching but cannot be owned. */
 int (*pair)(void *,const char *,const char *);
 int (*registered)(void *,const char *);
 int (*game)(void *,const TBItem *);
 int (*process)(void *,const char *,int32_t *);
 int (*busy)(void *,int32_t);
 int (*launch)(void *,const char *);
 int (*stop)(void *,const char *,int32_t);
 int (*save)(void *,const TBState *);
} TBOps;
int tb_npwr_valid(const char *s);
int tb_hex_valid(const char *s,size_t n);
int tb_queue_parse(const char *bytes,size_t size,TBQueue *queue);
int tb_state_valid(const TBState *s);
int tb_start(TBState *,const TBQueue *,const char *,uint64_t,const TBOps *,void *);
int tb_resume(TBState *,const TBQueue *,uint64_t,const TBOps *,void *);
int tb_step(TBState *,const TBQueue *,uint64_t,int,const TBOps *,void *);
int tb_terminal(const TBState *);
const char *tb_phase_name(unsigned);
const char *tb_result_name(unsigned);
#endif
