#!/usr/bin/env bash
set -euo pipefail
out=${1:?Usage: check_official_header.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
blob=b20487e9d34de76c7665b59378940317f63db2b4
curl --fail --location --retry 2 --connect-timeout 20 --max-time 90 \
  "https://api.github.com/repos/dyne/frei0r/git/blobs/$blob" -o "$out/frei0r.blob.json"
python3 - "$out" "$blob" <<'PY'
import base64,hashlib,json,pathlib,sys
out=pathlib.Path(sys.argv[1]);expected=sys.argv[2]
payload=json.loads((out/'frei0r.blob.json').read_text(encoding='utf-8'))
if payload.get('encoding')!='base64': raise SystemExit('Unexpected GitHub blob encoding')
data=base64.b64decode(payload['content'])
sha=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
if sha!=expected: raise SystemExit('Upstream header hash mismatch')
(out/'frei0r.h').write_bytes(data)
(out/'provenance.json').write_text(json.dumps({'git_blob':sha,'sha256':hashlib.sha256(data).hexdigest(),'unmodified':True},indent=2)+'\n')
print('Verified untouched upstream header:',sha)
PY
