#include "types.h"
#include "user.h"

// We can alter the number of processes used
int main() {
  schedlog(10000);
  int spoon;

  for (int i = 0; i < 3; i++) {
    spoon = fork();
    if (spoon == 0 && i % 3 == 2) {
      char *argv[] = {"short", 0};
      exec("short", argv);
    }

    if (spoon == 0 && i % 3 == 1) {
      char *argv[] = {"loop", 0};
      exec("loop", argv);
    }

    if (spoon == 0 && i % 3 == 0) {
      char *argv[] = {"long", 0};
      exec("long", argv);
    }
  }

  for (int i = 0; i < 3; i++) {
    wait();
  }

  shutdown();
}