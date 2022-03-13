#ifndef _BENCH_H_
#define _BENCH_H_

void B_Init();
void B_BenchStart();
void B_BenchEnd(const unsigned char id);
void B_Flush();
void B_NextFrame();


#define INITGFX 0
#define UPDATESOUND 1
#define RUNTICK 2
#define MAP2D 3
#define STATUSBAR2D 4
#define MENU2D 5
#define R_SETUPFRAME 6
#define R_CLEARBUFFERS 7
#define R_BSPNODE_FRONT 8
#define R_BSPNODE_SUBSECTOR 9
#define R_BSPNODE_BACK 10
#define R_DRAWPLANES 11
#define R_DRAWMASKED 12
#define UPDATENOBLIT 13
#define FINISHUPDATE 14

#endif
