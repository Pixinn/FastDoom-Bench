# FBENCH

FBench is a hack of FastDoom for DOS, in order to benchmark it. Timings of the benchmarked functions are written to *BENCH.CVS* and can be exploited by *plot.py* which will display a stacked-bar graph.

The benchmark itself does not consume much time but requires around 1.2 MiB of memory.
## How to build

Build tested with Watcom 1.9.

Run :
```bash
make clean bench
```
It will produced the executable *fbench.exe*.
If the executable was produced on another OS with cross-compilation it ended with two errors: embedding the DPMI server can only be done under a DOS. Go then to DOS or DosBox and run
```
sb -r fbench.exe
ss fbench.exe dos32a.d32
```

## How to bench

Run *Fbench.exe* in a folder where doom1.wad is available (the shareware data of doom). It will produce a *bench.csv* file containing the result off the bench.  
The wile is written to disk when the game exits. It can take a long time.

To bench using one of the standard recorded demo:

```bash
fbench -timedemo demo2
```

In order to exploit and draw it, use the provided *plot.pyµ Python3 script.

## About the code

### Benched functions

* DrawMasked, masked element : sprites, partialy transparent walls, player's weapon.
* DrawFlat, flat visplanes : empty vertical space representing flors, ceils, sky...

### ModeY vs Mode13h

* MODEY  
  Rendering is done in RAM then VRAM is updated by small blocks
  * I_UpdateNoBlit() writes in VRAM after every major write sequence
  * I_UpdateBox() copies *dirty boxes* into the VRAM
  * I_FinishUpdate() modify the adress of the framebuffer to be displayed
* MODE13H  
  Rendering is done in RAM then the screen is copied to VRAM as a single block.
  * I_UpdateNoBlit() does not exist
  * I_FinishUpdate() performs the copy of the whole screen.

