from pathlib import Path
import json,subprocess,sys,unittest,xml.etree.ElementTree as E
ROOT=Path(__file__).resolve().parents[1]
class Contract(unittest.TestCase):
 def test_schema_and_xml(self):
  schema=json.loads((ROOT/'kdenlive/studio_camera.json').read_text(encoding='utf-8'));self.assertEqual(schema['service'],'studio.camera');self.assertEqual(schema['version'],2);self.assertEqual([g['label'] for g in schema['groups']],['Движение камеры','Кадрирование','Время','Живая камера','Трекинг головы']);self.assertEqual(len(schema['controls']),13)
  root=E.parse(ROOT/'kdenlive/studio_camera.xml').getroot();self.assertEqual(root.attrib['tag'],'studio.camera');self.assertEqual(root.attrib['requires_in_out'],'1');params={p.attrib['name']:p.attrib for p in root.findall('parameter')}
  self.assertEqual(params['zoom']['default'],'120');self.assertEqual(params['zoom']['factor'],'1');self.assertEqual(params['duration']['default'],'0.65');self.assertNotIn('offset',params['duration']);self.assertEqual(params['cycle_period']['default'],'6');self.assertEqual(params['mode']['paramlist'],'0;1;2;3;4;5');self.assertEqual(params['track_path']['type'],'url');self.assertEqual(set(params),{c['key'] for c in schema['controls']}|{'studio_time_origin','studio_time_span','variant'})
 def test_generated_files_current(self):subprocess.run([sys.executable,str(ROOT/'scripts/generate_ui.py'),'--check'],check=True)
if __name__=='__main__':unittest.main(verbosity=2)
