#!/bin/bash
ME=$0
# BleepBox assets
ASSETS=$PWD/assets
CATEGORIES=$(ls -vd $ASSETS/*.wav | grep -oh "[a-zA-Z][a-zA-Z]*-" | sort -u | awk -F- '{print $1}')
CR='
'

usage() {
    echo ""
    echo "Droid Sound Generator"
    echo ""
    echo " Usage: $ME [options] [sound category] count [-play] [-out output_directory]"
    echo ""
    echo "        $ME -collection [options] [tempo] [suffix] [-out output_directory]"
    echo ""
    echo "        $ME -crowd [options] [-out output_directory]"
    echo ""
    echo "You can specify one or more of the following time and pitch ratio options."
    echo ""
    echo " -t<X>, --time <X>       Stretch to X times original duration, or"
    echo " -T<X>, --tempo <X>      Change tempo by multiple X (same as --time 1/X), or"
    echo " -T<X>, --tempo <X>:<Y>  Change tempo from X to Y (same as --time X/Y), or"
    echo " -D<X>, --duration <X>   Stretch or squash to make output file X seconds long"
    echo ""
    echo " -p<X>, --pitch <X>      Raise pitch by X semitones, or"
    echo " -f<X>, --frequency <X>  Change frequency by multiple X"
    echo ""
    echo " -M<F>, --timemap <F>    Use file F as the source for key frame map"
    echo ""
    echo "A map file consists of a series of lines each having two numbers separated"
    echo "by a single space.  These are source and target sample frame numbers for fixed"
    echo "time points within the audio data, defining a varying stretch factor through"
    echo "the audio.  You must specify an overall stretch factor using e.g. -t as well."
    echo ""
    echo "The following options provide a simple way to adjust the sound.  See below"
    echo "for more details."
    echo ""
    echo " -c<N>, --crisp <N>      Crispness (N = 0,1,2,3,4,5,6); default 5 (see below)"
    echo " -F,    --formant        Enable formant preservation when pitch shifting"
    echo ""
    echo "The remaining options fine-tune the processing mode and stretch algorithm."
    echo "These are mostly included for test purposes; the default settings and standard"
    echo "crispness parameter are intended to provide the best sounding set of options"
    echo "for most situations.  The default is to use none of these options."
    echo ""
    echo " -L,    --loose          Relax timing in hope of better transient preservation"
    echo " -P,    --precise        Ignored: The opposite of -L, this is default from 1.6"
    echo " --no-transients  Disable phase resynchronisation at transients"
    echo " --bl-transients  Band-limit phase resync to extreme frequencies"
    echo " --no-lamination  Disable phase lamination"
    echo " --window-long    Use longer processing window (actual size may vary)"
    echo " --window-short   Use shorter processing window"
    echo " --smoothing      Apply window presum and time-domain smoothing"
    echo " --detector-perc  Use percussive transient detector (as in pre-1.5)"
    echo " --detector-soft  Use soft transient detector"
    echo " --pitch-hq       In RT mode, use a slower, higher quality pitch shift"
    echo " --centre-focus   Preserve focus of centre material in stereo"
    echo "                  (at a cost in width and individual channel quality)"
    echo ""
    echo " -d<N>, --debug <N>      Select debug level (N = 0,1,2,3); default 0, full 3"
    echo "                         (N.B. debug level 3 includes audible ticks in output)"
    echo ""
    echo " Crispness levels:"
    echo "  -c 0   equivalent to --no-transients --no-lamination --window-long"
    echo "  -c 1   equivalent to --detector-soft --no-lamination --window-long (for piano)"
    echo "  -c 2   equivalent to --no-transients --no-lamination"
    echo "  -c 3   equivalent to --no-transients"
    echo "  -c 4   equivalent to --bl-transients"
    echo "  -c 5   default processing options"
    echo "  -c 6   equivalent to --no-lamination --window-short (may be good for drums)"
    echo ""
    echo " Sound Categories:"
    echo ""
    printf "   "
    for cat in ${CATEGORIES//\\n/$CR}
    do
        printf "$cat "
    done
    echo ""
    echo ""
    exit 0
}

function interrupt() {
  touch /tmp/__STOP_COMPOSE__
  echo Aborting
  reset
  exit 0
}

compose() {
    if [ -f /tmp/__STOP_COMPOSE__ ]; then
        exit 0
    fi

    FULL_ARGS=$@
    HAVE_RUBBERBAND_ARGS=0
    RUBBERBAND_ARGS=
    while [[ $1 == -* ]]; do
        RUBBERBAND_ARGS="$RUBBERBAND_ARGS $1"
        HAVE_RUBBERBAND_ARGS=1
        shift
    done
    CAT=
    if [ "$1" -eq "$1" ] 2>/dev/null ; then
        RANGE=$1
        shift
    else
        CAT=$1
        shift
        RANGE=$1
        shift
    fi

    ASSETFILE=
    if [ -f $ASSETS/$CAT ]; then
        ASSETFILE=$ASSETS/$CAT
        CAT=$(echo $CAT | awk -F- '{print $1}')
    fi

    DOPLAY=0
    if [ "$1" == "-play" ]; then
        DOPLAY=1
        shift
    fi
    OUTDIR=random
    if [ "$1" == "-out" ]; then
        shift
        OUTDIR=$1
        shift
    else
        echo BAD ARGS : $FULL_ARGS
        echo NUM1 : $1
        exit 0
    fi

    if [ -z $CAT ]; then
        CATCOUNT=1
        categories=($(cd ${ASSETS} ; ls *.wav))
        CATRANGE=$(echo "${#categories[@]}")
        while [ $CATCOUNT -le 2 ]
        do
            CATINDEX=$RANDOM
            let "CATINDEX %= $CATRANGE"
            CAT=$(echo ${categories[$CATINDEX]} | awk -F- '{print $1}')

            catfiles=($(ls ${ASSETS}/${CAT}-*.wav))
            CATCOUNT=$(echo "${#catfiles[@]}")
        done
    fi

    COUNT=$RANDOM
    let "COUNT %= $RANGE"
    let "COUNT += 1"
    shift
    mkdir -p $OUTDIR/wav/$CAT
    mkdir -p $OUTDIR/mp3/$CAT
    mkdir -p $OUTDIR/ogg/$CAT
    FILECOUNT=$(find $OUTDIR/wav/$CAT -maxdepth 1 -type f -name '*.wav' | wc -l | xargs echo)
    OUTPUT_WAV=$OUTDIR/wav/$CAT/$CAT-${FILECOUNT}.wav
    if [ "$HAVE_RUBBERBAND_ARGS" -eq 0 ]; then
        TEMP_OUTPUT_WAV=$OUTPUT_WAV
    else
        TEMP_OUTPUT_WAV=$OUTDIR/wav/$CAT/$CAT-${FILECOUNT}_tmp.wav
    fi
    OUTPUT_MP3=$OUTDIR/mp3/$CAT/$CAT-${FILECOUNT}.mp3
    OUTPUT_OGG=$OUTDIR/ogg/$CAT/$CAT-${FILECOUNT}.ogg
    if [ -z $ASSETFILE ]; then
        ASSETFILE=$(ls -vd $ASSETS/$CAT-*.wav | shuf -n $COUNT)
    fi
    echo Generating: $(basename $OUTPUT_WAV)
    ffmpeg -v 0 -y -f concat -safe 0 -i <(for f in $ASSETFILE; do echo "file '$f'"; done) -c copy $TEMP_OUTPUT_WAV
    if [ $? -ne 0 ]; then
        echo "Failed to create: $TEMP_OUTPUT_WAV"
        exit 1
    fi
    if [ "$HAVE_RUBBERBAND_ARGS" -ne 0 ]; then
        rubberband $RUBBERBAND_ARGS -q $TEMP_OUTPUT_WAV $OUTPUT_WAV
        rm $TEMP_OUTPUT_WAV
    fi
    if [ $DOPLAY -eq 1 ]; then
        aplay $OUTPUT_WAV
    fi

    ffmpeg -v 0 -y -i "$OUTPUT_WAV" -ac 2 -acodec libmp3lame "$OUTPUT_MP3"
    ffmpeg -v 0 -y -i "$OUTPUT_WAV" -ac 2 -acodec libvorbis "$OUTPUT_OGG"
}

compose_collection() {
    RUBBERBAND_ARGS=
    while [[ $1 == -* ]]; do
        RUBBERBAND_ARGS="$RUBBERBAND_ARGS $1"
        shift
    done

    OUTDIR=$1
    shift

    PLAYSOUND=""
    if [ "$1" == "-play" ]; then
        PLAYSOUND="-play"
        shift
    fi

    compose $RUBBERBAND_ARGS ack-1-0.wav 1 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS dome-1.wav 1 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS dome-2.wav 1 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS dome-3.wav 1 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS overhere 1 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS startup 10 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS cylon-1-0.wav 1 $PLAYSOUND -out $OUTDIR

    compose $RUBBERBAND_ARGS scream 5 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS scream 5 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS scream 5 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS scream 5 $PLAYSOUND -out $OUTDIR
    compose $RUBBERBAND_ARGS scream 5 $PLAYSOUND -out $OUTDIR

    x=300
    while [ $x -gt 0 ]; do
        compose $RUBBERBAND_ARGS 10 $PLAYSOUND -out $OUTDIR
        x=$(($x-1))
    done

    # Generate longer sounds
    x=100
    while [ $x -gt 0 ]; do
        compose $RUBBERBAND_ARGS 20 $PLAYSOUND -out $OUTDIR
        x=$(($x-1))
    done
}

compose_range() {
    tempo=$1
    shift
    suffix=$1
    shift

    OUTDIR="generated"
    RUBBERBAND_ARGS=
    while [[ $1 == -* ]]; do
        if [ "$1" == "-out" ]; then
            shift
            OUTDIR=$1
        else
            RUBBERBAND_ARGS="$RUBBERBAND_ARGS $1"
        fi
        shift
    done
    mkdir -p $OUTDIR

    compose_collection -p-10 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n10$suffix &
    compose_collection -p-9 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n9$suffix &
    compose_collection -p-8 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n8$suffix &
    compose_collection -p-7 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n7$suffix &
    compose_collection -p-6 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n6$suffix &
    compose_collection -p-5 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n5$suffix &
    compose_collection -p-4 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n4$suffix &
    compose_collection -p-3 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n3$suffix &
    compose_collection -p-2 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n2$suffix &
    compose_collection -p-1 $tempo $RUBBERBAND_ARGS $OUTDIR/r2n1$suffix &

    compose_collection -p1 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p1$suffix &
    compose_collection -p2 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p2$suffix &
    compose_collection -p3 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p3$suffix &
    compose_collection -p4 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p4$suffix &
    compose_collection -p5 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p5$suffix &
    compose_collection -p6 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p6$suffix &
    compose_collection -p7 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p7$suffix &
    compose_collection -p8 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p8$suffix &
    compose_collection -p9 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p9$suffix &
    compose_collection -p10 $tempo $RUBBERBAND_ARGS $OUTDIR/r2p10$suffix &
    for job in `jobs -p`
    do
        wait $job || let "FAIL+=1"
    done
}

compose_test() {
    sleep 10000
}

compose_crowd() {
    OUTDIR="generated"
    RUBBERBAND_ARGS=
    while [[ $1 == -* ]]; do
        if [ "$1" == "-out" ]; then
            shift
            OUTDIR=$1
        else
            RUBBERBAND_ARGS="$RUBBERBAND_ARGS $1"
        fi
        shift
    done
    compose_range -t0.75 f $RUBBERBAND_ARGS -out $OUTDIR &
    compose_range "" "" $RUBBERBAND_ARGS -out $OUTDIR &
    compose_range -t1.25 s $RUBBERBAND_ARGS -out $OUTDIR &
    for job in `jobs -p`
    do
        wait $job || let "FAIL+=1"
    done
}

if [ "$#" -eq 0 ]; then
    usage
fi

rm -f /tmp/__STOP_COMPOSE__
trap interrupt INT

if [ "$1" == "-collection" ]; then
    shift
    compose_collection $@
elif [ "$1" == "-crowd" ]; then
    shift
    compose_crowd $@
else
    compose $@
fi
