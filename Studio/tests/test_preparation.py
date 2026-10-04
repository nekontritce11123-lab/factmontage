"""Check integration on the actual pinned source and package input invariants."""
from pathlib import Path
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/'scripts'/(name+'.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PreparationTests(unittest.TestCase):
    def test_audio_controller_keeps_results_recoverable_and_subtitles_in_sync(self):
        source = (ROOT/'overlay/audio/studioaudio.cpp').read_text(encoding='utf-8')
        self.assertIn('studioJobForSources', source)
        self.assertIn('resultClipForJob', source)
        self.assertIn('adjustSubtitlesForCut(model, cut, undo, redo, error)', source)
        self.assertIn('requestSubtitleMove', source)
        self.assertIn('studio:audio:sha256', source)
        self.assertIn('studio:audio:source-role', source)
        self.assertIn('studio:audio:source-count', source)
        self.assertIn('m_controller->recover(&error)', source)
        self.assertIn('audioTrackSelection', source)
        self.assertIn('Вся выбранная дорожка', source)
        self.assertIn('Обработать заново', source)

    def test_panel_contains_v2_workflows(self):
        panel = (ROOT/'overlay/studiopanel.cpp').read_text(encoding='utf-8')
        header = (ROOT/'overlay/studiopanel.hpp').read_text(encoding='utf-8')
        self.assertNotIn('QTabWidget', panel + header)
        self.assertIn('QSaveFile', panel)
        self.assertIn('getMasterEffectStackModel', panel)
        self.assertIn('Применить к %1 клипам', panel)
        self.assertIn('MonitorSceneGeometry', panel)
        self.assertIn('moveCameraBeforeCard', panel)
        self.assertIn('previewScaling', panel)
        self.assertIn('studio-head-tracker', panel)
        self.assertIn('StudioJobs::thumbnail', panel)
        self.assertNotIn('clip->fetchPixmap', panel)
        for method, label in [('loadBackgroundFrame', 'm_backgroundFrame'), ('loadTrackingFrame', 'm_trackingFrame')]:
            body = panel.split(f'void StudioPanel::{method}()', 1)[1].split('\nvoid StudioPanel::', 1)[0]
            self.assertLess(body.index(f'{label}->setText(QStringLiteral("Подготовка стоп-кадра…"))'),
                            body.index('StudioJobs::thumbnail'))
        self.assertIn('Нажмите на лицо в стоп-кадре', panel)
        self.assertIn('Применить трекинг', panel)
        self.assertNotIn('Выбрать голову в мониторе клипа', panel)
        self.assertNotIn('startHeadSelection', panel + header)
        self.assertIn('key == QLatin1String("tracking")', panel)
        self.assertIn('key == QLatin1String("BACKGROUND")', panel)
        self.assertIn('cardExitEnd', panel)
        self.assertIn('QStringLiteral("17")', panel)
        self.assertIn('stackUsesStillImage', panel)
        self.assertIn('clip->clipType() == ClipType::Image', panel)
        self.assertIn('QStringLiteral("7")', panel)
        self.assertIn('next[source] = QStringLiteral("1")', panel)
        self.assertIn('const QString source = stackUsesStillImage(m_stack)', panel)
        self.assertNotIn('ensureCardExit', panel)
        self.assertNotIn('fade_to_black', panel)
        self.assertNotIn('getParamFromName', panel)
        self.assertIn('QTabBar', panel + header)
        self.assertIn('QActionGroup', panel)
        self.assertNotIn('Готовый трек', panel)
        self.assertIn('QFileDialog::getOpenFileName', panel)  # user-selected transition sound
        self.assertIn('it.value().header->setChecked(false)', panel)
        self.assertIn('camera ? 13 : 17', panel)
        self.assertIn('QCheckBox', panel + header)
        self.assertIn('Анимировать уход', (ROOT.parent/'Card_3D/kdenlive/studio.json').read_text(encoding='utf-8'))
        self.assertIn('values.insert(QStringLiteral("10"), QStringLiteral("0.35"))', panel)
        self.assertIn('Расположить карточку в мониторе', panel)
        self.assertIn('m_cardPositioning', panel + header)
        self.assertIn('key == QLatin1String("X") || key == QLatin1String("Y")', panel)
        self.assertIn('m_cameraTabs->addTab(QStringLiteral("Трекинг лица"))', panel)
        self.assertIn('m_pageTitle->setText', panel)
        self.assertIn('m_page == 2 ? it.key() == QLatin1String("tracking")', panel)
        self.assertIn('studioPrimary', panel)
        self.assertIn('projectDataFolder()', panel)
        self.assertIn('studio-tracks/%1', panel)
        self.assertIn('m_effectsNav->setText(QStringLiteral("Эффекты"))', panel)
        self.assertIn('m_color->setText(QStringLiteral("Цвет"))', panel)
        self.assertIn('m_audio->setText(QStringLiteral("Звук"))', panel)
        self.assertIn('StudioAudioPage', panel + header)
        self.assertIn('m_subtitles->setText(QStringLiteral("Текст"))', panel)
        self.assertIn('m_textTabs->addTab(QStringLiteral("Надписи"))', panel)
        self.assertIn('StudioSubtitlePage', panel + header)
        self.assertIn('StudioTextPage', panel + header)
        self.assertIn('new StudioFlowLayout', panel)
        self.assertIn('m_background->setText(QStringLiteral("Фон"))', panel)
        self.assertIn('backgroundAnalysisXml', panel)
        self.assertIn('studio-background-analyzer', panel)
        self.assertIn('const int frames = staticImage ? 1 : timelineFrames;', panel)
        self.assertNotIn('std::min(timelineFrames, availableFrames)', panel)
        self.assertIn('QStringLiteral("in=%1").arg(sourceIn)', panel)
        self.assertIn('readyReadStandardError', panel)
        self.assertIn('QStringLiteral("avformat:pipe:1")', panel)
        self.assertNotIn('QStringLiteral("avformat:-")', panel)
        self.assertIn('QStringLiteral("0=0;%1=1").arg(std::max(0, frames - 1))', panel)
        self.assertIn('backgroundNeedsAnalysis()', panel + header)
        self.assertIn('const bool stagedBackground', panel)
        self.assertIn('Проект не изменён; выполните анализ человека', panel)
        self.assertIn('StudioBackground::canUseSingleFrameMask', panel)
        self.assertIn('moveBackgroundBeforeStudio', panel)
        self.assertIn('StudioHelpers::setBackgroundMaskPath(effect, resolved)', panel)
        self.assertIn('QStringLiteral("studio_color")', panel)
        self.assertIn('moveColorBeforeStudio', panel)
        self.assertIn('schema.value(QStringLiteral("presets"))', panel)
        self.assertIn('schema.value(QStringLiteral("parameters"))', panel)
        self.assertNotIn('QStringList backgroundVisualParameters()', panel)
        color_schema = json.loads((ROOT.parent/'Color/kdenlive/studio_color.json').read_text(encoding='utf-8'))
        self.assertEqual(len(color_schema['presets']), 8)
        self.assertNotIn('auto_strength', [control['key'] for control in color_schema['controls']])
        self.assertIn('applyEffectRecipe', panel + header)
        self.assertIn('moveStudioFxBeforeCard', panel)
        self.assertIn('StudioFX/favorites', panel)
        self.assertIn('QStringLiteral("studiofx")', panel)
        self.assertIn('m_transitions->setText(QStringLiteral("Переходы"))', panel)
        self.assertIn('studioTransitionSelection', panel)
        self.assertIn('removeStudioTransition', panel)
        self.assertIn('transitionPresetFolder', panel)
        self.assertIn('studio:sfx_source', panel)
        self.assertIn('visible_styles', (ROOT.parent/'Transitions/kdenlive/studio_transition.json').read_text(encoding='utf-8'))

    def test_integration_targets_pinned_source(self):
        source = ROOT/'upstream/kdenlive-26.08.0'
        paths = ['src/assets/CMakeLists.txt', 'src/mainwindow.cpp', 'src/main.cpp',
                 'src/timeline2/view/timelinetabs.hpp', 'src/timeline2/view/timelinetabs.cpp',
                 'src/effects/effectsrepository.cpp',
                 'src/doc/documentvalidator.cpp',
                 'src/effects/effectstack/model/effectstackmodel.hpp',
                 'src/effects/effectstack/model/effectstackmodel.cpp',
                 'src/timeline2/model/timelinefunctions.cpp',
                 'src/timeline2/model/timelinemodel.hpp', 'src/timeline2/model/timelinemodel.cpp',
                 'src/timeline2/model/trackmodel.hpp', 'src/timeline2/model/trackmodel.cpp',
                 'src/render/renderrequest.cpp', 'src/doc/kdenlivedoc.cpp', 'tests/CMakeLists.txt',
                 'src/doc/kdenlivedoc.h', 'src/project/projectmanager.cpp',
                 'src/project/dialogs/archivewidget.cpp', 'src/project/dialogs/archivewidget.h', 'tests/test_utils.hpp',
                 'src/dialogs/subtitleedit.cpp', 'src/bin/model/subtitlemodel.cpp',
                 'src/timeline2/model/builders/meltBuilder.cpp', 'src/bin/projectitemmodel.cpp', 'src/CMakeLists.txt',
                 'src/assets/model/assetparametermodel.cpp']
        integrate = load('integrate')
        with tempfile.TemporaryDirectory() as folder:
            for relative in paths + ['src/mainwindow.h', 'src/core.cpp', 'src/effects/effectstack/model/effectitemmodel.cpp',
                    'src/effects/effectstack/view/effectstackview.cpp',
                    'src/jobs/abstracttask.h', 'src/jobs/taskmanager.h', 'src/jobs/taskmanager.cpp',
                    'src/jobs/proxytask.h', 'src/jobs/proxytask.cpp', 'src/jobs/cliploadtask.h', 'src/jobs/cliploadtask.cpp',
                    *[f'src/jobs/{name}task.cpp' for name in ('melt', 'filter', 'transcode', 'stabilize', 'speed', 'cut', 'scenesplit', 'mask', 'customjob')],
                    'src/bin/projectclip.cpp', 'src/kdenlivesettings.kcfg', 'src/monitor/monitor.h', 'src/monitor/monitor.cpp', 'src/monitor/videowidget.cpp', 'src/timeline2/view/timelinecontroller.h', 'src/timeline2/view/timelinecontroller.cpp',
                    'src/timeline2/view/previewmanager.h', 'src/timeline2/view/previewmanager.cpp', 'src/dialogs/renderwidget.cpp',
                    'renderer/kdenlive_render.cpp', 'src/icons.qrc']:
                dest = Path(folder)/relative
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source/relative, dest)
            previous = Path.cwd()
            try:
                os.chdir(folder)
                integrate.main()
                effect_view = Path('src/effects/effectstack/view/effectstackview.cpp').read_text(encoding='utf-8')
                load_effects = effect_view.split('void EffectStackView::loadEffects()', 1)[1].split('\nvoid EffectStackView::', 1)[0]
                self.assertLess(load_effects.index('if (!m_model) return;'), load_effects.index('m_model->plugBuiltinEffects()'))
                tasks = Path('src/jobs/taskmanager.cpp').read_text(encoding='utf-8')
                close = Path('src/project/projectmanager.cpp').read_text(encoding='utf-8').split('bool ProjectManager::closeCurrentDocument(', 1)[1].split('bool ProjectManager::saveFileAs(', 1)[0]
                self.assertLess(close.index('waitUntilIdle()'), close.index('pCore->cleanup()'))
                video_widget = Path('src/monitor/videowidget.cpp').read_text(encoding='utf-8')
                for signature in ('const QString &file', 'const std::shared_ptr<Mlt::Producer> &producer, bool isActive, int position'):
                    switch = video_widget.split(f'int VideoWidget::setProducer({signature})', 1)[1].split('\n}\n', 1)[0]
                    self.assertLess(switch.index('stop();'), switch.index('m_producer.reset();'))
                    self.assertLess(switch.index('stop();'), switch.index('reconfigure();'))
                shared_switch = video_widget.split('int VideoWidget::setProducer(const std::shared_ptr<Mlt::Producer>', 1)[1].split('\n}\n', 1)[0]
                self.assertIn('if (isActive) {\n        startConsumer();', shared_switch)
                self.assertIn('loop.exec(QEventLoop::ExcludeUserInputEvents)', tasks)
                self.assertIn('m_taskPool.activeThreadCount() == 0', tasks)
                self.assertIn('_sunimoAutomaticProxy', tasks)
                for name in ('melt', 'filter', 'transcode', 'stabilize', 'speed', 'cut', 'scenesplit', 'mask', 'customjob'):
                    worker = Path(f'src/jobs/{name}task.cpp').read_text(encoding='utf-8')
                    self.assertNotIn('waitForFinished(-1)', worker)
                    self.assertIn('StudioWorker::wait', worker)
                controller = Path('src/timeline2/view/timelinecontroller.cpp').read_text(encoding='utf-8')
                self.assertIn('pCore->currentTimelineId() != m_model->uuid()', controller)
                self.assertIn('m_disablePreview->isChecked()', controller)
                preview = Path('src/timeline2/view/previewmanager.cpp').read_text(encoding='utf-8')
                request = preview.split('void PreviewManager::requestAutomaticPreview(', 1)[1].split('void PreviewManager::startPreviewRender(', 1)[0]
                self.assertLess(request.index('!m_studioAutomatic'), request.index('m_studioAutomatic = true'))
                export = Path('src/dialogs/renderwidget.cpp').read_text(encoding='utf-8').split('void RenderWidget::startRendering(', 1)[1]
                self.assertLess(export.index('item->setStatus(STARTINGJOB)'), export.index('QTimer::singleShot(100'))
                self.assertIn('StudioPanel', Path(paths[1]).read_text(encoding='utf-8'))
                self.assertIn('assets/studio/audio/studioaudio.cpp', Path(paths[0]).read_text(encoding='utf-8'))
                self.assertIn('assets/studio/subtitles/studiosubtitles.cpp', Path(paths[0]).read_text(encoding='utf-8'))
                repository = Path(paths[5]).read_text(encoding='utf-8')
                self.assertIn('if (exists(QStringLiteral("card3d")))', repository)
                self.assertIn('m_assets.erase(QStringLiteral("frei0r.card3d"))', repository)
                self.assertIn('m_assets.erase(QStringLiteral("frei0r.studiofx"))', repository)
                self.assertIn('m_assets.erase(QStringLiteral("studio.background"))', repository)
                self.assertNotIn('studio_transition', repository)
                self.assertGreaterEqual(Path(paths[13]).read_text(encoding='utf-8').count('studio:sfx_source'), 2)
                effect_stack = Path(paths[8]).read_text(encoding='utf-8')
                archive_widget = Path(paths[19]).read_text(encoding='utf-8')
                self.assertIn('filter.property_exists("0") && QString::fromUtf8(filter.get("mlt_service")) == QLatin1String("frei0r.sunimo_text_studio")', effect_stack)
                self.assertIn('url = filter.get("0");', effect_stack)
                self.assertIn('propertyProcessUrl(e, QStringLiteral("0"), root);', archive_widget)
                self.assertIn('studioEffectAbsolutePaths(playlist, root)', Path(paths[-3]).read_text(encoding='utf-8'))
                self.assertIn('studiotext.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions"', Path(paths[-2]).read_text(encoding='utf-8'))
                validator = Path(paths[6]).read_text(encoding='utf-8')
                self.assertIn('Append-only Card 3D parameters', validator)
                self.assertIn('QDir(mlt.attribute(QStringLiteral("root"))).absoluteFilePath(scene)', validator)
                self.assertIn('QStringLiteral("9")', validator)
                self.assertIn('QStringLiteral("10")', validator)
                self.assertIn('QStringLiteral("11")', validator)
                self.assertIn('QStringLiteral("12")', validator)
                self.assertIn('QStringLiteral("13")', validator)
                self.assertIn('QStringLiteral("14")', validator)
                self.assertIn('QStringLiteral("17")', validator)
                integration = Path(paths[9]).read_text(encoding='utf-8')
                self.assertIn('preserveStudioCameraCut', integration)
                self.assertIn('getParam(QStringLiteral("studio_time_origin"))',
                              Path(paths[8]).read_text(encoding='utf-8'))
                self.assertIn('studioCardExitEnd', Path(paths[8]).read_text(encoding='utf-8'))
                self.assertIn('getAssetId() == QLatin1String("studiofx")', Path(paths[8]).read_text(encoding='utf-8'))
                self.assertIn('setParameter(QStringLiteral("10")', Path(paths[8]).read_text(encoding='utf-8'))
                self.assertIn('(newIn - oldIn) / pCore->getCurrentFps()', Path(paths[8]).read_text(encoding='utf-8'))
                self.assertIn('setParameter(QStringLiteral("sample_offset")', Path(paths[8]).read_text(encoding='utf-8'))
                self.assertIn('leftBackground', Path(paths[8]).read_text(encoding='utf-8'))
                timeline_header = Path(paths[10]).read_text(encoding='utf-8')
                timeline_source = Path(paths[11]).read_text(encoding='utf-8')
                track_source = Path(paths[13]).read_text(encoding='utf-8')
                self.assertIn('StudioTransitionStatus', timeline_header)
                self.assertIn('updateStudioTransition', timeline_source)
                self.assertIn('removeStudioTransition', timeline_source)
                self.assertIn('studio:transition', timeline_source)
                self.assertIn('initialParameters', track_source)
                mix = timeline_source.split('bool TimelineModel::mixClip(', 1)[1].split('bool TimelineModel::requestClipMix(', 1)[0]
                self.assertNotIn('extractZoneWithUndo', mix)
                self.assertNotIn('consumedFrames', timeline_header + timeline_source)
                self.assertNotIn('needsRebuild', timeline_header + timeline_source)
                self.assertIn('StudioTransitionStatus::SameSourceFrames', timeline_source)
                self.assertIn('if (getMixDuration(cid) == duration) return true;', timeline_source)
                self.assertIn('getMixAlign(cid), -1, undo, redo', timeline_source)
                child_source = Path('src/effects/effectstack/model/effectitemmodel.cpp').read_text(encoding='utf-8')
                self.assertEqual(child_source.count('for (const char *name : {"_sbg_mask_path", "_sbg_clip_in"})'), 2)
                self.assertIn('StudioHelpers::setBackgroundClipIn(effect, newIn)', effect_stack)
                self.assertIn('StudioHelpers::setBackgroundClipIn(dest, nextClipIn)', effect_stack)
                render_source = Path(paths[14]).read_text(encoding='utf-8')
                self.assertIn('validateStudioBackgrounds', render_source)
                self.assertIn('StudioBackground::validateMask(request)', render_source)
                self.assertIn('QStringLiteral("_sbg_mask_path")', render_source)
                parameter_model = Path(paths[-1]).read_text(encoding='utf-8')
                self.assertIn('const bool exactStudioTime', parameter_model)
                self.assertIn('m_asset->set(name.toLatin1().constData(), paramValue.toUtf8().constData())', parameter_model)
                mainwindow = Path(paths[1]).read_text(encoding='utf-8')
                shutdown = mainwindow.split('MainWindow::~MainWindow()', 1)[1].split('// virtual', 1)[0]
                self.assertLess(shutdown.index('delete m_studioPanel.data();'), shutdown.index('pCore->prepareShutdown();'))
                self.assertLess(shutdown.index('delete m_studioPanel.data();'), shutdown.index('Mlt::Factory::close();'))
                self.assertIn('m_studioPanel = studio;', mainwindow)
                self.assertIn('m_studioPanel->setPalette(qApp->palette());', mainwindow)
                self.assertIn('QPointer<StudioPanel> m_studioPanel;', Path('src/mainwindow.h').read_text(encoding='utf-8'))
                self.assertIn('studioArchiveDocument', mainwindow)
                document_source = Path(paths[15]).read_text(encoding='utf-8')
                self.assertIn('StudioBackground::restoreMaskPaths', document_source)
                self.assertIn('studio-background', document_source)
                self.assertIn('copyStudioAssetsForSave', document_source)
                self.assertIn('QSaveFile output(destination)', document_source)
                with self.assertRaises(RuntimeError): integrate.main()
            finally:
                os.chdir(previous)

    def test_package_is_separate_and_keeps_permissions(self):
        prepare = load('prepare')
        with tempfile.TemporaryDirectory() as folder:
            prepare.main(output=folder)
            self.assertEqual((Path(folder)/'overlay/text/scenecompiler.hpp').read_bytes(),
                             (ROOT.parent/'Text/qt/scenecompiler.hpp').read_bytes())
            self.assertEqual((Path(folder)/'overlay/text/styles.hpp').read_bytes(),
                             (ROOT.parent/'Text/qt/styles.hpp').read_bytes())
            upstream = json.loads((ROOT/'upstream/org.kde.kdenlive.json').read_text(encoding='utf-8'))
            path = Path(folder)/'local.VideoStudio.Kdenlive.json'
            studio = json.loads(path.read_text(encoding='utf-8'))
            for archive_path in Path(folder).glob('*-source.tar.gz'):
                if archive_path.name == 'studio-changes-source.tar.gz':
                    continue
                extracted = Path(folder)/archive_path.name.removesuffix('.tar.gz')
                with tarfile.open(archive_path) as bundle:
                    bundle.extractall(extracted, filter='data')
                checked = subprocess.run([sys.executable, '-B', str(extracted/'scripts/generate_ui.py'), '--check'],
                                         capture_output=True, text=True)
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            archive = Path(folder)/'studiofx-source.tar.gz'
            self.assertTrue(archive.is_file())
            transitions_archive = Path(folder)/'studio-transitions-source.tar.gz'
            self.assertTrue(transitions_archive.is_file())
            audio_archive = Path(folder)/'studio-audio-source.tar.gz'
            self.assertTrue(audio_archive.is_file())
            subtitles_archive = Path(folder)/'studio-subtitles-source.tar.gz'
            self.assertTrue(subtitles_archive.is_file())
            background_archive = Path(folder)/'studio-background-source.tar.gz'
            self.assertTrue(background_archive.is_file())
            with tarfile.open(Path(folder)/'studio-changes-source.tar.gz') as bundle:
                changes_files = {item.name for item in bundle.getmembers() if item.isfile()}
            self.assertIn('Studio/patches/whisper-vad-json-timestamps.patch', changes_files)
            for name in ('PROJECTS.md', 'PROJECTS_RU.md', 'RELEASE_NOTES.md', 'RELEASE_NOTES_RU.md', 'TESTING.md'):
                self.assertIn('docs/' + name, changes_files)
            timing_patch = next(m for m in studio['modules'] if m['name'] == 'whisper-cpp')['sources'][1]
            self.assertEqual(timing_patch['path'], 'whisper-vad-json-timestamps.patch')
            self.assertEqual(timing_patch['sha256'], prepare.sha(Path(folder)/timing_patch['path']))
            self.assertIn('dev', changes_files)
            self.assertIn('Studio/modules.json', changes_files)
            self.assertIn('Studio/tests/full_smoke.py', changes_files)
            with tarfile.open(archive) as bundle:
                files = [item.name for item in bundle.getmembers() if item.isfile()]
            self.assertEqual([name for name in files if name.endswith('.xml')],
                             ['kdenlive/studio_lens.xml', 'kdenlive/studio_vintage.xml', 'kdenlive/studiofx.xml'])
            self.assertEqual(len([name for name in files if name.startswith('previews/') and name.endswith('.png')]), 80)
            self.assertFalse(any('EffectsBrowser' in name or '/build/' in name or name.endswith(('.so', '.dll', '.exe')) for name in files))
            with tarfile.open(transitions_archive) as bundle:
                transition_files = [item.name for item in bundle.getmembers() if item.isfile()]
            self.assertEqual(len([name for name in transition_files if name.startswith('previews/') and name.endswith('.png')]), 15)
            self.assertEqual(len([name for name in transition_files if name.startswith('previews/') and name.endswith('.gif')]), 15)
            self.assertEqual(len([name for name in transition_files if name.startswith('sfx/') and name.endswith('.wav')]), 16)
            self.assertIn('LICENSE', transition_files)
            self.assertIn('NOTICE', transition_files)
            self.assertFalse(any('/build/' in name or name.endswith(('.so', '.dll', '.exe')) for name in transition_files))
            with tarfile.open(audio_archive) as bundle:
                audio_files = [item.name for item in bundle.getmembers() if item.isfile()]
            color_archive = Path(folder)/'studio-color-source.tar.gz'
            with tarfile.open(color_archive) as bundle:
                color_files = [item.name for item in bundle.getmembers() if item.isfile()]
            self.assertIn('tests/core_tests.cpp', color_files)
            self.assertIn('tests/mlt_smoke.cpp', color_files)
            self.assertIn('generated/parameters.json', audio_files)
            self.assertIn('SOURCES.json', audio_files)
            self.assertIn('tools/studio_audio_cli.cpp', audio_files)
            self.assertFalse(any('/build/' in name or name.endswith(('.so', '.dll', '.exe')) for name in audio_files))
            with tarfile.open(subtitles_archive) as bundle:
                subtitle_files = [item.name for item in bundle.getmembers() if item.isfile()]
            self.assertIn('tools/studio_subtitle_cli.cpp', subtitle_files)
            self.assertIn('Audio/src/audio.cpp', subtitle_files)
            self.assertIn('SOURCES.json', subtitle_files)
            self.assertIn('licenses/SILERO_VAD_LICENSE.txt', subtitle_files)
            with tarfile.open(background_archive) as bundle:
                background_files = [item.name for item in bundle.getmembers() if item.isfile()]
            self.assertIn('kdenlive/studio_background.xml', background_files)
            self.assertIn('kdenlive/studio_background.json', background_files)
            self.assertIn('SOURCES.json', background_files)
            self.assertIn('LICENSE', background_files)
            self.assertIn('licenses/ONNX_RUNTIME_LICENSE.txt', background_files)
            self.assertIn('licenses/PADDLESEG_LICENSE.txt', background_files)
            self.assertEqual(len([name for name in background_files if name.startswith('previews/') and name.endswith('.png')]), 6)
            self.assertEqual(len([name for name in background_files if name.startswith('previews/') and name.endswith('.gif')]), 6)
            self.assertFalse(any('/build/' in name or name.endswith(('.so', '.dll', '.exe')) for name in background_files))
        self.assertEqual(studio['app-id'], 'local.VideoStudio.Kdenlive')
        self.assertEqual(studio['finish-args'], upstream['finish-args'])
        self.assertIn('--env=MLT_REPOSITORY=/app/lib/mlt-7', studio['finish-args'])
        self.assertIn('--env=MLT_DATA=/app/share/mlt-7', studio['finish-args'])
        names = [m['name'] for m in studio['modules']]
        kdenlive_index = names.index('kdenlive')
        self.assertEqual(names[kdenlive_index - 12:kdenlive_index], ['onnxruntime-cpu', 'card3d', 'studio-camera', 'studiofx', 'studio-transitions', 'studio-color', 'studio-audio', 'whisper-cpp', 'studio-subtitles', 'studio-background', 'studio-text', 'flatpak-lib-compat'])
        registry = json.loads((ROOT/'modules.json').read_text(encoding='utf-8'))['modules']
        self.assertEqual(len(registry), len(names[kdenlive_index - 12:kdenlive_index]) - 3)
        self.assertEqual(studio['modules'][kdenlive_index]['sources'][0], upstream['modules'][-1]['sources'][0])
        self.assertEqual(names[-2:], ['kdenlive', 'studio-extension-anchor'])
        test_commands = studio['modules'][kdenlive_index]['test-commands']
        self.assertIn('/app/share/kdenlive/transitions/slide.xml', test_commands[0])
        self.assertIn('/app/share/kdenlive/transitions/wipe.xml', test_commands[0])
        self.assertIn('/app/lib/libvidstab.so.1.2', test_commands[0])
        self.assertIn('melt -query transitions', test_commands[1])
        self.assertIn('test ! -s /tmp/studio-build-mlt-errors.txt', test_commands[1])
        self.assertNotIn('/app/share/studio-tests', '\\n'.join(test_commands))
        baseline = prepare.baseline_manifest()
        baseline_index = [m['name'] for m in baseline['modules']].index('kdenlive')
        baseline_opencv = next(m for m in baseline['modules'] if m['name'] == 'opencv')
        studio_opencv = next(m for m in studio['modules'] if m['name'] == 'opencv')
        self.assertIn('-DBUILD_LIST=tracking,dnn,videoio', studio_opencv['config-opts'])
        self.assertIn('-DWITH_FFMPEG=OFF', studio_opencv['config-opts'])
        self.assertIn('-DWITH_GSTREAMER=ON', studio_opencv['config-opts'])
        self.assertIn('-DCMAKE_INSTALL_LIBDIR=lib', studio_opencv['config-opts'])
        self.assertIn('-DBUILD_LIST=tracking,dnn', baseline_opencv['config-opts'])
        self.assertNotIn('-DWITH_FFMPEG=OFF', baseline_opencv['config-opts'])
        self.assertEqual(studio['modules'][kdenlive_index - 1], baseline['modules'][-1])
        self.assertIn('--libdir=lib', next(m for m in baseline['modules'] if m['name'] == 'inih')['config-opts'])
        self.assertIn('-DCMAKE_INSTALL_LIBDIR=lib', next(m for m in baseline['modules'] if m['name'] == 'oneapi-libvpl')['config-opts'])
        self.assertIn('-DCMAKE_INSTALL_LIBDIR=lib', next(m for m in baseline['modules'] if m['name'] == 'mlt')['config-opts'])
        self.assertIn('-DCMAKE_INSTALL_LIBDIR=lib', next(m for m in baseline['modules'] if m['name'] == 'frei0r-plugins')['config-opts'])
        self.assertIn('-DCMAKE_INSTALL_LIBDIR=lib', next(m for m in studio['modules'] if m['name'] == 'frei0r-plugins')['config-opts'])
        self.assertEqual(next(m for m in baseline['modules'] if m['name'] == 'ffmpeg')['build-options']['prepend-pkg-config-path'],
                         '/app/lib64/pkgconfig')
        self.assertEqual(next(m for m in baseline['modules'] if m['name'] == 'bigsh0t')['post-install'][0],
                         'mkdir -p /app/lib/frei0r-1')
        self.assertIn('libvidstab.so.1.2', baseline['modules'][-1]['build-commands'][0])
        camera = next(m for m in studio['modules'] if m['name'] == 'studio-camera')
        self.assertIn('-DCMAKE_INSTALL_LIBDIR=lib', camera['config-opts'])
        effects = next(m for m in studio['modules'] if m['name'] == 'studiofx')
        self.assertIn('-DFREI0R_INCLUDE_DIR=/app/include', effects['config-opts'])
        transitions = next(m for m in studio['modules'] if m['name'] == 'studio-transitions')
        self.assertIn('-DTRANSITIONS_SYSTEM_FREI0R=ON', transitions['config-opts'])
        color = next(m for m in studio['modules'] if m['name'] == 'studio-color')
        self.assertIn('-DSTUDIO_COLOR_MLT=ON', color['config-opts'])
        self.assertIn('-DSTUDIO_COLOR_DEVEL=OFF', color['config-opts'])
        audio = next(m for m in studio['modules'] if m['name'] == 'studio-audio')
        self.assertIn('-DBUILD_TESTING=OFF', audio['config-opts'])
        text = next(m for m in studio['modules'] if m['name'] == 'studio-text')
        self.assertIn('-DBUILD_TESTING=OFF', text['config-opts'])
        whisper = next(m for m in studio['modules'] if m['name'] == 'whisper-cpp')
        self.assertEqual(whisper['sources'][0]['commit'], '927cfce34f31707e17f2bff35c349632fb9e2c3a')
        subtitles = next(m for m in studio['modules'] if m['name'] == 'studio-subtitles')
        self.assertEqual(subtitles['sources'][1]['sha256'], 'ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb')
        self.assertEqual(subtitles['sources'][2]['sha256'], '29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf')
        self.assertEqual(subtitles['sources'][2]['url'],
                         'https://huggingface.co/ggml-org/whisper-vad/resolve/e5614ed76a5dd4b03fad5068c89efcd2617a9d1e/ggml-silero-v5.1.2.bin')
        self.assertIn('-DSTUDIO_SUBTITLE_VAD_MODEL_FILE=/run/build/studio-subtitles/ggml-silero-v5.1.2.bin', subtitles['config-opts'])
        onnx = next(m for m in studio['modules'] if m['name'] == 'onnxruntime-cpu')
        self.assertEqual(onnx['sources'][0]['commit'], 'e0b66cad282043d4377cea5269083f17771b6dfc')
        self.assertEqual(onnx['sources'][1]['type'], 'patch')
        self.assertEqual(onnx['sources'][2]['sha256'], '0a11e4f71593a1335cae38c4847bb9f36e1b700148c0d257abfca9df802115c2')
        self.assertEqual(onnx['sources'][3]['sha256'], '6a31b662deaeb0ac35e6287bda2f3369b19836e6c9f8828d4da444346f420298')
        self.assertEqual(onnx['build-options']['build-args'], ['--share=network'])
        self.assertIn('--use_preinstalled_eigen', onnx['build-commands'][0])
        self.assertIn('--compile_no_warning_as_error', onnx['build-commands'][0])
        self.assertIn('FETCHCONTENT_SOURCE_DIR_EIGEN=', onnx['build-commands'][0])
        self.assertIn('FETCHCONTENT_SOURCE_DIR_PROTOBUF=', onnx['build-commands'][0])
        background = next(m for m in studio['modules'] if m['name'] == 'studio-background')
        self.assertIn('-DSBG_WITH_MLT=ON', background['config-opts'])
        self.assertIn('-DSBG_WITH_ONNX=ON', background['config-opts'])
        self.assertEqual(background['sources'][1]['sha256'], '2047b95bea344af15575f4279d6fac44bb07a42e7d537cf8d3265f15fe95cc82')


if __name__ == '__main__': unittest.main(verbosity=2)
