#include <stdio.h>
#include <string.h>
#include <stdlib.h> 
#include <stdarg.h>
#include <time.h>
#include <math.h>
#ifdef _MSC_VER
#include <direct.h>
#include <Winsock.h>
#define strdup _strdup
int getline(char** lineptr, size_t* n, FILE* fp);
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#endif

#ifdef HAVE_SMQ
#include <json-c/json.h>
#include "smq.h"
#endif

#include "AudioFile.h"
#include "portaudio.h"
#if defined(__linux)
#include <dlfcn.h>
#include "pa_linux_alsa.h"
#endif
#include "aubio.h"
#include "rubberband/RubberBandStretcher.h"
#include "sndfile.h"

#include <json-c/json.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_status.h>
#include <mach/mach_init.h>
#include <mach/task.h>
#include <mach/semaphore.h>
#define NULL_SEMAPHORE ((semaphore_t)0)
#elif defined(_MSC_VER)
#else
#include <semaphore.h>
#endif

#if defined(__linux)
#include <linux/unistd.h>
#include <sys/syscall.h>
#if defined(__NR_gettid)
#ifdef _syscall0
static inline _syscall0(pid_t, gettid);
#else
static inline pid_t gettid(void)
{
    return (pid_t)syscall(__NR_gettid);
}
#endif
#else
#define gettid() getpid()
#endif
static void pthread_setcurrentpriority(int pri)
{
    static int sPriorities[] = { 0, 10, 8, 4, 2, 0, -2, -4, -6, -8, -10, -12, -14, -16 };
    setpriority(PRIO_PROCESS, gettid(), sPriorities[pri]);
}
#else
#define pthread_setcurrentpriority(pri)
#endif

#if defined(__ARM_ARCH)
#define USE_CACHE
#endif

#ifdef _MSC_VER
typedef HANDLE OSTHREAD_HANDLE;
#else
typedef pthread_t OSTHREAD_HANDLE;
#endif

using namespace RubberBand;

#define SAMPLE_RATE   (44100)
// #if defined(USE_CACHE)
// #define FRAMES_PER_BUFFER  (64)
// #else
#define FRAMES_PER_BUFFER  (64)
// #endif

#define SizeOfArray(arr) (sizeof(arr)/sizeof(arr[0]))

extern "C" char* encode_md5(char* buf, ssize_t len);

typedef void* (*pthread_start_proc_t)(void*);

struct MarcCommand
{
    const char* fMarc;
    const char* fCmd;
    MarcCommand* fNext;
};

struct RNote
{
    smpl_t fPitch;
    uint_t fStartSampleTime;
    uint_t fEndSampleTime;
};

struct SoundSnippet;

struct SoundCategory
{
    const char*    fName;
    size_t         fCatIndex;
    size_t         fCount;
    SoundSnippet*  fSnippets;
    SoundCategory* fNext;
    size_t         fReserved;

    SoundCategory(const char* name, size_t catIndex) :
        fName(strdup(name)),
        fCatIndex(catIndex),
        fCount(0),
        fSnippets(NULL),
        fNext(NULL),
        fReserved(0)
    {
    }
};

struct AudioChannelCmd
{
    int fCmd;
    bool fLooping;
    int fOctaveOffset;
    int fCatIndex;
    int fSndIndex;
    int fSndIndexStart;
    int fSndIndexEnd;
    bool fDone;
    double fRatio;
    double fPitchshift;
    double fFrequencyshift;
    bool fMusic;
    bool fSingleSound;
    bool fReverse;
    char* fID;
};

struct AudioChannelThread
{
    int fChannel;
    OSTHREAD_HANDLE fThread;
    PaStream* fStream;
#ifdef _MSC_VER
    HANDLE fSem;
#elif defined(__APPLE__)
    semaphore_t fSem;
#else
    sem_t* fSem;
    sem_t fSemData;
#endif
    int fCurrentCmd;
    AudioChannelCmd fCmd;
    AudioChannelCmd fCmdStack[100];
    bool fInterrupt;
    // int fCmd;
    bool fQuit;
    // bool fLooping;
    // bool fMusic;
    // int fOctaveOffset;
    // char* fRTTTL;
    struct GlobalState* fData;

    int fCatIndex;
    int fSnippetIndex;
    int fSourceSoundIndex;
    int fUsedIndex;
    bool fRepeatSound;
    unsigned fZeroPadCount;
    bool fDone;
    bool fNext;
    bool fSilenced;
    char* fRecording;
    SNDFILE* fRecordingFile;
    SF_INFO fRecordingInfo;
    bool fRecordingPlayAtEnd;
    RubberBandStretcher* fStretcher;
    unsigned fOutDataIndex;
    unsigned fOutDataAvail;
    int fOutDataRead;
    int fInDataWritten;
    int fChirpCount;
    int fLongCount;
    int fChirpCountMax;
    int fLongCountMax;
    bool fRandom;
    bool fReverse;
    float fLastValue;
    float fFadeOutValue;
    float fTempo;
    float* fOutData;
    double fNoteDuration;
    int fMidiNote;
    SNDFILE* fCacheFile;
    SF_INFO fCacheInfo;
    float* fCacheMixDownBuffer;
    float* fCacheMixDownPtr;
    float* fCacheMixDownEnd;
    float fbuf[FRAMES_PER_BUFFER];
};

struct RTTTLSong
{
    const char* fName;
    const char* fRTTTL;
    RTTTLSong* fNext;
};

struct StealthVar
{
    const char* fName;
    const char* fValue;
    StealthVar* fNext;
};

struct SoundSnippet
{
    int fNum;
    int fSubNum;
    off_t fSize;
    int fUsedIndex;
    const char* fName;
    AudioFile<float>* fAudio;
    // change this to a dynamic structure
    int fNoteCount;
    RNote fNotes[30];
};

smpl_t lastmidi = 0.;

static int snippet_sorter(const void * a, const void * b)
{
    SoundSnippet* snda = (SoundSnippet*)a;
    SoundSnippet* sndb = (SoundSnippet*)b;
    int ret = snda->fNum - sndb->fNum;
    return (ret == 0) ? snda->fSubNum - sndb->fSubNum : ret;
}

off_t fsize(const char* dir, const char *filename)
{
    struct stat st;
    char path[2048];
    snprintf(path, sizeof(path)-1, "%s/%s", dir, filename);
    if (stat(path, &st) == 0)
        return st.st_size;

    return -1; 
}

static void process_block_notes(SoundSnippet* snippet, aubio_notes_t* notes, uint_t time_in_samples, fvec_t *ibuf, fvec_t *obuf)
{
    RNote* rnote = &snippet->fNotes[snippet->fNoteCount];

    aubio_notes_do(notes, ibuf, obuf);
    // did we get a note off?
    if (obuf->data[2] != 0)
    {
        lastmidi = obuf->data[2];
        if (rnote->fPitch != 0)
        {
            rnote->fEndSampleTime = time_in_samples;
            if (snippet->fNoteCount < SizeOfArray(snippet->fNotes))
            {
                snippet->fNoteCount++;
                rnote[1].fPitch = 0;
            }
        }
        // send_noteon(time_in_samples, lastmidi, 0);
    }
    rnote = &snippet->fNotes[snippet->fNoteCount];
    // did we get a note on?
    if (obuf->data[0] != 0)
    {
        lastmidi = obuf->data[0];
        rnote->fPitch = lastmidi;
        rnote->fStartSampleTime = time_in_samples;
        // send_noteon(time_in_samples, lastmidi, obuf->data[1]);
    }
}

AudioFile<float>* loadAudio(SoundSnippet* snippet, const char* dir, const char *filename, bool verbose = false)
{
    struct stat st;
    char path[2048];
    char jsonpath[2048+10];
    AudioFile<float>* audio = NULL;
    snprintf(path, sizeof(path)-1, "%s/%s", dir, filename);
    snprintf(jsonpath, sizeof(jsonpath)-1 , "%s/.%s.dat", dir, filename);
    if (stat(path, &st) == 0)
    {
        audio = new AudioFile<float>;
        if (!audio->load(path))
        {
            printf("Failed to load : %s\n", path);
            delete audio;
            audio = NULL;
        }
        else
        {
            uint_t samplerate = 0;
            uint_t buffer_size = 512;
            uint_t hop_size = 256;
            fvec_t* input_buffer;
            fvec_t* output_buffer;

            const char* onset_method = "default";
            smpl_t onset_threshold = 0.0; // will be set if != 0.
            smpl_t onset_minioi = 0.0; // will be set if != 0.

            const char* pitch_unit = "default";
            const char* pitch_method = "default";
            smpl_t pitch_tolerance = 0.0; // will be set if != 0.

            smpl_t silence_threshold = -90.;
            smpl_t release_drop = 10.;

            char* md5hash = encode_md5((char*)&audio->samples[0][0], audio->samples.size() * sizeof(float));

            struct stat st;
            bool hashMatch = false;
            json_object* jobj = NULL;
            if (stat(jsonpath, &st) == 0)
            {
                jobj = json_object_from_file(jsonpath);
                json_object* json_hash;
                if (json_object_object_get_ex(jobj, "hash", &json_hash))
                {
                    if (strcmp(json_object_get_string(json_hash), md5hash) == 0)
                    {
                        hashMatch = true;
                    }
                }
            }
            if (hashMatch)
            {
                json_object* json_notes;
                if (json_object_object_get_ex(jobj, "notes", &json_notes))
                {
                    snippet->fNoteCount = json_object_array_length(json_notes);
                    for (int i = 0; i < snippet->fNoteCount; i++)
                    {
                        RNote* rnote = &snippet->fNotes[i];
                        json_object* json_pitch;
                        json_object* json_start;
                        json_object* json_end;
                        json_object* jrnote = json_object_array_get_idx(json_notes, i);
                        if (jrnote != NULL &&
                            json_object_object_get_ex(jrnote, "pitch", &json_pitch) &&
                            json_object_object_get_ex(jrnote, "start", &json_start) &&
                            json_object_object_get_ex(jrnote, "end", &json_end))
                        {
                            rnote->fPitch = json_object_get_double(json_pitch);
                            rnote->fStartSampleTime = json_object_get_int64(json_start);
                            rnote->fEndSampleTime = json_object_get_int64(json_end);
                        }
                        else
                        {
                            hashMatch = false;
                            break;
                        }
                    }
                }
                else
                {
                    hashMatch = false;
                }
            }
            if (!hashMatch)
            {
                aubio_source_t* this_source;
                this_source = new_aubio_source(path, samplerate, hop_size);
                if (this_source != NULL)
                {
                    samplerate = aubio_source_get_samplerate(this_source);

                    input_buffer = new_fvec(hop_size);
                    output_buffer = new_fvec(hop_size);

                    aubio_notes_t* notes;
                    notes = new_aubio_notes ("default", buffer_size, hop_size, samplerate);

                    if (onset_minioi != 0.)
                    {
                        aubio_notes_set_minioi_ms(notes, onset_minioi);
                    }
                    if (onset_threshold != 0.)
                    {
                        printf("warning: onset threshold not supported yet\n");
                        //aubio_onset_set_threshold(aubio_notes_get_aubio_onset(o), onset_threshold);
                    }
                    if (silence_threshold != -90.)
                    {
                        if (aubio_notes_set_silence (notes, silence_threshold) != 0)
                        {
                            printf("failed setting notes silence threshold to %.2f\n", silence_threshold);
                        }
                    }
                    if (release_drop != 10.)
                    {
                        if (aubio_notes_set_release_drop (notes, release_drop) != 0)
                        {
                            printf("failed setting notes release drop to %.2f\n", release_drop);
                        }
                    }

                    uint_t read = 0;
                    uint_t total_read = 0;
                    int blocks = 0;
                    snippet->fNoteCount = 0;
                    memset(snippet->fNotes, '\0', sizeof(snippet->fNotes));

                    if (verbose)
                        printf("Processing ... %s\n", path);
                    do
                    {
                        aubio_source_do(this_source, input_buffer, &read);
                        process_block_notes(snippet, notes, blocks * hop_size, input_buffer, output_buffer);
                        blocks++;
                        total_read += read;
                    }
                    while (read == hop_size);

                    //printf("read %.2fs (%d samples in %d blocks of %d) from %s at %dHz\n", total_read * 1. / samplerate, total_read, blocks, hop_size, path, samplerate);
                    // send a last note off if required
                    if (lastmidi)
                    {
                        RNote* rnote = &snippet->fNotes[snippet->fNoteCount];
                        if (rnote->fPitch != 0 && rnote->fEndSampleTime == 0)
                        {
                            rnote->fEndSampleTime = blocks * hop_size;
                            snippet->fNoteCount++;
                        }
                    }
                    del_aubio_notes(notes);

                    del_aubio_source(this_source);
                    if (verbose)
                        printf("=============\n");
                    for (int i = 0; i < snippet->fNoteCount; i++)
                    {
                        RNote* rnote = &snippet->fNotes[i];
                        if (verbose && rnote->fPitch != 0)
                        {
                            printf("[%d] : %f [%d]\n", rnote->fStartSampleTime, rnote->fPitch, rnote->fEndSampleTime - rnote->fStartSampleTime);
                        }
                    }

                    json_object* jnotes = json_object_new_array();
                    for (int i = 0; i < snippet->fNoteCount; i++)
                    {
                        RNote* rnote = &snippet->fNotes[i];
                        json_object* jrnote = json_object_new_object();
                        json_object_object_add(jrnote, "pitch", json_object_new_double(rnote->fPitch));
                        json_object_object_add(jrnote, "start", json_object_new_int64(rnote->fStartSampleTime));
                        json_object_object_add(jrnote, "end", json_object_new_int64(rnote->fEndSampleTime));
                        json_object_array_add(jnotes, jrnote);
                    }
                    json_object *jroot = json_object_new_object();
                    json_object_object_add(jroot, "hash", json_object_new_string(md5hash));
                    json_object_object_add(jroot, "notes", jnotes);
                    json_object_to_file(jsonpath, jroot);
                    json_object_put(jroot);
                }
            }
            if (jobj != NULL)
                json_object_put(jobj);
            if (md5hash != NULL)
                free(md5hash);
        }
    }
    return audio;
}

struct StealthSoundBank
{
    int fCount;
    int fIndex;
    const char* fName;
    char** fStealthBank;
    char** fBleepBank;
};

struct GlobalState
{
    size_t fChannelCount;
    AudioChannelThread* fChannels;
    size_t fCategoryCount;
    size_t fCatSelectCount;
    size_t* fCatSelect;
    size_t* fSnippetCounts;
    SoundSnippet** fSnippets;
    SoundCategory* fCategoryList;
    RTTTLSong* fSongList;
    MarcCommand* fMarcList;

    OSTHREAD_HANDLE fMARCServerThread;
    const char* fVMusicPort;
    OSTHREAD_HANDLE fVMusicThread;
    int fVMusicFD;

    const char* fStealthPort;
    OSTHREAD_HANDLE fStealthThread;
    int fStealthFD;
    bool fConfigStealth;
    StealthVar* fStealthVarHead;
    StealthVar* fStealthVarTail;
    int fStealthSoundBankCount;
    StealthSoundBank* fStealthSoundBank;

    bool fDefHaveRatio;
    double fDefratio;
    double fDefpitchshift;
    double fDeffrequencyshift;
    bool fDefdoreverse;
    bool fQuit;
    bool fUseCache;
};

static int random(int max, int min =0) //range : [min, max)
{
   static bool first = true;
   if (first) 
   {  
        // struct timespec ts;
        // timespec_get(&ts, TIME_UTC);
        // srandom(ts.tv_nsec ^ ts.tv_sec);
   #ifdef _MSC_VER
       srand((unsigned int)time(NULL));
   #else
       struct timeval ts;
       gettimeofday(&ts, 0);
       srandom(ts.tv_usec ^ ts.tv_sec);
       first = false;
   #endif
   }
#ifdef _MSC_VER
   return min + rand() % ((max)-min);
#else
   return min + random() % (( max ) - min);
#endif
}

