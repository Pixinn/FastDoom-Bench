//
// Copyright (C) 1993-1996 Id Software, Inc.
// Copyright (C) 2016-2017 Alexey Khokholov (Nuke.YKT)
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//  DOOM Network game communication and protocol,
//  all OS independend parts.
//

#include "m_menu.h"
#include "i_system.h"
#include "g_game.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"


#include "b_bench.h"

//
// NETWORKING
//
// gametic is the tic about to (or currently being) run
// maketic is the tick that hasn't had control made for it yet
// nettics[] has the maketics for all players
//
// a gametic cannot be run until nettics[] > gametic for all players
//
#define PL_DRONE 0x80 // bit flag in doomdata->player

ticcmd_t localcmds[BACKUPTICS];

int nettics;

int maketic;
int skiptics;

void D_ProcessEvents(void);
void G_BuildTiccmd(ticcmd_t *cmd);
void D_DoAdvanceDemo(void);

//
//
//
int ExpandTics(int low)
{
	int delta;
	int opt;

	delta = low - (maketic & 0xff);
	opt = maketic & ~0xff;

	if (delta < -64)
		return opt + 256 + low;
	else if (delta > 64)
		return opt - 256 + low;

	return opt + low;
}

//
// NetUpdate
// Builds ticcmds for console player,
// sends out a packet
//
int gametime;

void NetUpdate(void)
{

	int nowtime;
	int newtics;
	int i, j;

    B_BenchStart();

	// check time
	nowtime = ticcount;
	newtics = nowtime - gametime;
	gametime = nowtime;

	if (newtics <= 0) { // nothing new to update
        B_BenchEnd(MISC);
		return;
    }

	if (skiptics <= newtics)
	{
		newtics -= skiptics;
		skiptics = 0;
	}
	else
	{
		skiptics -= newtics;
		newtics = 0;
	}

	// build new ticcmds for console player
	for (i = 0; i < newtics; i++)
	{
		I_StartTic();
		D_ProcessEvents();
		if (maketic - gametic >= BACKUPTICS / 2 - 1)
			break; // can't hold any more

		G_BuildTiccmd(&localcmds[maketic & (BACKUPTICS-1)]);
		maketic++;
	}

	if (singletics) {
        B_BenchEnd(MISC);
		return; // singletic update is syncronous
    }

	nettics = ExpandTics((byte)maketic);

    B_BenchEnd(MISC);
}

//
// D_CheckNetGame
// Works out player numbers among the net participants
//
void D_CheckNetGame(void)
{
	nettics = 0;
	playeringame = true;
}

//
// TryRunTics
//

extern byte advancedemo;

void TryRunTics(void)
{
	int i;
	int entertic;
	static int oldentertics;
	int realtics;
	int availabletics;
	int counts;

	// get real tics
	entertic = ticcount;
	realtics = entertic - oldentertics;
	oldentertics = entertic;

	// get available tics
	NetUpdate();

	availabletics = nettics - gametic;

	// decide how many tics to run
	if (realtics + 1 < availabletics)
		counts = realtics + 1;
	else if (realtics < availabletics)
		counts = realtics;
	else
		counts = availabletics;

	if (counts < 1)
		counts = 1;

	// wait for new tics if needed
	while (nettics < gametic + counts)
	{
		NetUpdate();

		// don't stay in here forever -- give the menu a chance to work
		if (ticcount - entertic >= 20)
		{
			M_Ticker();
			return;
		}

		// Render interpolated frames
		if (uncappedFPS){
			D_Display();
		}
	}

	// run the count dics
	while (counts--)
	{
		if (advancedemo)
			D_DoAdvanceDemo();
		M_Ticker();
		G_Ticker();
		gametic++;
		NetUpdate(); // check for new console commands
	}
}
