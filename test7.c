#include "types.h"
#include "user.h"

int main() {
  schedlog(100000000);

  for (int i = 0; i < 3; i++) {
    if (priofork(i) == 0) {
      char *argv[] = {"loop", 0};
      exec("loop", argv);
    }
  }

  for (int i = 0; i < 3; i++) {
    wait();
  }

  shutdown();
}