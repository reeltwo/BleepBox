#!/bin/bash
# ./volume_adjust.sh *.wav
mkdir -p adjust
for f in $*
do
	INVERSE_MAX_VOLUME=$(ffmpeg -i $f -af volumedetect -f null - 2>&1 >/dev/null | grep max_volume | awk '{ printf "%.2f", -$5}')
	of=$(basename $f)
	echo Adjusting ${of} by ${INVERSE_MAX_VOLUME}dB
	ffmpeg -hide_banner -loglevel panic -y -i $f -filter:a "volume=${INVERSE_MAX_VOLUME}dB" adjust/${of}
done