#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local-only editor server. Python standard library; no pip, cloud or accounts."""
from __future__ import annotations
import argparse, json, os, secrets, struct, subprocess, sys, threading, time, urllib.parse, webbrowser, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'tools'))
from native import Native, ROOT
from scene import metadata, atomic_write, safe_name, mlt_document, MAX_BYTES

def png(rgba:bytes,w:int,h:int)->bytes:
    def chunk(tag,data):return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data)&0xffffffff)
    stride=w*4; scan=b''.join(b'\0'+rgba[y*stride:(y+1)*stride] for y in range(h))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(scan,1))+chunk(b'IEND',b'')

class App:
    def __init__(self,export_root:Path):
        self.native=Native(); self.lock=threading.RLock(); self.data=None; self.token=secrets.token_urlsafe(32); self.revision=0
        self.export_root=export_root.expanduser().resolve();self.last_export=None
    def close(self):self.native.close()

class Handler(BaseHTTPRequestHandler):
    server_version='SUNIMOText/0.2'
    def log_message(self,format,*args):
        if args and str(args[1] if len(args)>1 else '') not in ('200','204'):super().log_message(format,*args)
    @property
    def app(self):return self.server.app
    def send(self,data:bytes,ctype='application/json; charset=utf-8',status=200):
        self.send_response(status);self.send_header('Content-Type',ctype);self.send_header('Content-Length',str(len(data)))
        self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff')
        self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; font-src 'self' blob:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
        self.end_headers()
        try:self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass
    def js(self,obj,status=200):self.send(json.dumps(obj,ensure_ascii=False).encode(),'application/json; charset=utf-8',status)
    def guard(self,api=True):
        host=self.headers.get('Host','')
        if host!=f'127.0.0.1:{self.server.server_port}':raise PermissionError('Недопустимый Host')
        origin=self.headers.get('Origin')
        if origin and origin!=f'http://{host}':raise PermissionError('Запрос с другого сайта запрещён')
        if api and self.headers.get('X-Sunimo-Token')!=self.app.token:raise PermissionError('Недействительный токен сессии')
    def do_GET(self):
        try:
            url=urllib.parse.urlparse(self.path);path=url.path;q=urllib.parse.parse_qs(url.query)
            self.guard(path.startswith('/api/'))
            if path=='/api/catalog':return self.send((ROOT/'web/catalog.json').read_bytes())
            if path=='/api/fonts':
                try:
                    proc=subprocess.run(['fc-list','--format','%{family}\n'],capture_output=True,text=True,timeout=5,check=True)
                    fonts=sorted({x.strip() for line in proc.stdout.splitlines() for x in line.split(',') if x.strip()},key=str.casefold)
                except (OSError,subprocess.SubprocessError):fonts=['sans-serif','serif','monospace']
                return self.js(fonts)
            if path=='/api/frame':
                t=float(q.get('t',['0'])[0]);w=int(q.get('w',['768'])[0]);h=int(q.get('h',['432'])[0])
                if not 64<=w<=1280 or not 64<=h<=1280 or w*h>1280*720:raise ValueError('Слишком большое превью')
                with self.app.lock:
                    if self.app.data is None:raise ValueError('Сначала создайте сцену')
                    if 'revision' in q and int(q['revision'][0])!=self.app.revision:raise ValueError('Сцена изменена в другом окне. Измените настройку для пересборки.')
                    image=self.app.native.render(t,w,h)
                if q.get('raw',['0'])[0]=='1':return self.send(image,'application/octet-stream')
                return self.send(png(image,w,h),'image/png')
            if path=='/api/scene':
                with self.app.lock:
                    if self.app.data is None:raise ValueError('Нет сцены')
                    return self.send(self.app.data,'application/octet-stream')
            static={'/':'index.html','/app.js':'app.js','/style.css':'style.css','/compiler.js':'compiler.js','/catalog.json':'catalog.json'}
            if path in static:
                p=ROOT/'web'/static[path];b=p.read_bytes()
                if path=='/':b=b.replace(b'__TOKEN__',self.app.token.encode())
                mime={'html':'text/html; charset=utf-8','js':'text/javascript; charset=utf-8','css':'text/css; charset=utf-8','json':'application/json'}[p.suffix[1:]]
                return self.send(b,mime)
            self.js({'error':'Не найдено'},404)
        except PermissionError as e:self.js({'error':str(e)},403)
        except (ValueError,RuntimeError,OverflowError,KeyError) as e:self.js({'error':str(e)},400)
        except Exception as e:self.js({'error':f'Ошибка сервера: {e}'},500)
    def do_POST(self):
        try:
            self.guard();path=urllib.parse.urlparse(self.path).path
            n=int(self.headers.get('Content-Length','0'))
            if n<0 or n>MAX_BYTES:raise ValueError('Размер запроса превышает 128 MiB')
            self.connection.settimeout(30);data=self.rfile.read(n)
            if len(data)!=n:raise ValueError('Неполный запрос')
            if path=='/api/scene':
                meta,cfg,w,h=metadata(data)
                with self.app.lock:
                    self.app.native.load(data);self.app.data=data;self.app.revision+=1
                return self.js({'ok':True,'revision':self.app.revision,'bytes':len(data)})
            if path=='/api/export':
                req=json.loads(data or b'{}');name=safe_name(str(req.get('name','Текст')))
                with self.app.lock:
                    if self.app.data is None:raise ValueError('Нет сцены')
                    if 'revision' in req and int(req['revision'])!=self.app.revision:raise ValueError('Сцена изменена в другом окне; экспорт отменён.')
                    meta,cfg,w,h=metadata(self.app.data)
                    self.app.export_root.mkdir(parents=True,exist_ok=True)
                    folder=self.app.export_root/name
                    if folder.exists():
                        suffix=1
                        while (self.app.export_root/f'{name} ({suffix})').exists():suffix+=1
                        folder=self.app.export_root/f'{name} ({suffix})'
                    folder.mkdir();scene=folder/(name+'.stxt');mlt=folder/(name+'.mlt')
                    atomic_write(scene,self.app.data);atomic_write(mlt,mlt_document(scene,cfg,w,h))
                    atomic_write(folder/(name+'.sunimo.json'),json.dumps(meta,ensure_ascii=False,indent=2).encode())
                    self.app.last_export=folder
                return self.js({'scene':str(scene),'mlt':str(mlt),'folder':str(folder),'frames':round(cfg[0]*cfg[23]/cfg[24])})
            if path=='/api/open-export':
                if not self.app.last_export:raise ValueError('Экспорт ещё не выполнен')
                subprocess.Popen(['xdg-open',str(self.app.last_export)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                return self.js({'ok':True})
            return self.js({'error':'Не найдено'},404)
        except PermissionError as e:self.js({'error':str(e)},403)
        except (ValueError,RuntimeError,OverflowError,KeyError) as e:self.js({'error':str(e)},400)
        except Exception as e:self.js({'error':f'Ошибка сервера: {e}'},500)

def main():
    p=argparse.ArgumentParser(description='SUNIMO Text Studio — локальный редактор титров')
    p.add_argument('--port',type=int,default=8767);p.add_argument('--no-browser',action='store_true');p.add_argument('--export-dir',type=Path,default=Path.home()/'Videos'/'SUNIMO Text')
    args=p.parse_args()
    try:
        app=App(args.export_dir);server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler)
    except (OSError,RuntimeError) as e:
        print(f'Не удалось запустить редактор: {e}',file=sys.stderr)
        print('Порт занят? Закройте предыдущий редактор или задайте --port 8768. Ошибка GLIBC? Прочитайте docs/INSTALL_RU.md.',file=sys.stderr)
        return 1
    server.daemon_threads=True;server.app=app
    url=f'http://127.0.0.1:{server.server_port}'
    print('SUNIMO Text Studio:',url,flush=True);print('Экспорт:',app.export_root,flush=True)
    if not args.no_browser:webbrowser.open(url)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close();app.close()
if __name__=='__main__':sys.exit(main() or 0)
