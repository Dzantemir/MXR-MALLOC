"""Usage: python check_elf.py firmware.elf [xtensa-lx106-elf-objdump]. Requires pyelftools.
Checks placement, and resolved direct/literal-loaded calls reachable from allocator roots.
Not a proof of all data references or hardware cache-off operation.
"""
import re,sys,subprocess,struct
from elftools.elf.elffile import ELFFile
with open(sys.argv[1],'rb') as f:
 e=ELFFile(f); sections=[(s['sh_addr'],s.data()) for s in e.iter_sections() if s['sh_type']!='SHT_NOBITS']
 symbols={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
def word(a):
 for start,data in sections:
  if start<=a and a+4<=start+len(data):return struct.unpack_from('<I',data,a-start)[0]
 return None
lo,hi=symbols['_iram_bss_start'],symbols['_iram_bss_end']
for n in ['s_stats','s_region','s_iram_fb_region','s_dram_desc','s_iram_desc']:
 assert lo<=symbols[n]<hi<=symbols['_iram_end'],n
 print(n,hex(symbols[n]),'inside startup-zeroed IRAM BSS')
text=subprocess.check_output([sys.argv[2] if len(sys.argv)>2 else 'xtensa-lx106-elf-objdump','-d',sys.argv[1]],text=True)
funcs={}; current=None
for l in text.splitlines():
 m=re.match(r'([0-9a-f]+) <([^>]+)>:',l)
 if m:current=int(m[1],16);funcs[current]=[m[2],[],[],[]];regs={};continue
 if current is None:continue
 m=re.search(r'\bl32r\s+(a\d+),\s*([0-9a-f]+)',l)
 if m:regs[m[1]]=word(int(m[2],16))
 m=re.search(r'\bcall0\s+([0-9a-f]+)',l)
 if m:funcs[current][1].append(int(m[1],16));regs={}
 m=re.search(r'\bcallx0\s+(a\d+)',l)
 if m:
  target=regs.get(m[1]);regs={}
  if target is None:funcs[current][2].append(l)
  else:funcs[current][1].append(target)
 m=re.search(r'\bl32r\s+a\d+,\s*([0-9a-f]+)',l)
 if m and 0x40200000<=int(m[1],16)<0x40300000:funcs[current][3].append(l)
roots=['mxr_malloc_caps','mxr_free','mxr_realloc_caps','mxr_calloc_caps','mxr_zalloc_caps']
seen=set();todo=[symbols[r] for r in roots];bad=[];unknown=[]
while todo:
 a=todo.pop()
 if a in seen:continue
 seen.add(a)
 if not 0x40000000<=a<0x40200000:bad.append(hex(a));continue
 if a not in funcs:continue # ROM
 # SDK's invalid-unlock panic prints through flash; not a normal allocator path.
 if funcs[a][0]=='ets_printf':
  print('Boundary: SDK ets_printf panic path excluded (unbalanced critical nesting).');continue
 name,calls,unresolved,literals=funcs[a]
 unknown+=unresolved;bad+=literals;todo+=calls
assert not bad, bad
assert not unknown, unknown
print('Resolved normal allocator call closure:',len(seen),'functions, no flash code/literal-pool fetches; PASS')
print('Pointer-derived data accesses and asynchronous interrupt/NMI paths still require hardware validation.')
