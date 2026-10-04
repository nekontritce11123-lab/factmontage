#!/usr/bin/env python3
"""Real Linux MLT render, not a Flatpak or GUI test. Requires melt and ffmpeg."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import xml.etree.ElementTree as E

ROOT=Path(__file__).resolve().parents[1]


def main():
    melt=shutil.which('melt') or shutil.which('melt-7')
    if not melt or not shutil.which('ffmpeg'):
        raise SystemExit('NOT TESTED: melt and ffmpeg are required')
    env=os.environ.copy();env['QT_QPA_PLATFORM']='offscreen'
    env['FREI0R_PATH']=str(ROOT/'bin/linux-x86_64')+':'+env.get('FREI0R_PATH','/usr/lib/frei0r-1:/usr/lib/x86_64-linux-gnu/frei0r-1')
    query=subprocess.run([melt,'-query','filter=frei0r.card3d'],env=env,text=True,capture_output=True,timeout=60)
    metadata=query.stdout+query.stderr
    (ROOT/'docs/mlt_metadata.txt').write_text(metadata,encoding='utf-8')
    assert query.returncode==0 and 'identifier: frei0r.card3d' in metadata,metadata
    folder=ROOT/'build/mlt';folder.mkdir(parents=True,exist_ok=True)
    profile=folder/'profile'
    profile.write_text('description=Card 3D smoke\nframe_rate_num=30\nframe_rate_den=1\nwidth=320\nheight=180\nprogressive=1\nsample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n',encoding='utf-8')
    hashes={}
    controls=['0=0.5','1=0','2=0.3333333333333333','3=0.1666666666666667',
              '4=0.68','5=0.34','7=0','8=0.2','9=1','10=0.35','11=1','12=0.2','13=0.8',
              '14=1','15=0','16=0.34','17=0']
    for name,motion in [('baseline',None),('static',0),('moving',1)]:
        output=folder/(name+'.mkv')
        args=[melt,'-profile',str(profile),'color:#e04020','in=0','out=11']
        if motion is not None:
            args+=['-attach','frei0r.card3d',*controls,f'6={motion}']
        args+=['-consumer',f'avformat:{output}','vcodec=ffv1','an=1','real_time=-1']
        run=subprocess.run(args,env=env,capture_output=True,text=True,timeout=120)
        (folder/(name+'.log')).write_text(run.stdout+run.stderr,encoding='utf-8')
        assert run.returncode==0,run.stderr
        data=subprocess.check_output(['ffmpeg','-v','error','-i',str(output),'-f','rawvideo','-pix_fmt','rgba','-'],timeout=60)
        size=320*180*4
        assert len(data)==12*size,(name,len(data))
        hashes[name]=[hashlib.sha256(data[n:n+size]).hexdigest() for n in range(0,len(data),size)]
    assert len(set(hashes['static']))==1,'Static preset changed over time'
    assert len(set(hashes['moving']))>1,'Motion preset did not animate'
    assert hashes['baseline'][0]!=hashes['static'][0],'MLT filter had no visible effect'
    video_controls=[('7=1' if value.startswith('7=') else value) for value in controls]
    video_output=folder/'video-source.mkv'
    video_run=subprocess.run([melt,'-profile',str(profile),'color:#e04020','in=0','out=11',
                              '-attach','frei0r.card3d',*video_controls,'6=0',
                              '-consumer',f'avformat:{video_output}','vcodec=ffv1','an=1','real_time=-1'],
                             env=env,capture_output=True,text=True,timeout=120)
    assert video_run.returncode==0,video_run.stderr
    video_data=subprocess.check_output(['ffmpeg','-v','error','-i',str(video_output),'-f','rawvideo','-pix_fmt','rgba','-'],timeout=60)
    assert any(any(video_data[n:n+size:4]) for n in range(0,len(video_data),size)), 'Video source mode rendered no pixels'
    exit_output=folder/'exit.mkv'
    exit_controls=[value for value in controls if not value.startswith('17=')]
    exit_run=subprocess.run([melt,'-profile',str(profile),'color:#e04020','in=0','out=11',
                              '-attach','frei0r.card3d',*exit_controls,'1=0.5','6=0',f'17={(11/30)/21600}',
                              '-consumer',f'avformat:{exit_output}','vcodec=ffv1','an=1','real_time=-1'],
                             env=env,capture_output=True,text=True,timeout=120)
    assert exit_run.returncode==0,exit_run.stderr
    exit_data=subprocess.check_output(['ffmpeg','-v','error','-i',str(exit_output),'-f','rawvideo','-pix_fmt','rgba','-'],timeout=60)
    size=320*180*4
    exit_energy=[sum(exit_data[n:n+size:4])+sum(exit_data[n+1:n+size:4])+sum(exit_data[n+2:n+size:4])
                 for n in range(0,len(exit_data),size)]
    assert max(exit_energy)>0 and exit_energy[-1]==0 and len(set(exit_energy[-8:]))>3,exit_energy
    trimmed=folder/'trimmed-entrance.mkv'
    entrance_controls=[value for value in controls if not value.startswith('1=')]
    trimmed_run=subprocess.run([melt,'-profile',str(profile),'color:#e04020','in=60','out=71',
                                '-attach','frei0r.card3d',*entrance_controls,'1=0.25','6=0',
                                '-consumer',f'avformat:{trimmed}','vcodec=ffv1','an=1','real_time=-1'],
                               env=env,capture_output=True,text=True,timeout=120)
    assert trimmed_run.returncode==0,trimmed_run.stderr
    data=subprocess.check_output(['ffmpeg','-v','error','-i',str(trimmed),'-f','rawvideo','-pix_fmt','rgba','-'],timeout=60)
    size=320*180*4
    trimmed_hashes=[hashlib.sha256(data[n:n+size]).hexdigest() for n in range(0,len(data),size)]
    assert len(set(trimmed_hashes))>2,'Entrance did not restart at the beginning of a trimmed source'
    saved=folder/'saved.mlt'
    save=subprocess.run([melt,'-profile',str(profile),'color:#e04020','in=0','out=11',
                         '-attach','frei0r.card3d',*controls,'6=0',
                         '-consumer',f'xml:{saved}','all=1','real_time=-1'],
                        env=env,capture_output=True,text=True,timeout=60)
    assert save.returncode==0,save.stderr
    properties={p.get('name'):p.text for p in E.parse(saved).findall('.//filter/property')}
    assert properties.get('9')=='1' and float(properties.get('10','nan'))==.35,properties
    assert properties.get('11')=='1' and float(properties.get('12','nan'))==.2 and float(properties.get('13','nan'))==.8,properties
    assert properties.get('14')=='1' and properties.get('17')=='0',properties
    reopened=folder/'reopened.mkv'
    reopen=subprocess.run([melt,str(saved),'-consumer',f'avformat:{reopened}',
                           'vcodec=ffv1','an=1','real_time=-1'],
                          env=env,capture_output=True,text=True,timeout=120)
    assert reopen.returncode==0,reopen.stderr
    data=subprocess.check_output(['ffmpeg','-v','error','-i',str(reopened),'-f','rawvideo','-pix_fmt','rgba','-'],timeout=60)
    size=320*180*4
    reopened_hashes=[hashlib.sha256(data[n:n+size]).hexdigest() for n in range(0,len(data),size)]
    assert reopened_hashes==hashes['static'],'Saved MLT changed Card 3D rendering'
    result=dict(scope='System Linux MLT; not Flatpak/Deck/GUI',frames_per_case=12,
                binary_sha256=hashlib.sha256((ROOT/'bin/linux-x86_64/card3d.so').read_bytes()).hexdigest(),
                saved_parameters={'9':1,'10':.35,'11':1,'12':.2,'13':.8,'14':1,'15':0,'16':.34,'17':0},
                exit_energy=exit_energy,trimmed_entrance_hashes=trimmed_hashes,hashes=hashes)
    (ROOT/'docs/mlt_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('MLT metadata + baseline/static/moving + save/reopen: PASS')


if __name__=='__main__':main()
