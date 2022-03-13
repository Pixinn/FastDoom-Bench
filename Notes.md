

Compositions en RAM puis copie par block dans VRAM.
* MODE_Y
  * I_UpdateNoBlit()		// Ecrit dans la VRAM "after every major write sequence"
      * Les copies des "dirtyboxes" sont réalisées par I_UpdateBox()
  * I_FinishUpdate()		// Update l'adresse pour source du framebuffer VRAM à afficher ?


* MODE_13H
  * I_UpdateNoBlit()		// Non existant
  * I_FinishUpdate()		// Copie l'intégralité du Framebuffer en VRAM pour affichage
