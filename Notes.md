
DrawMasked, masked element : sprites, partialy transparent walls, player's weapon.

DrawFlat, flat visplanes : empty vertical space representing flors, ceils, sky...

Compositions en RAM puis copie par block dans VRAM.
* MODE_Y
  * I_UpdateNoBlit()		// Ecrit dans la VRAM "after every major write sequence"
      * Les copies des "dirtyboxes" sont réalisées par I_UpdateBox()
  * I_FinishUpdate()		// Update l'adresse pour source du framebuffer VRAM à afficher ?


* MODE_13H
  * I_UpdateNoBlit()		// Non existant
  * I_FinishUpdate()		// Copie l'intégralité du Framebuffer en VRAM pour affichage


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
