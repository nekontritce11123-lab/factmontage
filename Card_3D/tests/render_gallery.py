#!/usr/bin/env python3
"""Extended previews using the real public plugin, without changing its controls.
The gallery is a viewer, not a Kdenlive panel or a preview of an unbuilt camera.
"""
from contextlib import ExitStack
from pathlib import Path
import hashlib
import html
import json
import sys
import numpy as np
from PIL import Image, ImageDraw
from host import Plugin, ROOT, library_path
from make_assets import panel, paper, portrait, font

OUT=ROOT/'build/native/preview/gallery'
FPS=15
BACKGROUND=(230,234,241,255)
SOURCES={'panel':panel(),'paper':paper(),'portrait':portrait()}
ITEMS=[]


def source(name,w,h):
    return np.array(SOURCES[name].resize((w,h),Image.Resampling.LANCZOS))


def save(name,title,description,frames,settings):
    path=OUT/(name+'.gif')
    durations=[60 if i%3==0 else 70 for i in range(len(frames))]
    frames[0].save(path,save_all=True,append_images=frames[1:],duration=durations,loop=0,disposal=2)
    frames[min(len(frames)-1,round(2.0*FPS))].save(OUT/(name+'.png'))
    with Image.open(path) as check:
        assert check.n_frames>1 and check.size==frames[0].size
    ITEMS.append(dict(name=name,title=title,description=description,frames=len(frames),fps=FPS,settings=settings))
    print(f'{title}: {len(frames)} frames, {path.stat().st_size//1024} KB',flush=True)


