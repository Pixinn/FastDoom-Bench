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
FUNCTION_MAX = 14 
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
        "R_BSPNODE_SUBSECTOR",
        "R_BSPNODE_BACK",
        "R_DRAWPLANES",
        "R_DRAWMASKED",
        "UPDATENOBLIT",
        "FINISHUPDATE"
    ];

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
    for frame in bench[frameNr:]:
        frames.append(frameNr)
        frameNr = frameNr + 1
        for func, time in enumerate(frame):
            functions[FUNCTIONS[func]].append(time)
    
    # Build stacked bars
    plt.figure(figsize=(22,14))
    firstFct = True
    prevFunc = ""
    for func in functions:
        if firstFct:
            firstFct = False
            plt.bar(frames, functions[func], 1, label = func)
        else:
            plt.bar(frames, functions[func], 1, bottom=functions[prevFunc], label = func)
        prevFunc = func

    # Display    
    plt.xlabel("Frame number")
    plt.ylabel("Time in ms")
    plt.legend()
    plt.show()

    

    sys.exit(0)