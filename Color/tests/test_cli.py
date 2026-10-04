#!/usr/bin/env python3
"""CLI + independent FFmpeg interpolation checks. Stdlib only."""
import json, pathlib, shutil, subprocess, sys, tempfile, random, math
exe=pathlib.Path(sys.argv[1]).resolve()
def run(args,ok=True):
 p=subprocess.run([str(exe),*map(str,args)],capture_output=True,text=True)
 assert (p.returncode==0)==ok,(args,p.stdout,p.stderr)
 return p
with tempfile.TemporaryDirectory(prefix='color tests кириллица ') as tmp:
 d=pathlib.Path(tmp);w,h=64,48
 rng=random.Random(19)
 data=bytearray(rng.randrange(256) for _ in range(w*h*4))
 for i in range(3,len(data),4):data[i]=255
 (d/'source.rgba').write_bytes(data)
 run(['process',d/'source.rgba',d/'identity.rgba',w,h])
 assert (d/'identity.rgba').read_bytes()==data
 run(['process',d/'source.rgba',d/'identity.rgba',w,h],False) # cannot overwrite
 run(['cube',d/'grade.cube','--preset','sunimo'])
 run(['process',d/'source.rgba',d/'ours.rgba',w,h,'--preset','sunimo'])
 stats=json.loads(run(['analyze',d/'source.rgba',w,h]).stdout)
 assert stats['schema']=='studio.color.stats/1'
 run(['cube',d/'bad.cube','--set','exposure=nan'],False)
 run(['cube',d/'bad.cube','--set','exposure=99'],False)
 (d/'truncated.rgba').write_bytes(data[:-1])
 run(['analyze',d/'truncated.rgba',w,h],False)
 (d/'clips.tsv').write_text(f'a\tvideo\tsource.rgba\t{w}\t{h}\nvoice\taudio\tunused\t0\t0\npng\timage\tsource.rgba\t{w}\t{h}\nlogo\tgraphic\tsource.rgba\t{w}\t{h}\nhdr\thdr\tunused\t0\t0\n',encoding='utf-8')
 result=json.loads(run(['batch',d/'clips.tsv',d/'batch result','--preset','sunimo']).stdout)
 assert result['audio_skipped']==1 and result['unsupported_skipped']==1
 assert len(list((d/'batch result').glob('*.rgba')))==3
 assert not (d/'batch result'/'clip-1.rgba').exists()
 if shutil.which('ffmpeg'):
  # cwd avoids shell quoting/filtergraph escaping of non-ASCII or spaces in full paths.
  proc=subprocess.run(['ffmpeg','-v','error','-f','rawvideo','-pixel_format','rgba','-video_size',f'{w}x{h}','-i','source.rgba','-vf','lut3d=file=grade.cube:interp=tetrahedral','-pix_fmt','rgba','-f','rawvideo','ffmpeg.rgba'],cwd=d,capture_output=True)
  assert proc.returncode==0,proc.stderr.decode(errors='replace')
  a=(d/'ours.rgba').read_bytes();b=(d/'ffmpeg.rgba').read_bytes()
  assert len(a)==len(b)
  differences=[abs(x-y) for i,(x,y) in enumerate(zip(a,b)) if i%4!=3]
  assert max(differences)<=1, max(differences) # FFmpeg truncates; kernel rounds to nearest.
  assert a[3::4]==b[3::4]
  print('FFmpeg tetrahedral cross-check: max LSB',max(differences),'mean LSB',sum(differences)/len(differences))
 else: print('SKIP: FFmpeg not installed; independent interpolation cross-check not run')
 print('CLI validation, non-destructive output, unicode paths, batch mixed media: PASS')
