;
; Copyright (C) 1993-1996 Id Software, Inc.
; Copyright (C) 1993-2008 Raven Software
;
; This program is free software; you can redistribute it and/or
; modify it under the terms of the GNU General Public License
; as published by the Free Software Foundation; either version 2
; of the License, or (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; DESCRIPTION: Assembly texture mapping routines for linear VGA mode
;

BITS 32
%include "defs.inc"
%include "macros.inc"

extern _centery

;============================================================================
; unwound vertical scaling code
;
; eax   light table pointer, 0 lowbyte overwritten
; ebx   all 0, low byte overwritten
; ecx   fractional step value
; edx   fractional scale value
; esi   start of source pixels
; edi   bottom pixel in screenbuffer to blit into
;
; ebx should be set to 0 0 0 dh to feed the pipeline
;
; The graphics wrap vertically at 128 pixels
;============================================================================

BEGIN_DATA_SECTION

%macro SCALEDEFINE 1
  dd vscale%1
%endmacro

align 4

scalecalls:
  %assign LINE 0
  %rep SCREENHEIGHT+1
    SCALEDEFINE LINE
  %assign LINE LINE+1
  %endrep

BEGIN_CODE_SECTION

; ================
; R_DrawColumn_13h
; ================
CODE_SYM_DEF R_DrawColumn_13h
  pushad

  mov  ebp,[_dc_yh]
  mov  ebx,ebp
  mov  edi,[_ylookup+ebx*4]
  mov  ebx,[_dc_x]
  add  edi,[_columnofs + ebx*4]

  mov  eax,[_dc_yl]
  sub  ebp,eax         ; ebp = pixel count
  or   ebp,ebp
  js   short .done

  mov   ecx,[_dc_iscale]

  sub   eax,[_centery]
  imul  ecx
  mov   edx,[_dc_texturemid]
  add   edx,eax
  shl   edx,9

  shl   ecx,9
  mov   esi,[_dc_source]

  mov   eax,[_dc_colormap]

  xor   ebx,ebx
  shld  ebx,edx,7
  call  [scalecalls+4+ebp*4]

.done:
  popad
  ret
; R_DrawColumn_13h ends

%macro SCALELABEL 1
  vscale%1
%endmacro

%assign LINE SCREENHEIGHT
%rep SCREENHEIGHT-1
  SCALELABEL LINE:
    mov  al,[esi+ebx]                   ; get source pixel
    add  edx,ecx                        ; calculate next location
    mov  al,[eax]                       ; translate the color
    mov  ebx,edx
    shr  ebx,25
    mov  [edi-(LINE-1)*SCREENWIDTH],al  ; draw a pixel to the buffer
    %assign LINE LINE-1
%endrep

vscale1:
  mov al,[esi+ebx]
  mov al,[eax]
  mov [edi],al

vscale0:
  ret

;============================================================================
; unwound horizontal texture mapping code
;
; eax   lighttable
; ebx   scratch register
; ecx   position 6.10 bits x, 6.10 bits y
; edx   step 6.10 bits x, 6.10 bits y
; esi   start of block
; edi   dest
; ebp   fff to mask bx
;
; ebp should by preset from ebx / ecx before calling
;============================================================================

CONTINUE_DATA_SECTION

%macro MAPDEFINE 1
  dd hmap%1
%endmacro

align 4

mapcalls:
  %assign LINE 0
  %rep SCREENWIDTH+1
    MAPDEFINE LINE
    %assign LINE LINE+1
  %endrep

callpoint:   dd 0
returnpoint: dd 0

CONTINUE_CODE_SECTION

; ==============
; R_DrawSpan_13h
; ==============
CODE_SYM_DEF R_DrawSpan_13h
  pusha

  mov     eax,[_ds_x1]
  mov     ebx,[_ds_x2]
  mov     eax,[mapcalls+eax*4]
  mov     [callpoint],eax       ; spot to jump into unwound
  mov     eax,[mapcalls+4+ebx*4]
  mov     [returnpoint],eax     ; spot to patch a ret at
  mov     [eax], byte OP_RET

  ; build composite position

  mov     ecx,[_ds_xfrac]
  shl     ecx,10
  and     ecx,0xFFFF0000
  mov     eax,[_ds_yfrac]
  shr     eax,6
  and     eax,0x0000FFFF
  or      ecx,eax

  ; build composite step

  mov     edx,[_ds_xstep]
  shl     edx,10
  and     edx,0xFFFF0000
  mov     eax,[_ds_ystep]
  shr     eax,6
  and     eax,0x0000FFFF
  or      edx,eax

  mov     esi,[_ds_source]

  mov     edi,[_ds_y]
  mov     edi,[_ylookup+edi*4]
  add     edi,[_columnofs]

  mov     eax,[_ds_colormap]

  ; feed the pipeline and jump in

  mov     ebp,0x0FFF  ; used to mask off slop high bits from position
  shld    ebx,ecx,22  ; shift y units in
  shld    ebx,ecx,6   ; shift x units in
  and     ebx,ebp     ; mask off slop bits
  call    [callpoint]

  mov     ebx,[returnpoint]
  mov     [ebx],byte OP_MOVAL         ; remove the ret patched in

  popad
  ret
; R_DrawSpan_13h ends

%macro MAPLABEL 1
  hmap%1
%endmacro

%assign LINE 0
%assign PCOL 0
%rep SCREENWIDTH/4
  %assign PLANE 0
  %rep 4
    MAPLABEL LINE:
      %assign LINE LINE+1
      mov   al,[esi+ebx]           ; get source pixel
      shld  ebx,ecx,22             ; shift y units in
      shld  ebx,ecx,6              ; shift x units in
      mov   al,[eax]               ; translate color
      and   ebx,ebp                ; mask off slop bits
      add   ecx,edx                ; position += step
      mov   [edi+PLANE+PCOL*4],al  ; write pixel
      %assign PLANE PLANE+1
  %endrep
%assign PCOL PCOL+1
%endrep

hmap320: ret
