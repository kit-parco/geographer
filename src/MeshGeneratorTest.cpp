#include <scai/lama.hpp>
#include <scai/logging.hpp>

#include <scai/lama/matrix/all.hpp>
#include <scai/lama/matutils/MatrixCreator.hpp>
#include <scai/lama/Vector.hpp>

#include <scai/dmemo/BlockDistribution.hpp>

#include <scai/hmemo/Context.hpp>
#include <scai/hmemo/HArray.hpp>
#include <scai/hmemo/WriteAccess.hpp>
#include <scai/hmemo/ReadAccess.hpp>

#include <memory>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <chrono>

#include "ParcoRepart.h"
#include "gtest/gtest.h"
#include "HilbertCurve.h"
#include "MeshGenerator.h"
#include "Settings.h"
#include "FileIO.h"


using namespace scai;
using scai::lama::CSRStorage;

namespace ITI {

class MeshGeneratorTest : public ::testing::Test {
protected:
        // the directory of all the meshes used
        std::string graphPath = "./meshes/";

};

//-----------------------------------------------------------------

TEST_F(MeshGeneratorTest, testCreateStructured3DMeshLocalDegreeSymmetry) {
	scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
	IndexType k = comm->getSize();

	IndexType nroot = 300;
	IndexType n = nroot * nroot * nroot;
	IndexType dimensions = 3;

	if (k > 16) {
		scai::dmemo::DistributionPtr dist ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, n) );
		scai::dmemo::DistributionPtr noDistPointer(new scai::dmemo::NoDistribution(n));

		auto a = scai::lama::zero<scai::lama::CSRSparseMatrix<ValueType>>( dist, noDistPointer);
		std::vector<ValueType> maxCoord(dimensions, nroot);
		std::vector<IndexType> numPoints(dimensions, nroot);

		std::vector<DenseVector<ValueType>> coordinates(dimensions);
		for(IndexType i=0; i<dimensions; i++){
		  coordinates[i].allocate(dist);
		  coordinates[i] = static_cast<ValueType>( 0 );
		}

