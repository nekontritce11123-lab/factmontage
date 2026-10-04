# SPDX-License-Identifier: MIT
from __future__ import annotations
import http.client,json,sys,tempfile,threading,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from studio import App,Handler,ThreadingHTTPServer
from tools.install import merged_paths

class ServerTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();cls.app=App(Path(cls.tmp.name))
  cls.server=ThreadingHTTPServer(('127.0.0.1',0),Handler);cls.server.daemon_threads=True;cls.server.app=cls.app
  cls.thread=threading.Thread(target=cls.server.serve_forever,daemon=True);cls.thread.start()
  cls.data=(ROOT/'examples/demo.stxt').read_bytes()
 @classmethod
 def tearDownClass(cls):
  cls.server.shutdown();cls.server.server_close();cls.thread.join();cls.app.close();cls.tmp.cleanup()
 def req(self,path='/',body=None,auth=True,headers=None):
  connection=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=10)
  h={'X-Sunimo-Token':self.app.token} if auth else {};h.update(headers or {})
  connection.request('POST' if body is not None else 'GET',path,body,headers=h)
  response=connection.getresponse();code=response.status;data=response.read();connection.close();return code,data
 def test_root_and_static(self):
  status,data=self.req('/',auth=False);self.assertEqual(status,200);self.assertIn(self.app.token.encode(),data)
  self.assertNotIn(b'__TOKEN__',data)
  self.assertEqual(self.req('/../studio.py')[0],404)
 def test_auth_origin_and_dns_rebinding(self):
  self.assertEqual(self.req('/api/catalog',auth=False)[0],403)
  self.assertEqual(self.req('/api/catalog',headers={'Origin':'https://untrusted.example'})[0],403)
  self.assertEqual(self.req('/',headers={'Host':'untrusted.example'})[0],403)
 def test_invalid_scene_is_rejected(self):
  self.assertEqual(self.req('/api/scene',self.data)[0],200);rev=self.app.revision
  self.assertEqual(self.req('/api/scene',b'invalid')[0],400);self.assertEqual(self.app.revision,rev)
  self.assertEqual(self.req('/api/scene')[1],self.data)
 def test_frame_png_and_invalid_dimensions(self):
  self.req('/api/scene',self.data)
  status,data=self.req('/api/frame?t=0.4&w=320&h=180');self.assertEqual(status,200);self.assertTrue(data.startswith(b'\x89PNG\r\n\x1a\n'))
  self.assertEqual(self.req('/api/frame?t=nan&w=320&h=180')[0],400)
  self.assertEqual(self.req('/api/frame?w=999999&h=99999')[0],400)
 def test_raw_preview_matches_native(self):
  self.req('/api/scene',self.data)
  status,data=self.req('/api/frame?t=1.5&w=320&h=180&raw=1');self.assertEqual(status,200)
  with self.app.lock:self.assertEqual(data,self.app.native.render(1.5,320,180))
 def test_exports_do_not_overwrite(self):
  self.req('/api/scene',self.data)
  req=json.dumps({'name':'../../Текст & тест'}).encode()
  a=json.loads(self.req('/api/export',req)[1]);b=json.loads(self.req('/api/export',req)[1])
  self.assertNotEqual(a['folder'],b['folder'])
  self.assertEqual(Path(a['folder']).parent.resolve(),Path(self.tmp.name).resolve())
  self.assertEqual(Path(a['scene']).read_bytes(),self.data)
 def test_stale_tab_cannot_export_another_scene(self):
  self.req('/api/scene',self.data);old=self.app.revision;self.req('/api/scene',self.data)
  self.assertEqual(self.req(f'/api/frame?t=0.4&w=320&h=180&revision={old}')[0],400)
  self.assertEqual(self.req('/api/export',json.dumps({'name':'old','revision':old}).encode())[0],400)
 def test_search_paths_preserve_other_plugins(self):
  value=merged_paths('/own','/old:/with space:/old',['/app/lib/frei0r-1','/own'])
  self.assertEqual(value,'/own:/old:/with space:/app/lib/frei0r-1')
if __name__=='__main__':unittest.main(verbosity=2)
