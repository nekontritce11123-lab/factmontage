from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
ROOT=Path(__file__).resolve().parents[1]
FONT=next((p for p in ['/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf','C:/Windows/Fonts/arial.ttf'] if Path(p).is_file()),None)
BOLD=next((p for p in ['/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf','C:/Windows/Fonts/arialbd.ttf'] if Path(p).is_file()),None)
def font(n,b=False):
    path=BOLD if b else FONT
    if path is None:raise RuntimeError('Install fonts-dejavu-core (Linux) or provide Arial (Windows)')
    return ImageFont.truetype(path,n)
def panel(w=1280,h=720,variant='dark'):
    # Deliberately rectangular input, no border/rounding/shadow/extrusion.
    im=Image.new('RGBA',(w,h),(23,31,43,255));d=ImageDraw.Draw(im)
    d.rectangle((0,0,w,14),fill=(244,175,59))
    d.text((65,49),'CARD3D',font=font(29,True),fill=(244,175,59))
    d.text((270,55),'CARD 3D  /  01',font=font(20),fill=(174,191,207))
    d.text((65,153),'Не просто картинка.',font=font(70,True),fill=(245,244,236))
    d.text((65,251),'Часть твоей истории.',font=font(51),fill=(214,227,240))
    d.line((65,367,w-65,367),fill=(75,90,109),width=2)
    # Overlapping test panels and straight grid: projective detail and crop proof.
    d.rectangle((65,409,606,642),fill=(242,174,61))
    d.text((92,434),'ОДИН ЭФФЕКТ',font=font(26,True),fill=(24,30,40))
    d.text((92,490),'Вход. Форма. Объём.',font=font(29),fill=(24,30,40))
    d.text((92,563),'Без ручных ключей',font=font(24),fill=(58,60,65))
    for x in range(666,1217,55):d.line((x,410,x,642),fill=(68,94,113),width=2)
    for y in range(410,646,46):d.line((666,y,1214,y),fill=(68,94,113),width=2)
    d.ellipse((821,445,1009,633),outline=(244,175,59),width=8)
    return im

def paper(w=1280,h=720):
    im=Image.new('RGBA',(w,h),(248,245,234,255));d=ImageDraw.Draw(im)
    d.text((63,47),'ЗАМЕТКА / CARD3D',font=font(25,True),fill=(143,96,43))
    d.line((63,115,w-63,115),fill=(196,185,165),width=2)
    d.text((63,161),'История начинается',font=font(64,True),fill=(45,43,39))
    d.text((63,247),'с маленькой детали.',font=font(64,True),fill=(45,43,39))
    lines=['Рваные края закреплены на листе.', 'Тень следует за формой карточки.', 'Текст и рамка движутся вместе.']
    for i,line in enumerate(lines):d.text((65,384+61*i),line,font=font(34),fill=(100,91,77))
    d.text((65,631),'ВХОД → ОФОРМЛЕНИЕ → ПЕРСПЕКТИВА',font=font(22,True),fill=(143,96,43))
    return im

def portrait(w=1280,h=720):
    im=Image.new('RGBA',(w,h),(0,0,0,0));card=Image.new('RGBA',(390,650),(24,33,47,255));d=ImageDraw.Draw(card)
    d.text((28,35),'CARD3D',font=font(26,True),fill=(242,174,61))
    d.rectangle((28,105,362,351),fill=(242,174,61))
    d.ellipse((98,136,293,328),outline=(35,43,56),width=7)
    d.text((28,395),'Вертикальный',font=font(31,True),fill=(242,242,232))
    d.text((28,443),'источник',font=font(40,True),fill=(242,242,232))
    d.text((28,548),'Пропорции',font=font(28),fill=(181,196,209))
    d.text((28,591),'сохраняются.',font=font(28),fill=(181,196,209))
    im.alpha_composite(card,((w-card.width)//2,(h-card.height)//2));return im
if __name__=='__main__':
    for name,im in [('source_panel.png',panel()),('source_paper.png',paper()),('source_portrait.png',portrait())]:im.save(ROOT/'demo'/name)
