from subprocess import call
import os
import math
import random
import argparse

from submitFileWrapper_Supermuc import *
#from test_SubmitScript import submitAllCompetitors, submitExp2
from test_SubmitScript import submitExp2, submitAllCompetitors
from header import *


parser = argparse.ArgumentParser(description='Submit jobs in Supermuc batch job system  for the selected tools for a signle file and one or more values of k (appropriate for one experiment run was forgotten or crashed)')
parser.add_argument('--tools','-t' , type=str , nargs='*', default="Geographer", help='Name of the tools. It can be: Geographer, parMetisGraph, parMetisGeom.')
#parser.add_argument('--configFile','-c', default="SaGa.config", help='The configuration file. ')
#parser.add_argument('--wantedExp', '-we', type=int, nargs='*', metavar='exp', help='A subset of the experiments that will be submited.')

parser.add_argument('--fileName','-f', help='The file/graph to be partitioned.')
parser.add_argument('--numBlocks','-k', type=int, nargs='*', help='The number of blocks/parts to partition to.')
parser.add_argument('--fileFormat', '-ff', help='The format of the file given.')
parser.add_argument('--dimensions', '-d', help='The dimensions of the coordinates.')

args = parser.parse_args()
print(args)

fileName = args.fileName
numPEs = args.numBlocks
fileFormat = args.fileFormat
dimensions = args.dimensions
wantedTools = args.tools

#coordFormat = 0


initialPartition = 3
#only works with for initialPartition=3 (k-means)
initialMigration = 0	#0:SFC, 3:k-means, 4:ms

repeatTimes = 5

#----------------------------------------------
#		check inout
fileEnding = fileName.split('.')[-1]

if (fileFormat==1 and fileEnding!="graph") or (fileFormat==6 and fileEnding!="bgf"):
	print("WARNING: file format and file given probably do not agree.")

if not os.path.exists(fileName):
	print("ERROR: file " + fileName + " does not exist.\nAborting...")
	exit(-1)
	
for k in numPEs:
	if int(k)/16 != int(int(k)/16):
		print("WARNING: k= " + k + " is not a multiple of 16")
if int(dimensions) <=0:
	print("WARNING: wrong value for dimension: " + str(exp.dimension))
if int(fileFormat) <0:
	print("WARNING: wrong value for fileFormat: " + str(exp.dimension))
		
		
''' TODO: separate coordFile and coordFormat
if coordFormat==0:
	coordEnding=".graph.xyz"
elif coordFormat==6:
	coordEnding=".bgf.xyz"
else:
	print("Unknown file format "+ str(fileFormat)+ ", aborting.")
	exit(-1)

coordFile = os.path.join(dirString, fileString+coordEnding )
if not os.path.exists(coordFile):
	print(coordFile + " does not exist.")
	exit(-1)
'''

# treat as an experiment
size = len(numPEs)

exp = experiment()
exp.expType = 2
exp.dimension = dimensions
exp.fileFormat = fileFormat
exp.ID = -1
exp.k = numPEs
exp.size = size

exp.paths = [fileName]*size
print( os.path.basename(fileName) )
exp.graphs = [ os.path.basename(fileName) ] * size

exp.printExp()

if wantedTools[0]=="all":
	#wantedTools = allTools[1:]
	print("\n\tWARNING: Will call allCompetitorsExe that runs the experiment with all tools!!")
	confirm = raw_input("Continue? :")
	while not(str(confirm)=="Y" or str(confirm)=="N" or str(confirm)=="y" or str(confirm)=="n"):
		#confirm= input("Please type Y or N ")		#python3
		confirm= raw_input("Please type Y/y or N/n: ")	
		
	if str(confirm)=='N' or str(confirm)=='n':
		print("Not submitting experiments, aborting...")
		exit(0)

	submitAllCompetitors( exp )	
		
	exit(0)
	

for tool in wantedTools:	
	
	confirm = raw_input("Submit experiments with >>> " + str(tool) +" <<< Y/N:")
	while not(str(confirm)=="Y" or str(confirm)=="N" or str(confirm)=="y" or str(confirm)=="n"):
		#confirm= input("Please type Y or N ")		#python3
		confirm= raw_input("Please type Y/y or N/n: ")	
		
	if str(confirm)=='N' or str(confirm)=='n':
		print("Not submitting experiments...")
		continue
		
	submitExp2( exp, tool )

frameinfo = getframeinfo(currentframe())
print("Exiting " +frameinfo.filename + " script")
exit(0)


