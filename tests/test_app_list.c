/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app_list.h"
#include "bootstrap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    int total, calls, max_requested, fail_at, mismatch;
} Provider;
static int query(void *ctx, int32_t *ids, int capacity) {
    Provider *p = ctx;
    p->calls++;
    p->max_requested = capacity;
    for (int i = 0; i < TB_APP_LIST_MAX; i++) assert(!ids[i]);
    if (capacity >= p->fail_at && p->fail_at) return (int)0x8002710B;
    int count = p->total < capacity ? p->total : capacity;
    for (int i = 0; i < count; i++) ids[i] = 100 + i;
    return p->mismatch ? 0 : count;
}
int main(void) {
    int32_t ids[TB_APP_LIST_MAX];
    Provider p = {.total=0};
    assert(tb_app_list(query,&p,ids)==0 && p.calls==1 && p.max_requested==1);
    p=(Provider){.total=1,.fail_at=4};
    assert(tb_app_list(query,&p,ids)==1 && p.calls==2 && p.max_requested==2);
    p=(Provider){.total=3,.fail_at=8};
    assert(tb_app_list(query,&p,ids)==3 && p.calls==3 && p.max_requested==4);
    assert(ids[2]==102); /* A third foreign app cannot disappear in a one-slot query. */
    p=(Provider){.total=3,.fail_at=2};
    assert(tb_app_list(query,&p,ids)==(int)0x8002710B && p.calls==2);
    p=(Provider){.total=1,.fail_at=1};
    assert(tb_app_list(query,&p,ids)==(int)0x8002710B && p.calls==1);
    p=(Provider){.total=TB_APP_LIST_MAX};
    assert(tb_app_list(query,&p,ids)==TB_CAPACITY && p.calls==6);
    p=(Provider){.total=1,.mismatch=1};
    assert(tb_app_list(query,&p,ids)==TB_ERROR);
    assert(tb_app_list(NULL,&p,ids)==TB_ERROR);
    puts("app list checks passed: small requests, complete foreign app coverage, no partial fallback, maximum saturation and contract mismatch stop");
    return 0;
}