static const char* sNotes[] = {
    "",    "",     "",    "",     "",    "",    "",     "",    "",     "",    "",     "",
    "",    "",     "",    "",     "",    "",    "",     "",    "",     "a0",  "a0#",  "b0", 
    "c1",  "c1#",  "d1",  "d1#",  "e1",  "f1",  "f1#",  "g1",  "g1#",  "a1",  "a1#",  "b1",
    "c2",  "c2#",  "d2",  "d2#",  "e2",  "f2",  "f2#",  "g2",  "g2#",  "a2",  "a2#",  "b2",
    "c3",  "c3#",  "d3",  "d3#",  "e3",  "f3",  "f3#",  "g3",  "g3#",  "a3",  "a3#",  "b3",
    "c4",  "c4#",  "d4",  "d4#",  "e4",  "f4",  "f4#",  "g4",  "g4#",  "a4",  "a4#",  "b4",
    "c5",  "c5#",  "d5",  "d5#",  "e5",  "f5",  "f5#",  "g5",  "g5#",  "a5",  "a5#",  "b5",
    "c6",  "c6#",  "d6",  "d6#",  "e6",  "f6",  "f6#",  "g6",  "g6#",  "a6",  "a6#",  "b6",
    "c7",  "c7#",  "d7",  "d7#",  "e7",  "f7",  "f7#",  "g7",  "g7#",  "a7",  "a7#",  "b7",
    "c8",  "c8#",  "d8",  "d8#",  "e8",  "f8",  "f8#",  "g8",  "g8#",  "a8",  "a8#",  "b8",
    "c9",  "c9#",  "d9",  "d9#",  "e9",  "f9",  "f9#",  "g9",  "g9#",  "a9",  "a9#",  "b9",
    "c10", "c10#", "d10", "d10#", "e10", "f10", "f10#", "g10", "g10#", "a10", "a10#", "b10",
    "c11", "c11#", "d11", "d11#", "e11", "f11", "f11#", "g11", "g11#", "a11", "a11#", "b11"
};

static const char* sScaleCMajor[] = {
    "c", "d", "e", "f", "g", "a", "b"
};

static const char* sScaleDMajor[] = {
    "d", "e", "f#", "g", "a", "b", "c#"
};

static const char* midiNoteToNote(int midiNote)
{
    return (midiNote != -1 && midiNote < SizeOfArray(sNotes)) ? sNotes[midiNote] : "";
}

static size_t noteToMidiNote(const char* note)
{
    if (note[0] != 0)
    {
        const char* dotidx = strchr(note, '.');
        size_t len = (dotidx != NULL) ? dotidx - note : strlen(note);
        for (size_t i = 0; i < SizeOfArray(sNotes); i++)
        {
            if (len == strlen(sNotes[i]) && strncmp(sNotes[i], note, len) == 0)
                return i;
        }
    }
    return -1;
}

static RubberBandStretcher* wrapSnippet(SoundSnippet* snippet, double ratio, double pitchshift = 0.0, double frequencyshift = 1.0)
{
    double duration = 0.0;
    int debug = 0;
    bool precise = true;
    int threading = 0;
    bool lamination = true;
    bool longwin = false;
    bool shortwin = false;
    bool smoothing = false;
    bool hqpitch = true;
    bool formant = true;
    bool together = true;
    bool crispchanged = false;
    int crispness = -1;
    bool help = false;
    bool version = false;
    bool quiet = false;

    enum {
        NoTransients,
        BandLimitedTransients,
        Transients
    } transients = Transients;

    enum {
        CompoundDetector,
        PercussiveDetector,
        SoftDetector
    } detector = CompoundDetector;

    switch (crispness)
    {
        case -1:
            crispness = 5;
            break;
        case 0:
            detector = CompoundDetector;
            transients = NoTransients;
            lamination = false;
            longwin = true;
            shortwin = false;
            break;
        case 1:
            detector = SoftDetector;
            transients = Transients;
            lamination = false;
            longwin = true;
            shortwin = false;
            break;
        case 2:
            detector = CompoundDetector;
            transients = NoTransients;
            lamination = false;
            longwin = false;
            shortwin = false;
            break;
        case 3:
            detector = CompoundDetector;
            transients = NoTransients;
            lamination = true;
            longwin = false;
            shortwin = false;
            break;
        case 4:
            detector = CompoundDetector;
            transients = BandLimitedTransients;
            lamination = true;
            longwin = false;
            shortwin = false;
            break;
        case 5:
            detector = CompoundDetector;
            transients = Transients;
            lamination = true;
            longwin = false;
            shortwin = false;
            break;
        case 6:
            detector = CompoundDetector;
            transients = Transients;
            lamination = false;
            longwin = false;
            shortwin = true;
            break;
    };

    RubberBandStretcher::Options options = 0;
    options |= RubberBandStretcher::OptionProcessRealTime;
    if (precise)     options |= RubberBandStretcher::OptionStretchPrecise;
    if (!lamination) options |= RubberBandStretcher::OptionPhaseIndependent;
    if (longwin)     options |= RubberBandStretcher::OptionWindowLong;
    if (shortwin)    options |= RubberBandStretcher::OptionWindowShort;
    if (smoothing)   options |= RubberBandStretcher::OptionSmoothingOn;
    if (formant)     options |= RubberBandStretcher::OptionFormantPreserved;
    if (hqpitch)     options |= RubberBandStretcher::OptionPitchHighQuality;
    if (together)    options |= RubberBandStretcher::OptionChannelsTogether;

    switch (threading)
    {
        case 0:
            options |= RubberBandStretcher::OptionThreadingAuto;
            break;
        case 1:
            options |= RubberBandStretcher::OptionThreadingNever;
            break;
        case 2:
            options |= RubberBandStretcher::OptionThreadingAlways;
            break;
    }

    switch (transients)
    {
        case NoTransients:
            options |= RubberBandStretcher::OptionTransientsSmooth;
            break;
        case BandLimitedTransients:
            options |= RubberBandStretcher::OptionTransientsMixed;
            break;
        case Transients:
            options |= RubberBandStretcher::OptionTransientsCrisp;
            break;
    }

    switch (detector)
    {
        case CompoundDetector:
            options |= RubberBandStretcher::OptionDetectorCompound;
            break;
        case PercussiveDetector:
            options |= RubberBandStretcher::OptionDetectorPercussive;
            break;
        case SoftDetector:
            options |= RubberBandStretcher::OptionDetectorSoft;
            break;
    }

    RubberBandStretcher* ts;
    AudioFile<float>* sourceAudio = snippet->fAudio;
    ts = new RubberBandStretcher(SAMPLE_RATE, 1, options, ratio, frequencyshift);
    ts->setExpectedInputDuration(sourceAudio->getNumSamplesPerChannel());
    return ts;
}

/* This routine will be called by the PortAudio engine when audio is needed.
** It may called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int audioChannelCallback(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer,
                                const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData)
{
    bool dofinal = false;
    volatile AudioChannelThread* act = (AudioChannelThread*)userData;
    GlobalState *data = act->fData;
    SoundSnippet* sourceSnippet = &data->fSnippets[act->fCatIndex][act->fSnippetIndex];
    AudioFile<float>* sourceAudio = sourceSnippet->fAudio;
    size_t sampleEnd = (sourceSnippet->fNoteCount > 0) ? sourceSnippet->fNotes[sourceSnippet->fNoteCount-1].fEndSampleTime : sourceAudio->samples[0].size();
    sampleEnd = sourceAudio->samples[0].size();
    float *out = (float*)outputBuffer;
    unsigned long i;

    (void) timeInfo; /* Prevent unused variable warnings. */
    (void) statusFlags;
    (void) inputBuffer;

    for (i = 0; i < framesPerBuffer; i++)
    {
        *out = 0;
        if (act->fDone)
        {
            act->fSilenced = true;
            *out++ = 0;
            continue;
        }
        if (act->fZeroPadCount != 0)
        {
            *out++ = 0;
            act->fZeroPadCount--;
            continue;
        }
        if (act->fCacheFile != NULL)
        {
            if (act->fCacheMixDownBuffer == NULL)
            {
                act->fCacheMixDownBuffer = new float[act->fCacheInfo.channels * 32768];
                act->fCacheMixDownEnd = act->fCacheMixDownBuffer + (act->fCacheInfo.channels * 32768);
                act->fCacheMixDownPtr = act->fCacheMixDownEnd;
            }
            if (act->fCacheMixDownPtr >= act->fCacheMixDownEnd)
            {
                // printf("framesPerBuffer          : %d\n", framesPerBuffer);
                // printf("act->fCacheInfo.channels : %d\n", act->fCacheInfo.channels);
                sf_count_t readcount = sf_read_float(act->fCacheFile, act->fCacheMixDownBuffer, act->fCacheInfo.channels * 32768);
                //printf("readcount : %d\n", readcount); 
                if (readcount <= 0)
                {
                    delete [] act->fCacheMixDownBuffer;
                    act->fCacheMixDownBuffer = NULL;
                    act->fDone = true;
                    continue;//return paComplete;
                }
                act->fCacheMixDownPtr = act->fCacheMixDownBuffer;
                act->fCacheMixDownEnd = act->fCacheMixDownBuffer + readcount;
            }
            else
            {
                if (act->fInterrupt)
                {
                    // printf("act->fInterrupt : %d\n", act->fInterrupt);
                    if (act->fCacheMixDownBuffer != NULL)
                        delete [] act->fCacheMixDownBuffer;
                    act->fCacheMixDownBuffer = NULL;
                    act->fDone = true;
                    return paComplete;
                }
                // printf("%p[%p] channels=%d\n", act->fCacheMixDownPtr, act->fCacheMixDownEnd, act->fCacheInfo.channels);
                for (int q = 0; q < act->fCacheInfo.channels; q++)
                    *out += *act->fCacheMixDownPtr++;
                if (act->fCacheInfo.channels > 1)
                    *out /= act->fCacheInfo.channels;
                if (*out > 1.0)
                    *out = 1.0;
                else if (*out < -1.0)
                    *out = -1.0;
                out++;
            }
            continue;
        }
    reloop:
        bool repeatSound = false;
        // printf("dofinal : %d [%p]\n", dofinal, &dofinal);
        if ((act->fCmd.fMusic || act->fCmd.fSingleSound) && act->fDone)
        {
            act->fDone = true;
            act->fSilenced = true;
            continue;
        }
        if (act->fFadeOutValue != 0)
        {
            act->fFadeOutValue = act->fFadeOutValue / 2;
            *out++ = act->fFadeOutValue;
            continue;
        }
        // Check if we have any data to read from our stretcher
        if (act->fOutDataIndex < act->fOutDataAvail)
        {
            // printf("[%d:%d] ", act->fOutDataIndex, act->fOutDataAvail);
            float val = act->fOutData[act->fOutDataIndex++];
            if (val > 1.f) val = 1.f;
            if (val < -1.f) val = -1.f;
            *out++ = act->fLastValue = val;
            // printf("%f ", val);
            act->fOutDataRead++;
            continue;
        }
        else if (act->fStretcher != NULL && (act->fOutDataAvail = act->fStretcher->available()) > 0)
        {
            if (act->fOutData != NULL)
                delete [] act->fOutData;
            // printf("READ : %d [%d] dofinal=%d\n", act->fOutDataAvail, act->fOutDataRead, dofinal);
            act->fOutData = new float[act->fOutDataAvail];
            act->fStretcher->retrieve((float**)&act->fOutData, act->fOutDataAvail);
            act->fOutDataIndex = 0;
            if (dofinal)
            {
                delete act->fStretcher;
                act->fStretcher = NULL;
                act->fNext = true;
            }
            goto reloop;
        }
        else if (act->fStretcher == NULL || act->fSourceSoundIndex >= sampleEnd)
        {
            bool noshift = false;
            // printf("OUT DATA : %d [%d] act->fNext=%d\n", act->fOutDataIndex, act->fOutDataAvail, act->fNext);
            // Need to select a new sound
            act->fReverse = act->fCmd.fReverse;
            if (!act->fCmd.fMusic && !act->fCmd.fSingleSound)
                act->fReverse = (sampleEnd < 8000 && random(100) < 25) ? true : false;
            if (act->fStretcher != NULL)
            {
                // printf("more act? : %d\n", act->fStretcher->available());
                // if (sourceAudio->samples[0][sourceAudio->samples[0].size()] != 0)
                // {
                //     act->fZeroPadCount = 10;
                // }
                // act->fZeroPadCount += random(sourceAudio->samples[0].size()/2, sourceAudio->samples[0].size()/1000);
            }
            // printf("act->fZeroPadCount : %d\n", act->fZeroPadCount);
            if (act->fCmd.fMusic)
            {
                if (dofinal)
                {
                    act->fDone = true;
                    act->fSilenced = true;
                    continue;
                }
            }
            else if (act->fCmd.fSingleSound)
            {
                // printf("HMM act->fStretcher=%p act->fNext=%d\n", act->fStretcher, act->fNext);
                noshift = true;
                act->fCatIndex = act->fCmd.fCatIndex;
                act->fSnippetIndex = act->fCmd.fSndIndex;
                if (act->fNext)
                {
                    if (act->fCmd.fLooping)
                    {
                        act->fCmd.fSndIndex = act->fCmd.fSndIndexStart; 
                    }
                    else
                    {
                        act->fDone = true;
                        act->fSilenced = true;
                        goto reloop;
                    }
                }
                act->fReverse = act->fCmd.fReverse;
                sourceSnippet = &act->fData->fSnippets[act->fCatIndex][act->fSnippetIndex];
                sourceAudio = sourceSnippet->fAudio;
            }
            else if (act->fRandom)
            {
                for (;;)
                {
                    if (act->fStretcher == NULL || sampleEnd >= 10000 || random(100) < 50)
                    {
                        if (act->fStretcher == NULL || random(100) < 4)
                            act->fCatIndex = random(data->fCategoryCount);
                        act->fSnippetIndex = /*(act->fStretcher != NULL && act->fCatIndex == 0 && random(100) > 10) ? random(11, 0) :*/ random(data->fSnippetCounts[act->fCatIndex]);
                    }
                    AudioFile<float>* nextAudio = data->fSnippets[act->fCatIndex][act->fSnippetIndex].fAudio;
                    if (act->fChirpCount < act->fChirpCountMax)
                    {
                        if (nextAudio->samples[0].size() < 10000)
                        {
                            act->fChirpCount++;
                            break;
                        }
                    }
                    else if (act->fLongCount < act->fLongCountMax)
                    {
                        if (nextAudio->samples[0].size() >= 10000)
                        {
                            act->fLongCount++;
                            break;
                        }
                    }
                    else
                    {
                        act->fLongCount = 0;
                        act->fChirpCount = 0;
                        act->fChirpCountMax = random(7, 3);
                        act->fLongCountMax = random(4, 1);
                        if (random(data->fSnippetCounts[act->fCatIndex]) < 10)
                            act->fCatIndex = 0;
                    }
                }
            }
            else
            {
                SoundSnippet* sourceSnippet = &data->fSnippets[act->fCatIndex][act->fSnippetIndex];
                if (act->fStretcher != NULL && !act->fCmd.fLooping)
                {
                    // advance to next sound
                    act->fFadeOutValue = act->fLastValue;
                    SoundSnippet* nextSnippet = &data->fSnippets[act->fCatIndex][act->fSnippetIndex+1];
                    if (act->fSnippetIndex + 1 < data->fSnippetCounts[act->fCatIndex] &&
                        sourceSnippet->fNum == nextSnippet->fNum)
                    {
                        if (sampleEnd < 10000)
                        {
                            if (random(100) < 50)
                                act->fSnippetIndex += 1;
                            else
                                repeatSound = true;
                        }
                        else if (random(100) > 10)
                            act->fSnippetIndex += 1;
                        else
                            repeatSound = true;
                    }
                    else
                    {
                        act->fDone = true;
                        act->fSilenced = true;
                        continue;
                    }
                }
            }
            sourceSnippet = &data->fSnippets[act->fCatIndex][act->fSnippetIndex];
            sourceAudio = sourceSnippet->fAudio;
            sampleEnd = (sourceSnippet->fNoteCount > 0) ? sourceSnippet->fNotes[sourceSnippet->fNoteCount-1].fEndSampleTime : sourceAudio->samples[0].size();
            sampleEnd = sourceAudio->samples[0].size();

            double pitchshift = (repeatSound || random(100) < 10) ? (sampleEnd < 10000) ? random(10, -5) : (act->fCatIndex == 1) ? random(2) : random(5) : 0;
            double ratio = (sampleEnd < 8000) ?
                (random(100) < 50) ? random(125, 100) / 100.0 : 1.0 :
                (random(100) < 25) ? random(200, 100) / 100.0 : 1.0;
            double frequencyshift = 1.0;
            if (noshift)
            {
                pitchshift = 0;
                ratio = 1.0;
            }
            if (act->fCmd.fMusic)
            {
                if (act->fMidiNote == 0)
                {
                    act->fDone = true;
                    act->fSilenced = true;
                    continue;
                }
                pitchshift = act->fMidiNote - sourceSnippet->fNotes[0].fPitch;
                if (pitchshift < -20)
                {
                    printf("act->fMidiNote   : %d\n", act->fMidiNote);
                    printf("fNotes[0].fPitch : %g\n", sourceSnippet->fNotes[0].fPitch);
                }
                double induration = double(sampleEnd) / 44.1;
                ratio = (induration != 0.0) ? act->fNoteDuration / induration : 1.0;
                // ratio += 0.1;
                if (pitchshift != 0.0)
                {
                    frequencyshift *= pow(2.0, pitchshift / 12);
                }
            }
            else if (act->fCmd.fSingleSound)
            {
                ratio = act->fCmd.fRatio;
                pitchshift = act->fCmd.fPitchshift;
                frequencyshift = act->fCmd.fFrequencyshift;
            }
            // pitchshift -= 10;
            // ratio += 2.0;
            printf("play : %s [%d:%d] [pitch:%g] [ratio:%g] [max:%d:%d] [reverse:%d] [repeat:%d]\n", sourceSnippet->fName, act->fCatIndex, act->fSnippetIndex, pitchshift, ratio, act->fChirpCountMax, act->fLongCountMax, act->fReverse, repeatSound);
            if (act->fStretcher != NULL)
            {
                // printf("PREVIOUS READ : %d WROTE : %d\n", act->fOutDataRead, act->fInDataWritten);
                delete act->fStretcher;
                act->fStretcher = NULL;
            }
            dofinal = false;
            act->fOutDataIndex = 0;
            act->fOutDataAvail = 0;
            act->fOutDataRead = 0;
            act->fInDataWritten = 0;
            act->fNext = false;
            act->fStretcher = wrapSnippet(sourceSnippet, ratio, pitchshift, frequencyshift);
            // printf("noteCount : %d start : %d end : %d [%d] [note : %f \"%s\"]\n",
            //     sourceSnippet->fNoteCount,
            //     sourceSnippet->fNotes[0].fStartSampleTime,
            //     sourceSnippet->fNotes[sourceSnippet->fNoteCount-1].fEndSampleTime,
            //     (int)sourceAudio->samples[0].size(),
            //     sourceSnippet->fNotes[0].fPitch,
            //     midiNoteToNote(sourceSnippet->fNotes[0].fPitch));
            act->fSourceSoundIndex = sourceSnippet->fNotes[0].fStartSampleTime;
            act->fSourceSoundIndex = 0;
            act->fTempo = ratio;
            sourceSnippet->fUsedIndex = ++act->fUsedIndex;
            if (act->fDone)
            {
                act->fSilenced = true;
                continue;
            }
        }

        // Pump sourceAudio into stretcher
        if (act->fSourceSoundIndex < sampleEnd)
        {
            size_t countIn = 0;
            float* ibuf = (float*)act->fbuf;
            size_t bufSize = SizeOfArray(act->fbuf);
            while (act->fSourceSoundIndex < sampleEnd && countIn < bufSize)
            {
                if (act->fReverse)
                {
                    ibuf[countIn++] = sourceAudio->samples[0][sampleEnd - ++act->fSourceSoundIndex];
                }
                else
                {
                    ibuf[countIn++] = sourceAudio->samples[0][act->fSourceSoundIndex++];
                }
            }
            dofinal = (act->fSourceSoundIndex == sampleEnd);

            act->fInDataWritten += countIn;
            // printf("PROCESS %d dofinal=%d [%d] act->fDone=%d\n", (int)countIn, dofinal, act->fInDataWritten, act->fDone);
            act->fStretcher->process(&ibuf, countIn, dofinal);
            goto reloop;
        }
    }
    
    return paContinue;
}

