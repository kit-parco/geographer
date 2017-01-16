/*
 * ParcoReport.cpp
 *
 *  Created on: 25.10.2016
 *      Author: moritzl
 */

#include <scai/dmemo/HaloBuilder.hpp>
#include <scai/dmemo/Distribution.hpp>
#include <scai/tracing.hpp>

#include <assert.h>
#include <cmath>
#include <climits>
#include <queue>
#include <string>

#include "PrioQueue.h"
#include "ParcoRepart.h"
#include "HilbertCurve.h"

namespace ITI {

template<typename IndexType, typename ValueType>
ValueType ParcoRepart<IndexType, ValueType>::getMinimumNeighbourDistance(const CSRSparseMatrix<ValueType> &input, const std::vector<DenseVector<ValueType>> &coordinates, IndexType dimensions) {
	// iterate through matrix to find closest neighbors, implying necessary recursion depth for space-filling curve
	// here it can happen that the closest neighbor is not stored on this processor.

	SCAI_REGION( "ParcoRepart.minimumNeighborDistance" )

    std::vector<scai::dmemo::DistributionPtr> coordDist(dimensions);
    for(IndexType i=0; i<dimensions; i++){
        coordDist[i] = coordinates[i].getDistributionPtr(); 
    }
	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	const IndexType localN = inputDist->getLocalSize();

	if (!input.getColDistributionPtr()->isReplicated()) {
		throw std::runtime_error("Column of input matrix must be replicated.");
	}

	const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
    std::vector<scai::utilskernel::LArray<ValueType>> localPartOfCoords(dimensions);
    for(IndexType i=0; i<dimensions; i++){
        localPartOfCoords[i] = coordinates[i].getLocalValues();
        if (localPartOfCoords[i].size() != localN) {
        	throw std::runtime_error("Local part of coordinate vector "+ std::to_string(i) + " has size " + std::to_string(localPartOfCoords[i].size()) 
        		+ ", but localN is " + std::to_string(localN));
        }
    }
        
	const scai::utilskernel::LArray<IndexType>& ia = localStorage.getIA();
    const scai::utilskernel::LArray<IndexType>& ja = localStorage.getJA();
    assert(ia.size() == localN+1);

    ValueType minDistanceSquared = std::numeric_limits<ValueType>::max();
	for (IndexType i = 0; i < localN; i++) {
		const IndexType beginCols = ia[i];
		const IndexType endCols = ia[i+1];
		assert(ja.size() >= endCols);
		for (IndexType j = beginCols; j < endCols; j++) {
			IndexType neighbor = ja[j];
			const IndexType globalI = inputDist->local2global(i);
                        // just check coordDist[0]. Is is enough? If coord[0] is here so are the others.
			if (neighbor != globalI && coordDist[0]->isLocal(neighbor)) {
				const IndexType localNeighbor = coordDist[0]->global2local(neighbor);
				ValueType distanceSquared = 0;
				for (IndexType dim = 0; dim < dimensions; dim++) {
					ValueType diff = localPartOfCoords[dim][i] -localPartOfCoords[dim][localNeighbor];
					distanceSquared += diff*diff;
				}
				if (distanceSquared < minDistanceSquared) minDistanceSquared = distanceSquared;
			}
		}
	}

	const ValueType minDistance = std::sqrt(minDistanceSquared);
	return minDistance;
}

template<typename IndexType, typename ValueType>
DenseVector<IndexType> ParcoRepart<IndexType, ValueType>::partitionGraph(CSRSparseMatrix<ValueType> &input, std::vector<DenseVector<ValueType>> &coordinates, IndexType k,  double epsilon)
{
	SCAI_REGION( "ParcoRepart.partitionGraph" )
	/**
	* check input arguments for sanity
	*/
	IndexType n = input.getNumRows();
	if (n != coordinates[0].size()) {
		throw std::runtime_error("Matrix has " + std::to_string(n) + " rows, but " + std::to_string(coordinates[0].size())
		 + " coordinates are given.");
	}
        
	if (n != input.getNumColumns()) {
		throw std::runtime_error("Matrix must be quadratic.");
	}

	if (!input.isConsistent()) {
		throw std::runtime_error("Input matrix inconsistent");
	}

	if (k > n) {
		throw std::runtime_error("Creating " + std::to_string(k) + " blocks from " + std::to_string(n) + " elements is impossible.");
	}

	if (epsilon < 0) {
		throw std::runtime_error("Epsilon " + std::to_string(epsilon) + " is invalid.");
	}

	const IndexType dimensions = coordinates.size();
        
	const scai::dmemo::DistributionPtr coordDist = coordinates[0].getDistributionPtr();
	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	const scai::dmemo::CommunicatorPtr comm = coordDist->getCommunicatorPtr();

	const IndexType localN = inputDist->getLocalSize();
	const IndexType globalN = inputDist->getGlobalSize();

	if (coordDist->getLocalSize() != localN) {
		throw std::runtime_error(std::to_string(coordDist->getLocalSize()) + " point coordinates, "
				+ std::to_string(localN) + " rows present.");
	}

	if( !coordDist->isEqual( *inputDist) ){
		throw std::runtime_error( "Distributions should be equal.");
	}
	
	/**
	*	gather information for space-filling curves
	*/
	std::vector<ValueType> minCoords(dimensions, std::numeric_limits<ValueType>::max());
	std::vector<ValueType> maxCoords(dimensions, std::numeric_limits<ValueType>::lowest());
	
	/**
	 * get minimum / maximum of local coordinates
	 */
	for (IndexType dim = 0; dim < dimensions; dim++) {
		//get local parts of coordinates
		scai::utilskernel::LArray<ValueType>& localPartOfCoords = coordinates[dim].getLocalValues();
		for (IndexType i = 0; i < localN; i++) {
			ValueType coord = localPartOfCoords[i];
			if (coord < minCoords[dim]) minCoords[dim] = coord;
			if (coord > maxCoords[dim]) maxCoords[dim] = coord;
		}
	}

	/**
	 * communicate to get global min / max
	 */
	for (IndexType dim = 0; dim < dimensions; dim++) {
		minCoords[dim] = comm->min(minCoords[dim]);
		maxCoords[dim] = comm->max(maxCoords[dim]);
	}
        
	ValueType maxExtent = 0;
	for (IndexType dim = 0; dim < dimensions; dim++) {
		if (maxCoords[dim] - minCoords[dim] > maxExtent) {
			maxExtent = maxCoords[dim] - minCoords[dim];
		}
	}

	/**
	* Several possibilities exist for choosing the recursion depth.
	* Either by user choice, or by the maximum fitting into the datatype, or by the minimum distance between adjacent points.
	*/
	const IndexType recursionDepth = std::log2(n);

	/**
	*	create space filling curve indices.
	*/
	scai::lama::DenseVector<ValueType> hilbertIndices(inputDist);
	for (IndexType i = 0; i < localN; i++) {
		IndexType globalIndex = inputDist->local2global(i);
		ValueType globalHilbertIndex = HilbertCurve<IndexType, ValueType>::getHilbertIndex(coordinates, dimensions, globalIndex, recursionDepth, minCoords, maxCoords);
		hilbertIndices.setValue(globalIndex, globalHilbertIndex);              
	}

	/**
	* now sort the global indices by where they are on the space-filling curve.
	*/

	scai::lama::DenseVector<IndexType> permutation, inversePermutation;
	hilbertIndices.sort(permutation, true);
	//permutation.redistribute(inputDist);
	DenseVector<IndexType> tmpPerm = permutation;
	tmpPerm.sort( inversePermutation, true);

	/**
	 * The permutations given by DenseVector.sort are distributed by BlockDistributions.
	 * However, the sorting does no guarantee that each processor has the same number of values.
	 * Without a redistribution step, the line result.getLocalValues()[i] = int( inversePermutation.getLocalValues()[i] *k/n);
	 * sometimes segfaults. We can't have that.
	 */
	inversePermutation.redistribute(inputDist);
	assert(inversePermutation.getDistributionPtr()->getLocalSize() == localN);

	/**
	* initial partitioning with sfc. Upgrade to chains-on-chains-partitioning later
	*/
	DenseVector<IndexType> result(inputDist);
        
	for (IndexType i = 0; i < localN; i++) {
		result.getLocalValues()[i] = int( inversePermutation.getLocalValues()[i] *k/n);
	}
   
	if (true) {
		ValueType gain = 1;
		ValueType cut = computeCut(input, result);

		while (gain > 0) {
			if (inputDist->isReplicated()) {
				gain = replicatedMultiWayFM(input, result, k, epsilon);
			} else {
				//redistribute input matrix to partition
				scai::utilskernel::LArray<IndexType> owners(globalN);
				for (IndexType i = 0; i < globalN; i++) {
					Scalar blockID = result.getValue(i);
					owners[i] = blockID.getValue<IndexType>();
				}
				scai::dmemo::DistributionPtr newDistribution(new scai::dmemo::GeneralDistribution(owners, comm));
                                /* // sorry for that but it throws an assertion error here at my test "ParcoRepartTest. testBorders_Distributed". Could not locate it yet!
                                 
                                std::cout<<__FILE__<< "  "<< __LINE__<< " __"<< *comm << " , input.colDist: "<< input.getColDistribution() << "\n input.rowDist: "<< input.getRowDistribution() << "\n newDist: "<< *newDistribution << "\n owners.size= " << owners.size()<< std::endl;  		*/		                                
				input.redistribute(newDistribution, input.getColDistributionPtr());
				result.redistribute(newDistribution);
				gain = distributedFMStep(input, result, k, epsilon);
			}
			ValueType oldCut = cut;
			cut = computeCut(input, result);
			assert(oldCut - gain == cut);
			std::cout << "Last FM round yielded gain of " << gain << ", for total cut of " << computeCut(input, result) << std::endl;
		}
	}
	return result;
}

template<typename IndexType, typename ValueType>
ValueType ParcoRepart<IndexType, ValueType>::replicatedMultiWayFM(const CSRSparseMatrix<ValueType> &input, DenseVector<IndexType> &part, IndexType k, ValueType epsilon, bool unweighted) {

	SCAI_REGION( "ParcoRepart.replicatedMultiWayFM" )
	const IndexType n = input.getNumRows();
	/**
	* check input and throw errors
	*/
	const Scalar minPartID = part.min();
	const Scalar maxPartID = part.max();
	if (minPartID.getValue<IndexType>() != 0) {
		throw std::runtime_error("Smallest block ID is " + std::to_string(minPartID.getValue<IndexType>()) + ", should be 0");
	}

	if (maxPartID.getValue<IndexType>() != k-1) {
		throw std::runtime_error("Highest block ID is " + std::to_string(maxPartID.getValue<IndexType>()) + ", should be " + std::to_string(k-1));
	}

	if (part.size() != n) {
		throw std::runtime_error("Partition has " + std::to_string(part.size()) + " entries, but matrix has " + std::to_string(n) + ".");
	}

	if (epsilon < 0) {
		throw std::runtime_error("Epsilon must be >= 0, not " + std::to_string(epsilon));
	}

	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	const scai::dmemo::DistributionPtr partDist = part.getDistributionPtr();

	if (!inputDist->isReplicated()) {
		throw std::runtime_error("Input matrix must be replicated, for now.");
	}

	if (!partDist->isReplicated()) {
		throw std::runtime_error("Input partition must be replicated, for now.");
	}

	if (k == 1) {
		//nothing to partition
		return 0;
	}

	/**
	* allocate data structures
	*/

	//const ValueType oldCut = computeCut(input, part, unweighted);

	const IndexType optSize = ceil(double(n) / k);
	const IndexType maxAllowablePartSize = optSize*(1+epsilon);

	std::vector<IndexType> bestTargetFragment(n);
	std::vector<PrioQueue<ValueType, IndexType>> queues(k, n);

	std::vector<IndexType> gains;
	std::vector<std::pair<IndexType, IndexType> > transfers;
	std::vector<IndexType> transferedVertices;
	std::vector<double> imbalances;

	std::vector<double> fragmentSizes(k);

	double maxFragmentSize = 0;

	for (IndexType i = 0; i < n; i++) {
		Scalar partID = part.getValue(i);
		assert(partID.getValue<IndexType>() >= 0);
		assert(partID.getValue<IndexType>() < k);
		fragmentSizes[partID.getValue<IndexType>()] += 1;

		if (fragmentSizes[partID.getValue<IndexType>()] < maxFragmentSize) {
			maxFragmentSize = fragmentSizes[partID.getValue<IndexType>()];
		}
	}

	std::vector<IndexType> degrees(n);
	std::vector<std::vector<ValueType> > edgeCuts(n);

	//Using ReadAccess here didn't give a performance benefit
	const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
	const scai::utilskernel::LArray<IndexType>& ia = localStorage.getIA();
	const scai::utilskernel::LArray<IndexType>& ja = localStorage.getJA();
	const scai::utilskernel::LArray<IndexType>& values = localStorage.getValues();
	if (!unweighted && values.min() < 0) {
		throw std::runtime_error("Only positive edge weights are supported, " + std::to_string(values.min()) + " invalid.");
	}

	ValueType totalWeight = 0;

	for (IndexType v = 0; v < n; v++) {
		edgeCuts[v].resize(k, 0);

		const IndexType beginCols = ia[v];
		const IndexType endCols = ia[v+1];
		degrees[v] = endCols - beginCols;
		for (IndexType j = beginCols; j < endCols; j++) {
			IndexType neighbor = ja[j];
			if (neighbor == v) continue;
			Scalar partID = part.getValue(neighbor);
			edgeCuts[v][partID.getValue<IndexType>()] += unweighted ? 1 : values[j];
			totalWeight += unweighted ? 1 : values[j];
		}
	}

	//setting initial best target for each node
	for (IndexType v = 0; v < n; v++) {
		ValueType maxCut = -totalWeight;
		IndexType idAtMax = k;
		Scalar partID = part.getValue(v);

		for (IndexType fragment = 0; fragment < k; fragment++) {
			if (unweighted) {
				assert(edgeCuts[v][fragment] <= degrees[v]);
			}
			assert(edgeCuts[v][fragment] >= 0);

			if (fragment != partID.getValue<IndexType>() && edgeCuts[v][fragment] > maxCut && fragmentSizes[fragment] <= maxAllowablePartSize) {
				idAtMax = fragment;
				maxCut = edgeCuts[v][fragment];
			}
		}

		assert(idAtMax < k);
		assert(maxCut >= 0);
		if (unweighted) assert(maxCut <= degrees[v]);
		bestTargetFragment[v] = idAtMax;
		assert(partID.getValue<IndexType>() < queues.size());
		if (fragmentSizes[partID.getValue<IndexType>()] > 1) {
			ValueType key = -(maxCut-edgeCuts[v][partID.getValue<IndexType>()]);
			assert(-key <= degrees[v]);
			queues[partID.getValue<IndexType>()].insert(key, v); //negative max gain
		}
	}

	ValueType gainsum = 0;
	bool allQueuesEmpty = false;

	std::vector<bool> moved(n, false);

	while (!allQueuesEmpty) {
	allQueuesEmpty = true;

	//choose largest partition with non-empty queue.
	IndexType largestMovablePart = k;
	IndexType largestSize = 0;

	for (IndexType partID = 0; partID < k; partID++) {
		if (queues[partID].size() > 0 && fragmentSizes[partID] > largestSize) {
			largestMovablePart = partID;
			largestSize = fragmentSizes[partID];
		}
	}

	if (largestSize > 1 && largestMovablePart != k) {
		//at least one queue is not empty
		allQueuesEmpty = false;
		IndexType partID = largestMovablePart;

		assert(partID < queues.size());
		assert(queues[partID].size() > 0);

		IndexType topVertex;
		ValueType topGain;
		std::tie(topGain, topVertex) = queues[partID].extractMin();
		topGain = -topGain;//invert, since the negative gain was used as priority.
		assert(topVertex < n);
		assert(topVertex >= 0);
		if (unweighted) {
			assert(topGain <= degrees[topVertex]);
		}
		assert(!moved[topVertex]);
		Scalar partScalar = part.getValue(topVertex);
		assert(partScalar.getValue<IndexType>() == partID);

		//now get target partition.
		IndexType targetFragment = bestTargetFragment[topVertex];
		ValueType storedGain = edgeCuts[topVertex][targetFragment] - edgeCuts[topVertex][partID];
		
		assert(abs(storedGain - topGain) < 0.0001);
		assert(fragmentSizes[partID] > 1);


		//move node there
		part.setValue(topVertex, targetFragment);
		moved[topVertex] = true;

		//udpate size map
		fragmentSizes[partID] -= 1;
		fragmentSizes[targetFragment] += 1;

		//update history
		gainsum += topGain;
		gains.push_back(gainsum);
		transfers.emplace_back(partID, targetFragment);
		transferedVertices.push_back(topVertex);
		assert(transferedVertices.size() == transfers.size());
		assert(gains.size() == transfers.size());

		double imbalance = (*std::max_element(fragmentSizes.begin(), fragmentSizes.end()) - optSize) / optSize;
		imbalances.push_back(imbalance);

		//std::cout << "Moved node " << topVertex << " to block " << targetFragment << " for gain of " << topGain << ", bringing sum to " << gainsum 
		//<< " and imbalance to " << imbalance  << "." << std::endl;

		//I've tried to replace these direct accesses by ReadAccess, program was slower
		const IndexType beginCols = ia[topVertex];
		const IndexType endCols = ia[topVertex+1];

		for (IndexType j = beginCols; j < endCols; j++) {
			const IndexType neighbour = ja[j];
			if (!moved[neighbour]) {
				//update edge cuts
				Scalar neighbourBlockScalar = part.getValue(neighbour);
				IndexType neighbourBlock = neighbourBlockScalar.getValue<IndexType>();

				edgeCuts[neighbour][partID] -= unweighted ? 1 : values[j];
				assert(edgeCuts[neighbour][partID] >= 0);
				edgeCuts[neighbour][targetFragment] += unweighted ? 1 : values[j];
				assert(edgeCuts[neighbour][targetFragment] <= degrees[neighbour]);

				//find new fragment for neighbour
				ValueType maxCut = -totalWeight;
				IndexType idAtMax = k;

				for (IndexType fragment = 0; fragment < k; fragment++) {
					if (fragment != neighbourBlock && edgeCuts[neighbour][fragment] > maxCut  && fragmentSizes[fragment] <= maxAllowablePartSize) {
						idAtMax = fragment;
						maxCut = edgeCuts[neighbour][fragment];
					}
				}

				assert(maxCut >= 0);
				if (unweighted) assert(maxCut <= degrees[neighbour]);
				assert(idAtMax < k);
				bestTargetFragment[neighbour] = idAtMax;

				ValueType key = -(maxCut-edgeCuts[neighbour][neighbourBlock]);
				assert(-key == edgeCuts[neighbour][idAtMax] - edgeCuts[neighbour][neighbourBlock]);
				assert(-key <= degrees[neighbour]);

				//update prioqueue
				queues[neighbourBlock].remove(neighbour);
				queues[neighbourBlock].insert(key, neighbour);
				}
			}
		}
	}

	const IndexType testedNodes = gains.size();
	if (testedNodes == 0) return 0;
	assert(gains.size() == transfers.size());

    /**
	* now find best partition among those tested
	*/
	IndexType maxIndex = -1;
	ValueType maxGain = 0;

	for (IndexType i = 0; i < testedNodes; i++) {
		if (gains[i] > maxGain && imbalances[i] <= epsilon) {
			maxIndex = i;
			maxGain = gains[i];
		}
	}
	assert(testedNodes >= maxIndex);
	//assert(maxIndex >= 0);
	assert(testedNodes-1 < transfers.size());

	/**
	 * apply partition modifications in reverse until best is recovered
	 */
	for (int i = testedNodes-1; i > maxIndex; i--) {
		assert(transferedVertices[i] < n);
		assert(transferedVertices[i] >= 0);
		part.setValue(transferedVertices[i], transfers[i].first);
	}
	return maxGain;
}

template<typename IndexType, typename ValueType>
ValueType ParcoRepart<IndexType, ValueType>::computeCut(const CSRSparseMatrix<ValueType> &input, const DenseVector<IndexType> &part, const bool ignoreWeights) {
	SCAI_REGION( "ParcoRepart.computeCut" )
	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	const scai::dmemo::DistributionPtr partDist = part.getDistributionPtr();

	const IndexType n = inputDist->getGlobalSize();
	const IndexType localN = inputDist->getLocalSize();
	const Scalar maxBlockScalar = part.max();
	const IndexType maxBlockID = maxBlockScalar.getValue<IndexType>();

	if (partDist->getLocalSize() != localN) {
		throw std::runtime_error("partition has " + std::to_string(partDist->getLocalSize()) + " local values, but matrix has " + std::to_string(localN));
	}

	const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
	scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());
	scai::hmemo::HArray<IndexType> localData = part.getLocalValues();
	scai::hmemo::ReadAccess<IndexType> partAccess(localData);

