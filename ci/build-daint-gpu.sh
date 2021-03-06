#!/bin/bash -l
#SBATCH --job-name=build-daint-gpu
#SBATCH --nodes=1
#SBATCH --constraint=mc
#SBATCH --partition=cscsci
#SBATCH --time=00:15:00
#SBATCH --output=build-daint-gpu.out
#SBATCH --error=build-daint-gpu.err

set -e

source ${ENVFILE}

mkdir -p build
(
    cd build
    cmake -DUSE_MKL=On \
          -DUSE_ELPA=Off \
          -DGPU_MODEL=P100 \
          -DUSE_MAGMA=On \
          -DUSE_CUDA=On \
          -DCREATE_PYTHON_MODULE=On \
          ../
    make -j24 VERBOSE=1
) && echo "build successful"
