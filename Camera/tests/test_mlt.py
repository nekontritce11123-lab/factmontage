#!/usr/bin/env python3
"""Discover and render studio.camera through a real MLT repository."""
from pathlib import Path
import hashlib,os,subprocess,sys,tempfile

ROOT=Path(__file__).resolve().parents[1]
module=Path(sys.argv[1]).resolve()
system=Path(subprocess.check_output(['pkg-config','--variable=moduledir','mlt-framework-7'],text=True).strip())
system_data=Path(subprocess.check_output(['pkg-config','--variable=mltdatadir','mlt-framework-7'],text=True).strip())

def run(args,env):
 result=subprocess.run(args,env=env,text=True,capture_output=True,timeout=60)
 if result.returncode:raise RuntimeError(result.stdout+result.stderr)
 return result.stdout+result.stderr

with tempfile.TemporaryDirectory(prefix='studio-camera-mlt-') as folder:
 work=Path(folder);repo=work/'repository';repo.mkdir()
 for source in system.glob('*.so'):
  if source.name != module.name: (repo/source.name).symlink_to(source)
 (repo/module.name).symlink_to(module)
 data=work/'data';data.mkdir()
 for source in system_data.iterdir():
  if source.name != 'studio': (data/source.name).symlink_to(source,target_is_directory=source.is_dir())
 studio=data/'studio';studio.mkdir();(studio/'filter_camera.yml').write_bytes((ROOT/'mlt/filter_camera.yml').read_bytes())
 env=os.environ.copy();env.update(MLT_REPOSITORY=str(repo),MLT_DATA=str(work/'data'),QT_QPA_PLATFORM='offscreen')
 query=run(['melt','-query','filter=studio.camera'],env)
 assert 'identifier: studio.camera' in query
 profile=work/'profile';profile.write_text('description=Studio camera test\nframe_rate_num=30\nframe_rate_den=1\nwidth=320\nheight=180\nprogressive=1\nsample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n')
 def frames(name,count,origin=None,span=None):
  output=work/(name+'.mkv');args=['melt','-profile',str(profile),'qimage:'+str(ROOT/'demo/camera_test.png'),'in=0',f'out={count-1}','-attach','studio.camera','mode=4','live=1','zoom=150','start_x=30','start_y=50','end_x=70','end_y=50','timing=0']
  if origin is not None:args.append(f'studio_time_origin={origin}')
  if span is not None:args.append(f'studio_time_span={span}')
  args += ['-consumer',f'avformat:{output}','vcodec=ffv1','an=1','real_time=-1'];run(args,env)
  assert output.exists(), 'MLT returned success without an output file'
  raw=subprocess.check_output(['ffmpeg','-v','error','-i',str(output),'-f','rawvideo','-pix_fmt','rgba','-'],timeout=60);size=320*180*4
  assert len(raw)==count*size
  return [hashlib.sha256(raw[i:i+size]).hexdigest() for i in range(0,len(raw),size)]
 original=frames('original',30)
 split=frames('left',12,0,29)+frames('right',18,12,29)
 assert len(set(original))>1 and original==split
 def cyclic(num,den,period):
  fps=num/den;count=round(period*fps)+1
  profile.write_text(f'description=Studio camera cycle\nframe_rate_num={num}\nframe_rate_den={den}\nwidth=64\nheight=36\nprogressive=1\nsample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n')
  output=work/f'cycle-{num}-{den}-{period}.mkv'
  run(['melt','-profile',str(profile),'qimage:'+str(ROOT/'demo/camera_test.png'),'in=0',f'out={count-1}',
       '-attach','studio.camera','mode=5','live=0','zoom=160',f'cycle_period={period}',
       '-consumer',f'avformat:{output}','vcodec=ffv1','an=1','real_time=-1'],env)
  raw=subprocess.check_output(['ffmpeg','-v','error','-i',str(output),'-f','rawvideo','-pix_fmt','rgba','-'],timeout=60)
  size=64*36*4
  assert len(raw)==count*size
  first=raw[:size];middle=raw[(count//2)*size:(count//2+1)*size];last=raw[-size:]
  mean=lambda a,b:sum(abs(x-y) for x,y in zip(a,b))/len(a)
  assert mean(first,middle)>1.0, (num,den,period,'cycle has no visible zoom')
  # Fractional frame rates cannot land exactly on the mathematical seam; the nearest frame must still be visually continuous.
  assert mean(first,last)<2.0, (num,den,period,mean(first,last))
 for fps in [(24,1),(25,1),(30,1),(50,1),(60,1),(30000,1001)]:
  for period in (2,6,12):cyclic(*fps,period)
 profile.write_text('description=Studio camera saved cycle\nframe_rate_num=25\nframe_rate_den=1\nwidth=64\nheight=36\nprogressive=1\nsample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n')
 saved=work/'cycle-saved.mlt';count=151
 run(['melt','-profile',str(profile),'qimage:'+str(ROOT/'demo/camera_test.png'),'in=0',f'out={count-1}',
      '-attach','studio.camera','mode=5','live=0','zoom=160','cycle_period=6','-consumer',f'xml:{saved}'],env)
 saved_text=saved.read_text(encoding='utf-8')
 assert '<property name="mode">5</property>' in saved_text and '<property name="cycle_period">6</property>' in saved_text
 reopened=work/'cycle-reopened.mkv'
 reopen_log=run(['melt',str(saved),'-consumer',f'avformat:{reopened}','vcodec=ffv1','an=1','real_time=-1'],env)
 assert reopened.exists(), reopen_log+'\n'+saved_text
 raw=subprocess.check_output(['ffmpeg','-v','error','-i',str(reopened),'-f','rawvideo','-pix_fmt','rgba','-'],timeout=60);size=64*36*4
 assert len(raw)==count*size
 first=raw[:size];middle=raw[(count//2)*size:(count//2+1)*size];last=raw[-size:]
 mean=lambda a,b:sum(abs(x-y) for x,y in zip(a,b))/len(a)
 assert mean(first,middle)>1.0 and mean(first,last)<2.0
 track=work/'голова тест.scam'
 track.write_text('SUNIMO_CAMERA_TRACK_V1\n# aspect=1.777777777778\n0,0.25,0.35,1\n1,0.5,0.5,1\n2,0.75,0.65,1\n',encoding='utf-8')
 profile.write_text('description=Studio camera tracking\nframe_rate_num=10\nframe_rate_den=1\nwidth=64\nheight=36\nprogressive=1\nsample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n')
 output=work/'tracking.mkv'
 run(['melt','-profile',str(profile),'qimage:'+str(ROOT/'demo/camera_test.png'),'in=0','out=20','-attach','studio.camera',
      'mode=0','live=0','zoom=160','tracking=1',f'track_path={track.as_uri()}',
      '-consumer',f'avformat:{output}','vcodec=ffv1','an=1','real_time=-1'],env)
 raw=subprocess.check_output(['ffmpeg','-v','error','-i',str(output),'-f','rawvideo','-pix_fmt','rgba','-'],timeout=60);size=64*36*4
 assert hashlib.sha256(raw[:size]).digest()!=hashlib.sha256(raw[-size:]).digest()
 print('MLT studio.camera discovery + split continuity + 18 cycle/FPS combinations + save/reopen + UTF-8 head track: PASS')
