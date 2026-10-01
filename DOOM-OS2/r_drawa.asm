; Emacs style mode select   -*- Asm -*-
;-----------------------------------------------------------------------------
;
; $Id:$
;
; Copyright (C) 2026 by Charlie Dobson.
;
; Written for the OS/2 port of DOOM.  The two routines below are assembly
; versions of R_DrawColumn and R_DrawSpan from R_DRAW.C, which are id
; Software's; the code here is not theirs, but what it computes is, exactly.
;
; This source is available for distribution and/or modification
; only under the terms of the DOOM Source Code License as
; published by id Software. All rights reserved.
;
; The source is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
; for more details.
;
; DESCRIPTION:
;	The two inner loops of the renderer, in 386 assembly.
;
;	Every wall pixel goes through R_DrawColumn and every floor and
;	ceiling pixel through R_DrawSpan, so between them they are most of a
;	frame.  id shipped hand written assembly for exactly these two in the
;	DOS release and left the C versions in the source for everyone else.
;
;	The win is not that a person writes better instructions than Watcom
;	does.  It is aliasing.  Both loops end in a store through a byte
;	pointer -- "*dest++ = ..." -- and a byte pointer may point at
;	anything, including the globals the loop is reading.  A correct
;	compiler therefore has to reload ds_colormap, ds_source, ds_xstep and
;	ds_ystep from memory on every single pixel, because the store it just
;	made might have changed them.  It cannot know otherwise.
;
;	Below, they sit in registers and are loaded once.  That is the whole
;	trick, and it is worth roughly the number of memory reads it removes.
;
;	(Watcom will assume no aliasing if asked -- that is the "a" in -oa --
;	and MKOS2.CMD deliberately does not ask, because this program has code
;	that would break under it.  Doing it here instead is the safe half of
;	that bargain: no aliasing assumption is made about anything except
;	these two loops, where it is provably true.)
;
;	What these do NOT change: R_DrawColumnLow and R_DrawSpanLow, which is
;	what the game calls in low detail mode, are still C.
;
;-----------------------------------------------------------------------------

		.386
		.model	flat

;
; SCREENWIDTH, from DOOMDEF.H.
;
; Hardcoded here because it is a #define and there is nothing to read at run
; time.  R_DRAW.C carries a #if that fails the build if the two ever disagree,
; so this cannot rot silently.
;
SCREENWIDTH	equ	320

; Masks out of R_DrawSpan's spot calculation.  63*64 is the v coordinate
; already multiplied by the 64 byte width of a flat; 63 is the u.
SPOT_V_MASK	equ	4032
SPOT_U_MASK	equ	63

		extrn	_dc_yl		: dword
		extrn	_dc_yh		: dword
		extrn	_dc_x		: dword
		extrn	_dc_iscale	: dword
		extrn	_dc_texturemid	: dword
		extrn	_dc_source	: dword
		extrn	_dc_colormap	: dword

		extrn	_ds_y		: dword
		extrn	_ds_x1		: dword
		extrn	_ds_x2		: dword
		extrn	_ds_xfrac	: dword
		extrn	_ds_yfrac	: dword
		extrn	_ds_xstep	: dword
		extrn	_ds_ystep	: dword
		extrn	_ds_source	: dword
		extrn	_ds_colormap	: dword

		extrn	_ylookup	: dword
		extrn	_columnofs	: dword
		extrn	_centery	: dword

		public	R_DrawColumnA_
		public	R_DrawSpanA_

		.code

;-----------------------------------------------------------------------------
;
; R_DrawColumnA
;
; void R_DrawColumnA (void)
;
; One vertical slice of a wall texture, scaled.  Exactly R_DrawColumn:
;
;	count = dc_yh - dc_yl;
;	if (count < 0) return;
;	dest = ylookup[dc_yl] + columnofs[dc_x];
;	frac = dc_texturemid + (dc_yl - centery) * dc_iscale;
;	do {
;	    *dest = dc_colormap[dc_source[(frac>>16) & 127]];
;	    dest += SCREENWIDTH;
;	    frac += dc_iscale;
;	} while (count--);
;
; Registers, all held across the loop:
;
;	esi	frac			edi	dest
;	ebx	fracstep		ebp	dc_source
;	ecx	count			edx	dc_colormap
;	eax	scratch
;
; EVERY register except eax is saved, and that is not belt and braces.
;
; Watcom's register calling convention is not "eax, ecx and edx are scratch".
; It is that a register is the callee's to destroy only if it was used to pass
; a parameter or to return a value -- so for a routine taking nothing and
; returning nothing, everything but eax belongs to the caller.  Watcom's own
; code for R_DrawColumn opens by pushing ebx, ecx, edx, esi, edi and ebp, all
; six, which is what settled it.
;
; Getting this wrong is not subtle in its effect and is very subtle to find:
; the caller kept a loop counter in edx across the call, this routine wrote
; over it, and a loop in the test harness ran a random number of times.
;
; It costs two pushes and two pops per column, against a loop that runs up to
; two hundred times inside it.
;
;-----------------------------------------------------------------------------

