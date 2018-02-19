/*
 * GraphUtils.cpp
 *
 *  Created on: 29.06.2017
 *      Author: moritzl
 */

#include <assert.h>
#include <queue>
#include <unordered_set>
#include <chrono>

#include <scai/hmemo/ReadAccess.hpp>
#include <scai/hmemo/WriteAccess.hpp>
#include <scai/dmemo/Halo.hpp>
#include <scai/dmemo/HaloBuilder.hpp>

#include "GraphUtils.h"
#include "RBC/Sort/SQuick.hpp"


using std::vector;
using std::queue;

namespace ITI {

namespace GraphUtils {

using scai::hmemo::ReadAccess;
using scai::dmemo::Distribution;
using scai::lama::CSRSparseMatrix;
using scai::lama::DenseVector;
using scai::lama::Scalar;
using scai::lama::CSRStorage;

template<typename IndexType, typename ValueType>
IndexType getFarthestLocalNode(const scai::lama::CSRSparseMatrix<ValueType> graph, std::vector<IndexType> seedNodes) {
	/**
	 * Yet another BFS. This currently has problems with unconnected graphs.
	 */
	const IndexType localN = graph.getLocalNumRows();
	const Distribution& dist = graph.getRowDistribution();

	if (seedNodes.size() == 0) return rand() % localN;

	vector<bool> visited(localN, false);
	queue<IndexType> bfsQueue;

	for (IndexType seed : seedNodes) {
		bfsQueue.push(seed);
		assert(seed >= 0 || seed < localN);
		visited[seed] = true;
	}

	const scai::lama::CSRStorage<ValueType>& storage = graph.getLocalStorage();
	ReadAccess<IndexType> ia(storage.getIA());
	ReadAccess<IndexType> ja(storage.getJA());

	IndexType nextNode = 0;
	while (bfsQueue.size() > 0) {
		nextNode = bfsQueue.front();
		bfsQueue.pop();
		visited[nextNode] = true;

		for (IndexType j = ia[nextNode]; j < ia[nextNode+1]; j++) {
			IndexType localNeighbour = dist.global2local(ja[j]);
			if (localNeighbour != nIndex && !visited[localNeighbour]) {
				bfsQueue.push(localNeighbour);
				visited[localNeighbour] = true;
			}
		}
	}

	//if nodes are unvisited, the graph is unconnected and the unvisited nodes are in fact the farthest
	for (IndexType v = 0; v < localN; v++) {
		if (!visited[v]) nextNode = v;
		break;
	}

	return nextNode;
}

template<typename IndexType, typename ValueType>
ValueType computeCut(const CSRSparseMatrix<ValueType> &input, const DenseVector<IndexType> &part, const bool weighted) {
	SCAI_REGION( "ParcoRepart.computeCut" )
	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	const scai::dmemo::DistributionPtr partDist = part.getDistributionPtr();

	const IndexType n = inputDist->getGlobalSize();
	const IndexType localN = inputDist->getLocalSize();
	const Scalar maxBlockScalar = part.max();
	const IndexType maxBlockID = maxBlockScalar.getValue<IndexType>();

    scai::dmemo::CommunicatorPtr comm = part.getDistributionPtr()->getCommunicatorPtr();
    
	std::chrono::time_point<std::chrono::system_clock> startTime =  std::chrono::system_clock::now();
     
	if( comm->getRank()==0 ){
        std::cout<<"Computing the cut..." << std::endl;
    }
    
	if (partDist->getLocalSize() != localN) {
		PRINT0("Local values mismatch for matrix and partition");
		throw std::runtime_error("partition has " + std::to_string(partDist->getLocalSize()) + " local values, but matrix has " + std::to_string(localN));
	}
	
	const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
	scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
	scai::hmemo::HArray<IndexType> localData = part.getLocalValues();
	scai::hmemo::ReadAccess<IndexType> partAccess(localData);

	scai::hmemo::ReadAccess<ValueType> values(localStorage.getValues());

	scai::dmemo::Halo partHalo = buildNeighborHalo<IndexType, ValueType>(input);
	scai::utilskernel::LArray<IndexType> haloData;
	partDist->getCommunicatorPtr()->updateHalo( haloData, localData, partHalo );

	ValueType result = 0;
	for (IndexType i = 0; i < localN; i++) {
		const IndexType beginCols = ia[i];
		const IndexType endCols = ia[i+1];
		assert(ja.size() >= endCols);

		const IndexType globalI = inputDist->local2global(i);
		assert(partDist->isLocal(globalI));
		IndexType thisBlock = partAccess[i];

		for (IndexType j = beginCols; j < endCols; j++) {
			IndexType neighbor = ja[j];
			assert(neighbor >= 0);
			assert(neighbor < n);

			IndexType neighborBlock;
			if (partDist->isLocal(neighbor)) {
				neighborBlock = partAccess[partDist->global2local(neighbor)];
			} else {
				neighborBlock = haloData[partHalo.global2halo(neighbor)];
			}

			if (neighborBlock != thisBlock) {
				if (weighted) {
					result += values[j];
				} else {
					result++;
				}
			}
		}
	}

	if (!inputDist->isReplicated()) { 
		//sum values over all processes
		result = inputDist->getCommunicatorPtr()->sum(result);
	}

	std::chrono::duration<double> endTime = std::chrono::system_clock::now() - startTime;
	double totalTime= comm->max(endTime.count() );
	if( comm->getRank()==0 ){
        std::cout<<"\t\t\t time to get the cut: " << totalTime <<  std::endl;
    }
    
	return result / 2; //counted each edge from both sides
}

//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
ValueType computeImbalance(const DenseVector<IndexType> &part, IndexType k, const DenseVector<ValueType> &nodeWeights) {
	SCAI_REGION( "ParcoRepart.computeImbalance" )
	const IndexType globalN = part.getDistributionPtr()->getGlobalSize();
	const IndexType localN = part.getDistributionPtr()->getLocalSize();
	const IndexType weightsSize = nodeWeights.getDistributionPtr()->getGlobalSize();
	const bool weighted = (weightsSize != 0);
    scai::dmemo::CommunicatorPtr comm = part.getDistributionPtr()->getCommunicatorPtr();
    
    if( comm->getRank()==0 ){
        std::cout<<"Computing the imbalance..." << std::endl;
    }
    
	ValueType minWeight, maxWeight;
	if (weighted) {
		assert(weightsSize == globalN);
		assert(nodeWeights.getDistributionPtr()->getLocalSize() == localN);
		minWeight = nodeWeights.min().Scalar::getValue<ValueType>();
		maxWeight = nodeWeights.max().Scalar::getValue<ValueType>();
	} else {
		minWeight = 1;
		maxWeight = 1;
	}

	if (maxWeight <= 0) {
		throw std::runtime_error("Node weight vector given, but all weights non-positive.");
	}

	if (minWeight < 0) {
		throw std::runtime_error("Negative node weights not supported.");
	}

	std::vector<ValueType> subsetSizes(k, 0);
	const IndexType minK = part.min().Scalar::getValue<IndexType>();
	const IndexType maxK = part.max().Scalar::getValue<IndexType>();

	if (minK < 0) {
		throw std::runtime_error("Block id " + std::to_string(minK) + " found in partition with supposedly " + std::to_string(k) + " blocks.");
	}

	if (maxK >= k) {
		throw std::runtime_error("Block id " + std::to_string(maxK) + " found in partition with supposedly " + std::to_string(k) + " blocks.");
	}

	scai::hmemo::ReadAccess<IndexType> localPart(part.getLocalValues());
	scai::hmemo::ReadAccess<ValueType> localWeight(nodeWeights.getLocalValues());
	assert(localPart.size() == localN);

	ValueType weightSum = 0.0;
	for (IndexType i = 0; i < localN; i++) {
		IndexType partID = localPart[i];
		ValueType weight = weighted ? localWeight[i] : 1;
		subsetSizes[partID] += weight;
		weightSum += weight;
	}

	ValueType optSize;
	
	if (weighted) {
		//get global weight sum
		weightSum = comm->sum(weightSum);
		//optSize = std::ceil(weightSum / k + (maxWeight - minWeight));
                optSize = std::ceil(ValueType(weightSum) / k );
	} else {
		optSize = std::ceil(ValueType(globalN) / k);
	}
        std::vector<ValueType> globalSubsetSizes(k);
	if (!part.getDistribution().isReplicated()) {
            //sum block sizes over all processes
            comm->sumImpl( globalSubsetSizes.data() , subsetSizes.data(), k, scai::common::TypeTraits<ValueType>::stype);
	}else{
            globalSubsetSizes = subsetSizes;
	}

	ValueType maxBlockSize = *std::max_element(globalSubsetSizes.begin(), globalSubsetSizes.end());

	if (!weighted) {
		assert(maxBlockSize >= optSize);
	}
	return (ValueType(maxBlockSize - optSize)/ optSize);
}
//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
scai::dmemo::Halo buildNeighborHalo(const CSRSparseMatrix<ValueType>& input) {

	SCAI_REGION( "ParcoRepart.buildPartHalo" )

	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();

	std::vector<IndexType> requiredHaloIndices = nonLocalNeighbors<IndexType, ValueType>(input);

	scai::dmemo::Halo halo;
	{
		scai::hmemo::HArrayRef<IndexType> arrRequiredIndexes( requiredHaloIndices );
		scai::dmemo::HaloBuilder::build( *inputDist, arrRequiredIndexes, halo );
	}

	return halo;
}
//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
std::vector<IndexType> nonLocalNeighbors(const CSRSparseMatrix<ValueType>& input) {
	SCAI_REGION( "ParcoRepart.nonLocalNeighbors" )
	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	const IndexType n = inputDist->getGlobalSize();
	const IndexType localN = inputDist->getLocalSize();

	const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
	scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());

