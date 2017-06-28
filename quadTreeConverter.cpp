#include <string>
#include <vector>
#include <iostream>

#include <scai/lama/matrix/CSRSparseMatrix.hpp>
#include <scai/lama/DenseVector.hpp>

#include "FileIO.h"

using std::string;

int main(int argc, char* argv[]) {
	vector<string> filenames;
	for (int i = 1; i < argc; i++) {
		string filename = std::string(argv[i]);
		string graphname = filename + ".graph";
		string coordname = graphname + ".xyz";
		std::vector<scai::lama::DenseVector<double>> coordinates;
		scai::lama::CSRSparseMatrix<double> graph;

		try {
			graph = ITI::FileIO<int,double>::readQuadTree(filename, coordinates);
			std::cout << "Read file " << filename << std::endl;
		} catch (...) {
			std::cout << "Couldn't read " << filename << std::endl;
			throw;
		}

		try {
			ITI::FileIO<int,double>::writeGraph(graph, graphname);
			ITI::FileIO<int,double>::writeCoords(coordinates, coordname);
			std::cout << "Wrote graph to " << graphname << " and coords to " << coordname << std::endl;
		} catch (...) {
			std::cout << "Couldn't write " << graphname << " or " <<  coordname  << std::endl;
			throw;
		}

	}
}