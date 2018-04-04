/*
 * KMeans.cpp
 *
 *  Created on: 19.07.2017
 *      Author: moritz
 */

#include <set>
#include <cmath>
#include <assert.h>
#include <algorithm>

#include <scai/dmemo/NoDistribution.hpp>
#include <scai/dmemo/GenBlockDistribution.hpp>

#include "KMeans.h"
#include "HilbertCurve.h"

namespace ITI {
namespace KMeans {

using scai::lama::Scalar;

template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType> > findInitialCentersSFC(
		const std::vector<DenseVector<ValueType> >& coordinates, IndexType k, const std::vector<ValueType> &minCoords,
		const std::vector<ValueType> &maxCoords, Settings settings) {

	SCAI_REGION( "KMeans.findInitialCentersSFC" );
	const IndexType localN = coordinates[0].getLocalValues().size();
	const IndexType globalN = coordinates[0].size();
	const IndexType dimensions = settings.dimensions;
	
	//convert coordinates, switch inner and outer order
	std::vector<std::vector<ValueType> > convertedCoords(localN);
	for (IndexType i = 0; i < localN; i++) {
		convertedCoords[i].resize(dimensions);
	}

	for (IndexType d = 0; d < dimensions; d++) {
		scai::hmemo::ReadAccess<ValueType> rAccess(coordinates[d].getLocalValues());
		assert(rAccess.size() == localN);
		for (IndexType i = 0; i < localN; i++) {
			convertedCoords[i][d] = rAccess[i];
		}
	}

	//get local hilbert indices
	std::vector<ValueType> sfcIndices = HilbertCurve<IndexType, ValueType>::getHilbertIndexVector( coordinates, settings.sfcResolution, settings.dimensions);
	SCAI_ASSERT_EQ_ERROR( sfcIndices.size(), localN, "wrong local number of indices (?) ");

	//prepare indices for sorting
	std::vector<IndexType> localIndices(localN);
	const typename std::vector<IndexType>::iterator firstIndex = localIndices.begin();
	typename std::vector<IndexType>::iterator lastIndex = localIndices.end();;
	std::iota(firstIndex, lastIndex, 0);

	//sort local indices according to SFC
	std::sort(localIndices.begin(), localIndices.end(), [&sfcIndices](IndexType a, IndexType b){return sfcIndices[a] < sfcIndices[b];});

	//compute wanted indices for initial centers
	std::vector<IndexType> wantedIndices(k);

	for (IndexType i = 0; i < k; i++) {
		wantedIndices[i] = i * (globalN / k) + (globalN / k)/2;
	}

	//setup general block distribution to model the space-filling curve
	scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
	scai::dmemo::DistributionPtr blockDist(new scai::dmemo::GenBlockDistribution(globalN, localN, comm));

	//set local values in vector, leave non-local values with zero
	std::vector<std::vector<ValueType> > result(dimensions);
	for (IndexType d = 0; d < dimensions; d++) {
		result[d].resize(k);
	}

	for (IndexType j = 0; j < k; j++) {
		IndexType localIndex = blockDist->global2local(wantedIndices[j]);
		if (localIndex != nIndex) {
			assert(localIndex < localN);
			IndexType permutedIndex = localIndices[localIndex];
			assert(permutedIndex < localN);
			assert(permutedIndex >= 0);
			for (IndexType d = 0; d < dimensions; d++) {
				result[d][j] = convertedCoords[permutedIndex][d];
			}
		}
	}

	//global sum operation
	for (IndexType d = 0; d < dimensions; d++) {
		comm->sumImpl(result[d].data(), result[d].data(), k, scai::common::TypeTraits<ValueType>::stype);
	}

	return result;
}



template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType> > findInitialCentersFromSFCOnly( const IndexType k, const std::vector<ValueType> &maxCoords, Settings settings){
	
	const IndexType dimensions = settings.dimensions;
		
	//set local values in vector, leave non-local values with zero
	std::vector<std::vector<ValueType> > result(dimensions);
	for (IndexType d = 0; d < dimensions; d++) {
		result[d].resize(k);
	}
	
	ValueType offset = 1.0/(ValueType(k)*2.0);
	std::vector<ValueType> centerCoords(dimensions,0);
	for (IndexType i = 0; i < k; i++) {
		ValueType centerHilbInd = i/ValueType(k) + offset;
//PRINT( centerHilbInd );		
		centerCoords = HilbertCurve<IndexType,ValueType>::HilbertIndex2PointVec( centerHilbInd, settings.sfcResolution, settings.dimensions);
		SCAI_ASSERT_EQ_ERROR( centerCoords.size(), dimensions, "Wrong dimensions for center.");
		
		for (IndexType d = 0; d < dimensions; d++) {
			result[d][i] = centerCoords[d]*maxCoords[d];
		}
	}
	return result;
}


template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType> > findInitialCenters(
		const std::vector<DenseVector<ValueType> >& coordinates, IndexType k, const DenseVector<ValueType> &nodeWeights) {

	SCAI_REGION( "KMeans.findInitialCenters" );

	const IndexType dim = coordinates.size();
	const IndexType n = coordinates[0].size();
	//const IndexType localN = coordinates[0].getLocalValues().size();

	std::vector<std::vector<ValueType> > result(dim);

	for (IndexType d = 0; d < dim; d++) {
		result[d].resize(k);
	}

	std::vector<IndexType> indices(k);

	for (IndexType i = 0; i < k; i++) {
		indices[i] = i * (n / k);
	}

	for (IndexType d = 0; d < dim; d++) {

		IndexType i = 0;
		for (IndexType index : indices) {
			//yes, this is very expensive, since each call triggers a global communication. However, this needs to be done anyway, if not here then later.
			result[d][i] = coordinates[d].getValue(index).Scalar::getValue<ValueType>();
			i++;
		}
	}

	//alternative:
	//sort coordinates according to x coordinate first, then y coordinate. The schizoQuicksort currently only supports one sorting key, have to extend that.

	return result;
}


template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType> > findLocalCenters(	const std::vector<DenseVector<ValueType> >& coordinates, const DenseVector<ValueType> &nodeWeights) {
		
	const IndexType dim = coordinates.size();
	//const IndexType n = coordinates[0].size();
	const IndexType localN = coordinates[0].getLocalValues().size();
	
	// get sum of local weights 
		
	scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights.getLocalValues());
	SCAI_ASSERT_EQ_ERROR( rWeights.size(), localN, "Mismatch of nodeWeights and coordinates size. Chech distributions.");
	
