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
#include <scai/dmemo/mpi/MPIException.hpp>

#include "KMeans.h"
#include "HilbertCurve.h"
#include "MultiLevel.h"
#include "quadtree/QuadNodeCartesianEuclid.h"
// temporary, for debugging
#include "FileIO.h"

//#include "PrioQueue.h"

namespace ITI {

template<typename ValueType>
using point = typename std::vector<ValueType>;

// base implementation
template<typename IndexType, typename ValueType>
std::vector<std::vector<point<ValueType>>> KMeans<IndexType,ValueType>::findInitialCentersSFC(
     const std::vector<DenseVector<ValueType>>& coordinates,
     const std::vector<ValueType> &minCoords,
     const std::vector<ValueType> &maxCoords,
     const scai::lama::DenseVector<IndexType> &partition,
     const std::vector<cNode<IndexType,ValueType>> hierLevel,
Settings settings) {

    SCAI_REGION("KMeans.findInitialCentersSFC");
    const IndexType localN = coordinates[0].getLocalValues().size();
    const IndexType globalN = coordinates[0].size();
    const IndexType dimensions = settings.dimensions;
    // global communicator
    const scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistributionPtr()->getCommunicatorPtr();

    // the input is already partitioned into numOldBlocks number of blocks
    // for every old block we must find a number of new centers/blocks

    const std::vector<unsigned int> numNewBlocksPerOldBlock = CommTree<IndexType,ValueType>().getGrouping(hierLevel);
    const unsigned int numOldBlocks = numNewBlocksPerOldBlock.size();

    // convert coordinates, switch inner and outer order
    std::vector<std::vector<ValueType> > convertedCoords(localN, std::vector<ValueType> (dimensions,0.0));

    for (IndexType d = 0; d < dimensions; d++) {
        scai::hmemo::ReadAccess<ValueType> rAccess(coordinates[d].getLocalValues());
        assert(rAccess.size() == localN);
        for (IndexType i = 0; i < localN; i++) {
            convertedCoords[i][d] = rAccess[i];
        }
    }

    // TODO: In the hierarchical case, we need to compute new centers
    // many times, not just once. Take the hilbert indices once
    // outside the function and not every time that is called

    // the local points but sorted according to the SFC
    // needed to find the correct(based on the sfc ordering) center index
    std::vector<IndexType> sortedLocalIndices(localN);
    {
        // get local hilbert indices
        std::vector<double> sfcIndices = HilbertCurve<IndexType, ValueType>::getHilbertIndexVector(coordinates, settings.sfcResolution, settings.dimensions);
        SCAI_ASSERT_EQ_ERROR(sfcIndices.size(), localN, "wrong local number of indices (?) ");

        // prepare indices for sorting
        std::iota(sortedLocalIndices.begin(), sortedLocalIndices.end(), 0);

        // sort local indices according to SFC
        std::sort(sortedLocalIndices.begin(), sortedLocalIndices.end(), [&sfcIndices](IndexType a, IndexType b) {
            return sfcIndices[a] < sfcIndices[b];
        });
    }

    // get prefix sum for every known block

    const unsigned int numPEs = comm->getSize();
    const IndexType rootPE = 0; // set PE 0 as root

    // the global number of points of each old block
    std::vector<IndexType> globalBlockSizes(numOldBlocks);
    // global prefix sum vector of size (p+1)*numOldBlocks
    // ATTENTION: this a a concatenation of prefix sum arrays
    // the real prefix sums are [prefixSumArray[0]:prefixSumArray[numPEs]],
    // prefixSumArray[numPEs+1]:prefixSumArray[2*numPEs]], ... ,
    // example: [0,4,10,15, 0,7,15,22, 0,12,20,30, 0, ... ]
    //           block 1    block 2     block 3 ... block numOldBlocks
    // every "subarray" has size numPEs+1 (in this example numPEs=3)
    std::vector<IndexType> concatPrefixSumArray;

    {
        std::vector<IndexType> oldBlockSizes(numOldBlocks, 0);
        scai::hmemo::ReadAccess<IndexType> localPart = partition.getLocalValues();
        SCAI_ASSERT_EQ_ERROR(localPart.size(), localN, "Partition size mismatch");

        // count the size (the number of points) of every block locally
        for (unsigned int i=0; i<localN; i++) {
            IndexType thisPointBlock = localPart[i];
            oldBlockSizes[ thisPointBlock ]++;
        }

        // gather all block sizes to root
        IndexType arraySize=1;
        if (comm->getRank()==rootPE) {
            // a possible bottleneck: in the last step arraySize=k*p
            // TODO?: cut to smaller chunks and do it in rounds?
            arraySize = numPEs*numOldBlocks;
        }
        IndexType allOldBlockSizes[arraySize];
        comm->gather(allOldBlockSizes, numOldBlocks, rootPE, oldBlockSizes.data());
        std::vector<IndexType> allOldSizesVec(allOldBlockSizes, allOldBlockSizes + arraySize);
        if (comm->getRank()==rootPE) {
            SCAI_ASSERT_EQ_ERROR(globalN, std::accumulate(allOldSizesVec.begin(), allOldSizesVec.end(), 0), "Mismatch in gathered array for sizes of all blocks for PE " << *comm);
        }

        // only root PE calculates the prefixSum
        if (comm->getRank()==rootPE) {
            for (unsigned int blockId=0; blockId<numOldBlocks; blockId++) {
                // prefix sum for every block starts with 0
                concatPrefixSumArray.push_back(0);
                for (unsigned int pe=0; pe<numPEs; pe++) {
                    concatPrefixSumArray.push_back(concatPrefixSumArray.back() + allOldBlockSizes[pe*numOldBlocks+blockId]);
                }
            }
            SCAI_ASSERT_EQ_ERROR(concatPrefixSumArray.size(), (numPEs+1)*numOldBlocks, "Prefix sum array has wrong size");
        } else {
            concatPrefixSumArray.resize((numPEs+1)*numOldBlocks, 0);
        }

        comm->bcast(concatPrefixSumArray.data(),(numPEs+1)*numOldBlocks, rootPE);

        for (unsigned int b=0; b<numOldBlocks; b++) {
            globalBlockSizes[b] = concatPrefixSumArray[(b+1)*numPEs+b];
            SCAI_ASSERT_EQ_ERROR(concatPrefixSumArray[b*(numPEs+1)], 0, "Wrong concat prefix sum array, values at indices b*(numPEs+1) must be zero, Failed for b=" << b);
        }
        IndexType prefixSumCheckSum = std::accumulate(globalBlockSizes.begin(), globalBlockSizes.end(), 0);
        SCAI_ASSERT_EQ_ERROR(prefixSumCheckSum, globalN, "Global sizes mismatch. Wrong calculation of prefix sum?");
    }

    // compute wanted indices for initial centers
    // newCenterIndWithinBLock[i] = a vector with the indices of the
    // centers for block i
    // newCenterIndWithinBLock[i].size() = numNewBlocksPerOldBlock[b], i.e., the
    // new number of blocks to partition previous block i
    // ATTENTION: newCenterIndWithinBLock[i][j] = x: is the index of the
    // center within block i. If x is 30, then we want the 30-th point
    // of block i.

    std::vector<std::vector<IndexType>> newCenterIndWithinBLock(numOldBlocks);

    // for all old blocks
    for (IndexType b=0; b<numOldBlocks; b++) {
        // the number of centers for block b
        IndexType k_b = numNewBlocksPerOldBlock[b];
        newCenterIndWithinBLock[b].resize(k_b);
        for (IndexType i = 0; i < k_b; i++) {
            // wantedIndices[i] = i * (globalN / k) + (globalN / k)/2;
            newCenterIndWithinBLock[b][i] = i*(globalBlockSizes[b]/k_b) + (globalBlockSizes[b]/k_b)/2;
        }
    }

    const IndexType thisPE = comm->getRank();

    // the centers to be returned, each PE fills only with owned centers
    std::vector<std::vector<point<ValueType>>> centersPerNewBlock(numOldBlocks);
    for (IndexType b=0; b<numOldBlocks; b++) {
        centersPerNewBlock[b].resize(numNewBlocksPerOldBlock[b], point<ValueType>(dimensions, 0.0));
    }

    // for debugging
    IndexType sumOfRanges = 0;
    IndexType numOwnedCenters = 0;

    for (IndexType b=0; b<numOldBlocks; b++) {
        IndexType fromInd = b*(numPEs+1)+thisPE;
        assert(fromInd+1<concatPrefixSumArray.size());

        // the range of the indices for block b for this PE
        IndexType rangeStart = concatPrefixSumArray[ fromInd ];
        IndexType rangeEnd = concatPrefixSumArray[ fromInd+1];
        sumOfRanges += rangeEnd-rangeStart;
        // keep a counter that indicates the index of a point within
        // a block in this PE.
        IndexType counter = rangeStart;

        // center indices for block b, pass by reference so not to copy
        const std::vector<IndexType>& centersForThisBlock = newCenterIndWithinBLock[b];

        // TODO: optimize? Now, complexity is localN*number of owned centers
        // can we do it with one linear scan? Well, newCenterIndWithinBLock[b] is expected
        // to be kinf of small unless k>>p

        // if some center indexes are local in this PE, store them.
        // Later, we will scan the local points for their coordinates
        for (unsigned int j=0; j<centersForThisBlock.size(); j++) {
            IndexType centerInd = centersForThisBlock[j];
            counter = rangeStart;// reset counter for next center
            // if center index for block b is owned by thisPE
            if (centerInd>=rangeStart and centerInd<=rangeEnd) {

                // since we own a center, go over all local points
                // and calculate their within-block index for the block
                // they belong to
                scai::hmemo::ReadAccess<IndexType> localPart = partition.getLocalValues();
                for (unsigned int i=0; i<localN; i++) {
                    // consider points based on their sorted sfc index
                    IndexType sortedIndex = sortedLocalIndices[i];
                    IndexType thisPointBlock = localPart[ sortedIndex ];
                    // TODO: remove assertion?
                    assert(thisPointBlock<numOldBlocks);
                    if (thisPointBlock!=b) {
                        continue;// not in desired block
                    }

                    IndexType withinBlockIndex = counter;
                    // desired center found
                    if (withinBlockIndex==centerInd) {
                        // store center coords
                        centersPerNewBlock[b][j] = convertedCoords[ sortedIndex ];
                        numOwnedCenters++;
                        // PRINT(*comm <<": adding center "<< centerInd << " with coordinates " << convertedCoords[sortedIndex][0] << ", " << convertedCoords[sortedIndex][1]);
                        break;
                    }
                    counter++;
                }// for i<localN
                SCAI_ASSERT_LE_ERROR(counter, rangeEnd, "Within-block index out of bounds");
            }// if center is local
        }// for j<centersForThisBlock.size()
    }// for b<numOldBlocks

    SCAI_ASSERT_EQ_ERROR(sumOfRanges, localN, thisPE << ": Sum of owned number of points per block should be equal the total number of local points");

    if (settings.debugMode) {
        PRINT(*comm << ": owns " << numOwnedCenters << " centers");
        unsigned int numNewTotalBlocks = std::accumulate(numNewBlocksPerOldBlock.begin(), numNewBlocksPerOldBlock.end(), 0);
        SCAI_ASSERT_EQ_ERROR(comm->sum(numOwnedCenters), numNewTotalBlocks, "Not all centers were found");
    }

    //
    // global sum operation. Doing it in a separate loop on purpose
    // since different PEs own centers from different blocks and a lot of
    // blocks own no centers at all
    //

    for (IndexType b=0; b<numOldBlocks; b++) {

        SCAI_ASSERT_EQ_ERROR(centersPerNewBlock[b][0].size(), dimensions, "Dimension mismatch for center");
        IndexType numCenters = centersPerNewBlock[b].size();

        // pack in a raw array
        std::vector<ValueType> allCenters(numCenters*dimensions);

        for (unsigned int c=0; c<numCenters; c++) {
            const point<ValueType>& thisCenter = centersPerNewBlock[b][c];
            // copy this center
            std::copy(thisCenter.begin(), thisCenter.end(), allCenters.begin() +c*dimensions);
        }

        // global sum
        comm->sumImpl(allCenters.data(), allCenters.data(), numCenters*dimensions, scai::common::TypeTraits<ValueType>::stype);

        // unpack back to vector<point>
        for (unsigned int c=0; c<numCenters; c++) {
            for (IndexType d=0; d<dimensions; d++) {
                // center c, for block b
                centersPerNewBlock[b][c][d] = allCenters[ c*dimensions+d ];
            }
        }
    }

    return centersPerNewBlock;
}


// overloaded function for non-hierarchical version.
// Set partition to 0 for all points
// A "flat" communication tree
// and return only the first (there is only one) group of centers
template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType>>  KMeans<IndexType,ValueType>::findInitialCentersSFC(
     const std::vector<DenseVector<ValueType>>& coordinates,
     const std::vector<ValueType> &minCoords,
     const std::vector<ValueType> &maxCoords,
Settings settings) {

    // TODO: probably must also change the settings.numBlocks

    // homogeneous case, all PEs have the same memory and speed
    // there is only hierarchy level
    // TODO: probably this needs some further adaptation for the hierarchical version

    // TODO: use commTree::createFlat?
    std::vector<cNode<IndexType,ValueType>> leaves(settings.numBlocks);
    for (int i=0; i<settings.numBlocks; i++) {
        leaves[i] = cNode<IndexType,ValueType>(std::vector<unsigned int> {0}, {1.0});
    }

    // every point belongs to one block in the beginning
    scai::lama::DenseVector<IndexType> partition(coordinates[0].getDistributionPtr(), 0);

    // return a vector of size 1 with
    std::vector<std::vector<point<ValueType>>> initialCenters = findInitialCentersSFC(coordinates, minCoords, maxCoords, partition, leaves, settings);

    SCAI_ASSERT_EQ_ERROR(initialCenters.size(), 1, "Wrong vector size");
    SCAI_ASSERT_EQ_ERROR(initialCenters[0].size(), settings.numBlocks, "Wrong vector size");

    // TODO: must change convert centers to a vector of size=dimensions
    // where initialCenters[0][d][i] is the d-th coordinate of the i-th center
    IndexType dimensions = settings.dimensions;

    SCAI_ASSERT_EQ_ERROR(minCoords.size(), settings.dimensions, "Wrong center dimensions");

// TODO: check/verify if we do not need to revert the vector order
    // reverse vector order here
    std::vector<std::vector<ValueType>> reversedCenters(dimensions, std::vector<ValueType>(settings.numBlocks, 0.0));
    for (unsigned int c=0; c<settings.numBlocks; c++) {
        for (unsigned int d=0; d<dimensions; d++) {
            reversedCenters[d][c] = initialCenters[0][c][d];
        }
    }
    // return reversedCenters;

    return initialCenters[0];
}


template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType>> KMeans<IndexType,ValueType>::findInitialCentersFromSFCOnly(
    const std::vector<ValueType> &minCoords,
    const std::vector<ValueType> &maxCoords,
    Settings settings) {
    // This assumes that minCoords is 0!
    // TODO: change or remove
    const IndexType dimensions = settings.dimensions;
    const IndexType k = settings.numBlocks;

    // set local values in vector, leave non-local values with zero
    std::vector<std::vector<ValueType> > result(dimensions);
    for (IndexType d = 0; d < dimensions; d++) {
        result[d].resize(k);
    }

    ValueType offset = 1.0/(ValueType(k)*2.0);
    std::vector<ValueType> centerCoords(dimensions,0);
    for (IndexType i = 0; i < k; i++) {
        ValueType centerHilbInd = i/ValueType(k) + offset;
        centerCoords = HilbertCurve<IndexType,ValueType>::HilbertIndex2Point(centerHilbInd, settings.sfcResolution, settings.dimensions);
        SCAI_ASSERT_EQ_ERROR(centerCoords.size(), dimensions, "Wrong dimensions for center.");

        // centerCoords are points in the unit square; project back to input space
        for (IndexType d = 0; d < dimensions; d++) {
            result[d][i] = (centerCoords[d]*(maxCoords[d]-minCoords[d]))+minCoords[d];
        }
    }
    return result;
}

//TODO: how to treat multiple weights
template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType>> KMeans<IndexType,ValueType>::findLocalCenters(const std::vector<DenseVector<ValueType> >& coordinates, const DenseVector<ValueType> &nodeWeights) {

    const IndexType dim = coordinates.size();
    const IndexType localN = coordinates[0].getLocalValues().size();

    // get sum of local weights

    scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights.getLocalValues());
    SCAI_ASSERT_EQ_ERROR(rWeights.size(), localN, "Mismatch of nodeWeights and coordinates size. Check distributions.");

