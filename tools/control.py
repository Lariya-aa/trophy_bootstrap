#!/usr/bin/env python3
"""Local queue/control files only. No FTP, process execution or trophy DAT edits."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import uuid
HEADER = 'TROPHY_BOOTSTRAP_QUEUE_V1'
RESERVED = {'TROPIO001','VITASHELL','TRPINI001','TRPPRB001','TRPBOOT01'}
TRP = re.compile(r'ux0:app/([A-Z0-9]{9})/sce_sys/trophy/(NPWR[0-9]{5}_[0-9]{2})/TROPHY[.]TRP')

def digest(blob):
    return hashlib.sha256(blob).hexdigest()

def checked_json(path):
    if path.is_symlink() or path.stat().st_size > 1024*1024:
        raise ValueError('unexpected JSON file')
    return json.loads(path.read_text())

def parse_queue(blob):
    if not 0 < len(blob) < 8192 or not blob.endswith(b'\n') or b'\r' in blob or b'\0' in blob:
        raise ValueError('invalid queue encoding/length')
    lines = blob.decode('ascii').splitlines()
    if len(lines) < 5 or lines[0] != HEADER or not re.fullmatch(r'batch=[0-9a-f]{32}',lines[1]) or not re.fullmatch(r'user=[0-9]{2}',lines[2]) or lines[3] not in {'window_ms=5000,10000','window_ms=5000,30000'}:
        raise ValueError('invalid queue header')
    items = []
    for line in lines[4:]:
        match = re.fullmatch(r'([A-Z0-9]{9}) (NPWR[0-9]{5}_[0-9]{2})',line)
        if not match or match[1].startswith('NPXS'):
            raise ValueError('invalid game row')
        items.append(match.groups())
    if len(items) > 100 or len({x[0] for x in items}) != len(items) or len({x[1] for x in items}) != len(items):
        raise ValueError('duplicate or oversized queue')
    return {'batch':lines[1][6:],'user':lines[2][5:],'items':items,'min_ms':5000,'max_ms':int(lines[3].split(',')[1]),'sha256':digest(blob)}

def prepare(args):
    d=checked_json(args.catalog)
    if d.get('schema') != 1 or not d.get('complete') or d.get('scan_error') != 0:
        raise ValueError('complete verified catalog required')
    personal={x['id'] for x in d['entries'] if x['sources'] & 1}
    capacity=100-len(personal)
    if capacity <= 0:
        raise ValueError('no slots available in catalog; actual device is checked again by plugin')
    choices={}
    for item in d['files']:
        match=TRP.fullmatch(item['path'])
        if not match or match[1] in RESERVED or match[1].startswith('NPXS') or match[2] in personal or item['bytes'] <= 0:
            continue
        if args.title and match[1] not in args.title:
            continue
        if args.ux0_root:
            path=args.ux0_root/Path(item['path'][4:])
            if path.is_symlink() or not path.is_file() or path.stat().st_size != item['bytes']:
                raise ValueError('installed TRP metadata differs from catalog')
        choices.setdefault(match[2],(match[1],match[2]))
    selected=[];seen=set()
    for title,npwr in sorted(choices.values(),key=lambda x:x[1]):
        if title not in seen:
            selected.append((title,npwr));seen.add(title)
        if len(selected) >= min(capacity,args.limit):break
    if not selected or (args.title and set(args.title) != {x[0] for x in selected}):
        raise ValueError('requested target not available, duplicated, or excluded')
    batch=uuid.uuid4().hex
    blob=('\n'.join([HEADER,'batch='+batch,'user='+args.user,f'window_ms=5000,{args.max_run_ms}',*[' '.join(x) for x in selected]])+'\n').encode('ascii')
    result=parse_queue(blob)
    args.output.mkdir(parents=True,exist_ok=False)
    (args.output/'queue.txt').write_bytes(blob)
    report={**result,'personal_sets_in_catalog':len(personal),'catalog_is_snapshot':True,'generation_unverified':True,'command_armed':False,'source_catalog':args.catalog.name}
    (args.output/'queue-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(report,ensure_ascii=False))

def command(args):
    root=args.root
    if not root.is_dir() or root.is_symlink():raise ValueError('control root unavailable')
    ready=checked_json(root/'ready.json');status=checked_json(root/'status.json')
    boot=ready['session']
    if not re.fullmatch('[0-9a-f]{32}',boot) or status['session'] != boot or status['storage_error']:
        raise ValueError('plugin session is not ready')
    if args.verb=='START':
        if status['active'] or status['needs_explicit_resume']:raise ValueError('unfinished batch; resume or cancel it first')
        queue=parse_queue((root/'queue.txt').read_bytes())
        if queue['batch']==status.get('batch'):raise ValueError('batch already executed; use a fresh queue')
        hash_=queue['sha256']
    else:
        hash_=status['queue_sha256']
        if not re.fullmatch('[0-9a-f]{64}',hash_):raise ValueError('no recorded active batch')
        queue=parse_queue((root/'jobs'/status['batch']/'queue.txt').read_bytes())
        if queue['sha256'] != hash_:raise ValueError('saved queue mismatch')
    final=root/'command.txt';temp=root/'command.txt.client-pending'
    if final.exists() or temp.exists():raise ValueError('unconsumed command retained; refused overwrite')
    blob=f'TROPHY_BOOTSTRAP_COMMAND_V1\n{boot} {args.verb} {hash_}\n'.encode()
    with temp.open('xb') as f:
        f.write(blob);f.flush();os.fsync(f.fileno())
    if temp.read_bytes()!=blob:raise ValueError('command readback failed')
    temp.rename(final)
    if final.read_bytes()!=blob:raise ValueError('published command readback failed')
    print(json.dumps({'command':args.verb,'batch':queue['batch'],'session':boot,'queued_only':True,'actual_execution_verified':False}))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    sub=p.add_subparsers(dest='action',required=True)
    q=sub.add_parser('prepare');q.add_argument('--catalog',type=Path,required=True);q.add_argument('--output',type=Path,required=True);q.add_argument('--title',action='append');q.add_argument('--limit',type=int,default=97);q.add_argument('--user',default='00');q.add_argument('--max-run-ms',type=int,choices=[10000,30000],default=10000);q.add_argument('--ux0-root',type=Path)
    c=sub.add_parser('command');c.add_argument('verb',choices=['START','RESUME','CANCEL']);c.add_argument('--root',type=Path,required=True)
    s=sub.add_parser('status');s.add_argument('--root',type=Path,required=True)
    args=p.parse_args()
    if args.action=='prepare':
        if not 1 <= args.limit <= 100 or not re.fullmatch('[0-9]{2}',args.user):p.error('invalid limit/account directory')
        prepare(args)
    elif args.action=='command':command(args)
    else:print(json.dumps(checked_json(args.root/'status.json'),ensure_ascii=False,indent=2))
if __name__=='__main__':main()
