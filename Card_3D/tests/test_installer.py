#!/usr/bin/env python3
"""Exercises file operations in a temporary home with a mocked Flatpak.
These tests are not an actual Kdenlive/Flatpak runtime compatibility test.
"""
import importlib.util
from pathlib import Path
import tempfile
from unittest.mock import patch
import unittest
import contextlib
import io
import os
import shutil

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('setup',ROOT/'scripts/flatpak_setup.py')
s=importlib.util.module_from_spec(spec); spec.loader.exec_module(s)

class FakeFlatpak:
    def __init__(self,data,env='',loader=True):
        self.data=data;self.env=env;self.loader=loader;self.overrides=[];self.running=False
    def __call__(self,*args,check=True):
        if args[0]=='info': return 'Kdenlive mock runtime\n'
        if args[0]=='ps': return s.APP if self.running else ''
        if args[0]=='override':
            arg=args[2];self.overrides.append(arg)
            self.env=arg.split('=',2)[2] if arg.startswith('--env=') else ''
            return ''
        if args[0]=='run':
            cmd=args[4]
            if 'CARD3D_DATA' in cmd:
                return f'CARD3D_DATA={self.data}\nCARD3D_FREI0R={self.env}\nCARD3D_MLT=\n'
            if 'CARD3D_ENGINE' in cmd:
                if self.loader is None: return 'CARD3D_ENGINE_UNAVAILABLE\n'
                return 'CARD3D_ENGINE_AVAILABLE\n'+('identifier: frei0r.card3d\n' if self.loader else 'Unknown filter\n')
            if cmd.startswith('test -r'):
                if not all(Path(x).is_file() for x in args[-2:]): raise s.SetupError('missing file')
                return ''
        raise AssertionError(args)