    ValueType localWeightSum = 0;
    for (IndexType i=0; i<localN; i++) {
        localWeightSum += rWeights[i];
    }

    std::vector<ValueType> localCenter(dim, 0);

    for (IndexType d = 0; d < dim; d++) {
        scai::hmemo::ReadAccess<ValueType> rCoords(coordinates[d].getLocalValues());
        for (IndexType i=0; i<localN; i++) {
            // this is more expensive than summing first and dividing later, but avoids overflows
            localCenter[d] += rWeights[i]*rCoords[i]/localWeightSum;
        }
    }

    // vector of size k for every center. Each PE only stores its local center and sum center
    // so all centers are replicated in all PEs

    const scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistribution().getCommunicatorPtr();
    const IndexType numPEs = comm->getSize();
    const IndexType thisPE = comm->getRank();
    std::vector<std::vector<ValueType> > result(dim, std::vector<ValueType>(numPEs,0));
    for (IndexType d=0; d<dim; d++) {
        result[d][thisPE] = localCenter[d];
    }

    for (IndexType d=0; d<dim; d++) {
        comm->sumImpl(result[d].data(), result[d].data(), numPEs, scai::common::TypeTraits<ValueType>::stype);
    }
    return result;
}


//TODO: how to treat multiple weights
template<typename IndexType, typename ValueType>
template<typename Iterator>
std::vector<std::vector<ValueType>> KMeans<IndexType,ValueType>::findCenters(
    const std::vector<DenseVector<ValueType>>& coordinates,
    const DenseVector<IndexType>& partition,
    const IndexType k,
    const Iterator firstIndex,
    const Iterator lastIndex,
    const std::vector<DenseVector<ValueType>>& nodeWeights) {
    SCAI_REGION("KMeans.findCenters");

    const IndexType dim = coordinates.size();
    const scai::dmemo::DistributionPtr resultDist(new scai::dmemo::NoDistribution(k));
    const scai::dmemo::CommunicatorPtr comm = partition.getDistribution().getCommunicatorPtr();

    // TODO: check that distributions align

    const IndexType numWeights= nodeWeights.size();

    //calculate a center for each block, for each weight, size: numWeights*dim*k
    std::vector<std::vector<std::vector<ValueType>>> allWeightsCenters( numWeights );

    for(unsigned int w=0; w<numWeights; w++){
        std::vector<std::vector<ValueType>> result(dim, std::vector<ValueType>(k,0) );
        std::vector<ValueType> weightSum(k, 0.0);

        scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights[w].getLocalValues());
        scai::hmemo::ReadAccess<IndexType> rPartition(partition.getLocalValues());

        // compute weight sums
        for (Iterator it = firstIndex; it != lastIndex; it++) {
            const IndexType i = *it;
            const IndexType part = rPartition[i];
            const ValueType weight = rWeights[i];
            weightSum[part] += weight;
            // the lines above are equivalent to: weightSum[rPartition[*it]] += rWeights[*it];
        }

        // find local centers
        for (IndexType d = 0; d < dim; d++) {
            scai::hmemo::ReadAccess<ValueType> rCoords(coordinates[d].getLocalValues());

            for (Iterator it = firstIndex; it != lastIndex; it++) {
                const IndexType i = *it;
                const IndexType part = rPartition[i];
                // this is more expensive than summing first and dividing later, but avoids overflows
                result[d][part] += rCoords[i]*rWeights[i] / weightSum[part];
            }
        }

        // communicate local centers and weight sums
        std::vector<ValueType> totalWeight(k, 0);
        comm->sumImpl(totalWeight.data(), weightSum.data(), k, scai::common::TypeTraits<ValueType>::stype);

        // compute updated centers as weighted average
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

        allWeightsCenters[w]= result ;
    }

    //
    //average the centers for each weight to create the final centers for each block
    //

    std::vector<std::vector<ValueType>> result(dim, std::vector<ValueType>(k,0) );

    for(IndexType d = 0; d < dim; d++) {
        for (IndexType j = 0; j < k; j++) {
            for(unsigned int w=0; w<numWeights; w++){
                //remember, allWeightsCenters size: numWeights*dim*k
                result[d][j] += allWeightsCenters[w][d][j]/numWeights;
            }
        }
    }

    return result;
}


template<typename IndexType, typename ValueType>
std::vector<point<ValueType>> KMeans<IndexType,ValueType>::vectorTranspose(const std::vector<std::vector<ValueType>>& points) {
    const IndexType dim = points.size();
    SCAI_ASSERT_GT_ERROR(dim, 0, "Dimension of points cannot be 0");

    const IndexType numPoints = points[0].size();
    SCAI_ASSERT_GT_ERROR(numPoints, 0, "Empty vector of points");

    std::vector<point<ValueType>> retPoints(numPoints, point<ValueType>(dim));

    for (unsigned int d=0; d<dim; d++) {
        for (unsigned int i=0; i<numPoints; i++) {
            retPoints[i][d] = points[d][i];
        }
    }

    return retPoints;
}