/*
 * This routine is called by portaudio when playback is done.
 */
static void StreamFinished( void* userData )
{
   AudioChannelThread* act = (AudioChannelThread*)userData;
}

static void StreamPlayTime(AudioChannelThread* act, long duration)
{
    if (act->fRecording != NULL)
    {
        size_t sampleCount = size_t(double(44.1) * duration);
        size_t bufSize = sampleCount * sizeof(float);
        float* buffer = (float*)malloc(bufSize);
        float* buffer_end = buffer + sampleCount;
        float* bufferp = buffer;

        memset(buffer, '\0', bufSize);
        while (bufferp < buffer_end)
        {
            unsigned long framesPerBuffer = (buffer_end - bufferp < FRAMES_PER_BUFFER) ? buffer_end - bufferp : FRAMES_PER_BUFFER;
            audioChannelCallback(NULL, bufferp, framesPerBuffer, NULL, 0, act);
            bufferp += framesPerBuffer;
        }
        if (sf_write_float(act->fRecordingFile, buffer, sampleCount) != sampleCount)
        {
            printf("sndfile error : %s\n", sf_strerror(act->fRecordingFile));
        }
        free(buffer);
    }
    else
    {
        Pa_Sleep(duration);
    }
}

void playRTTTLSequence(AudioChannelThread* act, const char *p, int octaveOffset)
{
    int default_dur = 4;
    int default_oct = 6;
    int bpm = 63;
    int num;
    int wholenote;
    int duration;
    int note;
    int midinote;
    int scale;
    bool playSound = false;
    int scount = 0;
    int sindex = 0;
    GlobalState* data = act->fData;


    act->fCatIndex = act->fCmd.fCatIndex;
    act->fSnippetIndex = act->fCmd.fSndIndex;

    // format: d=N,o=N,b=NNN:
    // find the start (skip name, etc)
    while (*p != ':') p++;    // ignore name
    p++;                     // skip ':'
    // get default duration
    if (*p == 'd') 
    {
        p++; p++;              // skip "d="
        num = 0;
        while(isdigit(*p))
        {
            num = (num * 10) + (*p++ - '0');
        }
        if (num > 0)
            default_dur = num;
        p++;                   // skip comma
    }
    // printf("ddur: %d\n", default_dur);
    // get default octave
    if (*p == 'o')
    {
        p++; p++;              // skip "o="
        num = *p++ - '0';
        if (num >= 3 && num <= 7)
            default_oct = num;
        p++;                   // skip comma
    }
    // printf("doct: %d\n", default_oct);
    // get BPM
    if (*p == 'b')
    {
        p++; p++;              // skip "b="
        num = 0;
        while (isdigit(*p))
        {
            num = (num * 10) + (*p++ - '0');
        }
        bpm = num;
        p++;                   // skip colon
    }
    // printf("bpm: %d\n", 10);
    // BPM usually expresses the number of quarter notes per minute
    wholenote = (60 * 1000L / bpm) * 4;  // this is the time for whole note (in milliseconds)
    // printf("wn: %d\n", wholenote);
    // now begin note loop
    while (*p && !act->fInterrupt)
    {
        bool reverse = false;
        if (*p == '[' || (reverse = (*p == ']')))
        {
            int sindex = -1;
            int catindex = -1; p++;
            if (isalpha(*p))
            {
                char cat[100];
                char* cp = cat;
                char* catbufend = &cat[sizeof(cat)-1];
                while (isalnum(*p))
                {
                    if (cp < catbufend)
                        *cp++ = *p;
                    p++;
                }
                *cp = '\0';
                SoundCategory* catList = data->fCategoryList;
                while (catList != NULL)
                {
                    if (strcmp(catList->fName, cat) == 0)
                    {
                        catindex = catList->fCatIndex;
                        break;
                    }
                    catList = catList->fNext;
                }
                if (*p == '-')
                {
                    if (cp < catbufend)
                        *cp++ = *p++;
                    while (isalnum(*p) || *p == '-')
                    {
                        if (cp < catbufend)
                            *cp++ = *p;
                        p++;
                    }
                    *cp = '\0';
                    if (catList != NULL)
                    {
                        for (size_t i = 0; i < catList->fCount; i++)
                        {
                            SoundSnippet* snippet = &catList->fSnippets[i];
                            if (strcmp(snippet->fName, cat) == 0)
                            {
                                sindex = i;
                                break;
                            }
                        }
                    }
                }
            }
            else if (isdigit(*p))
            {
                catindex = 0;
                while(isdigit(*p))
                {
                    catindex = (catindex * 10) + (*p++ - '0');
                }
                if (*p == ':')
                {
                    sindex = 0; p++;
                    while(isdigit(*p))
                    {
                        sindex = (sindex * 10) + (*p++ - '0');
                    }
                }
            }
            if (catindex == -1)
                catindex = random(data->fCategoryCount);
            act->fCatIndex = (catindex >= data->fCategoryCount) ? data->fCategoryCount-1 : catindex;
            if (sindex == -1)
                sindex = random(data->fSnippetCounts[act->fCatIndex]);
            act->fSnippetIndex = (sindex >= data->fSnippetCounts[act->fCatIndex]) ? data->fSnippetCounts[act->fCatIndex]-1 : sindex;
            act->fCmd.fReverse = reverse;
            if (*p == '+')
            {
                p++;
                scount = 0;
                while(isdigit(*p))
                {
                    scount = (scount * 10) + (*p++ - '0');
                }
                scount = (scount + act->fSnippetIndex >= data->fSnippetCounts[act->fCatIndex]) ?
                    data->fSnippetCounts[act->fCatIndex]-1-act->fSnippetIndex : scount;
            }
            if (*p == ',') p++;       // skip comma for next note (or we may be at the end)
        }
        // get note duration, if available
        num = 0;
        while(isdigit(*p))
        {
            num = (num * 10) + (*p++ - '0');
        }
        if (num)
            duration = wholenote / num;
        else
            duration = wholenote / default_dur;  // we will need to check if we are a dotted note after
        // now get the note
        note = midinote = 0;
        if (*p == '$')
        {
            p++;
            if (p[0] == 'd' && p[1] == 'o')
            {
                note = 1;
                p += 2;
            }
            else if (p[0] == 'd' && p[1] == 'i')
            {
                note = 2;
                p += 2;
            }
            else if (p[0] == 'r' && p[1] == 'e')
            {
                note = 3;
                p += 2;
            }
            else if (p[0] == 'r' && p[1] == 'i')
            {
                note = 4;
                p += 2;
            }
            else if (p[0] == 'm' && p[1] == 'i')
            {
                note = 5;
                p += 2;
            }
            else if (p[0] == 'f' && p[1] == 'a')
            {
                note = 6;
                p += 2;
            }
            else if (p[0] == 'f' && p[1] == 'i')
            {
                note = 7;
                p += 2;
            }
            else if (p[0] == 's' && p[1] == 'o')
            {
                note = 8;
                p += 2;
            }
            // else if (p[0] == 's' && p[1] == 'i')
            // {
            //     note = 9;
            //     p += 2;
            // }
            else if (p[0] == 'l' && p[1] == 'a')
            {
                note = 10;
                p += 2;
            }
            else if (p[0] == 'l' && p[1] == 'i')
            {
                note = 11;
                p += 2;
            }
            else if (p[0] == 's' && p[1] == 'i')
            {
                note = 12;
                p += 2;
            }
            // now, get optional '.' dotted note
            if (*p == '.')
            {
                duration += duration/2;
                p++;
            }  
            // now, get scale
            if (isdigit(*p))
            {
                scale = *p - '0';
                p++;
            }
            else
                scale = default_oct;
            scale += octaveOffset;
        }
        else if (*p == 'M')
        {
            p++;
            playSound = true;
            sindex = 0;
            midinote = 0;
            while(isdigit(*p))
            {
                midinote = (midinote * 10) + (*p++ - '0');
            }
        }
        else
        {
            if (*p == 'S')
            {
                playSound = true;
                sindex = 0;
                switch(*++p)
                {
                    case 'c':
                        note = 1;
                        p++;
                        break;
                    case 'd':
                        note = 3;
                        p++;
                        break;
                    case 'e':
                        note = 5;
                        p++;
                        break;
                    case 'f':
                        note = 6;
                        p++;
                        break;
                    case 'g':
                        note = 8;
                        p++;
                        break;
                    case 'a':
                        note = 10;
                        p++;
                        break;
                    case 'b':
                        note = 12;
                        p++;
                        break;
                    case 'p':
                        p++;
                    default:
                        note = 0;
                        break;
                }
            }
            else
            {
                switch(*p)
                {
                    case 'c':
                        note = 1;
                        break;
                    case 'd':
                        note = 3;
                        break;
                    case 'e':
                        note = 5;
                        break;
                    case 'f':
                        note = 6;
                        break;
                    case 'g':
                        note = 8;
                        break;
                    case 'a':
                        note = 10;
                        break;
                    case 'b':
                        note = 12;
                        break;
                    case 'p':
                    default:
                        note = 0;
                        break;
                }
                p++;
            }
            // now, get optional '#' sharp
            if (*p == '#')
            {
                note++;
                p++;
            }
            // now, get optional '.' dotted note
            if (*p == '.')
            {
                duration += duration/2;
                p++;
            }  
            // now, get scale
            if (isdigit(*p))
            {
                scale = *p - '0';
                p++;
            }
            else
                scale = default_oct;
            scale += octaveOffset;
        }
        if (*p == ',') p++;       // skip comma for next note (or we may be at the end)
        // now play the note
        if (note || midinote || playSound)
        {
            SoundSnippet* sourceSnippet = &data->fSnippets[act->fCatIndex][act->fSnippetIndex];
            if (playSound)
            {
                act->fSnippetIndex += (scount > 0) ? 1 : 0;
                sourceSnippet = &data->fSnippets[act->fCatIndex][act->fSnippetIndex];
                AudioFile<float>* sourceAudio = sourceSnippet->fAudio;
                printf("Play   : %s [%d:%d] note=%d\n", sourceSnippet->fName, sindex, scount, note);
                act->fNoteDuration = double(sourceAudio->samples[0].size()) / 44.1;
                act->fMidiNote = (midinote != 0) ? midinote : (note == 0) ? sourceSnippet->fNotes[0].fPitch : 59 + (scale - 4) * 12 + note;
                duration = act->fNoteDuration;
                if (sindex < scount)
                {
                    sindex++;
                    playSound = true;
                }
                else
                {
                    act->fSnippetIndex -= scount;
                    playSound = false;
                    sindex = 0;
                }
            }
            else
            {
                printf("Playing: %d[%s]:%d\n", 59 + (scale - 4) * 12 + note, midiNoteToNote(59 + (scale - 4) * 12 + note), duration);
                act->fNoteDuration = double(duration);
                act->fMidiNote = 59 + (scale - 4) * 12 + note;
            }

            act->fZeroPadCount = 0;
            act->fDone = false;
            act->fSilenced = false;
            act->fReverse = true;
            // tone(TONE_PIN, notes[(scale - 4) * 12 + note]);
            StreamPlayTime(act, duration);
            act->fDone = true;
            while (!act->fSilenced)
            {
                StreamPlayTime(act, 10);
            }
            act->fMidiNote = 0;
            act->fZeroPadCount = ~0;
            act->fSourceSoundIndex = 0;
            act->fOutDataIndex = 0;
            act->fOutDataAvail = 0;
            act->fOutDataRead = 0;
            act->fInDataWritten = 0;
            act->fFadeOutValue = 0;
            if (act->fOutData != NULL)
            {
                delete [] act->fOutData;
                act->fOutData = NULL;
            }
            if (act->fStretcher != NULL)
            {
                delete act->fStretcher;
                act->fStretcher = NULL;
            }
        }
        else
        {
            printf("\n");
            // printf("Pausing: %d\n", duration);
            act->fZeroPadCount = ~0;
            StreamPlayTime(act, duration);
            act->fDone = true;
            while (!act->fSilenced)
            {
                StreamPlayTime(act, 10);
            }
            act->fZeroPadCount = 0;
            act->fSourceSoundIndex = 0;
            act->fOutDataIndex = 0;
            act->fOutDataAvail = 0;
            act->fOutDataRead = 0;
            act->fInDataWritten = 0;
            act->fFadeOutValue = 0;
            if (act->fOutData != NULL)
            {
                delete [] act->fOutData;
                act->fOutData = NULL;
            }
            if (act->fStretcher != NULL)
            {
                delete act->fStretcher;
                act->fStretcher = NULL;
            }
            act->fDone = false;
        }
    }
}

// SWmarchLowGoodFull:d=4,o=5,b=80:[1:15,8d.,8d.,8d.,8a#4,16f,8d.,8a#4,16f,d.,32p,8a.,8a.,8a.,8a#,16f,8c#.,8a#4,16f,d.,32p,8d.6,8d,16d,8d6,32p,8c#6,16c6,16b,16a#,8b,32p,16d#,8g#,32p,8g,16f#,16f,16e,8f,32p,16a#4,8c#,32p,8a#4,16c#,8f.,8d,16f,a.,32p,8d.6,8d,16d,8d6,32p,8c#6,16c6,16b,16a#,8b,32p,16d#,8g#,32p,8g,16f#,16f,16e,8f,32p,16a#4,8c#,32p,8a#4,16f,8d.,8a#4,16f,d.
// $play SWmarchLowGoodFull
//
// -- play dome sounds
// $p #2,domestart
// $p #2,#LOOP,domerepeat
// $p #2,domestop

// $play music file
// $play #3,music/vader-1.wav
//
// -- stop channel
// $stop
// $stop #2
// $stop #3
//
// -- stop all channels
// $stopall
// $sa