R_DrawColumnA_	proc	near

		push	ebx
		push	ecx
		push	edx
		push	esi
		push	edi
		push	ebp

		mov	ecx, [_dc_yh]
		sub	ecx, [_dc_yl]		; count = dc_yh - dc_yl
		js	short cdone		; a column of no pixels at all

		;
		; dest = ylookup[dc_yl] + columnofs[dc_x]
		;
		mov	eax, [_dc_yl]
		mov	edi, [_ylookup+eax*4]
		mov	edx, [_dc_x]
		add	edi, [_columnofs+edx*4]

		;
		; frac = dc_texturemid + (dc_yl - centery) * fracstep
		;
		; imul r32,r32 keeps the low 32 bits, which is what C's int
		; multiply does and what the overflow here relies on.
		;
		mov	ebx, [_dc_iscale]
		mov	esi, eax		; eax is still dc_yl
		sub	esi, [_centery]
		imul	esi, ebx
		add	esi, [_dc_texturemid]

		mov	ebp, [_dc_source]
		mov	edx, [_dc_colormap]

		;
		; The two byte loads are written as "mov al" rather than movzx
		; on purpose.  After "and eax,127" the top 24 bits of eax are
		; zero, and writing al leaves them zero -- so eax is already
		; the zero extended index the next load needs, and the whole
		; sequence is two bytes an instruction with no partial
		; register stall on a 486.
		;
		; [ebp+eax] addresses through SS rather than DS.  Under the
		; flat model both describe the same 4GB, so it makes no odds.
		;
cloop:
		mov	eax, esi		; frac
		sar	eax, 16
		and	eax, 127
		mov	al, [ebp+eax]		; dc_source[...]
		add	esi, ebx		; frac += fracstep
		mov	al, [edx+eax]		; dc_colormap[...]
		mov	[edi], al
		add	edi, SCREENWIDTH
		dec	ecx
		jns	short cloop		; do { } while (count--)

cdone:
		pop	ebp
		pop	edi
		pop	esi
		pop	edx
		pop	ecx
		pop	ebx
		ret

R_DrawColumnA_	endp


;-----------------------------------------------------------------------------
;
; R_DrawSpanA
;
; void R_DrawSpanA (void)
;
; One horizontal run of floor or ceiling.  Exactly R_DrawSpan:
;
;	dest = ylookup[ds_y] + columnofs[ds_x1];
;	count = ds_x2 - ds_x1;
;	do {
;	    spot = ((yfrac>>10) & 4032) + ((xfrac>>16) & 63);
;	    *dest++ = ds_colormap[ds_source[spot]];
;	    xfrac += ds_xstep;
;	    yfrac += ds_ystep;
;	} while (count--);
;
; Registers:
;
;	esi	xfrac			edi	dest
;	ebp	yfrac			ebx	ds_source
;	eax	scratch			edx	ds_colormap
;	ecx	scratch
;
; That is one register more than there is, so the loop counter lives on the
; stack.  The steps are read from memory each time round, which costs nothing
; extra: they would be memory reads in either version, and unlike the C they
; are the ONLY memory reads left in the loop.
;
; The shifts are shr where the C has an arithmetic >>.  The two differ only
; above the bits that survive the mask -- both take bits 16 to 21 of the
; fraction -- so the result is identical and shr is the cheaper encoding.
;
;-----------------------------------------------------------------------------

R_DrawSpanA_	proc	near

		push	ebx
		push	ecx
		push	edx
		push	esi
		push	edi
		push	ebp

		mov	ecx, [_ds_x2]
		sub	ecx, [_ds_x1]		; count = ds_x2 - ds_x1
		js	short sdone		; see the note below

		push	ecx			; the loop counter

		;
		; dest = ylookup[ds_y] + columnofs[ds_x1]
		;
		mov	eax, [_ds_y]
		mov	edi, [_ylookup+eax*4]
		mov	eax, [_ds_x1]
		add	edi, [_columnofs+eax*4]

		mov	esi, [_ds_xfrac]
		mov	ebp, [_ds_yfrac]
		mov	ebx, [_ds_source]
		mov	edx, [_ds_colormap]

		;
		; movzx is needed here where the column loop did not need it:
		; spot runs to 4095, so the top of eax is not clear and
		; writing al alone would leave rubbish above the byte.
		;
sloop:
		mov	eax, ebp		; yfrac
		shr	eax, 10
		and	eax, SPOT_V_MASK
		mov	ecx, esi		; xfrac
		shr	ecx, 16
		and	ecx, SPOT_U_MASK
		add	eax, ecx		; spot

		movzx	eax, byte ptr [ebx+eax]	; ds_source[spot]
		mov	al, [edx+eax]		; ds_colormap[...]
		mov	[edi], al
		inc	edi

		add	esi, [_ds_xstep]
		add	ebp, [_ds_ystep]

		dec	dword ptr [esp]
		jns	short sloop

		pop	ecx

sdone:
		pop	ebp
		pop	edi
		pop	esi
		pop	edx
		pop	ecx
		pop	ebx
		ret

R_DrawSpanA_	endp

;
; A note on the "js short sdone" above.
;
; R_DrawSpan in C has no such test, and a span with x2 below x1 sends it into
; a loop of four thousand million pixels -- do..while(count--) never stops
; going once count starts negative.  Nothing calls it that way; RANGECHECK
; asserts as much when it is compiled in.
;
; Refusing to draw is the same answer in every case that is not already a
; disaster, and a considerably better one in the case that is.
;

		end