template<typename IndexType, typename ValueType>
template<typename Iterator>
DenseVector<IndexType> KMeans<IndexType,ValueType>::assignBlocks(
    const std::vector<std::vector<ValueType>>& coordinates,
    const std::vector<point<ValueType>>& centers,
    const std::vector<IndexType>& blockSizesPrefixSum,
    const Iterator firstIndex,
    const Iterator lastIndex,
    const std::vector<std::vector<ValueType>> &nodeWeights,
    const std::vector<std::vector<ValueType>> &normalizedNodeWeights,
    const DenseVector<IndexType> &previousAssignment,
    const DenseVector<IndexType> &oldBlock, // if repartition, this is the partition to be rebalanced
    const std::vector<std::vector<ValueType>> &targetBlockWeights,
    const SpatialCell<ValueType> &boundingBox,
    std::vector<ValueType> &upperBoundOwnCenter,
    std::vector<ValueType> &lowerBoundNextCenter,
    std::vector<std::vector<ValueType>> &influence,
    std::vector<ValueType> &imbalance,
    Settings settings,
    Metrics<ValueType>& metrics) {
    SCAI_REGION("KMeans.assignBlocks");

    const IndexType dim = coordinates.size();
    const scai::dmemo::DistributionPtr dist = previousAssignment.getDistributionPtr();
    const scai::dmemo::CommunicatorPtr comm = dist->getCommunicatorPtr();
    const IndexType localN = dist->getLocalSize();
    const IndexType currentLocalN = std::distance(firstIndex, lastIndex); //number of sampled points

    if (currentLocalN < 0) {
        throw std::runtime_error("currentLocalN: " + std::to_string(currentLocalN));
    }

    if (currentLocalN == 0) {
        PRINT("Process " + std::to_string(comm->getRank()) + " has no local points!");
        return previousAssignment;
    }

    // if repartition, numOldBlocks=1
    // number of blocks from the previous hierarchy
    const IndexType numOldBlocks= blockSizesPrefixSum.size()-1;

    if (settings.repartition) {
        assert(numOldBlocks==1);
        assert(blockSizesPrefixSum.size()==2);
    }
    const IndexType numNodeWeights = nodeWeights.size();

    if (settings.debugMode and not settings.repartition) {
        const IndexType maxPart = oldBlock.max(); // global operation
        SCAI_ASSERT_EQ_ERROR(numOldBlocks-1, maxPart, "The provided old assignment must have equal number of blocks as the length of the vector with the new number of blocks per part");
    }

    // numNewBlocks is equivalent to 'k' in the classic version
    const IndexType numNewBlocks = centers.size();

    SCAI_ASSERT_EQ_ERROR(blockSizesPrefixSum.back(), numNewBlocks, "Total number of new blocks mismatch");

    // centers are given as a 1D vector alongside with a prefix sum vector
    const std::vector<point<ValueType>>& centers1DVector = centers;

    SCAI_ASSERT_EQ_ERROR(centers1DVector.size(), numNewBlocks, "Vector size mismatch");
    SCAI_ASSERT_EQ_ERROR(centers1DVector[0].size(), dim, "Center dimensions mismatch");
    SCAI_ASSERT_EQ_ERROR(influence.size(), numNodeWeights, "Vector size mismatch");
    for (IndexType i = 0; i < numNodeWeights; i++) {
        SCAI_ASSERT_EQ_ERROR(influence[i].size(), numNewBlocks, "Vector size mismatch");
    }

    // pre-filter possible closest blocks
    std::vector<ValueType> minDistanceAllBlocks(numNewBlocks);
    std::vector<ValueType> effectMinDistAllBlocks(numNewBlocks);

    // for all new blocks
    for (IndexType newB=0; newB<numNewBlocks; newB++) {
        SCAI_REGION("KMeans.assignBlocks.filterCenters");

        point<ValueType> center = centers1DVector[newB];
        ValueType influenceMin = std::numeric_limits<ValueType>::max();
        for (IndexType i = 0; i < numNodeWeights; i++) {
            influenceMin = std::min(influenceMin, influence[i][newB]);
        }

        minDistanceAllBlocks[newB] = boundingBox.distances(center).first;
        assert(std::isfinite(minDistanceAllBlocks[newB]));
        effectMinDistAllBlocks[newB] = minDistanceAllBlocks[newB]\
                                       *minDistanceAllBlocks[newB]\
                                       *influenceMin;
        assert(std::isfinite(effectMinDistAllBlocks[newB]));
    }

    // sort centers according to their distance from the bounding box of this PE

    std::vector<IndexType> clusterIndicesAllBlocks(numNewBlocks);
    // cluster indices are "global": from 0 to numNewBlocks
    std::iota(clusterIndicesAllBlocks.begin(), clusterIndicesAllBlocks.end(), 0);

    // remember, if repartition, numOldBlocks=1 and blockSizesPrefixSum.size()=2

    for (IndexType oldB=0; oldB<numOldBlocks; oldB++) {
        const unsigned int rangeStart = blockSizesPrefixSum[oldB];
        const unsigned int rangeEnd = blockSizesPrefixSum[oldB+1];
        typename std::vector<IndexType>::iterator startIt = clusterIndicesAllBlocks.begin()+rangeStart;
        typename std::vector<IndexType>::iterator endIt = clusterIndicesAllBlocks.begin()+rangeEnd;
        // TODO: remove not needed assertions
        SCAI_ASSERT_LT_ERROR(rangeStart, rangeEnd, "Prefix sum vectors is wrong");
        SCAI_ASSERT_LE_ERROR(rangeEnd, numNewBlocks, "Range out of bounds");

        // sort the part of the indices that belong to this old block
        std::sort(startIt, endIt,
        [&](IndexType a, IndexType b) {
            return effectMinDistAllBlocks[a] < effectMinDistAllBlocks[b] \
                   || (effectMinDistAllBlocks[a] == effectMinDistAllBlocks[b] && a < b);
        }
                 );

        std::sort(effectMinDistAllBlocks.begin()+rangeStart, effectMinDistAllBlocks.begin()+rangeEnd);
    }

    IndexType iter = 0;
    IndexType skippedLoops = 0;
    ValueType totalBalanceTime = 0; // for timing/profiling
    std::vector<std::vector<bool>> influenceGrew(numNodeWeights, std::vector<bool>(numNewBlocks));
    std::vector<ValueType> influenceChangeUpperBound(numNewBlocks, 1+settings.influenceChangeCap);
    std::vector<ValueType> influenceChangeLowerBound(numNewBlocks, 1-settings.influenceChangeCap);

    // compute assignment and balance
    DenseVector<IndexType> assignment = previousAssignment;
    bool allWeightsBalanced = false; // balance over all weights and all blocks


    // iterate if necessary to achieve balance
    do
    {
        std::chrono::time_point<std::chrono::high_resolution_clock> balanceStart = std::chrono::high_resolution_clock::now();
        SCAI_REGION("KMeans.assignBlocks.balanceLoop");

        //TODO: probably, only a few blocks are local; maybe change vector to a map?
        // the block weight for all new blocks
        std::vector<std::vector<ValueType>> blockWeights(numNodeWeights, std::vector<ValueType>(numNewBlocks, 0.0));

        std::vector<ValueType> influenceEffectOfOwn(currentLocalN, 0); // TODO: also potentially move to outer function

        IndexType totalComps = 0;
        skippedLoops = 0;
        IndexType balancedBlocks = 0;

        scai::hmemo::ReadAccess<IndexType> rOldBlock(oldBlock.getLocalValues());
        {
            SCAI_REGION("KMeans.assignBlocks.balanceLoop.assign");
            scai::hmemo::WriteAccess<IndexType> wAssignment(assignment.getLocalValues());
            // for the sampled range
            for (Iterator it = firstIndex; it != lastIndex; it++) {
                const IndexType i = *it;
                //oldCluster: where it belonged in the previous iteration
                const IndexType oldCluster = wAssignment[i];
                //fatherBlock: meaningful in the hierarchical version, it is the block of this point in the previous hierarchy 
                const IndexType fatherBlock = rOldBlock[i]; 
                const IndexType veryLocalI = std::distance(firstIndex, it);

                if (not settings.repartition) {
                    SCAI_ASSERT_LT_ERROR(fatherBlock, numOldBlocks, "Wrong father block index");
                } else {
                    // numOldBlocks=1 but father block<numNewBlocks
                    SCAI_ASSERT_LT_ERROR(fatherBlock, numNewBlocks, "Wrong father block index");
                }

                assert(influenceEffectOfOwn[veryLocalI] == 0);
                for (IndexType j = 0; j < numNodeWeights; j++) {
                    influenceEffectOfOwn[veryLocalI] += influence[j][oldCluster]*normalizedNodeWeights[j][i];
                }

                if (lowerBoundNextCenter[i] > upperBoundOwnCenter[i]) {
                    // cluster assignment cannot have changed.
                    // wAssignment[i] = wAssignment[i];
                    skippedLoops++;
                } else {
                    ValueType sqDistToOwn = 0;
                    const point<ValueType>& myCenter = centers1DVector[oldCluster];
                    for (IndexType d = 0; d < dim; d++) {
                        sqDistToOwn += std::pow(myCenter[d]-coordinates[d][i], 2);
                    }

                    ValueType newEffectiveDistance = sqDistToOwn*influenceEffectOfOwn[veryLocalI];
                    SCAI_ASSERT_LE_ERROR(newEffectiveDistance, upperBoundOwnCenter[i], "Distance upper bound was wrong");
                    upperBoundOwnCenter[i] = newEffectiveDistance;
                    if (lowerBoundNextCenter[i] > upperBoundOwnCenter[i]) {
                        // cluster assignment cannot have changed.
                        // wAssignment[i] = wAssignment[i];
                        skippedLoops++;
                    } else {
                        // check the centers of this old block to find the closest one
                        IndexType bestBlock = 0;
                        ValueType bestValue = std::numeric_limits<ValueType>::max();
                        ValueType influenceEffectOfBestBlock = -1;
                        IndexType secondBest = 0;
                        ValueType secondBestValue = std::numeric_limits<ValueType>::max();

                        // if repartition, blockSizesPrefixSum only has two elements and the fatherBlock index is wrong
                        // where the range of indices starts for the father block
                        const IndexType rangeStart = settings.repartition ? 0 : blockSizesPrefixSum[fatherBlock];
                        const IndexType rangeEnd =  settings.repartition ? blockSizesPrefixSum.back() : blockSizesPrefixSum[fatherBlock+1];
                        SCAI_ASSERT_LE_ERROR(rangeEnd, clusterIndicesAllBlocks.size(), "Range out of bounds");

                        // start with the first center index
                        IndexType c = rangeStart;

                        // check all centers belonging to the father block to find the closest
                        while (c < rangeEnd && secondBestValue > effectMinDistAllBlocks[c]) {
                            totalComps++;
                            // remember: cluster centers are sorted according to their distance from the bounding box of this PE
                            // also, the cluster indices go from 0 till numNewBlocks
                            IndexType j = clusterIndicesAllBlocks[c];// maybe it would be useful to sort the whole centers array, aligning memory accesses.

                            // squared distance from previous assigned center
                            ValueType sqDist = 0;
                            const point<ValueType>& myCenter = centers1DVector[j];
                            // TODO: restructure arrays to align memory accesses better in inner loop
                            for (IndexType d = 0; d < dim; d++) {
                                sqDist += std::pow(myCenter[d]-coordinates[d][i], 2);
                            }

                            ValueType influenceEffect = 0;
                            for (IndexType w = 0; w < numNodeWeights; w++) {
                                influenceEffect += influence[w][j]*normalizedNodeWeights[w][i];
                            }

                            const ValueType effectiveDistance = sqDist*influenceEffect;

                            // update best and second-best centers
                            if (effectiveDistance < bestValue) {
                                secondBest = bestBlock;
                                secondBestValue = bestValue;
                                bestBlock = j;
                                bestValue = effectiveDistance;
                                influenceEffectOfBestBlock = influenceEffect;
                            } else if (effectiveDistance < secondBestValue) {
                                secondBest = j;
                                secondBestValue = effectiveDistance;
                            }
                            c++;
                        } // while

                        if (rangeEnd - rangeStart > 1) {
                            SCAI_ASSERT_NE_ERROR(bestBlock, secondBest, "Best and second best should be different");
                        }

                        assert(secondBestValue >= bestValue);

                        // this point has a new center
                        if (bestBlock != oldCluster) {
                            // assert(bestValue >= lowerBoundNextCenter[i]);
                            SCAI_ASSERT_GE_ERROR(bestValue, lowerBoundNextCenter[i], \
                                                 "PE " << comm->getRank() << ": difference " << std::abs(bestValue - lowerBoundNextCenter[i]) << \
                                                 " for i= " << i << ", oldCluster: " << oldCluster << ", newCluster: " << bestBlock << \
                                                 ", influenceEffect: " << influenceEffectOfBestBlock);
                        }

                        upperBoundOwnCenter[i] = bestValue;
                        lowerBoundNextCenter[i] = secondBestValue;
                        influenceEffectOfOwn[veryLocalI] = influenceEffectOfBestBlock;
                        wAssignment[i] = bestBlock;
                    }
                }
                // we found the best block for this point; increase the weight of this block
                for (IndexType j = 0; j <numNodeWeights; j++) {
                    blockWeights[j][wAssignment[i]] += nodeWeights[j][i];
                }

            }// for sampled indices

            std::chrono::duration<ValueType,std::ratio<1>> balanceTime = std::chrono::high_resolution_clock::now() - balanceStart;
            // timePerPE[comm->getRank()] += balanceTime.count();

            comm->synchronize();
        }// assignment block

        //get the total weight of the blocks
        for (IndexType j = 0; j < numNodeWeights; j++){
            SCAI_REGION("KMeans.assignBlocks.balanceLoop.blockWeightSum");
            comm->sumImpl(blockWeights[j].data(), blockWeights[j].data(), numNewBlocks, scai::common::TypeTraits<ValueType>::stype);
        }

        // calculate imbalance for every new block and every weight
        allWeightsBalanced = true;
        std::vector<std::vector<ValueType>> imbalancesPerBlock(numNodeWeights, std::vector<ValueType>(numNewBlocks));
        for (IndexType i = 0; i < numNodeWeights; i++) {
            for (IndexType newB=0; newB<numNewBlocks; newB++) {
                ValueType optWeight = targetBlockWeights[i][newB];
                imbalancesPerBlock[i][newB] = (ValueType(blockWeights[i][newB] - optWeight)/optWeight);
            }
            // imbalance for each weight is the maximum imbalance of all new blocks
            imbalance[i] = *std::max_element(imbalancesPerBlock[i].begin(), imbalancesPerBlock[i].end());

            if (settings.verbose and imbalance[i]<0) {
                PRINT0("Warning, imbalance in weight " + std::to_string(i) + " is " + std::to_string(imbalance[i]) + ". Probably the given target block sizes are all too large.");
            }

            //if different epsilons where given for each weight
            if( settings.epsilons.size()>0){
                assert( settings.epsilons.size()==numNodeWeights );
                if (imbalance[i] > settings.epsilons[i]) {
                    allWeightsBalanced = false;
                }
            }
            else{
                if (imbalance[i] > settings.epsilon) {
                    allWeightsBalanced = false;
                }
            }
        }

        //
        // adapt influence values based on the weight of each block
        //

        ValueType minRatio = std::numeric_limits<ValueType>::max();
        ValueType maxRatio = -std::numeric_limits<ValueType>::min();
        std::vector<std::vector<ValueType>> oldInfluence = influence;// size=numNewBlocks
        SCAI_ASSERT_EQ_ERROR( blockWeights.size(), numNodeWeights, "block sizes, wrong number of weights" );
        SCAI_ASSERT_EQ_ERROR( targetBlockWeights.size(), numNodeWeights, "target block sizes, wrong number of weights" );

        for (IndexType i = 0; i < numNodeWeights; i++) {
            assert(oldInfluence[i].size()== numNewBlocks);
            for (IndexType j=0; j<numNewBlocks; j++) {
                SCAI_REGION("KMeans.assignBlocks.balanceLoop.influence");
                ValueType ratio = ValueType(blockWeights[i][j])/targetBlockWeights[i][j];
                if (std::abs(ratio - 1) < settings.epsilon) {
                    //this block is balanced
                    balancedBlocks++; // TODO: update for multiple weights
                    if (settings.freezeBalancedInfluence) {
                        if (1 < minRatio) minRatio = 1;
                        if (1 > maxRatio) maxRatio = 1;
                        continue;
                    }
                }
                
                //use ratio^exponent only if it is within the bounds; otherwise use the bound
                const ValueType multiplier = std::max( influenceChangeLowerBound[j],
                    std::min( (ValueType)std::pow(ratio, settings.influenceExponent), influenceChangeUpperBound[j]) );
                influence[i][j] =  influence[i][j]*multiplier;

                assert(influence[i][j] > 0);

                ValueType influenceRatio = influence[i][j] / oldInfluence[i][j];

                assert(influenceRatio <= influenceChangeUpperBound[j] + 1e-6);
                assert(influenceRatio >= influenceChangeLowerBound[j] - 1e-6);
                if (influenceRatio < minRatio) minRatio = influenceRatio;
                if (influenceRatio > maxRatio) maxRatio = influenceRatio;

                if (settings.tightenBounds && iter > 0 && (static_cast<bool>(ratio > 1) != influenceGrew[i][j])) {
                    // influence change switched direction
                    influenceChangeUpperBound[j] = 0.1 + 0.9*influenceChangeUpperBound[j];
                    influenceChangeLowerBound[j] = 0.1 + 0.9*influenceChangeLowerBound[j];
                    assert(influenceChangeUpperBound[j] > 1);
                    assert(influenceChangeLowerBound[j] < 1);
                }
                influenceGrew[i][j] = static_cast<bool>(ratio > 1);
            }// for numNewBlocks
        }// for numNodeWeights

        // update bounds
        {
            SCAI_REGION("KMeans.assignBlocks.balanceLoop.updateBounds");
            scai::hmemo::ReadAccess<IndexType> rAssignement(assignment.getLocalValues());
            for (Iterator it = firstIndex; it != lastIndex; it++) {
                const IndexType i = *it;
                const IndexType cluster = rAssignement[i];
                const IndexType veryLocalI = std::distance(firstIndex, it);// maybe optimize?
                ValueType newInfluenceEffect = 0;
                for (IndexType j = 0; j < numNodeWeights; j++) {
                    newInfluenceEffect += influence[j][cluster]*normalizedNodeWeights[j][i];
                }

                SCAI_ASSERT_LE_ERROR((newInfluenceEffect / influenceEffectOfOwn[veryLocalI]), maxRatio + 1e-5, "Error in calculation of influence effect");
                SCAI_ASSERT_GE_ERROR((newInfluenceEffect / influenceEffectOfOwn[veryLocalI]), minRatio - 1e-5, "Error in calculation of influence effect");

                upperBoundOwnCenter[i] *= (newInfluenceEffect / influenceEffectOfOwn[veryLocalI]) + 1e-5;
                lowerBoundNextCenter[i] *= minRatio - 1e-5;
            }
        }

        // update possible closest centers
        {
            // TODO: repair once I have the equations figured out for the new boundaries
            SCAI_REGION("KMeans.assignBlocks.balanceLoop.filterCenters");
            for (IndexType newB=0; newB<numNewBlocks; newB++) {
                ValueType influenceMin = std::numeric_limits<ValueType>::max();
                for (IndexType i = 0; i < numNodeWeights; i++) {
                    influenceMin = std::min(influenceMin, influence[i][newB]);
                }

                effectMinDistAllBlocks[newB] = minDistanceAllBlocks[newB]*minDistanceAllBlocks[newB]*influenceMin;
            }

            // TODO: duplicated code as in the beginning of the assignBlocks, maybe move into lambda?
            for (IndexType oldB=0; oldB<numOldBlocks; oldB++) {
                const unsigned int rangeStart = blockSizesPrefixSum[oldB];
                const unsigned int rangeEnd = blockSizesPrefixSum[oldB+1];
                typename std::vector<IndexType>::iterator startIt = clusterIndicesAllBlocks.begin()+rangeStart;
                typename std::vector<IndexType>::iterator endIt = clusterIndicesAllBlocks.begin()+rangeEnd;
                // TODO: remove not needed assertions
                SCAI_ASSERT_LT_ERROR(rangeStart, rangeEnd, "Prefix sum vector is wrong");
                SCAI_ASSERT_LE_ERROR(rangeEnd, numNewBlocks, "Range out of bounds");
                // SCAI_ASSERT_ERROR(endIt!=clusterIndicesAllBlocks.end(), "Iterator out of bounds");

                // sort the part of the indices that belong to this old block
                std::sort(startIt, endIt,
                [&](IndexType a, IndexType b) {
                    return effectMinDistAllBlocks[a] < effectMinDistAllBlocks[b] \
                           || (effectMinDistAllBlocks[a] == effectMinDistAllBlocks[b] && a < b);
                }
                         );
                // sort also this part of the distances
                std::sort(effectMinDistAllBlocks.begin()+rangeStart, effectMinDistAllBlocks.begin()+rangeEnd);
            }
        }

        iter++;

        if (settings.verbose ) {

            std::vector<ValueType> influenceSpread(numNodeWeights);
            for (IndexType i = 0; i < numNodeWeights; i++) {
                const auto pair = std::minmax_element(influence[i].begin(), influence[i].end());
                influenceSpread[i] = *pair.second / *pair.first;
                if (comm->getRank() == 0 and settings.debugMode) { 
                    std:: cout<< "max influence= " << *pair.second << ", min influence= " << *pair.first << std::endl;
                    std::cout << "all influences and block sizes:"<< std::endl;
                    assert( influence.size()==blockWeights.size() );
                    ValueType optWeight = targetBlockWeights[i][0];
                    std::cout<< "opt weight " << optWeight << std::endl;
                    for( unsigned int ii=0; ii<influence[i].size(); ii++ ){
                        std::cout << ii << ": " << influence[i][ii] << ", " << blockWeights[i][ii] << std::endl;
                    }
                }
            }

            std::vector<ValueType> weightSpread(numNodeWeights);
            for (IndexType i = 0; i < numNodeWeights; i++) {
                const auto pair = std::minmax_element(blockWeights[i].begin(), blockWeights[i].end());
                weightSpread[i] = *pair.second / *pair.first;
            }

            std::chrono::duration<ValueType,std::ratio<1>> balanceTime = std::chrono::high_resolution_clock::now() - balanceStart;
            totalBalanceTime += balanceTime.count();
            const IndexType takenLoops = currentLocalN - skippedLoops;
            const ValueType averageComps = ValueType(totalComps) / currentLocalN;

            auto oldprecision = std::cout.precision(3);
            if (comm->getRank() == 0) {
                std::cout << "Iter " << iter << ", loop: " << 100*ValueType(takenLoops) / currentLocalN << "%, average comparisons: "
                          << averageComps << ", balanced blocks: " << 100*ValueType(balancedBlocks) / numNewBlocks << "%, influence spread: ";
                for (IndexType i = 0; i < numNodeWeights; i++) {
                    std::cout << influenceSpread[i] << " ";
                }
                std::cout << ", weight spread : ";
                for (IndexType i = 0; i < numNodeWeights; i++) {
                    std::cout << weightSpread[i] << " ";
                }
                std::cout << ", imbalance : ";
                for (IndexType i = 0; i < numNodeWeights; i++) {
                    std::cout << imbalance[i] << " ";
                }
                std::cout << ", time elapsed: " << totalBalanceTime << std::endl;
            }
            std::cout.precision(oldprecision);
        }

    } while ((!allWeightsBalanced) && iter < settings.balanceIterations);

    if (settings.verbose) {
        ValueType percentageSkipped = ValueType(skippedLoops*100) / (iter*localN);
        ValueType maxSkipped = comm->max(percentageSkipped);
        ValueType minSkipped = comm->min(percentageSkipped);
        ValueType avgSkipped = comm->sum(percentageSkipped) / comm->getSize();
        if (comm->getRank() == 0) {
            std::cout << "Skipped inner loops in %: " << "min: " << minSkipped << ", avg: " << avgSkipped << " " << ", max: " << maxSkipped << std::endl;
        }
    }

    // for kmeans profiling
    metrics.numBalanceIter.push_back(iter);

    return assignment;
}// assignBlocks


// WARNING: we do not use k as repartition assumes k=comm->getSize() and neither blockSizes and we assume
// that every block has the same size