	ValueType localWeightSum = 0;
	for (IndexType i=0; i<localN; i++) {
		localWeightSum += rWeights[i];
	}
		
	std::vector<ValueType> localCenter( dim , 0 );
	
	for (IndexType d = 0; d < dim; d++) {
		scai::hmemo::ReadAccess<ValueType> rCoords( coordinates[d].getLocalValues() );
		for (IndexType i=0; i<localN; i++) {
			//this is more expensive than summing first and dividing later, but avoids overflows
			localCenter[d] += rWeights[i]*rCoords[i]/localWeightSum;
		}
	}
	
	// vector of size k for every center. Each PE only stores its local center and sum center
	// so all centers are replicated in all PEs
	
	const scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistribution().getCommunicatorPtr();
	const IndexType numPEs = comm->getSize();
	const IndexType thisPE = comm->getRank();
	std::vector<std::vector<ValueType> > result( dim, std::vector<ValueType>(numPEs,0) );
	for (IndexType d=0; d<dim; d++){
		result[d][thisPE] = localCenter[d];
	}
	
	for (IndexType d=0; d<dim; d++){
		comm->sumImpl(result[d].data(), result[d].data(), numPEs, scai::common::TypeTraits<ValueType>::stype);
	}
	return result;
}


template<typename IndexType, typename ValueType, typename Iterator>
std::vector<std::vector<ValueType> > findCenters(
		const std::vector<DenseVector<ValueType> >& coordinates,
		const DenseVector<IndexType>& partition,
		const IndexType k,
		const Iterator firstIndex,
		const Iterator lastIndex,
		const DenseVector<ValueType>& nodeWeights) {
	SCAI_REGION( "KMeans.findCenters" );

	const IndexType dim = coordinates.size();
	//const IndexType n = partition.size();
	//const IndexType localN = partition.getLocalValues().size();
	const scai::dmemo::DistributionPtr resultDist(new scai::dmemo::NoDistribution(k));
	const scai::dmemo::CommunicatorPtr comm = partition.getDistribution().getCommunicatorPtr();

	//TODO: check that distributions align

	std::vector<std::vector<ValueType> > result(dim);
	std::vector<IndexType> weightSum(k, 0);

	scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights.getLocalValues());
	scai::hmemo::ReadAccess<IndexType> rPartition(partition.getLocalValues());