		MeshGenerator<IndexType, ValueType>::createStructured3DMesh_dist(a, coordinates, maxCoord, numPoints);
		ParcoRepart<IndexType, ValueType>::checkLocalDegreeSymmetry(a);
	} else {
		std::cout << "Not tested, since called with <= 16 processes, this implies you don't have enough memory for " << n << " nodes."<< std::endl;
	}
}
//-----------------------------------------------------------------
// Creates the part of a structured mesh in each processor ditributed and checks the matrix and the coordinates.
// For the coordinates checks if there are between min and max and for the matrix if every row has more than 3 and
// less than 6 ones ( every node has 3,4,5, or 6 neighbours).
TEST_F(MeshGeneratorTest, testCreateStructuredMesh_Distributed_3D) {
    std::vector<IndexType> numPoints= { 40, 40, 40};
    std::vector<ValueType> maxCoord= {441, 711, 1160};
    IndexType N= numPoints[0]*numPoints[1]*numPoints[2];
    
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    scai::dmemo::DistributionPtr dist ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, N) );
    scai::dmemo::DistributionPtr noDistPointer(new scai::dmemo::NoDistribution( N ));
    
    PRINT0("Building mesh of size "<< numPoints[0]<< "x"<< numPoints[1]<< "x"<< numPoints[2] << " , N=" << N );
    
    std::vector<DenseVector<ValueType>> coords(3);
    for(IndexType i=0; i<3; i++){ 
	  coords[i].allocate(dist);
	  coords[i] = static_cast<ValueType>( 0 );
    }
    
    auto adjM = scai::lama::zero<scai::lama::CSRSparseMatrix<ValueType>>( dist, noDistPointer);
    
    // create the adjacency matrix and the coordinates
    MeshGenerator<IndexType, ValueType>::createStructured3DMesh_dist(adjM, coords, maxCoord, numPoints);
    
    // print local values 
    /*
    for(IndexType i=0; i<dist->getLocalSize(); i++){
        std::cout<< i<< ": "<< *comm<< " - " <<coords[0].getLocalValues()[i] << " , " << coords[1].getLocalValues()[i] << " , " << coords[2].getLocalValues()[i] << std::endl;   
    }
    */
    
    EXPECT_EQ( adjM.getLocalNumColumns() , N);
    EXPECT_EQ( adjM.getLocalNumRows() , coords[0].getLocalValues().size() );
    EXPECT_EQ( true , adjM.getRowDistribution().isEqual(coords[0].getDistribution()) );
    
    // for a 3D structured grid with dimensions AxBxC the number of edges is 3ABC-AB-AC-BC
    IndexType numEdges= 3*numPoints[0]*numPoints[1]*numPoints[2] - numPoints[0]*numPoints[1]\
                                -numPoints[0]*numPoints[2] - numPoints[1]*numPoints[2];
    EXPECT_EQ( adjM.getNumValues() , numEdges*2 );     
    
    IndexType cntCorners= 0 , cntSides= 0, cntEdges= 0;
    
    {
        SCAI_REGION("testCreateStructuredMesh_Distributed_3D.check_adjM_2")
        const CSRStorage<ValueType>& localStorage = adjM.getLocalStorage();
	const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
        
        for(IndexType i=0; i<ia.size()-1; i++){
            // this checks that the number of non-zero elements in each row is less than 6 
            // (6 is the maximum number of neighbours a node can have in a structured grid)
            EXPECT_LE( ia[i+1]-ia[i], 6 );
            // and also more than 3 which is the minimum
            EXPECT_GE( ia[i+1]-ia[i], 3 );
            
            // count the nodes with 3 edges
            if(ia[i+1]-ia[i] == 3){
                ++cntCorners;
            }
            // count the nodes with 4 edges
            if(ia[i+1]-ia[i] == 4){
                ++cntEdges;
            }
            // count the nodes with 5 edges
            if(ia[i+1]-ia[i] == 5){
                ++cntSides;
            }
        }
    }
    IndexType numX= numPoints[0];
    IndexType numY= numPoints[1];
    IndexType numZ= numPoints[2];
    
    //PRINT( comm->sum(cntCorners) );
    
    // check the global values
    EXPECT_EQ( comm->sum(cntCorners), 8);
    EXPECT_EQ( comm->sum(cntEdges) , 4*(numX+numY+numZ)-24);
    EXPECT_EQ( comm->sum(cntSides) , 2*( (numX-2)*(numY-2)+ (numX-2)*(numZ-2)+ (numY-2)*(numZ-2) )  );
    
    //PRINT(comm << ", corner nodes= "<< cntCorners << " , edge nodes= "<< cntEdges<< " , side nodes= "<< cntSides);
    
    {
    SCAI_REGION("testCreateStructuredMesh_Distributed_3D.check_coords_2")
    std::vector<scai::hmemo::HArray<ValueType>> localCoords(3);
    for(IndexType i=0; i<3; i++){
        localCoords[i] = coords[i].getLocalValues();
    }
    for(IndexType i=0; i<localCoords[0].size(); i++){
        EXPECT_LE( localCoords[0][i] , maxCoord[0]);
        EXPECT_GE( localCoords[0][i] , 0);
        EXPECT_LE( localCoords[1][i] , maxCoord[1]);
        EXPECT_GE( localCoords[1][i] , 0);
        EXPECT_LE( localCoords[2][i] , maxCoord[2]);
        EXPECT_GE( localCoords[2][i] , 0);
    }
    }
    
}
//-----------------------------------------------------------------
// Creates the part of a structured mesh in each processor ditributed and checks the matrix and the coordinates.
// For the coordinates checks if there are between min and max and for the matrix if every row has more than 3 and
// less than 6 ones ( every node has 3,4,5, or 6 neighbours).
TEST_F(MeshGeneratorTest, testCreateStructuredMesh_Distributed_2D) {
    std::vector<IndexType> numPoints= { 31, 45};
    std::vector<ValueType> maxCoord= {441, 711};
    IndexType N= numPoints[0]*numPoints[1];
    
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    scai::dmemo::DistributionPtr dist ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, N) );
    scai::dmemo::DistributionPtr noDistPointer(new scai::dmemo::NoDistribution( N ));
    
    PRINT0("Building mesh of size "<< numPoints[0]<< "x"<< numPoints[1]<< " , N=" << N );
    
    std::vector<DenseVector<ValueType>> coords(2);
    for(IndexType i=0; i<2; i++){ 
	  coords[i].allocate(dist);
	  coords[i] = static_cast<ValueType>( 0 );
    }
    
    auto adjM = scai::lama::zero<scai::lama::CSRSparseMatrix<ValueType>>( dist, noDistPointer);
    
    // create the adjacency matrix and the coordinates
    MeshGenerator<IndexType, ValueType>::createStructured2DMesh_dist(adjM, coords, maxCoord, numPoints);
    
    // print local values 
    /*
    for(IndexType i=0; i<dist->getLocalSize(); i++){
        std::cout<< i<< ": "<< *comm<< " - " <<coords[0].getLocalValues()[i] << " , " << coords[1].getLocalValues()[i] << " , " << coords[2].getLocalValues()[i] << std::endl;   
    }
    */
    
    EXPECT_EQ( adjM.getLocalNumColumns() , N);
    EXPECT_EQ( adjM.getLocalNumRows() , coords[0].getLocalValues().size() );
    EXPECT_EQ( true , adjM.getRowDistribution().isEqual(coords[0].getDistribution()) );
    
    // for a 3D structured grid with dimensions AxBxC the number of edges is 3ABC-AB-AC-BC
    IndexType numEdges= (numPoints[0]-1)*numPoints[1] + numPoints[0]*(numPoints[1]-1);
    EXPECT_EQ( adjM.getNumValues() , numEdges*2 );     

    IndexType cntCorners= 0 , cntSides= 0, cntCenter= 0;
    
    {
        SCAI_REGION("testCreateStructuredMesh_Distributed_2D.check_adjM_2")
        const CSRStorage<ValueType>& localStorage = adjM.getLocalStorage();
        const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
        
        for(IndexType i=0; i<ia.size()-1; i++){
            // this checks that the number of non-zero elements in each row is less than 4 
            // (6 is the maximum number of neighbours a node can have in a structured grid)
            EXPECT_LE( ia[i+1]-ia[i], 4 );
            // and also more than 2 which is the minimum
            EXPECT_GE( ia[i+1]-ia[i], 2 );
            
            // count the nodes with 2 edges
            if(ia[i+1]-ia[i] == 2){
                ++cntCorners;
            }
            // count the nodes with 3 edges
            if(ia[i+1]-ia[i] == 3){
                ++cntSides;
            }
            // count the nodes with 4 edges
            if(ia[i+1]-ia[i] == 4){
                ++cntCenter;
            }
        }
    }
    //IndexType numX= numPoints[0];
    //IndexType numY= numPoints[1];
    
    //PRINT( comm->sum(cntCorners) );
    
    // check the global values
    EXPECT_EQ( comm->sum(cntCorners), 4);
    EXPECT_EQ( comm->sum(cntSides) , 2*(numPoints[0]+numPoints[1]-4));
    EXPECT_EQ( comm->sum(cntCenter) , N-2*(numPoints[0]+numPoints[1]-4)-4 );
    
    //PRINT(comm << ", corner nodes= "<< cntCorners << " , edge nodes= "<< cntEdges<< " , side nodes= "<< cntSides);
    
    {
        SCAI_REGION("testCreateStructuredMesh_Distributed_3D.check_coords_2")
        std::vector<scai::hmemo::HArray<ValueType>> localCoords(2);
        for(IndexType i=0; i<2; i++){
            localCoords[i] = coords[i].getLocalValues();
        }
        for(IndexType i=0; i<localCoords[0].size(); i++){
            EXPECT_LE( localCoords[0][i] , maxCoord[0]);
            EXPECT_GE( localCoords[0][i] , 0);
            EXPECT_LE( localCoords[1][i] , maxCoord[1]);
            EXPECT_GE( localCoords[1][i] , 0);
        }
    }
    
}