	scai::hmemo::ReadAccess<ValueType> values(localStorage.getValues());

	scai::dmemo::Halo partHalo = buildPartHalo(input, part);
	scai::utilskernel::LArray<IndexType> haloData;
	partDist->getCommunicatorPtr()->updateHalo( haloData, localData, partHalo );

	/**
	 * first pass, compute local cut and build list of required halo indices
	 */
	ValueType result = 0;
	for (IndexType i = 0; i < localN; i++) {
		const IndexType beginCols = ia[i];
		const IndexType endCols = ia[i+1];
		assert(ja.size() >= endCols);

		const IndexType globalI = inputDist->local2global(i);
		assert(partDist->isLocal(globalI));
		IndexType thisBlock;
		partAccess.getValue(thisBlock, i);
		
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
				if (ignoreWeights) {
					result++;
				} else {
					result += values[j];
				}
			}
		}
	}

	if (!inputDist->isReplicated()) {
    //sum values over all processes
    result = inputDist->getCommunicatorPtr()->sum(result);
  }

  return result / 2; //counted each edge from both sides
}

template<typename IndexType, typename ValueType>
IndexType ParcoRepart<IndexType, ValueType>::localBlockSize(const DenseVector<IndexType> &part, IndexType blockID) {
	SCAI_REGION( "ParcoRepart.localBlockSize" )
	IndexType result = 0;
	scai::hmemo::ReadAccess<IndexType> localPart(part.getLocalValues());

	for (IndexType i = 0; i < localPart.size(); i++) {
		if (localPart[i] == blockID) {
			result++;
		}
	}

	return result;
}

