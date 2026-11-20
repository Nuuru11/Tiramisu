// Hand-tuned FMOD / FMOD Studio stubs (spike parity).
//
// These are the functions whose out-params the managed FMOD wrapper
// computes from, or whose values gate boot progress. Every other FMOD
// P/Invoke Celeste declares gets a generated default stub (FMOD_RESULT 0 =
// OK) from gen-stubs.py. All handles returned here are fake: they point at
// a zeroed dummy buffer, never at a real FMOD object. There is no audio.
// These stubs exist so Celeste boots to the title screen offline; the next
// step replaces them with real dllmaps to Tiramisu's 64-bit FMOD bridge.
//
// Signatures are positional matches of the managed P/Invoke declarations
// in celeste.implmap (monodis --implmap of Celeste.exe). On SysV x86-64 the
// caller cleans the stack, so extra/ignored parameters are harmless.
#include <string.h>

static unsigned char fmod_dummy[256];
static void *fake_handle(void) { return fmod_dummy; }

// FMOD_OK = 0
#define FMOD_OK 0

int FMOD_System_Create(void **system) {
  *system = fake_handle();
  return FMOD_OK;
}

int FMOD_Studio_System_Create(void **system, unsigned int version) {
  (void)version;
  *system = fake_handle();
  return FMOD_OK;
}

int FMOD_System_CreateSound(void *system, const unsigned char *name,
                            unsigned int mode, void *exinfo, void **sound) {
  (void)system; (void)name; (void)mode; (void)exinfo;
  *sound = fake_handle();
  return FMOD_OK;
}

int FMOD_System_PlaySound(void *system, void *sound, void *group, int paused,
                          void **channel) {
  (void)system; (void)sound; (void)group; (void)paused;
  *channel = fake_handle();
  return FMOD_OK;
}

int FMOD_System_GetMasterChannelGroup(void *system, void **group) {
  (void)system;
  *group = fake_handle();
  return FMOD_OK;
}

int FMOD_System_GetMasterSoundGroup(void *system, void **group) {
  (void)system;
  *group = fake_handle();
  return FMOD_OK;
}

int FMOD_System_GetNumDrivers(void *system, int *numdrivers) {
  (void)system;
  *numdrivers = 1;
  return FMOD_OK;
}

int FMOD_System_GetSoftwareFormat(void *system, int *samplerate,
                                  int *speakermode, int *numrawspeakers) {
  (void)system;
  *samplerate = 48000;
  *speakermode = 3;  // FMOD_SPEAKERMODE_STEREO
  *numrawspeakers = 2;
  return FMOD_OK;
}

int FMOD_System_GetVersion(void *system, unsigned int *version) {
  (void)system;
  *version = 0x00020209u;  // FMOD 2.02.09
  return FMOD_OK;
}

int FMOD_Studio_System_GetEvent(void *system, const unsigned char *path,
                                void **description) {
  (void)system; (void)path;
  *description = fake_handle();
  return FMOD_OK;
}

int FMOD_Studio_System_GetBus(void *system, const unsigned char *path,
                              void **bus) {
  (void)system; (void)path;
  *bus = fake_handle();
  return FMOD_OK;
}

int FMOD_Studio_System_GetVCA(void *system, const unsigned char *path,
                              void **vca) {
  (void)system; (void)path;
  *vca = fake_handle();
  return FMOD_OK;
}

int FMOD_Studio_System_LoadBankFile(void *system, const unsigned char *path,
                                    unsigned int flags, void **bank) {
  (void)system; (void)path; (void)flags;
  *bank = fake_handle();
  return FMOD_OK;
}

int FMOD_Studio_Bank_GetEventCount(void *bank, int *count) {
  (void)bank;
  *count = 0;
  return FMOD_OK;
}

// FMOD_STUDIO_LOADING_STATE: UNLOADED=0, LOADING=1, LOADED=2. Report LOADED:
// a stub that forever reports UNLOADED would hang any load-wait polling.
int FMOD_Studio_Bank_GetLoadingState(void *bank, int *state) {
  (void)bank;
  *state = 2;
  return FMOD_OK;
}

int FMOD_Studio_Bank_GetSampleLoadingState(void *bank, int *state) {
  (void)bank;
  *state = 2;
  return FMOD_OK;
}

int FMOD_Studio_EventDescription_GetSampleLoadingState(void *description,
                                                       int *state) {
  (void)description;
  *state = 2;
  return FMOD_OK;
}

int FMOD_Studio_EventDescription_GetLength(void *description, int *length) {
  (void)description;
  *length = 60000;  // ms; a zero length is a divide-by-zero risk downstream
  return FMOD_OK;
}

// The spike's critical stub: a zero `retrieved` from GetPath crashes
// Celeste's GetEventName with count -1. Write a real path and a plausible
// length (bytes written including the NUL).
int FMOD_Studio_EventDescription_GetPath(void *description,
                                         unsigned char *path, int size,
                                         int *retrieved) {
  static const char stub_path[] = "event:/stub";
  const int need = (int)sizeof(stub_path);  // includes NUL
  (void)description;
  if (size > 0 && path != NULL) {
    int n = need < size ? need : size;
    memcpy(path, stub_path, (size_t)n);
    path[n - 1] = 0;
  }
  *retrieved = need;
  return FMOD_OK;
}

int FMOD_Studio_EventInstance_GetDescription(void *instance,
                                             void **description) {
  (void)instance;
  *description = fake_handle();
  return FMOD_OK;
}
