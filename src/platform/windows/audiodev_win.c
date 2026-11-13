/* How many channels the default sound device mixes in (WASAPI's shared-mode format): 2 for headphones and most
   speakers, 6 or 8 for a surround set-up; 0 if Windows does not say. video_win.c folds surround films down itself on a
   stereo device. Its own file: the audio headers need GDI's types, which video_win.c leaves out. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

static const GUID kClsEnum = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIidEnum = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIidClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };

int win_audio_channels(void) {
  int n = 0;
  IMMDeviceEnumerator *en = NULL;
  IMMDevice *dev = NULL;
  IAudioClient *ac = NULL;
  WAVEFORMATEX *wf = NULL;
  HRESULT ci = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (SUCCEEDED(CoCreateInstance(&kClsEnum, NULL, CLSCTX_ALL, &kIidEnum, (void **)&en)) &&
      SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
      SUCCEEDED(dev->lpVtbl->Activate(dev, &kIidClient, CLSCTX_ALL, NULL, (void **)&ac)) &&
      SUCCEEDED(ac->lpVtbl->GetMixFormat(ac, &wf)))
    n = wf->nChannels;
  if (wf)
    CoTaskMemFree(wf);
  if (ac)
    ac->lpVtbl->Release(ac);
  if (dev)
    dev->lpVtbl->Release(dev);
  if (en)
    en->lpVtbl->Release(en);
  if (SUCCEEDED(ci))
    CoUninitialize();
  return n;
}