template<typename IndexType, typename ValueType>
ValueType ParcoRepart<IndexType, ValueType>::computeImbalance(const DenseVector<IndexType> &part, IndexType k) {
	SCAI_REGION( "ParcoRepart.computeImbalance" )
	const IndexType n = part.getDistributionPtr()->getGlobalSize();
	std::vector<IndexType> subsetSizes(k, 0);
	scai::hmemo::ReadAccess<IndexType> localPart(part.getLocalValues());
	const Scalar maxK = part.max();
	if (maxK.getValue<IndexType>() >= k) {
		throw std::runtime_error("Block id " + std::to_string(maxK.getValue<IndexType>()) + " found in partition with supposedly" + std::to_string(k) + " blocks.");
	}
 	
	for (IndexType i = 0; i < localPart.size(); i++) {
		IndexType partID;
		localPart.getValue(partID, i);
		subsetSizes[partID] += 1;
	}
	IndexType optSize = std::ceil(n / k);

	//if we don't have the full partition locally, 
	scai::dmemo::CommunicatorPtr comm = part.getDistributionPtr()->getCommunicatorPtr();
	if (!part.getDistribution().isReplicated()) {
	  //sum block sizes over all processes
	  for (IndexType partID = 0; partID < k; partID++) {
	    subsetSizes[partID] = comm->sum(subsetSizes[partID]);
	  }
	}
	
	IndexType maxBlockSize = *std::max_element(subsetSizes.begin(), subsetSizes.end());
	return ((maxBlockSize - optSize)/ optSize);
}

template<typename IndexType, typename ValueType>
std::vector<DenseVector<IndexType>> ParcoRepart<IndexType, ValueType>::computeCommunicationPairings(const CSRSparseMatrix<ValueType> &input, const DenseVector<IndexType> &part,
			const DenseVector<IndexType> &blocksToPEs) {
	/**
	 * for now, trivial communication pairing in 2^(ceil(log_2(p))) steps.
	 */
	SCAI_REGION( "ParcoRepart.computeCommunicationPairings" )

	const Scalar maxPartID = blocksToPEs.max();
	const IndexType p = maxPartID.getValue<IndexType>() + 1;
	const IndexType rounds = std::ceil(std::log(p) / std::log(2));
	const IndexType upperPowerP = 1 << rounds;
	assert(upperPowerP >= p);
	assert(upperPowerP < 2*p);
	const IndexType steps = upperPowerP-1;
	assert(steps >= p-1);

	std::vector<DenseVector<IndexType>> result;

	for (IndexType step = 1; step <= steps; step++) {
		IndexType commPerm[p];

		for (IndexType i = 0; i < p; i++) {
			IndexType partner = i ^ step;
			commPerm[i] = partner < p ? partner : i;
		}

		result.emplace_back(p, &commPerm[0]);
	}
	return result;
}

