#!/usr/bin/env python3
"""Package the host's own code for sandboxed children; never touches the GRE."""
import base64,hashlib,json,pathlib,shutil,sys,zipfile
source=pathlib.Path(sys.argv[1]).resolve();target=pathlib.Path(sys.argv[2]).resolve();target.mkdir(parents=True,exist_ok=True)
for name in ['application.ini','distribution','defaults']:
 p=source/name
 if p.is_dir():shutil.copytree(p,target/name,dirs_exist_ok=True)
 else:shutil.copy2(p,target/name)
script=(source/'chrome/content/newtab-presentation.js').read_bytes()
hash_value=base64.b64encode(hashlib.sha256(script).digest()).decode()
policy="default-src 'none'; script-src 'sha256-"+hash_value+"'; style-src 'unsafe-inline'; img-src data: blob:; connect-src 'none'; frame-src 'none'; object-src 'none'; worker-src 'none'; base-uri 'none'; form-action 'none'"
document='<!doctype html><meta charset="utf-8"><meta name="referrer" content="no-referrer"><meta http-equiv="Content-Security-Policy" content="'+policy+'"><title>New Tab</title><script>'+script.decode()+'</script>'
with zipfile.ZipFile(target/'omni.ja','w',compression=zipfile.ZIP_STORED) as archive:
 for p in sorted(source.rglob('*')):
  relative=p.relative_to(source)
  if p.is_file() and relative.parts[0] not in ['application.ini','distribution','defaults']:
   archive.write(p,relative)
 archive.writestr('chrome/content/NewTabDocument.sys.mjs','export const NEW_TAB_DOCUMENT='+json.dumps(document,ensure_ascii=True)+';\n')
