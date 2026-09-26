#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <unicorn/unicorn.h>
static uc_engine *uc;
static void *kthread(void*x){ for(;;){ usleep(1000); uc_emu_stop(uc);} return 0;}
int main(){
 uc_open(UC_ARCH_X86,UC_MODE_64,&uc); uc_x86_set_system_mode(uc,1); uc_ctl_exits_enable(uc);
 size_t sz=64<<20; unsigned char*ram=calloc(1,sz); uc_mem_map_ptr(uc,0,sz,UC_PROT_ALL,ram);
 FILE*f=fopen("work.bin","rb"); if(fread(ram+0x100000,1,1<<20,f)){}; fclose(f);
 uint64_t rsp=0x3f00000, pc=0x100040; uc_reg_write(uc,UC_X86_REG_RSP,&rsp);
 pthread_t t; pthread_create(&t,0,kthread,0);
 unsigned *arr=(unsigned*)(ram+0x101000); long stops=0, incons=0;
 for(;;){ uc_err e=uc_emu_start(uc,pc,0,0,0); pc=uc_x86_get_pc64(uc); if(e||uc_x86_is_halted(uc)) break; stops++;
   uint64_t s[23]; uc_x86_get_state64(uc,s);
   if (pc==0x100018) { long idx=(s[0]-0x101000)/4; if (idx>0 && idx<(1<<20) && arr[idx]!=arr[idx-1]-1+0 && arr[idx]==arr[idx-1]) { if (incons<6) printf("incons: pc %llx rax %llx a[idx-1]=%u a[idx]=%u a[idx+1]=%u\n",(unsigned long long)pc,(unsigned long long)s[0],arr[idx-1],arr[idx],arr[idx+1]); incons++; } }
   else if (stops < 20) printf("stop at pc %llx\n",(unsigned long long)pc);
 }
 long bad=0; for(long i=0;i<(1<<20);i++) if(arr[i]!=20) bad++;
 printf("stops %ld incons %ld bad %ld\n",stops,incons,bad);
}