static bool isspaceeol(char ch)
{
    return (isspace(ch) || ch == 0);
}

static void sendACT_Command(AudioChannelThread* act, AudioChannelCmd &cmd)
{
    if (act->fCurrentCmd < SizeOfArray(act->fCmdStack))
    {
        act->fCmdStack[act->fCurrentCmd] = cmd;
        act->fCurrentCmd += 1;
    #ifdef _MSC_VER
        ::SetEvent(act->fSem);
    #elif defined(__APPLE__)
        semaphore_signal(act->fSem);
    #else
        sem_post(act->fSem);
    #endif
    }
}

static void sendACT_PlayRTTTL(AudioChannelThread* act, int catIndex, int sndIndex, const char* rtttl, int octaveOffset, bool doloop, bool doqueue)
{
    AudioChannelCmd cmd;
    memset(&cmd, 0, sizeof(AudioChannelCmd));
    cmd.fLooping = doloop;
    cmd.fMusic = true;
    cmd.fCatIndex = catIndex;
    cmd.fSndIndex = sndIndex;
    cmd.fSingleSound = false;
    cmd.fOctaveOffset = octaveOffset;
    cmd.fID = strdup(rtttl);
    cmd.fCmd = 1;
    act->fInterrupt = !doqueue;
    sendACT_Command(act, cmd);
}

static void sendACT_PlaySnippet(AudioChannelThread* act, int catIndex, int sndIndex, int sndIndexEnd, int octaveOffset, bool doloop, bool doqueue, double ratio, double pitchshift, double frequencyshift, bool doreverse)
{
    AudioChannelCmd cmd;
    memset(&cmd, 0, sizeof(AudioChannelCmd));
    cmd.fLooping = doloop;
    cmd.fMusic = false;
    cmd.fSingleSound = true;
    cmd.fRatio = ratio;
    cmd.fPitchshift = pitchshift;
    cmd.fFrequencyshift = frequencyshift;
    cmd.fCatIndex = catIndex;
    cmd.fSndIndex = sndIndex;
    cmd.fSndIndexStart = sndIndex;
    cmd.fSndIndexEnd = sndIndexEnd;
    cmd.fReverse = doreverse;
    cmd.fCmd = 2;
    act->fInterrupt = !doqueue;
    sendACT_Command(act, cmd);
}

static void sendACT_PlayCache(AudioChannelThread* act, const char* file, bool doqueue, bool doloop)
{
    AudioChannelCmd cmd;
    memset(&cmd, 0, sizeof(AudioChannelCmd));
    cmd.fID = strdup(file);
    cmd.fLooping = doloop;
    cmd.fCmd = 3;
    act->fInterrupt = !doqueue;
    sendACT_Command(act, cmd);
}

static void sendACT_Stop(AudioChannelThread* act)
{
    AudioChannelCmd cmd;
    memset(&cmd, 0, sizeof(AudioChannelCmd));
    cmd.fCmd = 4;
    act->fInterrupt = true;
    sendACT_Command(act, cmd);
}

static char* atof(char* p, double &val)
{
    char buffer[100];
    char* ch = buffer;
    char* chend = &buffer[sizeof(buffer)-1];
    while (*p != ',' && !isspace(*p))
    {
        if (ch < chend)
            *ch++ = *p;
        p++;
    }
    *ch++ = '\0';
    val = atof(buffer);
    return p;
}

static char* tempo_convert(char* p, double &val)
{
    char buffer[100];
    char* ch = buffer;
    char* chend = &buffer[sizeof(buffer)-1];
    while (*p != ',' && !isspace(*p))
    {
        if (ch < chend)
            *ch++ = *p;
        p++;
    }
    *ch++ = '\0';

    char *d = strchr(buffer, ':');
    if (!d || !*d)
    {
        val = atof(buffer);
        val = (val != 0.0) ? 1.0 / val : 1.0;
        return p;
    }

    char *a = strdup(buffer);
    char *b = strdup(d+1);
    a[d-buffer] = '\0';
    double m = atof(a);
    double n = atof(b);
    free(a);
    free(b);
    val = (n != 0.0 && m != 0.0) ? m / n : 1.0;
    return p;
}

static void CreateDirectoryIfMissing(const char* dirname)
{
    struct stat st;
    if (stat(dirname, &st) == -1)
    {
    #ifdef _MSC_VER
        _mkdir(dirname);
    #else
        mkdir(dirname, 0700);
    #endif
    }
}

static bool CheckIfAssetFileExists(const char* name)
{
    bool ret = false;
    int namelen = strlen(name);
    if (namelen > 0)
    {
            struct stat st;
            size_t cachedNameLen = namelen + strlen("assets/.wav") + 1;
            char* cachedName = (char*)malloc(cachedNameLen);
            if (cachedName != NULL)
            {
                snprintf(cachedName, cachedNameLen, "assets/%s.wav", name);
                ret = (stat(cachedName, &st) == 0);
                free(cachedName);
            }
        }
    return ret;
}

static bool CheckIfCachedFileExists(const char* name)
{
    bool ret = false;
    int namelen = strlen(name);
    if (namelen > 0)
    {
        struct stat st;
        size_t cachedNameLen = namelen + strlen("cache/.wav") + 1;
        char* cachedName = (char*)malloc(cachedNameLen);
        if (cachedName != NULL)
        {
            snprintf(cachedName, cachedNameLen, "cache/%s.wav", name);
            ret = (stat(cachedName, &st) == 0);
            free(cachedName);
        }
    }
    return ret;
}

static bool CheckIfMusicFileExists(const char* name)
{
    bool ret = false;
    int namelen = strlen(name);
    if (namelen > 0)
    {
        struct stat st;
        size_t musicNameLen = namelen + strlen("assets/music/.ogg") + 1;
        char* musicName = (char*)malloc(musicNameLen);
        if (musicName != NULL)
        {
            snprintf(musicName, musicNameLen, "assets/music/%s.ogg", name);
            ret = (stat(musicName, &st) == 0);
            free(musicName);
        }
    }
    return ret;
}

static bool CheckIfSpeechFileExists(const char* name)
{
    bool ret = false;
    int namelen = strlen(name);
    if (namelen > 0)
    {
        struct stat st;
        size_t speechNameLen = namelen + strlen("assets/speech/.ogg") + 1;
        char* speechName = (char*)malloc(speechNameLen);
        if (speechName != NULL)
        {
            snprintf(speechName, speechNameLen, "assets/speech/%s.ogg", name);
            ret = (stat(speechName, &st) == 0);
            free(speechName);
        }
    }
    return ret;
}

void processCommand(GlobalState* data, char* line, bool verbose)
{
    bool doRecord = false;
    bool doCache = false;
    char* p = line;
    while (isspace(*p))
        p++;
    // Ignore -- comments
    if (p[0] == '-' && p[1] == '-')
        return;
    if (*p == '#')
    {
        while (*p == '#')
        {
            p++;
            if (p[0] == 't' && p[1] == '=')
            {
                double tempoChange;
                p = atof(p+2, tempoChange);
                data->fDefratio *= tempoChange;
                data->fDefpitchshift = true;
            }
            else if (p[0] == 'T' && p[1] == '=')
            {
                double tempoChange;
                p = tempo_convert(p+2, tempoChange);
                data->fDefratio *= tempoChange;
                data->fDefpitchshift = true;
            }
            else if (p[0] == 'p' && p[1] == '=')
            {
                p = atof(p+2, data->fDefpitchshift);
                data->fDefHaveRatio = true;
            }
            else if (p[0] == 'f' && p[1] == '=')
            {
                p = atof(p+2, data->fDeffrequencyshift);
                data->fDefHaveRatio = true;
            }
            else if (p[0] == 'r' && p[1] == '=' && p[2] == '0')
            {
                data->fDefdoreverse = false;
            }
            else if (p[0] == 'r' && p[1] == '=' && p[2] == '1')
            {
                data->fDefdoreverse = true;
            }
            else if (p[0] == 'r' && p[1] == 'e' && p[2] == 's' && p[3] == 'e' && p[4] == 't')
            {
                data->fDefHaveRatio = false;
                data->fDefratio = 1.0;
                data->fDefpitchshift = 0.0;
                data->fDeffrequencyshift = 1.0;
                data->fDefdoreverse = false;
            }
            if (*p == ',') p++;
        }
    }
    else if (*p == '$')
    {
        bool haveRatio = data->fDefHaveRatio;
        double ratio = data->fDefratio;
        double duration = 0.0;
        double pitchshift = data->fDefpitchshift;
        double frequencyshift = data->fDeffrequencyshift;
        bool doreverse = data->fDefdoreverse;
        // play command
        if (p[1] == 'p' && p[2] == 'l' && p[3] == 'a' && p[4] == 'y' && isspaceeol(p[5]))
        {
            // play command
            p += 5;
            while (isspace(*p))
                p++;
    play_cmd:
            // default channel is 0
            bool doloop = false;
            bool doqueue = false;
            int octaveOffset = 0;
            int channelNumber = 0;
            while (*p == '#')
            {
                p++;
                if (isdigit(*p))
                {
                    while (isdigit(*p))
                    {
                        channelNumber = (channelNumber * 10) + (*p++ - '0');
                    }
                }
                else if (p[0] == 'L' && p[1] == 'O' && p[2] == 'O' && p[3] == 'P')
                {
                    p += 4;
                    // LOOP
                    doloop = true;
                }
                else if (p[0] == 'Q' && p[1] == 'U' && p[2] == 'E' && p[3] == 'U' && p[4] == 'E')
                {
                    p += 5;
                    // QUEUE
                    doqueue = true;
                }
                else if ((p[0] == 't') && p[1] == '=')
                {
                    double tempoChange;
                    p = atof(p+2, tempoChange);
                    ratio *= tempoChange;
                    pitchshift = true;
                }
                else if ((p[0] == 'T') && p[1] == '=')
                {
                    double tempoChange;
                    p = tempo_convert(p+2, tempoChange);
                    ratio *= tempoChange;
                    pitchshift = true;
                }
                else if ((p[0] == 'D') && p[1] == '=')
                {
                    p = atof(p+2, duration); haveRatio = true;
                }
                else if ((p[0] == 'p') && p[1] == '=')
                {
                    p = atof(p+2, pitchshift); haveRatio = true;
                }
                else if ((p[0] == 'f') && p[1] == '=')
                {
                    p = atof(p+2, frequencyshift); haveRatio = true;
                }
                else if (p[0] == 'r' && (isspace(p[1]) || p[1] == ','))
                {
                    doreverse = true;
                    p++;
                }
                if (*p == ',') p++;
            }
            if (pitchshift != 0.0)
            {
                frequencyshift *= pow(2.0, pitchshift / 12);
            }
            while (isspace(*p))
                p++;
            const char* name = p;
            AudioChannelThread* act = &data->fChannels[channelNumber];
            if (data->fUseCache && !doCache && !doqueue)
            {
                // Stop any current audio if using cache and #QUEUE wasn't specified
                if (act->fCurrentCmd > 0 || (act->fCmd.fCmd != 0 && !act->fCmd.fDone))
                    sendACT_Stop(act);
            }
            if (doRecord)
            {
                while (act->fCurrentCmd > 0 || (act->fCmd.fCmd != 0 && !act->fCmd.fDone))
                {
                    Pa_Sleep(200);
                }
                memset(&act->fRecordingInfo, 0, sizeof (act->fRecordingInfo));
                CreateDirectoryIfMissing("output");
                size_t recordNameLen = strlen(name) + strlen("output/.wav") + 1;
                char* recordName = (char*)malloc(recordNameLen);
                snprintf(recordName, recordNameLen, "output/%s.wav", name);
                act->fRecording = recordName;
                act->fRecordingInfo.samplerate = SAMPLE_RATE;
                act->fRecordingInfo.channels = 1;
                act->fRecordingInfo.format = (SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_FORMAT_FLOAT);
                act->fRecordingFile = sf_open(recordName, SFM_WRITE, &act->fRecordingInfo);
                act->fRecordingPlayAtEnd = false;
            }
            else if (CheckIfMusicFileExists(name))
            {
                printf("play music : %s\n", name);
                size_t musicNameLen = strlen(name) + strlen("assets/music/.ogg") + 1;
                char* musicName = (char*)malloc(musicNameLen);
                snprintf(musicName, musicNameLen, "assets/music/%s.ogg", name);
                sendACT_PlayCache(act, musicName, doqueue, doloop);
                free(musicName);
                name = NULL;
            }
            else if (CheckIfSpeechFileExists(name))
            {
                printf("play speech : %s\n", name);
                size_t speechNameLen = strlen(name) + strlen("assets/speech/.ogg") + 1;
                char* speechName = (char*)malloc(speechNameLen);
                snprintf(speechName, speechNameLen, "assets/speech/%s.ogg", name);
                sendACT_PlayCache(act, speechName, doqueue, doloop);
                free(speechName);
                name = NULL;
            }
            else if (data->fUseCache)
            {
                if (CheckIfAssetFileExists(name))
                {
                }
                else if (!CheckIfCachedFileExists(name))
                {
                    while (act->fCurrentCmd > 0 || (act->fCmd.fCmd != 0 && !act->fCmd.fDone))
                    {
                        Pa_Sleep(200);
                    }
                    memset(&act->fRecordingInfo, 0, sizeof (act->fRecordingInfo));
                    CreateDirectoryIfMissing("cache");
                    size_t recordNameLen = strlen(name) + strlen("cache/.wav") + 1;
                    char* recordName = (char*)malloc(recordNameLen);
                    snprintf(recordName, recordNameLen, "cache/%s.wav", name);
                    act->fRecording = recordName;
                    act->fRecordingInfo.samplerate = SAMPLE_RATE;
                    act->fRecordingInfo.channels = 1;
                    act->fRecordingInfo.format = (SF_FORMAT_WAV | SF_FORMAT_PCM_16);
                    act->fRecordingFile = NULL;//sf_open(recordName, SFM_WRITE, &act->fRecordingInfo);
                    act->fRecordingPlayAtEnd = !doCache;
                }
                else
                {
                play_cache:
                    if (doCache)
                        return;

                    printf("play cache : %s\n", name);
                    size_t cacheNameLen = strlen(name) + strlen("cache/.wav") + 1;
                    char* cacheName = (char*)malloc(cacheNameLen);
                    snprintf(cacheName, cacheNameLen, "cache/%s.wav", name);
                    sendACT_PlayCache(act, cacheName, doqueue, doloop);
                    free(cacheName);
                    name = NULL;
                }
            }
            if (name != NULL)
            {
                for (RTTTLSong* song = data->fSongList; song != NULL; song = song->fNext)
                {
                    if (strcmp(song->fName, name) == 0)
                    {
                        // Play rtttl
                        int catIndex = data->fCatSelect[random(data->fCatSelectCount)];
                        int sndIndex = random(data->fSnippetCounts[catIndex]);

                        printf("#channel : %d octaveOffset : %d doloop : %d\n", channelNumber, octaveOffset, doloop);
                        sendACT_PlayRTTTL(act, catIndex, sndIndex, song->fRTTTL, octaveOffset, doloop, doqueue);
                        name = NULL;
                        break;
                    }
                }
            }
            if (name != NULL)
            {
                if (*name == 0)
                {
                    int cat = data->fCatSelect[random(data->fCatSelectCount)];
                    for (SoundCategory* catList = data->fCategoryList; catList != NULL; catList = catList->fNext)
                    {
                        if (catList->fCatIndex == cat)
                        {
                            name = catList->fName;
                            // printf("FOUND CAT : %s\n", name);
                            break;
                        }
                    }
                }
                for (SoundCategory* catList = data->fCategoryList; name != NULL && catList != NULL; catList = catList->fNext)
                {
                    if (strcmp(name, catList->fName) == 0)
                    {
                        SoundSnippet* snippet = &catList->fSnippets[random(catList->fCount)];
                        strcpy(line, snippet->fName);
                        char*  le = line + strlen(line);
                        while (le > line)
                        {
                            char ch = *le;
                            *le-- = 0;
                            if (ch == '-')
                                break;
                        }
                        name = line;
                        if (act->fRecording != NULL)
                        {
                            char* recordName = NULL;
                            free(act->fRecording);
                            act->fRecording = NULL;
                            if (doRecord)
                            {
                                size_t recordNameLen = strlen(name) + strlen("output/.wav") + 1;
                                recordName = (char*)malloc(recordNameLen);
                                snprintf(recordName, recordNameLen, "output/%s.wav", name);
                            }
                            else if (data->fUseCache)
                            {
                                if (!CheckIfCachedFileExists(name))
                                {
                                    size_t recordNameLen = strlen(name) + strlen("cache/.wav") + 1;
                                    recordName = (char*)malloc(recordNameLen);
                                    snprintf(recordName, recordNameLen, "cache/%s.wav", name);
                                }
                                else
                                {
                                    act->fRecordingPlayAtEnd = false;
                                    goto play_cache;
                                }
                            }
                            act->fRecording = recordName;
                        }
                        // printf("FOUND : %s\n", name);
                        break;
                    }
                }
            }
            int namelen = (name != NULL) ? strlen(name) : 0;
            SoundCategory* catList = data->fCategoryList;
            while (name != NULL && catList != NULL)
            {
                for (size_t i = 0; i < catList->fCount; i++)
                {
                    SoundSnippet* snippet = &catList->fSnippets[i];
                    if (strcmp(snippet->fName, name) == 0)
                    {
                        printf("#channel : %d doloop : %d ratio : %g pitchshift : %g frequencyshift : %g\n", channelNumber, doloop, ratio, pitchshift, frequencyshift);
                        sendACT_PlaySnippet(act, catList->fCatIndex, i, i, octaveOffset, doloop, doqueue, ratio, pitchshift, frequencyshift, doreverse);
                        name = NULL;
                        break;
                    }
                    else if (strncmp(snippet->fName, name, namelen) == 0 && snippet->fName[namelen] == '-')
                    {
                        int sndIndex = i;
                        int sndIndexEnd = i;
                        while (i < catList->fCount)
                        {
                            SoundSnippet* snippet = &catList->fSnippets[i];
                            if (strncmp(snippet->fName, name, namelen) != 0 || snippet->fName[namelen] != '-')
                                break;
                            sndIndexEnd = i++;
                        }
                        printf("%d:[%d-%d]\n", (int)catList->fCatIndex, sndIndex, sndIndexEnd);
                        printf("#channel : %d doloop : %d ratio : %g pitchshift : %g frequencyshift : %g\n", channelNumber, doloop, ratio, pitchshift, frequencyshift);
                        sendACT_PlaySnippet(act, catList->fCatIndex, sndIndex, sndIndexEnd, octaveOffset, doloop, doqueue, ratio, pitchshift, frequencyshift, doreverse);
                        name = NULL;
                        break;
                    }
                }
                catList = catList->fNext;
            }
            if (name != NULL)
            {
                if (act->fRecording != NULL)
                    free(act->fRecording);
                act->fRecording = NULL;
                printf("No such sound : %s\n", name);
            }
        }
        else if (p[1] == 'p' && isspaceeol(p[2]))
        {
            // p (play) command
            p += 2;
            while (isspace(*p))
                p++;
            doRecord = false;
            goto play_cmd;
        }
        // cache command
        else if (p[1] == 'c' && p[2] == 'a' && p[3] == 'c' && p[4] == 'h' && p[5] == 'e' && isspaceeol(p[6]))
        {
            p += 6;
            while (isspace(*p))
                p++;
            doCache = true;
            goto play_cmd;
        }
        // record command
        else if (p[1] == 'r' && p[2] == 'e' && p[3] == 'c' && p[4] == 'o'  && p[5] == 'r' && p[6] == 'd' && isspaceeol(p[7]))
        {
            // play command
            p += 7;
            while (isspace(*p))
                p++;
            doRecord = true;
            goto play_cmd;
        }
        else if (p[1] == 'r' && isspaceeol(p[2]))
        {
            // r (record) command
            p += 2;
            while (isspace(*p))
                p++;
            doRecord = true;
            goto play_cmd;
        }
        else if (p[1] == 's' && p[2] == 't' && p[3] == 'o' && p[4] == 'p' && p[5] == 'a' && p[6] == 'l' && p[7] == 'l' && isspaceeol(p[8]))
        {
            // stopall command
            p += 8;
    stop_all:
            printf("STOPALL\n");
            for (int i = 0; i < data->fChannelCount; i++)
            {
                AudioChannelThread* act = &data->fChannels[i];
                sendACT_Stop(act);
            }
        }
        else if (p[1] == 's' && p[2] == 't' && p[3] == 'o' && p[4] == 'p' && isspaceeol(p[5]))
        {
            // stop command
            p += 5;
    stop_channel:
            while (isspace(*p))
                p++;
            int channelNumber = 0;
            if (*p == '#')
            {
                // Channel number
                channelNumber = atoi(++p);
            }
            AudioChannelThread* act = &data->fChannels[channelNumber];
            sendACT_Stop(act);
        }
        else if (p[1] == 'q' && p[2] == 'u' && p[3] == 'i' && p[4] == 't' && isspaceeol(p[5]))
        {
            // quit command
            p += 5;
            while (isspace(*p))
                p++;
        doquit:
            for (int i = 0; i < data->fChannelCount; i++)
            {
                AudioChannelThread* act = &data->fChannels[i];
                act->fQuit = true;
                sendACT_Stop(act);
            }
            data->fQuit = true;
            return;
        }
        else if (p[1] == 'q' && isspaceeol(p[2]))
        {
            // quit command
            p += 2;
            while (isspace(*p))
                p++;
            goto doquit;
        }
        else if (p[1] == 's' && p[2] == 'a' && isspaceeol(p[3]))
        {
            // sa (stopall) command
            p += 3;
            goto stop_all;
        }
        else if (p[1] == 's' && isspaceeol(p[2]))
        {
            // s (stop) command
            p += 3;
            goto stop_channel;
        }
        // else if (p[1] == 'i' && p[2] == 'n' && p[3] == 'c' && p[4] == 'l' && p[5] == 'u' && p[6] == 'd' && p[7] == 'e' && isspaceeol(p[8]))
        // {
        //     // include command
        //     p += 8;
        //     while (isspace(*p))
        //         p++;
        //     const char* name = p;
        //     FILE* file = fopen(name, "r");
        //     if (file != NULL)
        //     {
        //         processCommands(data, file, verbose);
        //         fclose(file);
        //     }
        // }
        // else if (p[1] == 'i' && p[2] == 'n' && p[3] == 'c' && isspaceeol(p[4]))
        // {
        //     // inc (include) command
        //     p += 4;
        //     while (isspace(*p))
        //         p++;
        //     const char* name = p;
        //     FILE* file = fopen(name, "r");
        //     if (file != NULL)
        //     {
        //         processCommands(data, file, verbose);
        //         fclose(file);
        //     }
        // }
        // marc command
        else if (p[1] == 'm' && p[2] == 'a' && p[3] == 'r' && p[4] == 'c')
        {
            p += 5;
            while (isspace(*p))
                p++;
            const char* marc_cmd = p;
            const char* infinite_cmd = "";
            while (*p && *p != ',')
                p++;
            if (*p == ',')
                *p ++= 0;
            if (*p == '$')
            {
                // full command
                infinite_cmd = strdup(p);
            }
            else if (*p != 0)
            {
                // short hand for play command
                int cmdlen = strlen(p) + 7;
                char* cmd = (char*)malloc(cmdlen);
                snprintf(cmd, cmdlen + 7, "$play %s", p);
                infinite_cmd = cmd;
            }
            if (*infinite_cmd != 0)
            {
                MarcCommand* marc = new MarcCommand;
                marc->fMarc = strdup(marc_cmd);
                marc->fCmd = infinite_cmd;
                marc->fNext = data->fMarcList;
                data->fMarcList = marc;

                if (verbose)
                {
                    printf("[%s]: %s\n", marc_cmd, infinite_cmd);
                }
            }
        }
        else if (p[1] == 'c' && p[2] == 'o' && p[3] == 'n' && p[4] == 'f' && p[5] == 'i' && p[6] == 'g')
        {
            p += 7;
            while (isspace(*p))
                p++;
            const char* config_cmd = p;
            while (*p && *p != '=')
                p++;
            if (*config_cmd == '\0')
            {
                data->fConfigStealth = false;
            }
            else if (strcmp(config_cmd, "stealth") == 0)
            {
                data->fConfigStealth = true;
            }
        }
    }
    else if (data->fConfigStealth)
    {
        // add stealth config if in stealth mode
        const char* start = p;
        while (*p != '\0' && *p != '=')
            p++;
        if (*p == '=')
        {
            int namelen = p - start;
            char* name = (char*)malloc(namelen + 1);
            strncpy(name, start, namelen);
            name[namelen] = 0;

            char* value = p + 1;
            StealthVar* var = new StealthVar;
            var->fName = name;
            var->fValue = strdup(value);
            var->fNext = NULL;
            if (data->fStealthVarTail != NULL)
                data->fStealthVarTail->fNext = var;
            if (data->fStealthVarHead == NULL)
                data->fStealthVarHead = var;
            data->fStealthVarTail = var;
            if (strcmp(name, "sb") == 0)
                data->fStealthSoundBankCount++;
        }
    }
    else
    {
        // define rtttl sequence
        const char* start = p;
        while (*p != '\0' && *p != ':')
            p++;
        if (*p == ':')
        {

        }
        if (p != start)
        {
            int namelen = p - start;
            char* name = (char*)malloc(namelen + 1);
            strncpy(name, start, namelen);
            name[namelen] = 0;

            RTTTLSong* song = new RTTTLSong;
            song->fName = name;
            song->fRTTTL = strdup(start);
            song->fNext = data->fSongList;
            data->fSongList = song;
        }
    }
}