template<typename IndexType, typename ValueType>
DenseVector<IndexType> KMeans<IndexType,ValueType>::computeRepartition(
    const std::vector<DenseVector<ValueType>>& coordinates,
    const std::vector<DenseVector<ValueType>>& nodeWeights,
    const Settings settings,
    Metrics<ValueType>& metrics) {

    const IndexType localN = coordinates[0].getLocalValues().size();
    const scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistributionPtr()->getCommunicatorPtr();
    const IndexType p = comm->getSize();
    SCAI_ASSERT_EQ_ERROR(p, settings.numBlocks, "Deriving the previous partition from the distribution cannot work for p != k");
    const IndexType numNodeWeights = nodeWeights.size();

    // calculate the global weight sum to set the block sizes
    // TODO: the local weight sums are already calculated in findLocalCenters, maybe extract info from there
    std::vector<std::vector<ValueType>> blockSizes(numNodeWeights);

    for (IndexType i = 0; i < numNodeWeights; i++)
    {
        scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights[i].getLocalValues());
        SCAI_ASSERT_EQ_ERROR(rWeights.size(), localN, "Mismatch of nodeWeights and coordinates size. Check distributions.");
        //const ValueType localWeightSum = std::accumulate(rWeights.begin(), rWeights.end(), 0.0);
        const ValueType localWeightSum = scai::utilskernel::HArrayUtils::sum( nodeWeights[i].getLocalValues() );
        const ValueType globalWeightSum = comm->sum(localWeightSum);
        blockSizes[i] = std::vector<ValueType>(settings.numBlocks, globalWeightSum/settings.numBlocks);
    }

    std::chrono::time_point<std::chrono::high_resolution_clock> startCents = std::chrono::high_resolution_clock::now();
    //
    // TODO: change to findCenters
    //
    std::vector<std::vector<ValueType> > initialCenters = findLocalCenters(coordinates, nodeWeights[0]);
    std::chrono::duration<ValueType,std::ratio<1>> centTime = std::chrono::high_resolution_clock::now() - startCents;
    ValueType time = centTime.count();
    std::cout<< comm->getRank()<< ": time " << time << std::endl;

    //in this case, the previous partition is the rank of every PE
    DenseVector<IndexType> previous( coordinates[0].getDistributionPtr(), 0);
    
    {   //set previous locally
        const IndexType rank = comm->getRank();
        scai::hmemo::WriteAccess<IndexType> wLocalPrev( previous.getLocalValues() );
        for(int i=0; i<wLocalPrev.size(); i++){
            wLocalPrev[i] = rank;
        }

    }

    //give initial centers as an array; this is required because of the
    // hierarchical version
    return computePartition(coordinates, nodeWeights, blockSizes, previous, {initialCenters}, settings, metrics);
}



template<typename IndexType, typename ValueType>
DenseVector<IndexType> KMeans<IndexType,ValueType>::computeRepartition(
    const std::vector<DenseVector<ValueType>>& coordinates,
    const std::vector<DenseVector<ValueType>>& nodeWeights,
    const std::vector<std::vector<ValueType>>& blockSizes,
    const DenseVector<IndexType>& previous,
    const Settings settings) {

    const IndexType localN = previous.getLocalValues().size();
    scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistributionPtr()->getCommunicatorPtr();
    std::vector<std::vector<ValueType> > initialCenters;

    if (settings.numBlocks == comm->getSize()
            && comm->all(scai::utilskernel::HArrayUtils::max(previous.getLocalValues()) == comm->getRank())
            && comm->all(scai::utilskernel::HArrayUtils::min(previous.getLocalValues()) == comm->getRank())) {
        // partition is equal to distribution
        // TODO:: change with findCenters
        initialCenters = findLocalCenters(coordinates, nodeWeights[0]);
    } else {
        std::vector<IndexType> indices(localN);
        std::iota(indices.begin(), indices.end(), 0);
        initialCenters = findCenters(coordinates, previous, settings.numBlocks, indices.begin(), indices.end(), nodeWeights);
    }

    std::vector<point<ValueType>> transpCenters = vectorTranspose(initialCenters);
    SCAI_ASSERT_EQ_ERROR(transpCenters[0].size(), settings.dimensions, "Wrong centers dimension?");

    // just one group with all the centers; needed in the hierarchical version
    std::vector<std::vector<point<ValueType>>> groupOfCenters = { transpCenters };

    SCAI_ASSERT_EQ_ERROR(groupOfCenters[0][0].size(), settings.dimensions, "Wrong centers dimension?");

    Settings tmpSettings = settings;
    tmpSettings.repartition = true;

    Metrics<ValueType> metrics(settings);

    return computePartition(coordinates, nodeWeights, blockSizes, previous, groupOfCenters, tmpSettings, metrics);
}

// WARNING: if settings.repartition=true then partition has a different meaning: is the partition to be rebalanced,
// not the partition from the previous hierarchy level.
// TODO?: add another DenseVector in order not to confuse the two?

