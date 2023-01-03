#include "types.h"
#include "user.h"

//Make this fixed. Just wanted to show what happens if a specific proc was killed
int main() {
  schedlog(10000);
  int spoon;

  for (int i = 0; i < 3; i++) {
    spoon = fork();
    if (spoon == 0 && i % 2 == 0) {
      char *argv[] = {"loop", 0};
      exec("loop", argv);
    }

    if (spoon == 0 && i % 2 != 0) {
      char *argv[] = {"cf", 0};
      exec("cf", argv);
    }
  }

  for (int i = 0; i < 3; i++) {
    wait();
  }

  shutdown();
}