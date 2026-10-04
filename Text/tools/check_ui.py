import sys,threading,tempfile,json,base64,time,http.client,re
from pathlib import Path
from playwright.sync_api import sync_playwright
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from studio import App,Handler,ThreadingHTTPServer
scratch=tempfile.TemporaryDirectory();OUTPUT=Path(scratch.name);app=App(OUTPUT/'exports')
server=ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True;server.app=app
threading.Thread(target=server.serve_forever,daemon=True).start()
try:
 with sync_playwright() as p:
  browser=p.chromium.launch(executable_path='/usr/bin/chromium',headless=True,args=['--no-sandbox'])
  page=browser.new_page(viewport={'width':1440,'height':1000});errs=[];page.on('pageerror',lambda e:errs.append(str(e)))
  # The container Chromium blocks navigation to localhost. Test DOM/Canvas from local source
  # in about:blank; fetch is bridged to the actual local HTTP server via Python.
  def bridge(req):
   conn=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=30)
   conn.request(req['method'],req['url'],base64.b64decode(req['body']) if req['body'] else None,req['headers'])
   res=conn.getresponse();data=res.read();out={'status':res.status,'data':base64.b64encode(data).decode(),'type':res.getheader('Content-Type')};conn.close();return out
  page.expose_function('testHTTP',bridge)
  html=(ROOT/'web/index.html').read_text().replace('__TOKEN__',app.token)
  html=re.sub(r'<script[^>]*src="/app.js"[^>]*></script>','',html)
  html=re.sub(r'<link[^>]*href="/style.css"[^>]*>','',html)
  page.set_content(html)
  page.add_style_tag(content=(ROOT/'web/style.css').read_text())
  page.add_script_tag(content="""window.fetch=async(url,options={})=>{
    let body=options.body,encoded='';if(body){const a=typeof body==='string'?new TextEncoder().encode(body):new Uint8Array(body);for(let i=0;i<a.length;i+=8192)encoded+=String.fromCharCode(...a.subarray(i,i+8192));encoded=btoa(encoded);}
    const r=await testHTTP({url,method:options.method||'GET',headers:options.headers||{},body:encoded});
    return new Response(Uint8Array.from(atob(r.data),c=>c.charCodeAt(0)),{status:r.status,headers:{'Content-Type':r.type}});
  };""")
  compiler=(ROOT/'web/compiler.js').read_text().replace('export ','')
  appjs=re.sub(r'^import .*?;','',(ROOT/'web/app.js').read_text(),flags=re.M)
  page.add_script_tag(content='(()=>{'+compiler+';window.testCompiler={compileScene,defaults,sanitize};'+appjs+'})()')
  page.wait_for_function('window.sunimo && window.sunimo.compiled', timeout=30000)
  page.evaluate('window.sunimo.pause()');print('default',page.evaluate('window.sunimo.state'))
  assert page.locator('[data-key=life]').input_value()=='9'
  page.evaluate("window.sunimo.setState({text:'ЖИВОЙ ТЕКСТ',size:152,keywords:'ТЕКСТ',name:'Живой текст v0.2',duration:8,inDuration:1.2,outDuration:1.0,inPreset:47,group:0,order:0,life:9,lifeSource:0,lifeAmount:.85,lifeSpeed:1,shadow:.2,spacing:0})")
  page.evaluate('window.sunimo.pause()')
  data=page.evaluate('Array.from(new Uint8Array(window.sunimo.compiled.buffer))');(OUTPUT/'life_v02.stxt').write_bytes(bytes(data))
  page.evaluate('window.sunimo.seek(3.5)');page.wait_for_timeout(200)
  page.locator('[data-tab=motion]').click();page.screenshot(path=str(OUTPUT/'interface_v02.png'))
  page.locator('#lifeOnlyBtn').click();page.wait_for_function('window.sunimo.compiled.state.inPreset===0 && window.sunimo.compiled.state.lifeSource===47')
  page.evaluate('window.sunimo.pause()');assert page.evaluate('window.sunimo.state.outPreset')==0
  data=page.evaluate('Array.from(new Uint8Array(window.sunimo.compiled.buffer))');(OUTPUT/'life_only_v02.stxt').write_bytes(bytes(data))
  # Validate static typographic layout: anchors in each line are ordered and share a baseline.
  result=page.evaluate('''async()=>{const {compileScene}=window.testCompiler;
   const c=await compileScene({...window.sunimo.state,text:'AVATAR fi ffi office\\nЙОГА, ТЕКСТ! ура',font:'DejaVu Sans',size:96,life:0,spacing:0});
   const v=new DataView(c.buffer),meta=v.getUint32(12,true),n=v.getUint32(16,true);let o=160+meta,units=[];
   for(let i=0;i<n;i++){units.push({x:v.getFloat32(o,true),ax:v.getFloat32(o+16,true),ay:v.getFloat32(o+20,true),line:v.getUint32(o+36,true)});o+=48+v.getUint32(o+44,true);}
   return {version:v.getUint32(8,true),units};}''')
  assert result['version']==2
  for line in set(u['line'] for u in result['units']):
   row=[u for u in result['units'] if u['line']==line];assert all(b['ax']>a['ax'] for a,b in zip(row,row[1:]));assert max(u['ay'] for u in row)-min(u['ay'] for u in row)<.001
  # Rapid seek should eventually display the last requested frame, not an earlier pending one.
  page.evaluate('window.sunimo.seek(1);window.sunimo.seek(6);window.sunimo.seek(4.5)');page.wait_for_timeout(300)
  assert page.evaluate('Number(document.querySelector("#seek").value)')==4.5
  print('UI errors:',errs);assert not errs
  print('PASS: boot, auto profile, native v2 compile, life-only, layout anchors, rapid seek, screenshot')
  browser.close()
finally:server.shutdown();server.server_close();app.close();scratch.cleanup()