class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.home=Path(self.tmp.name)/'home with spaces';self.home.mkdir()
        self.data=self.home/'.var/app/org.kde.kdenlive/data';self.data.mkdir(parents=True)
        self.state=self.home/'.local/state'/s.NAME
        self.fake=FakeFlatpak(self.data)
        # Installer logic uses a deliberately inert ELF header; never substitute
        # this fixture for a loadable release binary or a real MLT test.
        self.payload=Path(self.tmp.name)/'payload'
        (self.payload/'bin/linux-x86_64').mkdir(parents=True)
        (self.payload/'kdenlive').mkdir()
        header=bytearray(64);header[:6]=b'\x7fELF\x02\x01';header[16:20]=b'\x03\x00\x3e\x00'
        (self.payload/'bin/linux-x86_64'/s.BIN_NAME).write_bytes(header)
        shutil.copyfile(ROOT/'kdenlive'/s.XML_NAME,self.payload/'kdenlive'/s.XML_NAME)
        for name,value in [('ROOT',self.payload),('HOME',self.home),('STATE',self.state),('MANIFEST',self.state/'installation.json'),('fp',self.fake)]:
            p=patch.object(s,name,value);p.start();self.addCleanup(p.stop)
        for target,value in [('os.geteuid',1000),('platform.system','Linux'),('platform.machine','x86_64'),('shutil.which','/usr/bin/flatpak')]:
            p=patch(target,return_value=value,create=target=='os.geteuid');p.start();self.addCleanup(p.stop)
        self.output=io.StringIO();c=contextlib.redirect_stdout(self.output);c.__enter__();self.addCleanup(c.__exit__,None,None,None)
    @unittest.skipIf(os.name=='nt','Requires real POSIX paths; run in WSL')
    def test_existing_camera_plugin_path_untouched(self):
        folder=self.home/'existing effects';folder.mkdir()
        camera=folder/'camerashakeorganic.so';camera.write_bytes(b'existing-camera')
        self.fake.env=f'{folder}:/app/lib/frei0r-1'
        before=self.fake.env
        s.install(); self.assertTrue((folder/s.BIN_NAME).is_file())
        self.assertEqual(self.fake.env,before); self.assertEqual(self.fake.overrides,[])
        s.uninstall();self.assertEqual(camera.read_bytes(),b'existing-camera')
        self.assertEqual(self.fake.env,before);self.assertFalse((folder/s.BIN_NAME).exists())
    def test_v1_managed_directory_is_not_reused(self):
        folder=self.data/'sunimo_organic_perspective/plugins';folder.mkdir(parents=True)
        older=folder/'sunimo_organic_perspective.so';older.write_bytes(b'v1-remains')
        self.fake.env=str(folder)+':/app/lib/frei0r-1'
        before=self.fake.env
        s.install();state=s.read_manifest()
        self.assertNotEqual(Path(state['files'][0]['target']).parent,folder)
        self.assertIn(str(folder),self.fake.env)
        self.assertEqual(older.read_bytes(),b'v1-remains')
        s.uninstall();self.assertEqual(self.fake.env,before)
        self.assertEqual(older.read_bytes(),b'v1-remains')
    def test_new_path_preserves_builtin_plugins_and_uninstalls(self):
        s.install();state=s.read_manifest();self.assertTrue(state['active'])
        self.assertIn('/app/lib/frei0r-1',self.fake.env)
        s.uninstall();self.assertEqual(self.fake.env,'');self.assertFalse(s.read_manifest()['active'])
        for entry in state['files']: self.assertFalse(Path(entry['target']).exists())
    @unittest.skipIf(os.name=='nt','Requires real POSIX paths; run in WSL')
    def test_later_user_environment_additions_preserved(self):
        self.fake.env='/app/lib/frei0r-1'
        s.install();self.fake.env+=':/home/extra-plugins'
        s.uninstall();self.assertEqual(self.fake.env,'/app/lib/frei0r-1:/home/extra-plugins')
    def test_failed_loader_rolls_back_previous_xml(self):
        xml=self.data/'kdenlive/effects'/s.XML_NAME;xml.parent.mkdir(parents=True);xml.write_bytes(b'previous-ui')
        self.fake.loader=False
        with self.assertRaises(s.SetupError): s.install()
        self.assertEqual(xml.read_bytes(),b'previous-ui');self.assertEqual(self.fake.env,'')
        self.assertFalse(s.read_manifest()['active'])
    def test_reinstall_is_idempotent(self):
        s.install();state=s.read_manifest();calls=len(self.fake.overrides)
        s.install();self.assertEqual(state,s.read_manifest());self.assertEqual(calls,len(self.fake.overrides))
    def test_modified_file_is_not_deleted_and_other_file_is_left(self):
        s.install();state=s.read_manifest();binary=Path(state['files'][0]['target']);xml=Path(state['files'][1]['target'])
        binary.write_bytes(b'user-modified')
        s.uninstall();self.assertEqual(binary.read_bytes(),b'user-modified');self.assertTrue(xml.is_file())
        self.assertTrue(s.read_manifest()['active'])
    def test_open_editor_prevents_installation(self):
        self.fake.running=True
        with self.assertRaises(s.SetupError):s.install()
        self.assertFalse(s.MANIFEST.exists())
    def test_melt_unavailable_is_reported_not_falsely_confirmed(self):
        self.fake.loader=None;s.install()
        self.assertTrue(s.read_manifest()['active'])
        self.assertIn('нет melt',self.output.getvalue())

    def test_windows_binary_is_rejected_before_installing(self):
        (self.payload/'bin/linux-x86_64'/s.BIN_NAME).write_bytes(b'MZ'+bytes(100))
        with self.assertRaises(s.SetupError):s.install()
        self.assertFalse(s.MANIFEST.exists());self.assertFalse(self.fake.overrides)

    def test_missing_release_is_rejected(self):
        (self.payload/'bin/linux-x86_64'/s.BIN_NAME).unlink()
        with self.assertRaises(s.SetupError):s.install()
        self.assertFalse(s.MANIFEST.exists())

if __name__=='__main__':unittest.main(verbosity=2)