	std::set<IndexType> neighborSet;

	for (IndexType i = 0; i < localN; i++) {
		const IndexType beginCols = ia[i];
		const IndexType endCols = ia[i+1];

		for (IndexType j = beginCols; j < endCols; j++) {
			IndexType neighbor = ja[j];
			assert(neighbor >= 0);
			assert(neighbor < n);

			if (!inputDist->isLocal(neighbor)) {
				neighborSet.insert(neighbor);
			}
		}
	}
	return std::vector<IndexType>(neighborSet.begin(), neighborSet.end()) ;
}
//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
inline bool hasNonLocalNeighbors(const CSRSparseMatrix<ValueType> &input, IndexType globalID) {
	SCAI_REGION( "ParcoRepart.hasNonLocalNeighbors" )

	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();

	const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
	const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());

	const IndexType localID = inputDist->global2local(globalID);
	assert(localID != nIndex);

	const IndexType beginCols = ia[localID];
	const IndexType endCols = ia[localID+1];

	for (IndexType j = beginCols; j < endCols; j++) {
		if (!inputDist->isLocal(ja[j])) {
			return true;
		}
	}
	return false;
}
//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
std::vector<IndexType> getNodesWithNonLocalNeighbors(const CSRSparseMatrix<ValueType>& input, const std::set<IndexType>& candidates) {
    SCAI_REGION( "ParcoRepart.getNodesWithNonLocalNeighbors_cache" );
    std::vector<IndexType> result;
    const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();

    const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
    const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
    const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
    const IndexType localN = inputDist->getLocalSize();

    for (IndexType globalI : candidates) {
        const IndexType localI = inputDist->global2local(globalI);
        if (localI == nIndex) {
            continue;
        }
        const IndexType beginCols = ia[localI];
        const IndexType endCols = ia[localI+1];

        //over all edges
        for (IndexType j = beginCols; j < endCols; j++) {
            if (inputDist->isLocal(ja[j]) == 0) {
                result.push_back(globalI);
                break;
            }
        }
    }

    //nodes should have been sorted to begin with, so a subset of them will be sorted as well
    std::sort(result.begin(), result.end());
    return result;
}


template<typename IndexType, typename ValueType>
std::vector<IndexType> getNodesWithNonLocalNeighbors(const CSRSparseMatrix<ValueType>& input) {
	SCAI_REGION( "ParcoRepart.getNodesWithNonLocalNeighbors" )
	std::vector<IndexType> result;

	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	if (inputDist->isReplicated()) {
		//everything is local
		return result;
	}

	const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
	const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
	const IndexType localN = inputDist->getLocalSize();

	scai::hmemo::HArray<IndexType> ownIndices;
	inputDist->getOwnedIndexes(ownIndices);
	scai::hmemo::ReadAccess<IndexType> rIndices(ownIndices);

	//iterate over all nodes
	for (IndexType localI = 0; localI < localN; localI++) {
		const IndexType beginCols = ia[localI];
		const IndexType endCols = ia[localI+1];

		//over all edges
		for (IndexType j = beginCols; j < endCols; j++) {
			if (!inputDist->isLocal(ja[j])) {
				IndexType globalI = rIndices[localI];
				result.push_back(globalI);
				break;
			}
		}
	}

	//nodes should have been sorted to begin with, so a subset of them will be sorted as well
	assert(std::is_sorted(result.begin(), result.end()));
	return result;
}
//---------------------------------------------------------------------------------------

/* The results returned is already distributed
 */
