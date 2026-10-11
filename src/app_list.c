/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app_list.h"
#include "bootstrap.h"
#ifdef TB_VITA
#include <libk/string.h>
#else
#include <string.h>
#endif
int tb_app_list(TBAppQuery query, void *ctx, int32_t ids[TB_APP_LIST_MAX]) {
    if (!query || !ids) return TB_ERROR;
    for (int capacity = 1; capacity <= TB_APP_LIST_MAX; capacity *= 2) {
        memset(ids, 0, sizeof(*ids) * TB_APP_LIST_MAX);
        int count = query(ctx, ids, capacity);
        if (count < 0) return count;
        if (count > capacity) return TB_ERROR;
        for (int i = 0; i < count; i++) if (ids[i] <= 0) return TB_IDENTITY;
        for (int i = count; i < TB_APP_LIST_MAX; i++)
            if (ids[i]) return TB_ERROR; /* Retval and output contract disagree. */
        if (count < capacity) return count;
    }
    return TB_CAPACITY; /* Saturated maximum; do not accept a partial list. */
}
