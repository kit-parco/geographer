
#include <scai/lama.hpp>

#include <scai/lama/matrix/all.hpp>
#include <scai/lama/matutils/MatrixCreator.hpp>
#include <scai/lama/Vector.hpp>

#include <scai/dmemo/BlockDistribution.hpp>

#include <scai/hmemo/Context.hpp>
#include <scai/hmemo/HArray.hpp>
#include <scai/hmemo/WriteAccess.hpp>
#include <scai/hmemo/ReadAccess.hpp>

#include <scai/utilskernel/LArray.hpp>

#include <memory>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <chrono>

#include "MeshGenerator.h"
#include "Settings.h"
#include "FileIO.h"

typedef double ValueType;
typedef int IndexType;


int main(int argc, char** argv){

    using namespace boost::program_options;
    options_description desc("Supported options");
    
    IndexType maxNumberOfAreas= 10;
    IndexType pointsPerArea= 500;
    IndexType dimension = 3;
    ValueType maxCoord = 1000;
    std::string outFile = "./graphFromQuad3D/graphFromQuad3D_"+std::to_string(numberOfAreas);
    
    desc.add_options()
        ("numOfAreas", value<IndexType>(&maxNumberOfAreas), "The number of areas os interest where more points are gonna be inserted")
        ("pointsPerArea", value<IndexType>(&pointsPerArea), "The number of points which every area of interest has.")
        ("dimension", value<IndexType>(&dimension), "The dimension of the poits")
        ("maxCoord", value<ValueType>(&maxCoord), "The maximum coordinate the points will have")
        ("filename", valie<std::string>(&outFile), "The name of the output file to write graph and coordinates (coordinates will have the .xyz ending)")
        ;
    
    for(int numberOfAreas=1; numberOfAreas<maxNumberOfAreas; numberOfAreas++){
        std::chrono::time_point<std::chrono::system_clock> startTime = std::chrono::system_clock::now();
    
        scai::lama::CSRSparseMatrix<ValueType> graph;
        std::vector<DenseVector<ValueType>> coords( dimension );
        
        ITI::MeshGenerator<IndexType, ValueType>::createQuadMesh( graph, coords, dimension, numberOfAreas, pointsPerArea, maxCoord); 
        
        std::chrono::duration<double> genTime = std::chrono::system_clock::now() - startTime;
        std::cout<< "time to create quadTree and get the graph: "<<genTime.count();
        
        //PRINT("edges: "<< graph.getNumValues() << " , nodes: " << coords[0].size() );    
        graph.isConsistent();
        assert( coords[0].size() == graph.getNumRows());
        
        {
            // count the degree    
            const scai::lama::CSRStorage<ValueType>& localStorage = graph.getLocalStorage();
            const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
            IndexType upBound= 40*dimension;
            std::vector<IndexType> degreeCount( upBound, 0 );
            
            for(IndexType i=0; i<ia.size()-1; i++){
                IndexType nodeDegree = ia[i+1] -ia[i];
                SCAI_ASSERT(nodeDegree < degreeCount.size()-1, "Local node " << i << " has degree " << nodeDegree << ", which is too high.");
                ++degreeCount[nodeDegree];
            }
            
            IndexType numEdges = 0;
            IndexType maxDegree = 0;
            std::cout<< "\t Num of nodes"<< std::endl;
            for(int i=0; i<degreeCount.size(); i++){
                if(  degreeCount[i] !=0 ){
                    std::cout << "degree " << i << ":   "<< degreeCount[i]<< std::endl;
                    numEdges += i*degreeCount[i];
                    maxDegree = i;
                }
            }
            
            ValueType averageDegree = ValueType( numEdges)/ia.size();
            PRINT("num edges= "<< graph.getNumValues() << " , num nodes= " << graph.getNumRows() << ", average degree= "<< averageDegree << ", max degree= "<< maxDegree);  
        }
        
        std::chrono::duration<double> degreeTime = std::chrono::system_clock::now() - startTime -genTime;
        
        ITI::FileIO<IndexType, ValueType>::writeGraph( graph, outFile);
        
        std::string outCoords = outFile + ".xyz";
        ITI::FileIO<IndexType, ValueType>::writeCoords(coords, outCoords);
        
        std::cout<< "Output written in files \""<< outFile << "\" and .xyz  in time: "<< degreeTime.count()<< std::endl;
    }
    
}