def comparison(name,title,description,variants,seconds,cols,w=480,h=270):
    rows=(len(variants)+cols-1)//cols
    frames=[]
    with ExitStack() as stack:
        plugins=[stack.enter_context(Plugin(w,h)).set(PLACEMENT=0,SIZE=84,**v['settings']) for v in variants]
        inputs=[source(v.get('source','panel'),w,h) for v in variants]
        for n in range(seconds*FPS):
            t=n/FPS
            canvas=Image.new('RGBA',(cols*w,rows*(h+34)+54),BACKGROUND)
            draw=ImageDraw.Draw(canvas)
            draw.text((16,10),title,font=font(20,True),fill=(30,41,57))
            for i,(p,v,src) in enumerate(zip(plugins,variants,inputs)):
                x=(i%cols)*w;y=44+(i//cols)*(h+34)
                draw.text((x+16,y),v['title'],font=font(16,True),fill=(43,56,73))
                if v.get('dynamic'):
                    src=src.copy();left=15+int(t*100)%(w-100)
                    src[h-28:h-19,15:w-15]=[65,87,110,255]
                    src[h-28:h-19,left:left+60]=[244,175,59,255]
                frame=Image.fromarray(p.render(src,t))
                canvas.alpha_composite(frame,(x,y+26))
            draw.text((16,canvas.height-19),'Реальный рендер плагина · не интерфейс Kdenlive',font=font(12),fill=(76,88,104))
            frames.append(canvas.convert('RGB'))
    save(name,title,description,frames,[v['settings'] for v in variants])


def layered_scene():
    w,h=960,540
    src1=source('portrait',w,h);src2=source('paper',w,h)
    first=dict(STYLE=2,PLACEMENT=1,SIZE=86,ENTRANCE=3,CORNER=3,MOTION=1)
    second=dict(STYLE=1,PLACEMENT=2,SIZE=61,ENTRANCE=2,CORNER=0,MOTION=2)
    frames=[]
    with Plugin(w,h).set(**first) as a,Plugin(w,h).set(**second) as b:
        for n in range(6*FPS):
            t=n/FPS
            canvas=Image.new('RGBA',(w,h+70),(21,29,42,255))
            # Two instances model two timeline layers. The background and
            # compositing are external to the plugin, as they are in an editor.
            picture=Image.new('RGBA',(w,h),(21,29,42,255))
            picture=Image.alpha_composite(picture,Image.fromarray(a.render(src1,t)))
            picture=Image.alpha_composite(picture,Image.fromarray(b.render(src2,t-.35)))
            canvas.alpha_composite(picture,(0,44));draw=ImageDraw.Draw(canvas)
            draw.text((22,10),'Две карточки в одной сцене',font=font(24,True),fill=(243,239,229))
            draw.text((22,h+46),'Два экземпляра Card 3D · фон и сборка слоёв снаружи · общей камеры пока нет',font=font(15),fill=(188,203,223))
            frames.append(canvas.convert('RGB'))
    save('scene','Две карточки в одной сцене','Пример двух слоёв: разное оформление и движение. Вторая карточка начинает вход на 0,35 с позже — как клип, сдвинутый на таймлайне. Общая камера ещё не реализована.',frames,[first,second])


def gallery():
    cards=[]
    for i in ITEMS:
        name=i['name']
        cards.append(f'''<article id="{name}"><div class="section-title"><h2>{html.escape(i['title'])}</h2><span>{i['frames']/FPS:g} сек</span></div>
<p>{html.escape(i['description'])}</p><a href="{name}.gif" target="_blank" rel="noopener"><img src="{name}.png" data-base="{name}" alt="{html.escape(i['title'])}"></a>
<div class="controls"><button type="button" data-target="{name}">▶ Смотреть движение</button><a href="{name}.gif" target="_blank" rel="noopener">Открыть крупно ↗</a></div></article>''')
    page='''<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Card 3D — расширенное превью</title><style>
:root{color-scheme:dark;font-family:Segoe UI,Arial,sans-serif;background:#111822;color:#edf1f6}*{box-sizing:border-box}body{margin:0}main{max-width:1180px;margin:auto;padding:42px 22px 70px}header{margin-bottom:34px}.eyebrow{color:#e5b772;letter-spacing:.12em;font-size:12px}h1{font-size:clamp(28px,5vw,46px);margin:12px 0}header p{max-width:780px;color:#bdc9d8;font-size:17px;line-height:1.55}.badge{display:inline-block;padding:7px 11px;margin-right:6px;background:#223043;border-radius:7px;font-size:13px}.warning{border-left:3px solid #e5b772;padding-left:14px;color:#c7cfd9}nav{display:flex;flex-wrap:wrap;gap:9px;margin:23px 0}a{color:#e5b772;text-underline-offset:4px}nav a{padding:9px 12px;background:#202d3e;border-radius:6px;text-decoration:none}article{background:#192332;border:1px solid #2d3c50;border-radius:14px;margin:24px 0;padding:20px}.section-title{display:flex;align-items:center;justify-content:space-between;gap:16px}h2{font-size:24px;margin:0}.section-title span{color:#aab8cc;white-space:nowrap}article p{color:#b8c7d9;line-height:1.5;max-width:980px}img{display:block;width:100%;height:auto;border-radius:8px;background:#e6eaf1}.controls{display:flex;gap:20px;align-items:center;margin-top:16px;flex-wrap:wrap}button{background:#f0bf77;border:0;border-radius:7px;color:#182231;font:600 15px Segoe UI,Arial;padding:12px 18px;cursor:pointer;min-height:44px}button:focus-visible,a:focus-visible{outline:3px solid #8dc9ff;outline-offset:4px}footer{font-size:14px;line-height:1.6;color:#aab8cc}@media(max-width:650px){main{padding:24px 12px}article{padding:12px}h2{font-size:20px}}
</style><main><header><div class="eyebrow">CARD 3D / РАБОТАЮЩИЕ ВОЗМОЖНОСТИ</div><h1>Посмотри, как ведёт себя карточка</h1>
<p>Шесть сравнений: оформление, появление, характер движения, углы входа, разные источники и сцена из двух карточек. В каждом примере работает настоящая собранная библиотека.</p>
<span class="badge">3 оформления</span><span class="badge">4 появления + без появления</span><span class="badge">3 режима движения</span>
<p class="warning">Это офлайн-рендер Windows DLL, не запись Kdenlive и не измерение скорости Steam Deck. Камера пока не добавлена. Кнопка «Стоп-кадр» возвращает обзорный кадр, а не останавливает GIF в текущий момент.</p>
<nav>'''+''.join(f'<a href="{chr(35)+i["name"]}">{html.escape(i["title"])}</a>' for i in ITEMS)+'''</nav></header>'''+''.join(cards)+'''
<footer>Плагин создаёт форму, оформление и движение карточки. Фон, подписи и объединение независимых слоёв добавлены при сборке демонстрации. Частота GIF — 15 кадров/с, это формат превью, а не производительность плагина.</footer></main>
<script>document.querySelectorAll('button[data-target]').forEach(button=>button.addEventListener('click',()=>{const image=document.querySelector('#'+button.dataset.target+' img');const playing=button.dataset.playing==='true';image.src=image.dataset.base+(playing?'.png':'.gif?restart='+Date.now());button.dataset.playing=String(!playing);button.textContent=playing?'▶ Смотреть движение':'▣ Стоп-кадр';button.setAttribute('aria-pressed',String(!playing));}));</script></html>'''
    (OUT/'index.html').write_text(page,encoding='utf-8',newline='\n')


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    comparison('entrances','Четыре появления','Одинаковая карточка, размер и угол входа. Отличается только появление; движение после входа отключено.',
                [dict(title=title,settings=dict(ENTRANCE=i+1,MOTION=0,DURATION=1.25)) for i,title in enumerate(['Мягкий выезд','Выезд с вытягиванием','Из глубины','Раскрытие'])],4,2)
    comparison('motion','Три характера движения','Появление отключено. Слева карточка стоит прямо; справа два готовых режима перспективы. Сравнивать удобнее по границам и линиям внутри.',
                [dict(title=title,settings=dict(ENTRANCE=0,MOTION=i)) for i,title in enumerate(['Без движения','Спокойное','Живое'])],8,3,384,216)
    comparison('styles','Три оформления','Один и тот же исходник: плагин меняет край, рамку, скругление и торец. Тень и перспектива вычисляются вместе с карточкой.',
                [dict(title=title,settings=dict(STYLE=i,ENTRANCE=0,MOTION=1)) for i,title in enumerate(['Панель','Бумага','Фото'])],4,3,384,216)
    comparison('corners','Четыре угла входа','Конечное положение одинаковое — по центру. «Откуда» управляет появлением независимо от расположения.',
                [dict(title=title,settings=dict(CORNER=i,ENTRANCE=2,MOTION=0,DURATION=1.25)) for i,title in enumerate(['Справа снизу','Слева снизу','Справа сверху','Слева сверху'])],4,2)
    layered_scene()
    comparison('sources','Изображение и видео','Слева PNG с прозрачными полями: карточка подстраивается под вертикальное содержимое. Справа содержимое обновляется каждый кадр; границы остаются стабильными.',
                [dict(title='Изображение: прозрачные поля',source='portrait',settings=dict(STYLE=2,ENTRANCE=0,MOTION=1,SOURCE=0)),
                 dict(title='Видео: меняющееся содержимое',dynamic=True,settings=dict(STYLE=0,ENTRANCE=0,MOTION=1,SOURCE=1))],5,2)
    gallery()
    metadata=dict(scope='Offline native public plugin rendering; not Kdenlive, Linux acceptance or Deck performance',
                  platform=sys.platform,binary_sha256=hashlib.sha256(library_path().read_bytes()).hexdigest(),items=ITEMS)
    (OUT/'gallery_info.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(OUT/'index.html')


if __name__=='__main__':main()
