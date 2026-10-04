#!/usr/bin/env python3
"""Demo made with the actual public Card 3D binary. Captions/background only in Python.
--preview-only writes a native preview, never claims to be Kdenlive or a Linux release.
"""
import argparse
import hashlib
import json
import subprocess
import sys
import numpy as np
from PIL import Image, ImageDraw
from host import Plugin, ROOT, library_path
from make_assets import font, panel, paper, portrait


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--preview-only',action='store_true');args=parser.parse_args()
    folder=ROOT/('build/native/preview' if args.preview_only else 'demo');folder.mkdir(parents=True,exist_ok=True)
    width,height=640,360
    sources={name:np.array(im.resize((width,height),Image.Resampling.LANCZOS)) for name,im in [('panel',panel()),('paper',paper()),('portrait',portrait())]}
    scenes=[
        ('Панель','Мягкий выезд · спокойное движение','panel',dict(STYLE=0,ENTRANCE=1,MOTION=1),3),
        ('Бумага','Выезд с вытягиванием · живое движение','paper',dict(STYLE=1,ENTRANCE=2,MOTION=2),3),
        ('Фото','Из глубины · без движения','panel',dict(STYLE=2,ENTRANCE=3,MOTION=0),3),
        ('Раскрытие','Раскрытие справа сверху','paper',dict(STYLE=1,ENTRANCE=4,CORNER=2,MOTION=0),3),
        ('Вертикальная картинка','Источник: Изображение','portrait',dict(STYLE=2,ENTRANCE=1,SOURCE=0,MOTION=1),3),
        ('Меняющийся источник','Источник: Видео · стабильные границы','panel',dict(STYLE=0,ENTRANCE=0,SOURCE=1,MOTION=2),3),
    ]
    def frame_for(p,source,t,title,subtitle,dynamic=False):
        src=sources[source]
        if dynamic:
            src=src.copy();x=40+int(t*95)%480;src[height-22:height-16,x:x+60]=[242,174,61,255]
        result=p.render(src,t)
        frame=Image.new('RGBA',(width,height),(229,233,240,255))
        frame=Image.alpha_composite(frame,Image.fromarray(result))
        draw=ImageDraw.Draw(frame)
        draw.text((18,9),'Card 3D / '+title,font=font(20,True),fill=(29,39,56))
        draw.text((18,36),subtitle,font=font(13),fill=(63,73,86))
        label='Предпросмотр Windows DLL; не Kdenlive' if sys.platform=='win32' else 'Рендер Linux .so; не запись Kdenlive'
        draw.text((18,height-23),label,font=font(12),fill=(63,73,86))
        return frame.convert('RGB')

    # Contact sheet: all styles and a portrait, on the same uncluttered background.
    poster=Image.new('RGB',(width*2,height*2),(229,233,240))
    for i,style in enumerate((0,1,2,2)):
        title=['Панель','Бумага','Фото','Вертикальная картинка'][i]
        with Plugin(width,height).set(STYLE=style,PLACEMENT=0,SIZE=86,ENTRANCE=0,MOTION=0) as p:
            frame=frame_for(p,'portrait' if i==3 else 'paper' if style==1 else 'panel',2,title,'Готовый пресет · без ручных наклонов')
        poster.paste(frame,((i%2)*width,(i//2)*height))
    poster.save(folder/'presets.png')

    if args.preview_only:
        frames=[]
        with Plugin(width,height).set(STYLE=1,PLACEMENT=0,SIZE=86,ENTRANCE=2,MOTION=2) as p:
            for n in range(48):
                frames.append(frame_for(p,'paper',n/12,'Бумага','Появление и живое движение'))
        frames[0].save(folder/'card3d_preview.gif',save_all=True,append_images=frames[1:],duration=83,loop=0)
        count=48
    else:
        if sys.platform=='win32':raise SystemExit('Release showcase must be rendered from the verified Linux .so')
        output=folder/'Card_3D_Showcase.mp4'
        command=['ffmpeg','-v','error','-y','-f','rawvideo','-pix_fmt','rgb24','-s',f'{width}x{height}','-r','30','-i','-',
                 '-an','-c:v','libx264','-preset','veryfast','-crf','18','-pix_fmt','yuv420p','-movflags','+faststart',str(output)]
        proc=subprocess.Popen(command,stdin=subprocess.PIPE)
        count=0
        try:
            for title,subtitle,source,settings,seconds in scenes:
                with Plugin(width,height).set(PLACEMENT=0,SIZE=86,**settings) as p:
                    for n in range(seconds*30):
                        frame=frame_for(p,source,n/30,title,subtitle,settings.get('SOURCE')==1)
                        proc.stdin.write(np.asarray(frame).tobytes());count+=1
            proc.stdin.close()
            if proc.wait()!=0:raise RuntimeError('FFmpeg encoding failed')
        except BaseException:
            proc.kill();proc.wait();raise
    meta=dict(scope='native renderer only; not Kdenlive GUI/Deck',platform=sys.platform,
              binary=str(library_path()),binary_sha256=hashlib.sha256(library_path().read_bytes()).hexdigest(),frames=count)
    (folder/'render_info.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'Demo/preview: {folder}; {count} frames')


if __name__=='__main__':main()