template<typename IndexType, typename ValueType>
DenseVector<IndexType> getBorderNodes( const CSRSparseMatrix<ValueType> &adjM, const DenseVector<IndexType> &part) {

    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr();
    const IndexType localN = dist->getLocalSize();
    const scai::utilskernel::LArray<IndexType>& localPart= part.getLocalValues();
    DenseVector<IndexType> border(dist,0);
    scai::utilskernel::LArray<IndexType>& localBorder= border.getLocalValues();

    IndexType globalN = dist->getGlobalSize();
    IndexType max = part.max().Scalar::getValue<IndexType>();

    if( !dist->isEqual( part.getDistribution() ) ){
        std::cout<< __FILE__<< "  "<< __LINE__<< ", matrix dist: " << *dist<< " and partition dist: "<< part.getDistribution() << std::endl;
        throw std::runtime_error( "Distributions: should (?) be equal.");
    }

    const CSRStorage<ValueType>& localStorage = adjM.getLocalStorage();
	const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
	const scai::hmemo::ReadAccess<IndexType> partAccess(localPart);

	scai::dmemo::Halo partHalo = buildNeighborHalo<IndexType, ValueType>(adjM);
	scai::utilskernel::LArray<IndexType> haloData;
	dist->getCommunicatorPtr()->updateHalo( haloData, localPart, partHalo );

    for(IndexType i=0; i<localN; i++){    // for all local nodes
    	IndexType thisBlock = localPart[i];
    	for(IndexType j=ia[i]; j<ia[i+1]; j++){                   // for all the edges of a node
    		IndexType neighbor = ja[j];
    		IndexType neighborBlock;
			if (dist->isLocal(neighbor)) {
				neighborBlock = partAccess[dist->global2local(neighbor)];
			} else {
				neighborBlock = haloData[partHalo.global2halo(neighbor)];
			}
			assert( neighborBlock < max +1 );
			if (thisBlock != neighborBlock) {
				localBorder[i] = 1;
				break;
			}
    	}
    }

    assert(border.getDistributionPtr()->getLocalSize() == localN);
    return border;
}
//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
std::pair<std::vector<IndexType>,std::vector<IndexType>> getNumBorderInnerNodes	( const CSRSparseMatrix<ValueType> &adjM, const DenseVector<IndexType> &part, const struct Settings settings) {

    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    
    if( comm->getRank()==0 ){
        std::cout<<"Computing the border and inner nodes..." << std::endl;
    }
    std::chrono::time_point<std::chrono::system_clock> startTime =  std::chrono::system_clock::now();
	
    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr();
    const IndexType localN = dist->getLocalSize();
    const scai::utilskernel::LArray<IndexType>& localPart= part.getLocalValues();

    IndexType globalN = dist->getGlobalSize();
    IndexType max = part.max().Scalar::getValue<IndexType>();
	
	if(max!=settings.numBlocks-1){
		PRINT("\n\t\tWARNING: the max block id is " << max << " but it should be " << settings.numBlocks-1);
		max = settings.numBlocks-1;
	}
    
    // the number of border nodes per block
    std::vector<IndexType> borderNodesPerBlock( max+1, 0 );
    // the number of inner nodes
    std::vector<IndexType> innerNodesPerBlock( max+1, 0 );
    
    
    if( !dist->isEqual( part.getDistribution() ) ){
        std::cout<< __FILE__<< "  "<< __LINE__<< ", matrix dist: " << *dist<< " and partition dist: "<< part.getDistribution() << std::endl;
        throw std::runtime_error( "Distributions: should (?) be equal.");
    }

    const CSRStorage<ValueType>& localStorage = adjM.getLocalStorage();
	const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
	const scai::hmemo::ReadAccess<IndexType> partAccess(localPart);

	scai::dmemo::Halo partHalo = buildNeighborHalo<IndexType, ValueType>(adjM);
	scai::utilskernel::LArray<IndexType> haloData;
	dist->getCommunicatorPtr()->updateHalo( haloData, localPart, partHalo );

    for(IndexType i=0; i<localN; i++){    // for all local nodes
    	IndexType thisBlock = localPart[i];
        SCAI_ASSERT_LE_ERROR( thisBlock , max , "Wrong block id." );
        bool isBorderNode = false;
        
    	for(IndexType j=ia[i]; j<ia[i+1]; j++){                   // for all the edges of a node
    		IndexType neighbor = ja[j];
    		IndexType neighborBlock;
			if (dist->isLocal(neighbor)) {
				neighborBlock = partAccess[dist->global2local(neighbor)];
			} else {
				neighborBlock = haloData[partHalo.global2halo(neighbor)];
			}
			SCAI_ASSERT_LE_ERROR( neighborBlock , max , "Wrong block id." );
			if (thisBlock != neighborBlock) {
                borderNodesPerBlock[thisBlock]++;   //increase number of border nodes found
                isBorderNode = true;
				break;
			}
    	}
    	//if all neighbors are in the same block then this is an inner node
        if( !isBorderNode ){
            innerNodesPerBlock[thisBlock]++; 
        }
    }

    //comm->sumArray( borderNodesPerBlock );
    //std::vector<IndexType> globalBorderNodes(max+1, 0);
    comm->sumImpl( borderNodesPerBlock.data(), borderNodesPerBlock.data(), max+1, scai::common::TypeTraits<IndexType>::stype); 
    
    //std::vector<IndexType> globalInnerNodes(max+1, 0);
    comm->sumImpl( innerNodesPerBlock.data(), innerNodesPerBlock.data(), max+1, scai::common::TypeTraits<IndexType>::stype); 
    
	std::chrono::duration<double> endTime = std::chrono::system_clock::now() - startTime;
	double totalTime= comm->max(endTime.count() );
	if( comm->getRank()==0 ){
        std::cout<<"\t\t\t time to get number of border and inner nodes : " << totalTime <<  std::endl;
    }
    
    return std::make_pair( borderNodesPerBlock, innerNodesPerBlock );
}
//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
std::vector<IndexType> computeCommVolume( const CSRSparseMatrix<ValueType> &adjM, const DenseVector<IndexType> &part, const IndexType numBlocks) {
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    
    if( comm->getRank()==0 ){
        std::cout<<"Computing the communication volume ..." << std::endl;
    }
    std::chrono::time_point<std::chrono::system_clock> startTime =  std::chrono::system_clock::now();
	
    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr();
    const IndexType localN = dist->getLocalSize();
    const scai::utilskernel::LArray<IndexType>& localPart= part.getLocalValues();

    IndexType globalN = dist->getGlobalSize();
    //IndexType max = part.max().Scalar::getValue<IndexType>();
    
    // the communication volume per block for this PE
    std::vector<IndexType> commVolumePerBlock( numBlocks+1, 0 );
    
    
    if( !dist->isEqual( part.getDistribution() ) ){
        std::cout<< __FILE__<< "  "<< __LINE__<< ", matrix dist: " << *dist<< " and partition dist: "<< part.getDistribution() << std::endl;
        throw std::runtime_error( "Distributions: should (?) be equal.");
    }

    const CSRStorage<ValueType>& localStorage = adjM.getLocalStorage();
	const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
	const scai::hmemo::ReadAccess<IndexType> partAccess(localPart);

	scai::dmemo::Halo partHalo = buildNeighborHalo<IndexType, ValueType>(adjM);
	scai::utilskernel::LArray<IndexType> haloData;
	dist->getCommunicatorPtr()->updateHalo( haloData, localPart, partHalo );

    for(IndexType i=0; i<localN; i++){    // for all local nodes
    	IndexType thisBlock = localPart[i];
        SCAI_ASSERT_LE_ERROR( thisBlock , numBlocks , "Wrong block id." );
        bool isBorderNode = false;
        std::set<IndexType> allNeighborBlocks;
        
    	for(IndexType j=ia[i]; j<ia[i+1]; j++){                   // for all the edges of a node
    		IndexType neighbor = ja[j];
    		IndexType neighborBlock;
			if (dist->isLocal(neighbor)) {
				neighborBlock = partAccess[dist->global2local(neighbor)];
			} else {
				neighborBlock = haloData[partHalo.global2halo(neighbor)];
			}
			SCAI_ASSERT_LE_ERROR( neighborBlock , numBlocks , "Wrong block id." );
            
            // found a neighbor that belongs to a different block
			if (thisBlock != neighborBlock) {
                
                typename std::set<IndexType>::iterator it = allNeighborBlocks.find( neighborBlock );
                
                if( it==allNeighborBlocks.end() ){   // this block has not been encountered before
                    allNeighborBlocks.insert( neighborBlock );
                    commVolumePerBlock[thisBlock]++;   //increase volume
                }else{
                    // if neighnor belongs to a different block but we have already found another neighbor 
                    // from that block, then do not increase volume
                }
			}
    	}
    }

    // sum local volume
    comm->sumImpl( commVolumePerBlock.data(), commVolumePerBlock.data(), numBlocks+1, scai::common::TypeTraits<IndexType>::stype); 
	
	std::chrono::duration<double> endTime = std::chrono::system_clock::now() - startTime;
	double totalTime= comm->max(endTime.count() );
	if( comm->getRank()==0 ){
        std::cout<<"\t\t\t time to get volume: " << totalTime <<  std::endl;
    }
    return commVolumePerBlock;
}

