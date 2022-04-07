#!/usr/bin/env python3

from itertools import filterfalse
import sys
import os
import argparse
import csv
import np
from matplotlib import pyplot as plt
from collections import defaultdict



Errors = []

# Fatal Error
def Error_Fatal(msg): 
    print("Error: " + msg)
    sys.exit(-1)

# Recoverable Error: displaying and logging
def Error(msg):
    Errors.append(msg)
    print(msg)

# must be in sync with b_ben.h:
FUNCTION_MAX = 17 
FRAME  = 0
TIME   = 1
FCT_ID = 2

FUNCTIONS = [
        "INITGFX",
        "UPDATESOUND",
        "RUNTICK",
        "MAP2D",
        "STATUSBAR2D",
        "MENU2D",
        "R_SETUPFRAME",
        "R_CLEARBUFFERS",
        "R_BSPNODE_FRONT",
        "R_BSPNODE_FINDPLANE",
        "R_BSPNODE_ADDSPRITES",
        "R_BSPNODE_ADDLINE",
        "R_BSPNODE_BACK",
        "R_DRAWPLANES",
        "R_DRAWMASKED",
        "UPDATENOBLIT",
        "FINISHUPDATE",
        "MISC"
    ];

COLORS = [
        "black",
        "skyblue",
        "forestgreen",
        "silver",
        "blue",
        "red",
        "yellow",
        "cyan",
        "fuchsia",
        "chartreuse",
        "orange",
        "darkviolet",
        "springgreen",
        "bisque",
        "indigo",
        "coral",
        "khaki",
        "green"
]

if __name__ == '__main__':

    # Arguments
    ## parse the arguments
    parser = argparse.ArgumentParser(
        description="Plot the bench data",
        usage=''' plot <file>
file        File containing the bench data.
''')
    parser.add_argument("file", help="File containing the bench data.")
    parser.add_argument("begin", help="Beginning frame")
    parser.add_argument("stride", help="Frame stride")
    args = parser.parse_args()
    ## sanity
    if not os.path.isfile(args.file):
        Error_Fatal(args.file + " does not exists.")

    ## Parse csv
    frames = []
    with open(args.file) as csv_file:
        
        csv_reader = csv.reader(csv_file, delimiter=';')
        
        currFrame = 0
        bench = []
        frame_bench = [0] * (FUNCTION_MAX + 1)
        rowNr = 0
        for row in csv_reader:

            if not rowNr == 0:
                row = [int(num) for num in row] # CSV is text

                if not row[FRAME] == currFrame:
                    bench.append(frame_bench)
                    frame_bench = [0] * (FUNCTION_MAX + 1)
                    currFrame = row[FRAME]
                                  
                frame_bench[row[FCT_ID]] += float(row[TIME])/1.193182e3

            rowNr += 1

    # Format data for Matplotlib
    frames = []
    functions = defaultdict(list)
    frameNr = int(args.begin)
    stride = int(args.stride)
    for frame in bench[frameNr::stride]:
        frames.append(frameNr)
        frameNr = frameNr + stride
        for func, time in enumerate(frame):
            functions[FUNCTIONS[func]].append(time)
    
    # Build stacked bars
    plt.figure(figsize=(22,14))
    firstFct = True
    prevFunc = ""
    bottoms = [0] * len(functions[FUNCTIONS[0]])
    idx = 0
    for func in functions:
        if firstFct:
            firstFct = False
            plt.bar(frames, functions[func], stride, label = func, color=COLORS[idx])
        else:
            for i in range(len(bottoms)):
                bottoms[i] = bottoms[i] + functions[prevFunc][i]
            plt.bar(frames, functions[func], stride, bottom=bottoms, label = func, color=COLORS[idx])
        prevFunc = func
        idx = idx + 1

    # Display    
    plt.xlabel("Frame number")
    plt.ylabel("Time in ms")
    plt.legend(loc='upper left')
    plt.show()

    

    sys.exit(0)