//-----------------------------------------------------------------
// Creates the part of a structured mesh in each processor ditributed and checks the matrix and the coordinates.
// For the coordinates checks if there are between min and max and for the matrix if every row has more than 3 and
// less than 6 ones ( every node has 3,4,5, or 6 neighbours).
TEST_F(MeshGeneratorTest, testCreateRandomStructuredMesh_Distributed_3D) {
    std::vector<IndexType> numPoints= { 140, 24, 190};
    std::vector<ValueType> maxCoord= {441, 711, 1160};
    IndexType N= numPoints[0]*numPoints[1]*numPoints[2];

    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    scai::dmemo::DistributionPtr dist ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, N) );
    scai::dmemo::DistributionPtr noDistPointer(new scai::dmemo::NoDistribution( N ));
    
    PRINT0("Building mesh of size "<< numPoints[0]<< "x"<< numPoints[1]<< "x"<< numPoints[2] << " , N=" << N );
            
    scai::lama::CSRSparseMatrix<ValueType> adjM;
    adjM = scai::lama::zero<scai::lama::CSRSparseMatrix<ValueType>>( dist, noDistPointer);
        
    std::vector<DenseVector<ValueType>> coords(3);
    for(IndexType i=0; i<3; i++){ 
	  coords[i].allocate(dist);
	  coords[i] = static_cast<ValueType>( 0 );
    }
    
    // create the adjacency matrix and the coordinates
    MeshGenerator<IndexType, ValueType>::createRandomStructured3DMesh_dist(adjM, coords, maxCoord, numPoints);
    
    PRINT0("Constructed Mesh." );

    EXPECT_EQ( adjM.getLocalNumColumns() , N);
    EXPECT_EQ( adjM.getLocalNumRows() , coords[0].getLocalValues().size() );
    EXPECT_EQ( true , adjM.getRowDistribution().isEqual(coords[0].getDistribution()) );
    
    // check symmetry in every PE
    ParcoRepart<IndexType, ValueType>::checkLocalDegreeSymmetry( adjM );
    if (!adjM.isConsistent()) {
	throw std::runtime_error("Input matrix inconsistent");
    }
    PRINT(*comm<< ": "<< adjM.getLocalNumValues() );
    PRINT(*comm<< ": "<< comm->sum(adjM.getLocalNumValues()) );
    
    {
        SCAI_REGION("testCreateRandomStructuredMesh_Distributed_3D.noDist")
        // gather/replicate locally and test whole matrix
        adjM.redistribute(noDistPointer, noDistPointer);
        
        ParcoRepart<IndexType, ValueType>::checkLocalDegreeSymmetry( adjM );
        PRINT(*comm<<": "<< adjM.getNumValues() );
        if (!adjM.isConsistent()) {
            throw std::runtime_error("Input matrix inconsistent");
        }
    }
    
    {
        SCAI_REGION("testCreateRandomStructuredMesh_Distributed_3D.cyclicDist")
        // test with a cyclic distribution
        scai::dmemo::DistributionPtr distCyc ( scai::dmemo::Distribution::getDistributionPtr( "CYCLIC", comm, N) );
        adjM.redistribute( distCyc, noDistPointer);
        
        ParcoRepart<IndexType, ValueType>::checkLocalDegreeSymmetry( adjM );
        if (!adjM.isConsistent()) {
            throw std::runtime_error("Input matrix inconsistent");
        }
    }
    
}

