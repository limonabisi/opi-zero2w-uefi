#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>
static void blk(uc_engine*uc,uint64_t a,uint32_t sz,void*u){ printf("blk %llx size %u\n",(unsigned long long)a,sz); }
int main(){
 uc_engine*uc; uc_open(UC_ARCH_X86,UC_MODE_32,&uc); uc_x86_set_system_mode(uc,1); uc_ctl_exits_enable(uc);
 size_t sz=8<<20; unsigned char*ram=calloc(1,sz); uc_mem_map_ptr(uc,0,sz,UC_PROT_ALL,ram);
 uint64_t *pml4=(uint64_t*)(ram+0x1000),*pdpt0=(uint64_t*)(ram+0x2000),*pd0=(uint64_t*)(ram+0x3000),*pdpt1=(uint64_t*)(ram+0x4000),*pd1=(uint64_t*)(ram+0x5000);
 pml4[0]=0x2003; pml4[511]=0x4003; pdpt0[0]=0x3003; pd0[0]=0x83; pd0[1]=0x200083; pdpt1[510]=0x5003; pd1[0]=0x83|0x100; pd1[1]=0x200083;
 uint64_t gdt[3]={0,0x00af9a000000ffffULL,0x00cf92000000ffffULL}; memcpy(ram+0x500,gdt,sizeof gdt);
 FILE*f=fopen("hi.bin","rb"); fread(ram+0x10000,1,4096,f); fclose(f); f=fopen("hi2.bin","rb"); fread(ram+0x20000,1,4096,f); fclose(f);
 uc_x86_mmr g={0}; g.base=0x500; g.limit=23; uc_reg_write(uc,UC_X86_REG_GDTR,&g);
 uc_hook h; uc_hook_add(uc,&h,UC_HOOK_BLOCK,blk,NULL,1,0);
 uc_err e=uc_emu_start(uc,0x10000,0,0,0);
 uint64_t s[23]; uc_x86_get_state64(uc,s);
 printf("err %d %s rip %llx rbx %llx cr4 %llx halted %d\n",e,uc_strerror(e),(unsigned long long)s[16],(unsigned long long)s[3],(unsigned long long)s[21],uc_x86_is_halted(uc));
}