template<typename IndexType, typename ValueType>
std::vector<IndexType> ITI::ParcoRepart<IndexType, ValueType>::nonLocalNeighbors(const CSRSparseMatrix<ValueType>& input) {
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

//-----------------------------------------------------------------------------------------

//return: there is an edge is the block graph between blocks ret[0]-ret[1], ret[2]-ret[3] ... ret[2i]-ret[2i+1] 
template<typename IndexType, typename ValueType>
scai::dmemo::Halo ITI::ParcoRepart<IndexType, ValueType>::buildMatrixHalo(
		const CSRSparseMatrix<ValueType>& input) {

	SCAI_REGION( "ParcoRepart.buildMatrixHalo" )

	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	std::vector<IndexType> requiredHaloIndices = nonLocalNeighbors(input);

	assert(requiredHaloIndices.size() <= inputDist->getGlobalSize() - inputDist->getLocalSize());

	scai::dmemo::Halo mHalo;
	{
		scai::hmemo::HArrayRef<IndexType> arrRequiredIndexes( requiredHaloIndices );
		scai::dmemo::HaloBuilder::build( *inputDist, arrRequiredIndexes, mHalo );
	}

	return mHalo;
}

template<typename IndexType, typename ValueType>
scai::dmemo::Halo ITI::ParcoRepart<IndexType, ValueType>::buildPartHalo(
		const CSRSparseMatrix<ValueType>& input, const DenseVector<IndexType> &part) {

	SCAI_REGION( "ParcoRepart.buildPartHalo" )

	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	const scai::dmemo::DistributionPtr partDist = part.getDistributionPtr();

	if (inputDist->getLocalSize() != partDist->getLocalSize()) {
		throw std::runtime_error("Input matrix and partition must have the same distribution.");
	}

	std::vector<IndexType> requiredHaloIndices = nonLocalNeighbors(input);

	assert(requiredHaloIndices.size() <= partDist->getGlobalSize() - partDist->getLocalSize());

	scai::dmemo::Halo Halo;
	{
		scai::hmemo::HArrayRef<IndexType> arrRequiredIndexes( requiredHaloIndices );
		scai::dmemo::HaloBuilder::build( *partDist, arrRequiredIndexes, Halo );
	}

	return Halo;
}

template<typename IndexType, typename ValueType>
std::pair<std::vector<IndexType>, IndexType> ITI::ParcoRepart<IndexType, ValueType>::getInterfaceNodes(const CSRSparseMatrix<ValueType> &input, const DenseVector<IndexType> &part, IndexType thisBlock, IndexType otherBlock, IndexType depth) {

	SCAI_REGION( "ParcoRepart.getInterfaceNodes" )
	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	const scai::dmemo::DistributionPtr partDist = part.getDistributionPtr();

	const IndexType n = inputDist->getGlobalSize();
	const IndexType localN = inputDist->getLocalSize();

	if (partDist->getLocalSize() != localN) {
		throw std::runtime_error("Partition has " + std::to_string(partDist->getLocalSize()) + " local nodes, but matrix has " + std::to_string(localN) + ".");
	}

	Scalar maxBlockScalar = part.max();
	if (thisBlock > maxBlockScalar.getValue<IndexType>()) {
		throw std::runtime_error(std::to_string(thisBlock) + " is not a valid block id.");
	}

	if (otherBlock > maxBlockScalar.getValue<IndexType>()) {
		throw std::runtime_error(std::to_string(otherBlock) + " is not a valid block id.");
	}

	if (thisBlock == otherBlock) {
		throw std::runtime_error("Block IDs must be different.");
	}

	if (depth <= 0) {
		throw std::runtime_error("Depth must be positive");
	}

	scai::hmemo::HArray<IndexType> localData = part.getLocalValues();
	scai::hmemo::ReadAccess<IndexType> partAccess(localData);

	scai::dmemo::Halo partHalo = buildPartHalo(input, part);
	scai::utilskernel::LArray<IndexType> haloData;
	partDist->getCommunicatorPtr()->updateHalo( haloData, localData, partHalo );

	const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
	const scai::hmemo::ReadAccess<IndexType> ia(localStorage.getIA());
	const scai::hmemo::ReadAccess<IndexType> ja(localStorage.getJA());

	/**
	 * first get nodes directly at the border to the other block
	 */
	std::vector<IndexType> interfaceNodes;

	for (IndexType localI = 0; localI < localN; localI++) {
		const IndexType beginCols = ia[localI];
		const IndexType endCols = ia[localI+1];

		if (partAccess[localI] == thisBlock) {

			for (IndexType j = beginCols; j < endCols; j++) {
				IndexType neighbor = ja[j];
				IndexType neighborBlock;
				if (partDist->isLocal(neighbor)) {
					neighborBlock = partAccess[partDist->global2local(neighbor)];
				} else {
					neighborBlock = haloData[partHalo.global2halo(neighbor)];
				}

				if (neighborBlock == otherBlock) {
					interfaceNodes.push_back(inputDist->local2global(localI));
					//only add interface node once
					break;
				}
			}
		}
	}

	assert(interfaceNodes.size() <= localN);

	IndexType lastRoundMarker = 0;
	/**
	 * now gather buffer zone with breadth-first search
	 */
	if (depth > 1) {
		std::vector<bool> touched(localN, false);

		std::queue<IndexType> bfsQueue;
		for (IndexType node : interfaceNodes) {
			bfsQueue.push(node);
			touched[inputDist->global2local(node)] = true;
		}
		assert(bfsQueue.size() == interfaceNodes.size());

		for (IndexType round = 1; round < depth; round++) {
			lastRoundMarker = interfaceNodes.size();
			std::queue<IndexType> nextQueue;
			while (!bfsQueue.empty()) {
				IndexType nextNode = bfsQueue.front();
				bfsQueue.pop();

				const IndexType localI = inputDist->global2local(nextNode);
				assert(touched[localI]);
				const IndexType beginCols = ia[localI];
				const IndexType endCols = ia[localI+1];

				for (IndexType j = beginCols; j < endCols; j++) {
					IndexType neighbor = ja[j];
					if (inputDist->isLocal(neighbor) && partAccess[partDist->global2local(neighbor)] == thisBlock &&
							!touched[inputDist->global2local(neighbor)]) {
						nextQueue.push(neighbor);
						interfaceNodes.push_back(neighbor);
						touched[inputDist->global2local(neighbor)] = true;
					}
				}
			}
			bfsQueue = nextQueue;
		}
	}

	assert(interfaceNodes.size() <= localN);
	return {interfaceNodes, lastRoundMarker};
}

template<typename IndexType, typename ValueType>
IndexType ITI::ParcoRepart<IndexType, ValueType>::getDegreeSum(const CSRSparseMatrix<ValueType> &input, std::vector<IndexType> nodes) {
	const CSRStorage<ValueType>& localStorage = input.getLocalStorage();
	const scai::hmemo::ReadAccess<IndexType> localIa(localStorage.getIA());

	IndexType result = 0;

	for (IndexType node : nodes) {
		IndexType localID = input.getRowDistributionPtr()->global2local(node);
		result += localIa[localID+1] - localIa[localID];
	}
	return result;
}

template<typename IndexType, typename ValueType>
ValueType ITI::ParcoRepart<IndexType, ValueType>::distributedFMStep(CSRSparseMatrix<ValueType> &input, DenseVector<IndexType> &part, IndexType k, ValueType epsilon, bool unweighted) {
	const IndexType magicBorderRegionDepth = 4;
	SCAI_REGION( "ParcoRepart.distributedFMStep" )
	const IndexType globalN = input.getRowDistributionPtr()->getGlobalSize();
	scai::dmemo::CommunicatorPtr comm = input.getRowDistributionPtr()->getCommunicatorPtr();

	if (part.getDistributionPtr()->getLocalSize() != input.getRowDistributionPtr()->getLocalSize()) {
		throw std::runtime_error("Distributions of input matrix and partitions must be equal, for now.");
	}

	if (!input.getColDistributionPtr()->isReplicated()) {
		throw std::runtime_error("Column distribution needs to be replicated.");
	}

	if (epsilon < 0) {
		throw std::runtime_error("Epsilon must be >= 0, not " + std::to_string(epsilon));
	}

	//std::cout << "Thread " << comm->getRank() << ", entered distributed FM." << std::endl;
	/**
	 * get trivial mapping for now.
	 */
	//create trivial mapping
	scai::lama::DenseVector<IndexType> mapping(k, 0);
	for (IndexType i = 0; i < k; i++) {
		mapping.setValue(i, i);
	}

	std::vector<DenseVector<IndexType >> communicationScheme = computeCommunicationPairings(input, part, mapping);

    const Scalar maxBlockScalar = part.max();
    const IndexType maxBlockID = maxBlockScalar.getValue<IndexType>();

    if (k != maxBlockID + 1) {
    	throw std::runtime_error("Should have " + std::to_string(k) + " blocks, has maximum ID " + std::to_string(maxBlockID));
    }

    if (k != comm->getSize()) {
    	throw std::runtime_error("Called with " + std::to_string(comm->getSize()) + " processors, but " + std::to_string(k) + " blocks.");
    }

    ValueType gainSum = 0;

	for (IndexType i = 0; i < communicationScheme.size(); i++) {
		SCAI_REGION( "ParcoRepart.distributedFMStep.loop" )

		const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
		const scai::dmemo::DistributionPtr partDist = part.getDistributionPtr();

		const IndexType localN = inputDist->getLocalSize();
		assert(comm->sum(localN) == globalN);

		if (!communicationScheme[i].getDistributionPtr()->isLocal(comm->getRank())) {
			throw std::runtime_error("Scheme value for " + std::to_string(comm->getRank()) + " must be local.");
		}
		scai::hmemo::ReadAccess<IndexType> commAccess(communicationScheme[i].getLocalValues());
		IndexType partner = commAccess[communicationScheme[i].getDistributionPtr()->global2local(comm->getRank())];

		const IndexType localBlockID = comm->getRank();

		//copy into usable data structure with iterators
		std::vector<IndexType> myGlobalIndices(localN);
		for (IndexType j = 0; j < localN; j++) {
			myGlobalIndices[j] = inputDist->local2global(j);
		}//TODO: maybe refactor this someday to be outside the loop

		if (partner != comm->getRank()) {
			//processor is active this round

			//std::cout << "Thread " << comm->getRank() << " is active in round " << i << "." << std::endl;

			/**
			 * get indices of border nodes with breadth-first search
			 */
			std::vector<IndexType> interfaceNodes;
			IndexType lastRoundMarker;
			std::tie(interfaceNodes, lastRoundMarker)= getInterfaceNodes(input, part, localBlockID, partner, magicBorderRegionDepth+1);
			std::sort(interfaceNodes.begin(), interfaceNodes.end());

			//std::cout << "Thread " << comm->getRank() << ", round " << i << ", gathered interface nodes." << std::endl;

			/**
			 * now swap indices of nodes in border region with partner processor.
			 * For this, first find out the length of the swap array.
			 */

			//swap size of border region and total block size
			IndexType blockSize = localBlockSize(part, localBlockID);
			if (blockSize != localN) {
				throw std::runtime_error(std::to_string(localN) + " local nodes, but only " + std::to_string(blockSize) + " of them belong to block " + std::to_string(localBlockID) + ".");
			}

			IndexType swapField[4];
			swapField[0] = interfaceNodes.size();
			swapField[1] = lastRoundMarker;
			swapField[2] = blockSize;
			swapField[3] = getDegreeSum(input, interfaceNodes);
			comm->swap(swapField, 4, partner);
			const IndexType otherLastRoundMarker = swapField[1];
			const IndexType otherBlockSize = swapField[2];
			const IndexType otherDegreeSum = swapField[3];
			IndexType swapLength = std::max(int(swapField[0]), int(interfaceNodes.size()));

			if (interfaceNodes.size() == 0) {
				if (swapLength != 0) {
					throw std::runtime_error("Partner PE has a border region, but this PE doesn't. Looks like the block indices were allocated badly.");
				} else {
					/*
					 * These processors don't share a border and thus have no communication to do with each other.
					 */
					//continue;
				}
			}

			ValueType swapNodes[swapLength];
			for (IndexType i = 0; i < swapLength; i++) {
				if (i < interfaceNodes.size()) {
					swapNodes[i] = interfaceNodes[i];
				} else {
					swapNodes[i] = -1;
				}
			}

			comm->swap(swapNodes, swapLength, partner);

			//the number of interface nodes was stored in swapField[0] and then swapped.
			//swapField[0] now contains the number of nodes in the partner's border region
			std::vector<IndexType> requiredHaloIndices(swapField[0]);
			for (IndexType i = 0; i < swapField[0]; i++) {
				assert(swapNodes[i] >= 0);
				requiredHaloIndices[i] = swapNodes[i];
			}

			//std::cout << "Thread " << comm->getRank() << ", round " << i << ", swapped " << swapLength << " interface nodes with thread " << partner << std::endl;


			assert(requiredHaloIndices.size() <= globalN - inputDist->getLocalSize());

			/*
			 * extend halos to cover border region of other PE.
			 * This is probably very inefficient in a general distribution where all processors need to be contacted to gather the halo.
			 * Possible Improvements: Assemble arrays describing the subgraph, swap that.
			 * Exchanging the partHalo is actually unnecessary, since all indices in requiredHaloIndices have the same block.
			 */
			IndexType numValues = input.getLocalStorage().getValues().size();
			scai::dmemo::Halo graphHalo;
			{
				scai::hmemo::HArrayRef<IndexType> arrRequiredIndexes( requiredHaloIndices );
				scai::dmemo::HaloBuilder::build( *inputDist, arrRequiredIndexes, graphHalo );
			}//TODO: halos for matrices might need to be built differently

			CSRStorage<ValueType> haloMatrix;
			haloMatrix.exchangeHalo( graphHalo, input.getLocalStorage(), *comm );
			assert(input.getLocalStorage().getValues().size() == numValues);//number of edges in local part stays unchanged.
			assert(haloMatrix.getValues().size() == otherDegreeSum);
			for (IndexType node : requiredHaloIndices) {
				assert(graphHalo.global2halo(node) != nIndex);
			}

			//std::cout << "Thread " << comm->getRank() << ", round " << i << ", got halo." << std::endl;

			//why not use vectors in the FM step or use sets to begin with? Might be faster.
			//here we only exchange one round less than gathered. The other forms a dummy border layer.
			std::set<IndexType> firstRegion(interfaceNodes.begin(), interfaceNodes.begin()+lastRoundMarker);
			std::set<IndexType> secondRegion(requiredHaloIndices.begin(), requiredHaloIndices.begin()+otherLastRoundMarker);
			const std::set<IndexType> firstRegionCopy = firstRegion;

			std::set<IndexType> firstDummyLayer(interfaceNodes.begin()+lastRoundMarker, interfaceNodes.end());
			std::set<IndexType> secondDummyLayer(requiredHaloIndices.begin()+otherLastRoundMarker, requiredHaloIndices.end());

			//block sizes and capacities
			const IndexType optSize = ceil(double(globalN) / k);
			const IndexType maxAllowableBlockSize = optSize*(1+epsilon);
			std::pair<IndexType, IndexType> blockSizes = {blockSize, otherBlockSize};
			std::pair<IndexType, IndexType> maxBlockSizes = {maxAllowableBlockSize, maxAllowableBlockSize};

			//std::cout << "Thread " << comm->getRank() << ", round " << i << ", prepared sets." << std::endl;


			/**
			 * execute FM locally
			 */
			ValueType gain = twoWayLocalFM(input, haloMatrix, graphHalo, firstRegion, secondRegion, firstDummyLayer, secondDummyLayer, blockSizes, maxBlockSizes, epsilon, unweighted);

			//std::cout << "Thread " << comm->getRank() << ", round " << i << ", finished twoWayLocalFM with gain " << gain << "." << std::endl;


			//communicate achieved gain. PE with better solution should send their secondRegion.
			assert(unweighted); //if this assert fails, you need to change the type of swapField back to ValueType before removing it.
			swapField[0] = secondRegion.size();
			swapField[1] = gain;
			comm->swap(swapField, 2, partner);

			if (swapField[1] == 0 && gain == 0) {
				//Oh well. None of the processors managed an improvement. No need to update data structures.
				std::cout << "Thread " << comm->getRank() << ", round " << i << " no gain here and at " << partner << "." << std::endl;


			}	else {
				SCAI_REGION( "ParcoRepart.distributedFMStep.loop.prepareRedist" )

				gainSum += std::max(ValueType(swapField[1]), ValueType(gain));

				bool otherWasBetter = (swapField[1] > gain || (swapField[1] == gain && partner < comm->getRank()));

				if (otherWasBetter) {
					swapLength = swapField[0];
					std::cout << "Thread " << comm->getRank() << ", round " << i << " preparing to receive " << swapLength << " nodes from " << partner << "." << std::endl;
				} else {
					swapLength = secondRegion.size();
					std::cout << "Thread " << comm->getRank() << ", round " << i << " preparing to send " << swapLength << " nodes to " << partner << "." << std::endl;
				}

				ValueType resultSwap[swapLength];
				if (!otherWasBetter) {
					IndexType j = 0;
					for (IndexType nodeID : secondRegion) {
						resultSwap[j] = nodeID;
						j++;
					}
					assert(j == secondRegion.size());
				}

				comm->swap(resultSwap, swapLength, partner);

				//keep best solution
				if (otherWasBetter) {
					firstRegion.clear();
					for (IndexType j = 0; j < swapLength; j++) {
						firstRegion.insert(resultSwap[j]);
					}
					assert(firstRegion.size() == swapLength);
				}

				assert(std::is_sorted(firstRegion.begin(), firstRegion.end()));
				assert(std::is_sorted(firstRegionCopy.begin(), firstRegionCopy.end()));

				//get list of additional and removed nodes
				std::vector<IndexType> additionalNodes(firstRegion.size());
				//std::vector<IndexType>::iterator it;
				auto it = std::set_difference(firstRegion.begin(), firstRegion.end(), firstRegionCopy.begin(), firstRegionCopy.end(), additionalNodes.begin());
				additionalNodes.resize(it-additionalNodes.begin());
				std::vector<IndexType> deletedNodes(firstRegionCopy.size());
				auto it2 = std::set_difference(firstRegionCopy.begin(), firstRegionCopy.end(), firstRegion.begin(), firstRegion.end(), deletedNodes.begin());
				deletedNodes.resize(it2-deletedNodes.begin());

				//std::cout << "Thread " << comm->getRank() << ", round " << i << ", lost " << deletedNodes.size() << " nodes and gained " << additionalNodes.size() << std::endl;

				assert(std::is_sorted(additionalNodes.begin(), additionalNodes.end()));
				assert(std::is_sorted(deletedNodes.begin(), deletedNodes.end()));
				assert(std::is_sorted(myGlobalIndices.begin(), myGlobalIndices.end()));

				std::vector<IndexType> newIndices(myGlobalIndices.size()-deletedNodes.size()+additionalNodes.size());
				auto it3 = std::set_difference(myGlobalIndices.begin(), myGlobalIndices.end(), deletedNodes.begin(), deletedNodes.end(), newIndices.begin());
				newIndices.resize(it3-newIndices.begin());
				newIndices.insert(newIndices.end(), additionalNodes.begin(), additionalNodes.end());
				std::sort(newIndices.begin(), newIndices.end());
				assert(newIndices.size() == myGlobalIndices.size()-deletedNodes.size()+additionalNodes.size());
				myGlobalIndices = newIndices;
			}
		} else {
			const IndexType dummyPartner = comm->getRank() == 0 ? 1 : 0;
			//std::cout << "Thread " << comm->getRank() << " is inactive in round " << i << ", performing dummy interface and halo exchange." << std::endl;

			getInterfaceNodes(input, part, localBlockID, dummyPartner, 1);

			//dummy halo update
			std::vector<IndexType> requiredHaloIndices;
			scai::dmemo::Halo graphHalo;
			{
				scai::hmemo::HArrayRef<IndexType> arrRequiredIndexes( requiredHaloIndices );
				scai::dmemo::HaloBuilder::build( *inputDist, arrRequiredIndexes, graphHalo );
			}

			CSRStorage<ValueType> haloMatrix;
			haloMatrix.exchangeHalo( graphHalo, input.getLocalStorage(), *comm );
		}
		assert(std::is_sorted(myGlobalIndices.begin(), myGlobalIndices.end()));
		scai::utilskernel::LArray<IndexType> indexTransport(myGlobalIndices.size());
		for (IndexType j = 0; j < myGlobalIndices.size(); j++) {
			indexTransport[j] = myGlobalIndices[j];
		}

		IndexType participating = comm->sum(1);
		if (participating != comm->getSize()) {
			std::cout << participating << " of " << comm->getSize() << " threads in redistribution." << std::endl;
		}
		//std::cout << "Thread " << comm->getRank() << ", round " << i << ", " << myGlobalIndices.size() << " indices." << std::endl;

		//redistribute. This could probably be done faster by using the haloStorage already there. Maybe use joinHalo or splitHalo methods here.
		scai::dmemo::DistributionPtr newDistribution(new scai::dmemo::GeneralDistribution(globalN, indexTransport, comm));
		input.redistribute(newDistribution, input.getColDistributionPtr());
		part.redistribute(newDistribution);

		for (IndexType globalId : myGlobalIndices) {
			part.setValue(globalId, localBlockID);
		}

		//std::cout << "Thread " << comm->getRank() << ", round " << i << ", finished redistribution." << std::endl;

	}
	return comm->sum(gainSum) / 2;
}

template<typename IndexType, typename ValueType>
ValueType ITI::ParcoRepart<IndexType, ValueType>::twoWayLocalFM(const CSRSparseMatrix<ValueType> &input, const CSRStorage<ValueType> &haloStorage,
		const Halo &matrixHalo, std::set<IndexType> &firstregion,  std::set<IndexType> &secondregion,
		const std::set<IndexType> &firstDummyLayer, const std::set<IndexType> &secondDummyLayer,
		std::pair<IndexType, IndexType> blockSizes,
		const std::pair<IndexType, IndexType> blockCapacities, ValueType epsilon, const bool unweighted) {
	SCAI_REGION( "ParcoRepart.twoWayLocalFM" )

	if (blockSizes.first >= blockCapacities.first && blockSizes.second >= blockCapacities.second) {
		//cannot move any nodes, all blocks are overloaded already.
		return 0;
	}

	const scai::dmemo::DistributionPtr inputDist = input.getRowDistributionPtr();
	const IndexType globalN = inputDist->getGlobalSize();
	scai::dmemo::CommunicatorPtr comm = input.getRowDistributionPtr()->getCommunicatorPtr();

	/**
	 * These maps are mapping the b global indices received in the border region sets onto the interval [0,b-1].
	 */
	std::map<IndexType, IndexType> globalToVeryLocal;
	std::map<IndexType, IndexType> veryLocalToGlobal;
	IndexType i = 0;
	for (IndexType index : firstregion) {
		//edge information about elements must be either local or in halo
		assert(inputDist->isLocal(index) || matrixHalo.global2halo(index) != std::numeric_limits<IndexType>::max());
		//no overlap between input regions or dummy layers
		assert(secondregion.count(index) == 0);
		assert(firstDummyLayer.count(index) == 0);

		assert(globalToVeryLocal.count(index) == 0);
		globalToVeryLocal[index] = i;
		veryLocalToGlobal[i] = index;
		i++;
	}

	for (IndexType index : secondregion) {
		//edge information about elements must be either local or in halo
		assert(inputDist->isLocal(index) || matrixHalo.global2halo(index) != std::numeric_limits<IndexType>::max());
		assert(secondDummyLayer.count(index) == 0);

		assert(globalToVeryLocal.count(index) == 0);
		globalToVeryLocal[index] = i;
		veryLocalToGlobal[i] = index;
		i++;
	}

	assert(i == firstregion.size() + secondregion.size());
	assert(globalToVeryLocal.size() == i);
	assert(veryLocalToGlobal.size() == i);

	const IndexType veryLocalN = i;

	auto isVeryLocal = [&](IndexType globalID){return globalToVeryLocal.count(globalID) > 0;};
	auto isInFirstBlock = [&](IndexType globalID){return firstregion.count(globalID) + firstDummyLayer.count(globalID) > 0;};
	auto isInSecondBlock = [&](IndexType globalID){return secondregion.count(globalID) + secondDummyLayer.count(globalID) > 0;};

	/**
	 * check degree symmetry
	 */
	std::vector<IndexType> inDegree(veryLocalN, 0);
	std::vector<IndexType> outDegree(veryLocalN, 0);
	for (IndexType i = 0; i < veryLocalN; i++) {
		IndexType globalID = veryLocalToGlobal.at(i);
		const CSRStorage<ValueType>& storage = inputDist->isLocal(globalID) ? input.getLocalStorage() : haloStorage;
		const IndexType localID = inputDist->isLocal(globalID) ? inputDist->global2local(globalID) : matrixHalo.global2halo(globalID);
		assert(localID != nIndex);

		const scai::hmemo::ReadAccess<IndexType> localIa(storage.getIA());
		const scai::hmemo::ReadAccess<IndexType> localJa(storage.getJA());
		const IndexType beginCols = localIa[localID];
		const IndexType endCols = localIa[localID+1];
		for (IndexType j = beginCols; j < endCols; j++) {
			IndexType globalNeighbor = localJa[j];
			std::string locationString = inputDist->isLocal(globalID) ? "locally" : "in halo";
			std::string countString = globalNeighbor != globalID && isVeryLocal(globalNeighbor) ? "counted" : "not counted";
			//std::cout << "Thread " << comm->getRank() << ": (" << globalID << "," << globalNeighbor << ") found " << locationString << ", " << countString << std::endl;

			if (globalNeighbor != globalID && isVeryLocal(globalNeighbor)) {
				IndexType veryLocalNeighbor = globalToVeryLocal.at(globalNeighbor);
				outDegree[i]++;
				inDegree[veryLocalNeighbor]++;
			}
		}
	}

	for (IndexType i = 0; i < veryLocalN; i++) {
		if (inDegree[i] != outDegree[i]) {
			throw std::runtime_error("Process " + std::to_string(comm->getRank()) + ": Node " + std::to_string(veryLocalToGlobal[i]) + " has " + std::to_string(inDegree[i]) + " incoming local edges but "
					+ std::to_string(outDegree[i]) + " outgoing local edges.");
		}
	}


	auto computeGain = [&](IndexType globalID){
		SCAI_REGION( "ParcoRepart.computeGain" )
		bool firstBlock = isInFirstBlock(globalID);
		assert(firstBlock != isInSecondBlock(globalID));
		assert(isVeryLocal(globalID));

		ValueType result = 0;
		/**
		 * neighborhood information is either in local matrix or halo.
		 */
		const CSRStorage<ValueType>& storage = inputDist->isLocal(globalID) ? input.getLocalStorage() : haloStorage;
		const IndexType localID = inputDist->isLocal(globalID) ? inputDist->global2local(globalID) : matrixHalo.global2halo(globalID);
		assert(localID != nIndex);

		const scai::hmemo::ReadAccess<IndexType> localIa(storage.getIA());
		const scai::hmemo::ReadAccess<IndexType> localJa(storage.getJA());
		const scai::hmemo::ReadAccess<ValueType> localValues(storage.getValues());

		const IndexType beginCols = localIa[localID];
		const IndexType endCols = localIa[localID+1];

		for (IndexType j = beginCols; j < endCols; j++) {
			IndexType globalNeighbor = localJa[j];
			if (globalNeighbor == globalID) {
				//self-loop, not counted
				continue;
			}
			const ValueType edgeweight = unweighted ? 1 : localValues[j];
			bool same;
			if (isInSecondBlock(globalNeighbor)) {
				same = !firstBlock;
			} else if (isInFirstBlock(globalNeighbor)) {
				same = firstBlock;
			} else {
				//neighbor in other block, not relevant for gain
				continue;
			}

			result += same ? -edgeweight : edgeweight;
		}

		return result;
	};

	/**
	 * construct and fill gain table and priority queues. Since only one target block is possible, gain table is one-dimensional.
	 */
	PrioQueue<ValueType, IndexType> firstQueue(veryLocalN);
	PrioQueue<ValueType, IndexType> secondQueue(veryLocalN);

	std::vector<ValueType> gain(veryLocalN);

	for (IndexType globalIndex : firstregion) {
		IndexType veryLocalID = globalToVeryLocal.at(globalIndex);
		gain[veryLocalID] = computeGain(globalIndex);
		assert(!firstQueue.contains(veryLocalID));
		firstQueue.insert(-gain[veryLocalID], veryLocalID);
	}

	for (IndexType globalIndex : secondregion) {
		IndexType veryLocalID = globalToVeryLocal.at(globalIndex);
		gain[veryLocalID] = computeGain(globalIndex);
		assert(!firstQueue.contains(veryLocalID));
		assert(!secondQueue.contains(veryLocalID));
		secondQueue.insert(-gain[veryLocalID], veryLocalID);
	}

	std::vector<bool> moved(veryLocalN, false);
	std::vector<std::pair<IndexType, IndexType>> transfers;
	transfers.reserve(veryLocalN);

	ValueType gainSum = 0;
	std::vector<ValueType> gainSumList, fillFactorList;
	gainSumList.reserve(veryLocalN);
	fillFactorList.reserve(veryLocalN);

	IndexType iter = 0;
	while (firstQueue.size() + secondQueue.size() > 0) {
		SCAI_REGION( "ParcoRepart.twoWayLocalFM.queueloop" )
		IndexType bestQueueIndex;

		/*
		 * first check break situations
		 */
		if ((firstQueue.size() == 0 && blockSizes.first >= blockCapacities.first)
				 ||	(secondQueue.size() == 0 && blockSizes.second >= blockCapacities.second)) {
			//cannot move any nodes
			break;
		}

		if (firstQueue.size() == 0) {
			assert(blockSizes.first < blockCapacities.first);
			bestQueueIndex = 1;
		} else if (secondQueue.size() == 0) {
			assert(blockSizes.second < blockCapacities.second);
			bestQueueIndex = 0;
		} else {
			std::vector<ValueType> fullness = {double(blockSizes.first) / blockCapacities.first, double(blockSizes.second) / blockCapacities.second};
			std::vector<ValueType> gains = {firstQueue.inspectMin().first, secondQueue.inspectMin().first};

			assert(fullness[0] <= 1);
			assert(fullness[1] <= 1);
			assert(fullness[0] >= 0);
			assert(fullness[1] >= 0);

			//decide first by fullness, if fullness is equal, decide by gain. Since gain was inverted before inserting into queue, smaller gain is better.
			if (fullness[0] > fullness[1] || (fullness[0] == fullness[1] && gains[0] < gains[1])) {
				bestQueueIndex = 0;
			} else if (fullness[1] > fullness[0] || (fullness[0] == fullness[1] && gains[1] < gains[0])) {
				bestQueueIndex = 1;
			} else {
				//tie, break randomly
				assert(fullness[0] == fullness[1] && gains[0] == gains[1]);

				if ((rand() / RAND_MAX) < 0.5) {
					bestQueueIndex = 0;
				} else {
					bestQueueIndex = 1;
				}
			}
			//std::cout << "Fullness: (" << fullness[0] << "," << fullness[1] << ")" << ", gains (" << gains[0] << "," << gains[1] << "), picked queue " << bestQueueIndex << std::endl;
		}

		PrioQueue<ValueType, IndexType>& currentQueue = bestQueueIndex == 0 ? firstQueue : secondQueue;
		PrioQueue<ValueType, IndexType>& otherQueue = bestQueueIndex == 0 ? secondQueue : firstQueue;

		std::set<IndexType>& currentRegion = bestQueueIndex == 0 ? firstregion : secondregion;
		std::set<IndexType>& targetRegion = bestQueueIndex == 0 ? secondregion : firstregion;

		//now, we have selected a Queue.
		IndexType veryLocalID;
		ValueType topGain;
		std::tie(topGain, veryLocalID) = currentQueue.extractMin();
		assert(!currentQueue.contains(veryLocalID));
		assert(!otherQueue.contains(veryLocalID));
		topGain *= -1;

		IndexType topVertex = veryLocalToGlobal.at(veryLocalID);
		assert(isVeryLocal(topVertex));
		assert(!moved[veryLocalID]);
		if (topGain != computeGain(topVertex)) {
			throw std::runtime_error("iter " + std::to_string(transfers.size()) + ", queue key for " + std::to_string(topVertex) +
					": " + std::to_string(topGain) + ", computed gain: " + std::to_string(computeGain(topVertex))
					+ ", stored gain: " + std::to_string(gain[veryLocalID]));
		}
		assert(topGain == gain[veryLocalID]);

		//move node
		transfers.emplace_back(bestQueueIndex, topVertex);
		gainSum += topGain;
		gainSumList.push_back(gainSum);

		currentRegion.erase(topVertex);
		targetRegion.insert(topVertex);
		moved[veryLocalID] = true;

		blockSizes.first += bestQueueIndex == 0 ? -1 : 1;
		blockSizes.second += bestQueueIndex == 0 ? 1 : -1;
		//std::cout << "Moving " << topVertex << " from " << bestQueueIndex << ", bringing block sizes to " << blockSizes.first << ", " << blockSizes.second << std::endl;

		fillFactorList.push_back(std::max(double(blockSizes.first) / blockCapacities.first, double(blockSizes.second) / blockCapacities.second));

		//update gains of neighbors
		const CSRStorage<ValueType>& storage = inputDist->isLocal(topVertex) ? input.getLocalStorage() : haloStorage;
		const IndexType localID = inputDist->isLocal(topVertex) ? inputDist->global2local(topVertex) : matrixHalo.global2halo(topVertex);
		assert(localID != nIndex);

		const scai::hmemo::ReadAccess<IndexType> localIa(storage.getIA());
		const scai::hmemo::ReadAccess<IndexType> localJa(storage.getJA());
		const scai::hmemo::ReadAccess<ValueType> localValues(storage.getValues());
		const IndexType beginCols = localIa[localID];
		const IndexType endCols = localIa[localID+1];

		for (IndexType j = beginCols; j < endCols; j++) {
			SCAI_REGION( "ParcoRepart.twoWayLocalFM.queueloop.gainupdate" )
			IndexType neighbor = localJa[j];
			ValueType edgeweight = unweighted ? 1 : localValues[j];
			if (isVeryLocal(neighbor)) {
				IndexType veryLocalNeighborID = globalToVeryLocal.at(neighbor);
				if (moved[veryLocalNeighborID]) {
					continue;
				}
				bool wasInSameBlock = bestQueueIndex == 0 ? isInFirstBlock(neighbor) : isInSecondBlock(neighbor);
				assert(isInFirstBlock(neighbor) != isInSecondBlock(neighbor));

				//gain[veryLocalNeighborID] += wasInSameBlock ? edgeweight : -edgeweight;
				gain[veryLocalNeighborID] = computeGain(neighbor);

				if (wasInSameBlock) {
					assert(currentQueue.contains(veryLocalNeighborID));
					currentQueue.decreaseKey(-gain[veryLocalNeighborID], veryLocalNeighborID);
				} else {
					assert(otherQueue.contains(veryLocalNeighborID));
					otherQueue.decreaseKey(-gain[veryLocalNeighborID], veryLocalNeighborID);
				}
			}
		}
		iter++;
	}

	/**
	* now find best partition among those tested
	*/
	const IndexType testedNodes = gainSumList.size();
	if (testedNodes == 0) return 0;

	IndexType maxIndex = -1;
	ValueType maxGain = 0;

	for (IndexType i = 0; i < testedNodes; i++) {
		if (gainSumList[i] > maxGain && fillFactorList[i] <= 1) {
			maxIndex = i;
			maxGain = gainSumList[i];
		}
	}
	assert(testedNodes >= maxIndex);
	//assert(maxIndex >= 0);
	assert(testedNodes-1 < transfers.size());

	/**
	 * apply partition modifications in reverse until best is recovered
	 */
	for (int i = testedNodes-1; i > maxIndex; i--) {
		assert(transfers[i].second < globalN);
		std::set<IndexType>& sourceRegion = transfers[i].first == 0 ? firstregion : secondregion;
		std::set<IndexType>& targetRegion = transfers[i].first == 0 ? secondregion : firstregion;

		//apply movement in reverse
		sourceRegion.insert(transfers[i].second);
		targetRegion.erase(transfers[i].second);

		blockSizes.first += transfers[i].first == 0 ? 1 : -1;
		blockSizes.second += transfers[i].first == 0 ? -1 : 1;

	}
	assert(blockSizes.first <= blockCapacities.first);
	assert(blockSizes.second <= blockCapacities.second);
	assert(firstregion.size() + secondregion.size() == veryLocalN);
	return maxGain;
}


template<typename IndexType, typename ValueType>
DenseVector<IndexType> ParcoRepart<IndexType, ValueType>::getBorderNodes( const CSRSparseMatrix<ValueType> &adjM, const DenseVector<IndexType> &part) {

    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr();
    const scai::utilskernel::LArray<IndexType> localPart= part.getLocalValues();
    DenseVector<IndexType> border(dist,0);
    scai::utilskernel::LArray<IndexType> localBorder= border.getLocalValues();
    
    IndexType N = adjM.getNumColumns();
    IndexType max = part.max().Scalar::getValue<IndexType>();
    
    if( !dist->isEqual( part.getDistribution() ) ){
        std::cout<< __FILE__<< "  "<< __LINE__<< ", matrix dist: " << *dist<< " and partition dist: "<< part.getDistribution() << std::endl;
        throw std::runtime_error( "Distributions: should (?) be equal.");
    }
    //TODO: Use DenseVector from start: localV and nonLocalV sizes are not known beforehand. That is why I use a std::vector and then convert it to a DenseVector. 
    std::vector<IndexType> localV, nonLocalV;
    for(IndexType i=0; i<dist->getLocalSize(); i++){    // for all local nodes 
        scai::hmemo::HArray<ValueType> localRow;        // get local row on this processor
        adjM.getLocalRow( localRow, i);
        scai::hmemo::ReadAccess<ValueType> readLR(localRow); 
        assert(readLR.size() == adjM.getNumColumns());
        for(IndexType j=0; j<N; j++){                   // for all the edges of a node
            ValueType val;
            readLR.getValue(val, j);      
            if(val>0){                                  // i and j have an edge               
                if(dist->isLocal(j)){      
                    assert( localPart[ dist->global2local(j) ] < max +1 );
                    if( localPart[i] != localPart[ dist->global2local(j) ] ){ // i and j are in different parts
                        localBorder[i] = 1;             // then i is a border node
                        break;                          // if this is a border node then break 
                    }
                } else{     // if j is not local index in this PE, store the indices and gather later
                    localV.push_back(i);
                    nonLocalV.push_back(j);
                }
            }
        }
    }

    // take care of all the non-local indices found
    assert( localV.size() == nonLocalV.size() );
    DenseVector<IndexType> nonLocalDV( nonLocalV.size() , 0 );
    DenseVector<IndexType> gatheredPart(nonLocalDV.size() , 0);
    
    //get a DenseVector grom a vector
    for(IndexType i=0; i<nonLocalV.size(); i++){
        nonLocalDV.setValue(i, nonLocalV[i]);
    }
    //gather all non-local indexes
    gatheredPart.gather(part, nonLocalDV , scai::utilskernel::binary::COPY );

    assert( localV.size()==nonLocalDV.size() );
    assert( nonLocalDV.size()==gatheredPart.size() );
    for(IndexType i=0; i<gatheredPart.size(); i++){
        if(localPart[ localV[i]] != gatheredPart(i).Scalar::getValue<IndexType>()  ){
            localBorder[localV[i]]=1;
        }
    }
   
    border.setValues(localBorder);
    return border;
}

//----------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
scai::lama::CSRSparseMatrix<ValueType> ParcoRepart<IndexType, ValueType>::getPEGraph( const CSRSparseMatrix<ValueType> &adjM) {

    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr(); 
    
    //std::cout<< __FILE__<< " ,"<<__LINE__<<": matrix distribution:"<< *dist<< std::endl;
    
    scai::hmemo::HArray<IndexType> indicesH(0); 
    scai::hmemo::WriteAccess<IndexType> indicesHWrite(indicesH);

    for(IndexType i=0; i<dist->getLocalSize(); i++){    // for all local nodes 
        scai::hmemo::HArray<ValueType> localRow;        // get local row on this processor
        adjM.getLocalRow( localRow, i);
        scai::hmemo::ReadAccess<ValueType> readLR(localRow); 
        assert(readLR.size() == adjM.getNumColumns());  
        for(IndexType j=0; j<readLR.size(); j++){       // for all the edges of a node
            ValueType val;
            readLR.getValue(val, j);
            if(val>0){                                  // i and j have an edge             
                if( !dist->isLocal(j) ){                // if j is not a local node
                    indicesHWrite.resize( indicesH.size() +1);  
                    indicesHWrite[indicesHWrite.size()-1] = j;  // store the non local index
                }
            }
        }
    }
    indicesHWrite.release();
  
    // find the PEs that own every non-local index
    scai::hmemo::HArray<IndexType> owners(indicesH.size() , -1);
    dist->computeOwners( owners, indicesH);
    
    // create the PE adjacency matrix to be returned
    IndexType numPEs = comm->getSize();
    scai::dmemo::DistributionPtr distPEs ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, numPEs) );  
    scai::dmemo::DistributionPtr noDistPEs (new scai::dmemo::NoDistribution( numPEs ));
    // every PE must have one row of the matrix since we have numPes and the matrix is [numPes x numPEs]

    scai::lama::SparseAssemblyStorage<ValueType> myStorage( distPEs->getLocalSize(), numPEs);
    //scai::lama::MatrixStorage<ValueType> myStorage( distPEs->getLocalSize(), numPEs);
    scai::hmemo::ReadAccess<IndexType> readI(owners);
    for(IndexType i=0; i<readI.size(); i++){
        myStorage.setValue(0, readI[i], 1);
    }
    readI.release();
     
    scai::lama::CSRSparseMatrix<ValueType> PEgraph(myStorage, distPEs, noDistPEs);     

    return PEgraph;
}

