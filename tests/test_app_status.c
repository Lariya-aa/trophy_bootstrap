/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app_status.h"
#include "bootstrap.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    TBAppStatus s={0}; strcpy(s.title,"PCSB01309");s.pid=327684;s.app_id=42;
    /* An initial PID can stay unchanged for seconds while not a running game. */
    for (int i=0;i<20;i++) assert(tb_status_classify(&s,327684,"PCSB01309")==TB_PROCESS_STARTING);
    s.started=1;s.application_type=0;
    assert(tb_status_classify(&s,327684,"PCSB01309")==TB_PROCESS_STARTING);
    s.application_type=1;
    assert(tb_status_classify(&s,327684,"PCSB01309")==TB_PROCESS_RUNNING);
    assert(tb_status_binding(&s,-1)==TB_PROCESS_STARTING);
    assert(tb_status_binding(&s,144173)==TB_PROCESS_STARTING);
    assert(tb_status_binding(&s,327684)==TB_PROCESS_RUNNING);
    s.pid=144173;assert(tb_status_classify(&s,327684,"PCSB01309")==TB_IDENTITY);
    assert(tb_status_classify(&s,144173,"OTHER0001")==TB_IDENTITY);
    s.app_id=0;assert(tb_status_classify(&s,144173,"PCSB01309")==TB_IDENTITY);
    s.app_id=42;s.started=3;assert(tb_status_classify(&s,144173,"PCSB01309")==TB_PROCESS_RUNNING); /* SceBool is nonzero, not restricted to one. */
    s.started=1;s.application_type=7;assert(tb_status_classify(&s,144173,"PCSB01309")==TB_ERROR);
    s.application_type=1;memset(s.title,'X',sizeof(s.title));assert(tb_status_classify(&s,144173,"PCSB01309")==TB_IDENTITY);
    memset(&s,0,sizeof(s));strcpy(s.title,"NPXS10063");s.pid=6957101;s.app_id=921606;s.started=1;
    assert(tb_status_background(&s));
    s.application_type=1;assert(!tb_status_background(&s));
    s.application_type=2;assert(!tb_status_background(&s));
    s.application_type=0;strcpy(s.title,"NPXS10079");assert(tb_status_background(&s));
    strcpy(s.title,"NPXS10065");assert(!tb_status_background(&s)); /* Unknown system dialog. */
    strcpy(s.title,"VITASHELL");assert(!tb_status_background(&s));
    strcpy(s.title,"PCSB01106");assert(!tb_status_background(&s));
    strcpy(s.title,"NPXS10063");s.app_id=0;assert(!tb_status_background(&s));
    s.app_id=921606;s.pid=0;assert(!tb_status_background(&s));
    s.pid=6957101;memset(s.title,'X',sizeof(s.title));assert(!tb_status_background(&s));
    assert(!tb_status_background(NULL));
    puts("app status checks passed: startup/mini context never owned, running game requires PID/title/app identity and Shell binding, malformed status rejected");
    return 0;
}