	//compute weight sums
	for (Iterator it = firstIndex; it != lastIndex; it++) {
		const IndexType i = *it;
		const IndexType part = rPartition[i];
		const IndexType weight = rWeights[i];
		weightSum[part] += weight;
	}

	//find local centers
	for (IndexType d = 0; d < dim; d++) {
		result[d].resize(k);
		scai::hmemo::ReadAccess<ValueType> rCoords(coordinates[d].getLocalValues());

		for (Iterator it = firstIndex; it != lastIndex; it++) {
			const IndexType i = *it;
			const IndexType part = rPartition[i];
			//const IndexType weight = rWeights[i];
			result[d][part] += rCoords[i]*rWeights[i] / weightSum[part];//this is more expensive than summing first and dividing later, but avoids overflows
		}
	}

	//communicate local centers and weight sums
	std::vector<IndexType> totalWeight(k, 0);
	comm->sumImpl(totalWeight.data(), weightSum.data(), k, scai::common::TypeTraits<IndexType>::stype);

	//compute updated centers as weighted average
	for (IndexType d = 0; d < dim; d++) {
		for (IndexType j = 0; j < k; j++) {
			ValueType weightRatio = (ValueType(weightSum[j]) / totalWeight[j]);

			ValueType weightedCoord = weightSum[j] == 0 ? 0 : result[d][j] * weightRatio;
			result[d][j] = weightedCoord;
			assert(std::isfinite(result[d][j]));

			// make empty clusters explicit
			if (totalWeight[j] == 0) {
			    result[d][j] = NAN;
			}
		}
		comm->sumImpl(result[d].data(), result[d].data(), k, scai::common::TypeTraits<ValueType>::stype);
	}

	return result;
}

template<typename IndexType, typename ValueType>
DenseVector<IndexType> assignBlocks(const std::vector<DenseVector<ValueType> >& coordinates,
		const std::vector<std::vector<ValueType> >& centers) {

	const IndexType dim = coordinates.size();
	assert(dim > 0);
	assert(centers.size() == dim);
	//const IndexType n = coordinates[0].size();
	const IndexType localN = coordinates[0].getLocalValues().size();
	const IndexType k = centers[0].size();

	DenseVector<IndexType> assignment(coordinates[0].getDistributionPtr(), 0);

	std::vector<std::vector<ValueType>> squaredDistances(k);
	for (std::vector<ValueType>& sublist : squaredDistances) {
		sublist.resize(localN, 0);
	}

	{
		SCAI_REGION( "KMeans.assignBlocks.squaredDistances" );
		//compute squared distances. Since only the nearest neighbor is required, we could speed this up with a suitable data structure.
		for (IndexType d = 0; d < dim; d++) {
			scai::hmemo::ReadAccess<ValueType> rCoords(coordinates[d].getLocalValues());
			for (IndexType j = 0; j < k; j++) {
				ValueType centerCoord = centers[d][j];
				for (IndexType i = 0; i < localN; i++) {
					ValueType coord = rCoords[i];
					ValueType dist = std::abs(centerCoord - coord);
					squaredDistances[j][i] += dist*dist; //maybe use Manhattan distance here? Should align better with grid structure.
				}
			}
		}

		scai::hmemo::WriteAccess<IndexType> wAssignment(assignment.getLocalValues());
		{
			SCAI_REGION( "KMeans.assignBlocks.balanceLoop.assign" );
			for (IndexType i = 0; i < localN; i++) {
				int bestBlock = 0;
				int bestValue = squaredDistances[bestBlock][i];
				for (IndexType j = 0; j < k; j++) {
					if (squaredDistances[j][i] < bestValue) {
						bestBlock = j;
						bestValue = squaredDistances[bestBlock][i];
					}
				}
				wAssignment[i] = bestBlock;
			}
		}
	}
	return assignment;
}

