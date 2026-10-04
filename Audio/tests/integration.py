#!/usr/bin/env python3
"""End-to-end tests against the installed FFmpeg. No pip packages required.
Synthetic fixtures verify mechanics, NOT the perceptual quality of speech processing.
"""
from __future__ import annotations
import argparse, array, hashlib, json, math, os, re, shutil, signal, subprocess, tempfile, time, wave
from pathlib import Path

def run(args: list[str], **kw):
    return subprocess.run([str(x) for x in args], check=True, capture_output=True, text=True, timeout=120, **kw)

def fixture(path: Path, duration=4.5, rate=48000, kind='voice', channels=1):
    data=array.array('h')
    for i in range(round(duration*rate)):
        t=i/rate
        active=(.6<t<1.3) or (2.0<t<2.7) or (3.4<t<4.05)
        if kind=='voice': x=(.17*math.sin(2*math.pi*190*t)+.055*math.sin(2*math.pi*570*t)) if active else 0
        elif kind=='music': x=.15*math.sin(2*math.pi*440*t)+.07*math.sin(2*math.pi*660*t)
        elif kind=='silence': x=0
        elif kind=='crest': x=.007*math.sin(2*math.pi*330*t); x=.9 if i%48000==6000 else x
        elif kind=='noise': x=(.13*math.sin(2*math.pi*190*t) if active else 0)+.005*math.sin(2*math.pi*83*t)+.002*math.sin(2*math.pi*7643*t)
        elif kind=='levels': x=(.15 if t<4 else .015)*math.sin(2*math.pi*440*t)
        else: raise ValueError(kind)
        val=round(max(-.999,min(.999,x))*32767)
        for ch in range(channels): data.append(-val if channels==2 and ch==1 else val)
    if os.sys.byteorder!='little': data.byteswap()
    with wave.open(str(path),'wb') as f: f.setnchannels(channels); f.setsampwidth(2); f.setframerate(rate); f.writeframes(data.tobytes())

def independent_meter(path: Path):
    text=run(['ffmpeg','-hide_banner','-nostdin','-i',path,'-af','ebur128=peak=true','-f','null','-']).stderr
    summary=text.rsplit('Summary:',1)[-1]
    return {'lufs':float(re.search(r'I:\s*(-?[0-9.]+) LUFS',summary)[1]), 'true_peak_dbtp':float(re.search(r'Peak:\s*(-?[0-9.]+) dBFS',summary)[1])}

def segment_lufs(path: Path, start: int, end: int):
    text=run(['ffmpeg','-hide_banner','-nostdin','-i',path,'-af',f'atrim=start_sample={start}:end_sample={end},asetpts=PTS-STARTPTS,ebur128','-f','null','-']).stderr
    return float(re.search(r'I:\s*(-?[0-9.]+) LUFS',text.rsplit('Summary:',1)[-1])[1])

