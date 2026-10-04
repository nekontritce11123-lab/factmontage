#!/usr/bin/env python3
"""One short MLT timeline using the nine installed Studio engines."""
import array
import json
import math
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import wave


ROOT = Path(__file__).resolve().parents[2]


def run(*args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed ({result.returncode}): {result.stderr[-4000:].decode(errors="replace")}')
    return result.stdout


def picture(path, background, foreground):
    pixels = bytearray()
    for y in range(180):
        for x in range(320):
            pixels.extend(foreground if 95 <= x < 225 and 42 <= y < 150 else background)
    path.write_bytes(b'P6\n320 180\n255\n' + pixels)


def audio(path):
    samples = array.array('h', (int(5200 * math.sin(2 * math.pi * 220 * i / 48000)) for i in range(144000)))
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(48000)
        output.writeframes(samples.tobytes())


def recognition_contract(folder, source, prefix=Path('/app')):
    tone = folder / 'not-speech.wav'
    run('ffmpeg', '-v', 'error', '-i', source, '-ac', '1', '-ar', '16000', '-c:a', 'pcm_s16le', tone)
    model = prefix / 'share/studio-subtitles/ggml-small-q5_1.bin'
    vad = prefix / 'share/studio-subtitles/ggml-silero-v5.1.2.bin'
    output = folder / 'native-recognition'
    run(prefix / 'bin/whisper-cli', '-m', model, '-f', tone, '--vad', '--vad-model', vad,
        '-ojf', '-of', output, '--no-prints')
    native = json.loads(output.with_suffix('.json').read_text(encoding='utf-8'))
    assert native['params']['token_timestamps'] == 'original', 'Whisper uses processed token timestamps'
    assert native['vad_segments'] == [], 'VAD classified the pure tone as speech'
    assert native['transcription'] == [], 'Whisper transcribed the pure tone'
    assets = folder / 'non-speech-assets'
    rejected = subprocess.run([str(prefix / 'libexec/studio-subtitle'), '--audio', str(tone),
                               '--model', str(model), '--vad-model', str(vad),
                               '--whisper-cli', str(prefix / 'bin/whisper-cli'), '--out-dir', str(assets)],
                              capture_output=True, timeout=120)
    assert rejected.returncode == 1 and 'не найдена речь' in rejected.stderr.decode('utf-8'), rejected.stderr
    assert not list(assets.glob('*.ass')) and not list(assets.glob('*.words.json')) and not list(assets.glob('*.blocks.json'))


def main():
    with tempfile.TemporaryDirectory(prefix='studio-smoke-') as temporary:
        folder = Path(temporary) / 'проект с пробелами'
        folder.mkdir()
        first, second = folder / 'a.ppm', folder / 'b.ppm'
        picture(first, (12, 190, 35), (225, 58, 72))
        picture(second, (28, 65, 215), (245, 200, 50))
        source = folder / 'voice.wav'
        audio(source)
        recognition_contract(folder, source)
        processed = folder / 'audio'
        run('/app/libexec/studio-audio', '--mode', 'voice', '--voice', source, '--out-dir', processed,
            '--snapshot-token', 'studio-smoke')
        sound = processed / 'result.wav'
        assert sound.is_file() and sound.stat().st_size > 1000
        recognition = folder / 'recognition.wav'
        run('ffmpeg', '-v', 'error', '-i', sound, '-ac', '1', '-ar', '16000', '-c:a', 'pcm_s16le', recognition)

        fake = ROOT / 'Subtitles/tests/fake_whisper_cli.sh'
        whisper = folder / 'whisper.sh'
        whisper.write_text('#!/bin/sh\nexec /bin/sh ' + shlex.quote(str(fake)) + ' "$@"\n', encoding='utf-8')
        whisper.chmod(0o755)
        model = folder / 'model.bin'
        model.write_bytes(b'studio-smoke-fake-model')
        vad = folder / 'vad.bin'
        vad.write_bytes(b'studio-smoke-fake-vad')
        subtitle = json.loads(run('/app/libexec/studio-subtitle', '--audio', recognition, '--model', model,
                                  '--vad-model', vad, '--whisper-cli', whisper, '--out-dir', folder / 'subtitles'))
        ass = Path(subtitle['ass'])
        assert ass.is_file() and 'Привет' in ass.read_text(encoding='utf-8')

        scene = folder / 'надпись с пробелом.stxt'
        shutil.copyfile(ROOT / 'Text/examples/life_v02.stxt', scene)
        profile = folder / 'smoke.profile'
        profile.write_text('description=Studio smoke\nframe_rate_num=30\nframe_rate_den=1\nwidth=320\nheight=180\n'
                           'progressive=1\nsample_aspect_num=1\nsample_aspect_den=1\n'
                           'display_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n', encoding='ascii')
        for service in ('studio.background', 'studio.camera', 'frei0r.studiofx', 'studio.color',
                        'frei0r.card3d', 'frei0r.sunimo_text_studio', 'avfilter.subtitles'):
            assert f'identifier: {service}'.encode() in run('melt', '-query', f'filter={service}'), service
        assert b'identifier: frei0r.sunimo_transition' in run('melt', '-query', 'transition=frei0r.sunimo_transition')
        video = folder / 'result.mkv'
        def render(output, with_text=True, with_subtitles=True):
            command = ['melt', '-silent', '-loglevel', 'error', '-profile', profile,
                       f'qimage:{first}', 'out=44',
                       '-attach-clip', 'studio.background', 'method=0', 'output=1', 'key_r=.047', 'key_g=.745', 'key_b=.137',
                       'tolerance=.13', 'transition=.08', 'despill=.65', 'feather=1.2',
                       '-attach-clip', 'studio.camera', 'mode=4', 'live=1', 'zoom=110', 'start_x=30', 'end_x=70',
                       '-attach-clip', 'frei0r.studiofx', '0=.07874015748031496', '1=.46', '2=.47', '3=.5', '4=1',
                       '5=.5', '6=.5', '7=.65', '8=1', '9=.137', '10=0', 'threads=1',
                       '-attach-clip', 'studio.color', 'studio_input_validated=1', 'studio_color_algorithm=sdr-primary-v1',
                       'exposure=.2', 'saturation=105',
                       '-attach-clip', 'frei0r.card3d', '0=0', '1=.25', '2=0', '3=.16666666666666666',
                       '4=.68', '5=.34', '6=0', '7=1']
            if with_text:
                command += ['-attach-clip', 'frei0r.sunimo_text_studio', f'0={scene}']
            command += [f'qimage:{second}', 'out=44', '-mix', '15', '-mixer', 'frei0r.sunimo_transition',
                        '0=0=0;14=1', '1=0', '2=.45', '3=0', '4=.35', '5=.5 .5', '6=0']
            if with_subtitles:
                command += ['-attach', 'avfilter.subtitles', f'av.filename={ass}']
            command += ['-audio-track', sound, 'out=74']
            run(*command, '-consumer', f'avformat:{output}', 'vcodec=ffv1', 'acodec=pcm_s16le', 'real_time=-1')

        render(video)
        streams = json.loads(run('ffprobe', '-v', 'error', '-show_streams', '-of', 'json', video))['streams']
        assert {'video', 'audio'} <= {stream['codec_type'] for stream in streams}, streams
        frames = run('ffmpeg', '-v', 'error', '-i', video, '-vf', 'scale=48:27', '-f', 'rawvideo',
                     '-pix_fmt', 'rgb24', '-')
        size = 48 * 27 * 3
        assert len(frames) == 75 * size, len(frames)
        first_frame, seam, last_frame = (frames[index * size:(index + 1) * size] for index in (5, 37, 70))
        assert len({first_frame, seam, last_frame}) == 3
        assert any(value < 220 for value in seam), 'white frame'
        def frame_at(path):
            data = run('ffmpeg', '-v', 'error', '-i', path, '-vf', 'select=eq(n\\,30),scale=48:27',
                       '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-')
            assert len(data) == size
            return data

        without_text = folder / 'without-text.mkv'
        render(without_text, with_text=False)
        assert frame_at(video) != frame_at(without_text), 'Text filter did not alter the frame'
        without_subtitles = folder / 'without-subtitles.mkv'
        render(without_subtitles, with_subtitles=False)
        assert frame_at(video) != frame_at(without_subtitles), 'Subtitle filter did not alter the frame'
        samples = run('ffmpeg', '-v', 'error', '-i', video, '-map', '0:a:0', '-f', 's16le', '-')
        assert len(samples) > 96000 and any(samples), 'silent or absent audio'
        print(json.dumps({'video_frames': 75, 'audio_bytes': len(samples), 'subtitle_ass': ass.is_file(),
                          'distinct_seam_frames': True, 'text_visible': True, 'subtitles_visible': True,
                          'native_asr_contract': True, 'non_speech_rejected': True,
                          'status': 'PASS'}))


if __name__ == '__main__':
    main()
