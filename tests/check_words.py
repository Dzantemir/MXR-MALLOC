"""Compile actual word helpers, force out-of-line at -O2, check target opcodes.
Usage: python tests/check_words.py [xtensa-lx106-elf- tool prefix]
"""
import pathlib,re,subprocess,tempfile,sys
prefix=sys.argv[1] if len(sys.argv)>1 else 'xtensa-lx106-elf-'
s=(pathlib.Path(__file__).resolve().parents[1]/'mxr_malloc/mxr_malloc.c').read_text()
body='#include <stdint.h>\n#include <stddef.h>\n#define MXR_IRAM_ATTR __attribute__((section(".iram1.text"),noinline,used))\n'
for name in ['mxr_memcpy4','mxr_memmove4','mxr_memset4']:
 m=re.search(r'static inline void MXR_IRAM_ATTR '+name+r'\([^;]+?\)\s*\{',s)
 i=m.end();depth=1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 body+=s[m.start():i].replace('static inline void','static void')+'\n'
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d)/'helpers.c';p.write_text(body)
 subprocess.run([prefix+'gcc','-O2','-mlongcalls','-c',str(p),'-o',d+'/helpers.o'],check=True)
 dis=subprocess.check_output([prefix+'objdump','-dr',d+'/helpers.o'],text=True)
 assert not re.search(r'\b(?:l8ui|l16ui|l16si|s8i|s16i|call0|callx0)\b',dis),dis
 assert 'l32i' in dis and 's32i' in dis
 print(dis)
 print('O2 forced out-of-line IRAM word helpers: no byte/halfword accesses or calls; PASS')