template<typename IndexType, typename ValueType, typename Iterator>
DenseVector<IndexType> assignBlocks(
		const std::vector<std::vector<ValueType> >& coordinates,
		const std::vector<std::vector<ValueType> >& centers,
		const Iterator firstIndex,
		const Iterator lastIndex,
		const DenseVector<ValueType> &nodeWeights,
		const DenseVector<IndexType> &previousAssignment,
		const std::vector<IndexType> &targetBlockSizes,
		const SpatialCell &boundingBox,
		std::vector<ValueType> &upperBoundOwnCenter,
		std::vector<ValueType> &lowerBoundNextCenter,
		std::vector<ValueType> &influence,
		Settings settings) {
	SCAI_REGION( "KMeans.assignBlocks" );

	const IndexType dim = coordinates.size();
	const scai::dmemo::DistributionPtr dist = nodeWeights.getDistributionPtr();
	const scai::dmemo::CommunicatorPtr comm = dist->getCommunicatorPtr();
//	const IndexType localN = nodeWeights.getLocalValues().size();
	const IndexType k = targetBlockSizes.size();

	assert(influence.size() == k);

	//compute assignment and balance
	DenseVector<IndexType> assignment = previousAssignment;

	//pre-filter possible closest blocks
	std::vector<ValueType> effectiveMinDistance(k);
	std::vector<ValueType> minDistance(k);
	{
		SCAI_REGION( "KMeans.assignBlocks.filterCenters" );
		for (IndexType j = 0; j < k; j++) {
			std::vector<ValueType> center(dim);
			//TODO: this conversion into points is annoying. Maybe change coordinate format and use n in the outer dimension and d in the inner?
			//Can even use points as data structure. Update: Tried it, gave no performance benefit.
			for (IndexType d = 0; d < dim; d++) {
				center[d] = centers[d][j];
			}
			minDistance[j] = boundingBox.distances(center).first;
			assert(std::isfinite(minDistance[j]));
			effectiveMinDistance[j] = minDistance[j]*minDistance[j]*influence[j];
			assert(std::isfinite(effectiveMinDistance[j]));
		}
	}

	std::vector<IndexType> clusterIndices(k);
	std::iota(clusterIndices.begin(), clusterIndices.end(), 0);
	std::sort(clusterIndices.begin(), clusterIndices.end(),
			[&effectiveMinDistance](IndexType a, IndexType b){return effectiveMinDistance[a] < effectiveMinDistance[b] || (effectiveMinDistance[a] == effectiveMinDistance[b] && a < b);});
	std::sort(effectiveMinDistance.begin(), effectiveMinDistance.end());

	for (IndexType i = 0; i < k; i++) {
		IndexType c = clusterIndices[i];
		ValueType effectiveDist = minDistance[c]*minDistance[c]*influence[c];
		SCAI_ASSERT_LT_ERROR( std::abs(effectiveMinDistance[i] - effectiveDist), 1e-5, "effectiveMinDistance[" << i << "] = " << effectiveMinDistance[i] << " != " << effectiveDist << " = effectiveDist");
	}

	ValueType localSampleWeightSum = 0;
	{
		scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights.getLocalValues());

		for (Iterator it = firstIndex; it != lastIndex; it++) {
			localSampleWeightSum += rWeights[*it];
		}
	}
	const ValueType totalWeightSum = comm->sum(localSampleWeightSum);
	ValueType optSize = std::ceil(totalWeightSum / k );

	ValueType imbalance;
	IndexType iter = 0;
	std::vector<bool> influenceGrew(k);
	std::vector<ValueType> influenceChangeUpperBound(k,1+settings.influenceChangeCap);
	std::vector<ValueType> influenceChangeLowerBound(k,1-settings.influenceChangeCap);

	//iterate if necessary to achieve balance
	do
	{
		std::chrono::time_point<std::chrono::high_resolution_clock> balanceStart = std::chrono::high_resolution_clock::now();
		SCAI_REGION( "KMeans.assignBlocks.balanceLoop" );
		std::vector<ValueType> blockWeights(k,0.0);
		IndexType totalComps = 0;
		IndexType skippedLoops = 0;
		IndexType balancedBlocks = 0;
		scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights.getLocalValues());
		scai::hmemo::WriteAccess<IndexType> wAssignment(assignment.getLocalValues());
		{
			SCAI_REGION( "KMeans.assignBlocks.balanceLoop.assign" );
			for (Iterator it = firstIndex; it != lastIndex; it++) {
				const IndexType i = *it;
				const IndexType oldCluster = wAssignment[i];
				if (lowerBoundNextCenter[i] > upperBoundOwnCenter[i]) {
					//std::cout << upperBoundOwnCenter[i] << " " << lowerBoundNextCenter[i] << " " << distThreshold[oldCluster] << std::endl;
					//cluster assignment cannot have changed.
					//wAssignment[i] = wAssignment[i];
					skippedLoops++;
				} else {
					ValueType sqDistToOwn = 0;
					for (IndexType d = 0; d < dim; d++) {
						sqDistToOwn += std::pow(centers[d][oldCluster] - coordinates[d][i], 2);
					}
					ValueType newEffectiveDistance = sqDistToOwn*influence[oldCluster];
					assert(upperBoundOwnCenter[i] >= newEffectiveDistance);
					upperBoundOwnCenter[i] = newEffectiveDistance;
					if (lowerBoundNextCenter[i] > upperBoundOwnCenter[i]) {
						//cluster assignment cannot have changed.
						//wAssignment[i] = wAssignment[i];
						skippedLoops++;
					} else {
						int bestBlock = 0;
						ValueType bestValue = std::numeric_limits<ValueType>::max();
						IndexType secondBest = 0;
						ValueType secondBestValue = std::numeric_limits<ValueType>::max();

						IndexType c = 0;
						while(c < k && secondBestValue > effectiveMinDistance[c]) {
							totalComps++;
							IndexType j = clusterIndices[c];//maybe it would be useful to sort the whole centers array, aligning memory accesses.
							ValueType sqDist = 0;
							//TODO: restructure arrays to align memory accesses better in inner loop
							for (IndexType d = 0; d < dim; d++) {
								ValueType dist = centers[d][j] - coordinates[d][i];
								sqDist += dist*dist;
							}

							const ValueType effectiveDistance = sqDist*influence[j];
							if (effectiveDistance < bestValue) {
								secondBest = bestBlock;
								secondBestValue = bestValue;
								bestBlock = j;
								bestValue = effectiveDistance;
							} else if (effectiveDistance < secondBestValue) {
								secondBest = j;
								secondBestValue = effectiveDistance;
							}
							c++;
						}
						assert(bestBlock != secondBest);
						assert(secondBestValue >= bestValue);
						if (bestBlock != oldCluster) {
							if (bestValue < lowerBoundNextCenter[i]) {
								std::cout << "bestValue: " << bestValue << " lowerBoundNextCenter[" << i << "]: "<< lowerBoundNextCenter[i];
								std::cout << " difference " << std::abs(bestValue - lowerBoundNextCenter[i]) << std::endl;
							}
							assert(bestValue >= lowerBoundNextCenter[i]);
						}

						upperBoundOwnCenter[i] = bestValue;
						lowerBoundNextCenter[i] = secondBestValue;
						wAssignment[i] = bestBlock;
					}
				}
				blockWeights[wAssignment[i]] += rWeights[i];
			}
/*			
			if (settings.verbose) {
				std::chrono::duration<ValueType,std::ratio<1>> balanceTime = std::chrono::high_resolution_clock::now() - balanceStart;			
				ValueType time = balanceTime.count() ;
				std::cout<< comm->getRank()<< ": time " << time << std::endl;
			}
*/			
			comm->synchronize();
		}

		{
			SCAI_REGION( "KMeans.assignBlocks.balanceLoop.blockWeightSum" );
			comm->sumImpl(blockWeights.data(), blockWeights.data(), k, scai::common::TypeTraits<ValueType>::stype);
		}
		
		ValueType maxBlockWeight = *std::max_element(blockWeights.begin(), blockWeights.end());
	
		imbalance = (ValueType(maxBlockWeight - optSize)/ optSize);//TODO: adapt for block sizes

		std::vector<ValueType> oldInfluence = influence;

		double minRatio = std::numeric_limits<double>::max();
		double maxRatio = -std::numeric_limits<double>::min();

		for (IndexType j = 0; j < k; j++) {
			SCAI_REGION( "KMeans.assignBlocks.balanceLoop.influence" );
			double ratio = ValueType(blockWeights[j]) / targetBlockSizes[j];

			if (std::abs(ratio - 1) < settings.epsilon) {
				balancedBlocks++;
				if (settings.freezeBalancedInfluence) {
					if (1 < minRatio) minRatio = 1;
					if (1 > maxRatio) maxRatio = 1;
					continue;
				}
			}

			influence[j] = std::max(influence[j]*influenceChangeLowerBound[j], std::min(influence[j] * std::pow(ratio, settings.influenceExponent), influence[j]*influenceChangeUpperBound[j]));
			assert(influence[j] > 0);

			double influenceRatio = influence[j] / oldInfluence[j];
			assert(influenceRatio <= influenceChangeUpperBound[j] + 1e-10);
			assert(influenceRatio >= influenceChangeLowerBound[j] - 1e-10);
			if (influenceRatio < minRatio) minRatio = influenceRatio;
			if (influenceRatio > maxRatio) maxRatio = influenceRatio;

			if (settings.tightenBounds && iter > 0 && (bool(ratio > 1) != influenceGrew[j])) {
				//influence change switched direction
				influenceChangeUpperBound[j] = 0.1 + 0.9*influenceChangeUpperBound[j];
				influenceChangeLowerBound[j] = 0.1 + 0.9*influenceChangeLowerBound[j];
				//if (comm->getRank() == 0) std::cout << "Block " << j << ": reduced bounds to " << influenceChangeUpperBound[j] << " and " << influenceChangeLowerBound[j] << std::endl;
				assert(influenceChangeUpperBound[j] > 1);
				assert(influenceChangeLowerBound[j] < 1);
			}
			influenceGrew[j] = bool(ratio > 1);
		}

		//update bounds
		{
			SCAI_REGION( "KMeans.assignBlocks.balanceLoop.updateBounds" );
			for (Iterator it = firstIndex; it != lastIndex; it++) {
				const IndexType i = *it;
				const IndexType cluster = wAssignment[i];
				upperBoundOwnCenter[i] *= (influence[cluster] / oldInfluence[cluster]) + 1e-12;
				lowerBoundNextCenter[i] *= minRatio - 1e-12;//TODO: compute separate min ratio with respect to bounding box, only update that.
			}
		}

		//update possible closest centers
		{
			SCAI_REGION( "KMeans.assignBlocks.balanceLoop.filterCenters" );
			for (IndexType j = 0; j < k; j++) {
				effectiveMinDistance[j] = minDistance[j]*minDistance[j]*influence[j];
			}

			std::sort(clusterIndices.begin(), clusterIndices.end(),
						[&effectiveMinDistance](IndexType a, IndexType b){return effectiveMinDistance[a] < effectiveMinDistance[b] || (effectiveMinDistance[a] == effectiveMinDistance[b] && a < b);});
			std::sort(effectiveMinDistance.begin(), effectiveMinDistance.end());

		}

		iter++;
		if (settings.verbose) {
			const IndexType currentLocalN = std::distance(firstIndex, lastIndex);
			const IndexType takenLoops = currentLocalN - skippedLoops;
			const ValueType averageComps = ValueType(totalComps) / currentLocalN;
			//double minInfluence, maxInfluence;
			auto pair = std::minmax_element(influence.begin(), influence.end());
			const ValueType influenceSpread = *pair.second / *pair.first;
			auto oldprecision = std::cout.precision(3);
			if (comm->getRank() == 0) std::cout << "Iter " << iter << ", loop: " << 100*ValueType(takenLoops) / currentLocalN << "%, average comparisons: "
					<< averageComps << ", balanced blocks: " << 100*ValueType(balancedBlocks) / k << "%, influence spread: " << influenceSpread
					<< ", imbalance : " << imbalance << std::endl;
			std::cout.precision(oldprecision);
		}
	} while (imbalance > settings.epsilon - 1e-12 && iter < settings.balanceIterations);
	//std::cout << "Process " << comm->getRank() << " skipped " << ValueType(skippedLoops*100) / (iter*localN) << "% of inner loops." << std::endl;

	return assignment;
}