//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
std::tuple<std::vector<IndexType>, std::vector<IndexType>, std::vector<IndexType>> computeCommBndInner( 
	const CSRSparseMatrix<ValueType> &adjM, 
	const DenseVector<IndexType> &part, 
	const IndexType numBlocks) {
	
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    
    if( comm->getRank()==0 ){
        std::cout<<"Computing the communication volume, number of border and inner nodes ..." << std::endl;
    }
    std::chrono::time_point<std::chrono::system_clock> startTime =  std::chrono::system_clock::now();
	
    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr();
    const IndexType localN = dist->getLocalSize();
    const scai::utilskernel::LArray<IndexType>& localPart= part.getLocalValues();

    IndexType globalN = dist->getGlobalSize();

    // the communication volume per block for this PE
    std::vector<IndexType> commVolumePerBlock( numBlocks, 0 );
	// the number of border nodes per block
    std::vector<IndexType> borderNodesPerBlock( numBlocks, 0 );
    // the number of inner nodes
    std::vector<IndexType> innerNodesPerBlock( numBlocks, 0 );
    
    if( !dist->isEqual( part.getDistribution() ) ){
        std::cout<< __FILE__<< "  "<< __LINE__<< ", matrix dist: " << *dist<< " and partition dist: "<< part.getDistribution() << std::endl;
        throw std::runtime_error( "Distributions: should (?) be equal.");
    }

    const CSRStorage<ValueType>& localStorage = adjM.getLocalStorage();
	const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
	const scai::hmemo::ReadAccess<IndexType> partAccess(localPart);

	scai::dmemo::Halo partHalo = buildNeighborHalo<IndexType, ValueType>(adjM);
	scai::utilskernel::LArray<IndexType> haloData;
	dist->getCommunicatorPtr()->updateHalo( haloData, localPart, partHalo );

    for(IndexType i=0; i<localN; i++){    // for all local nodes
    	IndexType thisBlock = localPart[i];
        SCAI_ASSERT_LT_ERROR( thisBlock , numBlocks , "Wrong block id." );
        bool isBorderNode = false;
        std::set<IndexType> allNeighborBlocks;
        
    	for(IndexType j=ia[i]; j<ia[i+1]; j++){                   // for all the edges of a node
    		IndexType neighbor = ja[j];
    		IndexType neighborBlock;
			if (dist->isLocal(neighbor)) {
				neighborBlock = partAccess[dist->global2local(neighbor)];
			} else {
				neighborBlock = haloData[partHalo.global2halo(neighbor)];
			}
			SCAI_ASSERT_LT_ERROR( neighborBlock , numBlocks , "Wrong block id." );
            
            // found a neighbor that belongs to a different block
			if (thisBlock != neighborBlock) {
				if( not isBorderNode){
					borderNodesPerBlock[thisBlock]++;   //increase number of border nodes found
					isBorderNode = true;
				}
				
                typename std::set<IndexType>::iterator it = allNeighborBlocks.find( neighborBlock );
                
                if( it==allNeighborBlocks.end() ){   // this block has not been encountered before
                    allNeighborBlocks.insert( neighborBlock );
                    commVolumePerBlock[thisBlock]++;   //increase volume
                }else{
                    // if neighnor belongs to a different block but we have already found another neighbor 
                    // from that block, then do not increase volume
                }
			}
    	}
		//if all neighbors are in the same block then this is an inner node
		if( !isBorderNode ){
			innerNodesPerBlock[thisBlock]++; 
		}
    }

    // sum local volume
    comm->sumImpl( commVolumePerBlock.data(), commVolumePerBlock.data(), numBlocks, scai::common::TypeTraits<IndexType>::stype); 
	// sum border nodes
	comm->sumImpl( borderNodesPerBlock.data(), borderNodesPerBlock.data(), numBlocks, scai::common::TypeTraits<IndexType>::stype); 
    // sum inner nodes
    comm->sumImpl( innerNodesPerBlock.data(), innerNodesPerBlock.data(), numBlocks, scai::common::TypeTraits<IndexType>::stype); 
	
	std::chrono::duration<double> endTime = std::chrono::system_clock::now() - startTime;
	double totalTime= comm->max(endTime.count() );
	if( comm->getRank()==0 ){
        std::cout<<"\t\t\t\t time to get volume, number of border and inner nodes: " << totalTime <<  std::endl;
    }
    return std::make_tuple( std::move(commVolumePerBlock), std::move(borderNodesPerBlock), std::move(innerNodesPerBlock) );
}

//---------------------------------------------------------------------------------------

/** Get the maximum degree of a graph.
 * */
template<typename IndexType, typename ValueType>
IndexType getGraphMaxDegree( const scai::lama::CSRSparseMatrix<ValueType>& adjM){

    const scai::dmemo::DistributionPtr distPtr = adjM.getRowDistributionPtr();
    const scai::dmemo::CommunicatorPtr comm = distPtr->getCommunicatorPtr();
    const IndexType localN = distPtr->getLocalSize();
    const IndexType globalN = distPtr->getGlobalSize();
    
    {
        scai::dmemo::DistributionPtr noDist (new scai::dmemo::NoDistribution( globalN ));
        SCAI_ASSERT( adjM.getColDistributionPtr()->isEqual(*noDist) , "Adjacency matrix should have no column distribution." );
    }
    
    const scai::lama::CSRStorage<ValueType>& localStorage = adjM.getLocalStorage();
    scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
    
    // local maximum degree 
    IndexType maxDegree = ia[1]-ia[0];
    
    for(int i=1; i<ia.size(); i++){
        IndexType thisDegree = ia[i]-ia[i-1];
        if( thisDegree>maxDegree){
            maxDegree = thisDegree;
        }
    }
    //return global maximum
    return comm->max( maxDegree );
}
//------------------------------------------------------------------------------

/** Compute maximum communication= max degree of the block graph, and total communication= sum of all edges
 */
template<typename IndexType, typename ValueType>
std::pair<IndexType,IndexType> computeBlockGraphComm( const scai::lama::CSRSparseMatrix<ValueType>& adjM, const scai::lama::DenseVector<IndexType> &part, const IndexType k){

    scai::dmemo::CommunicatorPtr comm = part.getDistributionPtr()->getCommunicatorPtr();
    
    if( comm->getRank()==0 ){
        std::cout<<"Computing the block graph communication..." << std::endl;
    }
    //TODO: getting the block graph probably fails for p>5000, 
    scai::lama::CSRSparseMatrix<ValueType> blockGraph = getBlockGraph( adjM, part, k);
    
    IndexType maxComm = getGraphMaxDegree<IndexType,ValueType>( blockGraph );
    IndexType totalComm = blockGraph.getNumValues()/2;
    
    return std::make_pair(maxComm, totalComm);
}

//------------------------------------------------------------------------------

/** Returns the edges of the block graph only for the local part. Eg. if blocks 1 and 2 are local
 * in this processor it finds the edge (1,2) ( and the edge (2,1)).
 * Also if the other endpoint is in another processor it finds this edge: block 1 is local, it
 * shares an edge with block 3 that is not local, this edge is found and returned.
 *
 * @param[in] adjM The adjacency matrix of the input graph.
 * @param[in] part The partition of the input graph.
 *
 * @return A 2 dimensional vector with the edges of the local parts of the block graph:
 * edge (u,v) is (ret[0][i], ret[1][i]) if block u and block v are connected.
 */
