#include <stdio.h>
#include <string.h>
#include <time.h>

/* 0xB4 = 0b10110100  - channel 2 - access lohi - mode 2 - binary */
/* in  al, 0x43 to introduce a delay before the useful IO access  */
/*                                                                */
/* To 42h:                                                        */
/* ======                                                         */
/* load FFFF as divisor                                           */
/*                                                                */
/* To 61h:                                                        */
/* ======                                                         */                                     
/* and al, 0xFD  shut Speaker                                     */
/* or  al, 0x1   activate counter (enable timer)                  */
void counter_start();
#pragma aux counter_start =   \
"pushf"                         \
"cli"                           \
"mov al, 0xB4"                  \
"out 0x43, al"                  \
"mov al, 0xFF"                  \ 
"out 0x42, al"                  \
"out 0x42, al"                  \
"in  al, 0x61"                  \
"and al, 0xFD"                  \
"or  al, 0x1"                   \
"out 0x61, al"                  \
"popf"                          \
modify [eax]

/* xchg al, ah - x86 is little endian */
int counter_read();
#pragma aux counter_read =   \
"pushf"                         \
"cli"                           \
"xor eax, eax"                  \
"in al,  0x42"                  \
"mov ah,  al"                   \
"in al,  0x42"                  \
"xchg al, ah"                   \
"popf"                          \
modify [eax]                    \
value  [eax]                    


typedef struct  {    
    unsigned short int frameNr;
    unsigned short int time; 
    unsigned char      id;
} benchElem_t;

#define BENCH_NB_ELEMS 250000
static benchElem_t  BenchStorage[BENCH_NB_ELEMS];
static benchElem_t* Current;
static clock_t TimeRef;
static unsigned long int FrameNr = 0;
void B_Init()
{
    memset(BenchStorage, 0, BENCH_NB_ELEMS * sizeof(benchElem_t));
    Current = &BenchStorage[0];
    TimeRef = clock();
}

void B_BenchStart()
{
    counter_start();
}


void B_BenchEnd(const unsigned char id)
{
    Current->frameNr = FrameNr;
    Current->time = 0xFFFFF - counter_read();
    Current->id = id;
    if(Current++ == &BenchStorage[BENCH_NB_ELEMS]) {
        Current = &BenchStorage[0];
    }
}


void B_Flush()
{
    int i;
    benchElem_t* pElem = &BenchStorage[0];
    FILE* pFile;
    pFile = fopen ("bench.csv","w");
    if (pFile)
    {
        fprintf(pFile, "TIMESTAMP;BENCH;ID\n");
        for(i = 0; i < BENCH_NB_ELEMS; i++)
        {
            fprintf(pFile, "%u;%u;%u\n", pElem->frameNr, pElem->time, pElem->id);
            pElem++;
        }
        fclose(pFile);
    }
}

void B_NextFrame()
{
    FrameNr++;
}

