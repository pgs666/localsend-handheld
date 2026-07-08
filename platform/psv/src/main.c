#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>

int main(void) {
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  while (1) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) {
      break;
    }
    sceDisplayWaitVblankStart();
  }

  sceKernelExitProcess(0);
  return 0;
}

