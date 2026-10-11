/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TB_APP_STATUS_H
#define TB_APP_STATUS_H
#include <stdint.h>
#include <stddef.h>
/* VitaSDK current AppMgr status contract, FW 3.60 0x80-byte output.
   Pinned SDK has the import stub but no declaration/typed status. */
typedef struct {
    int32_t parent_pid;
    uint32_t launch_flags, protection;
    char title[32];
    uint32_t launch_param;
    int32_t app_id, pid;
    uint32_t active, started;
    uint8_t padding[0x30];
    uint8_t launch_type, application_type;
    uint8_t trailing[0xE];
} TBAppStatus;
typedef char TBStatusSize[(sizeof(TBAppStatus)==0x80)?1:-1];
typedef char TBStatusAppID[(offsetof(TBAppStatus,app_id)==0x30)?1:-1];
typedef char TBStatusPID[(offsetof(TBAppStatus,pid)==0x34)?1:-1];
typedef char TBStatusStarted[(offsetof(TBAppStatus,started)==0x3C)?1:-1];
typedef char TBStatusType[(offsetof(TBAppStatus,application_type)==0x71)?1:-1];
int tb_status_classify(const TBAppStatus *, int32_t named_pid, const char *title);
int tb_status_binding(const TBAppStatus *, int32_t listed_pid);
int tb_status_background(const TBAppStatus *);
#endif