//return: there is an edge in the block graph between blocks ret[0][i]-ret[1][i]
template<typename IndexType, typename ValueType>
std::vector<std::vector<IndexType>> getLocalBlockGraphEdges( const scai::lama::CSRSparseMatrix<ValueType> &adjM, const scai::lama::DenseVector<IndexType> &part) {
    SCAI_REGION("ParcoRepart.getLocalBlockGraphEdges");
    SCAI_REGION_START("ParcoRepart.getLocalBlockGraphEdges.initialise");
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr();
    const scai::utilskernel::LArray<IndexType>& localPart= part.getLocalValues();
    IndexType N = adjM.getNumColumns();
    IndexType max = part.max().Scalar::getValue<IndexType>();
   
    if( !dist->isEqual( part.getDistribution() ) ){
        std::cout<< __FILE__<< "  "<< __LINE__<< ", matrix dist: " << *dist<< " and partition dist: "<< part.getDistribution() << std::endl;
        throw std::runtime_error( "Distributions: should (?) be equal.");
    }
    SCAI_REGION_END("ParcoRepart.getLocalBlockGraphEdges.initialise");
    
    
    SCAI_REGION_START("ParcoRepart.getLocalBlockGraphEdges.addLocalEdge_newVersion");
    
    scai::hmemo::HArray<IndexType> nonLocalIndices( dist->getLocalSize() ); 
    scai::hmemo::WriteAccess<IndexType> writeNLI(nonLocalIndices, dist->getLocalSize() );
    IndexType actualNeighbours = 0;

    const scai::lama::CSRStorage<ValueType> localStorage = adjM.getLocalStorage();
    const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
    const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
    scai::hmemo::ReadAccess<ValueType> values(localStorage.getValues());
    
    // we do not know the size of the non-local indices that is why we use an std::vector
    // with push_back, then convert that to a DenseVector in order to call DenseVector::gather
    // TODO: skip the std::vector to DenseVector conversion. maybe use HArray or LArray
    std::vector< std::vector<IndexType> > edges(2);
    std::vector<IndexType> localInd, nonLocalInd;

    for(IndexType i=0; i<dist->getLocalSize(); i++){ 
        for(IndexType j=ia[i]; j<ia[i+1]; j++){ 
            if( dist->isLocal(ja[j]) ){ 
                IndexType u = localPart[i];         // partition(i)
                IndexType v = localPart[dist->global2local(ja[j])]; // partition(j), 0<j<N so take the local index of j
                assert( u < max +1);
                assert( v < max +1);
                if( u != v){    // the nodes belong to different blocks                  
                        bool add_edge = true;
                        for(IndexType k=0; k<edges[0].size(); k++){ //check that this edge is not already in
                            if( edges[0][k]==u && edges[1][k]==v ){
                                add_edge= false;
                                break;      // the edge (u,v) already exists
                            }
                        }
                        if( add_edge== true){       //if this edge does not exist, add it
                            edges[0].push_back(u);
                            edges[1].push_back(v);
                        }
                }
            } else{  // if(dist->isLocal(j)) 
                // there is an edge between i and j but index j is not local in the partition so we cannot get part[j].
                localInd.push_back(i);
                nonLocalInd.push_back(ja[j]);
            }
            
        }
    }
    SCAI_REGION_END("ParcoRepart.getLocalBlockGraphEdges.addLocalEdge_newVersion");
    
    // TODO: this seems to take quite a long !
    // take care of all the non-local indices found
    assert( localInd.size() == nonLocalInd.size() );
    scai::lama::DenseVector<IndexType> nonLocalDV( nonLocalInd.size(), 0 );
    scai::lama::DenseVector<IndexType> gatheredPart( nonLocalDV.size(),0 );
    
    //get a DenseVector from a vector
    for(IndexType i=0; i<nonLocalInd.size(); i++){
        nonLocalDV.setValue(i, nonLocalInd[i]);
    }
    SCAI_REGION_START("ParcoRepart.getLocalBlockGraphEdges.gatherNonLocal")
    //gather all non-local indexes
    gatheredPart.gather(part, nonLocalDV , scai::common::binary::COPY );
    SCAI_REGION_END("ParcoRepart.getLocalBlockGraphEdges.gatherNonLocal")
    
    assert( gatheredPart.size() == nonLocalInd.size() );
    assert( gatheredPart.size() == localInd.size() );
    
    for(IndexType i=0; i<gatheredPart.size(); i++){
        SCAI_REGION("ParcoRepart.getLocalBlockGraphEdges.addNonLocalEdge");
        IndexType u = localPart[ localInd[i] ];         
        IndexType v = gatheredPart.getValue(i).scai::lama::Scalar::getValue<IndexType>();
        assert( u < max +1);
        assert( v < max +1);
        if( u != v){    // the nodes belong to different blocks                  
            bool add_edge = true;
            for(IndexType k=0; k<edges[0].size(); k++){ //check that this edge is not already in
                if( edges[0][k]==u && edges[1][k]==v ){
                    add_edge= false;
                    break;      // the edge (u,v) already exists
                }
            }
            if( add_edge== true){       //if this edge does not exist, add it
                edges[0].push_back(u);
                edges[1].push_back(v);
            }
        }
    }
    return edges;
}
//-----------------------------------------------------------------------------------

/** Builds the block graph of the given partition.
 * Creates an HArray that is passed around in numPEs (=comm->getSize()) rounds and every time
 * a processor writes in the array its part.
 *
 * Not distributed.
 *
 * @param[in] adjM The adjacency matric of the input graph.
 * @param[in] part The partition of the input garph.
 * @param[in] k Number of blocks.
 *
 * @return The "adjacency matrix" of the block graph. In this version is a 1-dimensional array
 * with size k*k and [i,j]= i*k+j.
 */
