// SPDX-License-Identifier: MIT
import {defaults,sanitize,compileScene,readSceneMetadata,clearLayoutCache} from './compiler.js';
const $=id=>document.getElementById(id),token=document.querySelector('meta[name=sunimo-token]').content;
let uploadQueue=Promise.resolve(),frameAgain=false,seekSerial=0,previewWidth=640,renderEMA=0,holdLoop=false;
let state={...defaults},catalog=[],favorites=new Set(),styles=[],onlyFavorites=false,compiled=null,playing=false,t=0,lastTick=0,frameBusy=false,buildNumber=0,buildPending=false,debounce=null,history=[],future=[],lastUrl=null,lastFrameAt=0,backgroundIndex=0;
const storage={get:(key,fallback)=>{try{return JSON.parse(localStorage.getItem('sunimo-text-'+key))??fallback}catch{return fallback}},set:(key,value)=>{try{localStorage.setItem('sunimo-text-'+key,JSON.stringify(value))}catch{}}};
state=sanitize(storage.get('draft',defaults));favorites=new Set(storage.get('favorites',[2,3,21,42,51,63,91]));styles=storage.get('styles',[]);
async function api(path,{method='GET',body=null}={}){const res=await fetch('/api/'+path,{method,headers:{'X-Sunimo-Token':token},body});if(!res.ok){let e;try{e=await res.json()}catch{e={error:res.statusText}}throw Error(e.error||res.statusText)}return res;}
function status(text,error=false){$('status').textContent=text;$('status').classList.toggle('error',error);}
function checkpoint(){const current=JSON.stringify(state);if(history.at(-1)!==current){history.push(current);if(history.length>40)history.shift();}future=[];}
function sync(){
 for(const el of document.querySelectorAll('[data-key]')){const v=state[el.dataset.key];if(el.type==='checkbox')el.checked=!!v;else el.value=v;}
 $('strengthOut').textContent=Math.round(state.strength*100)+'%';$('staggerOut').textContent=Math.round(state.stagger*100)+'%';
 $('seek').max=state.duration;const recipe=catalog.find(p=>p.id===state.inPreset);$('motionTitle').textContent=recipe?.name||'Без анимации';
 $('resolution').textContent=state.format.replace('x',' × ');const [w,h]=state.format.split('x').map(Number);$('preview').style.aspectRatio=`${w}/${h}`;
 if(h>w){$('preview').style.width='auto';$('preview').style.height='100%';$('preview').style.maxWidth='65%';}else{$('preview').style.width='100%';$('preview').style.height='auto';$('preview').style.maxWidth='100%';}
 let a=state.inPreset?state.inDuration:0,b=(state.outPreset!==0&&!(state.outPreset===-1&&state.inPreset===0))?state.outDuration:0;
 if(a+b>state.duration*.9){const m=state.duration*.9/(a+b);a*=m;b*=m;}
 $('inPhase').style.flex=String(a||.01);$('holdPhase').style.flex=String(Math.max(.01,state.duration-a-b));$('outPhase').style.flex=String(b||.01);
 $('inPhase').textContent=a?'Вход':'Без входа';$('outPhase').textContent=b?'Выход':'Без выхода';
 $('holdPhase').textContent=state.life&&state.lifeAmount>0?'Жизнь текста':'Неподвижно';
 const source=state.lifeSource||state.inPreset,life=catalog.find(p=>p.id===source)?.life;
 $('lifeDescription').textContent=state.life===9?(life?life.name+'. '+life.description:'Тихое дыхание цельной фразы. Можно выбрать характер без входа и выхода.'):state.life===0?'Текст неподвижен между переходами.':'Ручной характер движения. Выбранная группировка сохраняется.';
 $('lifeSourceRow').hidden=state.life!==9;
 $('cost').textContent=state.motionBlur>0?'Размытие движения повышает нагрузку на CPU.':'Без нейросетей. Прозрачный фон.';
 document.querySelectorAll('.preset').forEach(el=>el.classList.toggle('active',Number(el.dataset.id)===state.inPreset));
}
function motionSelect(id){holdLoop=false;seekSerial++;checkpoint();const p=catalog.find(x=>x.id===id);state.inPreset=id;if(p){state.group=p.group;state.order=p.order;}t=0;sync();schedule();playing=true;$('play').textContent='Ⅱ';lastTick=performance.now();}
function renderCatalog(){
 const term=$('search').value.trim().toLocaleLowerCase('ru'),cat=$('category').value;
 const visible=catalog.filter(p=>(!cat||p.category===cat)&&(!onlyFavorites||favorites.has(p.id))&&(!term||`${p.name} ${p.description} ${p.category}`.toLocaleLowerCase('ru').includes(term)));
 $('catalog').replaceChildren();$('count').textContent=visible.length;
 for(const p of visible){const item=document.createElement('div');item.className='preset'+(p.id===state.inPreset?' active':'');item.dataset.id=p.id;item.setAttribute('role','listitem');
 const button=document.createElement('button');button.className='preset-main';button.title=p.description;
 const num=document.createElement('span');num.className='preset-number';num.textContent=String(p.id).padStart(3,'0');
 const text=document.createElement('span');text.className='preset-text';const title=document.createElement('strong');title.textContent=p.name;const subtitle=document.createElement('small');subtitle.textContent=p.category;text.append(title,subtitle);button.append(num,text);button.onclick=()=>motionSelect(p.id);
 const star=document.createElement('button');star.className='star'+(favorites.has(p.id)?' saved':'');star.textContent=favorites.has(p.id)?'★':'☆';star.setAttribute('aria-label','В избранное: '+p.name);star.onclick=()=>{if(favorites.has(p.id))favorites.delete(p.id);else favorites.add(p.id);storage.set('favorites',[...favorites]);renderCatalog()};item.append(button,star);$('catalog').append(item);}
 if(!visible.length){const empty=document.createElement('p');empty.className='hint';empty.textContent='Ничего не найдено. Сбрось поиск или категорию.';$('catalog').append(empty);}
}
function buildSelects(){
 for(const id of ['inPreset','outPreset','secondary']){const select=$(id);select.replaceChildren();if(id==='outPreset')select.add(new Option('↶ Реверс входа',-1));select.add(new Option('Без анимации',0));let group=null,category='';for(const p of catalog){if(p.category!==category){category=p.category;group=document.createElement('optgroup');group.label=category;select.append(group);}group.append(new Option(`${String(p.id).padStart(3,'0')} · ${p.name}`,p.id));}}
 $('lifeSource').replaceChildren();$('lifeSource').add(new Option('По появлению · автоматически',0));
 for(const p of catalog)$('lifeSource').add(new Option(`${String(p.id).padStart(3,'0')} · ${p.life.name}`,p.id));
 for(const category of [...new Set(catalog.map(p=>p.category))])$('category').add(new Option(category,category));
}
function schedule(){clearTimeout(debounce);buildPending=true;const n=++buildNumber;debounce=setTimeout(()=>build(n),170);}
async function build(n=++buildNumber){
 buildPending=true;status('Подготавливаю сцену…');$('exportBtn').disabled=true;$('saveBtn').disabled=true;
 try{const c=await compileScene(state);if(n!==buildNumber)return;await (uploadQueue=uploadQueue.catch(()=>{}).then(async()=>{if(n!==buildNumber)return;const reply=await (await api('scene',{method:'POST',body:c.buffer})).json();c.revision=reply.revision;}));if(n!==buildNumber)return;compiled=c;state=c.state;storage.set('draft',state);t=Math.min(t,state.duration);status(c.warnings.length?c.warnings.join(' '):`${c.units} элементов · ${(c.buffer.byteLength/1048576).toFixed(1)} МБ · один движок для превью и Kdenlive`);buildPending=false;await drawFrame();}
 catch(e){if(n===buildNumber){status(e.message,true);buildPending=false;compiled=null;}}
 finally{if(n===buildNumber){$('exportBtn').disabled=!compiled;$('saveBtn').disabled=!compiled;}}
}
async function ensureBuilt(){if(buildPending){clearTimeout(debounce);await build(++buildNumber);}if(!compiled)throw Error('Сначала исправь ошибку в настройках сцены.');return compiled;}
async function drawFrame(){
 if(frameBusy){if(!playing)frameAgain=true;return;}
 if(buildPending||!compiled||document.hidden)return;
 frameBusy=true;const number=buildNumber,serial=seekSerial,sampleT=t,started=performance.now();
 try{
 const w=compiled.W,h=compiled.H,size=playing?previewWidth:768,ratio=Math.min(size/w,(size*9/16)/h),pw=Math.max(64,Math.round(w*ratio/2)*2),ph=Math.max(64,Math.round(h*ratio/2)*2);
 const response=await api(`frame?t=${sampleT.toFixed(5)}&w=${pw}&h=${ph}&revision=${compiled.revision}&raw=1`);
 const bytes=await response.arrayBuffer();if(number!==buildNumber||serial!==seekSerial)return;
 if(bytes.byteLength!==pw*ph*4)throw Error('Неполный кадр предпросмотра');
 const canvas=$('frame');if(canvas.width!==pw||canvas.height!==ph){canvas.width=pw;canvas.height=ph;}
 canvas.getContext('2d',{alpha:true}).putImageData(new ImageData(new Uint8ClampedArray(bytes),pw,ph),0,0);
 const ms=performance.now()-started;renderEMA=renderEMA?renderEMA*.88+ms*.12:ms;
 const target=Math.min(30,Number(state.fps.split('/')[0])/Number(state.fps.split('/')[1]));
 if(playing&&renderEMA>1000/target*.85&&previewWidth>384){previewWidth=Math.max(384,previewWidth-64);renderEMA=0;}
 $('previewStats').textContent=`Превью ${pw} × ${ph} · ${ms.toFixed(0)} мс/кадр · экспорт ${state.format.replace('x',' × ')}`;
 }catch(e){status(e.message,true);playing=false;}
 finally{frameBusy=false;if(frameAgain){frameAgain=false;queueMicrotask(()=>drawFrame());}}
}
function holdBounds(){let a=state.inPreset?state.inDuration:0,b=(state.outPreset!==0&&!(state.outPreset===-1&&state.inPreset===0))?state.outDuration:0;if(a+b>state.duration*.9){const k=state.duration*.9/(a+b);a*=k;b*=k;}return [a,state.duration-b];}
function tick(now){if(playing&&!buildPending&&!document.hidden){const elapsed=Math.min(.12,(now-lastTick)/1000);
 if(holdLoop){const [lo,hi]=holdBounds();t=lo+((Math.max(0,t-lo)+elapsed)%Math.max(.01,hi-lo));}else t=(t+elapsed)%state.duration;
 const target=Math.min(30,Number(state.fps.split('/')[0])/Number(state.fps.split('/')[1]));
 if(now-lastFrameAt>=1000/target){lastFrameAt=now;drawFrame();}}
 lastTick=now;$('seek').value=t;$('clock').textContent=`${t.toFixed(2)} / ${state.duration.toFixed(2)}`;requestAnimationFrame(tick);}
