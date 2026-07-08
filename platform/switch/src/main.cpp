#include <switch.h>

#include <cstdio>

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  consoleInit(nullptr);

  std::printf("LocalSend Handheld\n");
  std::printf("Switch homebrew package smoke test\n\n");
  std::printf("Protocol target: LocalSend v2.1 HTTP\n");
  std::printf("Inbox path: sdmc:/switch/localsend/inbox/\n");
  std::printf("Encryption: disabled for MVP\n\n");
  std::printf("Press + to exit.\n");

  while (appletMainLoop()) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    padUpdate(&pad);

    const u64 pressed = padGetButtonsDown(&pad);
    if (pressed & HidNpadButton_Plus) {
      break;
    }

    consoleUpdate(nullptr);
  }

  consoleExit(nullptr);
  return 0;
}

