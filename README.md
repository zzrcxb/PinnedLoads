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


## Environment
To build and run Pinned Loads, you will need all libraries that are required by
[gem5](https://www.gem5.org/documentation/learning_gem5/part1/building/).
Note that due to a [bug](https://gem5.atlassian.net/browse/GEM5-631) in gem5,
it might crash on some SPEC benchmarks in SE mode if
gem5 is built on a system that is newer than Ubuntu 16.04.

To make building a "bug-free" gem5 easier,
we provide a Dockerfile under `docker/`,
which is based on Ubuntu 16.04 and captures all the gem5's dependencies.
To build the docker image, execute
```bash
cd docker && docker build -t gem5 .
```

Once the docker image is built, you can build gem5 inside the container,
by mounting the gem5 directory to it.
For example, assuming you have environment variable `$GEM5_ROOT` set:
```bash
docker run -it --rm -e LOCAL_USER_ID=$(id -u) -e GEM5_ROOT=$GEM5_ROOT -v $GEM5_ROOT:$GEM5_ROOT gem5 /bin/bash -c "cd $GEM5_ROOT && scons build/X86_MESI_Two_Level/gem5.opt -j$(nproc)"
```
will build `gem5.opt` for configuration `X86_MESI_Two_Level`.

Depending on your system, it may take about 5 to 20 minutes to build gem5.
So, grab a cup of coffee or take a nap, it's your choice :)


For more advanced Docker usage, please refer to Docker's
[official documents](https://docs.docker.com/engine/reference/commandline/docker/).

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