//-----------------------------------------------------------------------------------------

//return: there is an edge is the block graph between blocks ret[0]-ret[1], ret[2]-ret[3] ... ret[2i]-ret[2i+1] 
template<typename IndexType, typename ValueType>
std::vector<std::vector<IndexType>> ParcoRepart<IndexType, ValueType>::getLocalBlockGraphEdges( const CSRSparseMatrix<ValueType> &adjM, const DenseVector<IndexType> &part) {
    
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr();
    const scai::utilskernel::LArray<IndexType> localPart= part.getLocalValues();
    IndexType N = adjM.getNumColumns();
    IndexType max = part.max().Scalar::getValue<IndexType>();
   
    if( !dist->isEqual( part.getDistribution() ) ){
        std::cout<< __FILE__<< "  "<< __LINE__<< ", matrix dist: " << *dist<< " and partition dist: "<< part.getDistribution() << std::endl;
        throw std::runtime_error( "Distributions: should (?) be equal.");
    }
    
    // edges[i,j] if an edge exists between blocks i and j
    std::vector< std::vector<IndexType> > edges(2);
    std::vector<IndexType> localInd, nonLocalInd;
    
    for(IndexType i=0; i<dist->getLocalSize(); i++){    // for all local nodes 
        scai::hmemo::HArray<ValueType> localRow;        // get local row on this processor
        adjM.getLocalRow( localRow, i);
        scai::hmemo::ReadAccess<ValueType> readLR(localRow); 
        assert(readLR.size() == adjM.getNumColumns()); 
        for(IndexType j=0; j<readLR.size(); j++){       // for all the edges of a node
            ValueType val;
            readLR.getValue(val, j);
            if(val>0){                                  // i and j have an edge             
                //TODO: probably part.getValue(j) is not correct. If true the assertion should fail at some point.
                //edge (u,v) if the block graph
                //TODO: remove part.getValue(j) but now only gets an edge if both i and j are local. 
                //      when j is not local???
                if(dist->isLocal(j)){
                    IndexType u = localPart[i];         // partition(i)
                    IndexType v = localPart[dist->global2local(j)]; // partition(j), 0<j<N so take the local index of j
//std::cout<<  __FILE__<< " ,"<<__LINE__<<" == "<< i <<", " << j <<":  __"<< *comm<< " , u=" << u<< " , v="<< v << std::endl;   
                    assert( u < max +1);
                    assert( v < max +1);
                    if( u != v){    // the nodes belong to different blocks                  
                        bool add_edge = true;
                        for(IndexType k=0; k<edges[0].size(); k++){ //check that this edge is not already in
// std::cout<<  __FILE__<< " ,"<<__LINE__<<" == k="<< k <<":  __"<< *comm<< " , u=" << edges[0][k]<< " , v="<< edges[1][k] << std::endl;                            
                            if( edges[0][k]==u && edges[1][k]==v ){
                                add_edge= false;
                                break;      // the edge (u,v) already exists
                            }
                        }
                        if( add_edge== true){       //if this edge does not exist, add it
//std::cout<<  __FILE__<< " ,"<<__LINE__<< "\t __"<< *comm<<",  adding edge ("<< u<< ","<< v<< ")\n";    
                            edges[0].push_back(u);
                            edges[1].push_back(v);
                        }
                    }
                } else{  // if(dist->isLocal(j)) , what TODO when j is not local?
                // there is an edge between i and j but index j is not local in the partition so we cannot get part[j].
                    localInd.push_back(i);
                    nonLocalInd.push_back(j);
                }
            }
        }
    }//for 
    
    // take care of all the non-local indices found
    assert( localInd.size() == nonLocalInd.size() );
    DenseVector<IndexType> nonLocalDV( nonLocalInd.size() , 0 );
    DenseVector<IndexType> gatheredPart(nonLocalDV.size() , 0);
    
    //get a DenseVector grom a vector
    for(IndexType i=0; i<nonLocalInd.size(); i++){
        nonLocalDV.setValue(i, nonLocalInd[i]);
    }
    //gather all non-local indexes
    gatheredPart.gather(part, nonLocalDV , scai::utilskernel::binary::COPY );
    
    assert( gatheredPart.size() == nonLocalInd.size() );
    assert( gatheredPart.size() == localInd.size() );
    
    for(IndexType i=0; i<gatheredPart.size(); i++){
        IndexType u = localPart[ localInd[i] ];         
        IndexType v = gatheredPart.getValue(i).Scalar::getValue<IndexType>();
        //std::cout<<  __FILE__<< " ,"<<__LINE__<<" == "<< i <<", " << j <<":  __"<< *comm<< " , u=" << u<< " , v="<< v << std::endl;   
        assert( u < max +1);
        assert( v < max +1);
        if( u != v){    // the nodes belong to different blocks                  
            bool add_edge = true;
            for(IndexType k=0; k<edges[0].size(); k++){ //check that this edge is not already in
                // std::cout<<  __FILE__<< " ,"<<__LINE__<<" == k="<< k <<":  __"<< *comm<< " , u=" << edges[0][k]<< " , v="<< edges[1][k] << std::endl;                            
                if( edges[0][k]==u && edges[1][k]==v ){
                    add_edge= false;
                    break;      // the edge (u,v) already exists
                }
            }
            if( add_edge== true){       //if this edge does not exist, add it
                //std::cout<<  __FILE__<< " ,"<<__LINE__<< "\t __"<< *comm<<",  adding edge ("<< u<< ","<< v<< ")\n";    
                edges[0].push_back(u);
                edges[1].push_back(v);
            }
        }
    }
    return edges;
}