//-----------------------------------------------------------------
TEST_F(MeshGeneratorTest, testWriteMetis_Dist_3D){
    
    std::vector<IndexType> numPoints= { 10, 10, 10};
    std::vector<ValueType> maxCoord= { 10, 20, 30};
    IndexType N= numPoints[0]*numPoints[1]*numPoints[2];

    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    scai::dmemo::DistributionPtr dist ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, N) );
    scai::dmemo::DistributionPtr noDistPointer(new scai::dmemo::NoDistribution( N ));
    
    PRINT0("Building mesh of size "<< numPoints[0]<< "x"<< numPoints[1]<< "x"<< numPoints[2] << " , N=" << N );
        
    std::vector<DenseVector<ValueType>> coords(3);
    for(IndexType i=0; i<3; i++){ 
	  coords[i].allocate(dist);
	  coords[i] = static_cast<ValueType>( 0 );
    }
    
    auto adjM = scai::lama::zero<scai::lama::CSRSparseMatrix<ValueType>>( dist, noDistPointer);
    
    // create the adjacency matrix and the coordinates
    MeshGenerator<IndexType, ValueType>::createStructured3DMesh_dist(adjM, coords, maxCoord, numPoints);
    
    // write the mesh in p(=number of PEs) files
    FileIO<IndexType, ValueType>::writeGraphDistributed( adjM, "meshes/dist3D_");
    
}

