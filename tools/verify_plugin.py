#!/usr/bin/env python3
"""Inspect SELF envelope. Successful checks do not establish Shell compatibility."""
import argparse,hashlib,json,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('plugin',type=Path);args=p.parse_args();blob=args.plugin.read_bytes()
if len(blob)<4096 or blob[:4]!=b'SCE\0':raise ValueError('invalid SELF')
app=struct.unpack_from('<Q',blob,0x38)[0];authid=struct.unpack_from('<Q',blob,app)[0]
control,length=struct.unpack_from('<QQ',blob,0x68);end=control+length;minimum=None
if end>len(blob):raise ValueError('truncated control records')
while control<end:
 kind,size=struct.unpack_from('<II',blob,control)
 if size<16 or control+size>end:raise ValueError('bad control record')
 if kind==4:minimum=struct.unpack_from('<Q',blob,control+16+20+32+4)[0]
 control+=size
if minimum!=0:raise ValueError('unexpected firmware floor')
print(json.dumps({'artifact':args.plugin.name,'bytes':len(blob),'sha256':hashlib.sha256(blob).hexdigest(),'authid':hex(authid),'minimum_required_firmware':minimum,'runtime_verified':False},indent=2))
