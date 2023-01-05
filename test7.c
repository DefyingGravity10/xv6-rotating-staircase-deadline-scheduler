#include "types.h"
#include "user.h"

int main() {
  schedlog(100000000);

  for (int i = 0; i < 5; i++) {
    if (priofork(1) == 0) {
      char *argv[] = {"short", 0};
      exec("short", argv);
    }
  }

  for (int i = 0; i < 5; i++) {
    wait();
  }

  shutdown();
}