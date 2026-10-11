#!/bin/sh
set -eu
task_source=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
task_build=$(mktemp -d "${TMPDIR:-/tmp}/trophy-bootstrap-build.XXXXXX")
task_stage="$task_build/source"
python3 - "$task_source" "$task_stage" <<'PYCODE'
from pathlib import Path
import hashlib,json,sys
source,stage=map(Path,sys.argv[1:]);stage.mkdir()
files=[source/'CMakeLists.txt',source/'exports.yml']
for name in ['src','include']:
 files.extend(p for p in (source/name).rglob('*') if p.is_file())
manifest=[]
for p in files:
 if p.is_symlink():raise ValueError('source symlink refused')
 b=p.read_bytes()
 if not b or b'\0' in b:raise ValueError('empty or NUL-containing source refused')
 rel=p.relative_to(source);target=stage/rel;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(b)
 if target.read_bytes()!=b:raise ValueError('source staging readback failed')
 manifest.append({'path':str(rel),'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()})
(stage.parent/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
PYCODE
task_image='vitasdk/vitasdk@sha256:7f5eee50ff95b73c8c847dbfef6227aa0035886444b4d0254c21da8369ff1efc'
printf 'Build directory: %s\n' "$task_build"
if docker run --rm --network none -v "$task_stage:/workspace:ro" -v "$task_build:/build" -w /workspace "$task_image" sh -c 'cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release && cmake --build /build -j4' >"$task_build/build.log" 2>&1; then
 cat "$task_build/build.log"
 printf 'Plugin: %s/trophy_bootstrap.suprx\n' "$task_build"
else
 cat "$task_build/build.log"
 exit 1
fi