#ifndef GETLINE_MINSIZE
#define GETLINE_MINSIZE 16
#endif

static int printfFD(int fd, const char* fmt, ...)
{
    char buf[4096];
    va_list argp;
    va_start(argp, fmt);
    vsnprintf(buf, sizeof(buf), fmt, argp);
    va_end(argp);
    return write(fd, buf, strlen(buf));
}

static ssize_t readFD(int fd, char* buf, size_t len)
{
    size_t numbytes = 0;
    do
    {
        int nfds;
        unsigned char c;
        fd_set rfds;

        // Use select in case descriptor is non-blocking
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
    reselect:
        nfds = select(fd + 1, &rfds, NULL, NULL, NULL);
        if (nfds == 0)
        {
            break;
        }
        else if (nfds == -1)
        {
            if (errno == EINTR)
            {
                goto reselect;
            }
            else
            {
                return 1;
            }
        }
        ssize_t rb = read(fd, buf, len);
        if (rb == -1)
            break;
        numbytes += rb;
        buf += rb;
        len -= rb;
    }
    while (len != 0);
    return (numbytes != 0) ? numbytes : EOF;
}

static int getdelimFD(char** lineptr, size_t* n, char delim, int fd)
{
    int ch;
    int i = 0;
    char free_on_err = 0;
    char* p;

    errno = 0;
    if (lineptr == NULL || n == NULL || fd == -1)
    {
        errno = EINVAL;
        return -1;
    }

    if (*lineptr == NULL)
    {
        *n = GETLINE_MINSIZE;
        *lineptr = (char*)malloc(sizeof(char) * (*n));
        if (*lineptr == NULL)
        {
            errno = ENOMEM;
            return -1;
        }
        free_on_err = 1;
    }

    for (;;)
    {
        char c;

        ch = readFD(fd, &c, 1);
        ch = (ch != 1) ? EOF : c;
        while (i >= (*n) - 2)
        {
            *n *= 2;
            p = (char*)realloc(*lineptr, sizeof(char) * (*n));
            if (p == NULL)
            {
                if (free_on_err)
                    free(*lineptr);
                errno = ENOMEM;
                return -1;
            }
            *lineptr = p;
        }
        if (ch == EOF)
        {
            if (i == 0)
            {
                if (free_on_err)
                    free(*lineptr);
                return -1;
            }
            (*lineptr)[i] = '\0';
            return i;
        }

        if (ch == delim)
        {
            (*lineptr)[i] = delim;
            (*lineptr)[i + 1] = '\0';
            return i + 1;
        }
        (*lineptr)[i++] = (char)ch;
    }
    return -1;
}

static int getlineFD(char** lineptr, size_t* n, int fd)
{
    return getdelimFD(lineptr, n, '\n', fd);
}

