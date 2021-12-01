# Pinned Loads

This repository contains a gem5 implementation of
**Pinned Loads: Taming Speculative Loads in Secure Processors**.

Please check our paper for details:

```
@inproceedings{Zhao:PinnedLoads:ASPLOS22,
author = {Zhao, Zirui Neil and Ji, Houxiang and Morrison, Adam and Marinov, Darko and Torrellas, Josep},
title = {Pinned Loads: Taming Speculative Loads in Secure Processors},
year = {2022},
publisher = {Association for Computing Machinery},
booktitle = {Proceedings of the 27th ACM International Conference on Architectural Support for Programming Languages and Operating Systems (ASPLOS'22)},
}
```

[You can find the Pinned Loads paper here!](http://iacoma.cs.uiuc.edu/work/chrono.html)
(To appear at ASPLOS'22, coming soon)


## Building Pinned Loads
Depending on your system, it may take about 5 to 20 minutes to build gem5.

### Using Docker
To make the building process easier,
we provide a Dockerfile under `docker/` and a script `cgem.sh`
that will build the Docker image and compile Pinned Loads.
Before you start, please set environment variable `GEM5_ROOT` to
the root of Pinned Loads.
(For example, if you cloned Pinned Loads to `$HOME/PinnedLoads`, execute
`export GEM5_ROOT=$HOME/PinnedLoads` to set the variable.)
Then, execute the script:
```bash
./cgem.sh
```
to build the Docker image and Pinned Loads.

For more information about installing and using Docker, please refer to Docker's
[official documents](https://docs.docker.com/engine/reference/commandline/docker/).

### Manual
You will need all [libraries](https://www.gem5.org/documentation/learning_gem5/part1/building/)
that are required by gem5.
Note that due to a [bug](https://gem5.atlassian.net/browse/GEM5-631) in gem5,
it might crash on some SPEC benchmarks in SE mode if
gem5 is built on a system that is newer than Ubuntu 16.04.

Once all libraries are installed, execute the following command under the Pinned Loads directory:
```bash
scons build/X86_MESI_Two_Level/gem5.opt -j$(nproc)
```

## Running Pinned Loads
We provide scripts that run Pinned Loads for all the schemes proposed in our paper.
For more details, please refer to this [README](scripts/README.md).

## Reproducibility
All the results in the paper can be reproduced. For more information about
re-running our experiments, please refer to this section in the [README](scripts/README.md#Reproducibility).

## License
All [Pinned Loads related changes](https://github.com/zzrcxb/PinnedLoads/compare/vanilla..main)
are released under MIT license.
The rest of contents are under the [original gem5 license](./LICENSE).
