typedef unsigned long u64; typedef unsigned int u32;
#define N (1<<20)
u32 a[N] __attribute__((aligned(4096)));
u64 work(void) {
  u64 h = 5; int i, r;
  for (r = 0; r < 20; r++)
    for (i = 0; i < N; i++) { a[i] += 1; }
  return h;
}
void _start(void) { u64 h = work(); __asm__ volatile("mov %0, %%rax; hlt" :: "r"(h)); for(;;); }