template<typename IndexType, typename ValueType>
DenseVector<IndexType> getPartitionWithSFCCoords(const scai::lama::CSRSparseMatrix<ValueType>& graph, \
		const std::vector<DenseVector<ValueType> >& coordinates,\
		const DenseVector<ValueType> &  nodeWeights,\
		const Settings settings){
	
	scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
	
	// sorted pair of coordinates and global indices
	std::vector<sort_pair> localHilbertIndices = HilbertCurve<IndexType, ValueType>::getSortedHilbertIndices( coordinates );
	
	//create new distribution based on the hilbert indices
	const IndexType localN = localHilbertIndices.size();
	std::vector<IndexType> newLocalIndices(localN);
	
	const IndexType globalN = comm->sum( localN );
PRINT( *comm<<": localN= " << localN <<	", globlaN= " << globalN);
	// copy indices from the hilbert sfc to a vector
	for(IndexType i=0; i<localN; i++){
		newLocalIndices[i] = localHilbertIndices[i].index;
//PRINT0( *comm << ": " << newLocalIndices[i] );
	}
	
	scai::utilskernel::LArray<IndexType> indexTransport(newLocalIndices.size(), newLocalIndices.data());
	
	// the new distribution from the hilbert indices
	scai::dmemo::DistributionPtr newDistribution(new scai::dmemo::GeneralDistribution(globalN, indexTransport, comm));
	scai::dmemo::GeneralDistribution generalDist( globalN, indexTransport, comm);
		
PRINT(*comm<<": " << *newDistribution);	
{ 
	DenseVector<IndexType> tmpPartition( newDistribution, -1);
	for( int i=0; i< tmpPartition.getLocalValues().size(); i++){
		tmpPartition.getLocalValues()[i]= comm->getRank();
	}
	tmpPartition.redistribute( graph.getRowDistributionPtr() );
	//ITI::aux<IndexType,ValueType>::print2DGrid( graph, tmpPartition );
}
	


	const IndexType dimensions = settings.dimensions;
	
	std::vector<DenseVector<ValueType>> localCoords( dimensions, DenseVector<ValueType>( newDistribution, 0) );
	// nodeWeights must also be redistributed, TODO: can be avoided?
	DenseVector<ValueType> copyNodeWeights = nodeWeights;
	copyNodeWeights.redistribute( newDistribution );
	
	scai::hmemo::HArray<IndexType> localIndices = generalDist.getMyIndexes();
	scai::hmemo::ReadAccess<IndexType> rlocalInd( localIndices );
	//for( int i=0; i<newDistribution->getLocalSize(); i++){
		//PRINT0(*comm <<": local: "<< i<< " , global: " << rlocalInd[i] );
	//}
	
	// convert the hilbert indices to 2D/3D coordinates and copy to a vector<DenseVector>
	{
		SCAI_REGION( "KMeans.getPartitionWithSFCCoords.convert2DV" );

		const IndexType recLevel = 16;	//TODO?: maybe change?

		if( dimensions==2){
			scai::hmemo::WriteAccess<ValueType> wLocalCoords0( localCoords[0].getLocalValues() );
			scai::hmemo::WriteAccess<ValueType> wLocalCoords1( localCoords[1].getLocalValues() );
			
			std::vector<ValueType> point(2, 0);
			
			for(IndexType i=0; i<localN; i++){
				point = HilbertCurve<IndexType,ValueType>::Hilbert2DIndex2PointVec(localHilbertIndices[i].value, recLevel);
				wLocalCoords0[i] = point[0];
				wLocalCoords1[i] = point[1];
//PRINT(*comm <<": "<< i << ", hilbert index= "<< localHilbertIndices[i].value << " >> " << point[0] << ", " << point[1]);				
			}
		}else if (dimensions==3){
			scai::hmemo::WriteAccess<ValueType> wLocalCoords0( localCoords[0].getLocalValues() );
			scai::hmemo::WriteAccess<ValueType> wLocalCoords1( localCoords[1].getLocalValues() );
			scai::hmemo::WriteAccess<ValueType> wLocalCoords2( localCoords[2].getLocalValues() );
			
			std::vector<ValueType> point(3, 0);
			
			for(IndexType i=0; i<localN; i++){
				point = HilbertCurve<IndexType,ValueType>::Hilbert3DIndex2PointVec(localHilbertIndices[i].value, recLevel);
				wLocalCoords0[i] = point[0];
				wLocalCoords1[i] = point[1];
				wLocalCoords2[i] = point[2];
			}			
		}else{
			PRINT0("Dimensions: " << dimensions << " is not supported.\nAborting...");
			throw std::runtime_error("Number of dimensions not supported");
		}
	}
	
	//TODO: assuming uniform block sizes
	const std::vector<IndexType> blockSizes(settings.numBlocks, globalN/settings.numBlocks);
	
	/*
	DenseVector<IndexType> result (newDistribution, 0);
	{	
		for( int i=0; i< result.getLocalValues().size(); i++){
			result.getLocalValues()[i]= comm->getRank();
		}
		
		std::vector<IndexType> localIndices = ITI::GraphUtils::indexReorderCantor(localN);
		typename std::vector<IndexType>::iterator firstIndex, lastIndex;
		firstIndex = localIndices.begin();
		lastIndex = localIndices.end();
		
		std::vector<std::vector<ValueType> > centers = findCenters(localCoords, result, settings.numBlocks, firstIndex, lastIndex, nodeWeights);
		
		result = assignBlocks<IndexType>( localCoords, centers);
	}
	return result;
	*/
	return KMeans::computePartition(localCoords, settings.numBlocks, copyNodeWeights, blockSizes, settings);
	
}