// core implementation
template<typename IndexType, typename ValueType>
DenseVector<IndexType> KMeans<IndexType,ValueType>::computePartition(
    const std::vector<DenseVector<ValueType>> &coordinates, \
    const std::vector<DenseVector<ValueType>> &nodeWeights, \
    const std::vector<std::vector<ValueType>> &targetBlockWeights, \
    const DenseVector<IndexType> &partition, // if repartition, this is the partition to be rebalanced
    std::vector<std::vector<point<ValueType>>> &centers, \
    std::vector<std::vector<ValueType>> &influence, \
    const Settings settings, \
    Metrics<ValueType>& metrics) {

    SCAI_REGION("KMeans.computePartition");
    std::chrono::time_point<std::chrono::high_resolution_clock> KMeansStart = std::chrono::high_resolution_clock::now();

    // if repartition, by convention, numOldBlocks=1=center.size()
    // the number of blocks from the previous hierarchy level
    const IndexType numOldBlocks = centers.size();
    if (settings.debugMode and not settings.repartition) {
        const IndexType maxPart = partition.max(); // global operation
        SCAI_ASSERT_EQ_ERROR(numOldBlocks-1, maxPart, "The provided partition must have equal number of blocks as the length of the vector with the new number of blocks per part");
    }

    const IndexType numNodeWeights = nodeWeights.size();
    assert(targetBlockWeights.size() == numNodeWeights);

    std::vector<bool> heterogeneousBlockSizes(numNodeWeights, false);
    for (IndexType i = 0; i < targetBlockWeights.size(); i++) {
        auto minMax = std::minmax_element(targetBlockWeights[i].begin(), targetBlockWeights[i].end());
        if (*minMax.first != *minMax.second) {
            heterogeneousBlockSizes[i] = true;
            //if (settings.erodeInfluence) {
            //    throw std::logic_error("ErodeInfluence setting is not supported for heterogeneous blocks");
            //}
        }
    }

    // the number of new blocks per old block and the total number of new blocks
    std::vector<IndexType> blockSizesPrefixSum(numOldBlocks+1, 0);
    // in a sense, this is the new k = settings.numBlocks
    IndexType totalNumNewBlocks = 0;

    // if repartition, blockSizesPrefixSum=[ 0, totalNumNewBlocks ]
    for (int b=0; b<numOldBlocks; b++) {
        blockSizesPrefixSum[b+1] += blockSizesPrefixSum[b]+centers[b].size();
        totalNumNewBlocks += centers[b].size();
    }

    // if repartition, centers1DVector=centers[0] (remember, centers.size()=1)
    // Basically, one vector with all the centers and the blockSizesPrefixSum is "useless"

    // convert to a 1D vector
    std::vector<point<ValueType>> centers1DVector;
    for (int b=0; b<numOldBlocks; b++) {
        const unsigned int k = blockSizesPrefixSum[b+1]-blockSizesPrefixSum[b];
        assert(k==centers[b].size()); // not really needed, TODO: remove?
        for (IndexType i=0; i<k; i++) {
            centers1DVector.push_back(centers[b][i]);
        }
    }
    SCAI_ASSERT_EQ_ERROR(centers1DVector.size(), totalNumNewBlocks, "Vector size mismatch");

    const IndexType dim = coordinates.size();
    assert(dim > 0);
    const IndexType localN = coordinates[0].getLocalValues().size();
    const IndexType globalN = coordinates[0].size();
    for (IndexType i = 0; i < numNodeWeights; i++) {
        SCAI_ASSERT_EQ_ERROR(nodeWeights[i].getLocalValues().size(), localN, "Mismatch between node weights and coordinate size.");
    }
    SCAI_ASSERT_EQ_ERROR(centers[0][0].size(), dim, "Center dimensions mismatch");
    SCAI_ASSERT_EQ_ERROR(centers1DVector[0].size(), dim, "Center dimensions mismatch");

    scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistributionPtr()->getCommunicatorPtr();

    const IndexType p = comm->getSize();

    //
    // copy/convert node weights
    //

    std::vector<ValueType> nodeWeightSum(nodeWeights.size());
    std::vector<std::vector<ValueType>> convertedNodeWeights(nodeWeights.size());

    for (IndexType i=0; i<numNodeWeights; i++) {
        nodeWeightSum[i] = nodeWeights[i].sum();

        scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights[i].getLocalValues());
        convertedNodeWeights[i] = std::vector<ValueType>(rWeights.get(), rWeights.get()+localN);

        const ValueType blockWeightSum = std::accumulate(targetBlockWeights[i].begin(), targetBlockWeights[i].end(), 0.0);
        if (nodeWeightSum[i] > blockWeightSum*(1+settings.epsilon)) {
            for (ValueType blockSize : targetBlockWeights[i]) {
                PRINT0(std::to_string(blockSize) + " ");
            }
            throw std::invalid_argument("The total weight of the wanted blocks is " + std::to_string(blockWeightSum) + " which is smaller than the total vertex weight which is " + std::to_string(nodeWeightSum[i]) + "; i.e., the given input does not fit into the given block weights. Maybe you should try calling CommTree::adaptWeights().");
        }
    }

    // normalize node weights for adaptive influence calculation
    std::vector<std::vector<ValueType>> normalizedNodeWeights(numNodeWeights, std::vector<ValueType>(localN, 1));

    if (numNodeWeights > 1) {
        for (IndexType i = 0; i < localN; i++) {
            ValueType weightSum = 0;
            for (IndexType j = 0; j < numNodeWeights; j++) {
                weightSum += convertedNodeWeights[j][i];
            }
            for (IndexType j = 0; j < numNodeWeights; j++) {
                normalizedNodeWeights[j][i] = convertedNodeWeights[j][i] / weightSum;
            }
        }
    }

    //
    // copy coordinates
    //

    // min and max for local part of the coordinates
    std::vector<ValueType> minCoords(dim);
    std::vector<ValueType> maxCoords(dim);

    std::vector<std::vector<ValueType> > convertedCoords(dim);
    {
        for (IndexType d = 0; d < dim; d++) {
            scai::hmemo::ReadAccess<ValueType> rAccess(coordinates[d].getLocalValues());
            convertedCoords[d] = std::vector<ValueType>(rAccess.get(), rAccess.get()+localN);

            minCoords[d] = *std::min_element(convertedCoords[d].begin(), convertedCoords[d].end());
            maxCoords[d] = *std::max_element(convertedCoords[d].begin(), convertedCoords[d].end());

            assert(convertedCoords[d].size() == localN);
        }
    }

    std::vector<ValueType> globalMinCoords(dim);
    std::vector<ValueType> globalMaxCoords(dim);
    comm->minImpl(globalMinCoords.data(), minCoords.data(), dim, scai::common::TypeTraits<ValueType>::stype);
    comm->maxImpl(globalMaxCoords.data(), maxCoords.data(), dim, scai::common::TypeTraits<ValueType>::stype);

    ValueType diagonalLength = 0;
    ValueType volume = 1;
    ValueType localVolume = 1;
    for (IndexType d = 0; d < dim; d++) {
        const ValueType diff = globalMaxCoords[d] - globalMinCoords[d];
        const ValueType localDiff = maxCoords[d] - minCoords[d]; 
        diagonalLength += diff*diff;
        volume *= diff;
        localVolume *= localDiff;
    }

    // the bounding box is per PE. no need to change for the hierarchical version
    QuadNodeCartesianEuclid<ValueType> boundingBox(minCoords, maxCoords);
    if (settings.verbose) {
        std::cout << "(PE id, localN) = (" << comm->getRank() << ", "<< localN << ")" << std::endl;
        comm->synchronize();
        std::cout << "bBox volume: (PE id, localVolume/(globalVolume/p) = (" << comm->getRank() << ", "<< localVolume / (volume / p) << ")" << std::endl;
    }

    diagonalLength = std::sqrt(diagonalLength);
    const ValueType expectedBlockDiameter = pow(volume /totalNumNewBlocks, 1.0/dim);

    std::vector<ValueType> upperBoundOwnCenter(localN, std::numeric_limits<ValueType>::max());
    std::vector<ValueType> lowerBoundNextCenter(localN, 0);

    //
    // prepare sampling
    //

    std::vector<IndexType> localIndices(localN);
    std::iota(localIndices.begin(), localIndices.end(), 0);

    // hierar: in the heterogenous and hierarchical case, minSamplingNodes
    // makes more sense to be a percentage of the nodes, not a number. Or not?
    // hierar: number of sampling nodes is calculated per PE, right? not per block.

    const ValueType avgBlocksPerPE = ValueType(totalNumNewBlocks)/p;
    // We can calculate precisely the number of local old blocks using "partition"
    // but we will have to go over all local points
    IndexType minNodes = settings.minSamplingNodes*avgBlocksPerPE;
    if (settings.minSamplingNodes==-1) {
        minNodes = localN;
    }

    assert(minNodes > 0);
    IndexType samplingRounds = 0;   // number of rounds needed to see all points
    std::vector<IndexType> samples;

    const bool randomInitialization = comm->all(localN > minNodes);

    // perform sampling
    {
        if (randomInitialization) {
            //ITI::GraphUtils<IndexType, ValueType>::FisherYatesShuffle(localIndices.begin(), localIndices.end(), localN);
            // TODO: the cantor shuffle is more stable; random shuffling can yield better
            // results occasionally but has higher fluctuation/variance
            localIndices = GraphUtils<IndexType,ValueType>::indexReorderCantor(localN);

            SCAI_ASSERT_EQ_ERROR(*std::max_element(localIndices.begin(), localIndices.end()), localN -1, "Error in index reordering");
            SCAI_ASSERT_EQ_ERROR(*std::min_element(localIndices.begin(), localIndices.end()), 0, "Error in index reordering");

            samplingRounds = std::ceil(std::log2(globalN / ValueType(settings.minSamplingNodes*totalNumNewBlocks)))+1;

            samples.resize(samplingRounds);
            samples[0] = std::min(minNodes, localN);
        }

        if (settings.verbose) {
            PRINT0(*comm << ": localN= "<< localN << ", minNodes= " << minNodes << ", samplingRounds= " << samplingRounds << ", lastIndex: " << *localIndices.end());
        }
        if (samplingRounds > 0 && settings.verbose) {
            if (comm->getRank() == 0) std::cout << "Starting with " << samplingRounds << " sampling rounds." << std::endl;
        }
        // double the number of samples per round
        for (IndexType i = 1; i < samplingRounds; i++) {
            samples[i] = std::min(IndexType(samples[i-1]*2), localN);
        }
        if (samplingRounds > 0) {
            samples[samplingRounds-1] = localN;
        }
    }

    IndexType iter = 0;
    ValueType delta = 0;
    bool balanced = false;
    const ValueType threshold = 0.002*diagonalLength;// TODO: take global point density into account
    const IndexType maxIterations = settings.maxKMeansIterations;
    const typename std::vector<IndexType>::iterator firstIndex = localIndices.begin();
    typename std::vector<IndexType>::iterator lastIndex = localIndices.end();
    std::vector<ValueType> imbalances(numNodeWeights, 1);
    std::vector<ValueType> imbalancesOld(numNodeWeights, 0);

    // result[i]=b, means that point i belongs to cluster/block b
    DenseVector<IndexType> result(coordinates[0].getDistributionPtr(), 0);
    DenseVector<IndexType> mostBalancedResult(coordinates[0].getDistributionPtr(), 0);
    ValueType minImbalance = settings.numBlocks+1; //to store the solution with the minimum imbalance
    ValueType minAchievedImbalance = settings.epsilon; //used with multiple weights; TODO: adapt for multiple balance constrains

    // TODO, recheck:
    // if repartition, should it be result = previous???
    if (settings.repartition) {
        assert(partition.getDistributionPtr()->isEqual(*coordinates[0].getDistributionPtr()));
        result = partition;
    }
    if(comm->getRank()==0){
        std::cout<<"Delta threshold is " << threshold <<std::endl;
    }

    do {
        std::chrono::time_point<std::chrono::high_resolution_clock> iterStart = std::chrono::high_resolution_clock::now();
        if (iter < samplingRounds) {
            SCAI_ASSERT_LE_ERROR(samples[iter], localN, "invalid number of samples");
            lastIndex = localIndices.begin() + samples[iter];
            std::sort(localIndices.begin(), lastIndex);// sorting not really necessary, but increases locality
            [[maybe_unused]] ValueType ratio = ValueType(comm->sum(samples[iter])) / globalN;
            assert(ratio <= 1);
        } else {
            SCAI_ASSERT_EQ_ERROR(lastIndex - firstIndex, localN, "invalid iterators");
            assert(lastIndex == localIndices.end());
        }

        std::vector<std::vector<ValueType>> adjustedBlockSizes(numNodeWeights);

        for (IndexType i = 0; i < numNodeWeights; i++) {

            ValueType localSampleWeightSum = 0;
            {
                scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights[i].getLocalValues());
                for (auto it = firstIndex; it != lastIndex; it++) {
                    localSampleWeightSum += rWeights[*it];
                }
            }

            const ValueType totalSampledWeightSum = comm->sum(localSampleWeightSum);
            const ValueType ratio = totalSampledWeightSum / nodeWeightSum[i];
            adjustedBlockSizes[i].resize(targetBlockWeights[i].size());
            //TODO: merge to one assertion; or not...
            if( std::is_same<ValueType,float>::value ){
                //  SCAI_ASSERT_LE_ERROR(totalSampledWeightSum, nodeWeightSum[i]*(1+1e-4), "Error in sampled weight sum.");
            }else{  //double
                SCAI_ASSERT_LE_ERROR(totalSampledWeightSum, nodeWeightSum[i]*(1+1e-8), "Error in sampled weight sum.");
            }

            for (IndexType j = 0; j < targetBlockWeights[i].size(); j++) {
                adjustedBlockSizes[i][j] = ValueType(targetBlockWeights[i][j]) * ratio;
                if (settings.verbose && iter < samplingRounds) {
                    if (j == 0 || heterogeneousBlockSizes[i]) {
                        PRINT0("Adjusted " + std::to_string(targetBlockWeights[i][j]) + " down to " + std::to_string(adjustedBlockSizes[i][j]));
                    }
                }
            }
        }

        std::vector<ValueType> timePerPE(comm->getSize(), 0.0);

        result = assignBlocks(convertedCoords, centers1DVector, blockSizesPrefixSum, firstIndex, lastIndex, convertedNodeWeights, normalizedNodeWeights, result, partition, adjustedBlockSizes, boundingBox, upperBoundOwnCenter, lowerBoundNextCenter, influence, imbalances, settings, metrics);

        // TODO: too much info? remove?
        if (settings.verbose and settings.debugMode) {
            comm->sumImpl(timePerPE.data(), timePerPE.data(), comm->getSize(), scai::common::TypeTraits<ValueType>::stype);
            if (comm->getRank()==0) {
                vector<IndexType> indices(timePerPE.size());
                std::iota(indices.begin(), indices.end(), 0);
                std::sort(indices.begin(), indices.end(),
                [&timePerPE](int i, int j) {
                    return timePerPE[i]<timePerPE[j];
                });

                for (int i=0; i<comm->getSize(); i++) {
                    std::cout << indices[i]<< ": time for PE: " << timePerPE[indices[i]] << std::endl;
                    std::cout << "(" << indices[i] << "," << timePerPE[indices[i]] << ")" << std::endl;
                }
            }
        }

        // TODO: adapt for multiple weights
        std::vector<std::vector<ValueType>> newCenters = findCenters(coordinates, result, totalNumNewBlocks, firstIndex, lastIndex, nodeWeights);

        // newCenters have reversed order of the vectors
        // maybe turn centers to a 1D vector already in computePartition?

        std::vector<point<ValueType>> transCenters = vectorTranspose(newCenters);
        assert(transCenters.size()==totalNumNewBlocks);
        assert(transCenters[0].size()==dim);

        // keep centroids of empty blocks at their last known position
        for (IndexType j = 0; j <totalNumNewBlocks; j++) {
            // center for block j is empty
            if (std::isnan(transCenters[j][0])) {
                transCenters[j] = centers1DVector[j];
            }
        }
        std::vector<ValueType> squaredDeltas(totalNumNewBlocks,0);
        std::vector<ValueType> deltas(totalNumNewBlocks,0);
        std::vector<std::vector<ValueType>> oldInfluence = influence;
        ValueType minRatio = std::numeric_limits<ValueType>::max();

        for (IndexType j = 0; j < totalNumNewBlocks; j++) {
            for (int d = 0; d < dim; d++) {
                SCAI_ASSERT_LE_ERROR(transCenters[j][d], globalMaxCoords[d]+ 1e-6, "New center coordinate out of bounds");
                SCAI_ASSERT_GE_ERROR(transCenters[j][d], globalMinCoords[d]- 1e-6, "New center coordinate out of bounds");
                ValueType diff = (centers1DVector[j][d] - transCenters[j][d]);
                squaredDeltas[j] += diff*diff;
            }

            deltas[j] = std::sqrt(squaredDeltas[j]);

            if (settings.erodeInfluence) {
                const ValueType erosionFactor = 2/(1+exp(-std::max(deltas[j]/expectedBlockDiameter-ValueType(0.1), ValueType(0.0)))) - 1;
                for (IndexType i = 0; i < numNodeWeights; i++) {
                    influence[i][j] = exp((1-erosionFactor)*log(influence[i][j]));
                    if (oldInfluence[i][j] / influence[i][j] < minRatio) minRatio = oldInfluence[i][j] / influence[i][j];
                }
            }
        }

        centers1DVector = transCenters;

        delta = *std::max_element(deltas.begin(), deltas.end());
        assert(delta >= 0);
        const ValueType deltaSq = delta*delta;
        ValueType maxInfluence = 0;
        for (IndexType w = 0; w < numNodeWeights; w++) {
            maxInfluence = std::max(maxInfluence, *std::max_element(influence[w].begin(), influence[w].end()));
        }

        {
            SCAI_REGION("KMeans.computePartition.updateBounds");
            scai::hmemo::ReadAccess<IndexType> rResult(result.getLocalValues());

            for (auto it = firstIndex; it != lastIndex; it++) {
                const IndexType i = *it;
                IndexType cluster = rResult[i];
                assert(cluster<totalNumNewBlocks);

                ValueType influenceEffect = 0;
                for (IndexType w = 0; w < numNodeWeights; w++) {
                    influenceEffect += influence[w][cluster]*normalizedNodeWeights[w][i];
                }

                if (settings.erodeInfluence) {
                    // WARNING: erodeInfluence not supported for hierarchical version
                    // TODO: or it is?? or should it be??

//if (numNodeWeights > 1) throw std::logic_error("Influence erosion not yet implemented for multiple weights.");

                    // update due to erosion
                    upperBoundOwnCenter[i] *= (influence[0][cluster] / oldInfluence[0][cluster]) + 1e-6;
                    lowerBoundNextCenter[i] *= minRatio - 1e-6;
                }

                // update due to delta
                upperBoundOwnCenter[i] += (2*deltas[cluster]*std::sqrt(upperBoundOwnCenter[i]/influenceEffect) + squaredDeltas[cluster])*(influenceEffect + 1e-6);

                ValueType pureSqrt(std::sqrt(lowerBoundNextCenter[i]/maxInfluence));
                if (pureSqrt < delta) {
                    lowerBoundNextCenter[i] = 0;
                } else {
                    ValueType diff = (-2*delta*pureSqrt + deltaSq)*(maxInfluence + 1e-6);
                    assert(diff <= 0);
                    lowerBoundNextCenter[i] += diff;
                    if (!(lowerBoundNextCenter[i] > 0)) lowerBoundNextCenter[i] = 0;
                }

                assert(std::isfinite(lowerBoundNextCenter[i]));
            }
        }

        // find local weight of each block
        std::vector<std::vector<ValueType>> currentBlockWeights(numNodeWeights, std::vector<ValueType>(totalNumNewBlocks,0.0));
        {
            scai::hmemo::ReadAccess<IndexType> rResult(result.getLocalValues());
            for (IndexType j = 0; j < numNodeWeights; j++) {
                scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights[j].getLocalValues());
                for (auto it = firstIndex; it != lastIndex; it++) {
                    const IndexType i = *it;
                    IndexType cluster = rResult[i];
                    currentBlockWeights[j][cluster] += rWeights[i];
                }
            }
        }

        // print times before global reduce step
        // aux<IndexType,ValueType>::timeMeasurement(iterStart);
        std::chrono::duration<ValueType,std::ratio<1>> balanceTime = std::chrono::high_resolution_clock::now() - iterStart;
        ValueType time = balanceTime.count();

        if (settings.verbose) {
            PRINT0(*comm <<": in computePartition, iteration time: " << time);
        }

        {
            SCAI_REGION("KMeans.computePartition.currentBlockWeightSum");
            for (IndexType i = 0; i < numNodeWeights; i++) {
                comm->sumImpl(currentBlockWeights[i].data(), currentBlockWeights[i].data(), totalNumNewBlocks, scai::common::TypeTraits<ValueType>::stype);
            }
        }

        // check if all blocks are balanced
        balanced = true;
        for (IndexType i = 0; i < numNodeWeights; i++) {
            for (IndexType j=0; j<totalNumNewBlocks; j++) {
                if (currentBlockWeights[i][j] > adjustedBlockSizes[i][j]*(1+settings.epsilon)) {
                    balanced = false;
                }
            }
        }

        ValueType maxTime=0;
        if (settings.verbose) {
            balanceTime = std::chrono::high_resolution_clock::now() - iterStart;
            maxTime = comm->max(balanceTime.count());
        }

        if (comm->getRank() == 0) {
            std::cout << "i: " << iter<< ", delta: " << delta << ", imbalance=";
            for (IndexType i = 0; i < numNodeWeights; i++) {
                std::cout << " " << imbalances[i];
            }
            if (settings.verbose) {
                std::cout << ", time : "<< maxTime;
            }
            std::cout << std::endl;
        }

        //if the balance does not change much, consider it balanced
        ValueType imbalanceDiff =0;
        for (IndexType i = 0; i < numNodeWeights; i++) {
            imbalanceDiff += std::abs( imbalancesOld[i]-imbalances[i] );
        }
        if( imbalanceDiff/numNodeWeights < 0.001 ){
            balanced = true;
        }
        imbalancesOld = imbalances;
        
        //we must have sampled all indices, otherwise a solution will be a partial
        //assignment as the not sampled indices will belong to block 0 (from the initialization)

        if(settings.keepMostBalanced  and lastIndex == localIndices.end() ){
            const ValueType currMinImbalance = *std::min_element( imbalances.begin(), imbalances.end() );
            const ValueType currMaxImbalance = *std::max_element( imbalances.begin(), imbalances.end() );

            //if only one weight, keep the solution with minimum imbalance
            if( numNodeWeights<2 and currMinImbalance<minImbalance ){
                 if(comm->getRank()==0){
                    std::cout <<"Storing most balanced solution with minimum imbalance " << currMinImbalance << std::endl;
                }
                mostBalancedResult.assign(result);
                minImbalance = currMinImbalance;
            }

            //for more weights, try to keep solution that fulfills all balance constrains first
            if( numNodeWeights>1){
                //if max is less than epsilon then all weights are
                if(currMaxImbalance<minAchievedImbalance){
                    if(comm->getRank()==0){
                        std::cout <<"Storing most balanced solution with maximum imbalance " << currMaxImbalance << std::endl;
                    }
                    mostBalancedResult.assign(result);
                    minAchievedImbalance = currMaxImbalance;
                }
                //else if (currMinImbalance<minImbalance){ //keep the solution with the minimum maximum imbalance
                else if( currMaxImbalance<minImbalance){
                    if(comm->getRank()==0){
                        std::cout <<"Storing most balanced solution with maximum imbalance " << currMaxImbalance << std::endl;
                    }
                    mostBalancedResult.assign(result);
                    //minImbalance = currMinImbalance;
                    minImbalance = currMaxImbalance;
                }
            }
        }

        //metrics.kmeansProfiling.push_back(std::make_tuple(delta, maxTime, imbalances[0]));
        iter++;

    } while (iter < samplingRounds or (iter < maxIterations && (delta > threshold || !balanced)));


    std::chrono::duration<ValueType,std::ratio<1>> KMeansTime = std::chrono::high_resolution_clock::now() - KMeansStart;
    ValueType time = comm->max(KMeansTime.count());

    PRINT0("total KMeans time: " << time << " , number of iterations: " << iter);
    //special time for the core kmeans
    metrics.MM["timeKmeans"] = time;

    if(settings.keepMostBalanced){
        return mostBalancedResult;
    }else{
        return result;
    }

}// computePartition
// ------------------------------------------------------------------------------


template<typename IndexType, typename ValueType>
DenseVector<IndexType> KMeans<IndexType,ValueType>::computePartition(
    const std::vector<DenseVector<ValueType>> &coordinates, \
    const std::vector<DenseVector<ValueType>> &nodeWeights, \
    const std::vector<std::vector<ValueType>> &targetBlockWeights, \
    const DenseVector<IndexType> &partition, // if repartition, this is the partition to be rebalanced
    std::vector<std::vector<point<ValueType>>> centers, \
    const Settings settings, \
    Metrics<ValueType>& metrics) {

    const IndexType numNodeWeights = nodeWeights.size();

    IndexType totalNumNewBlocks = 0;
    for (int b=0; b<centers.size(); b++) {
        totalNumNewBlocks += centers[b].size();
    }

    //initialize influence with 1
    std::vector<std::vector<ValueType>> influence(numNodeWeights, std::vector<ValueType>(totalNumNewBlocks, 1));

    return computePartition(coordinates, nodeWeights, targetBlockWeights, partition, centers, influence, settings, metrics);
}//computePartition
// ------------------------------------------------------------------------------



template<typename IndexType, typename ValueType>
DenseVector<IndexType> KMeans<IndexType,ValueType>::computePartition(
    const std::vector<DenseVector<ValueType>> &coordinates,
    const Settings settings) {

    const scai::dmemo::DistributionPtr dist = coordinates[0].getDistributionPtr();
    const IndexType globalN = dist->getGlobalSize();
    const scai::lama::DenseVector<ValueType> unitNodeWeights = scai::lama::DenseVector<ValueType>(dist, 1);
    const std::vector<scai::lama::DenseVector<ValueType>> nodeWeights = {unitNodeWeights};
    std::vector<std::vector<ValueType>> blockSizes(1, std::vector<ValueType>(settings.numBlocks, std::ceil( (ValueType)globalN/settings.numBlocks)));
    Metrics<ValueType> metrics(settings);

    return computePartition(coordinates, nodeWeights, blockSizes, settings, metrics);
}
// ------------------------------------------------------------------------------

