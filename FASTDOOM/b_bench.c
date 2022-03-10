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

void B_CounterStart()
{
    counter_start();
}


int B_CounterRead()
{
    return counter_read();
}


