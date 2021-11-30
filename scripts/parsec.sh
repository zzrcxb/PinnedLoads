#! /bin/bash

if [ -z $GEM5_ROOT ]; then
    echo "ENV variable GEM5_ROOT is not set" >&2
    exit 1
fi

if [ -z $M5_PATH ]; then
    echo "ENV variable M5_PATH is not set" >&2
    exit 1
fi

EXEC="$GEM5_ROOT/build/X86_MESI_Two_Level/gem5.opt"

unset BENCHMARK SIMPT SUFFIX EXT L1CST L2CST SUITE_ROOT

MAXINSTS=50000000
L2VPARTITION=1
THREAT=Unsafe
HW=Unsafe
INPUT_SIZE=simmedium
CKPT_SAMPLE=sample
IMAGE=disks/x86root-SPLASH-PARSEC.img # relative to M5_PATH
KERNEL=binaries/vmlinux.x86 # relative to M5_PATH

usage() {
    echo "Usage: parsec.sh"
}

PARSED_ARGUMENTS=$(getopt -a -n parsec -o hb:s:dgt:H:i:f --long \
help,bench:,simpt:,threat-model:,hardware:,\
maxinsts:,l2-par:,delay-inv,ckpt:,ext:,l1-cst:,l2-cst:,\
cam,record-size:,sbd:,suite:,image:,kernel: -- "$@")

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
        --ckpt )
            echo "Using ckpt algorithm $2" >&2
            CKPT_SAMPLE=$2; shift 2;
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
        --image )
            echo "Using image $2" >&2
            IMAGE=$2; shift 2;
            ;;
        --kernel )
            echo "Using kernel $2" >&2
            KERNEL=$2; shift 2;
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

RUN_ROOT=$SUITE_ROOT/$BENCHMARK/$INPUT_SIZE/$CKPT_SAMPLE
CKPT_ROOT=$SUITE_ROOT/$BENCHMARK/$INPUT_SIZE/$CKPT_SAMPLE
OUTPUT_DIR=$GEM5_ROOT/output/${EXT}/${BENCHMARK}/${SIMPT}

git show HEAD | head -n 1 >  $OUTPUT_DIR/version
cd $RUN_ROOT
echo SUFFIX: $SUFFIX >&2
echo Simulation started at $(date) >&2
echo "" >&2

$EXEC -d $OUTPUT_DIR \
$GEM5_ROOT/configs/example/fs.py \
--disk-image=$M5_PATH/$IMAGE --kernel=$M5_PATH/$KERNEL \
--checkpoint-dir=$CKPT_ROOT --checkpoint-restore=$SIMPT \
--num-cpus=8 --mem-size=2048MB --cpu-type=DerivO3CPU --l1d_size=32kB \
--l1d_assoc=8 --l1i_assoc=4 --l2_assoc=16 --num-l2caches=8 --num-dirs=8 \
--l2-vpartition=$L2VPARTITION --needsTSO --ruby --l1-prefetch \
--threat-model=$THREAT --hw=$HW \
--network=simple --topology=Mesh_XY --mesh-rows=4 \
--maxinsts=$MAXINSTS --warmup-insts=1000000 --eager-translation $SUFFIX

EXIT_CODE=$?

echo "" >&2
echo Simulation ended at $(date) with $EXIT_CODE >&2

exit $EXIT_CODE