def alignment_regression(worker: Path, root: Path):
    # Nonperiodic bursts near both ends detect delay and loss of the flushed
    # FFT/limiter tail. This is a timing fixture, not a speech-quality test.
    source=root/'alignment.wav'; rate=48000; duration=2; data=array.array('h')
    for i in range(duration*rate):
        t=i/rate
        active=.03<t<.3 or 1.65<t<1.995
        x=.15*math.sin(2*math.pi*(190*t+430*t*t)) if active else 0
        data.append(round(x*32767))
    if os.sys.byteorder!='little': data.byteswap()
    with wave.open(str(source),'wb') as f:
        f.setnchannels(1); f.setsampwidth(2); f.setframerate(rate); f.writeframes(data.tobytes())
    (root/'provenance.json').write_text(json.dumps({'worker':str(worker),
        'worker_sha256':hashlib.sha256(worker.read_bytes()).hexdigest(),
        'input_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
        'ffmpeg':run(['ffmpeg','-version']).stdout.splitlines()[0]},indent=2)+'\n')
    segments=root/'alignment-segments.txt'; segments.write_text('0 2\n')
    def decode(path):
        p=subprocess.run(['ffmpeg','-v','error','-nostdin','-i',str(path),'-ac','1','-ar','8000','-f','f32le','-'],
                         capture_output=True,check=True,timeout=30)
        values=array.array('f',p.stdout)
        if os.sys.byteorder!='little': values.byteswap()
        return values
    original=decode(source); cases=[]
    for name,mode,noise,normalize in [('fft','voice','fft','off'),('limiter','music','off','on'),('fft-limiter','voice','fft','on')]:
        output=root/name
        cmd=[str(worker),'--mode',mode,'--voice',str(source),'--music',str(source),'--out-dir',str(output),
             '--strength','0','--noise',noise,'--normalize',normalize,'--target','-23']
        if normalize=='on': cmd+=['--level-segments',str(segments)]
        (root/(name+'-command.json')).write_text(json.dumps({'argv':cmd,'timeout_seconds':120}))
        try:
            p=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
        except subprocess.TimeoutExpired as error:
            (root/(name+'-stdout.log')).write_bytes(error.stdout or b'')
            (root/(name+'-stderr.log')).write_bytes(error.stderr or b'')
            raise
        (root/(name+'-command.json')).write_text(json.dumps({'argv':cmd,'exit':p.returncode}))
        (root/(name+'-stdout.log')).write_text(p.stdout)
        (root/(name+'-stderr.log')).write_text(p.stderr)
        assert p.returncode==0,(cmd,p.stdout,p.stderr)
        audio=output/'result.wav'; values=decode(audio)
        probe=json.loads(run(['ffprobe','-v','error','-show_entries','stream=duration_ts,sample_rate','-of','json',audio]).stdout)['streams'][0]
        assert int(probe['sample_rate'])==rate and int(probe['duration_ts'])==duration*rate,probe
        assert len(values)==len(original)
        anchors=[]
        for start,end in [(400,2000),(13600,15600)]:
            reference=original[start:end]; reference_energy=sum(x*x for x in reference)
            best=(-1,0)
            for lag in range(-320,321):
                actual=values[start+lag:end+lag]; energy=sum(x*x for x in actual)
                score=sum(x*y for x,y in zip(reference,actual))/math.sqrt(reference_energy*energy) if energy else 0
                if score>best[0]: best=(score,lag)
            anchors.append({'correlation':best[0],'delay_ms':best[1]/8})
            (root/(name+'-anchors.json')).write_text(json.dumps(anchors,indent=2)+'\n')
            assert best[0]>.95 and abs(best[1])<=8,(name,anchors)
        cases.append({'name':'alignment-'+name,'passed':True,'samples':duration*rate,'anchors':anchors,
                      'output_sha256':hashlib.sha256(audio.read_bytes()).hexdigest(),'evidence':str(root)})
    return cases

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--worker',required=True);ap.add_argument('--report',required=True)
    ap.add_argument('--alignment-only',action='store_true');a=ap.parse_args()
    worker=Path(a.worker).resolve(); results=[]
    destination=Path(a.report);destination.parent.mkdir(parents=True,exist_ok=True)
    def check_alignment():
        # Keep input/argv/stderr/output even when an assertion fails.
        evidence=Path(tempfile.mkdtemp(prefix='alignment-',dir=destination.parent))
        return alignment_regression(worker,evidence)
    if a.alignment_only:
        results=check_alignment()
        destination.write_text(json.dumps({'suite':'alignment','worker':str(worker),'cases':results},indent=2)+'\n')
        print(f'{len(results)} alignment scenarios passed; report: {destination}');return
    with tempfile.TemporaryDirectory(prefix='studio-audio-integration-') as temp:
        root=Path(temp);inputs=root/'Кириллица spaces ; dollar $ and apostrophe \' ';inputs.mkdir()
        for kind in ['voice','music','silence','crest','noise']:fixture(inputs/(kind+'.wav'),kind=kind)
        fixture(inputs/'antiphase.wav',kind='music',channels=2)
        fixture(inputs/'44100.wav',duration=2,rate=44100,kind='music')
        fixture(inputs/'short.wav',duration=2,kind='voice')
        fixture(inputs/'surround.wav',duration=1,kind='music',channels=6)
        fixture(inputs/'levels.wav',duration=8,kind='levels')
        segments=inputs/'level-segments.txt';segments.write_text('0 4\n4 8\n')
        def invoke(name,mode='voice',voice='voice.wav',music='music.wav',extra=(),fail=False):
            out=root/name
            cmd=[worker,'--mode',mode,'--out-dir',out,'--voice',inputs/voice,'--music',inputs/music,'--snapshot-token','project:sequence:revision:α']+list(extra)
            before={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs.iterdir()}
            start=time.monotonic();p=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,timeout=120)
            elapsed=time.monotonic()-start
            if fail:
                assert p.returncode!=0,(name,p.stdout,p.stderr)
                assert not out.exists(),name
                results.append({'name':name,'passed':True,'expected_rejection':True});return None
            assert p.returncode==0,(name,p.stdout,p.stderr)
            report=json.loads((out/'report.json').read_text())
            assert report['snapshot_token']=='project:sequence:revision:α'
            assert hashlib.sha256((out/'result.wav').read_bytes()).hexdigest()==report['audio_sha256']
            for path,h in before.items():assert hashlib.sha256(Path(path).read_bytes()).hexdigest()==h,'source altered'
            probe=json.loads(run(['ffprobe','-v','error','-select_streams','a:0','-show_entries','stream=sample_rate,channels,duration_ts','-of','json',out/'result.wav']).stdout)['streams'][0]
            assert int(probe['sample_rate'])==48000 and probe['channels']==2
            assert probe['duration_ts']==report['samples']
            item={'name':name,'passed':True,'elapsed_seconds':round(elapsed,3),'samples':report['samples'],'target_met':report['target_met'],'after':report['after']}
            if report['after']['finite']:
                item['independent_ebur128']=independent_meter(out/'result.wav')
                assert abs(item['independent_ebur128']['lufs']-report['after']['integrated_lufs'])<.25
                assert abs(item['independent_ebur128']['true_peak_dbtp']-report['after']['true_peak_dbtp'])<.25
            results.append(item);return report
        mixed=invoke('mix',mode='mix');assert mixed['target_met'];assert len(mixed['speech_intervals'])==3
        assert mixed['samples']==216000
        assert invoke('voice')['target_met']
        assert invoke('music',mode='music')['target_met']
        for mode in ('music','voice'):
            leveled=invoke('track-'+mode,mode=mode,voice='levels.wav',music='levels.wav',
                           extra=['--level-segments',segments,'--strength','0'] if mode=='voice' else ['--level-segments',segments])
            assert leveled['target_met'] and leveled['track_leveling']['verified']
            parts=leveled['track_leveling']['segments']
            measured=[segment_lufs(root/('track-'+mode)/'result.wav',p['start_sample'],p['end_sample']) for p in parts]
            assert len(parts)==2 and parts[1]['gain_db']>15 and abs(measured[0]-measured[1])<=1.0,measured
        bad=inputs/'bad-segments.txt';bad.write_text('0 5\n4 8\n')
        invoke('reject-overlapping-level-segments',mode='music',music='levels.wav',extra=['--level-segments',bad],fail=True)
        anti=invoke('phase',mode='music',music='antiphase.wav');assert anti['target_met']
        raw=subprocess.run(['ffmpeg','-v','error','-i',str(root/'phase/result.wav'),'-f','f32le','-c:a','pcm_f32le','-'],capture_output=True,check=True).stdout
        floats=array.array('f');floats.frombytes(raw)
        assert max(abs(floats[i]+floats[i+1]) for i in range(0,len(floats),2))<1e-6,'anti-phase destroyed'
        assert invoke('rate',mode='music',music='44100.wav')['samples']==96000
        assert invoke('short',mode='mix',voice='short.wav')['samples']==216000
        silent=invoke('silence',voice='silence.wav');assert not silent['target_met'];assert silent['status']=='silence_or_below_gate'
        assert invoke('noise',voice='noise.wav',extra=['--noise','fft'])['target_met']
        pauses=root/'pauses'
        p=run([worker,'--mode','pauses','--voice',inputs/'voice.wav','--out-dir',pauses,'--snapshot-token','project:sequence:revision:α'])
        pause_report=json.loads((pauses/'report.json').read_text())
        assert pause_report['mode']=='pauses' and not (pauses/'result.wav').exists()
        assert len(pause_report['pause_cuts'])==2
        assert all(abs((end-start)-.4)<.03 for start,end in pause_report['pause_cuts'])
        results.append({'name':'pauses-keep-300ms','passed':True,'cuts':pause_report['pause_cuts']})
        crest=invoke('crest',mode='music',music='crest.wav');assert crest['after']['true_peak_dbtp']<=-1.4
        bypass=invoke('bypass',extra=['--strength','0','--normalize','off']);assert bypass['settings']['normalize'] is False
        invoke('reject-missing',voice='not-there.wav',fail=True)
        invoke('reject-same',mode='mix',music='voice.wav',fail=True)
        invoke('reject-nan',extra=['--target','nan'],fail=True)
        invoke('reject-surround',voice='surround.wav',fail=True)
        invoke('reject-rnnoise',extra=['--noise','rnnoise'],fail=True)
        invoke('reject-codec-tool',extra=['--ffmpeg','/definitely-not-a-binary'],fail=True)
        # Existing destination must survive intact.
        old=(root/'mix/result.wav').read_bytes();p=subprocess.run([str(worker),'--mode','voice','--voice',str(inputs/'voice.wav'),'--out-dir',str(root/'mix')],capture_output=True)
        assert p.returncode!=0 and (root/'mix/result.wav').read_bytes()==old
        results.append({'name':'existing-result-not-overwritten','passed':True})
        # The final source hash must reject a file changed after it was processed.
        mutable=inputs/'mutable.wav';shutil.copyfile(inputs/'voice.wav',mutable)
        out=root/'stale';p=subprocess.Popen([str(worker),'--mode','voice','--voice',str(mutable),'--out-dir',str(out)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        edited=False
        for line in p.stderr:
            if line.startswith('PROGRESS\t87\t'):
                with mutable.open('ab') as f:f.write(b'changed-during-analysis')
                edited=True
        p.wait(timeout=10);assert edited and p.returncode!=0 and not out.exists()
        results.append({'name':'stale-source-rejected','passed':True})
        # Cancellation during a real second-pass job leaves no published directory.
        out=root/'cancel';p=subprocess.Popen([str(worker),'--mode','voice','--voice',str(inputs/'voice.wav'),'--out-dir',str(out)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        interrupted=False
        for line in p.stderr:
            if line.startswith('PROGRESS\t75\t'):
                p.send_signal(signal.SIGTERM);interrupted=True;break
        p.communicate(timeout=5);assert interrupted and p.returncode==130 and not out.exists()
        assert not list(root.glob('.studio-audio-*')),'orphan temporary directory'
        results.append({'name':'cancel-during-normalization-no-partial-result','passed':True})
        results.extend(check_alignment())
    destination=Path(a.report);destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_text(json.dumps({'suite':'integration','worker':str(worker),'ffmpeg':run(['ffmpeg','-version']).stdout.splitlines()[0], 'passed':len(results),'cases':results},ensure_ascii=False,indent=2)+'\n')
    print(f'{len(results)} integration scenarios passed; report: {destination}')
if __name__=='__main__':main()
