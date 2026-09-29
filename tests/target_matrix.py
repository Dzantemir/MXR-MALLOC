"""Usage: python3 tests/target_matrix.py /path/to/configured/validation-build"""
import json, pathlib, shlex, subprocess, sys, tempfile, re
b=pathlib.Path(sys.argv[1]).resolve()
commands=json.loads((b/'compile_commands.json').read_text())
entry=next(x for x in commands if x['file'].endswith('/mxr_malloc/mxr_malloc.c'))
base=shlex.split(entry['command']); root=pathlib.Path(entry['file']).parent
config=(b/'config/sdkconfig.h').read_text()
variants={
 'static-minimal': {},
 'core-only': {'MXR_IRAM_PATH_ALLOC_FAMILY':None,'MXR_IRAM_PATH_CORE':1},
 'no-hot-path': {'MXR_IRAM_PATH_ALLOC_FAMILY':None,'MXR_IRAM_HOT_PATH_DISABLED':1},
 'iram-only-dynamic': {'MXR_IRAM_DESC_DYNAMIC':1,'MXR_DESC_INIT':200,'MXR_DESC_CHUNK':16},
 'dram-only-dynamic': {'MXR_DESC_DYNAMIC':1,'MXR_STATE_IN_IRAM':None,'MXR_DESC_IN_IRAM_BSS':None,'MXR_DESC_IN_DRAM':1,'MXR_DESC_INIT':200,'MXR_DESC_CHUNK':16},
 'both-dynamic-full': {'MXR_DESC_DYNAMIC':1,'MXR_STATE_IN_IRAM':None,'MXR_DESC_IN_IRAM_BSS':None,'MXR_DESC_IN_DRAM':1,'MXR_IRAM_DESC_DYNAMIC':1,'MXR_DUMP_MINIMAL':None,'MXR_DUMP_FULL':1},
 'normal': {'MXR_DUMP_MINIMAL':None,'MXR_DUMP_NORMAL':1},
 'dram-only': {'MXR_USE_IRAM':None,'MXR_IRAM_FALLBACK_ENABLED':None,'MXR_STATE_IN_IRAM':None,'MXR_DESC_IN_IRAM_BSS':None,'MXR_DESC_IN_DRAM':1},
}
with tempfile.TemporaryDirectory() as d:
 for name,changes in variants.items():
  text=config
  for k,v in changes.items():
   text=re.sub(r'^#define CONFIG_'+k+r'\b.*\n','',text,flags=re.M)
   if v is not None:text+='\n#define CONFIG_'+k+' '+str(v)+'\n'
  pathlib.Path(d,'sdkconfig.h').write_text(text)
  for src in ['mxr_malloc.c','mxr_heap_wrap.c','mxr_heap_port.c','mxr_heap_compat.c']:
   args=[];skip=False
   for a in base:
    if skip:skip=False;continue
    if a=='-o':skip=True;continue
    if a==entry['file']:continue
    args.append(a)
   args.insert(1,'-I'+d)
   args+=['-O2','-Werror=implicit-function-declaration',str(root/src),'-o',d+'/'+src+'.o']
   subprocess.run(args,cwd=entry['directory'],check=True,stdout=subprocess.PIPE)
  if name in ('iram-only-dynamic','dram-only-dynamic'):
   check=pathlib.Path(d,'init-check.c')
   macro='MXR_IRAM_DESC_INIT' if name=='iram-only-dynamic' else 'MXR_DRAM_DESC_INIT'
   expected=128 if name=='iram-only-dynamic' else 200
   check.write_text('#include "mxr_malloc.h"\n_Static_assert('+macro+'=='+str(expected)+', "independent init");\n')
   subprocess.run([base[0],'-std=gnu99','-I'+d,'-I'+str(root/'include'),'-fsyntax-only',str(check)],check=True)
  print(name+': target compile PASS')
