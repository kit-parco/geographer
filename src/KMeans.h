/*
 * KMeans.h
 *
 *  Created on: 19.07.2017
 *      Author: Moritz von Looz
 */

#pragma once

#include <vector>
#include <numeric>
#include <chrono>
#include <utility>

#include <scai/lama/DenseVector.hpp>
#include <scai/tracing.hpp>

#include "Settings.h"
#include "Metrics.h"
#include "GraphUtils.h"
#include "HilbertCurve.h"
#include "AuxiliaryFunctions.h"
#include "CommTree.h"
#include "quadtree/SpatialCell.h"

namespace ITI {

using scai::lama::DenseVector;

/** K-means related algorithms for partitioning a point set
*/

template<typename IndexType, typename ValueType>
class KMeans {
public:

//to make it more readable
//using point = typename std::vector<ValueType>;

/**
 * @brief Partition a point set using balanced k-means.
 *
 * This is the main function, others with the same name are wrappers for this one.
 *
 * @param[in] coordinates first level index specifies dimension, second level index the point id:
 coordinates[i][p] is the i-th coordinate of point p
 * @param[in] nodeWeights The weights of the points. Each point can have multiple weights but all points
 must have the same number of weights.
 * @param[in] blockSizes target block sizes, not maximum sizes
 * @param[in] prevPartition This is used for the hierarchical version, it is the partition from the previous hierarchy level.
 * If settings.repartition=true then this has a different meaning: is the partition to be refined.
 * @param[in] centers initial k-means centers
 * @param[in] settings Settings struct
 *
 * @return Distributed DenseVector of length n, partition[i] contains the block ID of node i
 */

//core implementation
//template<typename IndexType, typename ValueType>
static DenseVector<IndexType> computePartition(
    const std::vector<DenseVector<ValueType>> &coordinates, \
    const std::vector<DenseVector<ValueType>> &nodeWeights, \
    const std::vector<std::vector<ValueType>> &blockSizes, \
    const DenseVector<IndexType>& prevPartition,\
    std::vector<std::vector< std::vector<ValueType> >> centers, \
    const Settings settings, \
    Metrics<ValueType>& metrics);

/** Version that also returns the influences per center and the centers
@param[in/out] centers The provided centers and their final position
@param[out] influence The influence per center calculated by the algorithm
*/

static DenseVector<IndexType> computePartition(
    const std::vector<DenseVector<ValueType>> &coordinates, \
    const std::vector<DenseVector<ValueType>> &nodeWeights, \
    const std::vector<std::vector<ValueType>> &blockSizes, \
    const DenseVector<IndexType>& prevPartition,\
    std::vector<std::vector< std::vector<ValueType>>> &centers, \
    std::vector<std::vector<ValueType>> &influence, \
    const Settings settings, \
    Metrics<ValueType>& metrics);


/** @brief Minimal wrapper with only the coordinates. Unit weights are assumed and uniform block sizes.
*/
//template<typename IndexType, typename ValueType>
static DenseVector<IndexType> computePartition(
    const std::vector<DenseVector<ValueType>> &coordinates,
    const Settings settings);

/**
 * @brief Partition a point set using balanced k-means
 *
 * Wrapper without initial centers. Calls computePartition with centers derived from a Hilbert Curve.
 *
 * @param[in] coordinates first level index specifies dimension, second level index the point id:
 coordinates[i][p] is the i-th coordinate of point p
 * @param[in] nodeWeights The weights of the points. Each point can have multiple weights but all points
 must have the same number of weights.
 * @param[in] blockSizes Target, i.e., wanted, block sizes, not maximum sizes.
 * @param[in] settings Settings struct
 * @param[in] metrics Metrics struct
 *
 * @return A partition of the points into \p settings.numBlocks number of blocks.
 */

//wrapper 1- no centers
//template<typename IndexType, typename ValueType>
static DenseVector<IndexType> computePartition(
    const std::vector<DenseVector<ValueType>> &coordinates,
    const std::vector<DenseVector<ValueType>> &nodeWeights,
    const std::vector<std::vector<ValueType>> &blockSizes,
    const Settings settings,
    Metrics<ValueType>& metrics);

/**
 * Given a tree of the processors graph, computes a partition into a hierarchical fashion.
 *
 * @param[in] coordinates First level index specifies dimension, second level index the point id
 * @param[in] nodeWeights The weights of the points. Each point can have multiple weights but all
 the same number of weights.
 * @param[in] commTree The tree describing the processor network. \sa CommTree
 **/

//template<typename IndexType, typename ValueType>
static DenseVector<IndexType> computeHierarchicalPartition(
    std::vector<DenseVector<ValueType>> &coordinates,
    std::vector<DenseVector<ValueType>> &nodeWeights,
    const CommTree<IndexType,ValueType> &commTree,
    Settings settings,
    Metrics<ValueType>& metrics);

/** Calls computeHierarchicalPartition() with an additional step of repartitioning in order to
provide a better global cut.

Parameters and return are same as in computeHierarchicalPartition()
*/
//template<typename IndexType, typename ValueType>
static DenseVector<IndexType> computeHierPlusRepart(
    std::vector<DenseVector<ValueType>>& coordinates,
    std::vector<DenseVector<ValueType>>& nodeWeights,
    const CommTree<IndexType,ValueType>& commTree,
    Settings settings,
    Metrics<ValueType>& metrics);

/**
 * @brief Repartition a point set using balanced k-means.
 *
 * @param[in] coordinates first level index specifies dimension, second level index the point id
 * @param[in] nodeWeights The weights of the points. Each point can have multiple weights but all points
 must have the same number of weights.
 * @param[in] blockSizes target block sizes, not maximum sizes. blockSizes.size()== number of weights
 * @param[in] previous Previous partition
 * @param[in] settings Settings struct
 *
 * @return partition
 */
//template<typename IndexType, typename ValueType>
static DenseVector<IndexType> computeRepartition(
    const std::vector<DenseVector<ValueType>>& coordinates,
    const std::vector<DenseVector<ValueType>>& nodeWeights,
    const std::vector<std::vector<ValueType>>& blockSizes,
    const DenseVector<IndexType> &previous,
    const Settings settings);

//template<typename IndexType, typename ValueType>
static DenseVector<IndexType> computeRepartition(
    const std::vector<DenseVector<ValueType>>& coordinates,
    const std::vector<DenseVector<ValueType>>& nodeWeights,
    const Settings settings,
    Metrics<ValueType>& metrics);

/** Version of k-means that does multiple runs to achieve balance.
*/
static DenseVector<IndexType> computePartition_targetBalance(
    const std::vector<DenseVector<ValueType>> &coordinates,
    const std::vector<DenseVector<ValueType>> &nodeWeights,
    const std::vector<std::vector<ValueType>> &blockSizes,
    DenseVector<IndexType> &previous,
    const Settings settings,
    Metrics<ValueType>& metrics);

/**
    Returns one entry for every local point. each entry has size centerToUse and stores a pair:
    first is the distance value, second is the center that realizes this distance.

    @param[in] centerInfluence The weight/influence of each center that is used to calculate
    the effective distance as: effDist = dist(p,c)*influence(c). centerInfluence.size()= the number
    of node weights and centerInfluence[i]=number of blocks

    @return One entry for every local point. each entry has size centerToUse and stores a pair:
    first is the distance value, second is the center that realizes this distance
*/

static std::vector<std::vector<std::pair<ValueType,IndexType>>> fuzzify( 
    const std::vector<DenseVector<ValueType>>& coordinates,
    const std::vector<DenseVector<ValueType>>& nodeWeights,
    const DenseVector<IndexType>& partition,
    const std::vector<ValueType>& centerInfluence,
    const Settings settings,
    const IndexType centersToUse=6);

static std::vector<std::vector<std::pair<ValueType,IndexType>>> fuzzify( 
    const std::vector<DenseVector<ValueType>>& coordinates,
    const std::vector<DenseVector<ValueType>>& nodeWeights,
    const DenseVector<IndexType>& partition,
    const Settings settings,
    const IndexType centersToUse=6);

/**
    Compute the membership values of every local point provided a fuzzy clustering vector.
    Membership is calculated using the function mentioned in https://en.wikipedia.org/wiki/Fuzzy_clustering
    using the euclidean distance.
    \sa fuzzify

    @return A vector for every point of size equal to the number of centers used to
    obtain the fuzzy clustering. return[i][j] is the membership value of point \p i
    to cluster \p j.
*/

static std::vector<std::vector<ValueType>> computeMembership(
    const std::vector<std::vector<std::pair<ValueType,IndexType>>>& fuzzyClustering);

/** 
    Compute the membership values of every local point provided a fuzzy clustering vector.
    Compared to the other version the function, it returns only one value per point.
    First we calculate the membership vector of every point using computeMembership and 
    the value for each point is the sum of (x_i-1/ctu)^2, where x_i us the membership
    value for center i and ctu is the size of the membership vector.
    Higher values mean that the points is close to one center, lower values indicate
    fuzzier points, points that are between centers.
*/
static std::vector<ValueType> computeMembershipOneValue(
    const std::vector<std::vector<std::pair<ValueType,IndexType>>>& fuzzyClustering);

/** #brief Normalize membership by the max membership per block.
*/
static std::vector<ValueType> computeMembershipOneValueNormalized(
    const std::vector<std::vector<std::pair<ValueType,IndexType>>>& fuzzyClustering,
    const DenseVector<IndexType>& partition,
    const IndexType numBlocks);


/** Given a partitioned input, move points to other blocks to improve imbalance.

@param[in/out] partition The partition of the points to be refined
*/
static IndexType rebalance(
    const std::vector<DenseVector<ValueType>> &coordinates,
    const std::vector<DenseVector<ValueType>> &nodeWeights,
    const std::vector<std::vector<ValueType>> &targetBlockWeights,
    DenseVector<IndexType>& partition,
    const Settings settings,
    const ValueType pointPerCent=0.5);


/** @brief Version for hierarchical version. The returned centers now are a vector of vectors,
	a vector of centers for every block/center in the previous hierarchy level.
	For every known block (given through \p partition), a number of centers is calculated independently
	of the rest of the blocks. How many centers we find for each block is determined by \p hierLevel.

	@param [in] hierLevel The previous hierarch level.
	@param[in] partition The block id of every point in the previous hierarchy.
	partition[i]=b means that point i was in block b in the previous hierarchy level.
	@return A vector of vectors of points.
*/
//template<typename IndexType, typename ValueType>
static std::vector<std::vector< std::vector<ValueType> >> findInitialCentersSFC(
     const std::vector<DenseVector<ValueType> >& coordinates,
     const std::vector<ValueType> &minCoords,
     const std::vector<ValueType> &maxCoords,
     const scai::lama::DenseVector<IndexType> &partition,
     const std::vector<cNode<IndexType,ValueType>> hierLevel,
     Settings settings);

/**
 * Find initial centers for k-means by sorting the local points along a space-filling curve.
 * Assumes that points are already divided globally according to their SFC indices, but the local order was changed to have increasing global node IDs,
 * as required by the GeneralDistribution constructor.
 *
 * @param[in] coordinates
 * @param[in] minCoords Minimum coordinate in each dimension, lower left point of bounding box (if in 2D)
 * @param[in] maxCoords Maximum coordinate in each dimension, upper right point of bounding box (if in 2D)
 * @param[in] settings
 *
 * @return coordinates of centers
 */
//template<typename IndexType, typename ValueType>
static std::vector<std::vector<ValueType>>  findInitialCentersSFC(
     const std::vector<DenseVector<ValueType> >& coordinates,
     const std::vector<ValueType> &minCoords,
     const std::vector<ValueType> &maxCoords,
     Settings settings);

/**
 * @brief Compute initial centers from space-filling curve without considering point positions.

 * @param[in] minCoords Minimum coordinate in each dimension
 * @param[in] maxCoords Maximum coordinate in each dimension
 * @param[in] settings
 *
 * @return coordinates of centers
 */
//template<typename IndexType, typename ValueType>
static std::vector<std::vector<ValueType> > findInitialCentersFromSFCOnly(
    const std::vector<ValueType> &minCoords,
    const std::vector<ValueType> &maxCoords,
    Settings settings);

/**
 * Compute centers based on the assumption that the partition is equal to the distribution.
 * Each process then picks the average of mass of its local points.
 *
 * @param[in] coordinates First level index specifies dimension, second level index the point id:
 coordinates[i][p] is the i-th coordinate of point p
 * @param[in] nodeWeights The weights of the points. Each point can have multiple weights but all points
 must have the same number of weights.
 *
 * @return coordinates of centers
 */

//TODO: how to treat multiple weights
static std::vector<std::vector<ValueType>> findLocalCenters(
    const std::vector<DenseVector<ValueType> >& coordinates,
    const DenseVector<ValueType> &nodeWeights);

/**
 * Find centers of current partition.
 * To enable random initialization of k-means with a subset of nodes, this function accepts iterators for the first and last local index that should be considered.
 *
 * @param[in] coordinates input points
 * @param[in] partition an already know partition of the points
 * @param[in] k number of blocks
 * @param[in] firstIndex begin of local node indices
 * @param[in] lastIndex end of local node indices
 * @param[in] nodeWeights node weights
 *
 * @return coordinates of centers
 */

//TODO: how to treat multiple weights
template<typename Iterator>
static std::vector< std::vector<ValueType> > findCenters(
    const std::vector<DenseVector<ValueType>>& coordinates,
    const DenseVector<IndexType>& partition,
    const IndexType k,
    const Iterator firstIndex,
    const Iterator lastIndex,
    const std::vector<DenseVector<ValueType>>& nodeWeights);


/** @brief Get minimum and maximum of the global coordinates.
 */
//template<typename ValueType>
static std::pair<std::vector<ValueType>, std::vector<ValueType> > getGlobalMinMaxCoords(const std::vector<DenseVector<ValueType>> &coordinates);


/** @brief Calculate the global weight of all blocks
*/
static std::vector<std::vector<ValueType>> getGlobalBlockWeight(
    const std::vector<DenseVector<ValueType>> &nodeWeights,
    const DenseVector<IndexType>& partition);

static std::vector<std::vector<ValueType>> getGlobalBlockWeight(
    const std::vector<std::vector<ValueType>> &nodeWeights,
    const DenseVector<IndexType>& partition);


/**
 * Computes the weighted distance between a vertex and a cluster, given the geometric distance and the weights and influence values.
 *
 * @param[in] distance
 * @param[in] nodeWeights
 * @param[in] influence
 * @param[in] vertex
 * @param[in] cluster
 */
//template<typename IndexType, typename ValueType>
static ValueType computeEffectiveDistance(
    const ValueType distance,
    const std::vector<DenseVector<ValueType>> &nodeWeights,
    const std::vector<std::vector<ValueType>> &influence,
    const IndexType vertex,
    const IndexType cluster);

/**
 * Assign points to block with smallest effective distance, adjusted for influence values.
 * Repeatedly adjusts influence values to adhere to the balance constraint given by \p settings.epsilon
 * To enable random initialization with a subset of the points, this function accepts iterators for the first and last local index that should be considered.
 *
 * The parameters \p upperBoundOwnCenter,\p lowerBoundNextCenter and \p influence are updated during point assignment.
 *
 * In contrast to the paper, the influence value is multiplied with the plain distance to compute the effective distance.
 * Thus, blocks with higher influence values have larger distances to all points and will receive less points in the next iteration.
 * Blocks with too few points get a lower influence value to attract more points in the next iteration.
 *
 * The returned vector has always as many entries as local points, even if only some of them are non-zero.
 *
 * @param[in] coordinates input points
 * @param[in] centers block centers
 * @param[in] firstIndex begin of local node indices
 * @param[in] lastIndex end local node indices
 * @param[in] nodeWeights node weights
 * @param[in] previousAssignment previous assignment of points
 * @param[in] oldBlock The block from the previous hierarchy that every point
 belongs to. In case of the non-hierarchical version, this is 0 for all points. This is different from previousAssignment
 because it does not change in-between k-means iteration while previousAssignement changes until it converges and the
 algorithm stops.
 * @param[in] blockSizesPerCent A value indicating a percentage per block of
 the points weight. If, W is the sum of weights of all the points, then
 for block i, its weight (sum of the weight of points in the block) must
 be at most (or near) blockSizesPerCent[i]*W.
 * @param[in] boundingBox min and max coordinates of local points, used to compute distance bounds
 * @param[in,out] upperBoundOwnCenter for each point, an upper bound of the effective distance to its own center
 * @param[in,out] lowerBoundNextCenter for each point, a lower bound of the effective distance to the next-closest center
 * @param[in,out] influence a multiplier for each block and for each balance constrain, to compute the effective distance
 * @param[in] settings
 *
 * @return assignment of points to blocks
 */
//template<typename IndexType, typename ValueType, typename Iterator>
template< typename Iterator>
static DenseVector<IndexType> assignBlocks(
    const std::vector<std::vector<ValueType>> &coordinates,
    const std::vector< std::vector<ValueType> >& centers,
    const std::vector<IndexType>& blockSizesPrefixSum,
    const Iterator firstIndex,
    const Iterator lastIndex,
    const std::vector<std::vector<ValueType>> &nodeWeights,
    const std::vector<std::vector<ValueType>> &normalizedNodeWeights,
    const DenseVector<IndexType> &previousAssignment,
    const DenseVector<IndexType> &oldBlocks,
    const std::vector<std::vector<ValueType>> &targetBlockWeights,
    const SpatialCell<ValueType> &boundingBox,
    std::vector<ValueType> &upperBoundOwnCenter,
    std::vector<ValueType> &lowerBoundNextCenter,
    std::vector<std::vector<ValueType>> &influence,
    std::vector<ValueType> &imbalance,
    Settings settings,
    Metrics<ValueType>& metrics);


/** Reverse the order of the vectors: given a 2D vector of size
dimension*numPoints, reverse it and return a vector of points
of size numPoints; in other words, the returned vector has size
numPoints*dimensions. In general, if the given 2D vector has size
A*B, the returned vector has size B*A.
*/
//template<typename IndexType, typename ValueType>
static std::vector<std::vector<ValueType>> vectorTranspose( const std::vector<std::vector<ValueType>>& points);

}; /* class KMeans */


} /* namespace ITI */