// wrapper 1 - called initially with no centers parameter
template<typename IndexType, typename ValueType>
DenseVector<IndexType> KMeans<IndexType,ValueType>::computePartition(
    const std::vector<DenseVector<ValueType>> &coordinates,
    const std::vector<DenseVector<ValueType>> &nodeWeights,
    const std::vector<std::vector<ValueType>> &blockSizes,
    const Settings settings,
    Metrics<ValueType>& metrics) {

    std::vector<ValueType> minCoords(settings.dimensions);
    std::vector<ValueType> maxCoords(settings.dimensions);
    std::tie(minCoords, maxCoords) = getGlobalMinMaxCoords(coordinates);

    std::vector<point<ValueType>> centers = findInitialCentersSFC(coordinates, minCoords, maxCoords, settings);
    SCAI_ASSERT_EQ_ERROR(centers.size(), settings.numBlocks, "Number of centers is not correct");
    SCAI_ASSERT_EQ_ERROR(centers[0].size(), settings.dimensions, "Dimension of centers is not correct");

    // just one group with all the centers; needed in the hierarchical version
    std::vector<std::vector<point<ValueType>>> groupOfCenters = { centers };

    // every point belongs to one block in the beginning
    scai::lama::DenseVector<IndexType> partition(coordinates[0].getDistributionPtr(), 0);

    return computePartition(coordinates, nodeWeights, blockSizes, partition, groupOfCenters, settings, metrics);
}


// ------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
DenseVector<IndexType> KMeans<IndexType,ValueType>::computeHierarchicalPartition(
    std::vector<DenseVector<ValueType>> &coordinates,
    std::vector<DenseVector<ValueType>> &nodeWeights,
    const CommTree<IndexType,ValueType> &commTree,
    Settings settings,
    Metrics<ValueType>& metrics) {
    SCAI_REGION("KMeans.computeHierarchicalPartition");

    typedef cNode<IndexType,ValueType> cNode;

    // check although numBlocks is not needed or used
    SCAI_ASSERT_EQ_ERROR(settings.numBlocks, commTree.getNumLeaves(), "The number of leaves and number of blocks must agree");

    // get global communicator
    scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistributionPtr()->getCommunicatorPtr();

    if (settings.erodeInfluence) {
        if (comm->getRank()==0) {
            std::cout << "WARNING: erode influence is not fully supported for the hierarchical version.\nWill try but maybe set it to false." << std::endl;
        }
        //settings.erodeInfluence = false;
    }

    const IndexType numNodeWeights = nodeWeights.size();
    const scai::dmemo::DistributionPtr dist = coordinates[0].getDistributionPtr();

    // redistribute points based on their hilbert curve index
    // warning: this functions redistributes the coordinates and the node weights.
    // TODO: is this supposed to be here? it is also in ParcoRepart::partitionGraph

    HilbertCurve<IndexType,ValueType>::redistribute(coordinates, nodeWeights, settings, metrics);

    if (settings.debugMode) {
        // added check to verify that the points are indeed distributed
        // based on the hilbert curve. Otherwise, the prefix sum needed to
        // calculate the centers, does not have the desired meaning.
        for (IndexType i=0; i<numNodeWeights; i++) {
            bool hasHilbertDist = HilbertCurve<IndexType, ValueType>::confirmHilbertDistribution(coordinates, nodeWeights[i], settings);
            SCAI_ASSERT_EQ_ERROR(hasHilbertDist, true, "Input must be distributed according to a hilbert curve distribution");
        }
    }

    std::vector<ValueType> minCoords(settings.dimensions);
    std::vector<ValueType> maxCoords(settings.dimensions);
    std::tie(minCoords, maxCoords) = getGlobalMinMaxCoords(coordinates);

    // used later for debugging and calculating imbalance
    std::vector<ValueType> totalWeightSum(numNodeWeights);
    for (IndexType i = 0; i < numNodeWeights; i++)
    {
        scai::hmemo::ReadAccess<ValueType> rW(nodeWeights[i].getLocalValues());
        ValueType localW = 0;
        for (IndexType j=0; j<rW.size(); j++) {
            localW += rW[j];
        }
        totalWeightSum[i] = comm->sum(localW);
    }

    // typedef of commNode from CommTree.h, see also KMeans.h
    cNode root = commTree.getRoot();
    if (settings.debugMode) {
        PRINT("Starting hierarchical KMeans.\nRoot node: ");
        root.print();
    }

    // every point belongs to one block in the beginning
    scai::lama::DenseVector<IndexType> partition(coordinates[0].getDistributionPtr(), 0);

    // skip root. If we start from the root, we will know the number
    // of blocks but not the memory and speed per block
    for (unsigned int h=1; h<commTree.getNumHierLevels(); h++) {

        /*
        There are already as many blocks as the number of leaves
        of the previous hierarchy level. The new number of blocks per
        old block is prevLeaf.numChildren. Example, if previous level
        had 3 leaves with 4, 6 and 10 children respectively, then
        the number of new blocks that we will partition in this step is
        4 for block 0, 6 for block 1 and 10 for block 2, in total 20.
        */

        // in how many blocks each known block (from the previous hier level) will be partitioned
        // numNewBlocksPerOldBlock[i]=k means that, current/old block i should be
        // partitioned into k new blocks

        std::vector<cNode> thisLevel = commTree.getHierLevel(h);

        PRINT0("-- Hierarchy level " << h << " with " << thisLevel.size() << " nodes");
        // TODO: probably too verbosy, remove
        if (settings.debugMode) {
            PRINT0("******* in debug mode");
            for (cNode c: thisLevel) { // print all nodes of this level
                c.print();
            }
        }

        //
        // 1- find initial centers for this hierarchy level
        //
        // Only the new level is passed and the previous level is
        // reconstructed internally

        std::vector<std::vector<point<ValueType>>> groupOfCenters = findInitialCentersSFC(coordinates, minCoords, maxCoords, partition, thisLevel, settings);

        SCAI_ASSERT_EQ_ERROR(groupOfCenters.size(), commTree.getHierLevel(h-1).size(), "Wrong number of blocks calculated");
        if (settings.debugMode) {
            PRINT0("******* in debug mode");
            IndexType sumNumCenters = 0;
            for (int g=0; g<groupOfCenters.size(); g++) {
                sumNumCenters += groupOfCenters[g].size();
            }
            SCAI_ASSERT_EQ_ERROR(sumNumCenters, thisLevel.size(), "Mismatch in number of new centers and hierarchy nodes")
        }

        // number of old, known blocks == previous level size
        IndexType numOldBlocks = groupOfCenters.size();

        // number of new blocks each old blocks must be partitioned to
        std::vector<unsigned int> numNewBlocks = commTree.getGrouping(thisLevel);
        SCAI_ASSERT_EQ_ERROR(numOldBlocks, numNewBlocks.size(), "Hierarchy level size mismatch");
        const IndexType totalNumNewBlocks = std::accumulate(numNewBlocks.begin(), numNewBlocks.end(), 0);

        if (settings.debugMode) {
            const IndexType maxPart = partition.max(); // global operation
            SCAI_ASSERT_EQ_ERROR(numOldBlocks-1, maxPart, "The provided partition must have equal number of blocks as the length of the vector with the new number of blocks per part");
        }

        //
        // 2- main k-means loop
        //

        // get the wanted block sizes for this level of the tree
        std::vector<std::vector<ValueType>> targetBlockWeights = commTree.getBalanceVectors(h);
        SCAI_ASSERT_EQ_ERROR(targetBlockWeights.size(), numNodeWeights, "Wrong number of weights");
        SCAI_ASSERT_EQ_ERROR(targetBlockWeights[0].size(), totalNumNewBlocks, "Wrong size of weights");
        //PRINT0(h << ": " << std::accumulate(targetBlockWeights[0].begin(), targetBlockWeights[0].end(), 0.0));
        // TODO: inside computePartition, settings.numBlocks is not
        // used. We infer the number of new blocks from the groupOfCenters
        // maybe, set also numBlocks for clarity??

        //automatically partition for balance if more than one node weights
        if( settings.focusOnBalance ){
            partition = computePartition(coordinates, nodeWeights, targetBlockWeights, partition, groupOfCenters, settings, metrics);
            partition = computePartition_targetBalance(coordinates, nodeWeights, targetBlockWeights, partition, settings, metrics);
        }else{
            partition = computePartition(coordinates, nodeWeights, targetBlockWeights, partition, groupOfCenters, settings, metrics);
        }

        // TODO: not really needed assertions
        SCAI_ASSERT_EQ_ERROR(coordinates[0].getDistributionPtr()->getLocalSize(),\
                             partition.getDistributionPtr()->getLocalSize(), "Partition distribution mismatch(?)");
        SCAI_ASSERT_EQ_ERROR(nodeWeights[0].getDistributionPtr()->getLocalSize(),\
                             partition.getDistributionPtr()->getLocalSize(), "Partition distribution mismatch(?)");

        if (settings.debugMode) {
            // this check is done before. TODO: remove?
            const IndexType maxPart = partition.max(); // global operation
            SCAI_ASSERT_EQ_ERROR(totalNumNewBlocks-1, maxPart, "The provided old assignment must have equal number of blocks as the length of the vector with the new number of blocks per part");
            if (settings.storeInfo) {
                // FileIO<IndexType,ValueType>::writePartitionParallel(partition, "./partResults/partHKM"+std::to_string(settings.numBlocks)+"_h"+std::to_string(h)+".out");
                FileIO<IndexType,ValueType>::writeDenseVectorCentral(partition, "./partResults/partHKM"+std::to_string(settings.numBlocks)+"_h"+std::to_string(h)+".out");
            }
        }

        // TODO?: remove?
        std::vector<ValueType> imbalances(numNodeWeights);
        for (IndexType i = 0; i < numNodeWeights; i++) {
            imbalances[i] = ITI::GraphUtils<IndexType, ValueType>::computeImbalance(partition, totalNumNewBlocks, nodeWeights[i], targetBlockWeights[i]);
        }

        MSG0("\nFinished hierarchy level " << h <<", partitioned into " << totalNumNewBlocks << " blocks and imbalance is:");
        if (comm->getRank()==0) {
            for (IndexType i = 0; i < imbalances.size(); i++) {
                std::cout<< " " << imbalances[i] <<std::endl;
            }
        }

    } // for (h=1; h<commTree.hierarchyLevels; h++){

    return partition;
}// computeHierarchicalPartition

// --------------------------------------------------------------------------
template<typename IndexType, typename ValueType>
DenseVector<IndexType> KMeans<IndexType,ValueType>::computeHierPlusRepart(
    std::vector<DenseVector<ValueType>> &coordinates,
    std::vector<DenseVector<ValueType>> &nodeWeights,
    const CommTree<IndexType,ValueType> &commTree,
    Settings settings,
    Metrics<ValueType>& metrics) {
    SCAI_REGION("KMeans.computeHierPlusRepart");

    // get a hierarchical partition
    DenseVector<IndexType> result = computeHierarchicalPartition(coordinates, nodeWeights, commTree, settings, metrics);

    std::vector<std::vector<ValueType>> blockSizes = commTree.getBalanceVectors(-1);

    const scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistributionPtr()->getCommunicatorPtr();
    PRINT0("Finished hierarchical partition");

    // refine using a repartition step

    std::chrono::time_point<std::chrono::high_resolution_clock> repartStart = std::chrono::high_resolution_clock::now();
    DenseVector<IndexType> result2 = computeRepartition(coordinates, nodeWeights, blockSizes, result, settings);
    std::chrono::duration<ValueType,std::ratio<1>> repartTime = std::chrono::high_resolution_clock::now() - repartStart;
    metrics.MM["timeKmeans"] += repartTime.count();

    return result2;
}// computeHierPlusRepart


template<typename IndexType, typename ValueType>
DenseVector<IndexType> KMeans<IndexType,ValueType>::computePartition_targetBalance(
    const std::vector<DenseVector<ValueType>> &coordinates,
    const std::vector<DenseVector<ValueType>> &nodeWeights,
    const std::vector<std::vector<ValueType>> &blockSizes,
    DenseVector<IndexType> &result,
    const Settings settings,
    Metrics<ValueType>& metrics){
    SCAI_REGION("KMeans.computePartition_targetBalance");

    const scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistributionPtr()->getCommunicatorPtr();
    const IndexType globalN = coordinates[0].getDistributionPtr()->getGlobalSize();
    const IndexType myRank = comm->getRank();

    //get first result if given one is empty
    if ( result.size()==0 or result.max()==0 ){
        PRINT0( "Preliminary partition not provided, will calculate it now" );
        Settings settingsCopy = settings;
        //settingsCopy.erodeInfluence = true;
        result = ITI::KMeans<IndexType,ValueType>::computePartition(coordinates, nodeWeights, blockSizes, settingsCopy, metrics);
    }

    PRINT0( std::endl<< "Repartitioning"<< std::endl );

    //
    //calculate max imbalance of the input,  maybe the initial partition is balanced enough
    //

    std::vector<ValueType> imbalances( settings.numNodeWeights, 1.0 );

    for(int w=0; w<settings.numNodeWeights; w++){
        //second argument was numBlock but when called from the hierarchical kmeans, the number of blocks is different
        //in earlier levels
        imbalances[w] = GraphUtils<IndexType,ValueType>::computeImbalance(result, blockSizes[w].size(), nodeWeights[w], blockSizes[w] );

        metrics.befRebImbalance.push_back( imbalances[w] );
        metrics.MM["befRebImbalance_w"+std::to_string(w)] = imbalances[w];
    }

    ValueType maxCurrImbalance = *std::max_element( imbalances.begin(), imbalances.end() );
    metrics.MM["befRebImbalance"] = maxCurrImbalance;
    const ValueType targetImbalance = settings.epsilon;
    ValueType imbalanceDiff = maxCurrImbalance - targetImbalance;

    if( myRank==0 ){
        std::cout<< "Imbalances before rebalancing: ";
        for(ValueType im : imbalances )  std::cout << im << ", ";
        std::cout << std::endl;
    }

    if( imbalanceDiff<0){
        if( myRank==0 ){
            std::cout<< "Partition is already balanced enough; will try to lower imbalance further" << std::endl;
        }
        //TODO: maybe return directly?
        imbalanceDiff *= -1; //improve further
    }else if(imbalanceDiff==0){
        imbalanceDiff = 0.00001;
    }else{
        imbalanceDiff *= 1.2; // add 20% to give some slack
    }

    const IndexType numTries = 5;
    const ValueType imbaDelta = imbalanceDiff/(numTries+1); //how much to reduce epsilon
    ValueType pointPerCent = 0.005;
    ValueType maxMinImbalance = maxCurrImbalance;

    Settings settingsCopy = settings;
    //IndexType currWeights = 2; //in the beginning, consider this many weights and add the other later
    settingsCopy.epsilon =  maxCurrImbalance; //settings.epsilon + numTries*0.01;

    const IndexType localN = coordinates[0].getDistributionPtr()->getLocalSize();
    settingsCopy.batchPercent = ((ValueType)100.0)/localN; //this sets batch to 100

    DenseVector<IndexType> bestResult = result;
    
    settingsCopy.epsilons = std::vector<double>(settings.numNodeWeights, maxCurrImbalance-imbaDelta);
    //settingsCopy.epsilons[1] = settingsCopy.epsilons[1]*1.5;

    const std::chrono::time_point<std::chrono::steady_clock> beforeRebalance =  std::chrono::steady_clock::now();

    for(int i=0; i<numTries; i++){
        if( myRank==0 ){
            std::cout <<"Repartition for epsilon(s)= ";
            //if( settingsCopy.epsilons.size()>0 ){
                for( double e : settingsCopy.epsilons )  std::cout<< e << ", " ;
                std::cout << std::endl;
            //}else{
            //    std::cout<< settings.epsilon << std::endl;
            //}
            
        }
        std::chrono::time_point<std::chrono::steady_clock> oneLoopTime =  std::chrono::steady_clock::now();

        if( settings.KMBalanceMethod=="repart" ){
            result = ITI::KMeans<IndexType,ValueType>::computeRepartition(coordinates, nodeWeights, blockSizes, result, settingsCopy);
        }else{
            //this ruins the cut. maybe add a repartition step to fix it
            IndexType numMoves = ITI::KMeans<IndexType,ValueType>::rebalance(coordinates, nodeWeights, blockSizes, result, settingsCopy, pointPerCent); 
            IndexType globalNumMoves = comm->sum(numMoves);
            //if we moved too few or too many points
            if( globalNumMoves < globalN*pointPerCent/settingsCopy.numBlocks*0.1 or
                globalNumMoves > globalN*pointPerCent/settingsCopy.numBlocks*0.9 ){
                pointPerCent += 0.05;
                PRINT0("globally moved vertices " << globalNumMoves << ", increase point percentage");
            }
            settingsCopy.minSamplingNodes = -1;
            settingsCopy.maxKMeansIterations = 10;
            settingsCopy.balanceIterations = 30;

            //result = ITI::KMeans<IndexType,ValueType>::computeRepartition(coordinates, nodeWeights, blockSizes, result, settingsCopy);
        }

        for(int w=0; w<settingsCopy.numNodeWeights; w++){
            imbalances[w] = GraphUtils<IndexType,ValueType>::computeImbalance(result, blockSizes[w].size(), nodeWeights[w], blockSizes[w] );
        }
        maxCurrImbalance = *std::max_element( imbalances.begin(), imbalances.end() );

        if( maxCurrImbalance<maxMinImbalance){
            PRINT0("\tStoring solution with maximum imbalance " << maxCurrImbalance );
            bestResult = result;
            maxMinImbalance = maxCurrImbalance;
        }

        //we are using epsilons even for one weight
        //settingsCopy.epsilon -= imbaDelta;
        for( int e=0; e<settingsCopy.epsilons.size(); e++){
            settingsCopy.epsilons[e] -= imbaDelta;
        }

        std::chrono::duration<double> oneLoopDuration =  std::chrono::steady_clock::now() - oneLoopTime;
        ValueType maxLoopTime = comm->max( oneLoopDuration.count() );
        PRINT0("one rebalance loop in time " << maxLoopTime );
    }//for numTries
    

    std::chrono::duration<double> rebalanceTime =  std::chrono::steady_clock::now() - beforeRebalance;
    metrics.MM["timeKmeansRebalance"] = comm->max( rebalanceTime.count() );

    PRINT0("Returning partition with imbalance " << maxMinImbalance << " in time " << metrics.MM["timeKmeansRebalance"]);
    return bestResult;
    
}//computePartition_targetBalance


