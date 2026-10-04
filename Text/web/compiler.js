// SPDX-License-Identifier: MIT
// Glyphs are rasterized ONCE. The same resulting scene is used for native preview and MLT.
export const defaults={
 text:'Буквы оживают.\nИстория начинается.',font:'sans-serif',size:96,weight:700,italic:false,spacing:0,lineHeight:1.18,
 color:'#fff9ef',accent:'#ffc369',keywords:'оживают',stroke:0,strokeColor:'#191923',shadow:0.4,
 align:'center',anchor:'center',x:50,y:50,width:84,format:'1920x1080',fps:'30/1',backgroundBox:false,boxColor:'#171b24',boxOpacity:.8,
 duration:4,inDuration:.85,outDuration:.7,inPreset:2,outPreset:-1,group:0,order:0,strength:.8,stagger:.32,
 life:9,lifeSource:0,lifeAmount:.65,lifeSpeed:1,secondary:0,secondaryMix:.3,focus:0,echo:0,motionBlur:0,quality:1,seed:24191,opacity:1,
 name:'Живой текст',version:2
};
const graphemes=s=>typeof Intl.Segmenter==='function'?[...new Intl.Segmenter('ru',{granularity:'grapheme'}).segment(s)].map(z=>z.segment):Array.from(s.normalize('NFC'));
const makeCanvas=(w,h)=>{const c=document.createElement('canvas');c.width=w;c.height=h;return c};
function roundRect(ctx,x,y,w,h,r){r=Math.min(r,w/2,h/2);ctx.beginPath();ctx.moveTo(x+r,y);ctx.arcTo(x+w,y,x+w,y+h,r);ctx.arcTo(x+w,y+h,x,y+h,r);ctx.arcTo(x,y+h,x,y,r);ctx.arcTo(x,y,x+w,y,r);ctx.closePath();}
export function sanitize(value){
 const s={...defaults,...value};s.version=2;
 // Old default drafts opt into the new auto profile; explicit old choices remain available.
 if(value?.version===1&&value.life===1&&value.lifeAmount===.25){s.life=9;s.lifeAmount=.65;s.lifeSpeed=1;}
 const ranges={size:[12,320],weight:[100,900],spacing:[-5,30],lineHeight:[.85,2],stroke:[0,12],shadow:[0,1],x:[0,100],y:[0,100],width:[10,98],boxOpacity:[0,1],duration:[.1,120],inDuration:[0,6],outDuration:[0,6],inPreset:[0,100],outPreset:[-1,100],group:[0,3],order:[0,5],strength:[0,1.5],stagger:[0,.9],life:[0,9],lifeSource:[0,100],lifeAmount:[0,1],lifeSpeed:[.2,2],secondary:[0,100],secondaryMix:[0,1],focus:[0,1],echo:[0,1],motionBlur:[0,1],quality:[0,2],seed:[0,16777215],opacity:[0,1]};
 for(const [key,[lo,hi]] of Object.entries(ranges)){let v=Number(s[key]);s[key]=Number.isFinite(v)?Math.max(lo,Math.min(hi,v)):defaults[key];}
 for(const key of ['inPreset','outPreset','group','order','life','lifeSource','secondary','quality','seed'])s[key]=Math.round(s[key]);
 for(const key of ['text','font','keywords','name'])s[key]=String(s[key]).slice(0,key==='text'?4096:256);
 for(const key of ['color','accent','strokeColor','boxColor'])if(!/^#[0-9a-f]{6}$/i.test(s[key]))s[key]=defaults[key];
 if(!['1920x1080','1080x1920','1080x1080','1280x720','3840x2160'].includes(s.format))s.format=defaults.format;
 if(!['24/1','25/1','30/1','50/1','60/1','24000/1001','30000/1001','60000/1001'].includes(s.fps))s.fps=defaults.fps;
 if(!['left','center','right'].includes(s.align))s.align='center';
 return s;
}
let cachedLayout=null;
export function clearLayoutCache(){cachedLayout=null;}
async function layoutScene(s){
 const keys=['text','font','size','weight','italic','spacing','lineHeight','color','accent','keywords','stroke','strokeColor','shadow','align','x','y','width','format','backgroundBox','boxColor','boxOpacity'];
 const key=JSON.stringify(keys.map(k=>s[k]));
 if(cachedLayout?.key===key)return cachedLayout.value;
 const [W,H]=s.format.split('x').map(Number);const scale=Math.min(W,H)/1080;
 const fontSize=s.size*scale;const spec=`${s.italic?'italic ':''}${s.weight} ${fontSize}px "${s.font.replace(/["\\]/g,'')}", sans-serif`;
 await document.fonts.load(spec);await document.fonts.ready;
 const probe=makeCanvas(8,8),ctx=probe.getContext('2d',{willReadFrequently:true});ctx.font=spec;ctx.textBaseline='alphabetic';ctx.textAlign='left';ctx.direction='ltr';ctx.fontKerning='normal';
 const text=s.text.replace(/\r\n?/g,'\n').normalize('NFC');
 // Word/grapheme split is explicit: joins in Arabic/Indic scripts cannot be preserved per-character.
 if(/[\u0600-\u08ff\u0900-\u0dff]/u.test(text))throw Error('Для связных арабских и индийских письменностей посимвольный режим этой версии не подходит. Поддерживаются русский и латиница.');
 const visible=graphemes(text).filter(c=>!/\s/u.test(c));if(visible.length>240)throw Error('Максимум 240 видимых знаков. Разделите длинный текст на несколько титров.');
 if(!visible.length)throw Error('Введите текст.');
 const maxWidth=W*s.width/100,letterSpace=s.spacing*scale;
 // Width, wrapping and placement use the same advance/kerning plan.
 // Measuring whole prefixes allowed ligatures to compress widths while rasterizing separate letters.
 const metrics=new Map(),pairs=new Map();
 const metric=ch=>{if(!metrics.has(ch))metrics.set(ch,ctx.measureText(ch));return metrics.get(ch);};
 const kern=(prev,ch)=>{if(!prev)return 0;const k=prev+'\0'+ch;if(!pairs.has(k)){
   ctx.fontKerning='normal';const normal=ctx.measureText(prev+ch).width;
   ctx.fontKerning='none';const unkerned=ctx.measureText(prev+ch).width;ctx.fontKerning='normal';
   pairs.set(k,normal-unkerned);
 }return pairs.get(k);};
 const plan=t=>{const chars=graphemes(t),positions=[];let cursor=0,prev='';
  for(let i=0;i<chars.length;i++){const ch=chars[i];cursor+=kern(prev,ch);positions.push(cursor);cursor+=metric(ch).width+(i<chars.length-1?letterSpace:0);prev=ch;}
  return {chars,positions,width:cursor};};
 const measure=t=>plan(t).width;
 const lines=[];
 for(const paragraph of text.split('\n')){
   let line='';for(const word of paragraph.match(/\S+\s*|\s+/gu)||['']){
      if(line&&measure(line+word.trimEnd())>maxWidth){lines.push(line.trimEnd());line='';}
      if(measure(word.trimEnd())>maxWidth){
         for(const c of graphemes(word)){if(line&&measure(line+c)>maxWidth){lines.push(line.trimEnd());line='';}line+=c;}
      }else line+=word;
   }lines.push(line.trimEnd());
 }
 if(lines.length>16)throw Error('Слишком много строк. Увеличьте ширину текстового блока или уменьшите размер.');
 const units=[],accentWords=new Set(s.keywords.toLocaleLowerCase('ru').split(/[,\s]+/).filter(Boolean));
 const linePlans=lines.map(plan),widths=linePlans.map(p=>p.width),maxLine=Math.max(1,...widths);
 const fm=ctx.measureText('MgЙру'),fontAsc=fm.fontBoundingBoxAscent??fontSize*.8,fontDesc=fm.fontBoundingBoxDescent??fontSize*.2;
 const totalH=(lines.length-1)*fontSize*s.lineHeight+fontAsc+fontDesc;
 const midX=W*s.x/100,midY=H*s.y/100,top=midY-totalH/2;
 let wordId=0;const pad=Math.ceil(fontSize*.2+16*scale+s.stroke*scale+fontSize*.16*s.shadow);
 const baselineOffset=fontAsc;
 for(let li=0;li<lines.length;li++){
  const chars=linePlans[li].chars;const full=lines[li];let left=midX-maxLine/2;
  if(s.align==='center')left=midX-widths[li]/2;else if(s.align==='right')left=midX+maxLine/2-widths[li];
  let prefix='',currentWord='';let letterIndex=0;
  const words=full.split(/(\s+)/);let scan=0;
  const highlights=new Map();for(const token of words){const colored=accentWords.has(token.replace(/[^\p{L}\p{N}_-]/gu,'').toLocaleLowerCase('ru'));for(let j=0;j<graphemes(token).length;j++)highlights.set(scan++,colored);}
  for(const ch of chars){
    if(/\s/u.test(ch)){if(currentWord){wordId++;currentWord='';}prefix+=ch;letterIndex++;continue;}
    currentWord+=ch;const met=metric(ch),prefixWidth=linePlans[li].positions[letterIndex];
    const lb=Math.ceil(Math.max(0,met.actualBoundingBoxLeft||0)),rb=Math.ceil(Math.max(met.width,met.actualBoundingBoxRight||0));
    const asc=Math.ceil(met.actualBoundingBoxAscent??fontSize*.8),desc=Math.ceil(Math.max(0,met.actualBoundingBoxDescent??fontSize*.2));
    const pw=Math.max(2,lb+rb+pad*2),ph=Math.max(2,asc+desc+pad*2);if(pw>4096||ph>4096)throw Error('Слишком крупный символ');
    const can=makeCanvas(pw,ph),c=can.getContext('2d',{willReadFrequently:true});c.font=spec;c.textBaseline='alphabetic';c.fontKerning='normal';
    c.lineJoin='round';c.miterLimit=2;
    const draw=(shadow)=>{
      c.shadowColor=shadow?`rgba(0,0,0,${s.shadow*.7})`:'transparent';c.shadowBlur=shadow?fontSize*.10*s.shadow:0;c.shadowOffsetY=shadow?fontSize*.055*s.shadow:0;
      c.fillStyle=highlights.get(letterIndex)?s.accent:s.color;
      if(s.stroke>0){c.strokeStyle=s.strokeColor;c.lineWidth=s.stroke*2*scale;c.strokeText(ch,pad+lb,pad+asc);}
      c.fillText(ch,pad+lb,pad+asc);
    };draw(s.shadow>0);
    const x=left+prefixWidth-lb-pad,y=top+li*fontSize*s.lineHeight+baselineOffset-asc-pad;
    units.push({x,y,w:pw,h:ph,ax:left+prefixWidth+met.width/2,ay:top+li*fontSize*s.lineHeight+baselineOffset-(fontAsc-fontDesc)/2,pw,ph,word:wordId,line:li,flags:0,pixels:c.getImageData(0,0,pw,ph).data});
    prefix+=ch;letterIndex++;
  }wordId++;
 }
 if(s.backgroundBox){
  const px=fontSize*.3,py=fontSize*.15,bw=Math.ceil(maxLine+px*2),bh=Math.ceil(totalH+py*2);
  if(bw>4096||bh>4096)throw Error('Подложка превышает 4096 px. Уменьшите размер текста.');
  const can=makeCanvas(bw,bh),c=can.getContext('2d');c.globalAlpha=s.boxOpacity;c.fillStyle=s.boxColor;roundRect(c,0,0,bw,bh,fontSize*.15);c.fill();
  units.unshift({x:midX-bw/2,y:top-py,w:bw,h:bh,ax:midX,ay:midY,pw:bw,ph:bh,word:0,line:0,flags:1,pixels:c.getImageData(0,0,bw,bh).data});
 }
 let sum=units.reduce((n,u)=>n+u.pw*u.ph,0);if(sum>16*1024*1024)throw Error('Сцена превышает 16 млн пикселей букв. Уменьшите размер, тень или количество букв.');
 const value={W,H,fontSize,units};cachedLayout={key,value};return value;
}
export async function compileScene(input){
 const s=sanitize(input);const {W,H,fontSize,units}=await layoutScene(s);
 const cfg=new Float32Array(32);const [num,den]=s.fps.split('/').map(Number);
 cfg.set([s.duration,s.inDuration,s.outDuration,s.strength,s.stagger,s.seed,s.inPreset,s.outPreset,s.group,s.order,s.life,s.lifeAmount,s.lifeSpeed,s.quality,s.secondary,s.secondaryMix,s.focus,s.echo,s.lifeSource,0,0,s.opacity,s.motionBlur,num,den,0,0,0,fontSize,0,0,0]);
 const meta=new TextEncoder().encode(JSON.stringify(s));const buffer=new ArrayBuffer(160+meta.length+units.reduce((n,u)=>n+48+u.pixels.length,0));const view=new DataView(buffer);let off=0;
 const u32=v=>{view.setUint32(off,v,true);off+=4}, f32=v=>{view.setFloat32(off,v,true);off+=4};
 new Uint8Array(buffer,0,8).set(new TextEncoder().encode('SUNMTX1\0'));off=8;
 [2,meta.length,units.length,W,H,32].forEach(u32);cfg.forEach(f32);new Uint8Array(buffer,off,meta.length).set(meta);off+=meta.length;
 for(const u of units){[u.x,u.y,u.w,u.h,u.ax,u.ay].forEach(f32);[u.pw,u.ph,u.word,u.line,u.flags,u.pixels.length].forEach(u32);new Uint8Array(buffer,off,u.pixels.length).set(u.pixels);off+=u.pixels.length;}
 const warnings=[];
 if((s.inPreset?s.inDuration:0)+(s.outPreset!==0&&!(s.outPreset===-1&&s.inPreset===0)?s.outDuration:0)>s.duration*.9)warnings.push('Вход и выход автоматически укорочены: оставлено 10% времени на чтение.');
 if(units.some(u=>u.x<0||u.y<0||u.x+u.w>W||u.y+u.h>H))warnings.push('Часть текста или тени выходит за границу кадра.');
 if(s.duration<1)warnings.push('Очень короткий титр: проверьте, успевает ли зритель прочитать текст.');
 return {buffer,state:s,warnings,units:units.length,W,H};
}
export function readSceneMetadata(buffer){if(buffer.byteLength<160)throw Error('Повреждённый файл');const v=new DataView(buffer);if(new TextDecoder().decode(new Uint8Array(buffer,0,8))!=='SUNMTX1\0'||![1,2].includes(v.getUint32(8,true)))throw Error('Неверный формат .stxt');const n=v.getUint32(12,true);if(n>2*1024*1024||160+n>buffer.byteLength)throw Error('Повреждённый файл');return sanitize(JSON.parse(new TextDecoder().decode(new Uint8Array(buffer,160,n))));}
