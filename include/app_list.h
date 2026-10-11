/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TB_APP_LIST_H
#define TB_APP_LIST_H
#include <stdint.h>
#define TB_APP_LIST_MAX 32
typedef int (*TBAppQuery)(void *, int32_t *, int);
/* Return a complete list only. A full/error result never proves absence. */
int tb_app_list(TBAppQuery query, void *ctx, int32_t ids[TB_APP_LIST_MAX]);
#endif
