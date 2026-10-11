#!/usr/bin/env python3
import hashlib,json,subprocess,sys,tempfile,unittest
from pathlib import Path
TOOL=Path(__file__).resolve().parents[1]/'tools/control.py'
class Controls(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);self.session='1'*32
  self.blob=b'TROPHY_BOOTSTRAP_QUEUE_V1\nbatch=0123456789abcdef0123456789abcdef\nuser=00\nwindow_ms=5000,10000\nPCSE00001 NPWR00001_00\n'
  self.hash=hashlib.sha256(self.blob).hexdigest();(self.root/'queue.txt').write_bytes(self.blob)
  (self.root/'ready.json').write_text(json.dumps({'session':self.session}))
  self.status={'session':self.session,'active':False,'storage_error':False,'needs_explicit_resume':False,'batch':'','queue_sha256':''};self.save()
 def save(self):(self.root/'status.json').write_text(json.dumps(self.status))
 def tearDown(self):self.temp.cleanup()
 def call(self,*args):return subprocess.run([sys.executable,str(TOOL),*args],capture_output=True,text=True)
 def test_new_session_command_and_no_overwrite(self):
  r=self.call('command','START','--root',str(self.root));self.assertEqual(r.returncode,0,r.stderr)
  blob=(self.root/'command.txt').read_bytes();self.assertIn(self.session.encode(),blob);self.assertIn(self.hash.encode(),blob)
  r=self.call('command','START','--root',str(self.root));self.assertNotEqual(r.returncode,0);self.assertEqual((self.root/'command.txt').read_bytes(),blob)
 def test_stale_session_and_busy_refuse(self):
  self.status['session']='2'*32;self.save();self.assertNotEqual(self.call('command','START','--root',str(self.root)).returncode,0)
  self.status['session']=self.session;self.status['active']=True;self.save();self.assertNotEqual(self.call('command','START','--root',str(self.root)).returncode,0);self.assertFalse((self.root/'command.txt').exists())
 def test_resume_uses_immutable_snapshot(self):
  batch='0123456789abcdef0123456789abcdef';folder=self.root/'jobs'/batch;folder.mkdir(parents=True);(folder/'queue.txt').write_bytes(self.blob)
  (self.root/'queue.txt').write_bytes(b'wrong active queue')
  self.status.update(batch=batch,queue_sha256=self.hash,needs_explicit_resume=True);self.save()
  self.assertEqual(self.call('command','RESUME','--root',str(self.root)).returncode,0)
  self.assertIn(self.hash,(self.root/'command.txt').read_text())
 def test_symlink_ready_is_rejected(self):
  (self.root/'real.json').write_bytes((self.root/'ready.json').read_bytes());(self.root/'ready.json').unlink();(self.root/'ready.json').symlink_to(self.root/'real.json')
  self.assertNotEqual(self.call('command','START','--root',str(self.root)).returncode,0);self.assertFalse((self.root/'command.txt').exists())
 def test_prepare_capacity_dedupe_and_exclusion(self):
  entries=[{'id':f'NPWR{i:05}_00','sources':1} for i in range(98)]
  files=[{'path':f'ux0:app/PCSE{i:05}/sce_sys/trophy/NPWR{i+100:05}_00/TROPHY.TRP','bytes':100} for i in range(3)]
  catalog=self.root/'catalog.json';catalog.write_text(json.dumps({'schema':1,'complete':True,'scan_error':0,'entries':entries,'files':files}))
  out=self.root/'new';r=self.call('prepare','--catalog',str(catalog),'--output',str(out),'--limit','100');self.assertEqual(r.returncode,0,r.stderr)
  report=json.loads((out/'queue-report.json').read_text());self.assertEqual(len(report['items']),2);self.assertFalse(report['command_armed']);self.assertFalse((self.root/'command.txt').exists())
 def test_extended_window_preparation_and_command(self):
  catalog=self.root/'catalog.json';catalog.write_text(json.dumps({'schema':1,'complete':True,'scan_error':0,'entries':[],'files':[{'path':'ux0:app/PCSE00001/sce_sys/trophy/NPWR00001_00/TROPHY.TRP','bytes':100}]}))
  out=self.root/'slow';r=self.call('prepare','--catalog',str(catalog),'--output',str(out),'--max-run-ms','30000')
  self.assertEqual(r.returncode,0,r.stderr);blob=(out/'queue.txt').read_bytes()
  self.assertIn(b'window_ms=5000,30000',blob);self.assertEqual(json.loads((out/'queue-report.json').read_text())['max_ms'],30000)
  (self.root/'queue.txt').write_bytes(blob)
  self.assertEqual(self.call('command','START','--root',str(self.root)).returncode,0)
  self.assertIn(hashlib.sha256(blob).hexdigest(),(self.root/'command.txt').read_text())
 def test_unsupported_window_cannot_publish_command(self):
  for window in [b'5000,0',b'5000,60000',b'5000,30001',b'0,30000',b'5000,30000 extra']:
   (self.root/'queue.txt').write_bytes(self.blob.replace(b'5000,10000',window))
   self.assertNotEqual(self.call('command','START','--root',str(self.root)).returncode,0)
   self.assertFalse((self.root/'command.txt').exists())
if __name__=='__main__':unittest.main(verbosity=2)
