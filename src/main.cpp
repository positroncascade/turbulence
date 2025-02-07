#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>

#include "nseof/Configuration.h"
#include "nseof/Simulation.h"
#include "nseof/flowmodels/laminar/Simulation.h"
#include "nseof/flowmodels/algebraic/Simulation.h"
#include "nseof/flowmodels/ke/Simulation.h"
#include "nseof/parallelManagers/PetscParallelConfiguration.h"
#include "nseof/geometry/GeometryManager.h"
#include "nseof/MeshsizeFactory.h"
#include "nseof/MultiTimer.h"

#ifdef WITH_HDF5
#include "nseof/hdf5/HDF5Plotter.h"
#endif

int main(int argc, char *argv[]) {
  auto timer = nseof::MultiTimer::get();
  timer->start("total");
  timer->start("initialization");

  // Parallelization related. Initialize and identify
  // ---------------------------------------------------
  int rank;   // This processor's identifier
  int nproc;  // Number of processors in the group
  PetscInitialize(&argc, &argv, "petsc_commandline_arg", PETSC_NULL);
  MPI_Comm_size(PETSC_COMM_WORLD, &nproc);
  MPI_Comm_rank(PETSC_COMM_WORLD, &rank);
  std::cout << "Rank: " << rank << ", Nproc: " << nproc << std::endl;
  //----------------------------------------------------
  // read configuration and store information in parameters object
  nseof::Configuration configuration(argv[1]);
  nseof::Parameters parameters;
  configuration.loadParameters(parameters);
  nseof::PetscParallelConfiguration parallelConfiguration(parameters);
  nseof::MeshsizeFactory::getInstance().initMeshsize(parameters);
  nseof::Simulation *simulation = NULL;

  if (parameters.geometry.dim == 2) {
    parameters.geometry.sizeZ = 1;
    parameters.parallel.localSize[2] = 1;
  }

#ifdef DEBUG
  std::cout << "Processor " << parameters.parallel.rank << " with index ";
  std::cout << parameters.parallel.indices[0] << ",";
  std::cout << parameters.parallel.indices[1] << ",";
  std::cout << parameters.parallel.indices[2];
  std::cout << " is computing the size of its subdomain and obtains ";
  std::cout << parameters.parallel.localSize[0] << ", ";
  std::cout << parameters.parallel.localSize[1] << " and ";
  std::cout << parameters.parallel.localSize[2];
  std::cout << ". Left neighbour: " << parameters.parallel.leftNb;
  std::cout << ", right neighbour: " << parameters.parallel.rightNb;
  std::cout << std::endl;
  std::cout << "Min. meshsizes: " << parameters.meshsize->getDxMin() << ", "
            << parameters.meshsize->getDyMin() << ", "
            << parameters.meshsize->getDzMin() << std::endl;
#endif

  nseof::geometry::GeometryManager gm(parameters);

  // initialise simulation
  if (parameters.simulation.type == "algebraic") {
    if (rank == 0) {
      std::cout << "Start turbulent simulation in " << parameters.geometry.dim
                << "D" << std::endl;
    }

    // create algebraic turbulent simulation
    simulation = new nseof::flowmodels::algebraic::Simulation(parameters, gm);
  } else if (parameters.simulation.type == "ke") {
    if (rank == 0) {
      std::cout << "Start turbulent simulation (k-epsilon) in "
                << parameters.geometry.dim << "D" << std::endl;
      std::cout << "with model parameters:" << std::endl;
      std::cout << "ce1     :  " << parameters.kEpsilon.ce1 << std::endl;
      std::cout << "ce2     :  " << parameters.kEpsilon.ce2 << std::endl;
      std::cout << "cmu     :  " << parameters.kEpsilon.cmu << std::endl;
      std::cout << "sigmaK  :  " << parameters.kEpsilon.sigmaK << std::endl;
      std::cout << "sigmaE  :  " << parameters.kEpsilon.sigmaE << std::endl;
    }

    // create k-epsilon turbulent simulation
    simulation = new nseof::flowmodels::ke::Simulation(parameters, gm);
  } else if (parameters.simulation.type == "dns") {
    if (rank == 0) {
      std::cout << "Start DNS simulation in " << parameters.geometry.dim << "D"
                << std::endl;
    }

    simulation = new nseof::flowmodels::laminar::Simulation(parameters, gm);
  } else {
    handleError(
        1, "Unknown simulation type! Currently supported: dns, turbulence");
  }
  // call initialization of simulation (initialize flow field)
  if (simulation == NULL) {
    handleError(1, "simulation==NULL!");
  }
  simulation->initializeFlowField();

  nseof::FLOAT time = 0.0;
  nseof::FLOAT timeVTK = parameters.vtk.interval;
  nseof::FLOAT timeBAK = parameters.restart.interval;
  int timeSteps = 0;

  // TODO WS1: plot initial state
  simulation->deserialize();
  simulation->init();

#ifdef WITH_HDF5
  nseof::FLOAT timeHDF5 = parameters.hdf5.interval;
  int lastHDF5 = -1;
  std::unique_ptr<nseof::hdf5::HDF5Plotter> hdf5plotter;
  if (parameters.hdf5.enabled) {
    hdf5plotter =
        std::make_unique<nseof::hdf5::HDF5Plotter>(parameters, rank, nproc);

    timer->start("hdf5");
    hdf5plotter->plotFlowField(0, simulation->getFlowField());
    timer->stop("hdf5");
  }
#endif

  timer->start("vtk");
  simulation->plotVTK(rank, 0);
  timer->stop("vtk");

  timer->stop("initialization");

  // time loop
  while (time < parameters.simulation.finalTime) {
    // DIRTY: Communicate current time to KE simulation
    parameters.timestep.time = time;
    parameters.timestep.timeSteps = timeSteps;

    simulation->solveTimestep();

    time += parameters.timestep.dt;

    // std-out: terminal info
    if (rank == 0) {
      std::cout << "Current time: " << time
                << "\ttimestep: " << parameters.timestep.dt << std::endl;
    }

    timeSteps++;

    // TODO WS1: trigger VTK output
    if (time >= timeVTK) {
      if (time >= parameters.vtk.start) {
        timer->start("vtk");
        simulation->plotVTK(rank, timeSteps);
        timer->stop("vtk");
      }

      timeVTK += parameters.vtk.interval;
    }

#ifdef WITH_HDF5
    if (time >= timeHDF5) {
      if (parameters.hdf5.enabled) {
        timer->start("hdf5");
        hdf5plotter->plotFlowField(timeSteps, simulation->getFlowField());
        timer->stop("hdf5");

        lastHDF5 = timeSteps;
      }

      timeHDF5 += parameters.hdf5.interval;
    }
#endif

    if (time >= timeBAK) {
      simulation->serialize();
      timeBAK += parameters.restart.interval;
    }
  }

  // TODO WS1: plot final output
  simulation->serialize();
  timer->start("vtk");
  simulation->plotVTK(rank, timeSteps);
  timer->stop("vtk");

#ifdef WITH_HDF5
  // Plot the final state if it was not plotted in the last iteration
  if (parameters.hdf5.enabled && timeSteps > lastHDF5) {
    timer->start("hdf5");
    hdf5plotter->plotFlowField(timeSteps, simulation->getFlowField());
    timer->stop("hdf5");
  }

  // Explicitly destroy the plotter here so that it finalizes the HDF5 file over
  // MPI before MPI is shut down via PetscFinalize.
  hdf5plotter.reset();
#endif

  delete simulation;
  simulation = NULL;

  PetscFinalize();

  timer->stop("total");

  if (parameters.timing.enabled) {
    std::ostringstream path;
    path << parameters.timing.prefix << "rank-" << rank;

    std::fstream file;
    file.open(path.str().c_str(), std::ios::out);

    if (file.fail()) {
      std::cout << "Could not open " << std::endl;
      return 1;
    }

    file << timer->toString();
    file << "timesteps " << timeSteps << std::endl;
  }
}
