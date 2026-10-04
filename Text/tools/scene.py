# SPDX-License-Identifier: MIT
"""Document helpers; the native decoder remains the authoritative safety check."""
from __future__ import annotations
import json, math, os, re, struct, tempfile
from pathlib import Path
import xml.etree.ElementTree as ET
MAGIC=b'SUNMTX1\0'
MAX_BYTES=128*1024*1024

def metadata(data:bytes)->tuple[dict,list[float],int,int]:
    if len(data)<160 or data[:8]!=MAGIC:raise ValueError('Это не файл SUNIMO .stxt')
    version,ml,count,w,h,cn=struct.unpack_from('<6I',data,8)
    if version not in (1,2,3) or cn!=32 or ml>2*1024*1024 or count>512 or 160+ml>len(data):raise ValueError('Неверный заголовок .stxt')
    cfg=list(struct.unpack_from('<32f',data,32))
    if not all(math.isfinite(v) for v in cfg):raise ValueError('Некорректные параметры')
    return json.loads(data[160:160+ml]),cfg,w,h

def with_config(data:bytes,changes:dict[int,float])->bytes:
    out=bytearray(data)
    for key,value in changes.items():
        if not 0<=key<32 or not math.isfinite(value):raise ValueError('Invalid config patch')
        struct.pack_into('<f',out,32+4*key,value)
    return bytes(out)

def atomic_write(path:Path,data:bytes)->None:
    path.parent.mkdir(parents=True,exist_ok=True)
    fd,tmp=tempfile.mkstemp(prefix='.'+path.name+'.',dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as f:f.write(data);f.flush();os.fsync(f.fileno())
        os.replace(tmp,path)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)

def safe_name(name:str)->str:
    name=re.sub(r'[^\w .()-]','_',name,flags=re.UNICODE).strip(' .')[:80]
    return name or 'Текст'

def mlt_document(scene_path:Path,cfg:list[float],w:int,h:int)->bytes:
    num,den=int(cfg[23]),int(cfg[24]); fps=num/den
    frames=max(1,math.ceil(cfg[0]*fps-1e-5)); end=frames-1
    root=ET.Element('mlt',{'producer':'main','version':'7.0.0','LC_NUMERIC':'C'})
    ET.SubElement(root,'profile',{'description':'SUNIMO Text','width':str(w),'height':str(h),'frame_rate_num':str(num),'frame_rate_den':str(den),'progressive':'1','sample_aspect_num':'1','sample_aspect_den':'1','display_aspect_num':str(w),'display_aspect_den':str(h),'colorspace':'709'})
    p=ET.SubElement(root,'producer',{'id':'text','in':'0','out':str(end)})
    def prop(parent,name,value):ET.SubElement(parent,'property',{'name':name}).text=str(value)
    for k,v in {'mlt_service':'color','resource':'0x00000000','mlt_image_format':'rgba','length':frames,'eof':'pause'}.items():prop(p,k,v)
    fx=ET.SubElement(p,'filter',{'in':'0','out':str(end)})
    for k,v in {'mlt_service':'frei0r.sunimo_text_studio','0':str(scene_path.resolve()),'1':0,'2':.5,'threads':1,'kdenlive:id':'sunimo_text_studio'}.items():prop(fx,k,v)
    play=ET.SubElement(root,'playlist',{'id':'main'});ET.SubElement(play,'entry',{'producer':'text','in':'0','out':str(end)})
    ET.indent(root)
    return ET.tostring(root,encoding='utf-8',xml_declaration=True)
