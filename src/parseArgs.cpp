#include <scai/lama.hpp>
#include <scai/dmemo/Communicator.hpp>

#include "parseArgs.h"
#include "Settings.h"

using namespace cxxopts;

namespace ITI {

Options populateOptions() {
    cxxopts::Options options("Geographer", "Parallel geometric graph partitioner for load balancing");

    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    Settings settings;

    options.add_options()
    ("help", "display options")
    ("version", "show version")
    //main arguments for daily use
    ("graphFile", "read graph from file", value<std::string>())
    ("coordFile", "coordinate file. If none given, assume that coordinates for graph arg are in file arg.xyz", value<std::string>())
    ("dimensions", "Number of dimensions of input graph", value<IndexType>()->default_value(std::to_string(settings.dimensions)))
    ("numBlocks", "Number of blocks, default is number of processes", value<IndexType>())
    ("epsilon", "Maximum imbalance. Each block has at most 1+epsilon as many nodes as the average.", value<double>()->default_value(std::to_string(settings.epsilon)))
    // other input specification
    ("fileFormat", "The format of the file to read: 0 is for AUTO format, 1 for METIS, 2 for ADCRIC, 3 for OCEAN, 4 for MatrixMarket format. See Readme.md for more details.", value<ITI::Format>())
    ("coordFormat", "format of coordinate file: AUTO = 0, METIS = 1, ADCIRC = 2, OCEAN = 3, MATRIXMARKET = 4 ", value<ITI::Format>())
    ("numNodeWeights", "Number of node weights to use. If the input graph contains more node weights, only the first ones are used.", value<IndexType>())
    ("seed", "random seed, default is current time", value<double>()->default_value(std::to_string(time(NULL))))
    //mapping
    ("PEgraphFile", "read communication graph from file", value<std::string>())
    ("blockSizesFile", " file to read the block sizes for every block", value<std::string>() )
    //repartitioning
    ("previousPartition", "file of previous partition, used for repartitioning", value<std::string>())
    //multi-level and local refinement
    ("initialPartition", "Choose initial partitioning method between space-filling curves ('SFC' or 0), pixel grid coarsening ('Pixel' or 1), spectral partition ('Spectral' or 2), k-means ('K-Means' or 3) and multisection ('MultiSection' or 4). SFC, Spectral and K-Means are most stable.", value<Tool>())
    ("noRefinement", "skip local refinement steps")
    ("multiLevelRounds", "Tuning Parameter: How many multi-level rounds with coarsening to perform", value<IndexType>()->default_value(std::to_string(settings.multiLevelRounds)))
    ("minBorderNodes", "Tuning parameter: Minimum number of border nodes used in each refinement step", value<IndexType>())
    ("stopAfterNoGainRounds", "Tuning parameter: Number of rounds without gain after which to abort localFM. 0 means no stopping.", value<IndexType>())
    ("minGainForNextGlobalRound", "Tuning parameter: Minimum Gain above which the next global FM round is started", value<IndexType>())
    ("gainOverBalance", "Tuning parameter: In local FM step, choose queue with best gain over queue with best balance", value<bool>())
    ("useDiffusionTieBreaking", "Tuning Parameter: Use diffusion to break ties in Fiduccia-Mattheyes algorithm", value<bool>())
    ("useGeometricTieBreaking", "Tuning Parameter: Use distances to block center for tie breaking", value<bool>())
    ("skipNoGainColors", "Tuning Parameter: Skip Colors that didn't result in a gain in the last global round", value<bool>())
    //multisection
    ("bisect", "Used for the multisection method. If set to true the algorithm perfoms bisections (not multisection) until the desired number of parts is reached", value<bool>())
    ("cutsPerDim", "If MultiSection is chosen, then provide d values that define the number of cuts per dimension.", value<std::string>())
    ("pixeledSideLen", "The resolution for the pixeled partition or the spectral", value<IndexType>())
    // K-Means
    ("minSamplingNodes", "Tuning parameter for K-Means", value<IndexType>())
    ("influenceExponent", "Tuning parameter for K-Means, default is ", value<ValueType>()->default_value(std::to_string(settings.influenceExponent)))
    ("influenceChangeCap", "Tuning parameter for K-Means", value<ValueType>())
    ("balanceIterations", "Tuning parameter for K-Means", value<IndexType>())
    ("maxKMeansIterations", "Tuning parameter for K-Means", value<IndexType>())
    ("tightenBounds", "Tuning parameter for K-Means")
    ("erodeInfluence", "Tuning parameter for K-Means, in case of large deltas and imbalances.")
    // using '/' to seperate the lines breaks the output message
    ("hierLevels", "The number of blocks per level. Total number of PEs (=number of leaves) is the product for all hierLevels[i] and there are hierLevels.size() hierarchy levels. Example: --hierLevels 3 4 10, there are 3 levels. In the first one, each node has 3 children, in the next one each node has 4 and in the last, each node has 10. In total 3*4*10= 120 leaves/PEs", value<std::string>())
    //output
    ("outFile", "write result partition into file", value<std::string>())
    //debug
    ("writeDebugCoordinates", "Write Coordinates of nodes in each block", value<bool>())
    ("verbose", "Increase output.")
    ("storeInfo", "Store timing and other metrics in file.")
    ("callExit", "Call std::exit after finishing partitioning, useful in case of lingering MPI data structures.")
    // evaluation
    ("repeatTimes", "How many times we repeat the partitioning process.", value<IndexType>())
    ("noComputeDiameter", "Compute diameter of resulting block files.")
    ("maxDiameterRounds", "abort diameter algorithm after that many BFS rounds", value<IndexType>())
    ("metricsDetail", "no: no metrics, easy:cut, imbalance, communication volume and diameter if possible, all: easy + SpMV time and communication time in SpMV", value<std::string>())
    //used for the competitors main
    // ("outDir", "write result partition into file", value<std::string>())
    //mesh generation
    ("generate", "generate uniform mesh as input graph")
    ("numX", "Number of points in x dimension of generated graph", value<IndexType>())
    ("numY", "Number of points in y dimension of generated graph", value<IndexType>())
    ("numZ", "Number of points in z dimension of generated graph", value<IndexType>())
    // exotic test cases
    ("quadTreeFile", "read QuadTree from file", value<std::string>())
    ("useDiffusionCoordinates", "Use coordinates based from diffusive systems instead of loading from file", value<bool>())
    ;

    return options;
}

Settings interpretSettings(cxxopts::ParseResult vm) {

    Settings settings;
    scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
    srand(vm["seed"].as<double>());

    if (vm.count("version")) {
        std::cout << "Git commit " << version << std::endl;
        settings.isValid = false;
        return settings;
    }

    if (vm.count("generate") + vm.count("graphFile") + vm.count("quadTreeFile") != 1) {
        std::cout << "Call with --graphFile <input>. Use --help for more parameters." << std::endl;
        settings.isValid = false;
        //return 126;
    }

    if (vm.count("generate") && (vm["dimensions"].as<IndexType>() != 3)) {
        std::cout << "Mesh generation currently only supported for three dimensions" << std::endl;
        settings.isValid = false;
        //return 126;
    }

    if (vm.count("coordFile") && vm.count("useDiffusionCoords")) {
        std::cout << "Cannot both load coordinates from file with --coordFile or generate them with --useDiffusionCoords." << std::endl;
        settings.isValid = false;
        //return 126;
    }

    if (vm.count("fileFormat") && vm["fileFormat"].as<ITI::Format>() == ITI::Format::TEEC) {
        if (!vm.count("numX")) {
            std::cout << "TEEC file format does not specify graph size, please set with --numX" << std::endl;
            settings.isValid = false;
            //return 126;
        }
    }

    // check if coordFormat is provided
    // if no coordFormat was given but was given a fileFormat assume they are the same
    if( !vm.count("coordFormat") and vm.count("fileFormat") ) {
        settings.coordFormat = settings.fileFormat;
    }

    if( settings.storeInfo && settings.outFile=="-" ) {
        PRINT0("Option to store information used but no output file given to write to. Specify an output file using the option --outFile. Aborting.");
        settings.isValid = false;
        //return 126;
    }

    if (!vm.count("influenceExponent")) {
        settings.influenceExponent = 1.0/settings.dimensions;
    }

    if( vm.count("metricsDetail") ) {
        if( not (settings.metricsDetail=="no" or settings.metricsDetail=="easy" or settings.metricsDetail=="all") ) {
            if(comm->getRank() ==0 ) {
                std::cout<<"WARNING: wrong value for parameter metricsDetail= " << settings.metricsDetail << ". Setting to all" <<std::endl;
                settings.metricsDetail="all";
            }
        }
    }

    if( vm.count("noComputeDiameter") ) {
        settings.computeDiameter = false;
    } else {
        settings.computeDiameter = true;
    }

    using std::vector;
    settings.verbose = vm.count("verbose");
    settings.storeInfo = vm.count("storeInfo");
    settings.erodeInfluence = vm.count("erodeInfluence");
    settings.tightenBounds = vm.count("tightenBounds");
    settings.noRefinement = vm.count("noRefinement");
    settings.useDiffusionCoordinates = vm.count("useDiffusionCoordinates");
    settings.gainOverBalance = vm.count("gainOverBalance");
    settings.useDiffusionTieBreaking = vm.count("useDiffusionTieBreaking");
    settings.useGeometricTieBreaking = vm.count("useGeometricTieBreaking");
    settings.skipNoGainColors = vm.count("skipNoGainColors");
    settings.bisect = vm.count("bisect");
    settings.writeDebugCoordinates = vm.count("writeDebugCoordinates");

    if (vm.count("fileFormat")) {
        settings.fileFormat = vm["fileFormat"].as<ITI::Format>();
    }
    if (vm.count("coordFormat")) {
        settings.coordFormat = vm["coordFormat"].as<ITI::Format>();
    }
    if (vm.count("PEgraphFile")) {
        settings.PEGraphFile = vm["PEgraphFile"].as<std::string>();
    }
    if (vm.count("numNodeWeights")) {
        settings.numNodeWeights = vm["numNodeWeights"].as<IndexType>();
    }
    if (vm.count("dimensions")) {
        settings.dimensions = vm["dimensions"].as<IndexType>();
    }
    if (vm.count("numX")) {
        settings.numX = vm["numX"].as<IndexType>();
    }
    if (vm.count("numY")) {
        settings.numY = vm["numY"].as<IndexType>();
    }
    if (vm.count("numZ")) {
        settings.numZ = vm["numZ"].as<IndexType>();
    }
    if (vm.count("numBlocks")) {
        settings.numBlocks = vm["numBlocks"].as<IndexType>();
    } else {
        settings.numBlocks = comm->getSize();
    }

    if (vm.count("epsilon")) {
        settings.epsilon = vm["epsilon"].as<double>();
    }
    if (vm.count("blockSizesFile")) {
        settings.blockSizesFile = vm["blockSizesFile"].as<std::string>();
    }
    if (vm.count("initialPartition")) {
        settings.initialPartition = vm["initialPartition"].as<Tool>();
    }
    if (vm.count("multiLevelRounds")) {
        settings.multiLevelRounds = vm["multiLevelRounds"].as<IndexType>();
    }
    if (vm.count("minBorderNodes")) {
        settings.minBorderNodes = vm["minBorderNodes"].as<IndexType>();
    }
    if (vm.count("stopAfterNoGainRounds")) {
        settings.stopAfterNoGainRounds = vm["stopAfterNoGainRounds"].as<IndexType>();
    }
    if (vm.count("minGainForNextGlobalRound")) {
        settings.minGainForNextRound = vm["minGainForNextGlobalRound"].as<IndexType>();
    }
    if (vm.count("cutsPerDim")) {
        std::stringstream ss( vm["cutsPerDim"].as<std::string>() );
        std::string item;
        std::vector<IndexType> cutsPerDim;
        IndexType product = 1;

        while (!std::getline(ss, item, ' ').fail()) {
            IndexType cutsInDim = std::stoi(item);
            cutsPerDim.push_back(cutsInDim);
            product *= cutsInDim;
        }

        settings.cutsPerDim = cutsPerDim;

        if (!vm.count("numBlocks")) {
            settings.numBlocks = product;
        } else {
            if (vm["numBlocks"].as<IndexType>() != product) {
                throw std::invalid_argument("When giving --cutsPerDim, either omit --numBlocks or set it to the product of cutsPerDim.");
            }
        }
    }
    if (vm.count("pixeledSideLen")) {
        settings.pixeledSideLen = vm["pixeledSideLen"].as<IndexType>();
    }
    if (vm.count("minSamplingNodes")) {
        settings.minSamplingNodes = vm["minSamplingNodes"].as<IndexType>();
    }
    if (vm.count("influenceExponent")) {
        settings.influenceExponent = vm["influenceExponent"].as<ValueType>();
    }
    if (vm.count("influenceChangeCap")) {
        settings.influenceChangeCap = vm["influenceChangeCap"].as<ValueType>();
    }
    if (vm.count("balanceIterations")) {
        settings.balanceIterations = vm["balanceIterations"].as<IndexType>();
    }
    if (vm.count("maxKMeansIterations")) {
        settings.maxKMeansIterations = vm["maxKMeansIterations"].as<IndexType>();
    }
    if (vm.count("hierLevels")) {
        std::stringstream ss( vm["hierLevels"].as<std::string>() );
        std::string item;
        std::vector<IndexType> hierLevels;
        IndexType product = 1;

        while (!std::getline(ss, item, ' ').fail()) {
            IndexType blocksInLevel = std::stoi(item);
            hierLevels.push_back(blocksInLevel);
            product *= blocksInLevel;
            std::cout << product << std::endl;
        }

        settings.hierLevels = hierLevels;
        if (!vm.count("numBlocks")) {
            settings.numBlocks = product;
        } else {
            if (vm["numBlocks"].as<IndexType>() != product) {
                std::cout << vm["numBlocks"].as<IndexType>() << " " << product << std::endl;
                throw std::invalid_argument("When giving --hierLevels, either omit --numBlocks or set it to the product of level entries.");
            }
        }
    }
    if (vm.count("outFile")) {
        settings.outFile = vm["outFile"].as<std::string>();
    }
    if (vm.count("repeatTimes")) {
        settings.repeatTimes = vm["repeatTimes"].as<IndexType>();
    }
    if (vm.count("maxDiameterRounds")) {
        settings.maxDiameterRounds = vm["maxDiameterRounds"].as<IndexType>();
    }
    if (vm.count("metricsDetail")) {
        settings.metricsDetail = vm["metricsDetail"].as<std::string>();
    }
    if (vm.count("outDir")) {
        settings.outDir = vm["outDir"].as<std::string>();
    }

    /*** consistency checks ***/
    if (vm.count("previousPartition")) {
        settings.repartition = true;
        if (vm.count("initialPartition")) {
            if (!(settings.initialPartition == Tool::geoKmeans || settings.initialPartition == Tool::none)) {
                std::cout << "Method " << settings.initialPartition << " not supported for repartitioning, currently only kMeans." << std::endl;
                settings.isValid = false;
                //return 126;
            }
        } else {
            PRINT0("Setting initial partitioning method to kMeans.");
            settings.initialPartition = Tool::geoKmeans;
        }
    }

    if ( settings.hierLevels.size() > 0 ) {
        if (!(settings.initialPartition == Tool::geoHierKM
                || settings.initialPartition == Tool::geoHierRepart)) {
            if(comm->getRank() ==0 ) {
                std::cout << " WARNING: Without using hierarchical partitioning, ";
                std::cout << "the given hierarchy levels will be ignored." << std::endl;
            }
        }

        if (!vm.count("numBlocks")) {
            IndexType numBlocks = 1;
            for (IndexType level : settings.hierLevels) {
                numBlocks *= level;
            }
            settings.numBlocks = numBlocks;
        }
    }

    return settings;

}

}