static int getlineFP(char** lineptr, size_t* n, FILE* fp)
{
    int ch;
    int i = 0;
    char free_on_err = 0;
    char* p;

    errno = 0;
    if (lineptr == NULL || n == NULL || fp == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (*lineptr == NULL)
    {
        *n = GETLINE_MINSIZE;
        *lineptr = (char*)malloc(sizeof(char) * (*n));
        if (*lineptr == NULL)
        {
            errno = ENOMEM;
            return -1;
        }
        free_on_err = 1;
    }

    for (i = 0; ; i++)
    {
        ch = fgetc(fp);
        while (i >= (*n) - 2)
        {
            *n *= 2;
            p = (char*)realloc(*lineptr, sizeof(char) * (*n));
            if (p == NULL)
            {
                if (free_on_err)
                    free(*lineptr);
                errno = ENOMEM;
                return -1;
            }
            *lineptr = p;
        }
        if (ch == EOF)
        {
            if (i == 0)
            {
                if (free_on_err)
                    free(*lineptr);
                return -1;
            }
            (*lineptr)[i] = '\0';
            return i;
        }

        if (ch == '\n')
        {
            (*lineptr)[i] = '\n';
            (*lineptr)[i + 1] = '\0';
            return i + 1;
        }
        (*lineptr)[i] = (char)ch;
    }
}

void processCommands(GlobalState* data, FILE* fd, bool verbose)
{
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while (!data->fQuit && (linelen = getlineFP(&line, &linecap, fd)) > 0)
    {
        // Remove trailing crlf
        while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
            line[--linelen] = '\0';
        processCommand(data, line, verbose);
    }
    free(line);
}

static void threadAudioChannelLoop(void* arg)
{
    AudioChannelThread* act = (AudioChannelThread*)arg;
    PaError err;
    PaStreamParameters outputParameters;

    act->fSourceSoundIndex = 0;
    act->fChirpCountMax = random(7, 3);
    act->fLongCountMax = random(4, 1);
    act->fRandom = false;

    outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
    if (outputParameters.device == paNoDevice)
    {
        fprintf(stderr, "Error: No default output device.\n");
        exit(1);
    }
    outputParameters.channelCount = 1;       /* stereo output */
    outputParameters.sampleFormat = paFloat32; /* 32 bit floating point output */
#if defined(__ARM_ARCH)
    // Raspberry Pi needs higher latency
    outputParameters.suggestedLatency = 0.5;
#else
    outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
#endif
    outputParameters.hostApiSpecificStreamInfo = NULL;

    err = Pa_OpenStream(
              &act->fStream,
              NULL, /* no input */
              &outputParameters,
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,
              paClipOff|paDitherOff,
              //paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              audioChannelCallback,
              act);
    if (err != paNoError) goto error;

    err = Pa_SetStreamFinishedCallback(act->fStream, &StreamFinished);
    if (err != paNoError) goto error;

    while (!act->fQuit)
    {
#ifdef _MSC_VER
        ::WaitForSingleObject(act->fSem, INFINITE);
#elif defined(__APPLE__)
        kern_return_t status;
        // do
        // {
            while (((status = semaphore_wait(act->fSem)) == -1) && errno == EINTR)
                ;
        // }
        // while (act->fCurrentCmd == 0);
#else
        int status;
        do
        {
            status = sem_wait(act->fSem);
        } while (status == -1 && errno == EINTR);
#endif
        if (act->fQuit)
        {
            break;
        }
        act->fCmd = act->fCmdStack[0];
        if (act->fCurrentCmd > 0)
        {
            memcpy(&act->fCmdStack[0], &act->fCmdStack[1], (act->fCurrentCmd-1) * sizeof(act->fCmd));
            act->fCurrentCmd--;
        }
        else
        {
            memset(act->fCmdStack, '\0', sizeof(act->fCmd));
        }
        bool startedRecording = (act->fRecording != NULL);
        if (startedRecording && act->fRecordingFile == NULL)
        {
            act->fRecordingFile = sf_open(act->fRecording, SFM_WRITE, &act->fRecordingInfo);            
        }
        switch (act->fCmd.fCmd)
        {
            case 1:
            {
                act->fDone = false;
                act->fZeroPadCount = 0;
                act->fSilenced = false;
                act->fInterrupt = false;
                act->fNext = false;
                err = (act->fRecording == NULL) ? Pa_StartStream(act->fStream) : paNoError;
                if (err != paNoError) goto error;
                do
                {
                    playRTTTLSequence(act, act->fCmd.fID, act->fCmd.fOctaveOffset);
                }
                while (!act->fInterrupt && act->fCmd.fLooping);
                err = (act->fRecording == NULL) ? Pa_StopStream(act->fStream) : paNoError;
                free(act->fCmd.fID);
                act->fCmd.fID = NULL;
                act->fCmd.fCmd = 0;
                if (err != paNoError) goto error;
                break;
            }
            case 2:
            {
                act->fCmd.fSndIndex = act->fCmd.fSndIndexStart;
                err = (act->fRecording == NULL) ? Pa_StartStream(act->fStream) : paNoError;
                if (err != paNoError) goto error;
                for (;;)
                {
                    SoundSnippet* sourceSnippet = &act->fData->fSnippets[act->fCmd.fCatIndex][act->fCmd.fSndIndex];
                    AudioFile<float>* sourceAudio = sourceSnippet->fAudio;

                    act->fDone = false;
                    act->fZeroPadCount = 0;
                    act->fSilenced = false;
                    act->fInterrupt = false;
                    act->fNext = false;
                    StreamPlayTime(act, double(sourceAudio->samples[0].size()) / 44.1 * act->fCmd.fRatio);
                    if (act->fInterrupt)
                        break;
                    if (act->fCmd.fSndIndex < act->fCmd.fSndIndexEnd)
                    {
                        act->fCmd.fSndIndex++;
                        act->fZeroPadCount = ~0;
                        // Pa_Sleep(random(40, 20));
                        act->fDone = true;
                        while (!act->fSilenced)
                        {
                            StreamPlayTime(act, 10);
                        }
                    }
                    else if (act->fCmd.fLooping)
                    {
                        act->fCmd.fSndIndex = act->fCmd.fSndIndexStart; 
                    }
                    else
                    {
                        break;
                    }
                }
                act->fCatIndex = 0;
                act->fSnippetIndex = 0;
                act->fRepeatSound = false;

                err = (act->fRecording == NULL) ? Pa_StopStream(act->fStream) : paNoError;
                if (err != paNoError) goto error;

                act->fCmd.fCmd = 0;
                break;
            }
            case 3:
            {
            play_recording:
                act->fInterrupt = false;
                do
                {
                    act->fDone = false;
                    SF_INFO* sfinfo = &act->fCacheInfo;
                    memset(sfinfo, '\0', sizeof(*sfinfo));
                    if ((act->fCacheFile = sf_open(act->fCmd.fID, SFM_READ, sfinfo)) == NULL)
                    {
                        printf("Unable to open cache file %s.\n", act->fCmd.fID);
                        sf_perror(NULL);
                    }
                    if (sfinfo->samplerate == SAMPLE_RATE && sfinfo->frames / sfinfo->samplerate < 0x7FFFFFFF)
                    {
                        Pa_StartStream(act->fStream);
                        act->fZeroPadCount = 0;
                        act->fSilenced = false;
                        double duration = (double(sfinfo->frames) / sfinfo->samplerate);
                    #if defined(__ARM_ARCH)
                        if (duration > 5)
                            duration = 1;
                    #else
                        if (duration > 2)
                            duration = 0.5;
                    #endif
                        while (!act->fInterrupt && !act->fDone)
                        {
                            Pa_Sleep(duration * 1000);
                        }
                        // printf("Done?\n");
                        err = Pa_StopStream(act->fStream);
                    }
                    else
                    {
                        printf("Unsupported file format or sample rate %s.\n", act->fCmd.fID);
                    }
                    sf_close(act->fCacheFile);
                }
                while (!act->fInterrupt && act->fCmd.fLooping);
                free(act->fCmd.fID);
                act->fCacheFile = NULL;
                if (err != paNoError) goto error;
                act->fCmd.fID = NULL;
                act->fCmd.fCmd = 0;
                break;
            }
            case 4:
            {
                // printf("Stream should have stopped\n");
                break;
            }
            default:
            {
                printf("Unexpected command\n");
                break;
            }
        }
        if (startedRecording && act->fRecording != NULL)
        {
            sf_write_sync(act->fRecordingFile);
            sf_close(act->fRecordingFile);
            act->fRecordingFile = NULL;
            if (act->fRecordingPlayAtEnd)
            {
                printf("fRecordingPlayAtEnd : %s\n", act->fRecording);
                memset(&act->fCmd, '\0', sizeof(act->fCmd));
                act->fCmd.fCmd = 3;
                act->fCmd.fID = act->fRecording;
                act->fRecording = NULL;
                goto play_recording;
            }
            free(act->fRecording);
            act->fRecording = NULL;
        }
        act->fCmd.fCmd = 0;
    }
    err = Pa_CloseStream(act->fStream);
error:
    if (err)
    {
        fprintf(stderr, "Error: Portaudio error #%d\n", err);
        exit(1);
    }
}

#define MAXPENDING 5      /* Maximum outstanding connection requests */
#define RCVBUFSIZE 1024   /* Size of receive buffer */

static void DieWithError(const char *errorMessage)
{
    perror(errorMessage);
    exit(1);
}

static const char* sBroadcastIP;

static unsigned int getMyBroadcastIP()
{
    if (sBroadcastIP == NULL)
    {
        char buf[1024];
        int err = gethostname(buf, sizeof(buf));
        if (err != 0)
        {
            DieWithError("Error getting local hostname.");
        }
        struct hostent* hostInfo = gethostbyname(buf);
        if (hostInfo == NULL || hostInfo->h_addr == NULL)
        {
            DieWithError("Unable to resolve local hostname.");
        }
        unsigned int addr = ntohl(*(unsigned int*)hostInfo->h_addr);
        addr = htonl(addr|0xFF);
        return addr;
    }
    unsigned int addr = inet_addr(sBroadcastIP);
    return addr;
}

#if defined(__ARM_ARCH)
const char* amixerDevice = "Speaker";
#else
const char* amixerDevice = "PCM";
#endif

static void MARC_eventHandler(const char* topic_name, const uint8_t* msg, size_t len, void* arg)
{
    GlobalState* data = (GlobalState*)arg;
    json_tokener* tok = json_tokener_new();
    json_object* jobj = json_tokener_parse_ex(tok, (const char*)msg, len);
    if (jobj != NULL)
    {
        json_object* jcmd = json_object_object_get(jobj, "cmd");
        if (jcmd != NULL)
        {
            const char* marccmd = json_object_get_string(jcmd);
            printf("MARC : \"%s\"\n", marccmd);
            if (marccmd != NULL)
            {
                for (MarcCommand* marc = data->fMarcList; marc != NULL; marc = marc->fNext)
                {
                    if (strcmp(marc->fMarc, marccmd) == 0)
                    {
                        char* bleepcmd = strdup(marc->fCmd);
                        processCommand(data, bleepcmd, false);
                        free(bleepcmd);
                        break;
                    }
                }
            #if defined(__linux)
                char cmdbuf[256];
                if (strcmp(marccmd, "$+") == 0)
                {
                    snprintf(cmdbuf, sizeof(cmdbuf), "amixer set %s 2dB+", amixerDevice);
                    system(cmdbuf);
                }
                else if (strcmp(marccmd, "$m") == 0)
                {
                    snprintf(cmdbuf, sizeof(cmdbuf), "amixer set %s 50%", amixerDevice);
                    system(cmdbuf);
                }
                else if (strcmp(marccmd, "$-") == 0)
                {
                    snprintf(cmdbuf, sizeof(cmdbuf), "amixer set %s 2dB-", amixerDevice);
                    system(cmdbuf);
                }
            #endif
            }
        }
        json_object_put(jobj);
    }
}

static void BleepBox_eventHandler(const char* topic_name, const uint8_t* msg, size_t len, void* arg)
{
    GlobalState* data = (GlobalState*)arg;
    json_tokener* tok = json_tokener_new();
    json_object* jobj = json_tokener_parse_ex(tok, (const char*)msg, len);
    if (jobj != NULL)
    {
        json_object* jcmd = json_object_object_get(jobj, "cmd");
        if (jcmd != NULL)
        {
            const char* bleepcmd = json_object_get_string(jcmd);
            printf("BleepBox : \"%s\"\n", bleepcmd);
            if (bleepcmd != NULL)
            {
                char* bleepcmddup = strdup(bleepcmd);
                processCommand(data, bleepcmddup, false);
                free(bleepcmddup);
            }
        }
        json_object_put(jobj);
    }
}

static OSTHREAD_HANDLE
createOSThread(void (*threadProc)(void*), void* arg)
{
#ifdef _MSC_VER
    DWORD threadID;
    HANDLE thread = ::CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)threadProc, arg, CREATE_SUSPENDED, &threadID);
    ::ResumeThread(thread);
#else
    pthread_t thread;
    pthread_create(&thread, NULL, (pthread_start_proc_t)threadProc, arg);
#endif
    return thread;
}

