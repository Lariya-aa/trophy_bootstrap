/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app_status.h"
#include "bootstrap.h"
#ifdef TB_VITA
#include <libk/string.h>
#else
#include <string.h>
#endif
int tb_status_classify(const TBAppStatus *s, int32_t named_pid, const char *title) {
    if (!s || !title || named_pid<=0) return TB_ERROR;
    if (!memchr(s->title,0,sizeof(s->title)) || strcmp(s->title,title)) return TB_IDENTITY;
    if (s->application_type>2) return TB_ERROR;
    /* A same-title startup context must block a second launch, but is not
       authority to own/terminate the game process. */
    if (!s->started || s->application_type!=1) return TB_PROCESS_STARTING;
    if (s->pid!=named_pid || s->pid<=0 || s->app_id<=0) return TB_IDENTITY;
    return TB_PROCESS_RUNNING;
}
int tb_status_binding(const TBAppStatus *s, int32_t listed_pid) {
    if (!s) return TB_ERROR;
    return listed_pid==s->pid && listed_pid>0 ? TB_PROCESS_RUNNING : TB_PROCESS_STARTING;
}
int tb_status_background(const TBAppStatus *s) {
    /* These verified system services can coexist with a running game.
       An unknown mini app or a title appearing as a game still blocks. */
    if (!s || !memchr(s->title,0,sizeof(s->title)) || s->pid<=0 || s->app_id<=0 || s->application_type!=0) return 0;
    return !strcmp(s->title,"NPXS10063") || !strcmp(s->title,"NPXS10079");
}