//-----------------------------------------------------------------------------------------

// in this version the graph is an HArray with size k*k and [i,j] = i*k+j
//
// Not distributed.
//
template<typename IndexType, typename ValueType>
scai::lama::CSRSparseMatrix<ValueType> ParcoRepart<IndexType, ValueType>::getBlockGraph( const CSRSparseMatrix<ValueType> &adjM, const DenseVector<IndexType> &part, const int k) {

    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    const scai::dmemo::DistributionPtr distPtr = adjM.getRowDistributionPtr();
    const scai::utilskernel::LArray<IndexType> localPart= part.getLocalValues();
    
    // IndexType k = part.max(); // if K is not given as a parameter
    // there are k blocks in the partition so the adjecency matrix for the block graph has dimensions [k x k]
    scai::dmemo::DistributionPtr distRowBlock ( scai::dmemo::Distribution::getDistributionPtr( "BLOCK", comm, k) );  
    scai::dmemo::DistributionPtr distColBlock ( new scai::dmemo::NoDistribution( k ));
    //scai::lama::CSRSparseMatrix<ValueType> blockGraph( distRowBlock, distColBlock );    // the distributed adjacency matrix of the block-graph
    
    IndexType size= k*k;
    // get, on each processor, the edges of the blocks that are local
    std::vector< std::vector<IndexType> > blockEdges = ParcoRepart<int, double>::getLocalBlockGraphEdges( adjM, part);
    
    // the gather function accepts a one dimensional array
    //IndexType blockGraphMatrix[size];    
    scai::hmemo::HArray<IndexType> sendPart(size, static_cast<ValueType>( 0 ));
    scai::hmemo::HArray<IndexType> recvPart(size);
    
    for(IndexType round=0; round<comm->getSize(); round++){
        {   // write your part 
            scai::hmemo::WriteAccess<IndexType> sendPartWrite( sendPart );
            for(IndexType i=0; i<blockEdges[0].size(); i++){
                IndexType u = blockEdges[0][i];
                IndexType v = blockEdges[1][i];
//std::cout<<__FILE__<< "  "<< __LINE__<< " , round:"<< round<< " , __"<< *comm << " setting edge ("<< u<< ", "<< v << ") , in send-index:"<< u*k+v <<std::endl;
                sendPartWrite[ u*k + v ] = 1;
            }
        }
        comm->shiftArray(recvPart , sendPart, 1);
        sendPart.swap(recvPart);
    } 
    
    // get numEdges
    IndexType numEdges=0;
    {
        scai::hmemo::ReadAccess<IndexType> recvPartRead( recvPart );
        for(IndexType i=0; i<recvPartRead.size(); i++){
            if( recvPartRead[i]>0 )
                ++numEdges;
        }
    }
    
    IndexType localBlockGraph [k][k];
    
    for(IndexType i=0; i<blockEdges[0].size(); i++){
        IndexType u = blockEdges[0][i];
        IndexType v = blockEdges[1][i];
std::cout<<__FILE__<< "  "<< __LINE__<< " , __"<< *comm << " setting edge ("<< u<< ", "<< v << ")"<<std::endl;        
        localBlockGraph[u][v] = 1;
    }
    
    //convert the k*k HArray to a [k x k] CSRSparseMatrix
    scai::lama::CSRStorage<ValueType> localMatrix;
    localMatrix.allocate( k ,k );
    
    scai::hmemo::HArray<IndexType> csrIA;
    scai::hmemo::HArray<IndexType> csrJA;
    scai::hmemo::HArray<ValueType> csrValues; 
    {
        IndexType numNZ = numEdges;     // this equals the number of edges of the graph
std::cout<< std::endl<< __FILE__<< "  "<< __LINE__<< ", __"<< *comm << " , numNZ="<< numNZ << " <> numEdges_2=" << numEdges<< std::endl;         
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
    
    scai::lama::CSRSparseMatrix<ValueType> matrix;
    localMatrix.swap( csrIA, csrJA, csrValues );
    matrix.assign(localMatrix);
    
    return matrix;
}

//-----------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
CSRSparseMatrix<ValueType> ParcoRepart<IndexType, ValueType>::getGraphEdgeColoring_local( const CSRSparseMatrix<ValueType> &adjM) {
    using namespace boost;
    IndexType N= adjM.getNumRows();
    assert( N== adjM.getNumColumns() ); // numRows = numColumns
    
    // use boost::Graph and boost::edge_coloring()
    typedef adjacency_list<vecS, vecS, undirectedS, no_property, size_t, no_property> Graph;
    typedef std::pair<std::size_t, std::size_t> Pair;
    std::vector<std::vector<IndexType>> edges(2);
    Graph G(N);
    
    for(IndexType i=0; i<N; i++){
        for(IndexType j=0; j<N; j++){
            if(adjM(i, j)== 1){ // there is an edge between nodes i and j. add the edge to G
                boost::add_edge(i, j, G).first;
                edges[0].push_back(i);  
                edges[1].push_back(j);
            }
        }
    }
    
    size_t colors = boost::edge_coloring(G, boost::get( boost::edge_bundle, G));
    
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    std::cout << *comm << ", Colored using " << colors << " colors" << std::endl;
    for (size_t i = 0; i <edges[0].size(); i++) {
        std::cout << "  " <<  edges[0][i] << "-" << edges[1][i] << ": " << \
        G[ boost::edge( edges[0][i],  edges[1][i], G).first] << std::endl;
    }
    CSRSparseMatrix<ValueType> ret;
    return ret;
}

//---------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
std::vector<IndexType> ParcoRepart<IndexType, ValueType>::getGraphEdgeColoring_local( const std::vector<std::vector<IndexType>> &edgeList) {
    using namespace boost;
    assert( edgeList.size() == 2);
    IndexType N= edgeList[0].size();
    
    // use boost::Graph and boost::edge_coloring()
    typedef adjacency_list<vecS, vecS, undirectedS, no_property, size_t, no_property> Graph;
    typedef std::pair<std::size_t, std::size_t> Pair;
    Graph G(N);
    
    size_t colors = boost::edge_coloring(G, boost::get( boost::edge_bundle, G));
    
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    std::vector<IndexType> ret;
    std::cout << *comm << ", Colored using " << colors << " colors" << std::endl;
    for (size_t i = 0; i <edgeList[0].size(); i++) {
        std::cout << "  " <<  edgeList[0][i] << "-" << edgeList[1][i] << ": " << \
        G[ boost::edge( edgeList[0][i],  edgeList[1][i], G).first] << std::endl;
        ret.push_back( G[ boost::edge( edgeList[0][i],  edgeList[1][i], G).first] );
    }
    
    return ret;
}

//---------------------------------------------------------------------------------------

//to force instantiation
template DenseVector<int> ParcoRepart<int, double>::partitionGraph(CSRSparseMatrix<double> &input, std::vector<DenseVector<double>> &coordinates, int k,  double epsilon);

template double ParcoRepart<int, double>::getMinimumNeighbourDistance(const CSRSparseMatrix<double> &input, const std::vector<DenseVector<double>> &coordinates, int dimensions);
			     
template double ParcoRepart<int, double>::computeImbalance(const DenseVector<int> &partition, int k);

template double ParcoRepart<int, double>::computeCut(const CSRSparseMatrix<double> &input, const DenseVector<int> &part, bool ignoreWeights);

template double ParcoRepart<int, double>::replicatedMultiWayFM(const CSRSparseMatrix<double> &input, DenseVector<int> &part, int k, double epsilon, bool unweighted);

template std::vector<DenseVector<int>> ParcoRepart<int, double>::computeCommunicationPairings(const CSRSparseMatrix<double> &input, const DenseVector<int> &part,	const DenseVector<int> &blocksToPEs);

template std::vector<int> ITI::ParcoRepart<int, double>::nonLocalNeighbors(const CSRSparseMatrix<double>& input);

template scai::dmemo::Halo ITI::ParcoRepart<int, double>::buildMatrixHalo(const CSRSparseMatrix<double> &input);

template scai::dmemo::Halo ITI::ParcoRepart<int, double>::buildPartHalo(const CSRSparseMatrix<double> &input,  const DenseVector<int> &part);

template std::pair<std::vector<int>, int> ITI::ParcoRepart<int, double>::getInterfaceNodes(const CSRSparseMatrix<double> &input, const DenseVector<int> &part, int thisBlock, int otherBlock, int depth);

template DenseVector<int> ParcoRepart<int, double>::getBorderNodes( const CSRSparseMatrix<double> &adjM, const DenseVector<int> &part);

template scai::lama::CSRSparseMatrix<double> ParcoRepart<int, double>::getPEGraph( const CSRSparseMatrix<double> &adjM);

template std::vector<std::vector<IndexType>> ParcoRepart<int, double>::getLocalBlockGraphEdges( const CSRSparseMatrix<double> &adjM, const DenseVector<int> &part);

template scai::lama::CSRSparseMatrix<double> ParcoRepart<int, double>::getBlockGraph( const CSRSparseMatrix<double> &adjM, const DenseVector<int> &part, const int k );

template scai::lama::CSRSparseMatrix<double>   ParcoRepart<int, double>::getGraphEdgeColoring_local( const CSRSparseMatrix<double> &adjM);

template std::vector<IndexType> ParcoRepart<int, double>::getGraphEdgeColoring_local( const std::vector<std::vector<IndexType>> &edgeList );

}