/**
 */
//WARNING: we do not use k as repartition assumes k=comm->getSize() and neither blockSizes and we assume
// 			that every block has the same size

template<typename IndexType, typename ValueType>
DenseVector<IndexType> computeRepartition(const std::vector<DenseVector<ValueType>> &coordinates, const DenseVector<ValueType> &nodeWeights, const Settings settings) {
	
	const IndexType localN = nodeWeights.getLocalValues().size();
	const scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
	const IndexType k = comm->getSize();
	
	// calculate the global weight sum to set the block sizes
	//TODO: the local weight sums are already calculated in findLocalCenters, maybe extract info from there
	ValueType globalWeightSum;
	
	{
		ValueType localWeightSum = 0;
		scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights.getLocalValues());
		SCAI_ASSERT_EQ_ERROR( rWeights.size(), localN, "Mismatch of nodeWeights and coordinates size. Chech distributions.");
		
		for (IndexType i=0; i<localN; i++) {
			localWeightSum += rWeights[i];
		}
	
		globalWeightSum = comm->sum(localWeightSum);
	}
	
	const std::vector<IndexType> blockSizes(settings.numBlocks, globalWeightSum/settings.numBlocks);
	
	std::vector<std::vector<ValueType> > initialCenters = findLocalCenters<IndexType,ValueType>(coordinates, nodeWeights);
	//std::vector<std::vector<ValueType> > initialCenters = findLocalCenters(coordinates, nodeWeights);
	
	return computePartition(coordinates, k, nodeWeights, blockSizes, initialCenters, settings);
}



