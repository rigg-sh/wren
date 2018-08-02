#include <stdio.h>
#include <string.h>

#include "wren.h"
#include "vm.h"

void callWrenCallRootRunTests(WrenVM* vm)
{
  wrenSetSlotCount(vm, 1);
  wrenGetVariable(vm, "./test/api/call_wren_call_root", "Test", 0);
  WrenHandle* testClass = wrenGetSlotHandle(vm, 0);

  WrenHandle* run = wrenMakeCallHandle(vm, "run()");

  WrenFiber* fiber = wrenGetCurrentFiber(vm);
  wrenSetSlotCount(vm, 1);
  wrenSetSlotHandle(vm, 0, testClass);
  WrenInterpretResult result = wrenCall(fiber, run);
  if (result == WREN_RESULT_RUNTIME_ERROR)
  {
    setExitCode(70);
  }
  else
  {
    printf("Missing runtime error.\n");
  }

  wrenReleaseHandle(vm, testClass);
  wrenReleaseHandle(vm, run);
}
