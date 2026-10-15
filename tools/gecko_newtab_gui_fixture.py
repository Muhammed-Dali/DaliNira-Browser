#!/usr/bin/env python3
"""Only launched after explicit desktop authorization through safe-gui-test.sh."""
import json,os,pathlib,resource,sys
if os.environ.get('DALINIRA_GUI_TESTS')!='1':
 print('SKIP: real desktop authorization and safe-gui-test.sh required');sys.exit(77)
root=pathlib.Path(os.environ.get('DALINIRA_GECKO_GUI_ROOT',''))
if not root.is_absolute() or not root.is_dir() or root.is_symlink() or root.stat().st_uid!=os.getuid() or root.stat().st_mode&0o077:
 raise SystemExit('private authorized GUI root required')
private=pathlib.Path(os.environ.get('DALINIRA_GECKO_GUI_PRIVATE_ROOT',''))
if not private.is_absolute() or not private.is_dir() or private.is_symlink() or private.stat().st_uid!=os.getuid() or private.stat().st_mode&0o077:
 raise SystemExit('private short GUI root required')
(root/'isolation.json').write_text(json.dumps({'privateRoot':str(private),'coreLimit':0},indent=2))
for name in ['home','config','cache','data','state','tmp','runtime']:
 (private/name).mkdir(mode=0o700,exist_ok=True)
for key,name in {'HOME':'home','XDG_CONFIG_HOME':'config','XDG_CACHE_HOME':'cache','XDG_DATA_HOME':'data','XDG_STATE_HOME':'state','TMPDIR':'tmp','XDG_RUNTIME_DIR':'runtime'}.items():os.environ[key]=str(private/name)
os.environ['DBUS_SESSION_BUS_ADDRESS']='unix:path='+str(private/'no-bus')
os.environ['NO_AT_BRIDGE']='1'
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
os.execv(sys.argv[1],[sys.argv[1],'--newtab-x11'])