template<typename IndexType, typename ValueType>
scai::lama::CSRSparseMatrix<ValueType> getBlockGraph( const scai::lama::CSRSparseMatrix<ValueType> &adjM, const scai::lama::DenseVector<IndexType> &part, const IndexType k) {
    SCAI_REGION("ParcoRepart.getBlockGraph");
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr distPtr = adjM.getRowDistributionPtr();
    const scai::utilskernel::LArray<IndexType>& localPart= part.getLocalValues();
    
    // there are k blocks in the partition so the adjecency matrix for the block graph has dimensions [k x k]
    scai::dmemo::DistributionPtr distRowBlock ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, k) );  
    scai::dmemo::DistributionPtr distColBlock ( new scai::dmemo::NoDistribution( k ));
    
    // TODO: memory costly for big k
    IndexType size= k*k;
    // get, on each processor, the edges of the blocks that are local
    std::vector< std::vector<IndexType> > blockEdges = getLocalBlockGraphEdges( adjM, part);
    assert(blockEdges[0].size() == blockEdges[1].size());
    
    scai::hmemo::HArray<IndexType> sendPart(size, static_cast<ValueType>( 0 ));
    scai::hmemo::HArray<IndexType> recvPart(size);
    
    for(IndexType round=0; round<comm->getSize(); round++){
        SCAI_REGION("ParcoRepart.getBlockGraph.shiftArray");
        {   // write your part 
            scai::hmemo::WriteAccess<IndexType> sendPartWrite( sendPart );
            for(IndexType i=0; i<blockEdges[0].size(); i++){
                IndexType u = blockEdges[0][i];
                IndexType v = blockEdges[1][i];
                sendPartWrite[ u*k + v ] = 1;
            }
        }
        comm->shiftArray(recvPart , sendPart, 1);
        sendPart.swap(recvPart);
    } 
    
    // get numEdges
    IndexType numEdges=0;
    
    scai::hmemo::ReadAccess<IndexType> recvPartRead( recvPart );
    for(IndexType i=0; i<recvPartRead.size(); i++){
        if( recvPartRead[i]>0 )
            ++numEdges;
    }
    
    //convert the k*k HArray to a [k x k] CSRSparseMatrix
    scai::lama::CSRStorage<ValueType> localMatrix;
    localMatrix.allocate( k ,k );
    
    scai::hmemo::HArray<IndexType> csrIA;
    scai::hmemo::HArray<IndexType> csrJA;
    scai::hmemo::HArray<ValueType> csrValues; 
    {
        IndexType numNZ = numEdges;     // this equals the number of edges of the graph
        scai::hmemo::WriteOnlyAccess<IndexType> ia( csrIA, k +1 );
        scai::hmemo::WriteOnlyAccess<IndexType> ja( csrJA, numNZ );
        scai::hmemo::WriteOnlyAccess<ValueType> values( csrValues, numNZ );   
        scai::hmemo::ReadAccess<IndexType> recvPartRead( recvPart );
        ia[0]= 0;
        
        IndexType rowCounter = 0; // count rows
        IndexType nnzCounter = 0; // count non-zero elements
        
        for(IndexType i=0; i<k; i++){
            IndexType rowNums=0;
            // traverse the part of the HArray that represents a row and find how many elements are in this row
            for(IndexType j=0; j<k; j++){
                if( recvPartRead[i*k+j] >0  ){
                    ++rowNums;
                }
            }
            ia[rowCounter+1] = ia[rowCounter] + rowNums;
           
            for(IndexType j=0; j<k; j++){
                if( recvPartRead[i*k +j] >0){   // there exist edge (i,j)
                    ja[nnzCounter] = j;
                    values[nnzCounter] = 1;
                    ++nnzCounter;
                }
            }
            ++rowCounter;
        }
    }
    SCAI_REGION_START("ParcoRepart.getBlockGraph.swapAndAssign");
        scai::lama::CSRSparseMatrix<ValueType> matrix;
        localMatrix.swap( csrIA, csrJA, csrValues );
        matrix.assign(localMatrix);
    SCAI_REGION_END("ParcoRepart.getBlockGraph.swapAndAssign");
    return matrix;
}
//----------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
scai::lama::CSRSparseMatrix<ValueType> getPEGraph( const scai::dmemo::Halo& halo) {
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
	scai::dmemo::DistributionPtr distPEs ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, comm->getSize()) );
	assert(distPEs->getLocalSize() == 1);
	scai::dmemo::DistributionPtr noDistPEs (new scai::dmemo::NoDistribution( comm->getSize() ));

    const scai::dmemo::CommunicationPlan& plan = halo.getProvidesPlan();
    std::vector<IndexType> neighbors;
    std::vector<ValueType> edgeCount;
    for (IndexType i = 0; i < plan.size(); i++) {
    	if (plan[i].quantity > 0) {
    		neighbors.push_back(plan[i].partitionId);
    		edgeCount.push_back(plan[i].quantity);
    	}
    }
    const IndexType numNeighbors = neighbors.size();

    SCAI_REGION_START("ParcoRepart.getPEGraph.buildMatrix");
	scai::utilskernel::LArray<IndexType> ia(2, 0, numNeighbors);
	scai::utilskernel::LArray<IndexType> ja(numNeighbors, neighbors.data());
	scai::utilskernel::LArray<ValueType> values(edgeCount.size(), edgeCount.data());
	scai::lama::CSRStorage<ValueType> myStorage(1, comm->getSize(), numNeighbors, ia, ja, values);
	SCAI_REGION_END("ParcoRepart.getPEGraph.buildMatrix");

    scai::lama::CSRSparseMatrix<ValueType> PEgraph(distPEs, noDistPEs);
    PEgraph.swapLocalStorage(myStorage);

    return PEgraph;
}
//-----------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
scai::lama::CSRSparseMatrix<ValueType> getPEGraph( const CSRSparseMatrix<ValueType> &adjM) {
    SCAI_REGION("ParcoRepart.getPEGraph");
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr(); 
    const IndexType numPEs = comm->getSize();
    
    const std::vector<IndexType> nonLocalIndices = GraphUtils::nonLocalNeighbors<IndexType, ValueType>(adjM);
    
    SCAI_REGION_START("ParcoRepart.getPEGraph.getOwners");
    scai::utilskernel::LArray<IndexType> indexTransport(nonLocalIndices.size(), nonLocalIndices.data());
    // find the PEs that own every non-local index
    scai::hmemo::HArray<IndexType> owners(nonLocalIndices.size() , -1);
    dist->computeOwners( owners, indexTransport);
    SCAI_REGION_END("ParcoRepart.getPEGraph.getOwners");
    
    scai::hmemo::ReadAccess<IndexType> rOwners(owners);
    std::vector<IndexType> neighborPEs(rOwners.get(), rOwners.get()+rOwners.size());
    rOwners.release();
    std::sort(neighborPEs.begin(), neighborPEs.end());
    //remove duplicates
    neighborPEs.erase(std::unique(neighborPEs.begin(), neighborPEs.end()), neighborPEs.end());
    const IndexType numNeighbors = neighborPEs.size();

    // create the PE adjacency matrix to be returned
    scai::dmemo::DistributionPtr distPEs ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, numPEs) );
    assert(distPEs->getLocalSize() == 1);
    scai::dmemo::DistributionPtr noDistPEs (new scai::dmemo::NoDistribution( numPEs ));

    SCAI_REGION_START("ParcoRepart.getPEGraph.buildMatrix");
    scai::utilskernel::LArray<IndexType> ia(2, 0, numNeighbors);
    scai::utilskernel::LArray<IndexType> ja(numNeighbors, neighborPEs.data());
    scai::utilskernel::LArray<ValueType> values(numNeighbors, 1);
    scai::lama::CSRStorage<ValueType> myStorage(1, numPEs, neighborPEs.size(), ia, ja, values);
    SCAI_REGION_END("ParcoRepart.getPEGraph.buildMatrix");
    
    scai::lama::CSRSparseMatrix<ValueType> PEgraph(distPEs, noDistPEs);
    PEgraph.swapLocalStorage(myStorage);

    return PEgraph;
}
//-----------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
scai::lama::CSRSparseMatrix<ValueType> getCSRmatrixFromAdjList_NoEgdeWeights( const std::vector<std::set<IndexType>>& adjList) {
    
    IndexType N = adjList.size();

    // the CSRSparseMatrix vectors
    std::vector<IndexType> ia(N+1);
    ia[0] = 0;
    std::vector<IndexType> ja;
        
    for(IndexType i=0; i<N; i++){
        std::set<IndexType> neighbors = adjList[i]; // the neighbors of this vertex
        for( typename std::set<IndexType>::iterator it=neighbors.begin(); it!=neighbors.end(); it++){
            ja.push_back( *it );
//PRINT(i << " -- "<< *it);
        }
        ia[i+1] = ia[i]+neighbors.size();
    }
    
    std::vector<IndexType> values(ja.size(), 1);
    
    scai::lama::CSRStorage<ValueType> myStorage( N, N, ja.size(), 
            scai::utilskernel::LArray<IndexType>(ia.size(), ia.data()),
            scai::utilskernel::LArray<IndexType>(ja.size(), ja.data()),
            scai::utilskernel::LArray<ValueType>(values.size(), values.data())
    );
    
    return scai::lama::CSRSparseMatrix<ValueType>(myStorage);
}
//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
scai::lama::DenseVector<IndexType> getDegreeVector( const scai::lama::CSRSparseMatrix<ValueType>& adjM){
    SCAI_REGION("GraphUtils.getDegreeVector");
    
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr distPtr = adjM.getRowDistributionPtr();
    const IndexType localN = distPtr->getLocalSize();
    
    scai::lama::DenseVector<IndexType> degreeVector(distPtr);
    scai::utilskernel::LArray<IndexType>& localDegreeVector = degreeVector.getLocalValues();
    
    const scai::lama::CSRStorage<ValueType> localAdjM = adjM.getLocalStorage();
    {
        const scai::hmemo::ReadAccess<IndexType> readIA ( localAdjM.getIA() );
        scai::hmemo::WriteOnlyAccess<IndexType> writeVector( localDegreeVector, localDegreeVector.size()) ;
        
        SCAI_ASSERT_EQ_ERROR(readIA.size(), localDegreeVector.size()+1, "Probably wrong distribution");
        
        for(IndexType i=0; i<readIA.size()-1; i++){
            writeVector[i] = readIA[i+1] - readIA[i];
        }
    }
    
    return degreeVector;
}