template std::vector<std::vector<ValueType> > findInitialCentersSFC( const std::vector<DenseVector<ValueType> >& coordinates, IndexType k, const std::vector<ValueType> &minCoords,    const std::vector<ValueType> &maxCoords, Settings settings);

//template std::vector<std::vector<ValueType> > findLocalCenters(const std::vector<DenseVector<ValueType> >& coordinates, const DenseVector<ValueType> &nodeWeights);
std::vector<std::vector<ValueType> > findLocalCenters(const std::vector<DenseVector<ValueType> >& coordinates, const DenseVector<ValueType> &nodeWeights);

template std::vector<std::vector<ValueType> > findInitialCentersFromSFCOnly( const IndexType k,  const std::vector<ValueType> &maxCoords, Settings settings);

template std::vector<std::vector<ValueType> > findInitialCenters(const std::vector<DenseVector<ValueType>> &coordinates, IndexType k, const DenseVector<ValueType> &nodeWeights);

template std::vector<std::vector<ValueType> > findCenters(const std::vector<DenseVector<ValueType>> &coordinates, const DenseVector<IndexType> &partition, const IndexType k, std::vector<IndexType>::iterator firstIndex, std::vector<IndexType>::iterator lastIndex, const DenseVector<ValueType> &nodeWeights);

template DenseVector<IndexType> assignBlocks(
		const std::vector<std::vector<ValueType>> &coordinates,
		const std::vector<std::vector<ValueType> > &centers,
        std::vector<IndexType>::iterator firstIndex, std::vector<IndexType>::iterator lastIndex,
        const DenseVector<ValueType> &nodeWeights, const DenseVector<IndexType> &previousAssignment, const std::vector<IndexType> &blockSizes, const SpatialCell &boundingBox,
        std::vector<ValueType> &upperBoundOwnCenter, std::vector<ValueType> &lowerBoundNextCenter, std::vector<ValueType> &influence, Settings settings);

template DenseVector<IndexType> assignBlocks(const std::vector<DenseVector<ValueType> >& coordinates, const std::vector<std::vector<double> >& centers);

template DenseVector<IndexType> getPartitionWithSFCCoords(const scai::lama::CSRSparseMatrix<ValueType>& adjM, const std::vector<DenseVector<ValueType> >& coordinates, const DenseVector<ValueType> &nodeWeights, const Settings settings);

template DenseVector<IndexType> computeRepartition(const std::vector<DenseVector<ValueType>> &coordinates, const DenseVector<ValueType> &nodeWeights, const Settings settings);

}

} /* namespace ITI */