function download(blob,name){const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),30000);}
function modal(title,body){$('dialogTitle').textContent=title;$('dialogBody').replaceChildren();if(typeof body==='string'){const p=document.createElement('p');p.textContent=body;$('dialogBody').append(p);}else $('dialogBody').append(body);$('dialog').showModal();}
function stylesList(){
 $('savedStyles').replaceChildren();styles.forEach((preset,i)=>{const row=document.createElement('div');row.className='saved-style';const b=document.createElement('button');b.textContent=preset.name;b.onclick=()=>{checkpoint();state=sanitize({...state,...preset.settings,text:state.text,name:preset.name});sync();schedule()};const del=document.createElement('button');del.textContent='×';del.setAttribute('aria-label','Удалить стиль '+preset.name);del.onclick=()=>{styles.splice(i,1);storage.set('styles',styles);stylesList()};row.append(b,del);$('savedStyles').append(row);});
}
for(const el of document.querySelectorAll('[data-key]')){
 el.addEventListener('focus',()=>checkpoint());el.addEventListener(el.type==='number'?'change':'input',()=>{
 const key=el.dataset.key;if(key==='inPreset'){const p=catalog.find(p=>p.id===Number(el.value));if(p){state.group=p.group;state.order=p.order;}}
 state[key]=el.type==='checkbox'?el.checked:typeof defaults[key]==='number'?Number(el.value):el.value;
 state=sanitize(state);sync();schedule();
 });
}
for(const tab of document.querySelectorAll('[data-tab]'))tab.onclick=()=>{for(const b of document.querySelectorAll('[data-tab]')){b.classList.toggle('active',b===tab);b.setAttribute('aria-selected',String(b===tab));}for(const p of document.querySelectorAll('.tab-panel'))p.classList.toggle('hidden',p.id!=='tab-'+tab.dataset.tab);};
$('search').oninput=renderCatalog;$('category').onchange=renderCatalog;$('favoritesBtn').onclick=()=>{onlyFavorites=!onlyFavorites;$('favoritesBtn').textContent=onlyFavorites?'★':'☆';renderCatalog();};
$('play').onclick=()=>{holdLoop=false;playing=!playing;$('play').textContent=playing?'Ⅱ':'▶';lastTick=performance.now();drawFrame();};
$('seek').oninput=()=>{seekSerial++;holdLoop=false;playing=false;$('play').textContent='▶';t=Number($('seek').value);drawFrame();};
$('lifeOnlyBtn').onclick=()=>{checkpoint();if(!state.lifeSource)state.lifeSource=state.inPreset||2;state.inPreset=0;state.outPreset=0;state.life=9;holdLoop=false;seekSerial++;t=0;sync();schedule();};
$('holdPreviewBtn').onclick=()=>{holdLoop=true;seekSerial++;t=holdBounds()[0];playing=true;$('play').textContent='Ⅱ';lastTick=performance.now();drawFrame();};
$('background').onclick=()=>{const variants=['checker','dark','light','warm'];$('preview').classList.remove(...variants);backgroundIndex=(backgroundIndex+1)%variants.length;$('preview').classList.add(variants[backgroundIndex]);};
$('safeBtn').onclick=()=>$('safe').classList.toggle('visible');
$('undoBtn').onclick=()=>{if(history.length){future.push(JSON.stringify(state));state=sanitize(JSON.parse(history.pop()));sync();schedule();}};
$('redoBtn').onclick=()=>{if(future.length){history.push(JSON.stringify(state));state=sanitize(JSON.parse(future.pop()));sync();schedule();}};
$('seedBtn').onclick=()=>{checkpoint();state.seed=crypto.getRandomValues(new Uint32Array(1))[0]%16777216;schedule();};
$('saveStyleBtn').onclick=()=>{const settings={...state};delete settings.text;styles.push({name:state.name||'Мой стиль',settings});if(styles.length>50)styles.shift();storage.set('styles',styles);stylesList();status('Стиль сохранён в этом браузере.');};
$('saveBtn').onclick=async()=>{try{const c=await ensureBuilt();download(new Blob([c.buffer],{type:'application/octet-stream'}),(state.name||'Текст')+'.stxt');}catch(e){status(e.message,true);}};
$('exportBtn').onclick=async()=>{try{await ensureBuilt();const data=await (await api('export',{method:'POST',body:JSON.stringify({name:state.name,revision:compiled.revision})})).json();const div=document.createElement('div');
 const p=document.createElement('p');p.textContent='Добавь этот .mlt в «Корзину проекта» Kdenlive, затем перетащи его на видеодорожку выше основного видео.';const code=document.createElement('code');code.textContent=data.mlt;
 const note=document.createElement('p');note.textContent='Один раз установи нативный плагин через install.sh. Не удаляй соседний .stxt: это исходник для воспроизведения. Файл .mlt хранит абсолютный путь к нему.';
 const actions=document.createElement('div');actions.className='actions';const open=document.createElement('button');open.className='primary';open.textContent='Открыть папку';open.onclick=async()=>{try{await api('open-export',{method:'POST',body:'{}'})}catch(e){status(e.message,true)}};const copy=document.createElement('button');copy.textContent='Скопировать путь';copy.onclick=async()=>{try{await navigator.clipboard.writeText(data.mlt);copy.textContent='Скопировано'}catch{copy.textContent='Выдели путь выше'}};actions.append(open,copy);div.append(p,code,note,actions);modal('Титр сохранён',div);}
 catch(e){status(e.message,true);}};