//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
scai::lama::CSRSparseMatrix<ValueType> getLaplacian( const scai::lama::CSRSparseMatrix<ValueType>& adjM){
    SCAI_REGION("GraphUtils.getLaplacian");
    
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr distPtr = adjM.getRowDistributionPtr();
    
    const IndexType globalN = distPtr->getGlobalSize();
    const IndexType localN = distPtr->getLocalSize();
    
    const CSRStorage<ValueType>& localStorage = adjM.getLocalStorage();
    
    // distributed vector of size globalN with the degree for every edge. It has the same distribution as the rowDistribution of adjM
    scai::lama::DenseVector<IndexType> degreeVector = getDegreeVector<IndexType,ValueType>( adjM );
    SCAI_ASSERT( degreeVector.size() == globalN, "Degree vector global size not correct: " << degreeVector.size() << " , shoulb be " << globalN);
    SCAI_ASSERT( degreeVector.getLocalValues().size() == localN,"Degree vector local size not correct: " << degreeVector.getLocalValues().size() << " , shoulb be " << localN);
    
    // data of the output graph
    scai::hmemo::HArray<IndexType> laplacianIA;
    scai::hmemo::HArray<IndexType> laplacianJA;
    scai::hmemo::HArray<ValueType> laplacianValues;
    
    IndexType laplacianNnzValues;
    {        
        // get local data of adjM
        const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
        const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
        const scai::hmemo::ReadAccess<ValueType> values(localStorage.getValues());
        
        // local data of degree vector
        scai::hmemo::ReadAccess<IndexType>  rLocalDegree( degreeVector.getLocalValues() );
        assert( degreeVector.getLocalValues().size() == localN );

        laplacianNnzValues = values.size() + localN;    // add one element per node/row
        
        // data of laplacian graph. laplacian and input are of the same size globalN x globalN
        scai::hmemo::WriteOnlyAccess<IndexType> wLaplacianIA( laplacianIA , ia.size() );  
        scai::hmemo::WriteOnlyAccess<IndexType> wLaplacianJA( laplacianJA , laplacianNnzValues );
        scai::hmemo::WriteOnlyAccess<ValueType> wLaplacianValues( laplacianValues, laplacianNnzValues );
        
        IndexType nnzCounter = 0;
        for(IndexType i=0; i<localN; i++){
            const IndexType beginCols = ia[i];
            const IndexType endCols = ia[i+1];
            assert(ja.size() >= endCols);
            
            IndexType globalI = distPtr->local2global(i);
            IndexType j = beginCols;
            
            // the index and value of the diagonal element to be set at the end for every row
            IndexType diagonalIndex=0;
            ValueType diagonalValue=0;
            
            while( ja[j]< globalI and j<endCols){     //bot-left part of matrix, before diagonal
                assert(ja[j] >= 0);
                assert(ja[j] < globalN);
                
                wLaplacianJA[nnzCounter] = ja[j];          // same indices
                wLaplacianValues[nnzCounter] = -values[j]; // opposite values
                diagonalValue += values[j];
                ++nnzCounter;
                assert( nnzCounter < laplacianNnzValues+1);
                ++j;
            }
            // out of while, must insert diagonal element that is the sum of the edges
            wLaplacianJA[nnzCounter] = globalI;
            assert( i < rLocalDegree.size() );
            wLaplacianValues[nnzCounter] = rLocalDegree[i];
            diagonalIndex = nnzCounter;       
            ++nnzCounter;
            
            // copy the rest of the row
            while( j<endCols){
                wLaplacianJA[nnzCounter] = ja[j];          // same indices
                wLaplacianValues[nnzCounter] = -values[j]; // opposite values
                diagonalValue += values[j];
                ++nnzCounter;
                assert( nnzCounter < laplacianNnzValues+1);
                ++j;
            }
            wLaplacianValues[ diagonalIndex ] = diagonalValue;
        }
        
        //fix ia array , we just added 1 element in every row, so...
        for(IndexType i=0; i<ia.size(); i++){
            wLaplacianIA[i] = ia[i] + i;
        }

    }
    
    SCAI_ASSERT_EQ_ERROR(laplacianJA.size(), laplacianValues.size(), "Wrong sizes." );
    {
        scai::hmemo::ReadAccess<IndexType> rLaplacianIA( laplacianIA );
        scai::hmemo::ReadAccess<IndexType> rLaplacianJA( laplacianJA );
        scai::hmemo::ReadAccess<ValueType> rLaplacianValues( laplacianValues );
        
        SCAI_ASSERT_EQ_ERROR(rLaplacianIA[ rLaplacianIA.size()-1] , laplacianJA.size(), "Wrong sizes." );
    }
    
    scai::lama::CSRStorage<ValueType> resultStorage( localN, globalN, laplacianNnzValues, laplacianIA, laplacianJA, laplacianValues);
    
    scai::lama::CSRSparseMatrix<ValueType> result(adjM.getRowDistributionPtr() , adjM.getColDistributionPtr() );
    result.swapLocalStorage( resultStorage );
    
    return result;

}


//------------------------------------------------------------------------------


template<typename IndexType, typename ValueType>
scai::lama::CSRSparseMatrix<ValueType> edgeList2CSR( std::vector< std::pair<IndexType, IndexType>> &edgeList ){

    const scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
	const IndexType thisPE = comm->getRank();
	IndexType localM = edgeList.size();
		
    int typesize;
	MPI_Type_size(SortingDatatype<sort_pair>::getMPIDatatype(), &typesize);
	assert(typesize == sizeof(sort_pair));
	
	//-------------------------------------------------------------------
	//
	// add edges to the local_pairs vector for sorting
	//
	
	//TODO: not filling with dummy values, each localPairs can have different sizes
	std::vector<sort_pair> localPairs(localM*2);
	
	//TODO: if nothing better comes up, duplicate and reverse all edges before SortingDatatype
	//		to ensure matrix will be symmetric
	
	IndexType maxLocalVertex=0;
	IndexType minLocalVertex=std::numeric_limits<IndexType>::max();
	
	for(IndexType i=0; i<localM; i++){
		IndexType v1 = edgeList[i].first;
		IndexType v2 = edgeList[i].second;
		localPairs[2*i].value = v1;
		localPairs[2*i].index = v2;
		
		//insert also reversed edge to keep matric symmetric
		localPairs[2*i+1].value = v2;
		localPairs[2*i+1].index = v1;
		
		IndexType minV = std::min(v1,v2);
		IndexType maxV = std::max(v1,v2);
		
		if( minV<minLocalVertex ){
			minLocalVertex = minV;
		}
		if( maxV>maxLocalVertex ){
			maxLocalVertex = maxV;
		}
	}
	//PRINT(thisPE << ": vertices range from "<< minLocalVertex << " to " << maxLocalVertex);
	
	const IndexType N = comm->max( maxLocalVertex );
	localM *=2 ;	// for the duplicated edges
	
	//
	// globally sort edges
	//
    std::chrono::time_point<std::chrono::system_clock> beforeSort =  std::chrono::system_clock::now();
    MPI_Comm mpi_comm = MPI_COMM_WORLD;
	SQuick::sort<sort_pair>(mpi_comm, localPairs, -1);
	
	std::chrono::duration<double> sortTmpTime = std::chrono::system_clock::now() - beforeSort;
	ValueType sortTime = comm->max( sortTime );
	PRINT0("time to sort edges: " << sortTime);
	
	//PRINT(thisPE << ": "<< localPairs.back().value << " - " << localPairs.back().index << " in total " <<  localPairs.size() );
	
	//-------------------------------------------------------------------
	//
	// communicate so each PE have all the edges of the last node
	// each PE just collect the edges of it last node and sends them to its +1 neighbor
	//
	
	// get vertex with max local id
	IndexType newMaxLocalVertex = localPairs.back().value;
	
	//TODO: communicate first to see if you need to send. now, just send to your +1 the your last vertex
	// store the edges you must send
	std::vector<IndexType> sendEdgeList;
	
	IndexType numEdgesToRemove = 0;
	for( std::vector<sort_pair>::reverse_iterator edgeIt = localPairs.rbegin(); edgeIt->value==newMaxLocalVertex; ++edgeIt){
		sendEdgeList.push_back( edgeIt->value); //Caution: This is an implicit conversion from double to int
		sendEdgeList.push_back( edgeIt->index);
		++numEdgesToRemove;
	}
	
	if( thisPE!= comm->getSize()-1){
		for( int i=0; i<numEdgesToRemove; i++ ){
			localPairs.pop_back();
		}
	}

    // make communication plan
    std::vector<IndexType> quantities(comm->getSize(), 0);
		
	if( thisPE==comm->getSize()-1 ){	//the last PE will only receive
		// do nothing, quantities is 0 for all
	}else{
		quantities[thisPE+1] = sendEdgeList.size();		// will only send to your +1 neighbor
	}
	
	scai::dmemo::CommunicationPlan sendPlan( quantities.data(), comm->getSize() );
	
	scai::dmemo::CommunicationPlan recvPlan;
	recvPlan.allocateTranspose( sendPlan, *comm );
	
	scai::utilskernel::LArray<IndexType> recvEdges;		// the edges to be received
	IndexType recvEdgesSize = recvPlan.totalQuantity();
	//PRINT(thisPE <<": received  " << recvEdgesSize << " edges");

	{
		scai::hmemo::WriteOnlyAccess<IndexType> recvVals( recvEdges, recvEdgesSize );
		comm->exchangeByPlan( recvVals.get(), recvPlan, sendEdgeList.data(), sendPlan );
	}

	const IndexType minLocalVertexBeforeInsertion = localPairs.front().value;
	SCAI_ASSERT_LE_ERROR(minLocalVertexBeforeInsertion - recvEdges[0], 1, "Gap too high between received edges and beginning of own.");

	// insert all the received edges to your local edges
	for( IndexType i=0; i<recvEdgesSize; i+=2){
		sort_pair sp;
		sp.value = recvEdges[i];
		sp.index = recvEdges[i+1];
		localPairs.insert( localPairs.begin(), sp);//this is horribly expensive! Will move the entire list of local edges with each insertion!
		//PRINT( thisPE << ": recved edge: "<< recvEdges[i] << " - " << recvEdges[i+1] );
	}

	IndexType numEdges = localPairs.size() ;
	
	SCAI_ASSERT_ERROR(std::is_sorted(localPairs.begin(), localPairs.end()), "Disorder after insertion of received edges." );

	//
	//remove duplicates
	//
	localPairs.erase(unique(localPairs.begin(), localPairs.end(), [](sort_pair p1, sort_pair p2) {
		return ( (p1.index==p2.index) and (p1.value==p2.value)); 	}), localPairs.end() );
	//PRINT( thisPE <<": removed " << numEdges - localPairs.size() << " duplicate edges" );

	//
	// check that all is correct
	//
	newMaxLocalVertex = localPairs.back().value;
	IndexType newMinLocalVertex = localPairs[0].value;
	IndexType checkSum = newMaxLocalVertex - newMinLocalVertex;
	IndexType globCheckSum = comm->sum( checkSum ) + comm->getSize() -1;

	SCAI_ASSERT_EQ_ERROR( globCheckSum, N , "Checksum mismatch, maybe some node id missing." );
	
	//PRINT( *comm << ": from "<< newMinLocalVertex << " to " << newMaxLocalVertex );
	
	localM = localPairs.size();					// after sorting, exchange and removing duplicates
	
	IndexType localN = newMaxLocalVertex-newMinLocalVertex+1;	
	IndexType globalN = comm->sum( localN );	
	IndexType globalM = comm->sum( localM );
	//PRINT(thisPE << ": N: localN, global= " << localN << ", " << globalN << ", \tM: local, global= " << localM  << ", " << globalM );

	//
	// create local indices and general distribution
	//
	scai::hmemo::HArray<IndexType> localIndices( localN , -1);
	IndexType index = 1;
	
	{
		scai::hmemo::WriteOnlyAccess<IndexType> wLocalIndices(localIndices);
		IndexType oldVertex = localPairs[0].value;
		wLocalIndices[0] = oldVertex;
		
		// go through all local edges and add a local index if it is not already added
		for(IndexType i=1; i<localPairs.size(); i++){
			IndexType newVertex = localPairs[i].value;
			if( newVertex!=wLocalIndices[index-1] ){
				wLocalIndices[index++] = newVertex;	
				SCAI_ASSERT_LE_ERROR( index, localN,"Too large index for localIndices array.");
			}
			// newVertex-oldVertex should be either 0 or 1, either are the same or differ by 1
			SCAI_ASSERT_LE_ERROR( newVertex-oldVertex, 1, "Vertex with id " << newVertex-1 <<" is missing. Error in edge list, vertex should be contunious");
			oldVertex = newVertex;
		}
		SCAI_ASSERT_NE_ERROR( wLocalIndices[localN-1], -1, "localIndices array not full");
	}
	
	const scai::dmemo::DistributionPtr genDist(new scai::dmemo::GeneralDistribution(globalN, localIndices, comm));
	
	//-------------------------------------------------------------------
	//
	// turn the local edge list to a CSRSparseMatrix
	//
	
	// the CSRSparseMatrix vectors
    std::vector<IndexType> ia(localN+1);
    ia[0] = 0;
	index = 0;
    std::vector<IndexType> ja;
	
	for( IndexType e=0; e<localM; ){
		IndexType v1 = localPairs[e].value;		//the vertices of this edge
		IndexType v1Degree = 0;
		// for all edges of v1
		for( std::vector<sort_pair>::iterator edgeIt = localPairs.begin()+e; edgeIt->value==v1 and edgeIt!=localPairs.end(); ++edgeIt){
			ja.push_back( edgeIt->index );	// the neighbor of v1
			//PRINT( thisPE << ": " << v1 << " -- " << 	edgeIt->index );
			++v1Degree;
			++e;
		}
		index++;
		//TODO: can remove the assertion if we do not initialise ia and use push_back
		SCAI_ASSERT_LE_ERROR( index, localN, thisPE << ": Wrong ia size and localN.");
		ia[index] = ia[index-1] + v1Degree;
	}
	SCAI_ASSERT_EQ_ERROR( ja.size(), localM, thisPE << ": Wrong ja size and localM.");
	std::vector<IndexType> values(ja.size(), 1);
	
	//assign/assemble the matrix
    scai::lama::CSRStorage<ValueType> myStorage ( localN, globalN, ja.size(), 
			scai::utilskernel::LArray<IndexType>(ia.size(), ia.data()),
    		scai::utilskernel::LArray<IndexType>(ja.size(), ja.data()),
    		scai::utilskernel::LArray<ValueType>(values.size(), values.data()));
	
	const scai::dmemo::DistributionPtr dist(new scai::dmemo::BlockDistribution(globalN, comm));
    const scai::dmemo::DistributionPtr noDist(new scai::dmemo::NoDistribution( globalN ));
	
	return scai::lama::CSRSparseMatrix<ValueType>(myStorage, genDist, noDist);
	
}

