from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'mxr_malloc/mxr_malloc.c').read_text()
def extract(name):

 # find definition, skip forward declaration if any
 import re
 m=re.search(r'static\s+(?:inline\s+void|bool|uint8_t)\s+(?:MXR_IRAM_ATTR\s+)?'+name+r'\([^;]*?\)\s*\{',s)
 start=m.start(); i=m.end(); depth=1
 while depth:
  if s[i]=='{':depth+=1
  if s[i]=='}':depth-=1
  i+=1
 return s[start:i]
prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#define MXR_IRAM_ATTR
#define MXR_DESC_BINARY_SEARCH_ACTIVE 0
#define MXR_EARLY_EXIT_ACTIVE 1
#define MXR_BEST_FIT_WASTE_SHIFT 3
#define MXR_ALIGN_SIZE 4
#define MXR_REGION_MAX_UNLIMITED 0
#define MXR_IS_SLIVER(w) ((w)>0 && (w)<8)
#define CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES 100
#define ESP_EARLY_LOGE(...) ((void)0)
typedef uint32_t mxr_class_t;
typedef struct {uint32_t min_bytes,max_bytes; uint8_t percent;} mxr_region_cfg_t;
typedef struct {uint32_t start_byte,total_bytes,max_bytes,free_bytes;} region;
typedef struct {uint32_t off,len;} desc;
static region s_region[1],s_iram_fb_region[1];
static desc s_dram_desc[4],s_iram_desc[4];
static uint16_t s_dram_desc_count,s_iram_desc_count;
static uint8_t s_iram_fb_region_count=1;
static struct {unsigned anti_sliver_expansions,best_fit_early_exits;} s_stats;
static uint32_t mxr_desc_off(const desc*d){return d->off;}
static uint32_t mxr_desc_len(const desc*d){return d->len;}
static uint32_t mxr_iram_fb_region_end(int r){return s_iram_fb_region[r].start_byte+s_iram_fb_region[r].total_bytes;}
static bool mxr_bgg_relaxed(uint32_t a,uint32_t b,uint32_t c){return false;}
'''
body='\n'.join(extract(n) for n in ['mxr_memset4','mxr_memcpy4','mxr_memmove4','mxr_iram_fb_find_free_and_largest','mxr_iram_fb_find_free_in_region','mxr_find_best_free','mxr_find_free_and_largest','mxr_parse_region_config'])
test=r'''
int main(void){
 uint32_t words[8]={1,2,3,4,5,6,7,8}, copy[8];
 mxr_memcpy4(copy,words,sizeof(words));
 for(unsigned i=0;i<8;++i) assert(copy[i]==i+1);
 mxr_memmove4(words+1,words,7*4);
 for(unsigned i=1;i<8;++i) assert(words[i]==i);
 mxr_memmove4(words,words+1,7*4);
 for(unsigned i=0;i<7;++i) assert(words[i]==i+1);
 mxr_memset4(words,sizeof(words));
 for(unsigned i=0;i<8;++i) assert(words[i]==0);
 puts("word copy/move/clear PASS");
 uint32_t off=0,largest=0,alloc=0; bool exact=false;
 s_iram_fb_region[0]=(region){0,104,0,104};
 assert(mxr_iram_fb_find_free_and_largest(0,100,&off,&largest,&alloc));
 printf("#9: request=100 limit=100 gap=104 allocated=%u\n",alloc); assert(alloc==100);
 assert(mxr_iram_fb_find_free_in_region(0,100,&off,&alloc,&largest,&exact)); assert(alloc==100);
 s_region[0]=(region){0,316,0,208};
 s_dram_desc_count=2; s_dram_desc[0]=(desc){108,4};s_dram_desc[1]=(desc){212,104};
 assert(mxr_find_free_and_largest(0,100,&off,&largest,&alloc,&exact));
 printf("#16: acceptable first gap=108, selected offset=%u gap=%u early_exits=%u\n",off,alloc,s_stats.best_fit_early_exits); assert(off==0 && !exact);
 /* Complete failed IRAM scan must report exact largest without another scan. */
 s_iram_fb_region[0]=(region){0,200,0,196};
 s_iram_desc_count=1; s_iram_desc[0]=(desc){96,4};
 assert(!mxr_iram_fb_find_free_in_region(0,104,&off,&alloc,&largest,&exact));
 assert(exact && largest==100);
 /* Interior anti-sliver and region cap, in both search implementations. */
 s_iram_fb_region[0]=(region){0,108,100,104};
 s_iram_desc[0]=(desc){104,4};
 assert(mxr_iram_fb_find_free_in_region(0,100,&off,&alloc,&largest,&exact));
 assert(alloc==100);
 assert(mxr_iram_fb_find_free_and_largest(0,100,&off,&largest,&alloc));
 assert(alloc==100);
 mxr_region_cfg_t cfg[1] = {0};
 unsigned n=mxr_parse_region_config("4294967300-50%",cfg,1);
 printf("#17: parsed=%u min=%u percent=%u\n",n,cfg[0].min_bytes,cfg[0].percent); assert(n==0);
 assert(mxr_parse_region_config("2147483648-50%",cfg,1)==0);
 assert(mxr_parse_region_config("4-50%",cfg,1)==1 && cfg[0].min_bytes==4);
 puts("regressions PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'isolated.c'; p.write_text(prefix+body+test)
 subprocess.run(['cc','-std=c99','-O2',str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