template<typename IndexType, typename ValueType>
std::vector<std::vector<std::pair<ValueType,IndexType>>> KMeans<IndexType,ValueType>::fuzzify( 
    const std::vector<DenseVector<ValueType>>& coordinates,
    const std::vector<DenseVector<ValueType>>& nodeWeights,
    const DenseVector<IndexType>& partition,
    const std::vector<ValueType>& centerInfluence, //TODO: use or discard
    const Settings settings,
    const IndexType centersToUse){

    SCAI_REGION("KMeans.fuzzify");
    const IndexType localN = coordinates[0].getLocalValues().size();
    const IndexType dimensions = settings.dimensions;
    const scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistributionPtr()->getCommunicatorPtr();
    assert(partition.getLocalValues().size()==localN);

    //find the centers of the provided partition
    std::vector<IndexType> indices(localN);
    std::iota(indices.begin(), indices.end(), 0);
    const std::vector< std::vector<ValueType> > centers = findCenters( coordinates, partition, settings.numBlocks, indices.begin(), indices.end(), nodeWeights);
    SCAI_ASSERT_EQ_ERROR( centers.size(), dimensions, "Wrong centers vector" );
    assert( centers[0].size()==settings.numBlocks );

    //reverse the vectors; TODO: is this needed?
    const std::vector< point<ValueType> > centersTranspose = vectorTranspose( centers );
    assert( centersTranspose.size()==settings.numBlocks );
    assert( centersTranspose[0].size()==dimensions );

    //convert the local coords to vector<vector>; TODO: maybe we can (or we need to) avoid that
    std::vector<std::vector<ValueType>> convertedCoords(dimensions);
   
    for (IndexType d = 0; d < dimensions; d++) {
        scai::hmemo::ReadAccess<ValueType> rAccess(coordinates[d].getLocalValues());
        assert(rAccess.size() == localN);
        convertedCoords[d] = std::vector<ValueType>(rAccess.get(), rAccess.get()+localN);
        assert(convertedCoords[d].size() == localN);
    }

    const IndexType numCenters = centersTranspose.size();
    //if more centers to use were given, use all
    const IndexType ctu = std::min(centersToUse, numCenters ); 

    //one entry for every point. each entry has size ctu and stores a pair:
    //first is the distance value, second is the center that realizes this distance
    std::vector<std::vector<std::pair<ValueType,IndexType>>> fuzzyClustering( localN );

    for(IndexType i=0; i<localN; i++){
        std::vector<std::pair<ValueType,IndexType>> allDistances( numCenters );
        for(IndexType c=0; c<numCenters; c++ ){
            allDistances[c] = std::pair<ValueType,IndexType>(0.0, c);
            const point<ValueType>& thisCenter = centersTranspose[c];
            for (IndexType d=0; d<dimensions; d++) {
                allDistances[c].first += std::pow(thisCenter[d]-convertedCoords[d][i], 2);
            }
            allDistances[c].first = std::sqrt( allDistances[c].first ); //*centerInfluence[c];
        }
        std::sort( allDistances.begin(), allDistances.end() );
        fuzzyClustering[i] = std::vector<std::pair<ValueType,IndexType>>( allDistances.begin(), allDistances.begin()+ctu);
    }

    return fuzzyClustering;
}//fuzzify


//version with no influence
template<typename IndexType, typename ValueType>
std::vector<std::vector<std::pair<ValueType,IndexType>>> KMeans<IndexType,ValueType>::fuzzify( 
    const std::vector<DenseVector<ValueType>>& coordinates,
    const std::vector<DenseVector<ValueType>>& nodeWeights,
    const DenseVector<IndexType>& partition,
    const Settings settings,
    const IndexType centersToUse){

    //initialize influence with 1
    std::vector<ValueType> influence(settings.numBlocks, 1);    

    return fuzzify( coordinates, nodeWeights, partition, influence, settings, centersToUse);
}


template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType>> KMeans<IndexType,ValueType>::computeMembership(
    const std::vector<std::vector<std::pair<ValueType,IndexType>>>& fuzzyClustering){

    SCAI_REGION("KMeans.computeMembership");
    const IndexType localN = fuzzyClustering.size();
    const IndexType vectorSize = fuzzyClustering[0].size();
    
    std::vector<std::vector<ValueType>> membership(localN, std::vector<ValueType>(vectorSize, 0.0));
    for( IndexType i=0; i<localN; i++ ){
        //IndexType myPart = partition[i];
        std::vector<std::pair<ValueType,IndexType>> myFuzzV = fuzzyClustering[i];
        
        //IndexType closestCenter = myFuzzV[0].second;
        ValueType centerDistSum= 0;
        for(IndexType t=0; t<vectorSize; t++ ){
            centerDistSum += 1/(myFuzzV[t].first*myFuzzV[t].first);
        }
        

        for(IndexType j=0; j<vectorSize; j++ ){
            ValueType distFromThisCenterSq = myFuzzV[j].first*myFuzzV[j].first;
            membership[i][j] = 1/( distFromThisCenterSq *centerDistSum );
        }
    }
    return membership;
}


template<typename IndexType, typename ValueType>
std::vector<ValueType> KMeans<IndexType,ValueType>::computeMembershipOneValue(
    const std::vector<std::vector<std::pair<ValueType,IndexType>>>& fuzzyClustering){

    const std::vector<std::vector<ValueType>> membership = computeMembership( fuzzyClustering );
    const IndexType localN = membership.size();
    const IndexType ctu = membership[0].size();
    
    std::vector<ValueType> result( localN, 0.0);

    for( IndexType i=0; i<localN; i++ ){
        for( IndexType c=0; c<ctu; c++ ){
            result[i] += std::pow( (membership[i][c]-1/ctu),2 );
        }
    }

    return result;
}


template<typename IndexType, typename ValueType>
std::vector<ValueType> KMeans<IndexType,ValueType>::computeMembershipOneValueNormalized(
    const std::vector<std::vector<std::pair<ValueType,IndexType>>>& fuzzyClustering,
    const DenseVector<IndexType>& partition,
    const IndexType numBlocks){

    SCAI_REGION("KMeans.computeMembershipOneValueNormalized");
    const scai::dmemo::CommunicatorPtr comm = partition.getDistributionPtr()->getCommunicatorPtr();
    const IndexType localN = partition.getLocalValues().size();

    std::vector<ValueType> mship = computeMembershipOneValue( fuzzyClustering);
    assert( mship.size()==localN );

    //normalize membership by the max membership per block
    
    std::vector<ValueType> maxMshipPerBlock( numBlocks, std::numeric_limits<ValueType>::lowest() );
    scai::hmemo::ReadAccess<IndexType> rPart(partition.getLocalValues());

    //find local max membership
    for(IndexType i=0; i<localN; i++ ){
        const IndexType myBlock = rPart[i];
        const ValueType myMship = mship[i];
        if( myMship>maxMshipPerBlock[myBlock] ){
            maxMshipPerBlock[myBlock]=myMship;
        }
    }

    //find global max membership
    for (IndexType b=0; b<numBlocks; b++) {
        maxMshipPerBlock[b] = comm->max(maxMshipPerBlock[b]);
    }

    //normalize local membership
    for(IndexType i=0; i<localN; i++ ){
        const IndexType myBlock = rPart[i];
        mship[i] /= maxMshipPerBlock[myBlock];
    }

    return mship;
}


//TODOs: consider the "local imbalance" of each block? If a PE has 20% of a block
//  it should not make many moves/changes; less than a PE that has 60% of the
//  same block

//TODO: it does not consider which is weight is the overbalanced one.
//If a block's max imbalance is high (over the given epsilon) then it does not move points to that block
// but it could move a point that has a high weight for weight 2 and remove another point that has high
// weight 1, if 1 is the problematic weight. But we consider only the max weight without checking which
// realizes the max imbalance

