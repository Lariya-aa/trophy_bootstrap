#!/bin/sh
set -eu
task_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
task_temp=$(mktemp -d "${TMPDIR:-/tmp}/trophy-bootstrap-tests.XXXXXX")
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-misleading-indentation -fsanitize=address,undefined -fno-omit-frame-pointer -I "$task_root/include" "$task_root/tests/test_scheduler.c" "$task_root/src/bootstrap.c" "$task_root/src/sha256.c" "$task_root/src/app_status.c" -o "$task_temp/test_scheduler"
"$task_temp/test_scheduler"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$task_root/include" "$task_root/tests/test_app_list.c" "$task_root/src/app_list.c" -o "$task_temp/test_app_list"
"$task_temp/test_app_list"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$task_root/include" "$task_root/tests/test_app_status.c" "$task_root/src/app_status.c" -o "$task_temp/test_app_status"
"$task_temp/test_app_status"
