#! /bin/bash

if [ -z $GEM5_ROOT ]; then
    echo "ENV variable GEM5_ROOT is not set" >&2
    exit 1
fi

EXEC="$GEM5_ROOT/build/X86_MESI_Two_Level/gem5.opt"

unset BENCHMARK SIMPT SUFFIX EXT L1CST L2CST SUITE_ROOT

MAXINSTS=50000000
L2VPARTITION=1
THREAT=Unsafe
HW=Unsafe

usage() {
    echo "Usage: spec.sh"
}

PARSED_ARGUMENTS=$(getopt -a -n spec -o hb:s:dgt:H:i:f --long \
help,bench:,simpt:,threat-model:,hardware:,\
maxinsts:,l2-par:,delay-inv,ext:,l1-cst:,l2-cst:,cam,\
record-size:,sbd:,suite: -- "$@")

if [ $? != 0 ]; then
    usage
    exit 1
fi

eval set -- "$PARSED_ARGUMENTS"
while :
do
    case "$1" in
        -b | --bench )
            echo "Running benchmark $2" >&2
            BENCHMARK=$2; shift 2;
            ;;
        -s | --simpt )
            echo "Restoring from simpt $2" >&2
            SIMPT=$2; shift 2;
            ;;
        -d | --delay-inv )
            echo "Delaying invalidation ack" >&2
            SUFFIX="$SUFFIX --delay-inv-ack"; shift;
            ;;
        --ext )
            echo "Output directory prefix is $2"
            EXT=$2; shift 2;
            ;;
        -g )
            echo "Enabled GDB" >&2
            EXEC="gdb --args $EXEC"; shift;
            ;;
        -t | --threat-model )
            echo "Using $2 as threat model" >&2
            THREAT=$2; shift 2;
            ;;
        -H | --hardware )
            echo "Using $2 as hardware scheme" >&2
            HW=$2; shift 2;
            ;;
        -i | --maxinsts )
            echo "Setting maximum instruction to $2" >&2
            MAXINSTS=$2; shift 2;
            ;;
        --l2-par )
            echo "Setting L2 virtual partition size to $2" >&2
            L2VPARTITION=$2; shift 2;
            ;;
        --l1-cst )
            echo "Using L1D CST $2" >&2
            SUFFIX="$SUFFIX --l1d-cst $2"; shift 2;
            ;;
        --l2-cst )
            echo "Using L2 CST $2" >&2
            SUFFIX="$SUFFIX --l2-cst $2"; shift 2;
            ;;
        --record-size )
            echo "Address field of CST is $2 bits" >&2
            SUFFIX="$SUFFIX --cst-record $2"; shift 2;
            ;;
        --cam )
            echo "Use CAM in CST" >&2
            SUFFIX="$SUFFIX --entry-cam"; shift;
            ;;
        --sbd )
            echo "Breakdown speculations in Comprehensive. Level: $2" >&2
            SUFFIX="$SUFFIX --spec-breakdown $2"; shift 2;
            ;;
        --suite )
            echo "Suite root is $2" >&2
            SUITE_ROOT=$2; shift 2;
            ;;
        -f )
            echo "Use gem5 fast" >&2
            EXEC="$GEM5_ROOT/build/X86_MESI_Two_Level/gem5.fast"; shift;
            ;;
        --dbg )
            echo "Use gem5 debug" >&2
            EXEC="$GEM5_ROOT/build/X86_MESI_Two_Level/gem5.debug"; shift;
            ;;
        -h | --help )
            usage
            exit 0
            ;;
        -- )
            shift; break;
            ;;
        * )
            echo "Unexpected option: $1" >&2
            usage
            exit 2
            ;;
    esac
done

if [ -z $BENCHMARK ] || [ -z $SIMPT ] || [ -z $SUITE_ROOT]; then
    echo "Error: need to provide BENCHMARK, SIMPT, and SUITE_ROOT" >&2
    usage
    exit 2
fi

RUN_ROOT=$SUITE_ROOT/run/$BENCHMARK
CKPT_ROOT=$SUITE_ROOT/ckpt/$BENCHMARK
OUTPUT_DIR=$GEM5_ROOT/output/${EXT}/${BENCHMARK}/${SIMPT}

git show HEAD | head -n 1 >  $OUTPUT_DIR/version
cd $RUN_ROOT
echo SUFFIX: $SUFFIX >&2
echo Simulation started at $(date) >&2
echo "" >&2

$EXEC -d $OUTPUT_DIR \
$GEM5_ROOT/configs/example/se.py --benchmark=$BENCHMARK \
--num-cpus=1 --mem-size=4096MB --cpu-type=DerivO3CPU \
--l1d_assoc=8 --l1i_assoc=4 --maxinsts=$MAXINSTS \
--l2_assoc=16 --num-l2caches=1 --l2-vpartition=$L2VPARTITION \
--needsTSO --ruby --l1-prefetch \
--at-instruction --simpt-ckpt=${SIMPT} --checkpoint-restore=1 \
--threat-model=$THREAT --hw=$HW --checkpoint-dir=$CKPT_ROOT \
--network=simple --topology=Mesh_XY --mesh-rows=1 \
--warmup-insts=1000000 --eager-translation $SUFFIX

EXIT_CODE=$?

echo "" >&2
echo Simulation ended at $(date) with $EXIT_CODE >&2

exit $EXIT_CODE