template<typename IndexType, typename ValueType>
IndexType KMeans<IndexType,ValueType>::rebalance(
    const std::vector<DenseVector<ValueType>> &coordinates,
    const std::vector<DenseVector<ValueType>> &nodeWeights,
    const std::vector<std::vector<ValueType>> &targetBlockWeights,
    DenseVector<IndexType>& partition,
    const Settings settings,
    const ValueType pointPerCent){

    SCAI_REGION("KMeans.rebalance");
    const scai::dmemo::CommunicatorPtr comm = coordinates[0].getDistributionPtr()->getCommunicatorPtr();
    const IndexType numWeights = nodeWeights.size();
    const IndexType localN = coordinates[0].getLocalValues().size();
    IndexType numBlocks = settings.numBlocks;
    const IndexType centersToUse = 6; //TODO: turn that to an user parameter?
    assert( targetBlockWeights.size()==numWeights);

    //if we are doing some kind of hierarchical partition, the number of blocks is different in every level
    // and is not always the final number of blocks
    if( settings.initialPartition==ITI::Tool::geoHierKM or settings.initialPartition==ITI::Tool::geoHierRepart ){
        numBlocks = targetBlockWeights[0].size();
    }
    SCAI_ASSERT_EQ_ERROR( targetBlockWeights[0].size(), numBlocks, "Possible reason is that the hierarchical kmeans is called." );

    //get a fuzzy clustering, a vector for each local point with length centersToUse
    Settings settingsCopy = settings;
    settingsCopy.numBlocks = numBlocks; //this is different  in the hierarchical version
    const std::vector<std::vector<std::pair<ValueType,IndexType>>> fuzzyClustering = fuzzify( coordinates, nodeWeights, partition, settingsCopy, centersToUse);
    assert( fuzzyClustering.size()==localN );

    //the size of its fuzziness vector
    const IndexType fuzzSize = fuzzyClustering[0].size();
    assert( fuzzSize==centersToUse or fuzzSize==numBlocks);

    const std::vector<ValueType> mship = computeMembershipOneValueNormalized( fuzzyClustering, partition, numBlocks);
    assert( mship.size()==localN );

    //
    // convert weights to vector<vector>
    //

    std::vector<std::vector<ValueType>> nodeWeightsV( numWeights );
    for(IndexType w=0; w<numWeights; w++){
        scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights[w].getLocalValues());
        nodeWeightsV[w] = std::vector<ValueType>(rWeights.get(), rWeights.get()+localN);
    }

    //the global weight of each block for each weight
    std::vector<std::vector<ValueType>> blockWeights = getGlobalBlockWeight( nodeWeightsV, partition );
    assert( blockWeights.size()==numWeights );
    SCAI_ASSERT_EQ_ERROR( blockWeights[0].size(), numBlocks, "block sizes, wrong number of weights" );

    //calculate the imbalance for every block; hardcode to double
    std::vector<std::vector<double>> imbalancesPerBlock(numWeights, std::vector<double>(numBlocks));
    //only the maximum imbalance
    std::vector<double> maxImbalancePerBlock(numBlocks, std::numeric_limits<double>::lowest() );
    //std::vector<double> maxImbalancePerBlockForWeight(numBlocks, std::numeric_limits<double>::lowest() );

    SCAI_ASSERT_EQ_ERROR( targetBlockWeights.size(), numWeights, "target block sizes, wrong number of weights" );

    for (IndexType w=0; w<numWeights; w++) {
    SCAI_ASSERT_EQ_ERROR( targetBlockWeights[w].size(), numBlocks, "block sizes, wrong number of weights" );
        for (IndexType b=0; b<numBlocks; b++) {
            ValueType optWeight = targetBlockWeights[w][b];
            imbalancesPerBlock[w][b] = (ValueType(blockWeights[w][b] - optWeight)/optWeight);
            if( imbalancesPerBlock[w][b]>maxImbalancePerBlock[b] ) {
                maxImbalancePerBlock[b] = imbalancesPerBlock[w][b];
                //maxImbalancePerBlockForWeight[b] = w;
            }
        }
    }

    //sort blocks based on their maxImbalance
    //blockIndices[0] is the block with the highest imbalance
    std::vector<IndexType> blockIndices(numBlocks);
    std::iota(blockIndices.begin(), blockIndices.end(), 0);
    std::sort(blockIndices.begin(), blockIndices.end(),
        [&maxImbalancePerBlock](int i, int j) {
            return maxImbalancePerBlock[i]>maxImbalancePerBlock[j];
    });

    //PRINT0("most imbalanced block is " << blockIndices[0] << " with weight " <<  maxImbalancePerBlock[blockIndices[0]] );

    scai::hmemo::ReadAccess<IndexType> rPart(partition.getLocalValues());
    assert(rPart.size()==localN);
    std::vector<IndexType> localPart( rPart.get(), rPart.get()+localN );
    rPart.release();

    //
    // sort local indices based on their membership value
    // lower values indicate fuzzier points
    //

    //sort lexicographically: first by the imbalance of the block this point belongs to.
    //  if they are in the same block or the imbalance is the same, sort by membership
    auto lexSort = [&](int i, int j)->bool{
        const IndexType blockI = localPart[i];
        const IndexType blockJ = localPart[j];
        //if in the same block, sort by membership
        if( blockI==blockJ ){
            return mship[i]<mship[j];
        }
        if( maxImbalancePerBlock[blockI]>maxImbalancePerBlock[blockJ]){
            return true;
        }else if(maxImbalancePerBlock[blockI]==maxImbalancePerBlock[blockJ]){
            //if blocks have the same imbalance, sort by membership
            return mship[i]<mship[j];
        }else{
            return false;
        }
    };

    //maxImba^2/mship
    auto squaredImbaSort = [&](int i, int j)->bool{
        const IndexType blockI = localPart[i];
        const IndexType blockJ = localPart[j];
        const ValueType fI = std::pow(maxImbalancePerBlock[blockI], 2)/mship[i];
        const ValueType fJ = std::pow(maxImbalancePerBlock[blockJ], 2)/mship[j];
        return fI>fJ;
    };
    
    std::function<bool(int,int)> sortFunction;
    if(settings.KMBalanceMethod=="reb_lex"){
        sortFunction = lexSort;
    }else{
         sortFunction = squaredImbaSort;
    }

    std::vector<IndexType> indices(localN);
    std::iota(indices.begin(), indices.end(), 0);

    std::sort(indices.begin(), indices.end(), sortFunction );

    const IndexType numPointsToCheck = comm->min(localN)*pointPerCent;
    std::vector<bool> hasMoved(localN, false);

    //here we store the difference to the block weight caused by each move
    std::vector<std::vector<ValueType>> blockWeightDifference(numWeights, std::vector<ValueType>(numBlocks, 0.0));
    
    const IndexType myBatchSize = localN*settings.batchPercent + 1;
    //pick min across all processors;  this is needed so they all do the global sum together
    IndexType batchSize = comm->min(myBatchSize);

    bool meDone = false;
    bool allDone = false;
    IndexType localI = 0;
    IndexType numMoves = 0;
    const IndexType maxNumRestarts = 5;
    IndexType thisRun=0;

    //for all local nodes until all points are checked
    while( not allDone ){
        const IndexType thisInd = indices[localI];
        const IndexType myBlock = localPart[thisInd];

        vector<ValueType> myWeights(numWeights);
        for (IndexType w=0; w<numWeights; w++) {
            myWeights[w] = nodeWeightsV[w][thisInd];
        }

        //the effect that the removal of this point will have to its current block

        //these are this block's (the block this point belongs to) weight and imbalance
        //after the removal of this point
        std::vector<double> thisBlockNewImbalances(numWeights);
        for (IndexType w=0; w<numWeights; w++) {
            ValueType optWeight = targetBlockWeights[w][myBlock];
            thisBlockNewImbalances[w] = imbalancesPerBlock[w][myBlock] - myWeights[w]/optWeight;
        }
        const ValueType thisBlockNewMaxImbalance = *std::max_element( thisBlockNewImbalances.begin(), thisBlockNewImbalances.end());
        SCAI_ASSERT_LE_ERROR( thisBlockNewMaxImbalance, maxImbalancePerBlock[myBlock], "Since we remove, imbalance value should be reduced");


        //possible centers to move are the ones that the point is closer
        //WARNING/TODO: "closer" depends on which distance function we use. Here we use the euclidean distance
        // but we should use the effective distance for better results with kmeans
        //TODO: use the centers based on their membership value by using computeMembership()

        IndexType bestBlock = myBlock;
        double bestBlockMaxNewImbalance = std::numeric_limits<double>::max();
        std::vector<double> bestBlockNewImbalances;

        //for all possible center in the fuzzy vector
        //if this point's block, new max imbalance is negative, do not move it
        for( IndexType c=0; c<fuzzSize and thisBlockNewMaxImbalance>0; c++){
			//these can be done outside the for loop but is more convenient here
            //because we do not need to take care of the localI or meDone variables
            if(hasMoved[thisInd]){//this point has already been moved, do not move it again
                break;
            }
            if( maxImbalancePerBlock[myBlock]< -0.05 ){ //if my block is too light, do not remove
                break;
            }
            //candidate block to change to
            const IndexType candidateBlock = fuzzyClustering[thisInd][c].second;
            SCAI_ASSERT_LT_ERROR( candidateBlock, numBlocks, "Block id too big");
            if( myBlock==candidateBlock){
                continue;
            }
            //if candidate block is "too" heavy do not consider it
            if( maxImbalancePerBlock[candidateBlock]>settings.epsilon ){
                continue;
            }
            assert( settings.epsilons.size()==imbalancesPerBlock.size() );
            for(int e=0; e<settings.epsilons.size(); e++ ){
                if( imbalancesPerBlock[e][candidateBlock]>settings.epsilons[e] )
                    continue;
            }

            //calculate block weight and imbalance of the new candidate block if we add this point

            std::vector<double> newBlockImbalances(numWeights);

            //the old max imbalance for the new block
            double maxOldImbalanceNewBlock = std::numeric_limits<double>::lowest(); //used for checks
            for (IndexType w=0; w<numWeights; w++) {
                ValueType optWeight = targetBlockWeights[w][candidateBlock];
                //will (possibly) add this point to the block, so add its weights
                newBlockImbalances[w] = imbalancesPerBlock[w][candidateBlock] + myWeights[w]/optWeight;

                if(imbalancesPerBlock[w][candidateBlock]>maxOldImbalanceNewBlock){
                    maxOldImbalanceNewBlock= imbalancesPerBlock[w][candidateBlock];
                }
            }
            //TODO: is maxOldImbalanceNewBlock  changed at this point ?
            SCAI_ASSERT_LE_ERROR( std::abs(maxOldImbalanceNewBlock-maxImbalancePerBlock[candidateBlock]), 1e-5, comm->getRank()<< ": for block " << candidateBlock << "; should not agree?" );

            //
            //evaluate if the move was beneficial
            //

            //the max imbalance of the new block is larger than the previous max imbalance of the same block 
            // since we added a point (with positive weight)
            const double maxNewImbalanceNewBlock = *std::max_element(newBlockImbalances.begin(), newBlockImbalances.end());
            SCAI_ASSERT_GE_ERROR( maxNewImbalanceNewBlock,  maxImbalancePerBlock[candidateBlock], "??") ;

            //if this candidate block offers a better imbalance
            if( bestBlockMaxNewImbalance>maxNewImbalanceNewBlock ){
                //from all the moves that improve the imbalance, keep the one that improves it the most
                //check also if the change in this possible block is less
                bestBlockMaxNewImbalance = maxNewImbalanceNewBlock;
                bestBlock = candidateBlock;
                bestBlockNewImbalances = newBlockImbalances;
            }
        }

        //here, we have picked a candidate block that worsens the imbalance the least
        //also check if moving to the best block is beneficial in total
        //if this block is not beneficial, then no other is

        if( bestBlock!=myBlock ){
            //if the new candidate block's imbalance is increased more than the 
            //block we removed the point from, then do not perform the move.
            //TODO: ^^ not completely true: the increase maybe be more but 
            //we just do not want to increase the global max imbalance
            if( thisBlockNewMaxImbalance < bestBlockMaxNewImbalance ){
                bestBlock=myBlock;
            }
        }   
        
        //actually move the point from myBlock to bestBlock
        if( bestBlock!=myBlock){
            //the new max imbalances
            maxImbalancePerBlock[bestBlock] = *std::max_element(bestBlockNewImbalances.begin(), bestBlockNewImbalances.end());
            maxImbalancePerBlock[myBlock] = thisBlockNewMaxImbalance;

            localPart[thisInd] = bestBlock; 

            //update values of the block weights and imbalances locally
            for (IndexType w=0; w<numWeights; w++) {
                blockWeightDifference[w][myBlock] -= myWeights[w];
                blockWeightDifference[w][bestBlock] += myWeights[w];
                imbalancesPerBlock[w][myBlock] = thisBlockNewImbalances[w];
                imbalancesPerBlock[w][bestBlock] = bestBlockNewImbalances[w];
            }
            //uncomment for debugging
            //PRINT0("moving point " << thisInd << " from block " << myBlock << " to " << bestBlock << " and imbalances are: old block's= " << thisBlockNewMaxImbalance << ", new block's= " << maxImbalancePerBlock[bestBlock]);
            SCAI_ASSERT_EQ_ERROR( *std::max_element(bestBlockNewImbalances.begin(), bestBlockNewImbalances.end()) , (double) maxImbalancePerBlock[bestBlock], comm->getRank() << ": wrong new max imbalance for block " << bestBlock );

            //TODO: block imbalance is updated after every move. 
            //This also affects the order of points that need to be resorted/
            //Maybe use some priority queue?
            numMoves++;
            hasMoved[thisInd] = true;
            //TODO: consider aborting if no PE has moved vertices for some rounds
        }
        //else no improvement was achieved

        //global sum needed
        if( (localI+1)%batchSize==0 or meDone ){
            //reset local block max weight imbalances
            std::fill( maxImbalancePerBlock.begin(), maxImbalancePerBlock.end(), std::numeric_limits<double>::lowest() );  

            for (IndexType w=0; w<numWeights; w++) {
                //sum all the differences for all blocks among PEs
                comm->sumImpl(blockWeightDifference[w].data(), blockWeightDifference[w].data(), numBlocks, scai::common::TypeTraits<ValueType>::stype );
                std::transform( blockWeights[w].begin(), blockWeights[w].end(), blockWeightDifference[w].begin(), blockWeights[w].begin(), std::plus<ValueType>() );
                SCAI_ASSERT_EQ_ERROR( targetBlockWeights[w].size(), numBlocks, "block sizes, wrong number of blocks" );
                SCAI_ASSERT_EQ_ERROR( blockWeights[w].size(), numBlocks, "block sizes, wrong number of blocks" );
                //recalculate imbalances after the new global blocks weights are summed
                for (IndexType b=0; b<numBlocks; b++) {
                    ValueType optWeight = targetBlockWeights[w][b];
                    imbalancesPerBlock[w][b] = (ValueType(blockWeights[w][b] - optWeight)/optWeight);
                    if( imbalancesPerBlock[w][b]>maxImbalancePerBlock[b] ) {
                        maxImbalancePerBlock[b] = imbalancesPerBlock[w][b];
                    }
                }

                //reset local block weight differences
                std::fill( blockWeightDifference[w].begin(), blockWeightDifference[w].end(), 0.0);
            }

            //TODO: check if resorting local points based on new global weights
            //and restarting would benefit

            if(thisRun<maxNumRestarts){
                std::sort(indices.begin(), indices.end(), sortFunction );
                //restart
                localI=-1;
                thisRun++;
            }else{
                //increase the batch size
                batchSize = std::min( (IndexType) (batchSize*1.05),  std::max(localN/1000+1, IndexType(1000)) );
                batchSize = comm->min(batchSize);
                //PRINT0( comm->getRank() << ": " << batchSize );
            }
        }

        //exit condition
        if( localI<numPointsToCheck-1 ){
            localI++;
        }else{
            meDone=true;
        }

        try{
            allDone = comm->all(meDone);
        }catch(scai::dmemo::MPIException& e){
            //e.addCallStack( std::cout );
            std::cout << e.what() << std::endl <<
                "Probably some PE is in another global operation...." << std::endl;
        }

    }//while

    assert( *std::min_element( localPart.begin(), localPart.end() ) >=0 );
    assert( *std::max_element( localPart.begin(), localPart.end() ) < numBlocks );

    // copy to DenseVector; TODO: a better way to do it?
    {
        scai::hmemo::WriteAccess<IndexType> wPart(partition.getLocalValues());
        for(IndexType i=0; i<localN; i++){
            wPart[i] = localPart[i];
        }
    }

    /*//uncomment for debugging
    for (IndexType i = 0; i < numWeights; i++) {
        ValueType imba = ITI::GraphUtils<IndexType, ValueType>::computeImbalance(partition, numBlocks, nodeWeights[i], targetBlockWeights[i]);
        PRINT0("weight " << i<< ", imbalance: " << imba);
    }
    */

    return numMoves;
}//rebalance


template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType>> KMeans<IndexType,ValueType>::getGlobalBlockWeight(
    const std::vector<DenseVector<ValueType>> &nodeWeights,
    const DenseVector<IndexType>& partition){

    const IndexType numWeights = nodeWeights.size();
    const IndexType localN = nodeWeights[0].size();

    //
    // convert to vector<vector> and then use overloaded function
    //

    std::vector<std::vector<ValueType>> nodeWeightsV( numWeights );

    for(IndexType w=0; w<numWeights; w++){
        scai::hmemo::ReadAccess<ValueType> rWeights(nodeWeights[w].getLocalValues());
        nodeWeightsV[w] = std::vector<ValueType>(rWeights.get(), rWeights.get()+localN);
    }

    return getGlobalBlockWeight( nodeWeightsV, partition );
}

template<typename IndexType, typename ValueType>
std::vector<std::vector<ValueType>> KMeans<IndexType,ValueType>::getGlobalBlockWeight(
    const std::vector<std::vector<ValueType>> &nodeWeights,
    const DenseVector<IndexType>& partition){

    SCAI_REGION("KMeans.getGlobalBlockWeight");
    const IndexType numWeights = nodeWeights.size();
    const IndexType localN = nodeWeights[0].size();
    assert( partition.getLocalValues().size()==localN);

    const IndexType numBlocks = partition.max()+1;

    scai::hmemo::ReadAccess<IndexType> rPart(partition.getLocalValues());

    //the global weight of each block for each weight
    std::vector<std::vector<ValueType>> blockWeights(numWeights, std::vector<ValueType>( numBlocks, 0.0));

    //calculate the local weight first
    for( IndexType i=0; i<localN; i++){
        const IndexType myBlock = rPart[i];
        for(IndexType w=0; w<numWeights; w++){
            blockWeights[w][myBlock] += nodeWeights[w][i];
        }
    }

    //take the global sum

    const scai::dmemo::CommunicatorPtr comm = partition.getDistributionPtr()->getCommunicatorPtr();

    for (IndexType w=0; w<numWeights; w++){
        comm->sumImpl(blockWeights[w].data(), blockWeights[w].data(), numBlocks, scai::common::TypeTraits<ValueType>::stype);     
    }

    return blockWeights;
}


/* Get local minimum and maximum coordinates
 * TODO: This isn't used any more! Remove?
 */
template<typename IndexType, typename ValueType>
std::pair<std::vector<ValueType>,std::vector<ValueType>> KMeans<IndexType,ValueType>::getGlobalMinMaxCoords(const std::vector<DenseVector<ValueType>> &coordinates) {
    const int dim = coordinates.size();
    std::vector<ValueType> minCoords(dim);
    std::vector<ValueType> maxCoords(dim);
    for (int d = 0; d < dim; d++) {
        minCoords[d] = coordinates[d].min();
        maxCoords[d] = coordinates[d].max();
        SCAI_ASSERT_NE_ERROR(minCoords[d], maxCoords[d], "min=max for dimension "<< d << ", this will cause problems to the hilbert index. local= " << coordinates[0].getLocalValues().size());
    }
    return {minCoords, maxCoords};
}

//
// instantiations
//

template class KMeans<IndexType, double>;
template class KMeans<IndexType, float>;

} /* namespace ITI */