//-----------------------------------------------------------------
TEST_F(MeshGeneratorTest, testMeshFromQuadTree_local){

    const IndexType numberOfAreas= 4;
    const IndexType pointsPerArea= 1000;
    const IndexType dimension = 3;
    const ValueType maxCoord = 100;

    scai::lama::CSRSparseMatrix<ValueType> graph;
    std::vector<DenseVector<ValueType>> coords( dimension );
    srand(time(NULL));
    int seed = rand()%10;
    
    ITI::MeshGenerator<IndexType, ValueType>::createQuadMesh( graph, coords, dimension, numberOfAreas, pointsPerArea, maxCoord, seed); 
    
    PRINT("edges: "<< graph.getNumValues() << " , nodes: " << coords[0].size() );    
    graph.isConsistent();
    //graph.checkSymmetry();
    assert( coords[0].size() == graph.getNumRows());
    
    // count the degree    
    const scai::lama::CSRStorage<ValueType>& localStorage = graph.getLocalStorage();
    const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
    IndexType upBound= 50;
    std::vector<IndexType> degreeCount( upBound*2, 0 );
    
    for(IndexType i=0; i<ia.size()-1; i++){
        IndexType nodeDegree = ia[i+1] -ia[i];
        EXPECT_LE(nodeDegree, degreeCount.size()-1);
        ++degreeCount[nodeDegree];
    }
    
    IndexType numEdges = 0;
    IndexType maxDegree = 0;
    //std::cout<< "\t Num of nodes"<< std::endl;
    for(int i=0; i<degreeCount.size(); i++){
        if(  degreeCount[i] !=0 ){
            //std::cout << "degree " << i << ":   "<< degreeCount[i]<< std::endl;
            numEdges += i*degreeCount[i];
            maxDegree = i;
        }
    }
    
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    
    ValueType averageDegree = ValueType( numEdges)/ia.size();
    PRINT0("num edges= "<< graph.getNumValues() << " , num nodes= " << graph.getNumRows() << ", average degree= "<< averageDegree << ", max degree= "<< maxDegree);  
    
    if(comm->getRank()==0){
        std::string outFile = graphPath + "quadTreeGraph3D_"+std::to_string(numberOfAreas)+".graph";
        ITI::FileIO<IndexType, ValueType>::writeGraph( graph, outFile);
        
        std::string outCoords = outFile + ".xyz";
        ITI::FileIO<IndexType, ValueType>::writeCoords(coords, outCoords);
    }
}
//-----------------------------------------------------------------

TEST_F(MeshGeneratorTest, testSimpleMeshFromQuadTree_2D){

    const IndexType numberOfAreas= 3;
    const IndexType dimension = 2;
    const IndexType pointsPerArea= 100*dimension;
    const ValueType maxCoord = 100;
    
    scai::lama::CSRSparseMatrix<ValueType> graph;
    std::vector<DenseVector<ValueType>> coords( dimension );
    srand(time(NULL));
    int seed = rand()%10;
    
    ITI::MeshGenerator<IndexType, ValueType>::createQuadMesh( graph, coords, dimension, numberOfAreas, pointsPerArea, maxCoord, seed); 
    
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    
    PRINT0("edges: "<< graph.getNumValues() << " , nodes: " << coords[0].size() );    
    graph.isConsistent();
    //graph.checkSymmetry();
    assert( coords[0].size() == graph.getNumRows());
    
    // count the degree    
    const scai::lama::CSRStorage<ValueType>& localStorage = graph.getLocalStorage();
    const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
    IndexType upBound= 50;
    std::vector<IndexType> degreeCount( upBound*2, 0 );
    
    for(IndexType i=0; i<ia.size()-1; i++){
        IndexType nodeDegree = ia[i+1] -ia[i];      
        EXPECT_LE(nodeDegree, degreeCount.size()-1);
        ++degreeCount[nodeDegree];
    }
    
    IndexType numEdges = 0;
    IndexType maxDegree = 0;
    //std::cout<< "\t Num of nodes"<< std::endl;
    for(int i=0; i<degreeCount.size(); i++){
        if(  degreeCount[i] !=0 ){
            //std::cout << "degree " << i << ":   "<< degreeCount[i]<< std::endl;
            numEdges += i*degreeCount[i];
            maxDegree = i;
        }
    }
    
    ValueType averageDegree = ValueType( numEdges)/ia.size();
    PRINT0("num edges= "<< graph.getNumValues() << " , num nodes= " << graph.getNumRows() << ", average degree= "<< averageDegree << ", max degree= "<< maxDegree);  
        
    if(comm->getRank()==0){
        std::string outFile = graphPath+ "graphFromQuad_2D.graph";
        ITI::FileIO<IndexType, ValueType>::writeGraph( graph, outFile);
        
        std::string outCoords = outFile + ".xyz";
        ITI::FileIO<IndexType, ValueType>::writeCoords(coords, outCoords);
    }
}

//-----------------------------------------------------------------
    
}//namespace ITI