$('openBtn').onclick=()=>$('fileInput').click();$('fileInput').onchange=async e=>{const file=e.target.files[0];if(!file)return;try{const next=file.name.endsWith('.stxt')?readSceneMetadata(await file.arrayBuffer()):sanitize(JSON.parse(await file.text()));checkpoint();state=next;sync();schedule();if(state.font.startsWith('SUNIMO_LOCAL_'))modal('Нужен исходный шрифт','Для редактирования этого титра повторно открой свой TTF / OTF. Уже сохранённый .stxt проигрывается без шрифта, но редактирование требует его загрузки.');}catch(err){status(err.message,true);}e.target.value='';};
$('fontBtn').onclick=()=>$('fontInput').click();$('fontInput').onchange=async e=>{const file=e.target.files[0];if(!file)return;try{if(file.size>20*1024*1024)throw Error('Шрифт больше 20 МБ');const family='SUNIMO_LOCAL_'+file.name.replace(/[^a-zA-Z0-9_-]/g,'_');const face=new FontFace(family,await file.arrayBuffer());await face.load();document.fonts.add(face);clearLayoutCache();checkpoint();state.font=family;sync();schedule();}catch(err){status('Не удалось открыть шрифт: '+err.message,true);}e.target.value='';};
$('closeDialog').onclick=()=>$('dialog').close();$('helpBtn').onclick=()=>{const div=document.createElement('div');for(const text of ['1. Выбери движение слева и напиши фразу. В «Движение» выбери реверс, другой выход или отсутствие анимации.','2. Нажми «В Kdenlive». Готовые .stxt и .mlt сохранятся в папку Videos / SUNIMO Text. Добавь .mlt как клип на верхнюю дорожку.','3. .stxt хранит настройки и готовые изображения букв. Для воспроизведения шрифт не нужен. Для правки открой .stxt в этой панели.','Плагин не изменяет существующий проект, не считывает выделение на таймлайне и не превращает обычный титр Kdenlive в посимвольный. Установка плагина выполняется один раз через install.sh.','После обрезки длина входа/выхода автоматически не привязывается к краю нового клипа. Измени «Длина титра» и экспортируй новый титр. На Deck начни с 1080p, короткого текста и выключенного размытия движения.']){const p=document.createElement('p');p.textContent=text;div.append(p)}modal('Текст → движение → Kdenlive',div);};
let drag=null;$('preview').onpointerdown=e=>{if(e.button!==0)return;checkpoint();drag={x:e.clientX,y:e.clientY,sx:state.x,sy:state.y};$('preview').setPointerCapture(e.pointerId);};$('preview').onpointerup=e=>{if(!drag)return;const rect=$('preview').getBoundingClientRect();state.x=Math.max(0,Math.min(100,drag.sx+(e.clientX-drag.x)/rect.width*100));state.y=Math.max(0,Math.min(100,drag.sy+(e.clientY-drag.y)/rect.height*100));drag=null;sync();schedule();};
document.addEventListener('keydown',e=>{if(['INPUT','TEXTAREA','SELECT'].includes(document.activeElement?.tagName)||$('dialog').open)return;if(e.code==='Space'){e.preventDefault();$('play').click();}if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='z'){e.preventDefault();$(e.shiftKey?'redoBtn':'undoBtn').click();}});
document.addEventListener('visibilitychange',()=>{lastTick=performance.now();if(!document.hidden)drawFrame();});
async function init(){try{catalog=await (await api('catalog')).json();buildSelects();renderCatalog();stylesList();sync();const fonts=await (await api('fonts')).json();for(const font of fonts){const o=document.createElement('option');o.value=font;$('fontList').append(o);}await build();playing=true;$('play').textContent='Ⅱ';requestAnimationFrame(tick);}catch(e){status(e.message,true);}}
window.sunimo={get state(){return state},get compiled(){return compiled},setState:async(next)=>{state=sanitize({...state,...next});sync();await build();},pause:()=>{playing=false;$('play').textContent='▶'},seek:async(time)=>{seekSerial++;holdLoop=false;playing=false;t=time;await drawFrame();}};
init();
