#! /bin/bash

if [ -z $GEM5_ROOT ]; then
    echo "ENV variable GEM5_ROOT is not set" >&2
    exit 1
fi

cd docker && docker build -t gem5-env .

docker run -it --rm -e LOCAL_USER_ID=$(id -u) \
                    -e GEM5_ROOT=$GEM5_ROOT \
                    -v $GEM5_ROOT:$GEM5_ROOT gem5-env \
                    /bin/bash -c "cd $GEM5_ROOT && scons build/X86_MESI_Two_Level/gem5.opt -j$(nproc)"