static void threadMarcduinoServerLoop(void* arg)
{
    GlobalState* data = (GlobalState*)arg;
    int servSock;
    int clntSock;
    struct sockaddr_in echoServAddr;
    struct sockaddr_in echoClntAddr;
    unsigned short echoServPort = 2000;
#ifdef _MSC_VER
    int clntLen;
#else
    unsigned int clntLen;
#endif

    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        DieWithError("socket() failed");

    int enable = 1;
#ifdef _MSC_VER
    if (setsockopt(servSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&enable, sizeof(int)) < 0)
        DieWithError("setsockopt(SO_REUSEADDR) failed");
#else
    if (setsockopt(servSock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
        DieWithError("setsockopt(SO_REUSEADDR) failed");
#endif

    memset(&echoServAddr, 0, sizeof(echoServAddr));
    echoServAddr.sin_family = AF_INET;
    echoServAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    echoServAddr.sin_port = htons(echoServPort);

    if (bind(servSock, (struct sockaddr *) &echoServAddr, sizeof(echoServAddr)) < 0)
        DieWithError("bind() failed");

    if (listen(servSock, MAXPENDING) < 0)
        DieWithError("listen() failed");

    for (;;)
    {
        clntLen = sizeof(echoClntAddr);

        if ((clntSock = accept(servSock, (struct sockaddr *) &echoClntAddr,  &clntLen)) < 0)
            DieWithError("accept() failed");

        printf("Handling client %s\n", inet_ntoa(echoClntAddr.sin_addr));

        const char* onlinemsg = "{ \"cmd\": \"$122\" }";
        smq_publish_hash("MARC", (uint8_t*)onlinemsg, strlen(onlinemsg));

        char msgBuffer[RCVBUFSIZE+1];
        int recvMsgSize;

        if ((recvMsgSize = recv(clntSock, msgBuffer, RCVBUFSIZE, 0)) < 0)
            DieWithError("recv() failed");

        msgBuffer[RCVBUFSIZE] = '\0';
        while (recvMsgSize > 0)
        {
            if ((send(clntSock, msgBuffer, recvMsgSize, 0)) < 0)
                DieWithError("recv() failed");

            for (char* ch = msgBuffer; ch < msgBuffer + recvMsgSize; ch++)
            {
                if (*ch == 0x0D)
                    *ch = '\0';
            }

            char *msg = msgBuffer;
            while (msg < msgBuffer + recvMsgSize)
            {
                int msglen = strlen(msg);
                if (msglen > 0)
                {
                    json_object* jobj = json_object_new_object();
                    json_object_object_add(jobj, "cmd", json_object_new_string(msg));
                    const char* jsonmsg = json_object_to_json_string(jobj);
                    // printf("jsonmsg : \"%s\"\n", jsonmsg);
                    smq_publish_hash("MARC", (uint8_t*)jsonmsg, strlen(jsonmsg));
                    json_object_put(jobj);
                }
                msg += msglen + 1;
            }

            /* See if there is more data to receive */
            if ((recvMsgSize = recv(clntSock, msgBuffer, RCVBUFSIZE, 0)) < 0)
                DieWithError("recv() failed");
            msgBuffer[RCVBUFSIZE] = '\0';
        }
    #ifdef _MSC_VER
        closesocket(clntSock);    /* Close client socket */
    #else
        close(clntSock);    /* Close client socket */
    #endif

        const char* disconnectemsg = "{ \"cmd\": \"$120\" }";
        smq_publish_hash("MARC", (uint8_t*)disconnectemsg, strlen(disconnectemsg));
    }
    /* NOT REACHED */
}

static void generateStealthConfig(GlobalState* data, const char* confname)
{
    CreateDirectoryIfMissing("cache");
    FILE* fd = fopen(confname, "wb+");
    if (fd == NULL)
    {
        fprintf(stderr, "Unable to generate Stealth configuration file : %s\n", confname);
        return;
    }
    for (StealthVar* var = data->fStealthVarHead; var != NULL; var = var->fNext)
    {
        const char* pch;
        if ((pch = strchr(var->fValue, '$')) != NULL)
        {
            // Substitute sound bank reference
            for (int i = 0; i < data->fStealthSoundBankCount; i++)
            {
                StealthSoundBank* sb = &data->fStealthSoundBank[i];
                if (strcmp(sb->fName, pch+1) == 0)
                {
                    fprintf(fd, "%s=%.*s%d\r\n", var->fName, int(pch - var->fValue), var->fValue, sb->fIndex);
                    pch = NULL;
                    break;
                }
            }
            if (pch != NULL)
            {
                int auxStringIdx = 0;
                // Look for auxiliary string references
                for (StealthVar* var2 = data->fStealthVarHead; var2 != NULL; var2 = var2->fNext)
                {
                    if (strcmp(var2->fName, "a") == 0)
                    {
                        ++auxStringIdx;
                        if (strcmp(var2->fValue, pch+1) == 0)
                        {
                            fprintf(fd, "%s=%.*s%d\r\n", var->fName, int(pch - var->fValue), var->fValue, auxStringIdx);
                            pch = NULL;
                            break;
                        }
                    }
                }
            }
            if (pch != NULL)
            {
                printf("Undefined soundbank or auxiliary string: %s\r\n", pch);
                fprintf(fd, "%s=\r\n", var->fName);
            }
        }
        else if (strcmp(var->fName, "sb") == 0)
        {
            int soundCount = 0;
            for (int i = 0; i < data->fStealthSoundBankCount; i++)
            {
                StealthSoundBank* sb = &data->fStealthSoundBank[i];
                if (strcmp(sb->fName, var->fValue) == 0)
                {
                    soundCount = sb->fCount;
                    break;
                }
            }
            fprintf(fd, "%s=%s,%d\r\n", var->fName, var->fValue, soundCount);
        }
        else
        {
            fprintf(fd, "%s=%s\r\n", var->fName, var->fValue);
        }
    }
    fclose(fd);
}

static void threadVMusicServerLoop(void* arg)
{
    GlobalState* data = (GlobalState*)arg;
    data->fVMusicFD = -1;
    while (data->fVMusicFD == -1)
    {
        data->fVMusicFD = smq_open_serial(data->fVMusicPort, 9600);
        printf("smq_open(\"%s\") : %d\n", data->fVMusicPort, data->fVMusicFD);
        if (data->fVMusicFD != -1)
        {
            char *line = NULL;
            size_t linecap = 0;
            ssize_t linelen;
            bool skipPrompt = false;
            char currentDir[4096];
            FILE* currentFile = NULL;

            *currentDir = '\0';
            printfFD(data->fVMusicFD, "\r");
            printfFD(data->fVMusicFD, "Ver 03.68VMSC1F On-Line:\r");
            printfFD(data->fVMusicFD, "Device Detected P2\r");
            printfFD(data->fVMusicFD, "No Upgrade\r");
            generateStealthConfig(data, "cache/.config.txt");
            while (!data->fQuit)
            {
                if (!skipPrompt)
                    printfFD(data->fVMusicFD, "D:\\>");
                skipPrompt = false;
                while ((linelen = getdelimFD(&line, &linecap, '\r', data->fVMusicFD)) > 0)
                {
                    struct stat st;
                    char path[4096];
                    // Remove trailing crlf
                    while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
                        line[--linelen] = '\0';
                    printf("[VMUSIC] %s\n", line);
                    char* cmd = line;
                    if (strncmp(cmd, "RDF ", 4) == 0)
                    {
                        // Read From File
                        cmd += 4;
                        size_t len = (unsigned(cmd[0]) << 24) | (unsigned(cmd[1]) << 16) |
                                        (unsigned(cmd[2]) << 8) | (unsigned(cmd[3]) << 0);
                        cmd += 4;
                        if (currentFile != NULL && len < sizeof(path))
                        {
                            ssize_t numBytes = fread(path, 1, len, currentFile);
                            if (numBytes < 0) 
                                numBytes = 0;
                            while (numBytes < len)
                            {
                                path[numBytes++] = 0;
                            }
                            write(data->fVMusicFD, path, numBytes);
                        }
                        else
                        {
                            // File not found
                            printfFD(data->fVMusicFD, "Command Failed\r");
                        }
                    }
                    else if (strncmp(cmd, "DIR ", 4) == 0)
                    {
                        // Directory
                        cmd += 4;
                        for (char* ch = cmd; *ch; ch++)
                            *ch = tolower(*ch);
                        snprintf(path, sizeof(path), "%s%s", currentDir, cmd);
                        if (strcmp(path, "config.txt") == 0 && stat("cache/.config.txt", &st) == 0)
                        {
                            if (S_ISDIR(st.st_mode))
                            {
                                // Directory listing not supported
                                printfFD(data->fVMusicFD, "Command Failed\r");
                            }
                            else
                            {
                                uint_t fileSize = uint_t(st.st_size);
                                unsigned char b1 = ((fileSize>>0) & 0xFF);
                                unsigned char b2 = ((fileSize>>8) & 0xFF);
                                unsigned char b3 = ((fileSize>>16) & 0xFF);
                                unsigned char b4 = ((fileSize>>24) & 0xFF);
                                snprintf(path, sizeof(path), "\r%s %c%c%c%c\r", "CONFIG.TXT", b1, b2, b3, b4);
                                write(data->fVMusicFD, path, strlen(cmd)+7);
                                skipPrompt = true;
                            }
                        }
                        else
                        {
                            // File not found
                            printfFD(data->fVMusicFD, "Command Failed\r");
                        }
                    }
                    else if (strncmp(cmd, "OPR ", 4) == 0)
                    {
                        // Open File for Read
                        cmd += 4;
                        for (char* ch = cmd; *ch; ch++)
                            *ch = tolower(*ch);
                        snprintf(path, sizeof(path), "%s%s", currentDir, cmd);
                        if (currentFile != NULL)
                            fclose(currentFile);
                        if (strcmp(path, "config.txt") == 0 && stat("cache/.config.txt", &st) == 0)
                        {
                            currentFile = fopen("cache/.config.txt", "rb");
                        }
                    }
                    else if (strncmp(cmd, "CLF ", 4) == 0)
                    {
                        // Close File
                        if (currentFile != NULL)
                        {
                            fclose(currentFile);
                            currentFile = NULL;
                        }
                    }
                    else if (strncmp(cmd, "CD ", 3) == 0)
                    {
                        // Change Directory
                        cmd += 3;
                        if (strcmp(cmd, "..") == 0)
                        {
                            // Only support one level nesting
                            if (*currentDir != '\0')
                                *currentDir = '\0';
                            else
                                printfFD(data->fVMusicFD, "Command Failed\r");
                        }
                        else
                        {
                            for (char* ch = cmd; *ch; ch++)
                                *ch = tolower(*ch);
                            snprintf(path, sizeof(path), "%s%s\n", currentDir, cmd);
                            snprintf(currentDir, sizeof(currentDir), "%s/", cmd);
                        }
                    }
                    else if (strcmp(cmd, "VST") == 0)
                    {
                        // Silence
                        const char* silencecmd = "{ \"cmd\": \"$sa\" }";
                        smq_publish_hash("BLEEP", (uint8_t*)silencecmd, strlen(silencecmd));
                    }
                    else if (strncmp(cmd, "VSV ", 4) == 0)
                    {
                        // Set Volume - vmusic specifies a range of 0-0xFE. Stealth only uses 0-100
                        cmd += 4;
                        int volume = unsigned(cmd[0]);
                        volume = ((100 - volume) * 100) / 100;
                    #if defined(__linux)
                        snprintf(path, sizeof(path), "amixer set %s %d%%", amixerDevice, volume);
                        system(path);
                    #endif
                    }
                    else if (strncmp(cmd, "VPF ", 4) == 0)
                    {
                        // Play File
                        cmd += 4;
                        // Search for matching sound
                        for (int sbi = 0; sbi < data->fStealthSoundBankCount; sbi++)
                        {
                            StealthSoundBank* sb = &data->fStealthSoundBank[sbi];
                            for (int i = 0; i < sb->fCount; i++)
                            {
                                if (strcmp(sb->fStealthBank[i], cmd) == 0)
                                {
                                    snprintf(path, sizeof(path), "{ \"cmd\": \"$play %s\" }", sb->fBleepBank[i]);
                                    smq_publish_hash("BLEEP", (uint8_t*)path, strlen(path));
                                    break;
                                }
                            }
                        }
                    }
                    else if (strncmp(cmd, "VWR ", 4) == 0)
                    {
                        // Write Command Register
                    }
                    else if (strncmp(cmd, "VRD ", 4) == 0)
                    {
                        // Read Command Register
                    }
                    else if (*cmd != '\0')
                    {
                        // Unsupported
                        printf("[VMUSIC] UNSUPPORTED: %s\n", line);
                    }
                }
            }
            free(line);
        }
        sleep(1);
    }
    if (data->fVMusicFD != -1)
    {
        smq_close_serial(data->fVMusicFD);
        data->fVMusicFD = -1;
    }
}

static void threadStealthServerLoop(void* arg)
{
    GlobalState* data = (GlobalState*)arg;
    data->fStealthFD = -1;
    while (data->fStealthFD == -1)
    {
        data->fStealthFD = smq_open_serial(data->fStealthPort, 57600);
        printf("smq_open(\"%s\") : %d\n", data->fStealthPort, data->fStealthFD);
        if (data->fStealthFD != -1)
        {
            smq_reset_serial(data->fStealthFD);
            char *line = NULL;
            size_t linecap = 0;
            ssize_t linelen;
            while (!data->fQuit && (linelen = getlineFD(&line, &linecap, data->fStealthFD)) > 0)
            {
                // Remove trailing crlf
                while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
                    line[--linelen] = '\0';
                printf("[STEALTH] %s\n", line);
            }
            free(line);
        }
        sleep(1);
    }
    if (data->fStealthFD != -1)
    {
        smq_close_serial(data->fStealthFD);
        data->fStealthFD = -1;
    }
}

#if defined(__linux)
static void alsa_silent_error_handler(const char *file, int line, const char *function, int err, const char *fmt, ...)
{
}
#endif

static void makeAlsaSilent()
{
#if defined(__linux)
    // Unfortunately portaudio doesn't silence spew from Alsa
    void* alsalib = dlopen("libasound.so", (RTLD_NOW|RTLD_GLOBAL));
    if (alsalib != NULL)
    {
        typedef int (*snd_lib_error_set_handler_proc)(void* handler);   
        snd_lib_error_set_handler_proc snd_lib_error_set_handler =
            (snd_lib_error_set_handler_proc)dlsym(alsalib, "snd_lib_error_set_handler");
        snd_lib_error_set_handler((void*)alsa_silent_error_handler);

    }
#endif
}

static void initPlatform()
{
#ifdef _MSC_VER
    WORD wVersionRequested;
    WSADATA wsaData;
    int err;
    wVersionRequested = MAKEWORD(2, 2);
    err = WSAStartup(wVersionRequested, &wsaData);
#endif
}

static int doStealthAssetFiles(const char* type, const char* stealthName, StealthSoundBank* sb = NULL)
{
    int assetCount = 0;
    char buffer[4096];

    if (sb != NULL)
    {
        sb->fStealthBank = new char*[sb->fCount];
        sb->fBleepBank = new char*[sb->fCount];
        memset(sb->fStealthBank, 0xFF, sb->fCount * sizeof(char*));
    }
#ifdef _MSC_VER
    HANDLE hFind;
    WIN32_FIND_DATA FindFileData;
    snprintf(buffer, sizeof(buffer), "assets\\%s\\*.ogg", type);
    if ((hFind = FindFirstFile(buffer, &FindFileData)) != INVALID_HANDLE_VALUE)
#else
    DIR *d;
    struct dirent *dir;
    snprintf(buffer, sizeof(buffer), "assets/%s", type);
    d = opendir(buffer);
    if (d)
#endif
    {
    #ifdef _MSC_VER
        do
    #else
        while ((dir = readdir(d)) != NULL)
    #endif
        {
        #ifndef _MSC_VER
            char* p = dir->d_name;
            if (dir->d_type != DT_REG)
                continue;
        #else
            char* p = FindFileData.cFileName;
        #endif
            char* fname = p;
            int sndnum = 0;
            char sndcategory[32];
            char* snd = sndcategory;
            while (*p != '\0' && *p != '-' && *p != '.')
            {
                if (snd+1 < sndcategory + sizeof(sndcategory))
                    *snd++ = *p;
                p++;
            }
            *snd = 0;
            if (*p == '-')
            {
                sndnum = 0; p++;
                while(isdigit(*p))
                {
                    sndnum = (sndnum * 10) + (*p++ - '0');
                }
            }
            if (*p == '.')
            {
                *p++ = '\0';
                if (strcmp(p, "ogg") == 0)
                {
                    snprintf(buffer, sizeof(buffer), "%s", sndcategory);
                    for (char* ch = buffer; *ch; ch++)
                        *ch = tolower(*ch);
                    if (strcmp(buffer, stealthName) == 0)
                    {
                        if (sb != NULL)
                        {
                            snprintf(buffer, sizeof(buffer), "%s-%d", stealthName, assetCount+1);
                            sb->fStealthBank[assetCount] = strdup(buffer);
                            sb->fBleepBank[assetCount] = strdup(fname);
                        }
                        assetCount++;
                    }
                }
            }
        }
    #ifdef _MSC_VER
        while (FindNextFile(hFind, &FindFileData));
        FindClose(hFind);
    #else
        closedir(d);
    #endif
    }
    return assetCount;
}

int main(int argc, const char* argv[])
{
#ifdef _MSC_VER
    HANDLE hFind;
    WIN32_FIND_DATA FindFileData;
#else
    DIR *d;
    struct dirent *dir;
#endif
    PaError err;
    bool server = false;
    bool verbose = false;
    bool playall = false;
    const char* vmusicAgentSerial = NULL;
    const char* serverAgentSerial = NULL;
    const char* stealthAgentSerial = NULL;
    static GlobalState data;
    SoundCategory* scat;
    SoundCategory* categoryList = NULL;
    size_t categoryCount = 0;
#ifdef RT_SCHED
    {
    struct sched_param param;
    param.sched_priority = 99;
    int status = sched_setscheduler(gettid(), RT_SCHED, &param);
    }
#endif
    memset(&data, '\0', sizeof(data));

    initPlatform();
#ifdef USE_CACHE
    data.fUseCache = true;
#endif
    for (int ai = 1; ai < argc; ai++)
    {
        const char* opt = argv[ai];
        if (*opt == '-')
        {
            opt++;
            if (strcmp(opt, "v") == 0 || strcmp(opt, "verbose") == 0)
            {
                verbose = true;
                continue;
            }
            else if (strcmp(opt, "s") == 0 || strcmp(opt, "server") == 0)
            {
                server = true;
                continue;
            }
            else if (strcmp(opt, "playall") == 0)
            {
                playall = true;
                continue;
            }
            else if (strcmp(opt, "c") == 0 || strcmp(opt, "cache") == 0)
            {
                data.fUseCache = true;
                continue;
            }
            else if (strncmp(opt, "smq=", 4) == 0)
            {
                server = true;
                serverAgentSerial = opt+4;
                continue;
            }
            else if (strncmp(opt, "amixer=", 7) == 0)
            {
                amixerDevice = opt+7;
                continue;
            }
            else if (strncmp(opt, "stealth=", 8) == 0)
            {
                server = true;
                stealthAgentSerial = opt+8;
                continue;
            }
            else if (strncmp(opt, "vmusic=", 7) == 0)
            {
                server = true;
                vmusicAgentSerial = opt+7;
                continue;
            }
            else if (strcmp(opt, "b") == 0)
            {
                if (ai+1 >= argc)
                {
                    printf("Command line option missing IP. Use -b <broadcast_ip>\n");
                    return 1;
                }
                sBroadcastIP = argv[++ai];
                continue;
            }
        }
        printf("Unknown or invalid command line option: \"%s\"\n", argv[ai]);
        return 1;
    }

#if defined(__linux)
    if (!verbose)
        makeAlsaSilent();
#endif
    err = Pa_Initialize();
    if (err != paNoError)
    {
        fprintf(stderr, "Error: Portaudio error #%d\n", err);
        exit(1);
    }

    // Count catagories
#ifdef _MSC_VER
    if ((hFind = FindFirstFile("assets\\*.wav", &FindFileData)) != INVALID_HANDLE_VALUE)
#else
    d = opendir("assets");
    if (d)
#endif
    {
    #ifdef _MSC_VER
        do
    #else
        while ((dir = readdir(d)) != NULL)
    #endif
        {
        #ifndef _MSC_VER
            const char* p = dir->d_name;
            if (dir->d_type != DT_REG)
                continue;
        #else
            const char* p = FindFileData.cFileName;
        #endif
            int sndnum = 0;
            int sndsubnum = 0;
            char sndcategory[32];
            char* snd = sndcategory;
            while (*p != '\0' && *p != '-' && *p != '.')
            {
                if (snd+1 < sndcategory + sizeof(sndcategory))
                    *snd++ = *p;
                p++;
            }
            *snd = 0;
            if (*p == '-')
            {
                sndnum = 0; p++;
                while(isdigit(*p))
                {
                    sndnum = (sndnum * 10) + (*p++ - '0');
                }
            }
            if (*p == '-')
            {
                sndsubnum = 0; p++;
                while(isdigit(*p))
                {
                    sndsubnum = (sndsubnum * 10) + (*p++ - '0');
                }
            }
            if (*p == '.')
            {
                p++;
                if (strcmp(p, "wav") == 0)
                {
                    SoundCategory** cat;
                    for (cat = &categoryList; *cat != NULL; cat = &(*cat)->fNext)
                    {
                        if (strcmp((*cat)->fName, sndcategory) == 0)
                            break;
                    }
                    if (*cat == NULL)
                        *cat = new SoundCategory(sndcategory, categoryCount++);
                    (*cat)->fCount++;
                }
            }
        }
    #ifdef _MSC_VER
        while (FindNextFile(hFind, &FindFileData));
        FindClose(hFind);
    #else
        closedir(d);
    #endif
    }
    if (categoryCount == 0)
    {
        printf("No wav files found in \"assets\" directory.\n");
        return 1;
    }
    data.fCatSelectCount = 0;;
    data.fCategoryCount = categoryCount;
    data.fSnippetCounts = new size_t[categoryCount];
    data.fSnippets = new SoundSnippet*[categoryCount];
    data.fCategoryList = scat = categoryList;

    data.fDefHaveRatio = false;
    data.fDefratio = 1.0;
    data.fDefpitchshift = 0.0;
    data.fDeffrequencyshift = 1.0;
    data.fDefdoreverse = false;

    for (size_t ci = 0; ci < categoryCount; ci++, scat = scat->fNext)
    {
        data.fSnippetCounts[ci] = scat->fCount;
        scat->fSnippets = new SoundSnippet[scat->fCount];
        data.fSnippets[ci] = scat->fSnippets;
        data.fCatSelectCount += scat->fCount;
    }
    scat = categoryList;
    size_t* sel = data.fCatSelect = new size_t[data.fCatSelectCount];
    for (size_t ci = 0; ci < categoryCount; ci++, scat = scat->fNext)
    {
        for (int i = 0; i < scat->fCount; i++)
            *sel++ = scat->fCatIndex;
    }

    // Count catagories
#ifdef _MSC_VER
    if ((hFind = FindFirstFile("assets\\*.wav", &FindFileData)) != INVALID_HANDLE_VALUE)
#else
    d = opendir("assets");
    if (d)
#endif
    {
#ifdef _MSC_VER
        do
#else
        while ((dir = readdir(d)) != NULL)
#endif
        {
        #ifndef _MSC_VER
            const char* p = dir->d_name;
            const char* fname = p;
            if (dir->d_type != DT_REG)
                continue;
        #else
            const char* p = FindFileData.cFileName;
            const char* fname = p;
        #endif
            int sndnum = 0;
            int sndsubnum = 0;
            char sndcategory[32];
            char* snd = sndcategory;
            while (*p != '\0' && *p != '-' && *p != '.')
            {
                if (snd+1 < sndcategory + sizeof(sndcategory))
                    *snd++ = *p;
                p++;
            }
            *snd = 0;
            if (*p == '-')
            {
                sndnum = 0; p++;
                while(isdigit(*p))
                {
                    sndnum = (sndnum * 10) + (*p++ - '0');
                }
            }
            if (*p == '-')
            {
                sndsubnum = 0; p++;
                while(isdigit(*p))
                {
                    sndsubnum = (sndsubnum * 10) + (*p++ - '0');
                }
            }
            if (*p == '.')
            {
                char* dotp = (char*)p;
                p++;
                if (strcmp(p, "wav") == 0)
                {
                    for (SoundCategory* cat = categoryList; cat != NULL; cat = cat->fNext)
                    {
                        if (strcmp(cat->fName, sndcategory) == 0 && cat->fReserved < cat->fCount)
                        {
                            SoundSnippet* snippet = &cat->fSnippets[cat->fReserved];
                            snippet->fNum = sndnum;
                            snippet->fUsedIndex = 0;
                            snippet->fSubNum = sndsubnum;
                            snippet->fSize = fsize("assets", fname);
                            snippet->fAudio = loadAudio(snippet, "assets", fname, verbose);
                            *dotp = '\0';
                            snippet->fName = (snippet->fAudio != NULL) ? strdup(fname) : NULL;
                            if (snippet->fName != NULL)
                                cat->fReserved++;
                            break;
                        }
                    }
                }
            }
        }
    #ifdef _MSC_VER
        while (FindNextFile(hFind, &FindFileData));
        FindClose(hFind);
    #else
        closedir(d);
    #endif

        for (scat = categoryList; scat != NULL; scat = scat->fNext)
        {
            scat->fCount = scat->fReserved;
            qsort(scat->fSnippets, scat->fCount, sizeof(SoundSnippet), snippet_sorter);
        }

        if (verbose)
        {
            for (scat = categoryList; scat != NULL; scat = scat->fNext)
            {
                printf("\nCategory: %s\n", scat->fName);
                for (size_t i = 0; i < scat->fCount; i++)
                {
                    printf("[%s %zu:%zu dur: %gms notes: ", scat->fSnippets[i].fName, scat->fCatIndex, i, double(scat->fSnippets[i].fAudio->samples[0].size()) / 44.1);
                    for (int n = 0; n < scat->fSnippets[i].fNoteCount; n++)
                    {
                        printf("%s ", midiNoteToNote(scat->fSnippets[i].fNotes[n].fPitch));
                    }
                    printf(" sol: ");
                    for (int n = 0; n < scat->fSnippets[i].fNoteCount; n++)
                    {
                        const char* s = midiNoteToNote(scat->fSnippets[i].fNotes[n].fPitch);
                        switch (s[0])
                        {
                            case 'c':
                                printf((s[2] == '#') ? "di" : "do");
                                break;
                            case 'd':
                                printf((s[2] == '#') ? "ri" : "re");
                                break;
                            case 'e':
                                printf("mi");
                                break;
                            case 'f':
                                printf((s[2] == '#') ? "fi" : "fa");
                                break;
                            case 'g':
                                printf((s[2] == '#') ? "si" : "so");
                                break;
                            case 'a':
                                printf((s[2] == '#') ? "li" : "la");
                                break;
                            case 'b':
                                printf("ti");
                                break;
                        }
                    }
                    printf("\n");
                }
            }
        }
#if 0
        for (scat = categoryList; scat != NULL; scat = scat->fNext)
        {
            for (size_t i = 0; i < scat->fCount; i++)
            {
                if (scat->fSnippets[i].fNoteCount == 1 && double(scat->fSnippets[i].fAudio->samples[0].size()) / 44.1 < 150 &&
                    double(scat->fSnippets[i].fAudio->samples[0].size()) / 44.1 >= 80)
                {
                    printf("afplay assets/%s.wav       sol: ", scat->fSnippets[i].fName);
                    for (int n = 0; n < scat->fSnippets[i].fNoteCount; n++)
                    {
                        const char* s = midiNoteToNote(scat->fSnippets[i].fNotes[n].fPitch);
                        switch (s[0])
                        {
                            case 'c':
                                printf((s[2] == '#') ? "di" : "do");
                                break;
                            case 'd':
                                printf((s[2] == '#') ? "ri" : "re");
                                break;
                            case 'e':
                                printf("mi");
                                break;
                            case 'f':
                                printf((s[2] == '#') ? "fi" : "fe");
                                break;
                            case 'g':
                                printf((s[2] == '#') ? "si" : "so");
                                break;
                            case 'a':
                                printf((s[2] == '#') ? "li" : "la");
                                break;
                            case 'b':
                                printf("ti");
                                break;
                        }
                        printf(" [%s:%g]", s, scat->fSnippets[i].fNotes[n].fPitch);
                    }
                    printf(" dur: %gms\n", double(scat->fSnippets[i].fAudio->samples[0].size()) / 44.1);
                }
            }
        }
#endif
        data.fChannelCount = 3;
        data.fChannels = new AudioChannelThread[data.fChannelCount];
        memset(data.fChannels, '\0', sizeof(AudioChannelThread) * data.fChannelCount);
        for (size_t ci = 0; ci < data.fChannelCount; ci++)
        {
            AudioChannelThread* act = &data.fChannels[ci];
            act->fData = &data;
            act->fChannel = ci;
        #ifdef _MSC_VER
            act->fSem = ::CreateEvent(NULL, FALSE, FALSE, NULL);
        #elif defined(__APPLE__)
            kern_return_t status = semaphore_create(mach_task_self(),
                &act->fSem, SYNC_POLICY_FIFO, 0);
            if (status != 0 || act->fSem == NULL_SEMAPHORE)
            {
                fprintf(stderr, "Error: Failed to allocate semaphore.\n");
                exit(1);
            }
        #else
            int status = sem_init(&act->fSemData, 0, 0);
            if (status != 0)
            {
                fprintf(stderr, "Error: Failed to allocate semaphore.\n");
                exit(1);
            }
            act->fSem = &act->fSemData;
        #endif

            act->fThread = createOSThread(threadAudioChannelLoop, act);
        #ifndef _MSC_VER
            pthread_setcurrentpriority(10);
        #endif
        }

        // Read our startup config if available
        FILE* startup = fopen("assets/config.txt", "r");
        if (startup != NULL)
        {
            processCommands(&data, startup, verbose);
            fclose(startup);
        }

        // Auto populate Stealth sound banks
        int sbIdx = 0;
        data.fStealthSoundBank = new StealthSoundBank[data.fStealthSoundBankCount];
        memset(data.fStealthSoundBank, '\0', sizeof(StealthSoundBank) * data.fStealthSoundBankCount);
        for (StealthVar* var = data.fStealthVarHead; var != NULL; var = var->fNext)
        {
            if (strcmp(var->fName, "sb") != 0)
                continue;

            int soundCount = 0;
            char snippetname[1024];
            char cmdbuffer[1024];
            StealthSoundBank* sb = &data.fStealthSoundBank[sbIdx++];
            sb->fIndex = sbIdx;
            sb->fName = strdup(var->fValue);
            for (SoundCategory* catList = data.fCategoryList; catList != NULL; catList = catList->fNext)
            {
                if (strcmp(catList->fName, var->fValue) == 0)
                {
                    *snippetname = '\0';
                    for (size_t i = 0; i < catList->fCount; i++)
                    {
                        int dashCount = 0;
                        SoundSnippet* snippet = &catList->fSnippets[i];
                        snprintf(cmdbuffer, sizeof(cmdbuffer), "%s", snippet->fName);
                        for (char *ch = cmdbuffer; *ch != '\0'; ch++)
                            dashCount += (*ch == '-');
                        if (dashCount > 1)
                        {
                            char*  le = cmdbuffer + strlen(cmdbuffer);
                            while (le > cmdbuffer)
                            {
                                char ch = *le;
                                *le-- = 0;
                                if (ch == '-')
                                    break;
                            }
                        }
                        if (strcmp(snippetname, cmdbuffer) != 0)
                        {
                            strcpy(snippetname, cmdbuffer);
                            soundCount++;
                        }
                    }
                    sb->fCount = soundCount;
                    sb->fStealthBank = new char*[soundCount];
                    sb->fBleepBank = new char*[soundCount];
                    memset(sb->fStealthBank, 0xFF, soundCount * sizeof(char*));
                    soundCount = 0;
                    *snippetname = '\0';
                    for (size_t i = 0; i < catList->fCount; i++)
                    {
                        int dashCount = 0;
                        SoundSnippet* snippet = &catList->fSnippets[i];
                        snprintf(cmdbuffer, sizeof(cmdbuffer), "%s", snippet->fName);
                        for (char *ch = cmdbuffer; *ch != '\0'; ch++)
                            dashCount += (*ch == '-');
                        if (dashCount > 1)
                        {
                            char*  le = cmdbuffer + strlen(cmdbuffer);
                            while (le > cmdbuffer)
                            {
                                char ch = *le;
                                *le-- = 0;
                                if (ch == '-')
                                    break;
                            }
                        }
                        if (strcmp(snippetname, cmdbuffer) != 0)
                        {
                            strcpy(snippetname, cmdbuffer);
                            snprintf(cmdbuffer, sizeof(cmdbuffer), "%s-%d.mp3", catList->fName, (soundCount+1));
                            sb->fStealthBank[soundCount] = strdup(cmdbuffer);
                            sb->fBleepBank[soundCount] = strdup(snippetname);
                            soundCount++;
                        }
                    }
                    break;
                }
            }
            if (soundCount == 0)
            {
                // check RTTTL songs
                for (RTTTLSong* song = data.fSongList; song != NULL; song = song->fNext)
                {
                    snprintf(snippetname, sizeof(snippetname), "%s", song->fName);
                    for (char* ch = snippetname; *ch; ch++)
                        *ch = tolower(*ch);
                    if (strcmp(song->fName, var->fValue) == 0)
                    {
                        sb->fCount = soundCount = 1;
                        sb->fStealthBank = new char*[1];
                        sb->fBleepBank = new char*[1];
                        snprintf(cmdbuffer, sizeof(cmdbuffer), "%s-1.mp3", snippetname);
                        sb->fStealthBank[0] = strdup(cmdbuffer);
                        sb->fBleepBank[0] = strdup(song->fName);
                        break;
                    }
                }
            }
            if (soundCount == 0)
            {
                soundCount = sb->fCount = doStealthAssetFiles("speech", var->fValue, NULL);
                if (soundCount != 0)
                {
                    doStealthAssetFiles("speech", var->fValue, sb);
                }
            }
            if (soundCount == 0)
            {
                soundCount = sb->fCount = doStealthAssetFiles("music", var->fValue);
                if (soundCount != 0)
                {
                    doStealthAssetFiles("music", var->fValue, sb);
                }
            }
        }

        if (data.fUseCache)
        {
            char cmdbuffer[1024];
            if (verbose)
                printf("Precache assets\n");
            for (RTTTLSong* song = data.fSongList; song != NULL; song = song->fNext)
            {
                snprintf(cmdbuffer, sizeof(cmdbuffer), "$cache %s", song->fName);
                processCommand(&data, cmdbuffer, verbose);
            }
            char snippetname[1024];
            *snippetname = '\0';
            for (SoundCategory* catList = data.fCategoryList; catList != NULL; catList = catList->fNext)
            {
                for (size_t i = 0; i < catList->fCount; i++)
                {
                    SoundSnippet* snippet = &catList->fSnippets[i];
                    snprintf(cmdbuffer, sizeof(cmdbuffer), "%s", snippet->fName);
                    char*  le = cmdbuffer + strlen(cmdbuffer);
                    while (le > cmdbuffer)
                    {
                        char ch = *le;
                        *le-- = 0;
                        if (ch == '-')
                            break;
                    }
                    if (strcmp(snippetname, cmdbuffer) != 0)
                    {
                        strcpy(snippetname, cmdbuffer);
                        snprintf(cmdbuffer, sizeof(cmdbuffer), "$cache %s", snippetname);
                        processCommand(&data, cmdbuffer, verbose);
                    }
                }
            }
        }
        if (playall)
        {
            char cmdbuffer[1024];
            if (verbose)
                printf("Playing all assets\n");
            // for (RTTTLSong* song = data.fSongList; song != NULL; song = song->fNext)
            // {
            //     snprintf(cmdbuffer, sizeof(cmdbuffer), "$play %s", song->fName);
            //     printf("%s\n", cmdbuffer);
            //     processCommand(&data, cmdbuffer, verbose);
            // }
            char snippetname[1024];
            *snippetname = '\0';
            for (SoundCategory* catList = data.fCategoryList; catList != NULL; catList = catList->fNext)
            {
                for (size_t i = 0; i < catList->fCount; i++)
                {
                    SoundSnippet* snippet = &catList->fSnippets[i];
                    snprintf(cmdbuffer, sizeof(cmdbuffer), "%s", snippet->fName);
                    char*  le = cmdbuffer + strlen(cmdbuffer);
                    while (le > cmdbuffer)
                    {
                        char ch = *le;
                        *le-- = 0;
                        if (ch == '-')
                            break;
                    }
                    if (strcmp(snippetname, cmdbuffer) != 0)
                    {
                        strcpy(snippetname, cmdbuffer);
                        snprintf(cmdbuffer, sizeof(cmdbuffer), "$play %s", snippetname);
                        printf("%s\n", cmdbuffer);
                        processCommand(&data, cmdbuffer, verbose);
                    }
                }
            }
            // Wait for completion
            AudioChannelThread* act = &data.fChannels[0];
            while (act->fCurrentCmd > 0 || (act->fCmd.fCmd != 0 && !act->fCmd.fDone))
            {
                Pa_Sleep(200);
            }
            snprintf(cmdbuffer, sizeof(cmdbuffer), "$quit");
            processCommand(&data, cmdbuffer, verbose);
            printf("DONE\n");
            goto doexit;
        }
        if (vmusicAgentSerial != NULL)
        {
            data.fVMusicPort = vmusicAgentSerial; 
            data.fVMusicThread = createOSThread(threadVMusicServerLoop, &data);
        }
        if (stealthAgentSerial != NULL)
        {
            data.fStealthPort = stealthAgentSerial; 
            data.fStealthThread = createOSThread(threadStealthServerLoop, &data);
        }

        if (server)
        {
            data.fMARCServerThread = createOSThread(threadMarcduinoServerLoop, &data);

            /* Initialize dzmq */
            if (!smq_init())
            {
                printf("Failed to initialize SMC\n");
                exit(-1);
            }
            if (!smq_advertise_hash("MARC"))
            {
                printf("Failed to advertise MARC\n");
                exit(-1);
            }
            if (!smq_subscribe_hash("MARC", MARC_eventHandler, &data))
            {
                printf("Failed to subscribe to MARC event\n");
                exit(-1);
            }
            if (!smq_advertise("MARC"))
            {
                printf("Failed to advertise MARC\n");
                exit(-1);
            }
            if (!smq_subscribe("MARC", MARC_eventHandler, &data))
            {
                printf("Failed to subscribe to MARC event\n");
                exit(-1);
            }
            if (!smq_advertise_hash("BLEEP"))
            {
                printf("Failed to advertise MARC\n");
                exit(-1);
            }
            if (!smq_subscribe_hash("BLEEP", BleepBox_eventHandler, &data))
            {
                printf("Failed to subscribe to BleepBox event\n");
                exit(-1);
            }

            int agentFD = 0;
            if (serverAgentSerial != NULL)
            {
                printf("serverAgentSerial : %s\n", serverAgentSerial);
                agentFD = smq_subscribe_serial(serverAgentSerial, 0);
            }

            /* Spin */
            while (smq_spin())
                ;

            if (agentFD != 0)
                smq_unsubscribe_serial(agentFD);
        }
        else
        {
            processCommands(&data, stdin, verbose);
        }
    }
doexit:
#ifndef _MSC_VER
    for (size_t ci = 0; ci < data.fChannelCount; ci++)
    {
        data.fChannels[ci].fDone = true;
        pthread_join(data.fChannels[ci].fThread, NULL);
    }
#endif
    Pa_Terminate();

    for (int i = 0; i < SizeOfArray(data.fSnippets); i++)
    {
        // Need to dispose of fAudio and fName
        delete [] data.fSnippets[i];
    }

    if (err != paNoError)
    {
        fprintf( stderr, "An error occured while using the portaudio stream\n" );
        fprintf( stderr, "Error number: %d\n", err );
        fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    }
    return err;
}

#ifdef _MSC_VER
#define GETLINE_MINSIZE 16

int getline(char** lineptr, size_t * n, FILE * fp)
{
    int ch;
    int i = 0;
    char free_on_err = 0;
    char* p;

    errno = 0;
    if (lineptr == NULL || n == NULL || fp == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (*lineptr == NULL)
    {
        *n = GETLINE_MINSIZE;
        *lineptr = (char*)malloc(sizeof(char) * (*n));
        if (*lineptr == NULL)
        {
            errno = ENOMEM;
            return -1;
        }
        free_on_err = 1;
    }

    for (i = 0; ; i++)
    {
        ch = fgetc(fp);
        while (i >= (*n) - 2)
        {
            *n *= 2;
            p = (char*)realloc(*lineptr, sizeof(char) * (*n));
            if (p == NULL)
            {
                if (free_on_err)
                    free(*lineptr);
                errno = ENOMEM;
                return -1;
            }
            *lineptr = p;
        }
        if (ch == EOF)
        {
            if (i == 0)
            {
                if (free_on_err)
                    free(*lineptr);
                return -1;
            }
            (*lineptr)[i] = '\0';
            *n = i;
            return i;
        }

        if (ch == '\n')
        {
            (*lineptr)[i] = '\n';
            (*lineptr)[i + 1] = '\0';
            *n = i + 1;
            return i + 1;
        }
        (*lineptr)[i] = (char)ch;

    }
}
#endif