//-----------------------------------------------------------------------------------


template IndexType getFarthestLocalNode(const CSRSparseMatrix<ValueType> graph, std::vector<IndexType> seedNodes);
template ValueType computeCut(const CSRSparseMatrix<ValueType> &input, const DenseVector<IndexType> &part, bool weighted);
template ValueType computeImbalance(const DenseVector<IndexType> &part, IndexType k, const DenseVector<ValueType> &nodeWeights);
template scai::dmemo::Halo buildNeighborHalo<IndexType,ValueType>(const CSRSparseMatrix<ValueType> &input);
template bool hasNonLocalNeighbors(const CSRSparseMatrix<ValueType> &input, IndexType globalID);
template std::vector<IndexType> getNodesWithNonLocalNeighbors(const CSRSparseMatrix<ValueType>& input);
template std::vector<IndexType> getNodesWithNonLocalNeighbors(const CSRSparseMatrix<ValueType>& input, const std::set<IndexType>& candidates);
template std::vector<IndexType> nonLocalNeighbors(const CSRSparseMatrix<ValueType>& input);
template DenseVector<IndexType> getBorderNodes( const CSRSparseMatrix<ValueType> &adjM, const DenseVector<IndexType> &part);
template std::pair<std::vector<IndexType>,std::vector<IndexType>> getNumBorderInnerNodes( const CSRSparseMatrix<ValueType> &adjM, const DenseVector<IndexType> &part, const struct Settings settings);
template std::tuple<std::vector<IndexType>, std::vector<IndexType>, std::vector<IndexType>> computeCommBndInner( const scai::lama::CSRSparseMatrix<ValueType> &adjM, const scai::lama::DenseVector<IndexType> &part, const IndexType numBlocks);
template std::vector<IndexType> computeCommVolume( const CSRSparseMatrix<ValueType> &adjM, const DenseVector<IndexType> &part, const IndexType k);
template std::vector<std::vector<IndexType>> getLocalBlockGraphEdges( const scai::lama::CSRSparseMatrix<ValueType> &adjM, const scai::lama::DenseVector<IndexType> &part);
template scai::lama::CSRSparseMatrix<ValueType> getBlockGraph( const scai::lama::CSRSparseMatrix<ValueType> &adjM, const scai::lama::DenseVector<IndexType> &part, const IndexType k);
template IndexType getGraphMaxDegree( const scai::lama::CSRSparseMatrix<ValueType>& adjM);
template  std::pair<IndexType,IndexType> computeBlockGraphComm( const scai::lama::CSRSparseMatrix<ValueType>& adjM, const scai::lama::DenseVector<IndexType> &part, const IndexType k);
template scai::lama::CSRSparseMatrix<ValueType> getPEGraph<IndexType,ValueType>( const scai::lama::CSRSparseMatrix<ValueType> &adjM);
template scai::lama::CSRSparseMatrix<ValueType> getCSRmatrixFromAdjList_NoEgdeWeights( const std::vector<std::set<IndexType>> &adjList);
template scai::lama::CSRSparseMatrix<ValueType> edgeList2CSR( std::vector< std::pair<IndexType, IndexType>> &edgeList );
template scai::lama::CSRSparseMatrix<ValueType> getLaplacian<IndexType,ValueType>( const scai::lama::CSRSparseMatrix<ValueType>& adjM);
template scai::lama::DenseVector<IndexType> getDegreeVector( const scai::lama::CSRSparseMatrix<ValueType>& adjM);

} /*namespace GraphUtils*/

} /* namespace ITI */
